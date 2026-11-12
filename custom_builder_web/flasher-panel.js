// The flasher panel: one progress line from "build requested" to "board flashed".
// Mounted by builder.html (follows the build it just dispatched) and by flasher.html (any run or file).
import {
  PHASES,
  chipMatchesImage,
  formatBytes,
  nrfFlashProgress,
  overallProgress,
} from "./flasher-core.js";
import { flashNrf, touchBootloader } from "./flasher-nrf.js";
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

const OTA_LOADER_MISSING =
  "Este paquete no trae el OTA Loader: la placa se flasheará, pero no podrá actualizarse por Bluetooth (error «Device does not have a valid OTA Loader»). Vuelve a compilar el firmware o flashéalo por cable con el loader.";

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
  const modal = h("dialog", { class: "flasher-modal" });
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
    modal,
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

  /**
   * A dialog that says what to do next. Whenever the flow stops waiting for the person (the firmware is
   * ready, the board must change mode, the flash is done) it pops up instead of hiding in the panel.
   * An action's `run` fires inside the click, so the browser still lets it open the port picker.
   */
  function showStep({ icon, title, steps = [], note = "", extra, actions }) {
    const buttons = actions.map(({ label, primary = false, run }) =>
      h(
        "button",
        {
          type: "button",
          class: `btn btn-sm ${primary ? "btn-primary" : "btn-outline"}`,
          onclick: () => {
            modal.close();
            run?.();
          },
        },
        label,
      ),
    );
    modal.replaceChildren(
      h("div", { class: "flasher-modal-icon", "aria-hidden": "true" }, icon),
      h("h3", {}, title),
      ...(steps.length
        ? [
            h(
              "ol",
              { class: "flasher-modal-steps" },
              ...steps.map((s) => h("li", {}, s)),
            ),
          ]
        : []),
      ...(note ? [h("p", { class: "flasher-modal-note" }, note)] : []),
      ...(extra ? [extra] : []),
      h("div", { class: "flasher-modal-actions" }, ...buttons),
    );
    if (!modal.open) modal.showModal();
  }

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
    if (modal.open) modal.close();
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
    // For nRF52 the zip is compressed: show the size of the firmware inside, which is what gets written.
    const size = resolved.dfu ? resolved.dfu.bin.length : image.data.length;
    ui.image.textContent = `${image.name} · ${formatBytes(size)}`;
    show(ui.image, true);

    if (resolved.kind === "uf2") {
      say(
        "Este paquete solo trae un UF2 (sin el paquete DFU), así que no se puede flashear desde aquí: descarga el archivo y cópialo a la unidad que aparece al pulsar dos veces RESET.",
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
    if (resolved.kind === "nrf52") return offerNrfFlash(resolved);
    const erase = h("input", { type: "checkbox", id: "flasherErase" });
    // the dialog has its own checkbox; both stay in step so the choice made in either one is the one used
    const modalErase = h("input", { type: "checkbox" });
    modalErase.addEventListener(
      "change",
      () => (erase.checked = modalErase.checked),
    );
    erase.addEventListener(
      "change",
      () => (modalErase.checked = erase.checked),
    );
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
      resolved.extraImages?.length
        ? "Firmware listo. Conecta la placa por USB y pulsa el botón; se abrirá el selector de puerto del navegador."
        : `Firmware listo. ${OTA_LOADER_MISSING}`,
      resolved.extraImages?.length ? "" : "error",
    );
    showStep({
      icon: "🔌",
      title: "Firmware listo para flashear",
      steps: [
        "Conecta la placa al ordenador con un cable USB que transmita datos.",
        "Pulsa «Conectar y flashear» y elige el puerto de la placa en el selector del navegador.",
        "Espera a que termine la barra de progreso sin desconectarla (unos 20 segundos).",
      ],
      extra: h(
        "label",
        { class: "flasher-modal-option" },
        modalErase,
        " Borrar toda la flash antes (pierde la configuración guardada)",
      ),
      actions: [
        { label: "Más tarde" },
        {
          label: "🔌 Conectar y flashear",
          primary: true,
          run: () => flashBoard(button, erase.checked),
        },
      ],
    });
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
        onErase: () =>
          setPhase(
            "flash",
            0,
            "Borrando toda la flash… puede tardar medio minuto",
          ),
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
      const withLoader = Boolean(bundle.extraImages?.length);
      say(
        withLoader
          ? "Listo: el firmware está escrito y la placa se ha reiniciado."
          : `Firmware escrito, pero sin OTA Loader. ${OTA_LOADER_MISSING}`,
        withLoader ? "success" : "error",
      );
      showStep({
        icon: withLoader ? "✅" : "⚠️",
        title: withLoader
          ? "¡Firmware instalado!"
          : "Firmware instalado, sin OTA Loader",
        steps: [
          "La placa se ha reiniciado sola con el firmware nuevo.",
          "Espera unos segundos a que arranque. Si no lo hace, pulsa el botón RESET.",
          "Ya puedes desconectarla del USB.",
        ],
        note: withLoader ? "" : OTA_LOADER_MISSING,
        actions: [{ label: "Entendido", primary: true }],
      });
    } catch (error) {
      fail(error);
    } finally {
      await closeEsp(session);
      busy = false;
      button.disabled = false;
    }
  }

  // ---------- nRF52: serial DFU through the bootloader ----------
  function offerNrfFlash(resolved) {
    const reboot = h(
      "button",
      {
        type: "button",
        class: "btn btn-secondary btn-sm",
        onclick: () => rebootToBootloader(reboot, showPortStep),
      },
      "1 · Reiniciar en modo bootloader",
    );
    const flash = h(
      "button",
      {
        type: "button",
        class: "btn btn-primary btn-sm",
        onclick: () => flashNrfBoard(flash),
      },
      "2 · Flashear",
    );
    const fallback = resolved.uf2
      ? h(
          "button",
          {
            type: "button",
            class: "btn btn-outline btn-sm",
            onclick: () => download(resolved.uf2),
          },
          "Descargar .uf2",
        )
      : null;
    ui.actions.replaceChildren(...[reboot, flash, fallback].filter(Boolean));
    say(
      "Firmware listo. Pon la placa en modo bootloader (botón 1, o pulsa dos veces RESET), después pulsa el 2 y elige su puerto. Tarda un par de minutos: no la desconectes.",
    );

    function showPortStep() {
      showStep({
        icon: "🔌",
        title: "Ahora elige el puerto del bootloader",
        steps: [
          "Pulsa «Flashear». Se abrirá el selector de puertos del navegador.",
          "Elige el dispositivo de la placa. En modo bootloader aparece con otro nombre que antes.",
          "Espera 2-3 minutos sin desconectarla: borra, escribe y activa el firmware.",
        ],
        actions: [
          { label: "Más tarde" },
          {
            label: "⚡ Flashear",
            primary: true,
            run: () => flashNrfBoard(flash),
          },
        ],
      });
    }

    showStep({
      icon: "🔄",
      title: "Pon la placa en modo bootloader",
      steps: [
        "Con la placa conectada por USB, pulsa «Reiniciar en modo bootloader» y elige su puerto actual.",
        "Si no responde, pulsa dos veces seguidas el botón RESET de la placa: el LED empieza a latir.",
        "Después te pediré que elijas el puerto del bootloader para flashear.",
      ],
      actions: [
        { label: "Ya está en bootloader", run: showPortStep },
        {
          label: "🔄 Reiniciar en modo bootloader",
          primary: true,
          run: () => rebootToBootloader(reboot, showPortStep),
        },
      ],
    });
  }

  async function rebootToBootloader(button, onDone) {
    if (busy) return;
    busy = true;
    button.disabled = true;
    try {
      say("");
      await touchBootloader();
      say(
        "Placa reiniciada en modo bootloader. Pulsa «2 · Flashear» y elige el puerto de la placa.",
        "success",
      );
      onDone?.();
    } catch (error) {
      fail(error);
    } finally {
      busy = false;
      button.disabled = false;
    }
  }

  const NRF_STAGE_TEXT = {
    start: "Iniciando la actualización…",
    erase: "Borrando la memoria de la placa…",
    init: "Enviando la cabecera del firmware…",
    activate: "Activando el firmware. No desconectes la placa…",
  };

  async function flashNrfBoard(button) {
    if (busy || !bundle?.dfu) return;
    busy = true;
    button.disabled = true;
    section.classList.remove("is-error");
    say("");
    let flashFraction = 0; // a new stage changes the text, never moves the bar back
    try {
      setPhase("connect", 0, "Elige el puerto del bootloader…");
      await flashNrf(bundle.dfu, {
        onConnected: () => setPhase("connect", 1, "Conectado al bootloader"),
        onStage: (stage) => {
          if (stage === "start") setPhase("flash", 0, NRF_STAGE_TEXT.start);
          else if (NRF_STAGE_TEXT[stage])
            setPhase("flash", flashFraction, NRF_STAGE_TEXT[stage]);
        },
        onProgress: (stage, fraction) => {
          flashFraction = nrfFlashProgress(stage, fraction);
          setPhase(
            "flash",
            flashFraction,
            stage === "write"
              ? `Escribiendo el firmware… ${Math.round(fraction * 100)} %`
              : NRF_STAGE_TEXT[stage],
          );
        },
      });
      setPhase("flash", 1, "Terminado");
      ui.phases
        .querySelectorAll("li")
        .forEach((li) => li.classList.replace("is-active", "is-done"));
      say(
        "Listo: el firmware está instalado y la placa arranca sola en unos segundos.",
        "success",
      );
      showStep({
        icon: "✅",
        title: "¡Firmware instalado!",
        steps: [
          "La placa sale del bootloader y arranca sola con el firmware nuevo: puede tardar unos segundos.",
          "Si no arranca, pulsa una vez el botón RESET.",
          "Ya puedes desconectarla del USB.",
        ],
        actions: [{ label: "Entendido", primary: true }],
      });
    } catch (error) {
      fail(error);
    } finally {
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
    async trackBuild({ dispatchedAtMs, knownRunIds }) {
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
          ignoreRunIds: knownRunIds,
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
