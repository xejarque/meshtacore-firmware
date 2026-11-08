// Glue between the flasher modules and the classic scripts of the builder pages (crypto.js, localStorage).

// Same repository as REPO_OWNER/REPO_NAME in app.js; flasher.html does not load app.js.
export const REPO = "xejarque/meshtacore-firmware";

const TOKEN_KEY = "meshtacore_gh_token"; // the key app.js saves the builder's token under

export const getStoredToken = () =>
  (localStorage.getItem(TOKEN_KEY) ?? "").trim();
export const storeToken = (token) =>
  localStorage.setItem(TOKEN_KEY, token.trim());

/** BuildCrypto.decryptBundle with an error that says what to do when this browser lacks the key. */
export async function decrypt(bytes) {
  try {
    return await BuildCrypto.decryptBundle(bytes);
  } catch {
    throw new Error(
      "No se pudo descifrar el paquete: este navegador no tiene la clave con la que se cifró. " +
        "Importa tu copia de seguridad de la clave en la página de descifrado, o repite la compilación desde este navegador.",
    );
  }
}
