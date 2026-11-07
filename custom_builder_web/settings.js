// Nexus settings panel: sections, explained cards, switches and lists that read the node's state and write back.
// `run(command)` is injected (BLE, remote node, or a recording in demo mode), so this file never touches the transport.
import * as C from "./settings-core.js";

const h = (tag, props = {}, ...kids) => {
  const n = Object.assign(document.createElement(tag), props);
  kids.forEach((k) => n.append(k));
  return n;
};

export class SettingsPanel {
  constructor({ root, run, catalogues, isConnected, label = "", autoRead = true }) {
    this.autoRead = autoRead;
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

  // Reads the entry's state and hands the reply to `show`. With autoRead off (a remote node, where every read
  // costs radio time) nothing is read until the user asks for it.
  whenRead(entry, box, show) {
    const cmd = entry.read.cmd;
    if (this.autoRead || this.cache.has(cmd)) return this.read(cmd).then(show);
    const b = h("button", { className: "sx-add", textContent: "Leer estado del nodo", disabled: !this.isConnected() });
    b.onclick = () => {
      b.textContent = "Leyendo… (puede tardar)";
      b.disabled = true;
      this.read(cmd).then(() => this.render());
    };
    box.append(b);
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
    const parts = [...(entry.flags || []), ...(entry.options || []), ...(entry.actions || []), ...(entry.fields || [])];
    const hay = [entry.title, entry.help, ...parts.flatMap((p) => [p.key, p.title, p.help])].join(" ").toLowerCase();
    return hay.includes(this.filter);
  }

  // `readonly`: the command only reports (or flushes), so it must not raise the "unsaved changes" bar.
  async write(entry, cmd, risk, readonly = false) {
    // On a remote node every write is confirmed (a mistake there cannot be fixed from the keyboard next to it).
    const text = risk === "danger" ? `Esto es delicado${this.label ? " en " + this.label : ""}:\n\n${cmd}\n\n¿Seguro que quieres enviarlo?` : this.label && !readonly ? `Vas a enviar a ${this.label}:\n\n${cmd}\n\n¿Continuar?` : null;
    if (text && !confirm(text)) return false;
    this.busy.add(entry.id);
    this.render();
    try {
      const reply = await this.run(cmd);
      this.notes.set(entry.id, { text: C.cleanReply(reply), ok: !/ERROR|invalid|unknown|expects|cannot|full/i.test(reply) });
      if (entry.read) this.invalidate(entry.read.cmd);
      if (!readonly) this.dirty = true;
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
    this.whenRead(entry, box, (reply) => {
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
    this.whenRead(entry, box, (reply) => {
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

  // Numeric fields (e.g. the global hop mask): shows the live state, an input per field, Apply and Disable.
  valuesBody(entry, box) {
    const body = h("div", { className: "sx-values" });
    box.append(body);
    this.whenRead(entry, box, (reply) => {
      const { state, values } = C.readValues(entry, reply);
      if (Object.values(values).every((v) => v === null)) return body.append(h("p", { className: "sx-note err" }, "No se pudo leer el valor. Respuesta: " + C.cleanReply(reply).slice(0, 160)));
      const inputs = {};
      const row = h("div", { className: "sx-addrow" });
      const attempt = (build) => {
        try {
          this.write(entry, build(), entry.risk);
        } catch (e) {
          this.notes.set(entry.id, { text: e.message, ok: false });
          this.render();
        }
      };
      for (const f of entry.fields) {
        inputs[f.key] = h("input", { type: "number", min: f.min, max: f.max, value: values[f.key], title: f.help || "" });
        const cell = h("label", { className: "sx-field" }, h("small", {}, f.title), inputs[f.key]);
        if (f.write) {
          // a field with its own command gets its own Apply button (e.g. one rate limit per bucket)
          const own = h("button", { className: "sx-add", textContent: "Aplicar", disabled: !this.isConnected() });
          own.onclick = () => attempt(() => C.fieldCommand(f, inputs[f.key].value));
          cell.append(own);
        }
        row.append(cell);
      }
      if (entry.write && entry.write.set) {
        const apply = h("button", { className: "sx-add", textContent: "Aplicar", disabled: !this.isConnected() });
        apply.onclick = () => attempt(() => C.valuesCommand(entry, Object.fromEntries(entry.fields.map((f) => [f.key, inputs[f.key].value]))));
        row.append(apply);
      }
      if (entry.write && entry.write.off) {
        const off = h("button", { className: "sx-clear", textContent: "Desactivar", disabled: !this.isConnected() || state === "OFF" });
        off.onclick = () => this.write(entry, entry.write.off, entry.risk);
        row.append(off);
      }
      const active = state === "ACTIVE" || state === "ON";
      body.append(h("p", { className: "sx-state" + (active ? " on" : "") }, state ? `Estado: ${active ? "activo" : "apagado"}` : ""), row);
    });
  }

  // One choice out of several: a dropdown whose explanation follows the selection, and an Apply button.
  enumBody(entry, box) {
    const body = h("div", { className: "sx-enum" });
    box.append(body);
    this.whenRead(entry, box, (reply) => {
      const { current, raw } = C.readEnum(entry, reply);
      if (raw === null) return body.append(h("p", { className: "sx-note err" }, "No se pudo leer el valor. Respuesta: " + C.cleanReply(reply).slice(0, 160)));
      const select = h("select", { disabled: !this.isConnected() });
      if (!current) select.append(h("option", { value: "", textContent: `(valor no listado: ${raw})` }));
      for (const o of entry.options) select.append(h("option", { value: o.value, textContent: o.title, selected: current === o }));
      const help = h("small", { className: "sx-enum-help" });
      const showHelp = () => (help.textContent = (entry.options.find((o) => o.value === select.value) || { help: "" }).help);
      select.onchange = showHelp;
      showHelp();
      const apply = h("button", { className: "sx-add", textContent: "Aplicar", disabled: !this.isConnected() });
      apply.onclick = () => {
        try {
          const option = entry.options.find((o) => o.value === select.value);
          this.write(entry, C.enumCommand(entry, select.value), option.risk || entry.risk);
        } catch (e) {
          this.notes.set(entry.id, { text: e.message, ok: false });
          this.render();
        }
      };
      body.append(h("p", { className: "sx-state" }, `Ahora: ${current ? current.title : raw}`), h("div", { className: "sx-addrow" }, select, apply), help);
    });
  }

  // One button per action; each runs a fixed command (confirmation for the delicate ones).
  actionsBody(entry, box) {
    const row = h("div", { className: "sx-actions" });
    for (const a of entry.actions) {
      const b = h("button", { className: (a.risk || entry.risk) === "danger" ? "sx-clear" : "sx-add", textContent: a.title, title: a.help, disabled: !this.isConnected() || this.busy.has(entry.id) });
      // {epoch}: the current time of this computer, in Unix seconds (for TIME)
      b.onclick = () => this.write(entry, a.cmd.replace("{epoch}", () => String(Math.floor(Date.now() / 1000))), a.risk || entry.risk, a.readonly === true);
      row.append(h("div", { className: "sx-action" }, b, h("small", {}, a.help)));
    }
    box.append(row);
  }

  card(entry) {
    const box = h("section", { className: "sx-card" + (this.busy.has(entry.id) ? " busy" : "") }, h("h3", {}, entry.title, entry.risk === "danger" ? h("span", { className: "sx-badge" }, "delicado") : ""), h("p", { className: "sx-help" }, entry.help));
    if (entry.type === "flags") this.flagsBody(entry, box);
    else if (entry.type === "list") this.listBody(entry, box);
    else if (entry.type === "values") this.valuesBody(entry, box);
    else if (entry.type === "enum") this.enumBody(entry, box);
    if (entry.actions) this.actionsBody(entry, box);
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
