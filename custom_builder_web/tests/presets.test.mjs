// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as R from "../presets.js";
import { parseLora } from "../parsers.js";

const { presets } = JSON.parse(readFileSync(new URL("../admin.presets.json", import.meta.url)));
const capture = JSON.parse(readFileSync(new URL("./fixtures/gat562-capture.json", import.meta.url)));
const byCmd = (c) => presets.find((p) => p.command === c);

test("bit rate matches the node's own LORA-STATUS figure (SF7 BW62.5 CR4/6 = 2278 b/s)", () => {
  assert.equal(Math.floor(R.bitrate(7, 62.5, 6)), 2278); // the node truncates the fraction
});

test("LongFast bit rate is the well known ~1.07 kb/s", () => {
  assert.ok(Math.abs(R.bitrate(11, 250, 5) - 1074) < 2);
});

test("a higher SF or a narrower BW hears weaker signals", () => {
  assert.ok(R.sensitivity(12, 125) < R.sensitivity(7, 125));
  assert.ok(R.sensitivity(7, 62.5) < R.sensitivity(7, 250));
});

test("every preset has a sane SF, BW, CR and a unique unambiguous command", () => {
  assert.equal(presets.length, 46);
  assert.equal(new Set(presets.map((p) => p.command)).size, presets.length);
  for (const p of presets) {
    assert.ok(p.sf >= 5 && p.sf <= 12, p.command);
    assert.ok(p.cr >= 5 && p.cr <= 8, p.command);
    assert.ok(["EU_868", "EU_N_868", "EU_866", "LORA_24"].includes(p.region), p.command);
    assert.ok(Number.isFinite(R.describe(p).bitrate), p.command);
  }
});

test("the bare ambiguous codes (LF, MS, ...) are never the command", () => {
  for (const code of ["LF", "MS", "MF", "SS", "SF", "LM", "LS", "ST", "LT"]) assert.ok(!presets.some((p) => p.command === code), code);
});

test("the node's current radio (captured) is recognised as SFNarrow / P1", () => {
  const lora = parseLora(capture.outputs.find((o) => o.command === "LORA").response);
  assert.equal(R.matchPreset(lora, presets).command, "P1");
});

test("an unknown radio matches nothing", () => {
  assert.equal(R.matchPreset({ sf: 9, bw: 50, freq: 433 }, presets), null);
  assert.equal(R.matchPreset(null, presets), null);
});

test("compareTo: LongSlow against LongFast is slower and reaches further", () => {
  const c = R.compareTo(byCmd("LS868"), byCmd("LF868"));
  assert.ok(c.speed < 1 && c.reach > 0);
  assert.match(c.text, /más lento/);
  assert.equal(R.compareTo(byCmd("LF868"), byCmd("LF868")).text, "misma velocidad, mismo alcance");
});

test("2.4 GHz presets are flagged as needing a dual-band radio", () => {
  assert.equal(R.needsDualBand(byCmd("MADRID24")), true);
  assert.equal(R.needsDualBand(byCmd("LF868")), false);
});
