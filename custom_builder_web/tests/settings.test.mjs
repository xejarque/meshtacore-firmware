// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import * as S from "../settings-core.js";

const here = (p) => new URL(p, import.meta.url);
const json = (p) => JSON.parse(readFileSync(here(p)));
const manifest = json("../admin.commands.json").commands;
// INVALID / UNINVALID are defined in section 3.3 of the reference but the section 17 index only lists INVALIDS.
// IGNORMQTT and TXPOWER are documented in section 7 but the manifest parser misses them (checked in NexusCommands.cpp).
const DOCUMENTED_OUTSIDE_THE_INDEX = ["INVALID", "UNINVALID", "IGNORMQTT", "TXPOWER"];
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

test("SECURITY and FEATURES: every switch reads off on the GAT562, including keys with a suffix (FAV_XT)", () => {
  const sec = S.readFlags(entry("sec.rules"), reply("SECURITY"));
  assert.deepEqual(sec, { REQ_SIG: false, ALLOW_DM: false, SILENT: false, FAV_NX: false, FAV_TR: false, BYPASS_RP: false });
  const feat = S.readFlags(entry("sec.features"), reply("FEATURES"));
  assert.ok(Object.values(feat).every((v) => v === false));
  assert.equal(Object.keys(feat).length, 4);
});

test("SECURITY: a flag that is on reads on, and the keys travel as the firmware names them", () => {
  const e = entry("sec.rules");
  assert.equal(S.readFlags(e, "🔴 MESHTACORE SECURITY:\n REQ_SIG:on\n ALLOW_DM:off\n SILENT:on").SILENT, true);
  assert.equal(S.flagCommand(e, e.flags.find((f) => f.key === "SILENT"), true), "SECURITY SILENT_LOG ON");
  assert.equal(S.flagCommand(e, e.flags.find((f) => f.key === "FAV_NX"), false), "SECURITY AUTO_FAV_NEXUS OFF");
});

test("SECURITY: the auto-favourite channel is read next to its on/off state", () => {
  const e = entry("sec.autofav_extra");
  assert.deepEqual(S.readValues(e, reply("SECURITY")), { state: "OFF", values: { ch: 0 } });
  assert.equal(S.valuesCommand(e, { ch: 3 }), "SECURITY AFX 3");
  assert.throws(() => S.valuesCommand(e, { ch: 8 }), /entre 0 y 7/);
});

test("ZH: the one-line reply gives the master switch and the four sub switches", () => {
  const e = entry("zh.switches");
  assert.deepEqual(S.readFlags(e, reply("ZH")), { ZH: false, POS: false, TELE: false, RTR: false, CLI: false });
  const live = S.readFlags(e, "🔴 MESHTACORE: ZH ON (POS:ON TELE:OFF RTR:ON[12] CLI:OFF[0])");
  assert.deepEqual(live, { ZH: true, POS: true, TELE: false, RTR: true, CLI: false });
  assert.equal(S.flagCommand(e, e.flags.find((f) => f.key === "RTR"), true), "ZH ROUTER ON");
});

test("HOPMASK: values and state come from the status line, and out-of-range input is refused", () => {
  const e = entry("hop.global");
  assert.deepEqual(S.readValues(e, reply("HOPMASK")), { state: "OFF", values: { s: 3, l: 3 } });
  assert.equal(S.readValues(e, "🔴 MESHTACORE Hop Masking: ACTIVE (S:2, L:5)").state, "ACTIVE");
  assert.equal(S.valuesCommand(e, { s: 2, l: 5 }), "HOPMASK 2 5");
  assert.throws(() => S.valuesCommand(e, { s: 9, l: 1 }), /entre 0 y 7/);
  assert.throws(() => S.valuesCommand(e, { s: "x", l: 1 }), /entre 0 y 7/);
});

const radio = json("./fixtures/gat562-radio.json");
const radioReply = (cmd) => radio.outputs.find((o) => o.command === cmd).response;

test("radio: role and rebroadcast mode are read as the option they are, and sent by digit or name", () => {
  const role = entry("radio.role");
  assert.equal(S.readEnum(role, radioReply("ROLE")).current.title, "CLIENT");
  assert.equal(S.enumCommand(role, "12"), "ROLE 12");
  assert.throws(() => S.enumCommand(role, "99"), /no válida/);
  const rb = entry("radio.rebroadcast");
  assert.equal(S.readEnum(rb, radioReply("REBROADCAST")).current.value, "ALL");
  assert.equal(S.readEnum(rb, "🔴 MESHTACORE: Rebroadcast Mode is NONE (4)").current.value, "NONE");
  assert.equal(S.readEnum(role, "🔴 MESHTACORE ROLE: REPEATER (4)").current, null);
  assert.equal(S.readEnum(role, "🔴 MESHTACORE ROLE: REPEATER (4)").raw, "4");
});

test("radio: toggles, power and preamble read from the real replies", () => {
  assert.deepEqual(S.readFlags(entry("radio.client_always"), radioReply("CLIENTALWAYS")), { CLIENTALWAYS: false });
  assert.deepEqual(S.readFlags(entry("radio.base_texts"), radioReply("BASETEXTS")), { BASETEXTS: true });
  assert.deepEqual(S.readFlags(entry("radio.ignore_mqtt"), radioReply("LORA")), { IGNORMQTT: false });
  assert.deepEqual(S.readValues(entry("radio.txpower"), radioReply("GETTXPOWER")).values, { dbm: 22 });
  assert.equal(S.readValues(entry("radio.txpower"), "🔴 MESHTACORE TX Power:\nActive: -5 dBm").values.dbm, -5);
  assert.equal(S.valuesCommand(entry("radio.txpower"), { dbm: 14 }), "TXPOWER 14");
  assert.deepEqual(S.readValues(entry("radio.preamble"), radioReply("PREAMBLE")).values, { len: 0 });
  assert.throws(() => S.valuesCommand(entry("radio.preamble"), { len: 2 }), /entre 6 y 1024/);
});

const system = json("./fixtures/gat562-system.json");
const sysReply = (cmd) => system.outputs.find((o) => o.command === cmd).response;

test("CONFIG: every interval and switch is read from the one multi-line reply", () => {
  const value = (id) => S.readValues(entry(id), sysReply("CONFIG")).values.secs;
  assert.equal(value("cfg.ni"), 43200);
  assert.equal(value("cfg.pos"), 2147483);
  assert.equal(value("cfg.tel_d"), 2147483);
  assert.equal(value("cfg.tel_e"), 3600);
  assert.equal(value("cfg.tel_p"), 3600);
  assert.deepEqual(S.readFlags(entry("cfg.position_switches"), sysReply("CONFIG")), { SMART: false, FIXED: false, GPS: false });
  assert.equal(S.valuesCommand(entry("cfg.tel_d"), { secs: 900 }), "CONFIG TEL_D 900");
  assert.throws(() => S.valuesCommand(entry("cfg.ni"), { secs: 5 }), /entre 3600 y 86400/);
});

test("FREEZE: frozen reads on, unfrozen reads off (the word contains the other one)", () => {
  const e = entry("vault.freeze");
  assert.ok(Object.values(S.readFlags(e, sysReply("FREEZE"))).every((v) => v === false));
  const live = S.readFlags(e, "🔴 MESHTACORE FREEZE:\n  NodeDB:  frozen\n  Warm:    unfrozen\n  Device:  unfrozen\n  Config:  frozen");
  assert.deepEqual(live, { NODEDB: true, WARM: false, DEVICE: false, CONFIG: true });
  assert.equal(S.flagCommand(e, e.flags[0], true), "FREEZE NODEFREEZE ON");
});

test("RATELIMIT: seven buckets, each with its own command", () => {
  const e = entry("limits.rate");
  const { values } = S.readValues(e, sysReply("RATELIMIT"));
  assert.deepEqual(Object.values(values), [0, 0, 0, 0, 0, 0, 0]);
  assert.equal(S.fieldCommand(e.fields[3], "30"), "RATELIMIT NODEINFO 30");
  assert.throws(() => S.fieldCommand(e.fields[0], ""), /entre 0 y 10000/);
  assert.equal(S.readValues(entry("vault.bd"), sysReply("BD")).values.secs, 5);
});

test("read-only actions are marked, and LOAD (which throws away unsaved work) is the delicate one", () => {
  const acts = entry("vault.tools").actions;
  assert.ok(acts.filter((a) => a.readonly).every((a) => !a.risk));
  assert.equal(acts.find((a) => a.cmd === "LOAD").risk, "danger");
});

test("fill refuses an empty value, safeArg refuses a second command smuggled in", () => {
  assert.throws(() => S.fill("RDROP {value}", { value: "" }), /falta el valor/);
  assert.ok(S.safeArg("!abcd1234"));
  assert.ok(!S.safeArg("A4\nSAVE"));
  assert.ok(!S.safeArg("A4; SAVE"));
});
