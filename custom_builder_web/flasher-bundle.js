// Turns whatever the person has (an Actions artifact, an encrypted bundle, a plain zip or a bare .bin)
// into the image to flash. No DOM and no serial port, so it runs in the browser and under node --test.
import { unzipSync } from "./vendor/fflate.js";
import { classifyBundle, isEncrypted, unwrapArtifact } from "./flasher-core.js";
import { readDfuPackage } from "./nrf-dfu-core.js";

const MAX_NESTING = 4; // artifact zip -> .zip.enc -> bundle zip is 3 deep; more means something is wrong
const isZip = (name) => /\.zip$/i.test(name);

function unzipEntries(bytes) {
  const files = unzipSync(bytes);
  return Object.entries(files).map(([name, data]) => ({ name, data }));
}

/**
 * @param {string} name file name as downloaded or dropped by the user
 * @param {Uint8Array} bytes its content
 * @param {{decrypt: (bytes: Uint8Array) => Promise<Uint8Array>}} deps BuildCrypto.decryptBundle in the browser
 * @returns the classification of {@link classifyBundle}
 */
export async function resolveBundle(name, bytes, { decrypt }, depth = 0) {
  if (depth > MAX_NESTING)
    throw new Error(
      "El archivo tiene demasiadas capas: no parece un paquete del builder.",
    );

  if (isEncrypted(name)) {
    const plain = await decrypt(bytes);
    return resolveBundle(
      name.replace(/\.enc$/i, ""),
      plain,
      { decrypt },
      depth + 1,
    );
  }

  if (/\.bin$/i.test(name)) return classifyBundle([{ name, data: bytes }]);

  if (!isZip(name))
    throw new Error(
      `Formato no soportado: ${name}. Usa un .zip, .zip.enc o .factory.bin.`,
    );

  const entries = unzipEntries(bytes);
  const files = entries.filter((e) => !e.name.endsWith("/"));
  // An Actions artifact is a zip holding the one file the workflow uploaded (usually the .zip.enc).
  if (
    files.length === 1 &&
    (isZip(files[0].name) || isEncrypted(files[0].name))
  ) {
    const inner = unwrapArtifact(entries);
    return resolveBundle(inner.name, inner.data, { decrypt }, depth + 1);
  }
  const bundle = classifyBundle(entries);
  if (bundle.kind === "nrf52") bundle.dfu = openDfuZip(bundle.image);
  return bundle;
}

/** The application image and init packet inside the DFU zip of an nRF52 build. */
function openDfuZip(image) {
  let entries;
  try {
    entries = unzipEntries(image.data);
  } catch {
    throw new Error(
      `El paquete DFU ${image.name} está dañado y no se puede abrir.`,
    );
  }
  return readDfuPackage(entries);
}
