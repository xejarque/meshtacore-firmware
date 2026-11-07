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
// Over the mesh a reply can take many seconds (the firmware even adds up to BD seconds of jitter per page).
export const REMOTE_TIMING = { single: 10000, paged: 15000, overall: 60000 };
const BROADCAST = 0xffffffff;
export const remoteText = (nodeId, command) => `/mtc-device ${nodeId} ${command.trim()}`;

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

// `channel`: channel index to send on; `pki`: ask the node to encrypt a direct message with the
// recipient's public key (MeshPacket.pki_encrypted); `wantAck`: ask for a delivery acknowledgement.
export function textPacketToRadio(toNode, text, id, { channel = 0, pki = false, wantAck = false } = {}) {
  const data = [...fVarint(1, PORTNUM_TEXT), ...fBytes(2, enc.encode(text))];
  const packet = [
    ...fFixed32(2, toNode), // to
    ...fVarint(3, channel), // channel
    ...fBytes(4, data), // decoded
    ...fFixed32(6, id), // id
    ...(wantAck ? fVarint(10, 1) : []), // want_ack
    ...(pki ? fVarint(17, 1) : []), // pki_encrypted
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
    const packet = readFields(pkt.bytes);
    const from = get(packet, 1);
    const data = get(packet, 4);
    if (!data || !data.bytes) return;
    const d = readFields(data.bytes);
    const port = get(d, 1);
    const payload = get(d, 2);
    if (port && port.value === PORTNUM_TEXT && payload && payload.bytes) this.onReply(dec.decode(payload.bytes), from ? from.value : 0);
  }

  onReply(text, from = 0) {
    // The node forwards ordinary mesh chatter to the phone too; only Nexus replies belong to a command.
    if (!isNexusReply(text)) return;
    // A reply from the remote node being administered belongs to the remote job, never to a local one.
    if (this.remotePending && from === this.remotePending.target) return this.onRemoteReply(text);
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

  onRemoteReply(text) {
    const job = this.remotePending;
    this.emit("rx", "(remoto) " + text);
    job.chunks.push(text);
    if (/Paging/.test(text) || /^P\d+: /.test(text)) job.paged = true;
    clearTimeout(job.timer);
    job.timer = setTimeout(job.done, job.paged ? REMOTE_TIMING.paged : REMOTE_TIMING.single);
  }

  // Sends one command to another node and waits for ITS reply (matched by MeshPacket.from).
  //   route {via: "channel", channel: n}: broadcast on a Nexus channel, so the remote must share that channel's key.
  //   route {via: "dm"}: direct message, encrypted with the remote's public key.
  // Remote jobs have their own queue, so a slow mesh never stalls the local node's commands.
  runRemote(nodeId, command, route) {
    const job = (this.remoteQueue || Promise.resolve()).then(() => this.runRemoteNow(nodeId, command, route));
    this.remoteQueue = job.catch(() => {});
    return job;
  }

  runRemoteNow(nodeId, command, route) {
    if (!this.connected) return Promise.reject(new Error("Sin conexión"));
    const target = parseInt(nodeId.replace("!", ""), 16) >>> 0;
    if (!/^!?[0-9a-f]{8}$/i.test(nodeId) || !target) return Promise.reject(new Error("Identificador de nodo no válido"));
    const text = remoteText("!" + target.toString(16).padStart(8, "0"), command);
    const options = route.via === "dm" ? { pki: true, wantAck: true } : { channel: route.channel };
    const to = route.via === "dm" ? target : BROADCAST;
    this.emit("tx", `(${route.via === "dm" ? "DM" : "canal " + route.channel}) ${text}`);
    return new Promise((resolve, reject) => {
      const done = () => {
        const job = this.remotePending;
        if (job) (clearTimeout(job.guard), clearTimeout(job.timer));
        this.remotePending = null;
        resolve(job ? job.chunks.join("\n") : "");
      };
      this.remotePending = { target, chunks: [], done, timer: null, guard: setTimeout(done, REMOTE_TIMING.overall) };
      const id = (Math.random() * 0xffffffff) >>> 0;
      this.toRadio.writeValue(textPacketToRadio(to, text, id, options)).catch((e) => {
        clearTimeout(this.remotePending.guard);
        this.remotePending = null;
        reject(e);
      });
    });
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
