// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as P from "../parsers.js";

const capture = JSON.parse(readFileSync(new URL("./fixtures/gat562-capture.json", import.meta.url)));
const out = (cmd) => capture.outputs.find((o) => o.command === cmd).response;

test("joinPages rebuilds text cut in the middle of a record", () => {
  const text = "banner\nP1: !aaaaaaaa:AB:1:5:K+ !bbbbbbbb:CD:2:-7\nP2: :K+ !cccccccc:EF:3:0:K+";
  assert.equal(P.joinPages("🔴 MESHTACORE: Paging X...\n" + text.replace("banner\n", "")), "!aaaaaaaa:AB:1:5:K+ !bbbbbbbb:CD:2:-7:K+ !cccccccc:EF:3:0:K+");
});

test("INFO", () => {
  assert.deepEqual(P.parseInfo(out("INFO")), {
    id: "!9bdbf569", shortName: "MGAT", role: "CLIENT", mac: "ee:11:e9:4e:25:8a", version: "2.8.201.a0260fb",
    target: "gat562_mesh_trial_tracker (nrf52)", repo: "derpyspike/jentastic", built: "2026-11-06", flags: "none",
  });
});

test("LORA", () => {
  assert.deepEqual(P.parseLora(out("LORA")), { sf: 7, bw: 62, cr: 6, preset: 0, presetState: "OFF", freq: 869.618, power: 22 });
});

test("LORA-STATUS", () => {
  assert.deepEqual(P.parseLoraStatus(out("LORA-STATUS")), {
    freq: 869.618, bw: 62.5, sf: 7, cr: 6, preambleSymbols: 16, preambleMs: 32, slotMs: 16, cwMin: 3, cwMax: 8, chipPower: 22, bitrate: 2278, syncWord: 0x2b,
  });
});

test("NODES: records survive page cuts and odd names", () => {
  const nodes = P.parseNodes(out("NODES"));
  assert.ok(nodes.length >= 100, "got " + nodes.length);
  assert.equal(nodes[0].id, "!9bdbf569");
  assert.equal(nodes[0].self, true);
  const split = nodes.find((n) => n.id === "!9d2fec97"); // cut between P1 and P2
  assert.deepEqual([split.name, split.hops, split.snr], ["DLF@", 2, -7]);
  assert.equal(nodes.find((n) => n.id === "!4d862682").name, "4.0 "); // trailing space in the name
  assert.equal(nodes.find((n) => n.id === "!4dcb2b42").name, "🐓");
  assert.ok(nodes.every((n) => /^![0-9a-f]{8}$/.test(n.id) && Number.isInteger(n.hops) && Number.isInteger(n.snr)));
  assert.equal(new Set(nodes.map((n) => n.id)).size, nodes.length, "no duplicated ids");
});

test("CHS: channel lines cut across pages are rebuilt", () => {
  const s = P.parseChannelStats(out("CHS"));
  assert.equal(s.totalPackets, 455);
  assert.equal(s.channels.length, 23);
  assert.deepEqual(s.channels[0], { index: 0, name: "SFNarrow", known: true, packets: 312, packetShare: 68.6, bytes: Math.round(14.1 * 1024), byteShare: 67.6 });
  assert.equal(s.channels[1].packetShare, 13); // "(15." / "2%)" boundary sits in the byte share, not here
  assert.equal(s.channels[1].byteShare, 15.2);
  assert.equal(s.channels.filter((c) => !c.known).length, 16);
  assert.equal(s.channels.reduce((a, c) => a + c.packets, 0), 455);
});

test("CH", () => {
  const ch = P.parseChannels(out("CH"));
  assert.equal(ch.length, 8);
  assert.deepEqual(ch[5], { index: 5, name: "MC:Public", role: 2 });
});

test("POWER without a sensor still yields utilisation", () => {
  assert.deepEqual(P.parsePower(out("POWER")), { channelUtil: 0.8, txUtil: 0.7, voltage: null, current: null, hasSensor: false });
});

test("VOLT", () => {
  assert.deepEqual(P.parseVolt(out("VOLT")), { battery: 4.19, hibernating: false });
});

test("every parser returns null/empty on garbage instead of throwing", () => {
  for (const f of [P.parseLora, P.parseLoraStatus, P.parsePower, P.parseVolt, P.parseInfo]) assert.equal(f("zzz"), null);
  assert.deepEqual(P.parseNodes(""), []);
  assert.equal(P.parseChannelStats("").channels.length, 0);
});
