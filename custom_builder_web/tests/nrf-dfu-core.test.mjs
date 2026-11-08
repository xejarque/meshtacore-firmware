// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import * as D from "../nrf-dfu-core.js";

const text = (s) => new TextEncoder().encode(s);
const bytes = (...values) => Uint8Array.from(values);

/** Textbook bitwise CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF): an independent check of the byte-swap form. */
function referenceCrc16(data) {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit++)
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

test("crc16: the published check value and an independent bitwise implementation agree", () => {
  assert.equal(D.crc16(text("123456789")), 0x29b1);
  assert.equal(D.crc16(bytes()), 0xffff);
  const random = Uint8Array.from(
    { length: 700 },
    (_, i) => (i * 37 + 11) & 0xff,
  );
  assert.equal(D.crc16(random), referenceCrc16(random));
  assert.equal(
    D.crc16(text("6789"), D.crc16(text("12345"))),
    0x29b1,
    "it can be continued",
  );
});

test("slip: delimiters and escapes never appear raw inside a frame, and decoding restores the data", () => {
  const raw = bytes(0x01, 0xc0, 0xdb, 0xdc, 0xdd, 0xc0, 0x02);
  const encoded = D.slipEncode(raw);
  assert.deepEqual(
    [...encoded],
    // C0 -> DB DC and DB -> DB DD; the DC and DD already in the data are not special
    [0x01, 0xdb, 0xdc, 0xdb, 0xdd, 0xdc, 0xdd, 0xdb, 0xdc, 0x02],
  );
  assert.equal(encoded.includes(0xc0), false);
  assert.deepEqual([...D.slipDecode(encoded)], [...raw]);
  assert.throws(() => D.slipDecode(bytes(0xdb, 0x00)), /escape inválida/);
});

test("header: matches the formula of the Nordic SLIP header, with a zero byte sum", () => {
  // seq 1, 9 bytes: 1 | 2<<3 | CRC present 0x40 | reliable 0x80 = 0xD1; type 14 | 9<<4 = 0x9E; len>>4 = 0
  assert.deepEqual(D.hciHeader(1, 9), [0xd1, 0x9e, 0x00, 0x91]);
  // seq 7 expects 0 next (wraps); the 12-bit length 4095 spreads over bytes 1 and 2
  assert.deepEqual(D.hciHeader(7, 4095), [0xc7, 0xfe, 0xff, 0x3c]);
  for (const [seq, len] of [
    [1, 4],
    [3, 516],
    [7, 0],
    [0, 1000],
  ]) {
    assert.equal(D.hciHeader(seq, len).reduce((a, b) => a + b, 0) % 256, 0);
  }
});

test("packet: is delimited, decodes back to header + payload + a valid CRC, even with bytes that need escaping", () => {
  const payload = bytes(4, 0, 0, 0, 0xc0, 0xdb, 0x01, 0xc0);
  const packet = D.hciPacket(2, payload);
  assert.equal(packet[0], 0xc0);
  assert.equal(packet.at(-1), 0xc0);
  assert.equal(packet.subarray(1, -1).includes(0xc0), false);

  const body = D.slipDecode(packet.subarray(1, -1));
  assert.deepEqual([...body.subarray(0, 4)], D.hciHeader(2, payload.length));
  assert.deepEqual([...body.subarray(4, -2)], [...payload]);
  const crc = body.at(-2) | (body.at(-1) << 8);
  assert.equal(
    crc,
    referenceCrc16(body.subarray(0, -2)),
    "CRC covers header and payload, little endian",
  );
});

test("frames: the first complete frame is taken out and the rest is kept for the next read", () => {
  assert.equal(
    D.takeFrame(bytes(0xc0, 1, 2)),
    null,
    "no closing delimiter yet",
  );
  assert.equal(D.takeFrame(bytes(1, 2, 3)), null, "no delimiter at all");
  const first = D.takeFrame(
    bytes(7, 7, 0xc0, 0xd2, 0xdb, 0xdc, 5, 0xc0, 0xc0, 9),
  );
  assert.deepEqual(
    [...first.frame],
    [0xd2, 0xc0, 5],
    "noise before the delimiter is dropped and escapes decoded",
  );
  assert.deepEqual([...first.rest], [0xc0, 9]);
  assert.deepEqual(
    [...D.takeFrame(bytes(0xc0, 0xc0, 4, 5, 6, 7, 0xc0)).frame],
    [4, 5, 6, 7],
    "empty frames are skipped",
  );
});

test("ack: the number the device expects next sits in bits 3-5 of the first header byte", () => {
  assert.equal(D.ackNumber(bytes(0b0001_0001, 0, 0, 0)), 2);
  assert.equal(D.ackNumber(bytes(0xc7, 0, 0, 0)), 0);
  assert.throws(() => D.ackNumber(bytes(1, 2)), /demasiado corta/);
  assert.equal(D.nextSequence(7), 0);
  assert.equal(D.nextSequence(0), 1);
});

test("messages: start, init, data and stop carry the little-endian codes of the protocol", () => {
  assert.deepEqual(
    [...D.startMessage(0x0102_0304)],
    [3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 3, 2, 1],
  );
  assert.deepEqual(
    [...D.initMessage(bytes(9, 8))],
    [1, 0, 0, 0, 9, 8, 0, 0],
    "init packet plus 2 padding bytes",
  );
  assert.deepEqual([...D.stopMessage()], [5, 0, 0, 0]);

  const firmware = Uint8Array.from({ length: 1030 }, (_, i) => i & 0xff);
  const data = D.dataMessages(firmware);
  assert.deepEqual(
    data.map((m) => m.length - 4),
    [512, 512, 6],
  );
  assert.deepEqual([...data[0].subarray(0, 4)], [4, 0, 0, 0]);
  const joined = data.flatMap((m) => [...m.subarray(4)]);
  assert.deepEqual(joined, [...firmware], "no byte lost or reordered");
  assert.deepEqual(D.dataMessages(bytes()), []);
});

test("timing: erase waits at least half a second, activation also covers writing the pages", () => {
  assert.equal(D.eraseWaitMs(100), 500);
  // 800 KB app: 201 pages x 89.7 ms
  assert.ok(Math.abs(D.eraseWaitMs(800 * 1024) - 201 * 89.7) < 1);
  assert.ok(D.activateWaitMs(800 * 1024) > D.eraseWaitMs(800 * 1024) + 20_000);
  assert.equal(D.FRAMES_PER_PAGE, 8);
});

const manifest = (content) => ({
  name: "manifest.json",
  data: text(JSON.stringify({ manifest: content })),
});

test("package: reads the application image and init packet named by the manifest", () => {
  const entries = [
    manifest({
      application: { bin_file: "firmware.bin", dat_file: "firmware.dat" },
      dfu_version: 0.5,
    }),
    { name: "firmware.bin", data: bytes(1, 2, 3) },
    { name: "firmware.dat", data: bytes(9) },
  ];
  const pkg = D.readDfuPackage(entries);
  assert.deepEqual([...pkg.bin], [1, 2, 3]);
  assert.deepEqual([...pkg.dat], [9]);
});

test("package: refuses what it cannot flash safely", () => {
  assert.throws(
    () => D.readDfuPackage([{ name: "firmware.bin", data: bytes(1) }]),
    /manifest\.json/,
  );
  assert.throws(
    () =>
      D.readDfuPackage([{ name: "manifest.json", data: text("{not json") }]),
    /no es válido/,
  );
  assert.throws(
    () =>
      D.readDfuPackage([
        manifest({ application: { bin_file: "a.bin", dat_file: "a.dat" } }),
        { name: "a.bin", data: bytes(1) },
      ]),
    /\.bin y \.dat/,
  );
  const withBootloader = [
    manifest({
      application: { bin_file: "a.bin", dat_file: "a.dat" },
      bootloader: { bin_file: "b.bin", dat_file: "b.dat" },
    }),
    { name: "a.bin", data: bytes(1) },
    { name: "a.dat", data: bytes(1) },
  ];
  assert.throws(
    () => D.readDfuPackage(withBootloader),
    /SoftDevice o bootloader/,
  );
});
