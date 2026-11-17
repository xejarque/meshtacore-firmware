// Convierte los errores técnicos de USB / esptool / red en mensajes que se entienden y dicen qué hacer.
// El texto original sigue yendo al registro técnico: aquí solo se traduce lo que ve la persona.

const RULES = [
  {
    test: /no port selected|no device selected|the user aborted|notfounderror/i,
    text: "No elegiste ningún puerto. Pulsa de nuevo el botón y, en la ventana que se abre, selecciona el de tu placa (suele llamarse USB, CP210x o CH340) y pulsa «Conectar».",
  },
  {
    test: /failed to open|access denied|resource busy|already (open|in use)|networkerror/i,
    text: "Otra pestaña o programa está usando la placa. Cierra el monitor serie, la app de Meshtastic y otras pestañas de este navegador, y vuelve a intentarlo.",
  },
  {
    test: /failed to connect|timed? ?out|no serial data received|wrong boot mode|sync/i,
    text: "La placa no responde. Mantén pulsado el botón BOOT, pulsa una vez RESET, suelta BOOT y vuelve a intentarlo. Comprueba también que el cable USB es de datos (algunos solo cargan).",
  },
  {
    test: /http 403|rate limit/i,
    text: "GitHub nos ha limitado temporalmente las descargas. Espera unos minutos y vuelve a intentarlo.",
  },
  {
    test: /failed to fetch|load failed|http 4\d\d|http 5\d\d/i,
    text: "No se ha podido descargar el archivo. Comprueba tu conexión a internet y vuelve a intentarlo en unos minutos.",
  },
];

/** Mensaje en lenguaje sencillo para un error; si no se reconoce, devuelve el texto original con una pista. */
export function friendlyError(error) {
  const raw = error instanceof Error ? error.message : String(error);
  for (const rule of RULES) {
    if (rule.test.test(raw)) return rule.text;
  }
  return `Ha ocurrido un problema: ${raw}. Si se repite, desconecta la placa, vuelve a conectarla y reintenta.`;
}
