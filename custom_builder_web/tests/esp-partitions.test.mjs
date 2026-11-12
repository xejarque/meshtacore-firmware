// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import {
  chooseFlashMode,
  hasAppImage,
  loaderToWrite,
  parsePartitionTable,
  planAppUpdate,
  planOtaLoader,
} from "../esp-partitions.js";

function entry(type, subtype, offset, size, label) {
  const out = new Uint8Array(32);
  out.set([0xaa, 0x50, type, subtype]);
  new DataView(out.buffer).setUint32(4, offset, true);
  new DataView(out.buffer).setUint32(8, size, true);
  out.set(new TextEncoder().encode(label), 12);
  return out;
}

function table(...entries) {
  const out = new Uint8Array(0xc00).fill(0xff);
  entries.forEach((e, i) => out.set(e, i * 32));
  return out;
}

// What Meshtastic's 16 MB layout looks like: nvs, otadata, app0 (ota_0), app1 (the OTA Loader), spiffs
const meshtastic = () =>
  table(
    entry(1, 2, 0x9000, 0x5000, "nvs"),
    entry(1, 0, 0xe000, 0x2000, "otadata"),
    entry(0, 0x10, 0x10000, 0x640000, "app0"),
    entry(0, 0x11, 0x650000, 0x20000, "app1"),
    entry(1, 0x82, 0x670000, 0x980000, "spiffs"),
  );

test("table: entries are read until the blank flash, with their label and little-endian fields", () => {
  const entries = parsePartitionTable(meshtastic());
  assert.equal(entries.length, 5);
  assert.deepEqual(entries[2], {
    type: 0,
    subtype: 0x10,
    offset: 0x10000,
    size: 0x640000,
    label: "app0",
  });
});

test("table: the MD5 marker entry ends the table", () => {
  const md5 = new Uint8Array(32).fill(0xff);
  md5.set([0xeb, 0xeb]);
  assert.equal(
    parsePartitionTable(table(entry(0, 0x10, 0x10000, 0x100000, "app0"), md5))
      .length,
    1,
  );
});

test("plan: the app goes to ota_0 and the boot selector is blanked, leaving NVS and the loader alone", () => {
  const plan = planAppUpdate(meshtastic(), 2_217_728);
  assert.deepEqual(plan.app, {
    offset: 0x10000,
    size: 0x640000,
    label: "app0",
  });
  assert.deepEqual(plan.otadata, { offset: 0xe000, size: 0x2000 });
});

test("plan: a factory slot is preferred when there is one", () => {
  const plan = planAppUpdate(
    table(
      entry(0, 0x10, 0x20000, 0x100000, "ota"),
      entry(0, 0, 0x10000, 0x100000, "factory"),
    ),
    1000,
  );
  assert.equal(plan.app.label, "factory");
  assert.equal(plan.otadata, null);
});

test("plan: a blank table, no app slot or an image that does not fit are refused with a pointer to the full flasher", () => {
  assert.throws(
    () => planAppUpdate(new Uint8Array(0xc00).fill(0xff), 10),
    /tabla de particiones/,
  );
  assert.throws(
    () => planAppUpdate(table(entry(1, 2, 0x9000, 0x5000, "nvs")), 10),
    /ranura de aplicación/,
  );
  assert.throws(() => planAppUpdate(meshtastic(), 0x640001), /no cabe/);
});

test("loader slot: ota_1 (app1) is where the OTA Loader goes, and a loader that does not fit is refused", () => {
  assert.deepEqual(planOtaLoader(meshtastic(), 100_000), {
    offset: 0x650000,
    size: 0x20000,
  });
  assert.throws(() => planOtaLoader(meshtastic(), 0x20001), /no cabe/);
});

test("loader slot: a table without ota_1 has nothing to add", () => {
  assert.equal(
    planOtaLoader(table(entry(0, 0x10, 0x10000, 0x100000, "app0")), 1000),
    null,
  );
  assert.equal(planOtaLoader(new Uint8Array(0xc00).fill(0xff), 1000), null);
});

test("loader write: only an empty slot gets the loader; one that holds an app is left alone", () => {
  const slot = { offset: 0x650000, size: 0x20000 };
  const loader = { name: "mt-esp32s3-ota.bin", data: new Uint8Array([1, 2]) };
  const blank = new Uint8Array(16).fill(0xff);
  const app = new Uint8Array(16).fill(0xff);
  app[0] = 0xe9;
  assert.deepEqual(loaderToWrite(slot, blank, loader), {
    name: "mt-esp32s3-ota.bin",
    data: loader.data,
    address: 0x650000,
  });
  assert.equal(loaderToWrite(slot, app, loader), null);
  assert.equal(loaderToWrite(null, blank, loader), null);
  assert.equal(loaderToWrite(slot, blank, null), null);
  assert.equal(hasAppImage(new Uint8Array()), false);
});

test("mode: a Meshtastic board is updated, a blank or foreign one is installed from scratch", () => {
  const known = chooseFlashMode(meshtastic(), 2_217_728);
  assert.equal(known.mode, "update");
  assert.equal(known.plan.app.label, "app0");

  const blank = chooseFlashMode(new Uint8Array(0xc00).fill(0xff), 10);
  assert.equal(blank.mode, "install");
  assert.match(blank.reason, /tabla de particiones/);

  const other = chooseFlashMode(meshtastic(), 0x640001);
  assert.equal(other.mode, "install");
  assert.match(other.reason, /no cabe/);
});
