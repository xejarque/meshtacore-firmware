// Run: node --test custom_builder_web/tests/
const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const manifest = require("../options.manifest.json");
const { emitNexusPrefs, emitOption } = require("../nexus-options.js");

const byId = (id) => manifest.options.find((o) => o.id === id);

test("an untouched form emits nothing (except the options that always had a value)", () => {
  const values = {};
  for (const o of manifest.options.filter((o) => !o.always)) {
    if (o.type === "bool") values[o.id] = Boolean(o.default);
    else if (o.type === "bitmask")
      values[o.id] = o.bits
        .filter((b) => (BigInt(o.default || 0) >> BigInt(b.bit)) & 1n)
        .map((b) => b.bit);
    else
      values[o.id] =
        o.default === null || o.default === undefined ? "" : String(o.default);
  }
  assert.deepEqual(emitNexusPrefs(manifest, values), {});
});

test("bool, int, enum and string are emitted only when they differ from the default", () => {
  assert.equal(emitOption(byId("pping_enabled"), true), "true");
  assert.equal(emitOption(byId("pping_rssi"), false), "false");
  assert.equal(emitOption(byId("meshcore_flood_max"), "10"), "10");
  assert.equal(emitOption(byId("meshcore_flood_max"), "64"), null);
  assert.equal(emitOption(byId("pping_emoji_mode"), "2"), "2");
  assert.equal(emitOption(byId("pping_trigger"), "!ping"), "!ping");
  assert.equal(emitOption(byId("pping_trigger"), "/ping"), null);
});

test("out-of-range values are not emitted", () => {
  assert.equal(emitOption(byId("meshcore_flood_max"), "65"), null);
  assert.equal(emitOption(byId("meshcore_path_hash_size"), "0"), null);
  assert.equal(emitOption(byId("preamble"), "5"), null);
  assert.equal(emitOption(byId("preamble"), "8"), "8");
  assert.equal(emitOption(byId("pping_emoji_mode"), "9"), null);
  assert.equal(emitOption(byId("broadcast_delay_max_s"), "301"), null);
});

test("string is cut to maxLen", () => {
  assert.equal(emitOption(byId("pping_trigger"), "x".repeat(40)).length, 15);
});

test("bitmasks: 64-bit hex, decimal channel mask, default skipped", () => {
  assert.equal(emitOption(byId("drop_mask"), [0, 48]), "0x1000000000001");
  assert.equal(emitOption(byId("features_mask"), [0, 3]), "0x9");
  assert.equal(emitOption(byId("pping_channels"), [0, 2]), "5");
  // pping_channels is `always`: its default (Bots + MC:#bot = 132) is emitted too, so the firmware never
  // falls back to its own built-in default (channel 0 only).
  assert.equal(emitOption(byId("pping_channels"), [2, 7]), "132");
  assert.equal(emitOption(byId("pping_channels"), [0]), "1");
  assert.equal(emitOption(byId("drop_mask"), []), null);
});

test("keys: C array passes through, base64 is converted", () => {
  assert.equal(
    emitOption(byId("master_key_2"), "{ 0x01, 0x02 }"),
    "{ 0x01, 0x02 }",
  );
  assert.equal(emitOption(byId("master_key_2"), "AQI="), "{ 0x01, 0x02 }");
  assert.equal(emitOption(byId("master_key_2"), ""), null);
});

test("migrated options keep their old output", () => {
  const out = emitNexusPrefs(manifest, {
    rebroadcast_mode: "ALL",
    meshcore_relay_mode: "ON",
    nexus_base_texts: false,
    meshcore_advert_hours: "12",
    node_info_broadcast_secs: "43200",
    fixed_ble_pin: "",
    smart_position: false,
    timezone_string: "CET-1CEST,M3.5.0,M10.5.0/3",
  });
  assert.deepEqual(out, {
    USERPREFS_CONFIG_DEVICE_REBROADCAST_MODE:
      "meshtastic_Config_DeviceConfig_RebroadcastMode_ALL",
    USERPREFS_MESHCORE_RELAY_MODE: "2",
    USERPREFS_NEXUS_BASE_TEXTS: "false",
    USERPREFS_MESHCORE_ADVERT_HOURS: "12",
    USERPREFS_CONFIG_DEVICE_NODE_INFO_BROADCAST_SECS: "43200",
    USERPREFS_TZ_STRING: "CET-1CEST,M3.5.0,M10.5.0/3",
  });
});

test("every manifest key is unique and well formed", () => {
  const keys = manifest.options.map((o) => o.key);
  assert.equal(new Set(keys).size, keys.length);
  for (const o of manifest.options) {
    assert.ok(
      manifest.groups.some((g) => g.id === o.group),
      `group of ${o.id}`,
    );
    if (o.type === "bitmask")
      for (const b of o.bits)
        assert.ok(b.bit < o.width, `${o.id} bit ${b.bit}`);
  }
});

test("invalidValues reports what emitOption would silently drop", () => {
  const { invalidValues } = require("../nexus-options.js");
  const intOpt = manifest.options.find((o) => o.type === "int");
  assert.deepEqual(invalidValues(manifest, { [intOpt.id]: "" }), []);
  assert.deepEqual(
    invalidValues(manifest, { [intOpt.id]: String(intOpt.min) }),
    [],
  );
  assert.equal(
    invalidValues(manifest, { [intOpt.id]: String(intOpt.max + 1) }).length,
    1,
  );
  assert.equal(invalidValues(manifest, { [intOpt.id]: "abc" }).length, 1);
});

test("a bit with `roles` names real device roles, and latebase is CLIENT_BASE only", () => {
  const known = [
    "CLIENT",
    "CLIENT_BASE",
    "CLIENT_MUTE",
    "ROUTER",
    "ROUTER_LATE",
    "LOST_AND_FOUND",
  ];
  for (const o of manifest.options.filter((o) => o.type === "bitmask"))
    for (const b of o.bits.filter((b) => b.roles))
      for (const r of b.roles)
        assert.ok(known.includes(r), `${o.id} bit ${b.bit}: ${r}`);
  const bits = manifest.options.flatMap((o) =>
    o.type === "bitmask" ? o.bits : [],
  );
  assert.deepEqual(bits.find((b) => b.id === "latebase").roles, [
    "CLIENT_BASE",
  ]);
});

test("channel masks follow the firmware numbering: position among the enabled channels", () => {
  const { remapChannelBits } = require("../nexus-options.js");
  const opt = manifest.options.find((o) => o.labelsFromChannels);
  const rows = (...on) => Array.from({ length: 8 }, (_, i) => on.includes(i));
  const all = rows(0, 1, 2, 3, 4, 5, 6, 7);
  // Every row enabled: nothing moves.
  assert.deepEqual(
    remapChannelBits(manifest, { [opt.id]: [2, 7] }, all)[opt.id],
    [2, 7],
  );
  // Row 1 off: rows 2 and 7 are channels 1 and 6 on the device.
  assert.deepEqual(
    remapChannelBits(manifest, { [opt.id]: [2, 7] }, rows(0, 2, 3, 4, 5, 6, 7))[
      opt.id
    ],
    [1, 6],
  );
  // A disabled row never reaches the firmware, so its bit is dropped.
  assert.deepEqual(
    remapChannelBits(manifest, { [opt.id]: [2, 7] }, rows(0))[opt.id],
    [],
  );
  // Other options are left alone.
  assert.deepEqual(
    remapChannelBits(manifest, { other: [3] }, rows(0)).other,
    [3],
  );
});
