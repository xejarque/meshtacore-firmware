// GitHub side of the flasher: find the build we just dispatched, follow it until it ends and download
// its artifact. Everything runs from the browser with the token the builder already asks for; both the
// API and the storage it redirects to answer with Access-Control-Allow-Origin: *.
import { describeRun, pickArtifact, pickRun } from "./flasher-core.js";

const API = "https://api.github.com";
const POLL_MS = 5000;

const sleep = (ms, signal) =>
  new Promise((resolve, reject) => {
    const timer = setTimeout(resolve, ms);
    signal?.addEventListener("abort", () => {
      clearTimeout(timer);
      reject(new DOMException("Cancelado", "AbortError"));
    });
  });

export function githubClient({
  token,
  repo,
  fetchFn = (...args) => fetch(...args),
}) {
  const headers = {
    Accept: "application/vnd.github+json",
    Authorization: `Bearer ${token}`,
    "X-GitHub-Api-Version": "2022-11-28",
  };

  async function request(path) {
    const res = await fetchFn(
      path.startsWith("http") ? path : `${API}${path}`,
      { headers },
    );
    if (!res.ok) {
      const body = await res.json().catch(() => ({}));
      const hint =
        res.status === 401 || res.status === 403
          ? " Revisa que el token tenga permiso de lectura de Actions."
          : "";
      throw new Error(
        `GitHub respondió ${res.status}: ${body.message ?? res.statusText}.${hint}`,
      );
    }
    return res;
  }
  const json = async (path) => (await request(path)).json();

  return {
    async whoami() {
      return (await json("/user")).login;
    },
    async listRuns(workflowFile) {
      return (
        await json(
          `/repos/${repo}/actions/workflows/${workflowFile}/runs?event=workflow_dispatch&per_page=10`,
        )
      ).workflow_runs;
    },
    getRun: (runId) => json(`/repos/${repo}/actions/runs/${runId}`),
    async getJobs(runId) {
      return (await json(`/repos/${repo}/actions/runs/${runId}/jobs`)).jobs;
    },
    async getArtifacts(runId) {
      return (await json(`/repos/${repo}/actions/runs/${runId}/artifacts`))
        .artifacts;
    },
    /** Downloads the artifact zip; `onProgress(fraction)` fires as the bytes arrive. */
    async downloadArtifact(artifact, onProgress = () => {}) {
      const res = await request(artifact.archive_download_url);
      const total =
        Number(res.headers.get("content-length")) ||
        artifact.size_in_bytes ||
        0;
      const reader = res.body.getReader();
      const chunks = [];
      let received = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        received += value.length;
        if (total) onProgress(Math.min(1, received / total));
      }
      const bytes = new Uint8Array(received);
      let offset = 0;
      for (const chunk of chunks) {
        bytes.set(chunk, offset);
        offset += chunk.length;
      }
      onProgress(1);
      return bytes;
    },
  };
}

/**
 * Polls until the run ends. Either follows `runId`, or finds the run we dispatched at `dispatchedAtMs`
 * (the dispatch endpoint does not say which run it started).
 * `onUpdate({state, fraction}, run)` fires on every poll. Resolves with the finished run.
 */
export async function waitForRun({
  gh,
  workflowFile,
  runId,
  dispatchedAtMs,
  actor,
  onUpdate,
  signal,
  pollMs = POLL_MS,
}) {
  for (;;) {
    const run = runId
      ? await gh.getRun(runId)
      : pickRun(await gh.listRuns(workflowFile), { dispatchedAtMs, actor });
    const jobs =
      run && run.status !== "completed" ? await gh.getJobs(run.id) : [];
    const status = describeRun(run, jobs);
    onUpdate?.(status, run);
    if (status.state === "success") return run;
    if (status.state === "failed") {
      throw new Error(
        `La compilación terminó con «${status.conclusion}». Mira el log en ${run.html_url}`,
      );
    }
    await sleep(pollMs, signal);
  }
}

/** The firmware artifact of a finished run. */
export async function fetchRunArtifact(gh, runId) {
  const artifact = pickArtifact(await gh.getArtifacts(runId));
  if (!artifact) {
    throw new Error(
      "El run no tiene ningún artifact de firmware (¿ha caducado? GitHub los guarda 30 días).",
    );
  }
  return artifact;
}
