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

// What bin/build-nrf52.sh ships: a DFU zip (manifest + application .bin + init packet .dat), the UF2s and helpers.
const dfuZip = (
  manifest = { application: { bin_file: "app.bin", dat_file: "app.dat" } },
) =>
  zipSync({
    "manifest.json": text(JSON.stringify({ manifest })),
    "app.bin": Uint8Array.from([1, 2, 3, 4]),
    "app.dat": Uint8Array.from([9, 9]),
  });
const nrfBundleZip = (dfu = dfuZip()) =>
  zipSync({
    "Meshtastic_nRF52_factory_erase_v3_S140_7.3.0.uf2": Uint8Array.from([0]),
    "firmware-thinknode_m1-2.8.201-ota.zip": dfu,
    "firmware-thinknode_m1-2.8.201.uf2": Uint8Array.from([5]),
  });

test("bundle: an encrypted nRF52 artifact opens down to the application image and its init packet", async () => {
  const artifact = zipSync({
    "meshtacore-thinknode_m1.zip.enc": encrypt(nrfBundleZip()),
  });
  const result = await resolveBundle("artifact.zip", artifact, { decrypt });
  assert.equal(result.kind, "nrf52");
  assert.deepEqual([...result.dfu.bin], [1, 2, 3, 4]);
  assert.deepEqual([...result.dfu.dat], [9, 9]);
  assert.equal(
    result.uf2.name,
    "firmware-thinknode_m1-2.8.201.uf2",
    "kept as the manual fallback",
  );
});

test("bundle: a corrupt or bootloader-carrying DFU zip is refused with a clear message", async () => {
  const corrupt = nrfBundleZip(text("this is not a zip"));
  await assert.rejects(resolveBundle("b.zip", corrupt, { decrypt }), /dañado/);
  const withBootloader = nrfBundleZip(
    dfuZip({
      application: { bin_file: "app.bin", dat_file: "app.dat" },
      bootloader: { bin_file: "app.bin", dat_file: "app.dat" },
    }),
  );
  await assert.rejects(
    resolveBundle("b.zip", withBootloader, { decrypt }),
    /SoftDevice o bootloader/,
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
