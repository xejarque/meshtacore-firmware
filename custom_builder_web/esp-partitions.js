// Reads an ESP32 partition table so an update can write only the application image and leave the
// configuration (NVS) and the OTA Loader slot alone. Pure functions: the flash read lives in flasher-esp.js.

export const PARTITION_TABLE_ADDRESS = 0x8000;
export const PARTITION_TABLE_SIZE = 0xc00; // 96 entries of 32 bytes

const ENTRY_SIZE = 32;
const ENTRY_MAGIC = [0xaa, 0x50];
const TYPE_APP = 0x00;
const TYPE_DATA = 0x01;
const SUBTYPE_APP_FACTORY = 0x00;
const SUBTYPE_APP_OTA_0 = 0x10;
const SUBTYPE_DATA_OTA = 0x00; // "otadata": which app slot boots

const readU32 = (bytes, at) =>
  (bytes[at] |
    (bytes[at + 1] << 8) |
    (bytes[at + 2] << 16) |
    (bytes[at + 3] << 24)) >>>
  0;

const readLabel = (bytes, at) => {
  const raw = bytes.subarray(at, at + 16);
  const end = raw.indexOf(0);
  return new TextDecoder().decode(end < 0 ? raw : raw.subarray(0, end));
};

/** @returns {{type:number, subtype:number, offset:number, size:number, label:string}[]} */
export function parsePartitionTable(bytes) {
  const entries = [];
  for (let at = 0; at + ENTRY_SIZE <= bytes.length; at += ENTRY_SIZE) {
    if (bytes[at] !== ENTRY_MAGIC[0] || bytes[at + 1] !== ENTRY_MAGIC[1]) break; // MD5 entry or blank flash
    entries.push({
      type: bytes[at + 2],
      subtype: bytes[at + 3],
      offset: readU32(bytes, at + 4),
      size: readU32(bytes, at + 8),
      label: readLabel(bytes, at + 12),
    });
  }
  return entries;
}

/**
 * Decides what an app-only update writes. The image goes to the factory or first OTA slot. The boot
 * selector (otadata) is blanked so the board starts that slot: after a Bluetooth/WiFi update it may point
 * at the OTA Loader's slot, and the new app would otherwise never run.
 * @returns {{app: {offset:number, size:number, label:string}, otadata: {offset:number, size:number}|null}}
 */
export function planAppUpdate(table, imageSize) {
  const entries = parsePartitionTable(table);
  if (entries.length === 0)
    throw new Error(
      "No se ha encontrado la tabla de particiones en la placa: no parece tener firmware de Meshtastic. Usa el flasher completo (con el .factory.bin).",
    );
  const app =
    entries.find(
      (e) => e.type === TYPE_APP && e.subtype === SUBTYPE_APP_FACTORY,
    ) ??
    entries.find((e) => e.type === TYPE_APP && e.subtype === SUBTYPE_APP_OTA_0);
  if (!app)
    throw new Error(
      "La tabla de particiones de la placa no tiene ranura de aplicación.",
    );
  if (imageSize > app.size)
    throw new Error(
      `El firmware (${imageSize} bytes) no cabe en la partición «${app.label}» de la placa (${app.size} bytes): su tabla de particiones es distinta. Usa el flasher completo.`,
    );
  const otadata =
    entries.find(
      (e) => e.type === TYPE_DATA && e.subtype === SUBTYPE_DATA_OTA,
    ) ?? null;
  return {
    app: { offset: app.offset, size: app.size, label: app.label },
    otadata: otadata && { offset: otadata.offset, size: otadata.size },
  };
}
