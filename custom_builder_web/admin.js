// Meshtacore node admin - console UI. Transport lives in ble.js.
import { NodeLink } from "./ble.js";
import { Panel } from "./panel.js";
import { SettingsPanel } from "./settings.js";
import { KeysPanel } from "./keys-panel.js";
import { redact } from "./keys-core.js";
import { RemotePanel } from "./remote-panel.js";

// Read-only commands only: no setters, and nothing that transmits or hops frequency.
// RSSI is left out on purpose: it measures the command packet itself and stays silent for a local one.
const CAPTURE_COMMANDS = ["INFO", "LORA", "LORA-STATUS", "NODES", "CHS", "CH", "POWER", "VOLT", "TM"];

const link = new NodeLink();
const ui = {};
const recording = []; // {command, response, at}
let catalogue = [];
let panel;
let settingsPanel;
let keysPanel;
let remotePanel;
const demoReplies = new Map(); // command -> recorded reply; filled only in ?demo mode
const isDemo = new URLSearchParams(location.search).has("demo");
const TABS = ["panel", "settings", "keys", "remote", "console"];

// In demo mode reads come from recordings and nothing is ever sent.
async function demoRun(command) {
  if (demoReplies.has(command)) return demoReplies.get(command);
  return `🔴 MESHTACORE: (demo) no se envía: ${command}`;
}

function showTab(name) {
  for (const t of TABS) {
    document.getElementById("tab-" + t).setAttribute("aria-selected", String(t === name));
    document.getElementById("view-" + t).hidden = t !== name;
  }
}

function log(msg, cls) {
  const line = document.createElement("div");
  line.className = "log-line " + (cls || "");
  line.textContent = msg;
  ui.log.appendChild(line);
  ui.log.scrollTop = ui.log.scrollHeight;
}

// Every command goes through here so the recording sees the console and the capture alike.
async function runRecorded(command) {
  const response = await link.run(command);
  recording.push({ command: redact(command), response, at: new Date().toISOString() }); // keys never reach the downloaded file
  ui.recCount.textContent = String(recording.length);
  return response;
}

function download(name, text) {
  const url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  const a = Object.assign(document.createElement("a"), { href: url, download: name });
  a.click();
  URL.revokeObjectURL(url);
}

function downloadRecording() {
  if (!recording.length) return log("Nada que descargar: ejecuta algún comando primero.", "err");
  const payload = { node: link.nodeId, capturedAt: new Date().toISOString(), outputs: recording };
  download("meshtacore-salidas-" + Date.now() + ".json", JSON.stringify(payload, null, 1));
}

async function captureAll(btn) {
  btn.disabled = true;
  try {
    for (const [i, cmd] of CAPTURE_COMMANDS.entries()) {
      btn.textContent = `Capturando ${i + 1}/${CAPTURE_COMMANDS.length}: ${cmd}…`;
      try {
        await runRecorded(cmd);
      } catch (e) {
        log(`${cmd}: ${e.message}`, "err");
      }
    }
    downloadRecording();
  } finally {
    btn.disabled = false;
    btn.textContent = "Capturar salidas del nodo";
  }
}

// ---------- connection ----------
link
  .on("tx", (t) => log("> " + redact(t), "tx"))
  .on("rx", (t) => log(t, "rx"))
  .on("error", (m) => log(m, "err"))
  .on("connected", (name, id) => {
    ui.status.textContent = "Conectado: " + name;
    ui.node.textContent = id;
    document.body.classList.add("connected");
    log("Conectado. Nodo " + id, "ok");
    panel.start();
    settingsPanel.reset();
    keysPanel.reset();
    remotePanel.reset();
  })
  .on("disconnected", () => {
    panel.stop();
    settingsPanel.reset();
    keysPanel.reset();
    remotePanel.reset();
    document.body.classList.remove("connected");
    ui.status.textContent = "Desconectado";
    log("Conexión Bluetooth cerrada.", "err");
  });

async function connect() {
  try {
    await link.connect();
  } catch (e) {
    log("No se pudo conectar: " + e.message, "err");
  }
}

// ---------- catalogue UI ----------
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
        out.textContent = (await runRecorded(input.value)) || "(sin respuesta)";
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
  ["status", "node", "log", "list", "detail", "search", "recCount"].forEach((id) => (ui[id] = document.getElementById(id)));
  panel = new Panel(link, document.getElementById("panel"));
  document.querySelectorAll("[data-tab]").forEach((b) => (b.onclick = () => showTab(b.dataset.tab)));
  document.getElementById("pause").onchange = (e) => (panel.paused = e.target.checked);
  document.getElementById("connect").onclick = connect;
  document.getElementById("disconnect").onclick = () => link.disconnect();
  const capture = document.getElementById("capture");
  capture.onclick = () => captureAll(capture);
  document.getElementById("download").onclick = downloadRecording;
  ui.search.oninput = () => renderList(ui.search.value);
  if (isDemo) {
    ui.status.textContent = "Modo demo (capturas de ejemplo, sin nodo)";
    try {
      for (const file of ["gat562-capture.json", "gat562-firewall.json", "gat562-radio.json", "gat562-system.json", "gat562-keys.json", "from-firmware-source-system.json"]) {
        const rec = await (await fetch("tests/fixtures/" + file)).json();
        rec.outputs.forEach((o) => demoReplies.set(o.command, o.response));
        if (file === "gat562-capture.json") panel.loadRecording(rec);
      }
    } catch (e) {
      log("No se pudo cargar la demo: " + e.message, "err");
    }
  }
  keysPanel = new KeysPanel({ root: document.getElementById("keys"), run: isDemo ? demoRun : runRecorded, isConnected: isDemo ? () => true : () => link.connected });
  keysPanel.render();
  try {
    const files = await (await fetch("settings/index.json")).json();
    const catalogues = await Promise.all(files.map(async (f) => (await fetch("settings/" + f)).json()));
    settingsPanel = new SettingsPanel({ root: document.getElementById("settings"), catalogues, run: isDemo ? demoRun : runRecorded, isConnected: isDemo ? () => true : () => link.connected });
    settingsPanel.render();
    // In demo mode the "remote" node answers from the recordings, with a short fake delay.
    const runRemote = isDemo ? async (_id, cmd) => (await new Promise((r) => setTimeout(r, 400)), demoRun(cmd)) : (id, cmd, route) => link.runRemote(id, cmd, route);
    remotePanel = new RemotePanel({ root: document.getElementById("remote"), runLocal: isDemo ? demoRun : runRecorded, runRemote, isConnected: isDemo ? () => true : () => link.connected, catalogues });
    const demoTarget = new URLSearchParams(location.search).get("target"); // demo only: ?demo&tab=remote&target=!1234abcd[&manage]
    if (isDemo && demoTarget) {
      remotePanel.target = { id: demoTarget, name: "Nodo de ejemplo" };
      if (new URLSearchParams(location.search).has("manage")) remotePanel.startManaging();
    }
    remotePanel.render();
  } catch (e) {
    log("No se pudo cargar el catálogo de ajustes: " + e.message, "err");
  }
  const params = new URLSearchParams(location.search);
  if (TABS.includes(params.get("tab"))) showTab(params.get("tab"));
  if (settingsPanel && (params.get("section") || params.get("find"))) {
    settingsPanel.section = params.get("section") || settingsPanel.section;
    settingsPanel.filter = (params.get("find") || "").toLowerCase();
    settingsPanel.render();
  }
  try {
    catalogue =(await (await fetch("admin.commands.json")).json()).commands;
    renderList("");
  } catch (e) {
    log("No se pudo cargar el catálogo de comandos: " + e.message, "err");
  }
}
document.addEventListener("DOMContentLoaded", init);
