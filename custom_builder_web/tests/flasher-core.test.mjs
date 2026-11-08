// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import * as F from "../flasher-core.js";

const bytes = (...values) => Uint8Array.from(values);
const NOW = Date.parse("2026-11-08T10:00:00Z");

/** An ESP image header: magic at `offset`, chip id at offset + 12. */
function espImage(chipId, offset = 0) {
  const data = new Uint8Array(offset + 64).fill(0xff);
  data[offset] = 0xe9;
  data[offset + 12] = chipId & 0xff;
  data[offset + 13] = chipId >> 8;
  return data;
}

test("progress: earlier phases count as done and the bar reaches 100 only at the end", () => {
  assert.equal(F.overallProgress("build", 0), 0);
  assert.equal(F.overallProgress("flash", 1), 100);
  assert.ok(F.overallProgress("download", 0) > F.overallProgress("build", 0.5));
  assert.ok(F.overallProgress("flash", 0.5) > F.overallProgress("flash", 0.1));
  assert.equal(F.overallProgress("flash", 7), 100, "fractions are clamped");
  assert.throws(() => F.overallProgress("nope", 0));
});

test("run: picks the newest dispatch run of the actor created after the request", () => {
  const run = (id, created_at, login, event = "workflow_dispatch") => ({
    id,
    created_at,
    event,
    actor: { login },
  });
  const runs = [
    run(1, "2026-11-08T09:00:00Z", "me"), // before we dispatched
    run(2, "2026-11-08T10:00:05Z", "someone-else"),
    run(3, "2026-11-08T10:00:03Z", "me"),
    run(4, "2026-11-08T10:00:08Z", "me", "push"),
    run(5, "2026-11-08T10:00:09Z", "me"),
  ];
  assert.equal(F.pickRun(runs, { dispatchedAtMs: NOW, actor: "me" }).id, 5);
  assert.equal(
    F.pickRun(runs, { dispatchedAtMs: NOW, actor: "someone-else" }).id,
    2,
  );
  assert.equal(
    F.pickRun(runs, { dispatchedAtMs: NOW }).id,
    5,
    "no actor: newest of anyone",
  );
  assert.equal(F.pickRun([], { dispatchedAtMs: NOW }), null);
  assert.equal(
    F.pickRun(runs, { dispatchedAtMs: NOW + 3_600_000, actor: "me" }),
    null,
  );
});

test("run: describes queued, running (by finished steps), success and failure", () => {
  assert.equal(F.describeRun(null).state, "queued");
  assert.equal(F.describeRun({ status: "queued" }).state, "queued");
  const jobs = [
    {
      steps: [
        { status: "completed" },
        { status: "completed" },
        { status: "in_progress" },
        { status: "pending" },
      ],
    },
  ];
  assert.deepEqual(F.describeRun({ status: "in_progress" }, jobs), {
    state: "running",
    fraction: 0.5,
  });
  assert.equal(
    F.describeRun({ status: "in_progress" }, []).state,
    "queued",
    "no steps yet",
  );
  assert.equal(
    F.describeRun({ status: "completed", conclusion: "success" }).state,
    "success",
  );
  const failed = F.describeRun({
    status: "completed",
    conclusion: "cancelled",
  });
  assert.deepEqual([failed.state, failed.conclusion], ["failed", "cancelled"]);
});

test("artifact: skips expired ones and anything that is not a bundle", () => {
  const artifacts = [
    { name: "github-pages", expired: false },
    { name: "old.zip.enc", expired: true },
    { name: "a.zip.enc", expired: false, created_at: "2026-11-08T07:00:00Z" },
    { name: "b.zip.enc", expired: false, created_at: "2026-11-08T08:00:00Z" },
  ];
  assert.equal(F.pickArtifact(artifacts).name, "b.zip.enc");
  assert.equal(F.pickArtifact([{ name: "github-pages" }]), null);
  assert.equal(F.pickArtifact(undefined), null);
});

test("artifact: unwraps the single file and refuses anything else", () => {
  assert.equal(
    F.unwrapArtifact([{ name: "x.zip.enc", data: bytes(1) }]).name,
    "x.zip.enc",
  );
  assert.throws(() => F.unwrapArtifact([]), /único archivo/);
  assert.throws(
    () => F.unwrapArtifact([{ name: "a" }, { name: "b" }]),
    /único archivo/,
  );
  assert.equal(F.isEncrypted("meshtacore-x.zip.enc"), true);
  assert.equal(F.isEncrypted("meshtacore-x.zip"), false);
});

test("bundle: ESP32 bundles flash the factory image at 0x0, ignoring folders and other bins", () => {
  const factory = {
    name: "release/firmware-heltec-v3-2.8.201.factory.bin",
    data: bytes(1),
  };
  const entries = [
    { name: "release/", data: bytes() },
    { name: "release/config_used.jsonc", data: bytes(2) },
    { name: "release/firmware-heltec-v3-2.8.201.bin", data: bytes(3) },
    factory,
  ];
  const result = F.classifyBundle(entries);
  assert.equal(result.kind, "esp32");
  assert.equal(result.image.name, "firmware-heltec-v3-2.8.201.factory.bin");
  assert.equal(result.image.address, 0);
  assert.equal(result.image.data, factory.data);
});

test("bundle: the plain app image is offered for updates, never the factory image or the OTA Loader", () => {
  const entries = [
    { name: "firmware-heltec-v3-2.8.201.factory.bin", data: bytes(1) },
    { name: "firmware-heltec-v3-2.8.201.bin", data: bytes(2) },
    { name: "mt-esp32s3-ota.bin", data: bytes(3) },
  ];
  const { updateImage } = F.classifyBundle(entries);
  assert.equal(updateImage.name, "firmware-heltec-v3-2.8.201.bin");
  assert.deepEqual([...updateImage.data], [2]);
  assert.equal(
    F.classifyBundle([entries[0]]).updateImage,
    undefined,
    "no plain image, no update",
  );
});

test("bundle: the OTA Loader goes to the ota_1 offset taken from the build manifest", () => {
  const manifest = {
    part: [
      { name: "app0", subtype: "ota_0", offset: "0x10000" },
      { name: "app1", subtype: "ota_1", offset: "0x650000" },
    ],
  };
  const entries = [
    { name: "firmware-heltec-v4-2.8.201.factory.bin", data: bytes(1) },
    { name: "mt-esp32s3-ota.bin", data: bytes(7) },
    {
      name: "firmware-heltec-v4-2.8.201.mt.json",
      data: new TextEncoder().encode(JSON.stringify(manifest)),
    },
  ];
  const { extraImages } = F.classifyBundle(entries);
  assert.equal(extraImages.length, 1);
  assert.equal(extraImages[0].address, 0x650000);
  assert.equal(extraImages[0].name, "mt-esp32s3-ota.bin");
});

test("bundle: without loader, manifest or ota_1 there is no extra image to write", () => {
  const factory = { name: "firmware-x.factory.bin", data: bytes(1) };
  const noParts = {
    name: "firmware-x.mt.json",
    data: new TextEncoder().encode("{}"),
  };
  const loader = { name: "mt-esp32-ota.bin", data: bytes(7) };
  assert.deepEqual(F.classifyBundle([factory]).extraImages, []);
  assert.deepEqual(F.classifyBundle([factory, loader]).extraImages, []);
  assert.deepEqual(
    F.classifyBundle([factory, loader, noParts]).extraImages,
    [],
  );
});

test("bundle: nRF52 bundles are recognised by their UF2 and anything else is refused", () => {
  const result = F.classifyBundle([
    { name: "firmware-thinknode_m1.uf2", data: bytes(1) },
  ]);
  assert.equal(result.kind, "uf2");
  assert.throws(
    () => F.classifyBundle([{ name: "config_used.jsonc", data: bytes(1) }]),
    /ningún firmware/,
  );
});

// What bin/build-nrf52.sh puts in release/: the DFU zip, the build's UF2 and the helper UF2s of bin/.
const nrfBundle = (extra = []) => [
  {
    name: "release/Meshtastic_nRF52_factory_erase_v3_S140_7.3.0.uf2",
    data: bytes(0),
  },
  {
    name: "release/update-lilygo_techo_bootloader-0.6.1_nosd.uf2",
    data: bytes(0),
  },
  { name: "release/firmware-thinknode_m1-2.8.201-ota.zip", data: bytes(7) },
  { name: "release/firmware-thinknode_m1-2.8.201.uf2", data: bytes(8) },
  { name: "release/firmware-thinknode_m1-2.8.201.elf", data: bytes(9) },
  ...extra,
];

test("bundle: an nRF52 bundle flashes the DFU zip and offers the build's own UF2, never the erase or bootloader ones", () => {
  const result = F.classifyBundle(nrfBundle());
  assert.equal(result.kind, "nrf52");
  assert.equal(result.image.name, "firmware-thinknode_m1-2.8.201-ota.zip");
  assert.deepEqual([...result.image.data], [7]);
  assert.equal(result.uf2.name, "firmware-thinknode_m1-2.8.201.uf2");
  assert.deepEqual([...result.uf2.data], [8]);
});

test("bundle: with only helper UF2s there is nothing to flash, and a UF2-only bundle still works", () => {
  const helpersOnly = nrfBundle().filter((e) => !e.name.includes("firmware-"));
  assert.throws(() => F.classifyBundle(helpersOnly), /ningún firmware/);
  const uf2Only = nrfBundle().filter((e) => !e.name.endsWith("-ota.zip"));
  const result = F.classifyBundle(uf2Only);
  assert.equal(result.kind, "uf2");
  assert.equal(result.image.name, "firmware-thinknode_m1-2.8.201.uf2");
});

test("combined progress: several files weigh by size and the bar never goes back", () => {
  const sizes = [900, 100]; // firmware + OTA Loader
  assert.equal(F.combinedProgress(sizes, 0, 0), 0);
  assert.equal(F.combinedProgress(sizes, 0, 1), 0.9);
  assert.equal(
    F.combinedProgress(sizes, 1, 0),
    0.9,
    "the next file starts where the previous ended",
  );
  assert.equal(F.combinedProgress(sizes, 1, 1), 1);
  assert.equal(F.combinedProgress(sizes, 1, 5), 1, "fractions are clamped");
  assert.equal(F.combinedProgress([0], 0, 0.5), 0, "nothing to write");
  assert.equal(F.combinedProgress([500], 0, 0.5), 0.5);
});

test("dfu progress: erase, write and activate fill the flash phase in order without gaps", () => {
  assert.equal(F.nrfFlashProgress("erase", 0), 0);
  assert.equal(F.nrfFlashProgress("erase", 1), F.nrfFlashProgress("write", 0));
  assert.equal(
    F.nrfFlashProgress("write", 1),
    F.nrfFlashProgress("activate", 0),
  );
  assert.equal(F.nrfFlashProgress("activate", 1), 1);
  assert.ok(
    F.nrfFlashProgress("write", 0.5) > F.nrfFlashProgress("write", 0.4),
  );
  assert.throws(() => F.nrfFlashProgress("nope", 0), /desconocida/);
});

test("chip: reads the target from the header, also for the classic ESP32 bootloader at 0x1000", () => {
  assert.equal(F.imageChip(espImage(9)), "ESP32-S3");
  assert.equal(F.imageChip(espImage(5)), "ESP32-C3");
  assert.equal(F.imageChip(espImage(0, 0x1000)), "ESP32");
  assert.equal(F.imageChip(bytes(0, 1, 2, 3)), null);
  assert.equal(F.imageChip(espImage(999)), null, "unknown chip id");
});

test("chip: an image for another chip is refused, and ESP32 does not match ESP32-S3", () => {
  const s3 = espImage(9);
  assert.equal(
    F.chipMatchesImage("ESP32-S3 (QFN56) (revision v0.2)", s3).ok,
    true,
  );
  const wrong = F.chipMatchesImage("ESP32-D0WD-V3 (revision v3.1)", s3);
  assert.equal(wrong.ok, false);
  assert.match(wrong.reason, /ESP32-S3/);
  assert.equal(
    F.chipMatchesImage("ESP32-S3 (QFN56)", espImage(0, 0x1000)).ok,
    false,
    "S3 board, classic image",
  );
  assert.equal(F.chipMatchesImage("ESP32", espImage(0, 0x1000)).ok, true);
  assert.equal(F.chipMatchesImage("ESP32-S3", bytes(1, 2, 3)).ok, false);
});

test("format: sizes read as KB or MB", () => {
  assert.equal(F.formatBytes(2_283_696), "2.2 MB");
  assert.equal(F.formatBytes(2048), "2 KB");
});

test("chip: classic and suffixed descriptions map to their family", () => {
  assert.equal(F.chipFamily("ESP32-D0WD-V3 (revision v3.1)"), "ESP32");
  assert.equal(F.chipFamily("ESP32-PICO-D4 (revision v1.1)"), "ESP32");
  assert.equal(F.chipFamily("ESP32-S2FNR2 (revision v1.0)"), "ESP32-S2");
  assert.equal(F.chipFamily("ESP32-S3 (QFN56) (revision v0.2)"), "ESP32-S3");
  assert.equal(F.chipFamily("ESP32-C6 (QFN40)"), "ESP32-C6");
  assert.equal(F.chipMatchesImage("ESP32-D0WD-V3 (revision v3.1)", espImage(0, 0x1000)).ok, true);
  assert.equal(F.chipMatchesImage("ESP32-D0WDQ6", espImage(9, 0)).ok, false);
});

test("run: runs that existed before the dispatch are ignored", () => {
  const runs = [
    { id: 1, event: "workflow_dispatch", created_at: new Date(NOW - 5_000).toISOString() },
  ];
  assert.equal(F.pickRun(runs, { dispatchedAtMs: NOW }).id, 1);
  assert.equal(F.pickRun(runs, { dispatchedAtMs: NOW, ignoreRunIds: [1] }), null);
});
