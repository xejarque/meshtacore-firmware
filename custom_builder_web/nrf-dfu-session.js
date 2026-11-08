// Drives one nRF52 serial DFU: start -> erase -> init packet -> firmware -> stop -> activate.
// It talks to a `link`, so it runs unchanged on Web Serial (flasher-nrf.js) and on a simulated board in tests.
//
//   link.write(bytes)            sends bytes to the board
//   link.readFrame(timeoutMs)    next decoded frame from the board, or null if none arrives in time
//   link.close()                 releases the port

import {
  FRAMES_PER_PAGE,
  PAGE_WRITE_PAUSE_MS,
  activateWaitMs,
  ackNumber,
  dataMessages,
  eraseWaitMs,
  hciPacket,
  initMessage,
  nextSequence,
  startMessage,
  stopMessage,
} from "./nrf-dfu-core.js";

export const ACK_TIMEOUT_MS = 1000;
export const MAX_ATTEMPTS = 3;
const WAIT_STEP_MS = 250; // granularity of the progress while the bootloader erases or activates

const defaultSleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * @param {{bin: Uint8Array, dat: Uint8Array}} dfu the application image and its init packet
 * @param {{onStage?: (stage: string) => void, onProgress?: (stage: string, fraction: number) => void, sleep?: (ms: number) => Promise<void>}} hooks
 *   stages: "start", "erase", "init", "write", "activate"
 */
export async function runDfu(
  link,
  { bin, dat },
  { onStage = () => {}, onProgress = () => {}, sleep = defaultSleep } = {},
) {
  let sequence = 0;

  async function send(message, what) {
    sequence = nextSequence(sequence);
    const packet = hciPacket(sequence, message);
    // A lost packet or acknowledgement is retransmitted with the same sequence number: the board drops duplicates.
    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
      await link.write(packet);
      const frame = await link.readFrame(ACK_TIMEOUT_MS);
      if (frame && ackNumber(frame) === nextSequence(sequence)) return;
    }
    throw new Error(
      `La placa no responde (${what}). Comprueba que está en modo bootloader (doble pulsación de RESET) y vuelve a intentarlo.`,
    );
  }

  async function wait(totalMs, stage) {
    for (let waited = 0; waited < totalMs; waited += WAIT_STEP_MS) {
      await sleep(Math.min(WAIT_STEP_MS, totalMs - waited));
      onProgress(stage, Math.min(1, (waited + WAIT_STEP_MS) / totalMs));
    }
  }

  onStage("start");
  await send(startMessage(bin.length), "inicio");

  // The bootloader erases the flash for the whole image after the start message and ignores packets meanwhile.
  onStage("erase");
  await wait(eraseWaitMs(bin.length), "erase");

  onStage("init");
  await send(initMessage(dat), "paquete de inicialización");

  onStage("write");
  const frames = dataMessages(bin);
  for (const [index, message] of frames.entries()) {
    await send(message, `datos ${index + 1}/${frames.length}`);
    onProgress("write", (index + 1) / frames.length);
    // Every 8th frame fills a flash page and the bootloader's CPU is busy writing it.
    if (index % FRAMES_PER_PAGE === 0) await sleep(PAGE_WRITE_PAUSE_MS);
  }
  await sleep(PAGE_WRITE_PAUSE_MS);

  await send(stopMessage(), "fin de datos");

  // The bootloader now validates the image and copies it into place. Reopening the port meanwhile would
  // reset the board halfway and corrupt the flash, so release it and just wait.
  onStage("activate");
  await link.close();
  await wait(activateWaitMs(bin.length), "activate");
}
