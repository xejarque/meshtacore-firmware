// Meshtacore Theme Controller - Light Mode by Default
(function () {
  const STORAGE_KEY = "meshtacore_theme";

  // Check saved theme; default to 'light'
  let saved = null;
  try {
    saved = localStorage.getItem(STORAGE_KEY);
  } catch (e) {
    // Storage blocked (private mode, strict settings): the default theme is fine.
  }
  const initialTheme = saved === "dark" ? "dark" : "light";
  document.documentElement.setAttribute("data-theme", initialTheme);

  window.toggleTheme = function () {
    const current =
      document.documentElement.getAttribute("data-theme") || "light";
    const next = current === "dark" ? "light" : "dark";
    document.documentElement.setAttribute("data-theme", next);
    try {
      localStorage.setItem(STORAGE_KEY, next);
    } catch (e) {
      console.warn("localStorage unavailable for theme preference");
    }
    updateButtons(next);
  };

  function updateButtons(theme) {
    document.querySelectorAll(".theme-toggle-btn").forEach((btn) => {
      if (theme === "dark") {
        btn.innerHTML =
          '<span class="theme-icon">☀️</span> <span class="theme-label">Claro</span>';
        btn.setAttribute("title", "Cambiar a Modo Claro");
      } else {
        btn.innerHTML =
          '<span class="theme-icon">🌙</span> <span class="theme-label">Oscuro</span>';
        btn.setAttribute("title", "Cambiar a Modo Oscuro");
      }
    });
  }

  // Ensure DOM buttons reflect current theme once loaded
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => {
      const active =
        document.documentElement.getAttribute("data-theme") || "light";
      updateButtons(active);
      document.querySelectorAll(".theme-toggle-btn").forEach((btn) => {
        btn.addEventListener("click", window.toggleTheme);
      });
    });
  } else {
    const active =
      document.documentElement.getAttribute("data-theme") || "light";
    updateButtons(active);
    document.querySelectorAll(".theme-toggle-btn").forEach((btn) => {
      btn.addEventListener("click", window.toggleTheme);
    });
  }
})();

// El menú "Más" se cierra al pulsar fuera de él o con la tecla Escape.
(function () {
  function closeMenus(except) {
    document.querySelectorAll(".nav-more details[open]").forEach((d) => {
      if (d !== except) d.removeAttribute("open");
    });
  }
  document.addEventListener("click", (event) => {
    closeMenus(event.target.closest(".nav-more details"));
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") closeMenus(null);
  });
})();
