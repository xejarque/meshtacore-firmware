// The flasher panel: one progress line from "build requested" to "board flashed".
// Mounted by builder.html (follows the build it just dispatched) and by flasher.html (any run or file).
import {
  PHASES,
  chipMatchesImage,
  formatBytes,
  overallProgress,
} from "./flasher-core.js";
import { resolveBundle } from "./flasher-bundle.js";
import {
  fetchRunArtifact,
  githubClient,
  waitForRun,
} from "./flasher-github.js";
import {
  closeEsp,
  connectEsp,
  flashEsp,
  webSerialSupported,
} from "./flasher-esp.js";

const WORKFLOW_FILE = "custom_builder.yml";

function h(tag, props = {}, ...children) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (key === "class") el.className = value;
    else if (key.startsWith("on")) el.addEventListener(key.slice(2), value);
    else el.setAttribute(key, value);
  }
  el.append(...children);
  return el;
}

const errorText = (error) =>
  error instanceof Error ? error.message : String(error);

export function mountFlasher(root, { repo, getToken, decrypt }) {
  const ui = {
    fill: h("div", { class: "flasher-bar-fill" }),
    phaseText: h("span"),
    percentText: h("span"),
    phases: h(
      "ul",
      { class: "flasher-phases" },
      ...PHASES.map((p) => h("li", { "data-phase": p.id }, p.label)),
    ),
    image: h("p", { class: "flasher-image hidden" }),
    message: h("p", { class: "flasher-message hidden" }),
    actions: h("div", { class: "flasher-actions" }),
    log: h("pre", { class: "flasher-log hidden" }),
  };
  const section = h(
    "section",
    { class: "flasher" },
    h("h3", { class: "flasher-title" }, "⚡ Flashear por USB"),
    h(
      "div",
      {
        class: "flasher-bar",
        role: "progressbar",
        "aria-valuemin": "0",
        "aria-valuemax": "100",
      },
      ui.fill,
    ),
    h("div", { class: "flasher-bar-text" }, ui.phaseText, ui.percentText),
    ui.phases,
    ui.image,
    ui.message,
    ui.actions,
    ui.log,
  );
  root.replaceChildren(section);

  let bundle = null;
  let abort = null;
  let busy = false;

  const show = (el, visible) => el.classList.toggle("hidden", !visible);
  const log = (text) => {
    show(ui.log, true);
    ui.log.textContent += `${text}\n`;
    ui.log.scrollTop = ui.log.scrollHeight;
  };
  const say = (text, kind = "") => {
    ui.message.textContent = text;
    ui.message.className = `flasher-message ${kind ? `is-${kind}` : ""}`;
    show(ui.message, Boolean(text));
  };

  function setPhase(id, fraction = 0, detail = "") {
    const percent = overallProgress(id, fraction);
    ui.fill.style.width = `${percent}%`;
    section
      .querySelector(".flasher-bar")
      .setAttribute("aria-valuenow", String(percent));
    ui.percentText.textContent = `${percent} %`;
    ui.phaseText.textContent = detail || PHASES.find((p) => p.id === id).label;
    const active = PHASES.findIndex((p) => p.id === id);
    ui.phases.querySelectorAll("li").forEach((li, i) => {
      li.classList.toggle("is-done", i < active);
      li.classList.toggle("is-active", i === active);
    });
  }

  function reset() {
    abort?.abort();
    abort = new AbortController();
    bundle = null;
    section.classList.remove("is-error");
    ui.actions.replaceChildren();
    ui.log.textContent = "";
    show(ui.log, false);
    show(ui.image, false);
    say("");
  }

  function fail(error) {
    if (error?.name === "AbortError") return;
    section.classList.add("is-error");
    say(errorText(error), "error");
    log(`Error: ${errorText(error)}`);
  }

  // ---------- from a finished bundle to the board ----------
  function offerFlash(resolved) {
    bundle = resolved;
    setPhase("connect", 0, "Listo para conectar la placa");
    const { image } = resolved;
    ui.image.textContent = `${image.name} · ${formatBytes(image.data.length)}`;
    show(ui.image, true);

    if (resolved.kind !== "esp32") {
      say(
        "Este paquete es para una placa nRF52 (UF2). El flasheo por USB de estas placas aún no está disponible: descarga el archivo y cópialo a la unidad que aparece al pulsar dos veces RESET.",
      );
      ui.actions.replaceChildren(
        h(
          "button",
          {
            type: "button",
            class: "btn btn-secondary btn-sm",
            onclick: () => download(image),
          },
          "Descargar .uf2",
        ),
      );
      return;
    }
    if (!webSerialSupported()) {
      say(
        "Este navegador no puede acceder al USB. Abre esta página en Chrome o Edge de escritorio.",
        "error",
      );
      return;
    }
    const erase = h("input", { type: "checkbox", id: "flasherErase" });
    const button = h(
      "button",
      {
        type: "button",
        class: "btn btn-primary btn-sm",
        onclick: () => flashBoard(button, erase.checked),
      },
      "🔌 Conectar placa y flashear",
    );
    ui.actions.replaceChildren(
      button,
      h(
        "label",
        {},
        erase,
        " Borrar toda la flash antes (pierde la configuración guardada)",
      ),
    );
    say(
      "Firmware listo. Conecta la placa por USB y pulsa el botón; se abrirá el selector de puerto del navegador.",
    );
  }

  function download(image) {
    const url = URL.createObjectURL(new Blob([image.data]));
    h("a", { href: url, download: image.name }).click();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
  }

  async function flashBoard(button, eraseAll) {
    if (busy || !bundle) return;
    busy = true;
    button.disabled = true;
    section.classList.remove("is-error");
    say("");
    let session = null;
    try {
      setPhase("connect", 0, "Elige el puerto de la placa…");
      session = await connectEsp({ onLog: (line) => line.trim() && log(line) });
      setPhase("connect", 1, `Placa detectada: ${session.chip}`);

      const match = chipMatchesImage(session.chip, bundle.image.data);
      if (!match.ok) throw new Error(`${match.reason} No se ha escrito nada.`);

      if (!bundle.extraImages?.length)
        log(
          "Aviso: el paquete no trae el OTA Loader; la actualización por Bluetooth no funcionará hasta flashearlo.",
        );
      await flashEsp(session, bundle.image, {
        extraImages: bundle.extraImages,
        eraseAll,
        onProgress: (f) =>
          setPhase(
            "flash",
            f,
            `Escribiendo el firmware… ${Math.round(f * 100)} %`,
          ),
      });
      setPhase("flash", 1, "Terminado");
      ui.phases
        .querySelectorAll("li")
        .forEach((li) => li.classList.replace("is-active", "is-done"));
      say(
        "Listo: el firmware está escrito y la placa se ha reiniciado.",
        "success",
      );
    } catch (error) {
      fail(error);
    } finally {
      await closeEsp(session);
      busy = false;
      button.disabled = false;
    }
  }

  // ---------- the three ways in ----------
  async function openArtifact(gh, runId) {
    setPhase("download", 0);
    const artifact = await fetchRunArtifact(gh, runId);
    log(`Artifact: ${artifact.name} (${formatBytes(artifact.size_in_bytes)})`);
    const bytes = await gh.downloadArtifact(artifact, (f) =>
      setPhase(
        "download",
        f,
        `Descargando el firmware… ${Math.round(f * 100)} %`,
      ),
    );
    setPhase("decrypt", 0);
    offerFlash(await resolveBundle("artifact.zip", bytes, { decrypt }));
  }

  function client() {
    const token = getToken();
    if (!token)
      throw new Error(
        "Falta el token de GitHub (permiso de lectura de Actions).",
      );
    return githubClient({ token, repo });
  }

  return {
    /** Follow the build the builder has just dispatched, then offer to flash it. */
    async trackBuild({ dispatchedAtMs }) {
      reset();
      try {
        const gh = client();
        setPhase("build", 0, "Esperando a que GitHub arranque la compilación…");
        const actor = await gh.whoami().catch(() => undefined);
        const run = await waitForRun({
          gh,
          workflowFile: WORKFLOW_FILE,
          dispatchedAtMs,
          actor,
          signal: abort.signal,
          onUpdate: (status) => {
            const label =
              status.state === "queued"
                ? "En cola en GitHub Actions…"
                : "Compilando en GitHub Actions…";
            setPhase("build", status.fraction, label);
          },
        });
        log(`Compilación terminada: ${run.html_url}`);
        await openArtifact(gh, run.id);
      } catch (error) {
        fail(error);
      }
    },

    /** Flash the artifact of a build that already finished. */
    async loadRun(runId) {
      reset();
      try {
        if (!/^\d+$/.test(runId))
          throw new Error("El ID del run son solo números.");
        await openArtifact(client(), runId);
      } catch (error) {
        fail(error);
      }
    },

    /** Flash a file the person already has: .zip.enc, .zip or .factory.bin. */
    async loadFile(file) {
      reset();
      try {
        setPhase("decrypt", 0, "Abriendo el archivo…");
        offerFlash(
          await resolveBundle(
            file.name,
            new Uint8Array(await file.arrayBuffer()),
            { decrypt },
          ),
        );
      } catch (error) {
        fail(error);
      }
    },
  };
}
