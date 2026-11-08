// Mounts the USB flasher under the builder's "Disparo directo" button and starts following a build
// as soon as app.js reports that GitHub accepted the dispatch.
import { mountFlasher } from "./flasher-panel.js";
import { REPO, decrypt } from "./flasher-env.js";

const container = document.getElementById("flasherPanel");
const tokenInput = document.getElementById("githubToken");

const flasher = mountFlasher(container, {
  repo: REPO,
  getToken: () => tokenInput.value.trim(),
  decrypt,
});

document.getElementById("flasherFile").addEventListener("change", (event) => {
  const file = event.target.files[0];
  if (!file) return;
  container.classList.remove("hidden");
  flasher.loadFile(file);
  event.target.value = ""; // so picking the same file again triggers a new load
});

document.addEventListener("meshtacore:build-dispatched", (event) => {
  container.classList.remove("hidden");
  container.scrollIntoView({ behavior: "smooth", block: "nearest" });
  flasher.trackBuild(event.detail);
});
