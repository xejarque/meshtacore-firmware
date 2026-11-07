// Guided configuration. Every write shows the exact command, asks for confirmation, and re-reads the node afterwards.
import { parseLora } from "./parsers.js";
import * as R from "./presets.js";

const SVG = "http://www.w3.org/2000/svg";
const APPLY_DELAY_MS = 7000; // the node applies a preset 5 s after replying

const svg = (name, attrs = {}, ...kids) => {
  const n = document.createElementNS(SVG, name);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  kids.forEach((k) => n.append(k));
  return n;
};
const html = (tag, props = {}, ...kids) => {
  const n = Object.assign(document.createElement(tag), props);
  kids.forEach((k) => n.append(k));
  return n;
};

const REGION_COLOUR = { EU_868: "#2a9d6f", EU_N_868: "#3b82c4", EU_866: "#d9a21b", LORA_24: "#a855c7" };
const REGION_NAME = { EU_868: "EU 868", EU_N_868: "EU 868 estrecho", EU_866: "EU 866", LORA_24: "2,4 GHz" };

export class PresetWizard {
  constructor(link, root, presets) {
    this.link = link;
    this.root = root;
    this.presets = presets.map(R.describe);
    this.current = null; // parsed LORA reply
    this.selected = null;
    this.result = null; // {kind, text}
    this.onApplied = () => {};
  }

  setCurrent(lora) {
    this.current = lora;
    this.render();
  }

  async refreshCurrent() {
    if (!this.link.connected) return;
    try {
      this.setCurrent(parseLora(await this.link.run("LORA")));
    } catch (e) {
      this.result = { kind: "err", text: "No se pudo leer el módem: " + e.message };
      this.render();
    }
  }

  get currentPreset() {
    return R.matchPreset(this.current, this.presets);
  }

  select(preset) {
    this.selected = preset;
    this.result = null;
    this.render();
  }

  selectByCommand(command) {
    const preset = this.presets.find((p) => p.command === command);
    if (preset) this.select(preset);
  }

  async apply(preset) {
    this.result = { kind: "info", text: `Enviando PRESET ${preset.command}…` };
    this.render();
    try {
      const reply = await this.link.run(`PRESET ${preset.command}`);
      if (!/accepted/i.test(reply)) {
        this.result = { kind: "err", text: reply || "El nodo no respondió. No se ha cambiado nada." };
        return this.render();
      }
      this.result = { kind: "info", text: reply + "\nEsperando a que el nodo aplique el cambio…" };
      this.render();
      await new Promise((r) => setTimeout(r, APPLY_DELAY_MS));
      await this.refreshCurrent();
      const now = this.currentPreset;
      const ok = now && now.command === preset.command;
      this.result = { kind: ok ? "ok" : "err", text: ok ? `Hecho: el nodo usa ahora ${preset.longName || preset.command}.` : "El nodo respondió pero la lectura posterior no coincide. Comprueba con LORA en la consola." };
      this.onApplied();
    } catch (e) {
      this.result = { kind: "err", text: "Error: " + e.message };
    }
    this.render();
  }

  chart() {
    const W = 560,
      H = 300,
      m = { l: 56, r: 16, t: 14, b: 40 };
    const xs = this.presets.map((p) => Math.log10(p.bitrate));
    const ys = this.presets.map((p) => p.sensitivity);
    const x0 = Math.min(...xs) - 0.1,
      x1 = Math.max(...xs) + 0.1,
      y0 = Math.min(...ys) - 2,
      y1 = Math.max(...ys) + 2;
    const X = (v) => m.l + ((Math.log10(v) - x0) / (x1 - x0)) * (W - m.l - m.r);
    const Y = (v) => m.t + ((v - y0) / (y1 - y0)) * (H - m.t - m.b); // more negative = further = higher up
    const root = svg("svg", { viewBox: `0 0 ${W} ${H}`, class: "pchart", role: "img", "aria-label": "Presets: velocidad frente a alcance" });
    for (const f of [1e2, 1e3, 1e4, 1e5]) {
      if (Math.log10(f) < x0 || Math.log10(f) > x1) continue;
      root.append(svg("line", { x1: X(f), x2: X(f), y1: m.t, y2: H - m.b, class: "pgrid" }), svg("text", { x: X(f), y: H - m.b + 16, class: "ptick", "text-anchor": "middle" }, f >= 1000 ? `${f / 1000} kb/s` : `${f} b/s`));
    }
    for (let d = Math.ceil(y0 / 5) * 5; d <= y1; d += 5) root.append(svg("line", { x1: m.l, x2: W - m.r, y1: Y(d), y2: Y(d), class: "pgrid" }), svg("text", { x: m.l - 6, y: Y(d) + 3, class: "ptick", "text-anchor": "end" }, `${d}`));
    root.append(svg("text", { x: (W + m.l) / 2, y: H - 6, class: "plabel", "text-anchor": "middle" }, "Velocidad →"), svg("text", { x: 12, y: H / 2, class: "plabel", transform: `rotate(-90 12 ${H / 2})`, "text-anchor": "middle" }, "← Más alcance (sensibilidad dBm)"));
    const cur = this.currentPreset;
    for (const p of this.presets) {
      const g = svg("g", { class: "ppoint", tabindex: 0, role: "button", "aria-label": `${p.command}: ${R.compareTo(p, cur || p).text}` });
      const isSel = this.selected === p,
        isCur = cur === p;
      g.append(svg("circle", { cx: X(p.bitrate), cy: Y(p.sensitivity), r: isSel || isCur ? 7 : 5, fill: REGION_COLOUR[p.region], class: isSel ? "sel" : isCur ? "cur" : "" }));
      g.append(svg("title", {}, `${p.command}${p.longName && p.longName !== "-" ? " · " + p.longName : ""}\nSF${p.sf} · ${p.bw} kHz · 4/${p.cr} · ${p.freq} MHz\n${Math.round(p.bitrate)} b/s · ${p.sensitivity.toFixed(0)} dBm`));
      g.onclick = () => this.select(p);
      g.onkeydown = (e) => (e.key === "Enter" || e.key === " ") && this.select(p);
      root.append(g);
    }
    return root;
  }

  detail() {
    const p = this.selected;
    if (!p) return html("p", { className: "muted" }, "Elige un preset en el gráfico o en la lista para ver qué cambiaría.");
    const cur = this.currentPreset;
    const same = cur === p;
    const box = html("div", { className: "pdetail" });
    box.append(html("h3", {}, p.command), html("p", { className: "muted" }, [p.longName !== "-" ? p.longName : "", p.group].filter(Boolean).join(" · ")));
    box.append(
      html(
        "dl",
        { className: "kv" },
        ...[
          ["SF / BW / CR", `${p.sf} / ${p.bw} kHz / 4:${p.cr}`],
          ["Frecuencia", `${p.freq} MHz (${REGION_NAME[p.region]})`],
          ["Velocidad", `${Math.round(p.bitrate)} b/s`],
          ["Sensibilidad", `${p.sensitivity.toFixed(0)} dBm`],
        ].flatMap(([k, v]) => [html("dt", {}, k), html("dd", {}, v)]),
      ),
    );
    if (cur && !same) box.append(html("p", { className: "compare" }, `Frente al actual (${cur.command}): ${R.compareTo(p, cur).text}.`));
    if (same) box.append(html("p", { className: "compare" }, "Es el preset que usa el nodo ahora."));
    if (R.needsDualBand(p)) box.append(html("p", { className: "warn" }, "Requiere un radio con 2,4 GHz (LR1121 o LR2021). Un radio de una sola banda lo rechazará."));
    box.append(html("p", { className: "warn" }, "Cambiar de preset te deja sin contacto con los nodos que sigan en otro. Comprueba también que esa banda y potencia son legales en tu país."));
    const ack = html("input", { type: "checkbox", id: "preset-ack" });
    const btn = html("button", { className: "run", disabled: true, textContent: "Aplicar preset" });
    const sync = () => (btn.disabled = !ack.checked || !this.link.connected || same);
    ack.onchange = sync;
    sync();
    btn.onclick = () => this.apply(p);
    box.append(html("p", {}, html("code", {}, `/mtcl PRESET ${p.command}`)), html("label", { className: "ack" }, ack, " Entiendo que el nodo puede quedar aislado de la malla actual"), btn);
    if (!this.link.connected) box.append(html("p", { className: "muted" }, "Conecta el nodo por Bluetooth para poder aplicar."));
    if (this.result) box.append(html("pre", { className: "out " + this.result.kind }, this.result.text));
    return box;
  }

  list() {
    const wrap = html("div", { className: "plist" });
    for (const [group, items] of R.groupPresets(this.presets)) {
      wrap.append(html("h4", {}, group), html("div", { className: "chips" }, ...items.map((p) => html("button", { className: "chip" + (p === this.selected ? " on" : "") + (p === this.currentPreset ? " now" : ""), onclick: () => this.select(p), title: `SF${p.sf} ${p.bw} kHz` }, p.command))));
    }
    return wrap;
  }

  render() {
    const cur = this.currentPreset;
    const head = html("p", { className: "muted" }, this.current ? `Radio actual: SF${this.current.sf} · ${this.current.bw} kHz · ${this.current.freq} MHz${cur ? " → " + cur.command : " (no coincide con ningún preset)"}` : "Leyendo el módem…");
    const legend = html("p", { className: "legend" }, ...Object.entries(REGION_NAME).map(([k, v]) => html("span", {}, html("i", { style: `background:${REGION_COLOUR[k]}` }), v)));
    this.root.replaceChildren(head, html("div", { className: "pgrid2" }, html("div", {}, this.chart(), legend, this.list()), this.detail()));
  }
}
