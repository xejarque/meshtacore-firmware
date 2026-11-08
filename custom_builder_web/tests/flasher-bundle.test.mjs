// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { zipSync } from "../vendor/fflate.js";
import { resolveBundle } from "../flasher-bundle.js";

const FACTORY = Uint8Array.from([0xe9, 1, 2, 3]);
const text = (s) => new TextEncoder().encode(s);

// What custom_builder.yml produces: release/* zipped, optionally encrypted, then wrapped by upload-artifact.
const bundleZip = () =>
  zipSync({
    "config_used.jsonc": text("{}"),
    "firmware-heltec-v3-2.8.201.factory.bin": FACTORY,
  });

// The test double for BuildCrypto.decryptBundle: "encryption" is a reversible prefix.
const MARK = text("ENC:");
const encrypt = (bytes) => Uint8Array.from([...MARK, ...bytes]);
const decrypt = async (bytes) => {
  assert.deepEqual(
    [...bytes.slice(0, MARK.length)],
    [...MARK],
    "decrypt received something that is not encrypted",
  );
  return bytes.slice(MARK.length);
};

test("bundle: a plain zip yields the factory image", async () => {
  const result = await resolveBundle("meshtacore-heltec-v3.zip", bundleZip(), {
    decrypt,
  });
  assert.equal(result.kind, "esp32");
  assert.deepEqual([...result.image.data], [...FACTORY]);
});

test("bundle: an encrypted bundle is decrypted before it is opened", async () => {
  const result = await resolveBundle(
    "meshtacore-heltec-v3.zip.enc",
    encrypt(bundleZip()),
    { decrypt },
  );
  assert.equal(result.image.name, "firmware-heltec-v3-2.8.201.factory.bin");
});

test("bundle: the Actions artifact (zip around the .zip.enc) is unwrapped, decrypted and opened", async () => {
  const artifact = zipSync({
    "meshtacore-heltec-v3.zip.enc": encrypt(bundleZip()),
  });
  const result = await resolveBundle("artifact.zip", artifact, { decrypt });
  assert.equal(result.kind, "esp32");
  assert.equal(result.image.address, 0);
});

test("bundle: the artifact of an unencrypted build (zip around the .zip) works too", async () => {
  const artifact = zipSync({ "meshtacore-heltec-v3.zip": bundleZip() });
  const result = await resolveBundle("artifact.zip", artifact, { decrypt });
  assert.equal(result.kind, "esp32");
});

test("bundle: a bare factory .bin is accepted as is", async () => {
  const result = await resolveBundle("firmware-x.factory.bin", FACTORY, {
    decrypt,
  });
  assert.equal(result.image.address, 0);
});

test("bundle: a non-factory .bin and unknown formats are refused with a clear message", async () => {
  await assert.rejects(
    resolveBundle("firmware-x.bin", FACTORY, { decrypt }),
    /ningún firmware/,
  );
  await assert.rejects(
    resolveBundle("notes.txt", text("hi"), { decrypt }),
    /Formato no soportado/,
  );
});

test("bundle: a zip without firmware is refused, and a failed decryption surfaces its error", async () => {
  await assert.rejects(
    resolveBundle(
      "empty.zip",
      zipSync({ "a.txt": text("a"), "b.txt": text("b") }),
      { decrypt },
    ),
    /ningún firmware/,
  );
  const failing = async () => {
    throw new Error("No se pudo descifrar");
  };
  await assert.rejects(
    resolveBundle("x.zip.enc", text("junk"), { decrypt: failing }),
    /No se pudo descifrar/,
  );
});

test("bundle: endless nesting is cut off", async () => {
  let bytes = bundleZip();
  for (let i = 0; i < 8; i++) bytes = zipSync({ "inner.zip": bytes });
  await assert.rejects(
    resolveBundle("outer.zip", bytes, { decrypt }),
    /demasiadas capas/,
  );
});
