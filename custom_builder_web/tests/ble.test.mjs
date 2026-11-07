// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { textPacketToRadio, wantConfig, readFields, NodeLink, REMOTE_TIMING } from "../ble.js";

const field = (fields, n) => fields.find((f) => f.field === n);

test("wantConfig encodes ToRadio.want_config_id (field 3, varint)", () => {
  assert.deepEqual([...wantConfig(5)], [0x18, 5]);
  const f = readFields(wantConfig(300));
  assert.equal(field(f, 3).value, 300);
});

test("a text packet round-trips: ToRadio.packet.decoded carries TEXT_MESSAGE_APP and the command", () => {
  const bytes = textPacketToRadio(0xdefc895b, "/mtcl INFO", 0x01020304);
  const packet = readFields(field(readFields(bytes), 1).bytes);
  assert.equal(field(packet, 2).value, 0xdefc895b); // to
  assert.equal(field(packet, 6).value, 0x01020304); // id
  const data = readFields(field(packet, 4).bytes);
  assert.equal(field(data, 1).value, 1); // portnum
  assert.equal(new TextDecoder().decode(field(data, 2).bytes), "/mtcl INFO");
});

test("multi-byte UTF-8 survives the payload", () => {
  const data = readFields(field(readFields(field(readFields(textPacketToRadio(1, "ñ✓", 1)), 1).bytes), 4).bytes);
  assert.equal(new TextDecoder().decode(field(data, 2).bytes), "ñ✓");
});

test("a text packet can go to a channel, or be a PKI direct message that asks for an ack", () => {
  const unpack = (bytes) => readFields(field(readFields(bytes), 1).bytes);
  const onChannel = unpack(textPacketToRadio(0xffffffff, "x", 1, { channel: 2 }));
  assert.equal(field(onChannel, 3).value, 2);
  assert.equal(field(onChannel, 10), undefined);
  assert.equal(field(onChannel, 17), undefined);
  const dm = unpack(textPacketToRadio(0x11223344, "x", 1, { pki: true, wantAck: true }));
  assert.equal(field(dm, 10).value, 1); // want_ack
  assert.equal(field(dm, 17).value, 1); // pki_encrypted
});

// A FromRadio carrying a text packet from `from`.
function fromRadioText(from, text) {
  const bytes = (n, b) => [(n << 3) | 2, b.length, ...b];
  const data = [8, 1, ...bytes(2, [...new TextEncoder().encode(text)])];
  const packet = [(1 << 3) | 5, from & 255, (from >>> 8) & 255, (from >>> 16) & 255, (from >>> 24) & 255, ...bytes(4, data)];
  return new Uint8Array(bytes(2, packet));
}

function connectedLink() {
  const link = new NodeLink();
  const sent = [];
  link.device = { gatt: { connected: true } };
  link.myNode = 0xaaaaaaaa;
  link.toRadio = { writeValue: async (b) => sent.push(b) };
  link.sent = sent;
  return link;
}

test("remote: the reply is matched by the sender, and chatter or other nodes' replies are ignored", async () => {
  Object.assign(REMOTE_TIMING, { single: 20, paged: 40, overall: 500 });
  const link = connectedLink();
  const job = link.runRemote("!1234abcd", "VERSION", { via: "channel", channel: 3 });
  await new Promise((r) => setTimeout(r, 5));
  link.handleFromRadio(fromRadioText(0xbbbbbbbb, "🔴 MESHTACORE: not mine")); // another node
  link.handleFromRadio(fromRadioText(0x1234abcd, "hola a todos")); // not a Nexus reply
  link.handleFromRadio(fromRadioText(0x1234abcd, "🔴 MESHTACORE: v2.8.201"));
  assert.equal(await job, "🔴 MESHTACORE: v2.8.201");
  const packet = readFields(field(readFields(link.sent[0]), 1).bytes);
  assert.equal(field(packet, 2).value, 0xffffffff); // broadcast
  assert.equal(field(packet, 3).value, 3); // the chosen channel
  const text = new TextDecoder().decode(field(readFields(field(packet, 4).bytes), 2).bytes);
  assert.equal(text, "/mtc-device !1234abcd VERSION");
});

test("remote: a DM goes to the node itself, and a silent node resolves empty after the overall timeout", async () => {
  Object.assign(REMOTE_TIMING, { single: 20, paged: 40, overall: 60 });
  const link = connectedLink();
  const reply = await link.runRemote("!1234abcd", "INFO", { via: "dm" });
  assert.equal(reply, "");
  const packet = readFields(field(readFields(link.sent[0]), 1).bytes);
  assert.equal(field(packet, 2).value, 0x1234abcd);
  assert.equal(field(packet, 17).value, 1);
});

test("remote: a malformed node id is refused before anything is sent, and paged replies are joined", async () => {
  Object.assign(REMOTE_TIMING, { single: 20, paged: 40, overall: 500 });
  const link = connectedLink();
  await assert.rejects(link.runRemote("!zzzz", "INFO", { via: "dm" }), /no válido/);
  assert.equal(link.sent.length, 0);
  const job = link.runRemote("!1234abcd", "NODES", { via: "dm" });
  await new Promise((r) => setTimeout(r, 5));
  link.handleFromRadio(fromRadioText(0x1234abcd, "🔴 MESHTACORE: Paging x..."));
  link.handleFromRadio(fromRadioText(0x1234abcd, "P1: abc"));
  assert.equal(await job, "🔴 MESHTACORE: Paging x...\nP1: abc");
});

test("a local command still gets the local node's reply while a remote job is waiting", async () => {
  Object.assign(REMOTE_TIMING, { single: 200, paged: 200, overall: 400 });
  const link = connectedLink();
  const remote = link.runRemote("!1234abcd", "INFO", { via: "dm" });
  const local = link.run("DROPS");
  await new Promise((r) => setTimeout(r, 10));
  link.handleFromRadio(fromRadioText(0xaaaaaaaa, "🔴 MESHTACORE ACTIVE DROPS:\nTR"));
  link.handleFromRadio(fromRadioText(0x1234abcd, "🔴 MESHTACORE: remote info"));
  assert.equal(await local, "🔴 MESHTACORE ACTIVE DROPS:\nTR");
  assert.equal(await remote, "🔴 MESHTACORE: remote info");
});

test("readFields stops on an unknown wire type instead of misparsing", () => {
  assert.deepEqual(readFields(new Uint8Array([0x0b, 1, 2, 3])), []); // field 1, wire type 3 (deprecated group)
});
