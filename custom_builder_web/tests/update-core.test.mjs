// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import {
  listBoards,
  parseFirmwareName,
  releaseVersion,
} from "../update-core.js";

test("name: board and version are split even when the board has dashes and digits", () => {
  assert.deepEqual(
    parseFirmwareName("firmware-heltec-v4-r8-oled-2.8.201.231dbc6.bin"),
    {
      name: "firmware-heltec-v4-r8-oled-2.8.201.231dbc6.bin",
      board: "heltec-v4-r8-oled",
      version: "2.8.201.231dbc6",
      kind: "esp32",
    },
  );
  assert.equal(
    parseFirmwareName("firmware-thinknode_m1-2.8.201.231dbc6.zip").kind,
    "nrf52",
  );
});

test("name: anything that is not a firmware image is refused", () => {
  assert.equal(parseFirmwareName("index.json"), null);
  assert.equal(parseFirmwareName("firmware-x.uf2"), null);
  assert.equal(parseFirmwareName("firmware-x-nover.bin"), null);
});

test("boards: the factory image is left out and the rest is sorted by board", () => {
  const index = {
    tag: "meshtacore-beta-2.8.201.231dbc6",
    files: [
      { name: "firmware-t-deck-2.8.201.231dbc6.bin", size: 3 },
      { name: "firmware-heltec-v3-2.8.201.231dbc6.factory.bin", size: 9 },
      { name: "firmware-heltec-v3-2.8.201.231dbc6.bin", size: 2 },
      { name: "firmware-rak4631-2.8.201.231dbc6.zip", size: 1 },
    ],
  };
  assert.deepEqual(
    listBoards(index).map((b) => [b.board, b.kind, b.size]),
    [
      ["heltec-v3", "esp32", 2],
      ["rak4631", "nrf52", 1],
      ["t-deck", "esp32", 3],
    ],
  );
  assert.equal(releaseVersion(index), "2.8.201.231dbc6");
});

test("boards: a missing or empty index gives an empty list", () => {
  assert.deepEqual(listBoards(null), []);
  assert.deepEqual(listBoards({ tag: null, files: [] }), []);
  assert.equal(releaseVersion({}), null);
});
