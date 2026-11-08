// ESP32 flashing over Web Serial with esptool-js (vendored, Apache-2.0). Chrome/Edge on desktop only.
import { ESPLoader, Transport } from "./vendor/esptool-js.bundle.js";

const FLASH_BAUDRATE = 921600;

export const webSerialSupported = () =>
  typeof navigator !== "undefined" && "serial" in navigator;

/**
 * Asks the person to pick the port (needs a click), enters the ROM bootloader and identifies the chip.
 * @returns {{loader, transport, chip: string}} chip is esptool's description, e.g. "ESP32-S3 (QFN56) (revision v0.2)"
 */
export async function connectEsp({ onLog = () => {} } = {}) {
  const port = await navigator.serial.requestPort();
  const transport = new Transport(port, true);
  const terminal = { clean() {}, write: onLog, writeLine: onLog };
  const loader = new ESPLoader({
    transport,
    baudrate: FLASH_BAUDRATE,
    terminal,
  });
  try {
    const chip = await loader.main();
    return { loader, transport, chip };
  } catch (error) {
    await transport.disconnect().catch(() => {});
    throw error;
  }
}

/**
 * Writes `image.data` at `image.address` plus any `extraImages` (the OTA Loader), reports 0-1 progress
 * and reboots the board into the new firmware.
 */
export async function flashEsp(
  session,
  image,
  { extraImages = [], eraseAll = false, onProgress = () => {} } = {},
) {
  await session.loader.writeFlash({
    fileArray: [image, ...extraImages].map(({ data, address }) => ({
      data,
      address,
    })),
    flashSize: "keep",
    flashMode: "keep",
    flashFreq: "keep",
    eraseAll,
    compress: true,
    reportProgress: (_fileIndex, written, total) =>
      onProgress(total ? written / total : 0),
  });
  await session.loader.after("hard_reset");
}

export async function closeEsp(session) {
  await session?.transport.disconnect().catch(() => {});
}
