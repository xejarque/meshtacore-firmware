// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { textPacketToRadio, wantConfig, readFields } from "../ble.js";

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

test("readFields stops on an unknown wire type instead of misparsing", () => {
  assert.deepEqual(readFields(new Uint8Array([0x0b, 1, 2, 3])), []); // field 1, wire type 3 (deprecated group)
});
