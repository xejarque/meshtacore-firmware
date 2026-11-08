// nRF52 flashing over Web Serial: the Adafruit bootloader speaks serial DFU on its USB CDC port.
// The protocol lives in nrf-dfu-core.js / nrf-dfu-session.js; this file only owns the browser serial port.
import { takeFrame } from "./nrf-dfu-core.js";
import { runDfu } from "./nrf-dfu-session.js";

const DFU_BAUDRATE = 115200;
const TOUCH_BAUDRATE = 1200; // opening the port at 1200 baud and closing it asks the firmware to reboot into the bootloader
const TOUCH_SETTLE_MS = 100;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const TIMEOUT = Symbol("timeout");

const concat = (a, b) => {
  const joined = new Uint8Array(a.length + b.length);
  joined.set(a);
  joined.set(b, a.length);
  return joined;
};

/**
 * Reboots a running node into the bootloader. The board then shows up as a different USB device,
 * so the person has to pick the port again (the browser asks for a click each time) before flashing.
 */
export async function touchBootloader() {
  const port = await navigator.serial.requestPort();
  await port.open({ baudRate: TOUCH_BAUDRATE });
  await sleep(TOUCH_SETTLE_MS);
  await port.close();
}

/** The `link` that runDfu expects, on top of an open Web Serial port. */
function serialLink(port) {
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();
  let buffer = new Uint8Array(0);
  let pending = null; // a read that timed out is kept and reused, so no byte is lost between calls
  let closed = false;

  return {
    write: (bytes) => writer.write(bytes),

    async readFrame(timeoutMs) {
      const deadline = performance.now() + timeoutMs;
      for (;;) {
        const taken = takeFrame(buffer);
        if (taken) {
          buffer = taken.rest;
          return taken.frame;
        }
        const remaining = deadline - performance.now();
        if (remaining <= 0) return null;
        pending ??= reader.read();
        const result = await Promise.race([
          pending,
          sleep(remaining).then(() => TIMEOUT),
        ]);
        if (result === TIMEOUT) return null;
        pending = null;
        if (result.done)
          throw new Error("El puerto se cerró de forma inesperada.");
        buffer = concat(buffer, result.value);
      }
    },

    async close() {
      if (closed) return;
      closed = true;
      await reader.cancel().catch(() => {});
      pending?.catch(() => {});
      reader.releaseLock();
      writer.releaseLock();
      await port.close().catch(() => {});
    },
  };
}

/**
 * Asks for the bootloader's port and flashes the application image of a DFU package.
 * @param {{bin: Uint8Array, dat: Uint8Array}} dfu
 * @param {Parameters<typeof runDfu>[2] & {onConnected?: () => void}} hooks
 */
export async function flashNrf(dfu, { onConnected = () => {}, ...hooks } = {}) {
  const port = await navigator.serial.requestPort();
  await port.open({ baudRate: DFU_BAUDRATE });
  onConnected();
  const link = serialLink(port);
  try {
    await runDfu(link, dfu, hooks);
  } finally {
    await link.close();
  }
}
