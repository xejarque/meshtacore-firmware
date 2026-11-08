// "Actualizar": installs a new application image on a board that already runs Meshtastic, keeping its
// configuration. Two sources: the standard release published next to this page, or the user's own build.
import { chipMatchesImage, formatBytes } from "./flasher-core.js";
import { resolveBundle } from "./flasher-bundle.js";
import {
  closeEsp,
  connectEsp,
  updateEspApp,
  webSerialSupported,
} from "./flasher-esp.js";
import { decrypt } from "./flasher-env.js";
import {
  buildIdFromImage,
  listBoards,
  parseFirmwareName,
  releaseVersion,
} from "./update-core.js";

const $ = (id) => document.getElementById(id);
const errorText = (error) =>
  error instanceof Error ? error.message : String(error);

const ui = {
  boardSelect: $("boardSelect"),
  releaseInfo: $("releaseInfo"),
  confirm: $("confirmStandard"),
  releaseButton: $("btnUpdateRelease"),
  ownFile: $("ownFile"),
  ownInfo: $("ownInfo"),
  ownButton: $("btnUpdateOwn"),
  fill: $("updateFill"),
  bar: $("updateBar"),
  status: $("updateStatus"),
  message: $("updateMessage"),
  log: $("updateLog"),
  modal: $("updateModal"),
};

let releaseBoards = [];
let ownImage = null; // {name, data}
let busy = false;

// ---------- feedback ----------
function setProgress(fraction, text) {
  const percent = Math.round(Math.max(0, Math.min(1, fraction)) * 100);
  ui.fill.style.width = `${percent}%`;
  ui.bar.setAttribute("aria-valuenow", String(percent));
  ui.status.textContent = `${text} · ${percent} %`;
}

function say(text, kind = "") {
  ui.message.textContent = text;
  ui.message.className = `flasher-message ${kind ? `is-${kind}` : ""} ${text ? "" : "hidden"}`;
}

function log(line) {
  ui.log.classList.remove("hidden");
  ui.log.textContent += `${line}\n`;
  ui.log.scrollTop = ui.log.scrollHeight;
}

function showDialog({ icon, title, steps, actions }) {
  const buttons = actions.map(({ label, primary }) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `btn btn-sm ${primary ? "btn-primary" : "btn-outline"}`;
    button.textContent = label;
    button.addEventListener("click", () => ui.modal.close());
    return button;
  });
  const list = document.createElement("ol");
  list.className = "flasher-modal-steps";
  for (const step of steps) {
    const item = document.createElement("li");
    item.textContent = step;
    list.append(item);
  }
  const iconEl = document.createElement("div");
  iconEl.className = "flasher-modal-icon";
  iconEl.setAttribute("aria-hidden", "true");
  iconEl.textContent = icon;
  const heading = document.createElement("h3");
  heading.textContent = title;
  const actionsEl = document.createElement("div");
  actionsEl.className = "flasher-modal-actions";
  actionsEl.append(...buttons);
  ui.modal.replaceChildren(iconEl, heading, list, actionsEl);
  ui.modal.showModal();
}

// ---------- the update itself ----------
async function updateBoard(image, buttons) {
  if (busy) return;
  busy = true;
  buttons.forEach((b) => (b.disabled = true));
  say("");
  ui.log.textContent = "";
  let session = null;
  try {
    setProgress(0, "Elige el puerto de la placa");
    session = await connectEsp({ onLog: (line) => line.trim() && log(line) });
    setProgress(0.05, `Placa detectada: ${session.chip}`);

    const match = chipMatchesImage(session.chip, image.data);
    if (!match.ok) throw new Error(`${match.reason} No se ha escrito nada.`);

    const plan = await updateEspApp(session, image, {
      onProgress: (f) =>
        setProgress(0.05 + f * 0.95, "Escribiendo el firmware"),
    });
    log(
      `Escrito en la partición «${plan.app.label}» (0x${plan.app.offset.toString(16)}).`,
    );
    setProgress(1, "Terminado");
    say(
      "Listo: el firmware está actualizado y la placa se ha reiniciado.",
      "success",
    );
    showDialog({
      icon: "✅",
      title: "¡Placa actualizada!",
      steps: [
        "La placa se ha reiniciado sola con el firmware nuevo.",
        "Tu configuración (canales, LoRa, nombre) se ha conservado.",
        "Espera unos segundos a que arranque y ya puedes desconectarla.",
      ],
      actions: [{ label: "Entendido", primary: true }],
    });
  } catch (error) {
    ui.bar.classList.add("is-error");
    say(errorText(error), "error");
    log(`Error: ${errorText(error)}`);
  } finally {
    await closeEsp(session);
    busy = false;
    buttons.forEach((b) => (b.disabled = false));
    refreshButtons();
  }
}

function confirmThenUpdate(image, buttons) {
  ui.bar.classList.remove("is-error");
  showDialog({
    icon: "🔌",
    title: "Conecta la placa por USB",
    steps: [
      "Usa un cable USB que transmita datos y conecta la placa.",
      "Pulsa «Actualizar» y elige el puerto de la placa en el selector del navegador.",
      "Espera a que termine sin desconectarla (unos 20 segundos).",
    ],
    actions: [{ label: "Cancelar" }],
  });
  // The picker needs a click, so the dialog's own button starts the update.
  const go = document.createElement("button");
  go.type = "button";
  go.className = "btn btn-sm btn-primary";
  go.textContent = "🔌 Actualizar";
  go.addEventListener("click", () => {
    ui.modal.close();
    updateBoard(image, buttons);
  });
  ui.modal.querySelector(".flasher-modal-actions").append(go);
}

// ---------- standard release ----------
function selectedBoard() {
  return releaseBoards.find((b) => b.name === ui.boardSelect.value) ?? null;
}

function refreshButtons() {
  const board = selectedBoard();
  const noUsb = !webSerialSupported();
  ui.releaseButton.disabled =
    noUsb || busy || !board || board.kind !== "esp32" || !ui.confirm.checked;
  ui.ownButton.disabled = noUsb || busy || !ownImage;
}

function describeBoard() {
  const board = selectedBoard();
  if (!board) return;
  ui.releaseInfo.textContent =
    board.kind === "esp32"
      ? `${board.name} · ${formatBytes(board.size)}${board.buildId ? ` · build ${board.buildId}` : ""}`
      : "Las placas nRF52 aún no se pueden actualizar desde aquí.";
  refreshButtons();
}

async function loadRelease() {
  try {
    const response = await fetch("firmware/index.json", { cache: "no-cache" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const index = await response.json();
    releaseBoards = listBoards(index);
    if (releaseBoards.length === 0) throw new Error("vacío");
    $("releaseVersion").textContent = releaseVersion(index);
    ui.boardSelect.replaceChildren(
      ...releaseBoards.map((board) => {
        const option = document.createElement("option");
        option.value = board.name;
        option.textContent = `${board.board}${board.kind === "nrf52" ? " (nRF52, próximamente)" : ""}`;
        return option;
      }),
    );
    describeBoard();
  } catch {
    ui.boardSelect.replaceChildren();
    ui.releaseInfo.textContent =
      "No hay firmware publicado en esta página todavía. Usa la pestaña «Mi build».";
    ui.boardSelect.disabled = true;
  }
}

async function updateFromRelease() {
  const board = selectedBoard();
  if (!board) return;
  ui.releaseButton.disabled = true;
  setProgress(0, "Descargando el firmware");
  try {
    const response = await fetch(`firmware/${encodeURIComponent(board.name)}`);
    if (!response.ok)
      throw new Error(
        `no se pudo descargar ${board.name} (HTTP ${response.status})`,
      );
    const data = new Uint8Array(await response.arrayBuffer());
    setProgress(0, "Firmware descargado");
    confirmThenUpdate({ name: board.name, data }, [
      ui.releaseButton,
      ui.ownButton,
    ]);
  } catch (error) {
    ui.bar.classList.add("is-error");
    say(errorText(error), "error");
  } finally {
    refreshButtons();
  }
}

// ---------- own build ----------
async function loadOwnFile(file) {
  ownImage = null;
  ui.ownInfo.textContent = "Leyendo el archivo…";
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    let image;
    if (/\.bin$/i.test(file.name) && parseFirmwareName(file.name)) {
      image = { name: file.name, data: bytes }; // a plain app image, as published in Releases
    } else {
      const bundle = await resolveBundle(file.name, bytes, { decrypt });
      image = bundle.updateImage;
      if (!image)
        throw new Error(
          bundle.kind === "esp32"
            ? "El paquete no trae el .bin de la aplicación (solo el .factory.bin)."
            : "Este paquete es de una placa nRF52: aún no se puede actualizar desde aquí.",
        );
    }
    ownImage = image;
    const id = buildIdFromImage(image.data);
    ui.ownInfo.textContent = `${image.name} · ${formatBytes(image.data.length)}${id ? ` · build ${id}` : ""}`;
  } catch (error) {
    ui.ownInfo.textContent = errorText(error);
  }
  refreshButtons();
}

// ---------- tabs and start-up ----------
function selectTab(name) {
  for (const tab of document.querySelectorAll("[data-tab]"))
    tab.setAttribute("aria-selected", String(tab.dataset.tab === name));
  for (const panel of document.querySelectorAll("[data-panel]"))
    panel.classList.toggle("hidden", panel.dataset.panel !== name);
}

document
  .querySelectorAll("[data-tab]")
  .forEach((tab) =>
    tab.addEventListener("click", () => selectTab(tab.dataset.tab)),
  );
ui.boardSelect.addEventListener("change", describeBoard);
ui.confirm.addEventListener("change", refreshButtons);
ui.releaseButton.addEventListener("click", updateFromRelease);
ui.ownButton.addEventListener("click", () =>
  confirmThenUpdate(ownImage, [ui.releaseButton, ui.ownButton]),
);
ui.ownFile.addEventListener("change", (event) => {
  const file = event.target.files[0];
  if (file) loadOwnFile(file);
});

if (!webSerialSupported()) {
  say(
    "Este navegador no puede acceder al USB. Abre esta página en Chrome o Edge de escritorio.",
    "error",
  );
  ui.releaseButton.disabled = true;
  ui.ownButton.disabled = true;
}
selectTab("release");
loadRelease();
