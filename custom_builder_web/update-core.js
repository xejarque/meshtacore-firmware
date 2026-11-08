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
      return parsed ? { ...parsed, size: file.size } : null;
    })
    .filter(Boolean)
    .sort((a, b) => a.board.localeCompare(b.board));
}

/** The version shared by the files of a release, or null when there are none. */
export const releaseVersion = (index) => listBoards(index)[0]?.version ?? null;
