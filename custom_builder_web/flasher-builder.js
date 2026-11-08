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

document.addEventListener("meshtacore:build-dispatched", (event) => {
  container.classList.remove("hidden");
  container.scrollIntoView({ behavior: "smooth", block: "nearest" });
  flasher.trackBuild(event.detail);
});
