// Nexus settings panel: sections, explained cards, switches and lists that read the node's state and write back.
// `run(command)` is injected (BLE, remote node, or a recording in demo mode), so this file never touches the transport.
import * as C from "./settings-core.js";

const h = (tag, props = {}, ...kids) => {
  const n = Object.assign(document.createElement(tag), props);
  kids.forEach((k) => n.append(k));
  return n;
};

export class SettingsPanel {
  constructor({ root, run, catalogues, isConnected, label = "" }) {
    this.root = root;
    this.run = run;
    this.isConnected = isConnected;
    this.label = label; // e.g. the remote node's name, shown in the confirmations
    this.catalogues = catalogues;
    this.section = catalogues[0] && catalogues[0].id;
    this.filter = "";
    this.cache = new Map(); // cmd -> Promise<reply>
    this.notes = new Map(); // entry id -> last reply text
    this.dirty = false;
    this.busy = new Set();
  }

  get current() {
    return this.catalogues.find((c) => c.id === this.section);
  }

  // One read per distinct command: several entries can share it.
  read(cmd) {
    if (!this.cache.has(cmd)) this.cache.set(cmd, this.run(cmd).catch((e) => `ERROR ${e.message}`));
    return this.cache.get(cmd);
  }

  invalidate(cmd) {
    this.cache.delete(cmd);
  }

  reset() {
    this.cache.clear();
    this.render();
  }

  matches(entry) {
    if (!this.filter) return true;
    const hay = [entry.title, entry.help, ...(entry.flags || []).flatMap((f) => [f.key, f.title, f.help])].join(" ").toLowerCase();
    return hay.includes(this.filter);
  }

  async write(entry, cmd, risk) {
    const text = risk === "danger" ? `Esto es delicado${this.label ? " en " + this.label : ""}:\n\n${cmd}\n\n¿Seguro que quieres enviarlo?` : null;
    if (text && !confirm(text)) return false;
    this.busy.add(entry.id);
    this.render();
    try {
      const reply = await this.run(cmd);
      this.notes.set(entry.id, { text: C.cleanReply(reply), ok: !/ERROR|invalid|unknown|expects|cannot|full/i.test(reply) });
      if (entry.read) this.invalidate(entry.read.cmd);
      this.dirty = true;
    } catch (e) {
      this.notes.set(entry.id, { text: "Error: " + e.message, ok: false });
    }
    this.busy.delete(entry.id);
    this.render();
    return true;
  }

  async save() {
    this.busy.add("save");
    this.render();
    try {
      const reply = await this.run("SAVE");
      this.notes.set("save", { text: C.cleanReply(reply), ok: !/ERROR|fail/i.test(reply) });
      this.dirty = false;
    } catch (e) {
      this.notes.set("save", { text: "Error: " + e.message, ok: false });
    }
    this.busy.delete("save");
    this.render();
  }

  // ---------- rendering ----------
  noteEl(entry) {
    const n = this.notes.get(entry.id);
    return n ? h("p", { className: "sx-note " + (n.ok ? "ok" : "err") }, n.text) : "";
  }

  flagsBody(entry, box) {
    const body = h("div", { className: "sx-flags" });
    box.append(body);
    this.read(entry.read.cmd).then((reply) => {
      const state = C.readFlags(entry, reply);
      let lastGroup = null;
      for (const f of entry.flags) {
        if (this.filter && ![f.key, f.title, f.help].join(" ").toLowerCase().includes(this.filter) && !entry.title.toLowerCase().includes(this.filter)) continue;
        if (f.group && f.group !== lastGroup) {
          lastGroup = f.group;
          body.append(h("h4", { className: "sx-group" }, f.group));
        }
        const on = state[f.key];
        const input = h("input", { type: "checkbox", checked: on === true, disabled: on === null || !this.isConnected() || this.busy.has(entry.id) });
        input.onchange = async () => {
          const wanted = input.checked;
          const done = await this.write(entry, C.flagCommand(entry, f, wanted), f.risk || entry.risk);
          if (!done) input.checked = !wanted;
        };
        body.append(
          h("label", { className: "sx-flag" + (on ? " on" : "") }, h("span", { className: "sx-switch" }, input, h("i")), h("span", { className: "sx-flag-text" }, h("b", {}, f.title, " ", h("code", {}, f.key)), h("small", {}, f.help))),
        );
      }
      if (on_unknown(state)) body.prepend(h("p", { className: "sx-note err" }, "No se pudo leer el estado de este bloque. Respuesta del nodo: " + C.cleanReply(reply).slice(0, 160)));
    });
  }

  listBody(entry, box) {
    const body = h("div", { className: "sx-list" });
    box.append(body);
    this.read(entry.read.cmd).then((reply) => {
      const items = C.readList(entry, reply);
      if (items === null) return body.append(h("p", { className: "sx-note err" }, "No se pudo leer la lista. Respuesta: " + C.cleanReply(reply).slice(0, 160)));
      const chips = h("div", { className: "sx-chips" });
      if (!items.length) chips.append(h("span", { className: "muted" }, "Lista vacía"));
      for (const it of items) {
        const x = h("button", { className: "sx-x", title: "Quitar", textContent: "✕", disabled: !this.isConnected() });
        x.onclick = () => this.write(entry, C.fill(entry.list.remove, { value: it.value }), entry.risk);
        chips.append(h("span", { className: "sx-chip" }, it.label, x));
      }
      const input = h("input", { type: "text", placeholder: entry.list.placeholder || "", spellcheck: false });
      const add = h("button", { className: "sx-add", textContent: "Añadir", disabled: !this.isConnected() });
      add.onclick = async () => {
        const v = input.value.trim();
        if (!v || !C.safeArg(v)) return this.notes.set(entry.id, { text: "Valor no válido.", ok: false }), this.render();
        await this.write(entry, C.fill(entry.list.add, { value: v }), entry.risk);
      };
      input.onkeydown = (e) => e.key === "Enter" && add.click();
      const row = h("div", { className: "sx-addrow" }, input, add);
      if (entry.list.clear && items.length) {
        const clear = h("button", { className: "sx-clear", textContent: "Vaciar", disabled: !this.isConnected() });
        clear.onclick = () => this.write(entry, entry.list.clear, "danger");
        row.append(clear);
      }
      body.append(chips, row);
    });
  }

  card(entry) {
    const box = h("section", { className: "sx-card" + (this.busy.has(entry.id) ? " busy" : "") }, h("h3", {}, entry.title, entry.risk === "danger" ? h("span", { className: "sx-badge" }, "delicado") : ""), h("p", { className: "sx-help" }, entry.help));
    if (entry.type === "flags") this.flagsBody(entry, box);
    else if (entry.type === "list") this.listBody(entry, box);
    box.append(this.noteEl(entry));
    return box;
  }

  render() {
    const sec = this.current;
    const nav = h("nav", { className: "sx-nav" }, h("input", { type: "search", placeholder: "Buscar opción…", value: this.filter, oninput: (e) => ((this.filter = e.target.value.toLowerCase()), this.renderMain()) }));
    for (const c of this.catalogues) {
      const b = h("button", { className: c.id === this.section ? "on" : "", onclick: () => ((this.section = c.id), this.render()) }, `${c.icon || "•"} ${c.title}`);
      nav.append(b);
    }
    this.root.replaceChildren(this.saveBar(), h("div", { className: "sx-layout" }, nav, (this.main = h("div", { className: "sx-main" }))));
    this.renderMain();
    this.root.querySelector(".sx-nav input").value = this.filter;
  }

  renderMain() {
    const sec = this.current;
    if (!sec) return;
    this.main.replaceChildren(h("h2", {}, `${sec.icon || ""} ${sec.title}`), h("p", { className: "sx-intro" }, sec.intro), ...sec.entries.filter((e) => this.matches(e)).map((e) => this.card(e)));
    if (!this.isConnected()) this.main.prepend(h("p", { className: "sx-note err" }, "Conecta el nodo para leer y cambiar estas opciones."));
  }

  saveBar() {
    const bar = h("div", { className: "sx-save" + (this.dirty ? " show" : "") });
    if (this.dirty) {
      const btn = h("button", { textContent: this.busy.has("save") ? "Guardando…" : "Guardar en el nodo", disabled: this.busy.has("save") });
      btn.onclick = () => this.save();
      bar.append(h("span", {}, "⚠ Hay cambios sin guardar. Viven en la memoria del nodo y se pierden al reiniciar."), btn);
    } else if (this.notes.get("save")) bar.append(h("span", { className: "sx-note ok" }, "Guardado: " + this.notes.get("save").text));
    return bar;
  }
}

function on_unknown(state) {
  const v = Object.values(state);
  return v.length > 0 && v.every((x) => x === null);
}
