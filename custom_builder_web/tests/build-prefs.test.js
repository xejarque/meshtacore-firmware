// Run: node --test custom_builder_web/tests/
const test = require("node:test");
const assert = require("node:assert/strict");
const manifest = require("../options.manifest.json");
const NexusOptions = require("../nexus-options.js");
const {
  buildUserPrefs,
  formatPskToCArray,
  MQTT_DEFAULTS,
} = require("../build-prefs.js");

const channel = (name, enabled = true) => ({
  enabled,
  name,
  psk: "AQ==",
  precision: 0,
  uplink: false,
  downlink: false,
});

// What getFormData() returns for an untouched companion form, with the fields the tests care about overridable.
const form = (over = {}) => ({
  device_role: "CLIENT",
  node_preset: "companion",
  lora_region: "EU_868",
  gps_mode: "ENABLED",
  has_screen: true,
  enable_bluetooth: true,
  lora_preset_mode: "modem_preset_long_fast",
  mqtt_enabled: false,
  mqtt_root: "msh/EU_868",
  ...over,
});

// No manifest/nexusOptions: only the form-to-prefs logic is under test.
const ctx = (channels = []) => ({ channels });

test("only the solar repeater profile asks the firmware to allow ROUTER", () => {
  const solar = buildUserPrefs(
    form({ node_preset: "solar_repeater", device_role: "ROUTER" }),
    ctx(),
  );
  assert.equal(solar.USERPREFS_ALLOW_ROUTER_ROLE, "1");
  assert.equal(
    solar.USERPREFS_CONFIG_DEVICE_ROLE,
    "meshtastic_Config_DeviceConfig_Role_ROUTER",
  );
  for (const over of [
    { node_preset: "companion", device_role: "ROUTER" },
    { node_preset: "solar_repeater", device_role: "CLIENT" },
    { node_preset: "companion", device_role: "CLIENT" },
  ])
    assert.equal(
      "USERPREFS_ALLOW_ROUTER_ROLE" in buildUserPrefs(form(over), ctx()),
      false,
      JSON.stringify(over),
    );
});

test("MQTT off emits only the switch; on emits the defaults for empty fields", () => {
  const off = buildUserPrefs(form({ mqtt_enabled: false }), ctx());
  assert.equal(off.USERPREFS_MQTT_ENABLED, "0");
  assert.equal(
    Object.keys(off).some(
      (k) => k.startsWith("USERPREFS_MQTT_") && k !== "USERPREFS_MQTT_ENABLED",
    ),
    false,
  );
  const on = buildUserPrefs(form({ mqtt_enabled: true }), ctx());
  assert.equal(on.USERPREFS_MQTT_ADDRESS, MQTT_DEFAULTS.address);
  assert.equal(on.USERPREFS_MQTT_USERNAME, MQTT_DEFAULTS.username);
  assert.equal(on.USERPREFS_MQTT_PASSWORD, MQTT_DEFAULTS.password);
  assert.equal(on.USERPREFS_MQTT_ROOT_TOPIC, "msh/EU_868");
});

test("channel slots count only the enabled channels, in order", () => {
  const prefs = buildUserPrefs(
    form(),
    ctx([channel("A"), channel("B", false), channel("C")]),
  );
  assert.equal(prefs.USERPREFS_CHANNELS_TO_WRITE, "2");
  assert.equal(prefs.USERPREFS_CHANNEL_0_NAME, "A");
  assert.equal(prefs.USERPREFS_CHANNEL_1_NAME, "C");
  assert.equal("USERPREFS_CHANNEL_2_NAME" in prefs, false);
});

test("a bad channel key names the channel instead of falling back to the public key", () => {
  const bad = { ...channel("Oops"), psk: "not a key!" };
  assert.throws(() => buildUserPrefs(form(), ctx([bad])), /Canal "Oops"/);
});

test("the bot's channel mask follows the enabled channels end to end", () => {
  const opt = manifest.options.find((o) => o.labelsFromChannels);
  const nexus = {
    channels: [
      channel("SFNarrow"),
      channel("Test", false),
      channel("Bots"),
      channel("MC:#bot"),
    ],
    nexusManifest: manifest,
    // The page reads checkboxes; here the user ticked rows 2 (Bots) and 3 (MC:#bot).
    nexusOptions: {
      ...NexusOptions,
      readValues: () => ({ [opt.id]: [2, 3] }),
      invalidValues: () => [],
    },
  };
  const prefs = buildUserPrefs(form(), nexus);
  // Rows 2 and 3 are device channels 1 and 2 because row 1 is not written.
  assert.equal(prefs[opt.key], String((1 << 1) | (1 << 2)));
});

test("master key: a predefined name or a byte array is accepted, anything else is ignored", () => {
  assert.match(
    buildUserPrefs(form({ nexus_master_key: "eevee" }), ctx())
      .NEXUS_MASTER_KEY_1,
    /^\{ 0x9a, /,
  );
  assert.equal(
    buildUserPrefs(form({ nexus_master_key: "{ 0x01 }" }), ctx())
      .NEXUS_MASTER_KEY_1,
    "{ 0x01 }",
  );
  assert.equal(
    "NEXUS_MASTER_KEY_1" in
      buildUserPrefs(form({ nexus_master_key: "nope" }), ctx()),
    false,
  );
});

test("formatPskToCArray: index, hex, base64 and rejects", () => {
  assert.equal(formatPskToCArray(""), "{ 0x01 }");
  assert.equal(formatPskToCArray("2"), "{ 0x02 }");
  assert.equal(formatPskToCArray("AQ=="), "{ 0x01 }");
  assert.equal(formatPskToCArray("00".repeat(16)).split(",").length, 16);
  assert.throws(() => formatPskToCArray("256"), /no cabe en un byte/);
  assert.throws(() => formatPskToCArray("AAAA"), /debe tener 1, 16 o 32/);
  assert.throws(() => formatPskToCArray("{ 1, 2 }"), /0x/);
});
