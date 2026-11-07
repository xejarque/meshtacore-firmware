// Meshtacore node admin - prototype.
// Web Bluetooth -> Meshtastic BLE service -> local Nexus command (/mtcl) -> text replies.
// Protobuf is hand-encoded: only the handful of fields this page needs.
(function () {
  "use strict";

  const SERVICE = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
  const TO_RADIO = "f75c76d2-129e-4dad-a1dd-7866124401e7";
  const FROM_RADIO = "2c55e69e-4993-11ed-b878-0242ac120002";
  const FROM_NUM = "ed9da18c-a800-4f66-a670-aa7547e34453";
  const LOCAL_ROOT = "/mtcl";
  const PORTNUM_TEXT = 1;
  const REPLY_QUIET_MS = 2500;

  // ---------- minimal protobuf codec ----------
  const enc = new TextEncoder();
  const dec = new TextDecoder();

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

  function textPacketToRadio(toNode, text, id) {
    const data = [...fVarint(1, PORTNUM_TEXT), ...fBytes(2, enc.encode(text))];
    const packet = [
      ...fFixed32(2, toNode), // to
      ...fVarint(3, 0), // channel
      ...fBytes(4, data), // decoded
      ...fFixed32(6, id), // id
    ];
    return new Uint8Array(fBytes(1, packet)); // ToRadio.packet
  }

  const wantConfig = (id) => new Uint8Array(fVarint(3, id)); // ToRadio.want_config_id

  function readFields(buf) {
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

  // ---------- BLE session ----------
  const state = { device: null, toRadio: null, fromRadio: null, fromNum: null, myNode: 0, pending: null, draining: false };
  const ui = {};

  function log(msg, cls) {
    const el = document.createElement("div");
    el.className = "log-line " + (cls || "");
    el.textContent = msg;
    ui.log.appendChild(el);
    ui.log.scrollTop = ui.log.scrollHeight;
  }

  async function drainFromRadio() {
    if (state.draining) return;
    state.draining = true;
    try {
      for (;;) {
        const value = await state.fromRadio.readValue();
        if (value.byteLength === 0) break;
        handleFromRadio(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      }
    } catch (e) {
      log("Error leyendo del nodo: " + e.message, "err");
    } finally {
      state.draining = false;
    }
  }

  function handleFromRadio(buf) {
    const fields = readFields(buf);
    const myInfo = get(fields, 3);
    if (myInfo && myInfo.bytes) {
      const num = get(readFields(myInfo.bytes), 1);
      if (num) {
        state.myNode = num.value;
        ui.node.textContent = "!" + num.value.toString(16).padStart(8, "0");
      }
    }
    const pkt = get(fields, 2);
    if (pkt && pkt.bytes) {
      const data = get(readFields(pkt.bytes), 4);
      if (!data || !data.bytes) return;
      const d = readFields(data.bytes);
      const port = get(d, 1);
      const payload = get(d, 2);
      if (port && port.value === PORTNUM_TEXT && payload && payload.bytes) onReply(dec.decode(payload.bytes));
    }
  }

  function onReply(text) {
    if (!state.pending) return log(text, "rx");
    state.pending.chunks.push(text);
    log(text, "rx");
    clearTimeout(state.pending.timer);
    state.pending.timer = setTimeout(state.pending.done, REPLY_QUIET_MS);
  }

  async function connect() {
    if (!navigator.bluetooth) {
      log("Este navegador no soporta Web Bluetooth. Usa Chrome o Edge (escritorio/Android).", "err");
      return;
    }
    try {
      state.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] });
      state.device.addEventListener("gattserverdisconnected", onDisconnect);
      const server = await state.device.gatt.connect();
      const svc = await server.getPrimaryService(SERVICE);
      state.toRadio = await svc.getCharacteristic(TO_RADIO);
      state.fromRadio = await svc.getCharacteristic(FROM_RADIO);
      state.fromNum = await svc.getCharacteristic(FROM_NUM);
      await state.fromNum.startNotifications();
      state.fromNum.addEventListener("characteristicvaluechanged", drainFromRadio);
      await state.toRadio.writeValue(wantConfig(Math.floor(Math.random() * 0xffffff) + 1));
      await drainFromRadio();
      ui.status.textContent = "Conectado: " + (state.device.name || "nodo");
      document.body.classList.add("connected");
      log("Conectado. Nodo " + ui.node.textContent, "ok");
    } catch (e) {
      log("No se pudo conectar: " + e.message, "err");
    }
  }

  function onDisconnect() {
    document.body.classList.remove("connected");
    ui.status.textContent = "Desconectado";
    log("Conexión Bluetooth cerrada.", "err");
  }

  function disconnect() {
    if (state.device && state.device.gatt.connected) state.device.gatt.disconnect();
  }

  // Sends one command and resolves with the text collected until the node goes quiet.
  function run(command) {
    if (!state.toRadio || !state.myNode) return Promise.reject(new Error("Sin conexión"));
    const text = LOCAL_ROOT + " " + command.trim();
    log("> " + text, "tx");
    return new Promise(async (resolve, reject) => {
      const done = () => {
        const out = state.pending ? state.pending.chunks.join("\n") : "";
        state.pending = null;
        resolve(out);
      };
      state.pending = { chunks: [], done, timer: setTimeout(done, REPLY_QUIET_MS * 2) };
      try {
        const id = (Math.random() * 0xffffffff) >>> 0;
        await state.toRadio.writeValue(textPacketToRadio(state.myNode, text, id));
      } catch (e) {
        clearTimeout(state.pending.timer);
        state.pending = null;
        reject(e);
      }
    });
  }

  // ---------- catalogue UI ----------
  let catalogue = [];
  const el = (tagName, props, ...kids) => {
    const n = Object.assign(document.createElement(tagName), props || {});
    kids.forEach((k) => n.append(k));
    return n;
  };

  function renderList(filter) {
    const q = (filter || "").toLowerCase();
    ui.list.replaceChildren();
    let lastCat = null;
    catalogue
      .filter((c) => !q || [c.name, ...c.aliases, c.summary, c.category].join(" ").toLowerCase().includes(q))
      .forEach((c) => {
        if (c.category !== lastCat) {
          lastCat = c.category;
          ui.list.append(el("h3", { className: "cat" }, lastCat));
        }
        ui.list.append(el("button", { className: "cmd-item", onclick: () => select(c) }, el("b", {}, c.name), " ", c.summary));
      });
  }

  function select(cmd) {
    ui.detail.replaceChildren(
      el("h2", {}, cmd.name),
      el("p", { className: "muted" }, [cmd.category, cmd.aliases.length ? "alias: " + cmd.aliases.join(", ") : ""].filter(Boolean).join(" · ")),
      el("p", {}, cmd.summary),
    );
    const usages = cmd.usage.length ? cmd.usage : [{ syntax: cmd.name, description: "" }];
    usages.forEach((u) => {
      const input = el("input", { type: "text", value: u.syntax, spellcheck: false });
      const out = el("pre", { className: "out" });
      const btn = el("button", { className: "run", textContent: "Ejecutar" });
      btn.onclick = async () => {
        btn.disabled = true;
        out.textContent = "Esperando respuesta…";
        try {
          out.textContent = (await run(input.value)) || "(sin respuesta)";
        } catch (e) {
          out.textContent = "Error: " + e.message;
        } finally {
          btn.disabled = false;
        }
      };
      ui.detail.append(el("div", { className: "usage" }, el("p", { className: "desc" }, u.description.replace(/`/g, "")), el("div", { className: "row" }, input, btn), out));
    });
  }

  async function init() {
    ["status", "node", "log", "list", "detail", "search"].forEach((id) => (ui[id] = document.getElementById(id)));
    document.getElementById("connect").onclick = connect;
    document.getElementById("disconnect").onclick = disconnect;
    ui.search.oninput = () => renderList(ui.search.value);
    try {
      catalogue = (await (await fetch("admin.commands.json")).json()).commands;
      renderList("");
    } catch (e) {
      log("No se pudo cargar el catálogo de comandos: " + e.message, "err");
    }
  }
  document.addEventListener("DOMContentLoaded", init);
})();
