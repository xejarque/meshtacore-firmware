// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { buildIdFromImage, compareBuilds } from "../update-core.js";

const text = (s) => new TextEncoder().encode(s);

test("build id: read from the MTCBUILD marker inside the image", () => {
  const image = new Uint8Array([
    0xe9,
    0,
    0,
    ...text("junk MTCBUILD:zzzzzzzz MTCBUILD:a1b2c3d4\0more"),
  ]);
  assert.equal(
    buildIdFromImage(image),
    "a1b2c3d4",
    "a malformed marker is skipped",
  );
  assert.equal(buildIdFromImage(text("no marker here")), null);
  assert.equal(buildIdFromImage(text("MTCBUILD:a1b2c3")), null, "cut short");
  assert.equal(buildIdFromImage(new Uint8Array()), null);
});

test("build comparison: same options, customised, or not known", () => {
  assert.equal(compareBuilds("a1b2c3d4", "a1b2c3d4"), "same");
  assert.equal(compareBuilds("a1b2c3d4", "00000000"), "different");
  assert.equal(compareBuilds(null, "a1b2c3d4"), "unknown");
  assert.equal(compareBuilds("a1b2c3d4", null), "unknown");
});
