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
 * `actor` narrows it to the person's own runs when several people build at once.
 */
export function pickRun(runs, { dispatchedAtMs, actor } = {}) {
  const candidates = (runs ?? []).filter(
    (run) =>
      run.event === "workflow_dispatch" &&
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
    return {
      kind: "esp32",
      image: { name: baseName(factory.name), data: factory.data, address: 0 },
      extraImages: otaLoaderImages(files),
    };
  }
  const uf2 = files.find((e) => /\.uf2$/i.test(baseName(e.name)));
  if (uf2) {
    return { kind: "uf2", image: { name: baseName(uf2.name), data: uf2.data } };
  }
  throw new Error(
    "El paquete no trae ningún firmware flasheable (*.factory.bin o *.uf2).",
  );
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

/** Does the chip esptool reports ("ESP32-S3 (QFN56) (revision v0.2)") match the image's target? */
export function chipMatchesImage(chipDescription, data) {
  const target = imageChip(data);
  if (!target)
    return {
      ok: false,
      target: null,
      reason: "La imagen no tiene cabecera de ESP32.",
    };
  // "ESP32" must not match "ESP32-S3": compare the whole name before the first space.
  const connected = String(chipDescription).split(" ")[0];
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
