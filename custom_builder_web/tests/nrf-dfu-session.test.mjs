// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { crc16, slipDecode } from "../nrf-dfu-core.js";
import { MAX_ATTEMPTS, runDfu } from "../nrf-dfu-session.js";

/** A board that decodes every packet like the real bootloader would, and can drop acknowledgements. */
function fakeBoard({ dropAcks = [] } = {}) {
  const board = {
    messages: [], // payloads of the packets it accepted, in order
    sequences: [], // the sequence number of every packet that reached it (retransmissions included)
    closedAt: null,
    writes: 0,
    pending: null,
    async write(packet) {
      board.writes++;
      assert.equal(packet[0], 0xc0);
      assert.equal(packet.at(-1), 0xc0);
      const body = slipDecode(packet.subarray(1, -1));
      const crc = body.at(-2) | (body.at(-1) << 8);
      assert.equal(
        crc,
        crc16(body.subarray(0, -2)),
        "the board rejects a packet with a bad CRC",
      );

      const sequence = body[0] & 0x07;
      board.sequences.push(sequence);
      if (board.sequences.at(-2) !== sequence)
        board.messages.push(body.subarray(4, -2)); // duplicates are dropped
      board.pending = dropAcks.includes(board.writes)
        ? null
        : Uint8Array.of(((sequence + 1) % 8) << 3, 0, 0, 0);
    },
    async readFrame() {
      const frame = board.pending;
      board.pending = null;
      return frame;
    },
    async close() {
      board.closedAt = board.messages.length;
    },
  };
  return board;
}

const noWait = async () => {};
const firmware = Uint8Array.from({ length: 1200 }, (_, i) => (i * 7) & 0xff); // 3 data messages
const dfu = { bin: firmware, dat: Uint8Array.of(0xaa, 0xbb) };
const type = (message) => message[0];

test("session: sends start, init, the whole firmware and stop, in that order", async () => {
  const board = fakeBoard();
  await runDfu(board, dfu, { sleep: noWait });

  assert.deepEqual(board.messages.map(type), [3, 1, 4, 4, 4, 5]);
  const received = board.messages
    .filter((m) => type(m) === 4)
    .flatMap((m) => [...m.subarray(4)]);
  assert.deepEqual(received, [...firmware]);
  assert.deepEqual(
    [...board.messages[1].subarray(4)],
    [0xaa, 0xbb, 0, 0],
    "init packet and padding",
  );
  assert.equal(
    board.messages[0].at(-4),
    1200 & 0xff,
    "the start message announces the firmware size",
  );
});

test("session: sequence numbers count 1..7 then wrap to 0, and the port is closed before the activation wait", async () => {
  const board = fakeBoard();
  const sleeps = [];
  await runDfu(board, dfu, { sleep: async (ms) => sleeps.push(ms) });

  assert.deepEqual(board.sequences, [1, 2, 3, 4, 5, 6]);
  const long = fakeBoard();
  await runDfu(
    long,
    { bin: new Uint8Array(512 * 9), dat: dfu.dat },
    { sleep: noWait },
  );
  assert.deepEqual(
    long.sequences.slice(5, 11),
    [6, 7, 0, 1, 2, 3],
    "wraps after 7",
  );

  assert.equal(
    board.closedAt,
    board.messages.length,
    "closed only after the stop message",
  );
  assert.ok(
    sleeps.at(-1) > 0,
    "and it still waits for the bootloader afterwards",
  );
});

test("session: reports the stages and a progress that reaches 1 in every one", async () => {
  const stages = [];
  const progress = new Map();
  await runDfu(fakeBoard(), dfu, {
    sleep: noWait,
    onStage: (s) => stages.push(s),
    onProgress: (stage, fraction) => progress.set(stage, fraction),
  });
  assert.deepEqual(stages, ["start", "erase", "init", "write", "activate"]);
  for (const stage of ["erase", "write", "activate"])
    assert.equal(progress.get(stage), 1, stage);
});

test("session: waits for the flash pages to be written after every 8th data frame", async () => {
  const sleeps = [];
  await runDfu(
    fakeBoard(),
    { bin: new Uint8Array(512 * 17), dat: dfu.dat },
    { sleep: async (ms) => sleeps.push(ms) },
  );
  const pageWrites = sleeps.filter((ms) => Math.abs(ms - 102.4) < 0.01);
  // frames 0, 8 and 16, plus the pause after the last one
  assert.equal(pageWrites.length, 4);
});

test("session: a lost acknowledgement is retransmitted with the same sequence number and nothing is duplicated", async () => {
  const board = fakeBoard({ dropAcks: [3] }); // the third packet written is not acknowledged
  await runDfu(board, dfu, { sleep: noWait });

  assert.deepEqual(
    board.sequences,
    [1, 2, 3, 3, 4, 5, 6],
    "sequence 3 was sent twice",
  );
  assert.deepEqual(
    board.messages.map(type),
    [3, 1, 4, 4, 4, 5],
    "the board processed it once",
  );
});

test("session: a board that never answers fails after a few attempts and says to check the bootloader", async () => {
  const board = fakeBoard({ dropAcks: [1, 2, 3, 4, 5] });
  await assert.rejects(
    runDfu(board, dfu, { sleep: noWait }),
    /no responde \(inicio\).*modo bootloader/,
  );
  assert.equal(board.writes, MAX_ATTEMPTS);
  assert.equal(board.closedAt, null, "it never reached the activation");
});

test("session: an acknowledgement for the wrong sequence is not accepted", async () => {
  const board = fakeBoard();
  const wrongAck = Uint8Array.of(5 << 3, 0, 0, 0);
  board.readFrame = async () => wrongAck;
  await assert.rejects(runDfu(board, dfu, { sleep: noWait }), /no responde/);
});
