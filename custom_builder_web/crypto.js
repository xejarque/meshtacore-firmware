// Meshtacore build crypto (browser side).
//
// Build requests carry PSKs/passwords and the resulting firmware embeds them, but issues,
// Actions logs, artifacts and releases are public. So:
//   1. the request is encrypted with the repo public key (only the workflow can read it);
//   2. a key pair is generated in THIS browser and only its public half is sent; the workflow
//      encrypts the resulting ZIP with it, so only this browser can open the download.
//
// Wire format (mirrors .github/scripts/build_crypto.py):
//   RSA-OAEP(SHA-256) wrapped AES key (256 B) | IV (12 B) | AES-256-GCM ciphertext+tag

const BuildCrypto = (() => {
  // SPKI (DER, base64) of the repo key. The matching private key is the BUILD_PRIVATE_KEY secret.
  const REPO_PUBLIC_KEY_SPKI =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAofA72uFMI65/gRpaEEey9aEWpYR01kMgSauqUPwLvNJb+vltSpC6jW+2tLNETtV7BLkPZymer0HUyR5HqO1KV6FPvKrQFndqtqOx41rrFVfYVGb+6gjbjbS+UWBixhftH808jwuEr+4neAi2aVXRyp4FUcjUjiTgMV+hLMNO2bCc4D6Ka6jdaO4ggGOWT62MAH3f4hWHrMvh14kbhLDr7LiFrJtgc/+C0xWtBhojlfUxtl42gqni7F3ffImaXHzcW6q0VdcGGnxUOT7yLQlTTGguVNIH7yp/rPIqmiNZed+UC//v64k7cm20CGQt0jtC7XLv11wl0Stcm8LRfMzLnQIDAQAB";

  const STORAGE_KEY = "meshtacore.userKeyPair.v1";
  const WRAPPED_KEY_LEN = 256;
  const IV_LEN = 12;
  const RSA_PARAMS = { name: "RSA-OAEP", hash: "SHA-256" };

  const subtle = () => {
    if (!window.crypto || !window.crypto.subtle) {
      throw new Error(
        "Este navegador no soporta WebCrypto (se requiere HTTPS).",
      );
    }
    return window.crypto.subtle;
  };

  function bytesToB64(bytes, urlSafe = false) {
    let bin = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    }
    const b64 = btoa(bin);
    return urlSafe
      ? b64.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "")
      : b64;
  }

  function b64ToBytes(text) {
    const std = text.replace(/\s+/g, "").replace(/-/g, "+").replace(/_/g, "/");
    const bin = atob(std + "=".repeat((4 - (std.length % 4)) % 4));
    return Uint8Array.from(bin, (c) => c.charCodeAt(0));
  }

  async function pipeThrough(bytes, stream) {
    const out = new Response(new Blob([bytes]).stream().pipeThrough(stream));
    return new Uint8Array(await out.arrayBuffer());
  }

  const deflateRaw = (bytes) =>
    pipeThrough(bytes, new CompressionStream("deflate-raw"));

  async function hybridEncrypt(plain, rsaPublicKey) {
    const aesKey = await subtle().generateKey(
      { name: "AES-GCM", length: 256 },
      true,
      ["encrypt"],
    );
    const iv = crypto.getRandomValues(new Uint8Array(IV_LEN));
    const ciphertext = new Uint8Array(
      await subtle().encrypt({ name: "AES-GCM", iv }, aesKey, plain),
    );
    const rawKey = await subtle().exportKey("raw", aesKey);
    const wrapped = new Uint8Array(
      await subtle().encrypt({ name: "RSA-OAEP" }, rsaPublicKey, rawKey),
    );
    const out = new Uint8Array(wrapped.length + IV_LEN + ciphertext.length);
    out.set(wrapped, 0);
    out.set(iv, wrapped.length);
    out.set(ciphertext, wrapped.length + IV_LEN);
    return out;
  }

  async function hybridDecrypt(blob, rsaPrivateKey) {
    const wrapped = blob.subarray(0, WRAPPED_KEY_LEN);
    const iv = blob.subarray(WRAPPED_KEY_LEN, WRAPPED_KEY_LEN + IV_LEN);
    const ciphertext = blob.subarray(WRAPPED_KEY_LEN + IV_LEN);
    const rawKey = await subtle().decrypt(
      { name: "RSA-OAEP" },
      rsaPrivateKey,
      wrapped,
    );
    const aesKey = await subtle().importKey("raw", rawKey, "AES-GCM", false, [
      "decrypt",
    ]);
    return new Uint8Array(
      await subtle().decrypt({ name: "AES-GCM", iv }, aesKey, ciphertext),
    );
  }

  const importPublic = (spkiB64) =>
    subtle().importKey("spki", b64ToBytes(spkiB64), RSA_PARAMS, true, [
      "encrypt",
    ]);

  // ---- Requester key pair (lives in this browser; private half never leaves it) ----

  async function generateUserKeys() {
    const pair = await subtle().generateKey(
      {
        ...RSA_PARAMS,
        modulusLength: 2048,
        publicExponent: new Uint8Array([1, 0, 1]),
      },
      true,
      ["encrypt", "decrypt"],
    );
    return {
      publicKey: await subtle().exportKey("jwk", pair.publicKey),
      privateKey: await subtle().exportKey("jwk", pair.privateKey),
    };
  }

  async function getUserKeys() {
    let stored = localStorage.getItem(STORAGE_KEY);
    if (!stored) {
      stored = JSON.stringify(await generateUserKeys());
      localStorage.setItem(STORAGE_KEY, stored);
    }
    return JSON.parse(stored);
  }

  async function getUserPublicSpkiB64() {
    const { publicKey } = await getUserKeys();
    const key = await subtle().importKey("jwk", publicKey, RSA_PARAMS, true, [
      "encrypt",
    ]);
    return bytesToB64(new Uint8Array(await subtle().exportKey("spki", key)));
  }

  async function getUserPrivateKey() {
    const { privateKey } = await getUserKeys();
    return subtle().importKey("jwk", privateKey, RSA_PARAMS, false, [
      "decrypt",
    ]);
  }

  async function exportUserKeys() {
    return JSON.stringify(await getUserKeys(), null, 2);
  }

  async function importUserKeys(json) {
    const parsed = JSON.parse(json);
    if (!parsed.publicKey || !parsed.privateKey)
      throw new Error("Fichero de clave no válido.");
    await subtle().importKey("jwk", parsed.privateKey, RSA_PARAMS, false, [
      "decrypt",
    ]);
    localStorage.setItem(STORAGE_KEY, JSON.stringify(parsed));
  }

  // ---- Public API ----

  // Returns a URL-safe base64 blob only the workflow can decrypt. `config` is the object the
  // parser expects (target, custom_userprefs, ...).
  async function encryptBuildRequest(config) {
    const payload = { config, user_public_key: await getUserPublicSpkiB64() };
    const compressed = await deflateRaw(
      new TextEncoder().encode(JSON.stringify(payload)),
    );
    const encrypted = await hybridEncrypt(
      compressed,
      await importPublic(REPO_PUBLIC_KEY_SPKI),
    );
    return bytesToB64(encrypted, true);
  }

  // Opens a firmware bundle (.zip.enc) that the workflow encrypted for this browser.
  async function decryptBundle(arrayBuffer) {
    return hybridDecrypt(
      new Uint8Array(arrayBuffer),
      await getUserPrivateKey(),
    );
  }

  return { encryptBuildRequest, decryptBundle, exportUserKeys, importUserKeys };
})();
