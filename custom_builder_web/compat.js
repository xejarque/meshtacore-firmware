// Avisa nada más abrir la página si el navegador no puede instalar firmware por USB.
// WebUSB / Web Serial solo existen en Google Chrome y Microsoft Edge de escritorio (no en móvil, Safari ni Firefox).
(function () {
  const canUseUsb = "serial" in navigator || "usb" in navigator;
  if (canUseUsb) return;
  const main = document.querySelector("main");
  if (!main) return;
  const banner = document.createElement("div");
  banner.className = "banner-warning";
  banner.setAttribute("role", "alert");
  banner.innerHTML =
    "<strong>Este navegador no puede instalar el firmware por USB.</strong><br />" +
    "Abre esta misma página con <strong>Google Chrome</strong> o <strong>Microsoft Edge</strong> " +
    "en un <strong>ordenador</strong> (no sirve el móvil, ni Safari ni Firefox).";
  main.insertBefore(banner, main.firstChild);
})();
