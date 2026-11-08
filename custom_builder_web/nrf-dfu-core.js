// Pure logic of the nRF52 serial DFU used by the Adafruit bootloader (no DOM, no serial port).
//
// Independent implementation of the wire protocol that adafruit-nrfutil speaks over USB CDC
// ("dfu serial", Nordic's HCI-over-SLIP transport). The protocol facts (packet layout, CRC16,
// message codes, flash timings) come from Adafruit_nRF52_nrfutil, BSD-3-Clause, Nordic Semiconductor.
//
// One DFU message = 4-byte little-endian type + payload, wrapped as:
//   C0 | SLIP( header(4) | message | CRC16 LE ) | C0
// and the device answers every packet with an empty one whose header carries the next sequence number.

export const DFU_INIT_PACKET = 1;
export const DFU_START_PACKET = 3;
export const DFU_DATA_PACKET = 4;
export const DFU_STOP_DATA_PACKET = 5;
export const DFU_UPDATE_MODE_APP = 4;

export const DFU_PACKET_MAX_SIZE = 512; // bytes of firmware per data message
export const FRAME_DELIMITER = 0xc0;
const ESCAPE = 0xdb;
const ESCAPED_DELIMITER = 0xdc;
const ESCAPED_ESCAPE = 0xdd;

const HCI_PACKET_TYPE = 14;
const SEQUENCE_MODULO = 8;

// nRF52840 flash: erasing a page takes up to ~90 ms, writing a whole page ~100 ms.
const FLASH_PAGE_SIZE = 4096;
const FLASH_PAGE_ERASE_MS = 89.7;
const FLASH_PAGE_WRITE_MS = (FLASH_PAGE_SIZE / 4) * 0.1;
const MIN_ERASE_WAIT_MS = 500;

/** Every 8th data frame completes a flash page: the bootloader's CPU is busy writing it. */
export const FRAMES_PER_PAGE = FLASH_PAGE_SIZE / DFU_PACKET_MAX_SIZE;
export const PAGE_WRITE_PAUSE_MS = FLASH_PAGE_WRITE_MS;

// ---------- checksums and framing ----------
/** CRC-16/CCITT-FALSE as Nordic computes it (init 0xFFFF). */
export function crc16(data, init = 0xffff) {
  let crc = init;
  for (const byte of data) {
    crc = ((crc >> 8) & 0x00ff) | ((crc << 8) & 0xff00);
    crc ^= byte;
    crc ^= (crc & 0x00ff) >> 4;
    crc ^= (crc << 8) << 4;
    crc ^= ((crc & 0x00ff) << 4) << 1;
  }
  return crc & 0xffff;
}

/** Replaces 0xC0 with DB DC and 0xDB with DB DD so a delimiter never appears inside a frame. */
export function slipEncode(bytes) {
  const out = [];
  for (const byte of bytes) {
    if (byte === FRAME_DELIMITER) out.push(ESCAPE, ESCAPED_DELIMITER);
    else if (byte === ESCAPE) out.push(ESCAPE, ESCAPED_ESCAPE);
    else out.push(byte);
  }
  return Uint8Array.from(out);
}

export function slipDecode(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== ESCAPE) {
      out.push(bytes[i]);
      continue;
    }
    const next = bytes[++i];
    if (next === ESCAPED_DELIMITER) out.push(FRAME_DELIMITER);
    else if (next === ESCAPED_ESCAPE) out.push(ESCAPE);
    else
      throw new Error(
        "Respuesta corrupta de la placa (secuencia de escape inválida).",
      );
  }
  return Uint8Array.from(out);
}

/** The 4 header bytes: sequence, expected next sequence, CRC-present and reliable flags, type, length, checksum. */
export function hciHeader(sequence, payloadLength) {
  const b0 =
    sequence | (((sequence + 1) % SEQUENCE_MODULO) << 3) | (1 << 6) | (1 << 7);
  const b1 = HCI_PACKET_TYPE | ((payloadLength & 0x000f) << 4);
  const b2 = (payloadLength & 0x0ff0) >> 4;
  const b3 = (~(b0 + b1 + b2) + 1) & 0xff;
  return [b0, b1, b2, b3];
}

/** A complete packet ready to write to the port. */
export function hciPacket(sequence, payload) {
  const body = [...hciHeader(sequence, payload.length), ...payload];
  const crc = crc16(Uint8Array.from(body));
  body.push(crc & 0xff, crc >> 8);
  return Uint8Array.from([
    FRAME_DELIMITER,
    ...slipEncode(body),
    FRAME_DELIMITER,
  ]);
}

/** The sequence the next packet must use; the first one is 1. */
export const nextSequence = (sequence) => (sequence + 1) % SEQUENCE_MODULO;

/**
 * Takes the first complete frame out of what has been read so far.
 * @returns {{frame: Uint8Array, rest: Uint8Array} | null} null while the closing delimiter has not arrived
 */
export function takeFrame(buffer) {
  const start = buffer.indexOf(FRAME_DELIMITER);
  if (start < 0) return null;
  // Consecutive delimiters are the end of one frame and the start of the next: skip empty frames.
  let from = start + 1;
  while (buffer[from] === FRAME_DELIMITER) from++;
  const end = buffer.indexOf(FRAME_DELIMITER, from);
  if (end < 0) return null;
  return {
    frame: slipDecode(buffer.subarray(from, end)),
    rest: buffer.subarray(end + 1),
  };
}

/** The sequence number the device expects next, from the header of its acknowledgement. */
export function ackNumber(frame) {
  if (frame.length < 4)
    throw new Error("Respuesta de la placa demasiado corta.");
  return (frame[0] >> 3) & 0x07;
}

// ---------- DFU messages ----------
const u32 = (value) => [
  value & 0xff,
  (value >> 8) & 0xff,
  (value >> 16) & 0xff,
  (value >>> 24) & 0xff,
];

/** Announces an application-only update of `appSize` bytes (no SoftDevice, no bootloader). */
export const startMessage = (appSize) =>
  Uint8Array.from([
    ...u32(DFU_START_PACKET),
    ...u32(DFU_UPDATE_MODE_APP),
    ...u32(0),
    ...u32(0),
    ...u32(appSize),
  ]);

/** The .dat file of the package, followed by the two padding bytes the bootloader expects. */
export const initMessage = (dat) =>
  Uint8Array.from([...u32(DFU_INIT_PACKET), ...dat, 0, 0]);

export const stopMessage = () => Uint8Array.from(u32(DFU_STOP_DATA_PACKET));

/** The firmware cut into data messages of at most 512 bytes. */
export function dataMessages(bin) {
  const messages = [];
  for (let offset = 0; offset < bin.length; offset += DFU_PACKET_MAX_SIZE) {
    messages.push(
      Uint8Array.from([
        ...u32(DFU_DATA_PACKET),
        ...bin.subarray(offset, offset + DFU_PACKET_MAX_SIZE),
      ]),
    );
  }
  return messages;
}

const pagesOf = (size) => Math.floor(size / FLASH_PAGE_SIZE) + 1;

/** How long the bootloader needs to erase the flash for `size` bytes after the start message. */
export const eraseWaitMs = (size) =>
  Math.max(MIN_ERASE_WAIT_MS, pagesOf(size) * FLASH_PAGE_ERASE_MS);

/**
 * How long the dual-bank bootloader needs after the stop message to move the new image into place.
 * The port must stay closed meanwhile: reopening it resets the board and corrupts the flash.
 */
export const activateWaitMs = (size) =>
  eraseWaitMs(size) + pagesOf(size) * FLASH_PAGE_WRITE_MS;

// ---------- the .zip DFU package ----------
const baseName = (path) => path.split("/").pop();
const NOT_APPLICATION_ONLY = [
  "softdevice",
  "bootloader",
  "softdevice_bootloader",
];

/**
 * The application image (.bin) and its init packet (.dat) from the files of a DFU zip.
 * Packages that also carry a SoftDevice or bootloader are refused: this flasher only updates the app.
 */
export function readDfuPackage(entries) {
  const find = (name) => entries.find((e) => baseName(e.name) === name);
  const manifestFile = find("manifest.json");
  if (!manifestFile) throw new Error("El paquete DFU no tiene manifest.json.");

  let manifest;
  try {
    manifest = JSON.parse(new TextDecoder().decode(manifestFile.data)).manifest;
  } catch {
    throw new Error("El manifest.json del paquete DFU no es válido.");
  }
  if (NOT_APPLICATION_ONLY.some((key) => manifest?.[key])) {
    throw new Error(
      "El paquete incluye SoftDevice o bootloader: solo se puede flashear la aplicación.",
    );
  }
  const app = manifest?.application;
  const bin = app && find(app.bin_file);
  const dat = app && find(app.dat_file);
  if (!bin || !dat)
    throw new Error("El paquete DFU no incluye la aplicación (.bin y .dat).");
  return { bin: bin.data, dat: dat.data };
}
