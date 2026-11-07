// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import * as S from "../settings-core.js";

const here = (p) => new URL(p, import.meta.url);
const json = (p) => JSON.parse(readFileSync(here(p)));
const manifest = json("../admin.commands.json").commands;
// INVALID / UNINVALID are defined in section 3.3 of the reference but the section 17 index only lists INVALIDS.
const DOCUMENTED_OUTSIDE_THE_INDEX = ["INVALID", "UNINVALID"];
const known = new Set([...manifest.flatMap((c) => [c.name, ...c.aliases]), ...DOCUMENTED_OUTSIDE_THE_INDEX].map((n) => n.toUpperCase()));
const catalogues = readdirSync(here("../settings/")).filter((f) => f.endsWith(".json") && f !== "index.json").map((f) => json("../settings/" + f));
const entries = catalogues.flatMap((c) => c.entries);
const firewall = json("./fixtures/gat562-firewall.json");
const reply = (cmd) => firewall.outputs.find((o) => o.command === cmd).response;
const entry = (id) => entries.find((e) => e.id === id);

test("every catalogue entry is well formed and only uses commands that exist", () => {
  const problems = entries.flatMap((e) => S.validateEntry(e, known));
  assert.deepEqual(problems, []);
});

test("entry ids are unique, and so are the keys inside a flags entry", () => {
  assert.equal(new Set(entries.map((e) => e.id)).size, entries.length);
  for (const e of entries.filter((e) => e.flags)) assert.equal(new Set(e.flags.map((f) => f.key)).size, e.flags.length, e.id);
});

test("settings/index.json lists exactly the catalogue files, in order", () => {
  const files = readdirSync(here("../settings/")).filter((f) => f.endsWith(".json") && f !== "index.json").sort();
  assert.deepEqual(json("../settings/index.json"), files);
});

test("cleanReply drops the signature emoji and the MESHTACORE prefix", () => {
  assert.equal(S.cleanReply("🔴 MESHTACORE: TR filter ACTIVE"), "TR filter ACTIVE");
  assert.equal(S.cleanReply("🔒 MESHTACORE SECURITY: rejected"), "SECURITY: rejected");
});

test("isNexusReply keeps Nexus replies and pages, drops ordinary mesh chatter", () => {
  assert.ok(S.isNexusReply("🔴 MESHTACORE ACTIVE DROPS:\nTR"));
  assert.ok(S.isNexusReply("P2: = PRALLOW == (empty)"));
  assert.ok(!S.isNexusReply("@[NMG PORTABLE]: 3 hops [3.00s]"));
  assert.ok(!S.isNexusReply("hola, alguien por Barcelona?"));
});

test("DROPS: nothing of ours is active, the P_OFF/T_OFF suffix is not a flag", () => {
  const flags = S.readFlags(entry("fw.drops"), reply("DROPS"));
  assert.ok(Object.values(flags).every((v) => v === false), JSON.stringify(flags));
  assert.equal(Object.keys(flags).length, entry("fw.drops").flags.length);
});

test("DROPS: active tags are read by alias or by full name (shape verified on the node: TR BRNI P_OFF T_OFF)", () => {
  const live = "🔴 MESHTACORE ACTIVE DROPS:\nTR BRNI P_OFF T_OFF ";
  const flags = S.readFlags(entry("fw.drops"), live);
  assert.equal(flags.TR, true);
  assert.equal(flags.BRNI, true);
  assert.equal(flags.NI, false);
  assert.equal(S.readFlags(entry("fw.drops"), "🔴 MESHTACORE ACTIVE DROPS:\nTRACEROUTE_DIRECT").TRD, true);
});

test("an unexpected reply reads as unknown (null), never as off", () => {
  const flags = S.readFlags(entry("fw.drops"), "🔴 MESHTACORE: Unknown command");
  assert.ok(Object.values(flags).every((v) => v === null));
});

test("OUTDROPS empty list", () => {
  assert.ok(Object.values(S.readFlags(entry("fw.outdrops"), reply("OUTDROPS"))).every((v) => v === false));
});

test("INVALID: pairs, including the GERM abbreviation", () => {
  const flags = S.readFlags(entry("fw.invalid"), reply("INVALID"));
  assert.deepEqual(flags, { NULL: false, SPAIN: false, GERMANY: false, HW: false, ID: false });
  assert.equal(S.readFlags(entry("fw.invalid"), "MESHTACORE INVALID Rules:\nNULL:ON (3) SPAIN:OFF (0)\nGERM:ON (1) HW:OFF (0)\nID:OFF (0)").GERMANY, true);
});

test("lists: empty lists read as empty, not as unknown", () => {
  for (const id of ["fw.relay_drop", "fw.idr", "fw.prwhitelist", "fw.prblacklist"]) {
    const cmd = entry(id).read.cmd;
    assert.deepEqual(S.readList(entry(id), reply(cmd)), [], id);
  }
  assert.deepEqual(S.readList(entry("fw.ignored"), reply("IGNORED")), []);
  assert.deepEqual(S.readList(entry("fw.nign"), reply("NIGN")), []);
});

test("lists: favourites come back as ids", () => {
  assert.deepEqual(S.readList(entry("fw.favorites"), reply("FAVS")), [{ value: "!f3a45b84", label: "!f3a45b84:5b84" }]);
});

test("lists: relay ids and channel hashes", () => {
  assert.deepEqual(S.readList(entry("fw.relay_drop"), "🔴 MESHTACORE RDROP: *A4 *1B ").map((i) => i.value), ["*A4", "*1B"]);
  assert.deepEqual(S.readList(entry("fw.prwhitelist"), "🔴 MESHTACORE Whitelist: 195(0xC3) ").map((i) => i.value), ["195(0xC3)"]);
});

test("flag commands: the key travels, ON and OFF use different verbs", () => {
  const e = entry("fw.drops");
  const tr = e.flags.find((f) => f.key === "TR");
  assert.equal(S.flagCommand(e, tr, true), "DROP TR");
  assert.equal(S.flagCommand(e, tr, false), "UNDROP TR");
  const inv = entry("fw.invalid");
  assert.equal(S.flagCommand(inv, inv.flags.find((f) => f.key === "GERMANY"), false), "UNINVALID GERMANY");
});

test("fill refuses an empty value, safeArg refuses a second command smuggled in", () => {
  assert.throws(() => S.fill("RDROP {value}", { value: "" }), /falta el valor/);
  assert.ok(S.safeArg("!abcd1234"));
  assert.ok(!S.safeArg("A4\nSAVE"));
  assert.ok(!S.safeArg("A4; SAVE"));
});
