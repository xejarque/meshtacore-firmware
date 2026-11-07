// Live panel: polls read-only commands through the shared queue and draws the results.
import * as P from "./parsers.js";

const SVG = "http://www.w3.org/2000/svg";
const MAX_HISTORY = 60;
// Fast data is cheap (one short reply); NODES pages through ~14 messages, so it polls rarely.
const POLLS = [
  { cmd: "POWER", every: 10_000, parse: P.parsePower, key: "power" },
  { cmd: "VOLT", every: 30_000, parse: P.parseVolt, key: "volt" },
  { cmd: "LORA-STATUS", every: 60_000, parse: P.parseLoraStatus, key: "radio" },
  { cmd: "CHS", every: 45_000, parse: P.parseChannelStats, key: "channels" },
  { cmd: "NODES", every: 60_000, parse: P.parseNodes, key: "nodes" },
  { cmd: "INFO", every: 600_000, parse: P.parseInfo, key: "info" },
];

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

// SNR -> colour (LoRa decodes down to about -20 dB; above 0 is comfortable).
function snrColour(snr) {
  if (snr >= 5) return "var(--ok, #2a9d6f)";
  if (snr >= -5) return "var(--warn, #d9a21b)";
  return "var(--bad, #d1493f)";
}

// Half-circle gauge: value in [min,max], coloured by a caller-supplied rule.
function gauge({ label, value, min, max, unit, colour, decimals = 1 }) {
  const frac = value === null ? 0 : Math.max(0, Math.min(1, (value - min) / (max - min)));
  const r = 52;
  const arc = (f) => {
    const a = Math.PI * (1 - f);
    return `${60 + r * Math.cos(a)} ${64 - r * Math.sin(a)}`;
  };
  return html(
    "div",
    { className: "gauge" },
    svg(
      "svg",
      { viewBox: "0 0 120 78", role: "img", "aria-label": `${label}: ${value === null ? "sin dato" : value.toFixed(decimals) + unit}` },
      svg("path", { d: `M ${60 - r} 64 A ${r} ${r} 0 0 1 ${60 + r} 64`, class: "gauge-track" }),
      frac > 0 ? svg("path", { d: `M ${60 - r} 64 A ${r} ${r} 0 0 1 ${arc(frac)}`, class: "gauge-fill", stroke: colour }) : "",
      svg("text", { x: 60, y: 62, class: "gauge-value" }, value === null ? "—" : value.toFixed(decimals) + unit),
    ),
    html("div", { className: "gauge-label" }, label),
  );
}

function sparkline(values, { width = 160, height = 36, colour = "currentColor" } = {}) {
  if (values.length < 2) return html("span", { className: "muted" }, "recopilando…");
  const max = Math.max(...values, 1);
  const pts = values.map((v, i) => `${((i / (values.length - 1)) * width).toFixed(1)},${(height - (v / max) * (height - 4) - 2).toFixed(1)}`);
  return svg("svg", { viewBox: `0 0 ${width} ${height}`, class: "spark", role: "img", "aria-label": "histórico" }, svg("polyline", { points: pts.join(" "), fill: "none", stroke: colour, "stroke-width": 2 }));
}

// Neighbours as dots on hop rings. Angle comes from the node id, so a node keeps its place between refreshes.
function radar(nodes) {
  const size = 320;
  const c = size / 2;
  const rings = 7;
  const hash = (s) => [...s].reduce((h, ch) => (h * 31 + ch.charCodeAt(0)) >>> 0, 7);
  const root = svg("svg", { viewBox: `0 0 ${size} ${size}`, class: "radar", role: "img", "aria-label": "Radar de vecinos por saltos" });
  for (let i = 1; i <= rings; i++) root.append(svg("circle", { cx: c, cy: c, r: (c - 14) * (i / rings), class: "radar-ring" }));
  for (let i = 1; i <= rings; i += 2) root.append(svg("text", { x: c + 3, y: c - (c - 14) * (i / rings) + 10, class: "radar-tick" }, `${i}`));
  root.append(svg("circle", { cx: c, cy: c, r: 6, class: "radar-self" }));
  nodes
    .filter((n) => !n.self)
    .forEach((n) => {
      const h = hash(n.id);
      const angle = ((h % 3600) / 3600) * Math.PI * 2;
      const hops = Math.max(0, Math.min(rings, n.hops));
      const radius = (c - 14) * ((hops + (((h >> 8) % 60) - 30) / 100 + 0.5) / (rings + 0.5));
      const dot = svg("circle", { cx: c + radius * Math.cos(angle), cy: c + radius * Math.sin(angle), r: 4, fill: snrColour(n.snr), class: "radar-dot" });
      dot.append(svg("title", {}, `${n.name.trim() || n.id} · ${n.id}\n${n.hops} saltos · SNR ${n.snr} dB`));
      root.append(dot);
    });
  return root;
}

function hopHistogram(nodes) {
  const counts = new Map();
  nodes.filter((n) => !n.self).forEach((n) => counts.set(n.hops, (counts.get(n.hops) || 0) + 1));
  const peak = Math.max(1, ...counts.values());
  const box = html("div", { className: "bars" });
  [...counts.keys()].sort((a, b) => a - b).forEach((h) => {
    const n = counts.get(h);
    box.append(html("div", { className: "bar-row" }, html("span", {}, h === 0 ? "directos" : `${h} salto${h > 1 ? "s" : ""}`), html("div", { className: "bar" }, html("i", { style: `width:${(n / peak) * 100}%` })), html("b", {}, String(n))));
  });
  return box;
}

function channelBars(stats) {
  const top = stats.channels.slice(0, 8);
  const peak = Math.max(1, ...top.map((c) => c.packets));
  const box = html("div", { className: "bars" });
  top.forEach((c) =>
    box.append(
      html("div", { className: "bar-row" }, html("span", { title: `Canal ${c.index}` }, c.known ? c.name : `Ch${c.index} (sin clave)`), html("div", { className: "bar" }, html("i", { style: `width:${(c.packets / peak) * 100}%` })), html("b", {}, `${c.packets}`)),
    ),
  );
  return box;
}

const card = (title, ...body) => html("section", { className: "card" }, html("h3", {}, title), ...body);

export class Panel {
  constructor(link, root) {
    this.link = link;
    this.root = root;
    this.data = {};
    this.history = { chanUtil: [], txUtil: [] };
    this.timers = [];
    this.running = false;
  }

  start() {
    if (this.running) return;
    this.running = true;
    // Stagger the first reads so the cheap ones paint first; the queue keeps them from overlapping.
    POLLS.forEach((p, i) => {
      const tick = () => this.poll(p);
      setTimeout(() => this.running && tick(), i * 150);
      this.timers.push(setInterval(tick, p.every));
    });
    this.render();
  }

  // Paints the panel from a recorded capture ({outputs:[{command,response}]}); no node needed.
  loadRecording(recording) {
    const byCmd = Object.fromEntries(recording.outputs.map((o) => [o.command, o.response]));
    for (const p of POLLS) {
      const value = byCmd[p.cmd] === undefined ? null : p.parse(byCmd[p.cmd]);
      if (value !== null) this.data[p.key] = value;
    }
    if (this.data.power) {
      this.history.chanUtil = [0.4, 0.9, 0.6, 1.4, 0.8, 0.5, this.data.power.channelUtil];
      this.history.txUtil = [0.2, 0.3, 0.9, 0.5, 0.4, 0.8, this.data.power.txUtil];
    }
    this.render();
  }

  stop() {
    this.running = false;
    this.timers.forEach(clearInterval);
    this.timers = [];
  }

  async poll(p) {
    if (!this.running || !this.link.connected || this.paused) return;
    try {
      const value = p.parse(await this.link.run(p.cmd));
      if (value === null || (Array.isArray(value) && !value.length)) return;
      this.data[p.key] = value;
      if (p.key === "power") {
        this.history.chanUtil = [...this.history.chanUtil, value.channelUtil].slice(-MAX_HISTORY);
        this.history.txUtil = [...this.history.txUtil, value.txUtil].slice(-MAX_HISTORY);
      }
      this.render();
    } catch (e) {
      this.data.error = `${p.cmd}: ${e.message}`;
      this.render();
    }
  }

  render() {
    const { info, power, volt, radio, nodes, channels, error } = this.data;
    const cards = [];
    if (info) cards.push(card("Nodo", html("p", { className: "big" }, `${info.shortName} · ${info.role}`), html("p", { className: "muted" }, `${info.id} · v${info.version}`), html("p", { className: "muted" }, info.target)));
    cards.push(
      card(
        "Estado",
        html(
          "div",
          { className: "gauges" },
          gauge({ label: "Ocupación canal", value: power ? power.channelUtil : null, min: 0, max: 50, unit: "%", colour: power && power.channelUtil > 25 ? "var(--bad, #d1493f)" : "var(--ok, #2a9d6f)" }),
          gauge({ label: "Uso TX", value: power ? power.txUtil : null, min: 0, max: 10, unit: "%", colour: power && power.txUtil > 7 ? "var(--bad, #d1493f)" : "var(--ok, #2a9d6f)" }),
          gauge({ label: "Batería", value: volt ? volt.battery : null, min: 3.3, max: 4.2, unit: " V", decimals: 2, colour: volt && volt.battery < 3.6 ? "var(--bad, #d1493f)" : "var(--ok, #2a9d6f)" }),
        ),
        html("div", { className: "sparks" }, html("span", {}, "Canal ", sparkline(this.history.chanUtil)), html("span", {}, "TX ", sparkline(this.history.txUtil))),
      ),
    );
    if (radio) {
      cards.push(
        card(
          "Módem",
          html(
            "dl",
            { className: "kv" },
            ...[
              ["Frecuencia", `${radio.freq} MHz`],
              ["SF / BW / CR", `${radio.sf} / ${radio.bw} kHz / 4:${radio.cr}`],
              ["Velocidad", `${radio.bitrate} b/s`],
              ["Potencia", `${radio.chipPower} dBm`],
              ["Preámbulo", `${radio.preambleSymbols} símbolos (${radio.preambleMs} ms)`],
              ["Sync word", `0x${radio.syncWord.toString(16).toUpperCase()}`],
            ].flatMap(([k, v]) => [html("dt", {}, k), html("dd", {}, v)]),
          ),
        ),
      );
    }
    if (nodes) {
      const others = nodes.filter((n) => !n.self);
      cards.push(card(`Vecinos (${others.length})`, html("div", { className: "radar-wrap" }, radar(nodes), hopHistogram(nodes)), html("p", { className: "muted" }, "Anillos = saltos. Color = SNR: verde ≥ 5 dB, ámbar ≥ -5 dB, rojo por debajo.")));
    }
    if (channels && channels.channels.length) cards.push(card(`Tráfico por canal (${channels.totalPackets} paquetes)`, channelBars(channels)));
    if (error) cards.push(html("p", { className: "err" }, error));
    if (!power && !nodes) cards.unshift(html("p", { className: "muted" }, "Leyendo el nodo…"));
    this.root.replaceChildren(...cards);
  }
}
