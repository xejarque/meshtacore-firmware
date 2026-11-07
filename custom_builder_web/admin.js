// Meshtacore node admin - console UI. Transport lives in ble.js.
import { NodeLink } from "./ble.js";
import { Panel } from "./panel.js";

// Read-only commands only: no setters, and nothing that transmits or hops frequency.
// RSSI is left out on purpose: it measures the command packet itself and stays silent for a local one.
const CAPTURE_COMMANDS = ["INFO", "LORA", "LORA-STATUS", "NODES", "CHS", "CH", "POWER", "VOLT", "TM"];

const link = new NodeLink();
const ui = {};
const recording = []; // {command, response, at}
let catalogue = [];
let panel;

function showTab(name) {
  for (const t of ["panel", "console"]) {
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
  recording.push({ command, response, at: new Date().toISOString() });
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
  .on("tx", (t) => log("> " + t, "tx"))
  .on("rx", (t) => log(t, "rx"))
  .on("error", (m) => log(m, "err"))
  .on("connected", (name, id) => {
    ui.status.textContent = "Conectado: " + name;
    ui.node.textContent = id;
    document.body.classList.add("connected");
    log("Conectado. Nodo " + id, "ok");
    panel.start();
  })
  .on("disconnected", () => {
    panel.stop();
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
  if (new URLSearchParams(location.search).has("demo")) {
    ui.status.textContent = "Modo demo (captura de ejemplo, sin nodo)";
    try {
      panel.loadRecording(await (await fetch("tests/fixtures/gat562-capture.json")).json());
    } catch (e) {
      log("No se pudo cargar la demo: " + e.message, "err");
    }
  }
  try {
    catalogue =(await (await fetch("admin.commands.json")).json()).commands;
    renderList("");
  } catch (e) {
    log("No se pudo cargar el catálogo de comandos: " + e.message, "err");
  }
}
document.addEventListener("DOMContentLoaded", init);
