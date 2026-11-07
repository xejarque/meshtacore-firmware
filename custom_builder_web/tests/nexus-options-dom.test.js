// Run: node --test custom_builder_web/tests/
// El renderizado de nexus-options.js con un DOM mínimo (dom-shim.js): reparto de canales y roles.
const test = require("node:test");
const assert = require("node:assert/strict");
const manifest = require("../options.manifest.json");
const { makeDocument } = require("./dom-shim.js");

const NAMES = [
  "SFNarrow",
  "Test",
  "Bots",
  "Catalunya",
  "Barcelona",
  "MC:Public",
  "MC:#test",
  "MC:#bot",
];
const pping = manifest.options.find((o) => o.id === "pping_channels");

function page(names = [...NAMES]) {
  global.document = makeDocument();
  const NexusOptions = require("../nexus-options.js");
  const container = document.createElement("div");
  document.body.appendChild(container);
  NexusOptions.render(manifest, container, { channelName: (i) => names[i] });
  const bits = (side) =>
    document
      .getElementById(`nx_${pping.id}__${side}`)
      .querySelectorAll("input")
      .map((i) => Number(i.dataset.nxBit));
  const checked = () => NexusOptions.readValues(manifest, {})[pping.id];
  const sw = () => document.getElementById(`nx_${pping.id}__switch`);
  return { NexusOptions, names, bits, checked, sw };
}

test("the bots group draws Meshtastic, MeshCore and shared sections in that order", () => {
  page();
  const titles = document.body.querySelectorAll("h4").map((h) => h.textContent);
  assert.deepEqual(titles, [
    "Meshtastic",
    "MeshCore",
    "Compartido (Meshtastic y MeshCore)",
  ]);
});

test("channels are split by name, and the default selection and switch match /mc PPING ON", () => {
  const p = page();
  assert.deepEqual(p.bits("plain"), [0, 1, 2, 3, 4]);
  assert.deepEqual(p.bits("prefixed"), [5, 6, 7]);
  assert.deepEqual(p.checked(), [2, 7]);
  assert.equal(p.sw().checked, true);
});

test("the MeshCore switch only moves the MC: channels, like /mc PPING OFF and ON", () => {
  const p = page();
  p.sw().click(); // off
  assert.deepEqual(
    p.checked(),
    [2],
    "Meshtastic channel untouched, every MC: channel cleared",
  );
  p.sw().click(); // on: only MC:#bot, as the command does
  assert.deepEqual(p.checked(), [2, 7]);
});

test("ticking a MeshCore channel by hand keeps the switch in step", () => {
  const p = page();
  const box = (bit) =>
    document.body.querySelector(
      `input[data-nx-opt="${pping.id}"][data-nx-bit="${bit}"]`,
    );
  box(7).click(); // MC:#bot off
  assert.equal(p.sw().checked, false);
  box(5).click(); // MC:Public on
  assert.equal(p.sw().checked, true);
  assert.deepEqual(p.checked(), [2, 5]);
});

test("renaming a channel moves it between lists and keeps what was ticked", () => {
  const p = page();
  p.names[7] = "Otro"; // no longer MeshCore
  p.NexusOptions.refreshBitLabels();
  assert.deepEqual(p.bits("plain"), [0, 1, 2, 3, 4, 7]);
  assert.deepEqual(p.bits("prefixed"), [5, 6]);
  assert.deepEqual(p.checked(), [2, 7]);
  assert.equal(p.sw().checked, false, "no MeshCore channel is ticked any more");
});

test("without an MC:#bot channel the switch has nothing to turn on", () => {
  const p = page([
    "SFNarrow",
    "Test",
    "Bots",
    "Catalunya",
    "Barcelona",
    "MC:Public",
    "MC:#test",
    "MC:otro",
  ]);
  p.sw().click(); // off
  p.sw().click(); // on, but there is no MC:#bot
  assert.equal(p.sw().checked, false);
  assert.deepEqual(p.checked(), [2]);
});

test("with no MeshCore channel at all the switch is disabled and the list says so", () => {
  const p = page(NAMES.map((n) => n.replace("MC:", "")));
  assert.deepEqual(p.bits("prefixed"), []);
  assert.equal(p.sw().disabled, true);
  assert.match(
    document.getElementById(`nx_${pping.id}__prefixed`).textContent,
    /Ningún canal/,
  );
});

test("latebase is shown and kept only with CLIENT_BASE", () => {
  const p = page();
  const features = manifest.options.find(
    (o) => o.bits && o.bits.some((b) => b.id === "latebase"),
  );
  const bit = features.bits.find((b) => b.id === "latebase").bit;
  const label = () =>
    document.body.querySelector(
      `label[data-nx-opt="${features.id}"][data-nx-bit-label="${bit}"]`,
    );
  const input = () => label().querySelector("input");
  p.NexusOptions.applyRoles(manifest, "CLIENT_BASE");
  assert.equal(label().hidden, false);
  input().click();
  assert.ok(p.NexusOptions.readValues(manifest, {})[features.id].includes(bit));
  p.NexusOptions.applyRoles(manifest, "CLIENT");
  assert.equal(label().hidden, true);
  assert.equal(
    p.NexusOptions.readValues(manifest, {})[features.id].includes(bit),
    false,
  );
});
