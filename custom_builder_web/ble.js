// Meshtastic BLE transport for the Meshtacore admin page.
// Web Bluetooth -> Meshtastic service -> local Nexus command (/mtcl) -> text replies.
// Protobuf is hand-encoded: only the handful of fields this page needs.
import { isNexusReply } from "./settings-core.js";

const SERVICE = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
const TO_RADIO = "f75c76d2-129e-4dad-a1dd-7866124401e7";
const FROM_RADIO = "2c55e69e-4993-11ed-b878-0242ac120002";
const FROM_NUM = "ed9da18c-a800-4f66-a670-aa7547e34453";
const LOCAL_ROOT = "/mtcl";
const PORTNUM_TEXT = 1;
const REPLY_QUIET_MS = 2500; // silence that ends a paged reply
const SINGLE_QUIET_MS = 700; // silence that ends a one-message reply

const enc = new TextEncoder();
const dec = new TextDecoder();

// ---------- minimal protobuf codec ----------
function varint(n) {
  const out = [];
  while (n > 0x7f) {
    out.push((n & 0x7f) | 0x80);
    n = Math.floor(n / 128);
  }
  out.push(n);
  return out;
}
const tag = (field, wire) => varint((field << 3) | wire);
const fVarint = (f, n) => [...tag(f, 0), ...varint(n)];
const fFixed32 = (f, n) => [...tag(f, 5), n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
const fBytes = (f, bytes) => [...tag(f, 2), ...varint(bytes.length), ...bytes];

export function textPacketToRadio(toNode, text, id) {
  const data = [...fVarint(1, PORTNUM_TEXT), ...fBytes(2, enc.encode(text))];
  const packet = [
    ...fFixed32(2, toNode), // to
    ...fVarint(3, 0), // channel
    ...fBytes(4, data), // decoded
    ...fFixed32(6, id), // id
  ];
  return new Uint8Array(fBytes(1, packet)); // ToRadio.packet
}

export const wantConfig = (id) => new Uint8Array(fVarint(3, id)); // ToRadio.want_config_id

export function readFields(buf) {
  const fields = [];
  let i = 0;
  const readVarint = () => {
    let v = 0,
      mul = 1,
      b;
    do {
      b = buf[i++];
      v += (b & 0x7f) * mul;
      mul *= 128;
    } while (b & 0x80);
    return v;
  };
  while (i < buf.length) {
    const key = readVarint();
    const field = key >> 3;
    const wire = key & 7;
    if (wire === 0) fields.push({ field, value: readVarint() });
    else if (wire === 2) {
      const len = readVarint();
      fields.push({ field, bytes: buf.subarray(i, i + len) });
      i += len;
    } else if (wire === 5) {
      fields.push({ field, value: (buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16) | (buf[i + 3] << 24)) >>> 0 });
      i += 4;
    } else if (wire === 1) i += 8;
    else break; // unknown wire type: stop rather than misparse
  }
  return fields;
}
const get = (fields, n) => fields.find((f) => f.field === n);

// ---------- session ----------
export class NodeLink {
  constructor() {
    this.device = null;
    this.toRadio = null;
    this.fromRadio = null;
    this.fromNum = null;
    this.myNode = 0;
    this.pending = null;
    this.draining = false;
    this.queue = Promise.resolve();
    this.listeners = {};
  }

  on(event, fn) {
    (this.listeners[event] ||= []).push(fn);
    return this;
  }

  emit(event, ...args) {
    (this.listeners[event] || []).forEach((fn) => fn(...args));
  }

  get connected() {
    return Boolean(this.device && this.device.gatt.connected && this.myNode);
  }

  get nodeId() {
    return this.myNode ? "!" + this.myNode.toString(16).padStart(8, "0") : "";
  }

  async connect() {
    if (!navigator.bluetooth) {
      throw new Error("Este navegador no soporta Web Bluetooth. Usa Chrome o Edge (escritorio/Android).");
    }
    this.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] });
    this.device.addEventListener("gattserverdisconnected", () => {
      this.myNode = 0;
      this.emit("disconnected");
    });
    const server = await this.device.gatt.connect();
    const svc = await server.getPrimaryService(SERVICE);
    this.toRadio = await svc.getCharacteristic(TO_RADIO);
    this.fromRadio = await svc.getCharacteristic(FROM_RADIO);
    this.fromNum = await svc.getCharacteristic(FROM_NUM);
    await this.fromNum.startNotifications();
    this.fromNum.addEventListener("characteristicvaluechanged", () => this.drain());
    await this.toRadio.writeValue(wantConfig(Math.floor(Math.random() * 0xffffff) + 1));
    await this.drain();
    this.emit("connected", this.device.name || "nodo", this.nodeId);
  }

  disconnect() {
    if (this.device && this.device.gatt.connected) this.device.gatt.disconnect();
  }

  async drain() {
    if (this.draining) return;
    this.draining = true;
    try {
      for (;;) {
        const value = await this.fromRadio.readValue();
        if (value.byteLength === 0) break;
        this.handleFromRadio(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      }
    } catch (e) {
      this.emit("error", "Error leyendo del nodo: " + e.message);
    } finally {
      this.draining = false;
    }
  }

  handleFromRadio(buf) {
    const fields = readFields(buf);
    const myInfo = get(fields, 3);
    if (myInfo && myInfo.bytes) {
      const num = get(readFields(myInfo.bytes), 1);
      if (num) this.myNode = num.value;
    }
    const pkt = get(fields, 2);
    if (!pkt || !pkt.bytes) return;
    const data = get(readFields(pkt.bytes), 4);
    if (!data || !data.bytes) return;
    const d = readFields(data.bytes);
    const port = get(d, 1);
    const payload = get(d, 2);
    if (port && port.value === PORTNUM_TEXT && payload && payload.bytes) this.onReply(dec.decode(payload.bytes));
  }

  onReply(text) {
    // The node forwards ordinary mesh chatter to the phone too; only Nexus replies belong to a command.
    if (!isNexusReply(text)) return;
    this.emit("rx", text);
    if (!this.pending) return;
    this.pending.chunks.push(text);
    // A paged reply keeps coming for a while; a single-message reply is done almost at once.
    if (/Paging/.test(text) || /^P\d+: /.test(text)) this.pending.paged = true;
    clearTimeout(this.pending.timer);
    this.pending.timer = setTimeout(this.pending.done, this.pending.paged ? REPLY_QUIET_MS : SINGLE_QUIET_MS);
  }

  // Queues one command; resolves with the text collected until the node goes quiet.
  // Commands never overlap, so the panel's polling cannot interleave with the console.
  run(command) {
    const job = this.queue.then(() => this.runNow(command));
    this.queue = job.catch(() => {});
    return job;
  }

  runNow(command) {
    if (!this.connected) return Promise.reject(new Error("Sin conexión"));
    const text = LOCAL_ROOT + " " + command.trim();
    this.emit("tx", text);
    return new Promise((resolve, reject) => {
      const done = () => {
        const out = this.pending ? this.pending.chunks.join("\n") : "";
        this.pending = null;
        resolve(out);
      };
      this.pending = { chunks: [], done, timer: setTimeout(done, REPLY_QUIET_MS * 2) };
      const id = (Math.random() * 0xffffffff) >>> 0;
      this.toRadio.writeValue(textPacketToRadio(this.myNode, text, id)).catch((e) => {
        clearTimeout(this.pending.timer);
        this.pending = null;
        reject(e);
      });
    });
  }
}
