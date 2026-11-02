// Meshtacore Theme Controller - Light Mode by Default
(function () {
  const STORAGE_KEY = "meshtacore_theme";

  // Check saved theme; default to 'light'
  const saved = localStorage.getItem(STORAGE_KEY);
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
