// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as K from "../keys-core.js";

const fixture = JSON.parse(readFileSync(new URL("./fixtures/gat562-keys.json", import.meta.url)));
const reply = (cmd) => fixture.outputs.find((o) => o.command === cmd).response;
const HEX16 = "00112233445566778899aabbccddeeff";
const HEX32 = HEX16 + HEX16;

test("keys: hex and base64 of 16 or 32 bytes are accepted, anything else is refused", () => {
  const hex = K.parseKey(HEX16);
  assert.equal(hex.format, "hex");
  assert.equal(K.bytesToBase64(hex.bytes), "ABEiM0RVZneImaq7zN3u/w==");
  assert.deepEqual([...K.parseKey("ABEiM0RVZneImaq7zN3u/w==").bytes], [...hex.bytes]);
  assert.equal(K.parseKey("  " + HEX32.slice(0, 32) + "\n" + HEX32.slice(32)).bytes.length, 32);
  assert.ok(K.parseKey("1234").error);
  assert.ok(K.parseKey("").error);
  assert.ok(K.parseKey("AQ==").error, "the 1-byte default alias is not a key you can set from here");
  assert.ok(K.parseKey("zz" + HEX16).error);
});

test("keys: url-safe base64 is read too", () => {
  const std = K.bytesToBase64(K.parseKey(HEX32).bytes);
  assert.equal(K.parseKey(std.replace(/\+/g, "-").replace(/\//g, "_")).bytes.length, 32);
});

test("keys: generation uses the injected random source and refuses odd sizes", () => {
  const k = K.generateKey(32, (a) => a.fill(7));
  assert.equal(k.length, 32);
  assert.ok(k.every((b) => b === 7));
  assert.throws(() => K.generateKey(20), /tamaño/);
  assert.equal(K.generateKey(16).length, 16);
});

test("channels: CH and CHASH combine into one table", () => {
  const table = K.parseChannelTable(reply("CH"), reply("CHASH"));
  assert.equal(table.length, 8);
  assert.deepEqual(table[1], { index: 1, name: "Nexus", role: 2, hash: 84 });
  assert.deepEqual(table[0], { index: 0, name: "SFNarrow", role: 1, hash: 32 });
  assert.equal(table[7].name, "MC:#bot");
  assert.ok(K.isMeshCoreChannel(table[5].name));
  assert.ok(K.isTrustedChannel("Nexus") && K.isTrustedChannel("nexus2") && !K.isTrustedChannel("Nexus3"));
});

test("channels: names with spaces survive", () => {
  const t = K.parseChannelTable("🔴 MESHTACORE CHANNELS:\n0:Mi malla(1) 1:Otro(2) ", "🔴 MESHTACORE CHASH:\n0:Mi malla(0x20/32) 1:Otro(0x01/1) ");
  assert.equal(t[0].name, "Mi malla");
  assert.equal(t[0].hash, 32);
});

test("TRUSTED: slots with an empty name read as off", () => {
  assert.deepEqual(K.parseTrusted(reply("TRUSTED")), [
    { slot: 0, name: "", active: false },
    { slot: 1, name: "", active: false },
  ]);
  assert.equal(K.parseTrusted("🔴 MESHTACORE TRUSTED Slots:\n[0] Nexus (ON)\n[1]  (OFF)")[0].name, "Nexus");
});

test("channel commands: keys always travel as base64, U-variants only when asked", () => {
  assert.equal(K.channelKeyCommand(3, HEX16), "CHPSK 3 ABEiM0RVZneImaq7zN3u/w==");
  assert.equal(K.channelKeyCommand(1, HEX16, true), "UCHPSK 1 ABEiM0RVZneImaq7zN3u/w==");
  assert.equal(K.addChannelCommand("Ops"), "ADDCH Ops");
  assert.equal(K.addChannelCommand("Ops", HEX16), "ADDCH Ops ABEiM0RVZneImaq7zN3u/w==");
  assert.equal(K.renameCommand(2, "Mi malla"), "CHNAME 2 Mi malla");
  assert.equal(K.renameCommand(1, "Otro", true), "UCHNAME 1 Otro");
  assert.equal(K.deleteChannelCommand(4), "DELCH 4");
  assert.equal(K.deleteChannelCommand(1, true), "UDELCH 1");
});

test("channel commands: bad input is refused before anything is sent", () => {
  assert.throws(() => K.deleteChannelCommand(0), /principal/);
  assert.throws(() => K.addChannelCommand("con espacio"), /espacios/);
  assert.throws(() => K.addChannelCommand("DemasiadoLargoParaCanal"), /máximo 11/);
  assert.throws(() => K.addChannelCommand("Ok", "clave-mala"), /Clave no válida/);
  assert.throws(() => K.renameCommand(1, "a;SAVE"), /no permitidos/);
  assert.throws(() => K.channelKeyCommand(1, ""), /Falta la clave/);
});

test("admin keys: 32 bytes only, sent as 64 hex digits; wipe has its own command", () => {
  assert.equal(K.adminKeyCommand(2, HEX32), "SKEY 2 " + HEX32);
  assert.equal(K.adminKeyCommand(1, K.bytesToBase64(K.parseKey(HEX32).bytes)), "SKEY 1 " + HEX32);
  assert.throws(() => K.adminKeyCommand(1, HEX16), /Clave no válida/);
  assert.throws(() => K.adminKeyCommand(5, HEX32), /1 a 4/);
  assert.equal(K.wipeAdminKeyCommand(3), "SKEY 3 WIPE");
});

test("trusted slots and owner", () => {
  assert.equal(K.trustedCommand(0, "Nexus"), "TRUSTED 0 Nexus");
  assert.equal(K.trustedCommand(1, null), "TRUSTED 1 OFF");
  assert.throws(() => K.trustedCommand(2, "x"), /0 o 1/);
  assert.throws(() => K.trustedCommand(0, "off"), /Desactivar/);
  assert.equal(K.ownerCommand("BASE", "Estación Base"), "OWNER BASE Estación Base");
  assert.throws(() => K.ownerCommand("LARGO", "x"), /nombre corto/);
  assert.throws(() => K.ownerCommand("AB", ""), /nombre largo/);
});

test("MeshCore: the 32 hex digits become the base64 the Meshtastic app wants", () => {
  assert.equal(K.meshCoreKeyToBase64(HEX16), "ABEiM0RVZneImaq7zN3u/w==");
  assert.throws(() => K.meshCoreKeyToBase64("ABEiM0RVZneImaq7zN3u/w=="), /32 dígitos/);
});

test("share URL: a ChannelSet with the key and the name, base64url, no padding", () => {
  const url = K.channelUrl("Ops", K.parseKey(HEX16).bytes);
  assert.match(url, /^https:\/\/meshtastic\.org\/e\/\?add=true#[A-Za-z0-9_-]+$/);
  const b64 = url.split("#")[1].replace(/-/g, "+").replace(/_/g, "/");
  const bytes = [...atob(b64 + "=".repeat((4 - (b64.length % 4)) % 4))].map((c) => c.charCodeAt(0));
  // 0x0a len | 0x12 0x10 <16 key bytes> | 0x1a 0x03 "Ops"
  assert.deepEqual(bytes.slice(0, 4), [0x0a, 23, 0x12, 16]);
  assert.deepEqual(bytes.slice(4, 20), [...K.parseKey(HEX16).bytes]);
  assert.deepEqual(bytes.slice(20), [0x1a, 3, 79, 112, 115]);
});

test("redact: no key ever reaches a log or a recording", () => {
  assert.equal(K.redact("CHPSK 3 ABEiM0RVZneImaq7zN3u/w=="), "CHPSK 3 ***");
  assert.equal(K.redact("UCHPSK 1 secret"), "UCHPSK 1 ***");
  assert.equal(K.redact("ADDCH Ops secret"), "ADDCH Ops ***");
  assert.equal(K.redact("ADDCH Ops"), "ADDCH Ops");
  assert.equal(K.redact("SKEY 2 " + HEX32), "SKEY 2 ***");
  assert.equal(K.redact("SKEY 2 WIPE"), "SKEY 2 WIPE");
  assert.equal(K.redact("MESHCORE KEY " + HEX16), "MESHCORE KEY ***");
  assert.equal(K.redact("/mtcl CHPSK 3 secret"), "/mtcl CHPSK 3 ***", "the transport prefix does not hide it");
  assert.equal(K.redact("(DM) /mtc-device !1234abcd CHPSK 2 secret"), "(DM) /mtc-device !1234abcd CHPSK 2 ***", "remote commands too");
  assert.equal(K.redact("(canal 1) /mtc-device !1234abcd SKEY 1 " + HEX32), "(canal 1) /mtc-device !1234abcd SKEY 1 ***");
  assert.equal(K.redact("DROPS"), "DROPS");
});
