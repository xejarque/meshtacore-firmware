// What the "Actualizar" section offers from the firmware/index.json that the Pages deploy publishes
// (bin/pages-firmware.sh). Pure functions.

const NAME = /^firmware-(.+)-(\d+\.\d+\.\d+\.[0-9a-f]+)\.(bin|zip)$/i;

/** `firmware-heltec-v3-2.8.201.231dbc6.bin` → board, version and the kind of chip it runs on. */
export function parseFirmwareName(name) {
  const match = NAME.exec(name);
  if (!match) return null;
  const [, board, version, extension] = match;
  return {
    name,
    board,
    version,
    kind: extension.toLowerCase() === "bin" ? "esp32" : "nrf52",
  };
}

/**
 * The boards of a release, sorted by name. Anything that is not a firmware image (the .factory.bin, which
 * the name pattern does not match, or a malformed name) is left out.
 */
export function listBoards(index) {
  const files = Array.isArray(index?.files) ? index.files : [];
  return files
    .map((file) => {
      const parsed = parseFirmwareName(file.name);
      return parsed
        ? { ...parsed, size: file.size, buildId: file.buildId ?? null }
        : null;
    })
    .filter(Boolean)
    .sort((a, b) => a.board.localeCompare(b.board));
}

const MARKER = new TextEncoder().encode("MTCBUILD:");
const HEX = /^[0-9a-f]{8}$/;

/**
 * The build fingerprint stored in a firmware image as the text `MTCBUILD:<8 hex digits>` (see the BUILD command
 * of the firmware), or null for builds that predate it. The first well-formed marker wins.
 */
export function buildIdFromImage(bytes) {
  outer: for (let at = 0; at + MARKER.length + 8 <= bytes.length; at++) {
    for (let i = 0; i < MARKER.length; i++)
      if (bytes[at + i] !== MARKER[i]) continue outer;
    const id = String.fromCharCode(
      ...bytes.subarray(at + MARKER.length, at + MARKER.length + 8),
    );
    if (HEX.test(id)) return id;
  }
  return null;
}

/**
 * How a node's build compares with the standard release it would be updated to.
 * `same`: identical options; `different`: customised (or another target); `unknown`: one side has no id.
 */
export function compareBuilds(nodeId, releaseId) {
  if (!nodeId || !releaseId) return "unknown";
  return nodeId === releaseId ? "same" : "different";
}

/** The version shared by the files of a release, or null when there are none. */
export const releaseVersion = (index) => listBoards(index)[0]?.version ?? null;
