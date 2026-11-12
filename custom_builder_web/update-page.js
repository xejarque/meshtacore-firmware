// "Actualizar": installs a new application image on a board that already runs Meshtastic, keeping its
// configuration. Two sources: the standard release published next to this page, or the user's own build.
import {
  chipMatchesImage,
  formatBytes,
  otaLoaderName,
} from "./flasher-core.js";
import { resolveBundle } from "./flasher-bundle.js";
import {
  closeEsp,
  connectEsp,
  installEsp,
  planBoard,
  readNodeBuildId,
  updateEspApp,
  webSerialSupported,
} from "./flasher-esp.js";
import { decrypt } from "./flasher-env.js";
import {
  buildIdFromImage,
  compareBuilds,
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

/** A yes/no question in the modal; resolves true only for the confirming button. */
function askToContinue({ icon, title, steps, confirmLabel }) {
  return new Promise((resolve) => {
    showDialog({
      icon,
      title,
      steps,
      actions: [{ label: "Cancelar" }],
    });
    const go = document.createElement("button");
    go.type = "button";
    go.className = "btn btn-sm btn-primary";
    go.textContent = confirmLabel;
    go.addEventListener("click", () => {
      go.dataset.confirmed = "1";
      ui.modal.close();
    });
    ui.modal.querySelector(".flasher-modal-actions").append(go);
    ui.modal.addEventListener(
      "close",
      () => resolve(go.dataset.confirmed === "1"),
      { once: true },
    );
  });
}

/**
 * Standard releases are picked by hand and only the chip family is checked against the image, so compare the
 * fingerprint of what the board runs with the release's: a different one means another board or a customised build.
 */
async function confirmReleaseMatchesNode(session, image) {
  setProgress(0.06, "Comprobando la build instalada");
  const releaseId = buildIdFromImage(image.data);
  let nodeId = null;
  try {
    nodeId = await readNodeBuildId(session, image.data.length);
  } catch (error) {
    log(`No se pudo leer la build instalada: ${errorText(error)}`);
  }
  const verdict = compareBuilds(nodeId, releaseId);
  log(`Build de la placa: ${nodeId ?? "?"} · de la release: ${releaseId ?? "?"}`);
  if (verdict === "same") return true;
  return askToContinue({
    icon: "⚠️",
    title:
      verdict === "different"
        ? "La placa no tiene la build estándar de esta release"
        : "No se puede comprobar qué placa es",
    steps:
      verdict === "different"
        ? [
            "El firmware instalado se compiló con otras opciones o es de otra placa.",
            `Vas a instalar ${image.name}: sustituirá esas opciones por las estándar.`,
            "Si no estás seguro de que sea la placa correcta, cancela.",
          ]
        : [
            "La placa o la release no traen huella de build (firmware anterior a este control).",
            `Comprueba tú que ${image.name} es el firmware de esta placa.`,
          ],
    confirmLabel: "Actualizar igualmente",
  });
}

/** The OTA Loader for the connected chip, or null (with the reason logged) so the update still goes ahead. */
async function fetchLoader(image, chip) {
  if (!image.getLoader) return null;
  try {
    const loader = await image.getLoader(chip);
    if (!loader) log(`No hay OTA Loader para ${chip}: no se añadirá.`);
    return loader;
  } catch (error) {
    log(`No se pudo obtener el OTA Loader: ${errorText(error)}`);
    return null;
  }
}

/** A blank board, or one with another layout: erases it and writes the factory image plus the OTA Loader. */
async function installFromScratch(session, image, loader, reason) {
  const factory = await image.getFactory?.();
  if (!factory) throw new Error(reason);
  const ok = await askToContinue({
    icon: "⚠️",
    title: "Instalación completa: se borrará la placa",
    steps: [
      "La placa no tiene firmware de Meshtastic con el reparto de memoria de esta release.",
      "Se borra toda la flash, incluida la configuración, y se instala desde cero.",
      "Si la placa ya funciona y esperabas conservar tu configuración, cancela.",
    ],
    confirmLabel: "Borrar e instalar",
  });
  if (!ok) throw new Error("Instalación cancelada. No se ha escrito nada.");
  const { loaderWritten } = await installEsp(session, factory, {
    loader,
    onErase: () =>
      setProgress(0.08, "Borrando toda la flash… puede tardar medio minuto"),
    onProgress: (f) => setProgress(0.1 + f * 0.9, "Escribiendo el firmware"),
  });
  return loaderWritten;
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

    const loader = await fetchLoader(image, session.chip);
    const choice = await planBoard(session, image.data.length);
    let loaderWritten;
    if (choice.mode === "install") {
      loaderWritten = await installFromScratch(
        session,
        image,
        loader,
        choice.reason,
      );
    } else {
      if (
        image.checkBuild &&
        !(await confirmReleaseMatchesNode(session, image))
      )
        throw new Error("Actualización cancelada. No se ha escrito nada.");
      const plan = await updateEspApp(session, image, {
        loader,
        onProgress: (f) =>
          setProgress(0.05 + f * 0.95, "Escribiendo el firmware"),
      });
      log(
        `Escrito en la partición «${plan.app.label}» (0x${plan.app.offset.toString(16)}).`,
      );
      loaderWritten = plan.loaderWritten;
    }
    setProgress(1, "Terminado");
    const installed = choice.mode === "install";
    say(
      installed
        ? "Listo: el firmware está instalado y la placa se ha reiniciado."
        : "Listo: el firmware está actualizado y la placa se ha reiniciado.",
      "success",
    );
    showDialog({
      icon: "✅",
      title: installed ? "¡Placa instalada!" : "¡Placa actualizada!",
      steps: [
        "La placa se ha reiniciado sola con el firmware nuevo.",
        installed
          ? "Es una instalación limpia: configura la región y los canales."
          : "Tu configuración (canales, LoRa, nombre) se ha conservado.",
        loaderWritten
          ? "Se ha añadido el OTA Loader: ya puedes actualizar por Bluetooth desde la app."
          : "Espera unos segundos a que arranque y ya puedes desconectarla.",
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

/** Fetched only when needed: the loader for the chip that turns out to be connected, the factory image for an install. */
function releaseExtras(board) {
  const download = async (file) => {
    const response = await fetch(`firmware/${encodeURIComponent(file)}`);
    if (!response.ok) throw new Error(`${file} (HTTP ${response.status})`);
    return { name: file, data: new Uint8Array(await response.arrayBuffer()) };
  };
  return {
    getLoader: async (chip) => {
      const file = otaLoaderName(chip);
      return file ? download(file) : null;
    },
    getFactory: () => download(board.name.replace(/\.bin$/i, ".factory.bin")),
  };
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
    confirmThenUpdate(
      { name: board.name, data, checkBuild: true, ...releaseExtras(board) },
      [ui.releaseButton, ui.ownButton],
    );
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
      image = bundle.updateImage && {
        ...bundle.updateImage,
        getFactory: async () => bundle.image,
        getLoader: async () => bundle.extraImages?.[0] ?? null,
      };
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
