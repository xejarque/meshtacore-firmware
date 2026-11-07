// Keys and channels panel: channel list with rename / change key / share / delete, new channels (including
// MeshCore ones), admin keys, trusted channel slots, node identity and a key toolbox.
// `run(command)` is injected (BLE or demo). Secrets stay in the browser and travel only to the node.
import * as K from "./keys-core.js";
import { cleanReply } from "./settings-core.js";

const h = (tag, props = {}, ...kids) => {
  const n = Object.assign(document.createElement(tag), props);
  kids.forEach((k) => n.append(k));
  return n;
};
const FAILED = /error|invalid|missing|protected|no free|usage|not found/i;

// A modal question. With `expect`, the confirm button stays off until that exact text is typed.
function modal(title, body, { confirm = "Aceptar", danger = false, expect = null, cancel = "Cancelar" } = {}) {
  return new Promise((resolve) => {
    const dlg = h("dialog", { className: "kx-dialog" });
    const ok = h("button", { className: danger ? "sx-clear" : "sx-add", textContent: confirm, disabled: expect !== null });
    const no = h("button", { textContent: cancel });
    const parts = [h("h3", {}, title), body];
    if (expect !== null) {
      const typed = h("input", { type: "text", placeholder: expect, spellcheck: false, autocomplete: "off" });
      typed.oninput = () => (ok.disabled = typed.value !== expect);
      parts.push(h("p", { className: "kx-typed" }, "Para confirmar escribe ", h("code", {}, expect), ":"), typed);
    }
    const done = (v) => (dlg.close(), dlg.remove(), resolve(v));
    ok.onclick = () => done(true);
    no.onclick = () => done(false);
    dlg.oncancel = () => done(false);
    dlg.append(...parts, h("div", { className: "kx-actions" }, ...(cancel ? [no] : []), ok));
    document.body.append(dlg);
    dlg.showModal();
  });
}

const warn = (text) => h("p", { className: "kx-warn" }, text);
const copy = (text, btn) => navigator.clipboard.writeText(text).then(() => ((btn.textContent = "Copiado ✓"), setTimeout(() => (btn.textContent = "Copiar"), 1500)));

function copyRow(label, value) {
  const btn = h("button", { textContent: "Copiar" });
  btn.onclick = () => copy(value, btn);
  return h("div", { className: "kx-copy" }, h("small", {}, label), h("code", {}, value), btn);
}

function qrBlock(url) {
  const box = h("div", { className: "kx-qr" });
  if (window.qrcode) {
    const qr = window.qrcode(0, "M");
    qr.addData(url);
    qr.make();
    box.innerHTML = qr.createSvgTag(4, 4);
  }
  return box;
}

export class KeysPanel {
  constructor({ root, run, isConnected }) {
    this.root = root;
    this.run = run;
    this.isConnected = isConnected;
    this.channels = null; // [{index,name,role,hash}] | null while unknown
    this.trusted = null;
    this.notes = new Map();
    this.dirty = false;
    this.busy = false;
  }

  reset() {
    this.channels = null;
    this.trusted = null;
    this.render();
  }

  note(id, reply) {
    const text = cleanReply(reply);
    this.notes.set(id, { text, ok: !FAILED.test(text) });
  }

  // Runs a write, keeps its answer in the card's note, refreshes what depends on it.
  async exec(cardId, command, { persistsNow = false, reload = false } = {}) {
    this.busy = true;
    this.render();
    let reply = "";
    try {
      reply = await this.run(command);
      this.note(cardId, reply);
      if (!persistsNow && this.notes.get(cardId).ok) this.dirty = true;
    } catch (e) {
      this.notes.set(cardId, { text: "Error: " + e.message, ok: false });
    }
    if (reload) {
      this.channels = null;
      this.trusted = null;
    }
    this.busy = false;
    this.render();
    return !FAILED.test(reply);
  }

  async load() {
    if ((this.channels !== null && this.trusted !== null) || !this.isConnected() || this.loading) return;
    this.loading = true;
    try {
      if (this.channels === null) this.channels = K.parseChannelTable(await this.run("CH"), await this.run("CHASH"));
      if (this.trusted === null) this.trusted = K.parseTrusted(await this.run("TRUSTED"));
    } catch (e) {
      this.channels = this.channels || [];
      this.trusted = this.trusted || [];
      this.notes.set("channels", { text: "No se pudieron leer los canales: " + e.message, ok: false });
    }
    this.loading = false;
    this.render();
  }

  // ---------- key input with generator ----------
  keyField({ placeholder = "Clave en base64 o hex (16 o 32 bytes)", sizes = K.KEY_SIZES, generate = true } = {}) {
    const input = h("input", { type: "text", className: "kx-key", placeholder, spellcheck: false, autocomplete: "off" });
    const hint = h("small", { className: "kx-hint" });
    const refresh = () => {
      if (!input.value.trim()) return (hint.textContent = "");
      const k = K.parseKey(input.value, sizes);
      hint.textContent = k.error ? k.error : `✓ ${k.bytes.length * 8} bits (${k.format}) → base64: ${K.bytesToBase64(k.bytes)}`;
      hint.className = "kx-hint " + (k.error ? "bad" : "good");
    };
    input.oninput = refresh;
    const row = h("div", { className: "kx-keyrow" }, input);
    if (generate) {
      for (const size of K.KEY_SIZES.filter((s) => sizes.includes(s))) {
        const b = h("button", { type: "button", textContent: `Generar ${size * 8} bits` });
        b.onclick = () => ((input.value = K.bytesToBase64(K.generateKey(size))), refresh());
        row.append(b);
      }
    }
    return { input, hint, el: h("div", {}, row, hint) };
  }

  // After a key was set: the only moment the user can copy it, so show it with its share URL and QR.
  async showKeyResult(name, keyText) {
    const k = K.parseKey(keyText);
    if (k.error) return;
    const url = K.channelUrl(name, k.bytes);
    await modal(
      `Clave de «${name}»`,
      h(
        "div",
        {},
        warn("Guárdala ahora: el nodo no permite volver a leerla. Para unir otros nodos a este canal necesitan esta clave."),
        copyRow("Base64", K.bytesToBase64(k.bytes)),
        copyRow("Hex", K.bytesToHex(k.bytes)),
        copyRow("Enlace del canal", url),
        qrBlock(url),
      ),
      { confirm: "Cerrar", cancel: "" },
    );
  }

  // ---------- channel actions ----------
  protectedNote(ch) {
    return K.isTrustedChannel(ch.name) ? warn(`«${ch.name}» es un canal de confianza: es el que autoriza órdenes /mtc. Cambiarlo puede dejarte sin administración remota.`) : "";
  }

  async renameChannel(ch) {
    const input = h("input", { type: "text", value: ch.name, maxLength: K.CHANNEL_NAME_MAX });
    const ok = await modal(`Renombrar canal ${ch.index}`, h("div", {}, this.protectedNote(ch), h("p", {}, `Hasta ${K.CHANNEL_NAME_MAX} caracteres.`), input), { confirm: "Renombrar", danger: K.isTrustedChannel(ch.name), expect: K.isTrustedChannel(ch.name) ? ch.name : null });
    if (!ok) return;
    try {
      await this.exec("channels", K.renameCommand(ch.index, input.value, K.isTrustedChannel(ch.name)), { persistsNow: true, reload: true });
    } catch (e) {
      this.notes.set("channels", { text: e.message, ok: false });
      this.render();
    }
  }

  async changeKey(ch) {
    const field = this.keyField();
    const body = h("div", {}, this.protectedNote(ch), warn("Los nodos que tengan la clave anterior dejarán de entender este canal y quedarás aislado de ellos hasta que también la cambien. Si estás conectado por radio a un nodo remoto por este canal, perderás el acceso."), field.el);
    if (!(await modal(`Cambiar la clave de «${ch.name}»`, body, { confirm: "Cambiar clave", danger: true, expect: ch.name }))) return;
    try {
      const command = K.channelKeyCommand(ch.index, field.input.value, K.isTrustedChannel(ch.name));
      if (await this.exec("channels", command, { persistsNow: true })) await this.showKeyResult(ch.name, field.input.value);
    } catch (e) {
      this.notes.set("channels", { text: e.message, ok: false });
      this.render();
    }
  }

  async shareChannel(ch) {
    const field = this.keyField({ placeholder: "La clave actual del canal (base64 o hex)", generate: false });
    const body = h("div", {}, h("p", {}, "El nodo no permite leer la clave de un canal, así que pega aquí la que usas para generar el enlace y el QR. Nada se envía al nodo."), field.el);
    if (!(await modal(`Compartir «${ch.name}»`, body, { confirm: "Generar enlace" }))) return;
    const k = K.parseKey(field.input.value);
    if (k.error) {
      this.notes.set("channels", { text: k.error, ok: false });
      return this.render();
    }
    await this.showKeyResult(ch.name, field.input.value);
  }

  async deleteChannel(ch) {
    const body = h("div", {}, this.protectedNote(ch), h("p", {}, "El canal se desactiva y deja de enviar y recibir. Su posición queda libre para otro canal."));
    if (!(await modal(`Borrar el canal «${ch.name}»`, body, { confirm: "Borrar canal", danger: true, expect: ch.name }))) return;
    try {
      await this.exec("channels", K.deleteChannelCommand(ch.index, K.isTrustedChannel(ch.name)), { persistsNow: true, reload: true });
    } catch (e) {
      this.notes.set("channels", { text: e.message, ok: false });
      this.render();
    }
  }

  async addChannel() {
    const kind = h("select", {}, ...[["normal", "Canal normal"], ["mc-public", "MeshCore: Public"], ["mc-hashtag", "MeshCore: #sala (sin clave)"], ["mc-private", "MeshCore: privado (con la clave de MeshCore)"]].map(([v, t]) => h("option", { value: v, textContent: t })));
    const name = h("input", { type: "text", placeholder: "Nombre", maxLength: K.CHANNEL_NAME_MAX });
    const holder = h("div", {});
    const info = h("p", { className: "kx-hint" });
    let field = null;
    const update = () => {
      holder.replaceChildren();
      field = null;
      name.disabled = kind.value === "mc-public";
      name.value = kind.value === "mc-public" ? "MC:Public" : name.value;
      info.textContent =
        {
          normal: "Si dejas la clave vacía se usa la clave por defecto de Meshtastic, que es pública. Genera una propia para un canal privado.",
          "mc-public": "Canal público de MeshCore. No lleva clave.",
          "mc-hashtag": "Escribe el nombre sin «MC:#»; el nodo deduce la clave del nombre, igual que MeshCore.",
          "mc-private": "Escribe el nombre (sin «MC:») y los 32 dígitos hex que MeshCore muestra como clave.",
        }[kind.value];
      if (kind.value === "normal") holder.append((field = this.keyField()).el);
      if (kind.value === "mc-private") holder.append((field = this.keyField({ placeholder: "32 dígitos hex de MeshCore", sizes: [16], generate: false })).el);
    };
    kind.onchange = update;
    update();
    const body = h("div", {}, h("label", {}, "Tipo ", kind), name, info, holder);
    if (!(await modal("Añadir un canal", body, { confirm: "Añadir" }))) return;
    try {
      const full = { normal: name.value.trim(), "mc-public": "MC:Public", "mc-hashtag": "MC:#" + name.value.trim().replace(/^#/, ""), "mc-private": "MC:" + name.value.trim() }[kind.value];
      const keyText = field && field.input.value.trim() ? field.input.value : null;
      const ok = await this.exec("channels", K.addChannelCommand(full, keyText), { persistsNow: true, reload: true });
      if (ok && keyText && kind.value === "normal") await this.showKeyResult(full, keyText);
    } catch (e) {
      this.notes.set("channels", { text: e.message, ok: false });
      this.render();
    }
  }

  // ---------- cards ----------
  noteEl(id) {
    const n = this.notes.get(id);
    return n ? h("p", { className: "sx-note " + (n.ok ? "ok" : "err") }, n.text) : "";
  }

  channelsCard() {
    const card = h("section", { className: "sx-card" }, h("h3", {}, "Canales"), h("p", { className: "sx-help" }, "Los canales cifran lo que se envía por radio: solo los nodos con la misma clave se entienden. Los cambios de canal se guardan en el nodo al momento."));
    if (!this.isConnected()) return card.append(h("p", { className: "muted" }, "Conecta el nodo para ver sus canales.")), card;
    if (this.channels === null) return card.append(h("p", { className: "muted" }, "Leyendo canales…")), card;
    const table = h("table", { className: "kx-table" }, h("thead", {}, h("tr", {}, ...["#", "Nombre", "Rol", "Hash", ""].map((t) => h("th", {}, t)))));
    const body = h("tbody");
    for (const ch of this.channels) {
      const btn = (text, fn, danger) => {
        const b = h("button", { className: danger ? "sx-clear" : "", textContent: text, disabled: this.busy });
        b.onclick = fn;
        return b;
      };
      const tags = [K.isTrustedChannel(ch.name) ? h("span", { className: "sx-badge" }, "confianza") : "", K.isMeshCoreChannel(ch.name) ? h("span", { className: "sx-badge mc" }, "MeshCore") : ""];
      body.append(
        h("tr", {}, h("td", {}, String(ch.index)), h("td", {}, ch.name, " ", ...tags), h("td", {}, ch.role === 1 ? "principal" : "secundario"), h("td", {}, ch.hash === null ? "–" : String(ch.hash)), h("td", { className: "kx-rowbtns" }, btn("Renombrar", () => this.renameChannel(ch)), btn("Clave", () => this.changeKey(ch)), btn("Compartir", () => this.shareChannel(ch)), ch.index === 0 ? "" : btn("Borrar", () => this.deleteChannel(ch), true))),
      );
    }
    table.append(body);
    const add = h("button", { className: "sx-add", textContent: "Añadir canal", disabled: this.busy });
    add.onclick = () => this.addChannel();
    card.append(table, h("div", { className: "sx-addrow" }, add), this.noteEl("channels"));
    return card;
  }

  adminKeysCard() {
    const card = h("section", { className: "sx-card" }, h("h3", {}, "Claves de administración ", h("span", { className: "sx-badge" }, "delicado")), h("p", { className: "sx-help" }, "Cuatro huecos para claves públicas de administradores (32 bytes). Quien tenga la clave privada correspondiente puede administrar tu nodo por radio con firma criptográfica. El nodo no permite leer qué hay en cada hueco, así que solo se puede poner o borrar. Hay que pulsar Guardar para que persista."));
    for (let slot = 1; slot <= 4; slot++) {
      const field = this.keyField({ placeholder: `Clave pública del administrador ${slot} (base64 o hex, 32 bytes)`, sizes: [32], generate: false });
      const set = h("button", { className: "sx-add", textContent: "Poner", disabled: this.busy || !this.isConnected() });
      const wipe = h("button", { className: "sx-clear", textContent: "Borrar", disabled: this.busy || !this.isConnected() });
      set.onclick = async () => {
        try {
          const command = K.adminKeyCommand(slot, field.input.value);
          const body = h("div", {}, warn("Esta clave podrá administrar tu nodo por radio: cambiar ajustes, claves y reiniciarlo. Asegúrate de que es la clave pública de alguien de confianza."));
          if (await modal(`Poner clave de administración ${slot}`, body, { confirm: "Poner clave", danger: true, expect: `SKEY ${slot}` })) await this.exec("skey", command);
        } catch (e) {
          this.notes.set("skey", { text: e.message, ok: false });
          this.render();
        }
      };
      wipe.onclick = async () => {
        if (await modal(`Borrar clave de administración ${slot}`, h("p", {}, "El hueco queda vacío. Si ese administrador era el único, perderás la administración remota por firma."), { confirm: "Borrar", danger: true })) await this.exec("skey", K.wipeAdminKeyCommand(slot));
      };
      card.append(h("div", { className: "kx-slot" }, h("b", {}, `Hueco ${slot}`), field.el, h("div", { className: "kx-slotbtns" }, set, wipe)));
    }
    card.append(this.noteEl("skey"));
    return card;
  }

  trustedCard() {
    const card = h("section", { className: "sx-card" }, h("h3", {}, "Canales de confianza"), h("p", { className: "sx-help" }, "Nombres de los canales por los que el nodo acepta órdenes /mtc de la malla (hasta dos). Es el equivalente al canal «Nexus». Hay que pulsar Guardar para que persista."));
    if (!this.trusted) return card.append(h("p", { className: "muted" }, "Conecta el nodo para ver los huecos.")), card;
    for (const t of this.trusted) {
      const input = h("input", { type: "text", value: t.name, maxLength: 15, placeholder: "Nombre del canal" });
      const set = h("button", { className: "sx-add", textContent: "Poner", disabled: this.busy });
      const off = h("button", { className: "sx-clear", textContent: "Desactivar", disabled: this.busy || !t.active });
      set.onclick = async () => {
        try {
          await this.exec("trusted", K.trustedCommand(t.slot, input.value));
        } catch (e) {
          this.notes.set("trusted", { text: e.message, ok: false });
          this.render();
        }
        this.trusted = null;
        this.load();
      };
      off.onclick = async () => {
        await this.exec("trusted", K.trustedCommand(t.slot, null));
        this.trusted = null;
        this.load();
      };
      card.append(h("div", { className: "kx-slot" }, h("b", {}, `Hueco ${t.slot} `, h("span", { className: t.active ? "kx-on" : "kx-off" }, t.active ? "activo" : "apagado")), h("div", { className: "sx-addrow" }, input, set, off)));
    }
    card.append(this.noteEl("trusted"));
    return card;
  }

  identityCard() {
    const short = h("input", { type: "text", maxLength: K.OWNER_SHORT_MAX, placeholder: "Corto (4)" });
    const long = h("input", { type: "text", maxLength: K.OWNER_LONG_MAX, placeholder: "Nombre largo" });
    const apply = h("button", { className: "sx-clear", textContent: "Cambiar nombre", disabled: this.busy || !this.isConnected() });
    apply.onclick = async () => {
      try {
        const command = K.ownerCommand(short.value, long.value);
        const body = h("div", {}, warn("Cambiar el nombre asigna un número de nodo nuevo: el resto de la malla verá un nodo distinto y se pierden los favoritos y listas que apuntaban al anterior."));
        if (await modal("Cambiar la identidad del nodo", body, { confirm: "Cambiar", danger: true, expect: short.value.trim() })) await this.exec("owner", command, { persistsNow: true });
      } catch (e) {
        this.notes.set("owner", { text: e.message, ok: false });
        this.render();
      }
    };
    return h("section", { className: "sx-card" }, h("h3", {}, "Identidad del nodo ", h("span", { className: "sx-badge" }, "delicado")), h("p", { className: "sx-help" }, "Nombre corto (hasta 4 caracteres) y nombre largo que ven los demás nodos."), h("div", { className: "sx-addrow" }, short, long, apply), this.noteEl("owner"));
  }

  toolboxCard() {
    const gen = h("div", { className: "kx-keyrow" });
    const out = h("div", {});
    for (const size of K.KEY_SIZES) {
      const b = h("button", { textContent: `Generar ${size * 8} bits` });
      b.onclick = () => {
        const bytes = K.generateKey(size);
        out.replaceChildren(copyRow("Base64", K.bytesToBase64(bytes)), copyRow("Hex", K.bytesToHex(bytes)));
      };
      gen.append(b);
    }
    const conv = this.keyField({ placeholder: "Pega una clave para convertirla (hex ↔ base64)", generate: false });
    const mc = h("input", { type: "text", className: "kx-key", placeholder: "32 dígitos hex de una clave de canal de MeshCore", spellcheck: false });
    const mcOut = h("div", {});
    mc.oninput = () => {
      try {
        mcOut.replaceChildren(copyRow("Para la app Meshtastic (base64)", K.meshCoreKeyToBase64(mc.value.trim())));
      } catch (e) {
        mcOut.replaceChildren(mc.value.trim() ? h("small", { className: "kx-hint bad" }, e.message) : "");
      }
    };
    return h("section", { className: "sx-card" }, h("h3", {}, "Herramientas de claves"), h("p", { className: "sx-help" }, "Todo se calcula en tu navegador. Una clave de canal debe ser aleatoria: genera una, no la inventes."), gen, out, h("h4", { className: "sx-group" }, "Convertir"), conv.el, h("h4", { className: "sx-group" }, "MeshCore → Meshtastic"), mc, mcOut);
  }

  saveBar() {
    const bar = h("div", { className: "sx-save" + (this.dirty ? " show" : "") });
    if (!this.dirty) return bar;
    const btn = h("button", { textContent: "Guardar en el nodo" });
    btn.onclick = async () => {
      await this.exec("save", "SAVE", { persistsNow: true });
      this.dirty = false;
      this.render();
    };
    bar.append(h("span", {}, "⚠ Hay claves o huecos sin guardar. Viven en la memoria del nodo y se pierden al reiniciar."), btn);
    return bar;
  }

  render() {
    this.root.replaceChildren(this.saveBar(), h("p", { className: "sx-intro" }, "Claves y canales: lo más delicado del nodo. Las claves nunca salen de tu navegador salvo hacia el nodo por Bluetooth, y no se guardan en la grabación ni en el registro."), this.channelsCard(), this.adminKeysCard(), this.trustedCard(), this.identityCard(), this.toolboxCard(), this.noteEl("save"));
    this.load();
  }
}
