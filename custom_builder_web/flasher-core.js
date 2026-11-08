// Pure logic of the web flasher (no DOM, no network): progress phases, picking the workflow run and
// artifact, reading what is inside a firmware bundle and checking the image against the connected chip.

// ---------- progress phases ----------
// The bar is one continuous line across the whole path from "build requested" to "board flashed".
// Weights are rough shares of the time each phase takes.
export const PHASES = [
  { id: "build", label: "Compilando en GitHub Actions", weight: 55 },
  { id: "download", label: "Descargando el firmware", weight: 8 },
  { id: "decrypt", label: "Descifrando", weight: 2 },
  { id: "connect", label: "Conectando con la placa", weight: 5 },
  { id: "flash", label: "Escribiendo el firmware", weight: 30 },
];

const clamp01 = (n) => Math.min(1, Math.max(0, Number.isFinite(n) ? n : 0));

/** Overall progress (0-100) when `phaseId` is `fraction` (0-1) done. Earlier phases count as complete. */
export function overallProgress(phaseId, fraction) {
  const index = PHASES.findIndex((p) => p.id === phaseId);
  if (index < 0) throw new Error(`Fase desconocida: ${phaseId}`);
  const total = PHASES.reduce((sum, p) => sum + p.weight, 0);
  const done = PHASES.slice(0, index).reduce((sum, p) => sum + p.weight, 0);
  return Math.round(
    ((done + PHASES[index].weight * clamp01(fraction)) / total) * 100,
  );
}

// ---------- GitHub Actions run tracking ----------
const CLOCK_SKEW_MS = 15_000;

/**
 * The newest workflow_dispatch run created at or after the moment we dispatched.
 * `actor` narrows it to the person's own runs when several people build at once; `ignoreRunIds` are the runs
 * that already existed before the dispatch, which the clock-skew margin would otherwise let through.
 */
export function pickRun(
  runs,
  { dispatchedAtMs, actor, ignoreRunIds = [] } = {},
) {
  const candidates = (runs ?? []).filter(
    (run) =>
      run.event === "workflow_dispatch" &&
      !ignoreRunIds.includes(run.id) &&
      Date.parse(run.created_at) >= dispatchedAtMs - CLOCK_SKEW_MS &&
      (!actor || run.actor?.login === actor),
  );
  candidates.sort(
    (a, b) => Date.parse(b.created_at) - Date.parse(a.created_at),
  );
  return candidates[0] ?? null;
}

/** Where the build is: queued / running / success / failed, with a 0-1 fraction from the job steps. */
export function describeRun(run, jobs = []) {
  if (!run) return { state: "queued", fraction: 0 };
  if (run.status === "completed") {
    return run.conclusion === "success"
      ? { state: "success", fraction: 1 }
      : { state: "failed", fraction: 1, conclusion: run.conclusion };
  }
  const steps = jobs.flatMap((job) => job.steps ?? []);
  if (run.status === "queued" || steps.length === 0)
    return { state: "queued", fraction: 0 };
  const finished = steps.filter((s) => s.status === "completed").length;
  return { state: "running", fraction: finished / steps.length };
}

/** The firmware artifact of a run: not expired, and a bundle (plain or encrypted zip). */
export function pickArtifact(artifacts) {
  const bundles = (artifacts ?? []).filter(
    (a) => !a.expired && /\.zip(\.enc)?$/i.test(a.name),
  );
  bundles.sort(
    (a, b) => Date.parse(b.created_at ?? 0) - Date.parse(a.created_at ?? 0),
  );
  return bundles[0] ?? null;
}

// ---------- bundle contents ----------
/** An Actions artifact is a zip around the one file the workflow uploaded: return that file. */
export function unwrapArtifact(entries) {
  const files = entries.filter((e) => !e.name.endsWith("/"));
  if (files.length !== 1) {
    throw new Error(
      `El artifact debería traer un único archivo y trae ${files.length}.`,
    );
  }
  return files[0];
}

export const isEncrypted = (name) => /\.enc$/i.test(name);

const baseName = (path) => path.split("/").pop();

/**
 * The *.factory.bin does not carry the OTA Loader: it lives in the ota_1 partition and must be written
 * on its own, or the node answers "Device does not have a valid OTA Loader" to a Bluetooth update.
 * The offset comes from the partition table of the build manifest (*.mt.json). Without the loader
 * file or the manifest there is nothing to add.
 */
export function otaLoaderImages(files) {
  const loader = files.find((e) => /(^|\/)mt-[^/]*-ota\.bin$/i.test(e.name));
  const manifest = files.find((e) => /\.mt\.json$/i.test(baseName(e.name)));
  if (!loader || !manifest) return [];
  let parts;
  try {
    parts = JSON.parse(new TextDecoder().decode(manifest.data)).part;
  } catch {
    return [];
  }
  const ota1 = Array.isArray(parts)
    ? parts.find((p) => p.subtype === "ota_1")
    : null;
  const address = Number(ota1?.offset);
  if (!Number.isInteger(address) || address <= 0) return [];
  return [{ name: baseName(loader.name), data: loader.data, address }];
}

/**
 * What can be flashed from the files of a bundle. ESP32 images are the *.factory.bin (bootloader +
 * partition table + app, written at 0x0); nRF52/RP2040 bundles carry a UF2 instead.
 */
export function classifyBundle(entries) {
  const files = entries.filter((e) => !e.name.endsWith("/"));
  const factory = files.find((e) => /\.factory\.bin$/i.test(baseName(e.name)));
  if (factory) {
    // the plain app image is what an update writes (the factory image would overwrite the configuration)
    const app = files.find((e) =>
      /^firmware-.*(?<!\.factory)\.bin$/i.test(baseName(e.name)),
    );
    return {
      kind: "esp32",
      image: { name: baseName(factory.name), data: factory.data, address: 0 },
      extraImages: otaLoaderImages(files),
      updateImage: app && { name: baseName(app.name), data: app.data },
    };
  }
  // nRF52 bundles also carry bin/*.uf2 helpers (factory erase, bootloader updates): only the build's own
  // firmware-*.uf2 is the firmware, and flashing any of the others would wipe the node or its bootloader.
  const uf2Entry = files.find((e) =>
    /^firmware-.*\.uf2$/i.test(baseName(e.name)),
  );
  const uf2 = uf2Entry && {
    name: baseName(uf2Entry.name),
    data: uf2Entry.data,
  };
  const dfu = files.find((e) =>
    /^firmware-.*-ota\.zip$/i.test(baseName(e.name)),
  );
  if (dfu) {
    return {
      kind: "nrf52",
      image: { name: baseName(dfu.name), data: dfu.data },
      uf2,
    };
  }
  if (uf2) return { kind: "uf2", image: uf2 };
  throw new Error(
    "El paquete no trae ningún firmware flasheable (*.factory.bin, *-ota.zip o *.uf2).",
  );
}

/**
 * Progress (0-1) across several files that esptool reports one at a time: each file counts for its
 * size, so the bar keeps growing instead of going back to 0 when the next file starts.
 */
export function combinedProgress(sizes, fileIndex, fraction) {
  const total = sizes.reduce((sum, size) => sum + size, 0);
  if (!total) return 0;
  const before = sizes.slice(0, fileIndex).reduce((sum, size) => sum + size, 0);
  return (before + sizes[fileIndex] * clamp01(fraction)) / total;
}

// ---------- nRF52 DFU progress ----------
// The flash phase of an nRF52 has three parts of very different length (roughly 18 s erasing, 90 s
// writing and 38 s activating for an 800 KB image); this keeps the bar moving at a steady pace.
const NRF_STAGES = {
  erase: [0, 0.12],
  write: [0.12, 0.74],
  activate: [0.74, 1],
};

/** Progress (0-1) of the whole flash phase when `stage` is `fraction` (0-1) done. */
export function nrfFlashProgress(stage, fraction) {
  const range = NRF_STAGES[stage];
  if (!range) throw new Error(`Etapa DFU desconocida: ${stage}`);
  return range[0] + (range[1] - range[0]) * clamp01(fraction);
}

// ---------- image vs chip safety check ----------
// ESP image header: byte 0 is 0xE9, bytes 12-13 (little endian) hold the chip the image was built for.
const IMAGE_MAGIC = 0xe9;
const CLASSIC_BOOTLOADER_OFFSET = 0x1000; // the classic ESP32 bootloader sits at 0x1000, not 0x0
export const CHIP_IDS = {
  0: "ESP32",
  2: "ESP32-S2",
  5: "ESP32-C3",
  9: "ESP32-S3",
  12: "ESP32-C2",
  13: "ESP32-C6",
  16: "ESP32-H2",
};

/** Chip name the image targets, or null if the data does not look like an ESP image. */
export function imageChip(data) {
  for (const offset of [0, CLASSIC_BOOTLOADER_OFFSET]) {
    if (data.length > offset + 13 && data[offset] === IMAGE_MAGIC) {
      const id = data[offset + 12] | (data[offset + 13] << 8);
      if (id in CHIP_IDS) return CHIP_IDS[id];
    }
  }
  return null;
}

/**
 * Family of the chip esptool describes: "ESP32-D0WD-V3 (revision v3.1)" → "ESP32", "ESP32-S2FNR2" → "ESP32-S2".
 * Classic parts carry a package suffix (D0WD, PICO, U4WDH...) and some others a flash/PSRAM one (S2FNR2).
 */
export function chipFamily(chipDescription) {
  const name = String(chipDescription).trim().split(/[\s(]/)[0].toUpperCase();
  const derived = Object.values(CHIP_IDS)
    .filter((family) => family !== "ESP32")
    .sort((a, b) => b.length - a.length)
    .find((family) => name.startsWith(family));
  if (derived) return derived;
  return name.startsWith("ESP32") ? "ESP32" : name;
}

/** Does the chip esptool reports ("ESP32-S3 (QFN56) (revision v0.2)") match the image's target? */
export function chipMatchesImage(chipDescription, data) {
  const target = imageChip(data);
  if (!target)
    return {
      ok: false,
      target: null,
      reason: "La imagen no tiene cabecera de ESP32.",
    };
  const connected = chipFamily(chipDescription);
  return connected === target
    ? { ok: true, target }
    : {
        ok: false,
        target,
        reason: `La imagen es para ${target} y la placa es ${connected}.`,
      };
}

export const formatBytes = (n) =>
  n >= 1_048_576
    ? `${(n / 1_048_576).toFixed(1)} MB`
    : `${Math.ceil(n / 1024)} KB`;
