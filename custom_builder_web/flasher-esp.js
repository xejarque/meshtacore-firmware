// ESP32 flashing over Web Serial with esptool-js (vendored, Apache-2.0). Chrome/Edge on desktop only.
import { ESPLoader, Transport } from "./vendor/esptool-js.bundle.js";
import { combinedProgress } from "./flasher-core.js";
import { buildIdFromImage } from "./update-core.js";
import {
  PARTITION_TABLE_ADDRESS,
  PARTITION_TABLE_SIZE,
  chooseFlashMode,
  loaderToWrite,
  planAppUpdate,
  planOtaLoader,
} from "./esp-partitions.js";

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
  {
    extraImages = [],
    eraseAll = false,
    onErase = () => {},
    onProgress = () => {},
  } = {},
) {
  // Erased here instead of through writeFlash({eraseAll}) so the person can be told: a full chip erase
  // takes tens of seconds, during which esptool reports nothing and the bar would look frozen.
  if (eraseAll) {
    onErase();
    await session.loader.eraseFlash();
  }

  const images = [image, ...extraImages];
  const sizes = images.map(({ data }) => data.length);
  await session.loader.writeFlash({
    fileArray: images.map(({ data, address }) => ({ data, address })),
    flashSize: "keep",
    flashMode: "keep",
    flashFreq: "keep",
    eraseAll: false,
    compress: true,
    // esptool reports progress per file: weigh each by its size so the bar never goes back to 0
    reportProgress: (fileIndex, written, total) =>
      onProgress(
        combinedProgress(sizes, fileIndex, total ? written / total : 0),
      ),
  });
  await session.loader.after("hard_reset");
}

/**
 * Updates the application only: reads the board's own partition table, writes `data` into its app slot and
 * blanks the boot selector. The configuration (NVS), the OTA Loader slot and the filesystem are not touched,
 * unlike a .factory.bin, whose padding overwrites them.
 * @returns {{app: {offset:number, size:number, label:string}}} where it wrote
 */
export async function updateEspApp(
  session,
  { name, data },
  { onProgress, loader = null } = {},
) {
  const table = await readBoardTable(session);
  const plan = planAppUpdate(table, data.length);
  const extraImages = plan.otadata
    ? [
        {
          name: "otadata",
          address: plan.otadata.offset,
          data: new Uint8Array(plan.otadata.size).fill(0xff),
        },
      ]
    : [];
  // A board flashed with only a .factory.bin has an empty ota_1: without the loader, Bluetooth updates fail.
  const slot = loader ? planOtaLoader(table, loader.data.length) : null;
  const head = slot
    ? await session.loader.readFlash(slot.offset, 16)
    : new Uint8Array();
  const loaderImage = loaderToWrite(slot, head, loader);
  if (loaderImage) extraImages.push(loaderImage);
  await flashEsp(
    session,
    { name, data, address: plan.app.offset },
    { extraImages, onProgress },
  );
  return { ...plan, loaderWritten: Boolean(loaderImage) };
}

/** The first 0xc00 bytes of the partition table the board has now (blank flash reads as 0xFF). */
export const readBoardTable = (session) =>
  session.loader.readFlash(PARTITION_TABLE_ADDRESS, PARTITION_TABLE_SIZE);

/** What to do with this board for an image of `appSize` bytes: update it, or install from scratch. */
export async function planBoard(session, appSize) {
  return chooseFlashMode(await readBoardTable(session), appSize);
}

/**
 * Full install: erases the chip and writes the .factory.bin at 0x0 plus the OTA Loader at the ota_1 slot its own
 * partition table declares, which is what device-install.sh does. The configuration is lost.
 * @returns {{loaderWritten: boolean}}
 */
export async function installEsp(
  session,
  factory,
  { onProgress, onErase, loader = null } = {},
) {
  const table = factory.data.subarray(
    PARTITION_TABLE_ADDRESS,
    PARTITION_TABLE_ADDRESS + PARTITION_TABLE_SIZE,
  );
  const slot = loader ? planOtaLoader(table, loader.data.length) : null;
  const extraImages = slot
    ? [{ name: loader.name, data: loader.data, address: slot.offset }]
    : [];
  await flashEsp(
    session,
    { name: factory.name, data: factory.data, address: 0 },
    { extraImages, eraseAll: true, onErase, onProgress },
  );
  return { loaderWritten: extraImages.length > 0 };
}

/**
 * The build fingerprint ("MTCBUILD:<id>") of the firmware the board runs now, read from its app slot, or null when
 * the slot has none (older firmware, or the board boots another slot). Reads at most `maxBytes` of the slot.
 */
export async function readNodeBuildId(session, maxBytes) {
  const table = await session.loader.readFlash(
    PARTITION_TABLE_ADDRESS,
    PARTITION_TABLE_SIZE,
  );
  const { app } = planAppUpdate(table, 0);
  const bytes = await session.loader.readFlash(
    app.offset,
    Math.min(app.size, maxBytes),
  );
  return buildIdFromImage(bytes);
}

export async function closeEsp(session) {
  await session?.transport.disconnect().catch(() => {});
}
