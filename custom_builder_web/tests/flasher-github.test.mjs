// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import {
  fetchRunArtifact,
  githubClient,
  waitForRun,
} from "../flasher-github.js";

const DISPATCHED = Date.parse("2026-11-08T10:00:00Z");
const run = (over = {}) => ({
  id: 7,
  event: "workflow_dispatch",
  created_at: "2026-11-08T10:00:02Z",
  actor: { login: "me" },
  html_url: "https://example.test/run/7",
  status: "in_progress",
  ...over,
});
const steps = (done, total) => ({
  steps: Array.from({ length: total }, (_, i) => ({
    status: i < done ? "completed" : "pending",
  })),
});

/** A scripted GitHub: each poll of listRuns/getRun returns the next snapshot. */
function fakeGh(snapshots) {
  let poll = 0;
  const current = () => snapshots[Math.min(poll++, snapshots.length - 1)];
  let last;
  return {
    listRuns: async () => {
      last = current();
      return last.run ? [last.run] : [];
    },
    getRun: async () => (last = current()).run,
    getJobs: async () => [last.jobs ?? steps(0, 0)],
  };
}

test("waitForRun: follows the dispatched run through queued, running and success", async () => {
  const seen = [];
  const gh = fakeGh([
    { run: null }, // the run does not exist yet
    { run: run({ status: "queued" }) },
    { run: run(), jobs: steps(2, 4) },
    { run: run({ status: "completed", conclusion: "success" }) },
  ]);
  const finished = await waitForRun({
    gh,
    workflowFile: "custom_builder.yml",
    dispatchedAtMs: DISPATCHED,
    actor: "me",
    pollMs: 1,
    onUpdate: (status) => seen.push([status.state, status.fraction]),
  });
  assert.equal(finished.status, "completed");
  assert.deepEqual(seen, [
    ["queued", 0],
    ["queued", 0],
    ["running", 0.5],
    ["success", 1],
  ]);
});

test("waitForRun: a failed build rejects with the conclusion and the log URL", async () => {
  const gh = fakeGh([
    { run: run({ status: "completed", conclusion: "failure" }) },
  ]);
  await assert.rejects(
    waitForRun({
      gh,
      workflowFile: "w.yml",
      dispatchedAtMs: DISPATCHED,
      pollMs: 1,
    }),
    /«failure».*example\.test\/run\/7/,
  );
});

test("waitForRun: with a runId it follows that run and ignores the dispatch time", async () => {
  const gh = fakeGh([
    {
      run: run({
        status: "completed",
        conclusion: "success",
        created_at: "2020-01-01T00:00:00Z",
      }),
    },
  ]);
  const finished = await waitForRun({ gh, runId: 7, pollMs: 1 });
  assert.equal(finished.conclusion, "success");
});

test("waitForRun: can be cancelled while it waits for the next poll", async () => {
  const controller = new AbortController();
  const gh = fakeGh([{ run: null }]);
  const pending = waitForRun({
    gh,
    workflowFile: "w.yml",
    dispatchedAtMs: DISPATCHED,
    pollMs: 60_000,
    signal: controller.signal,
  });
  setTimeout(() => controller.abort(), 5);
  await assert.rejects(pending, { name: "AbortError" });
});

test("fetchRunArtifact: explains an expired or missing artifact", async () => {
  await assert.rejects(
    fetchRunArtifact(
      { getArtifacts: async () => [{ name: "a.zip.enc", expired: true }] },
      7,
    ),
    /caducado/,
  );
  const artifact = { name: "a.zip.enc", expired: false };
  assert.equal(
    await fetchRunArtifact({ getArtifacts: async () => [artifact] }, 7),
    artifact,
  );
});

test("client: sends the token, and HTTP errors say what to check", async () => {
  const calls = [];
  const fetchFn = async (url, init) => {
    calls.push({ url, auth: init.headers.Authorization });
    return {
      ok: false,
      status: 403,
      statusText: "Forbidden",
      json: async () => ({ message: "Resource not accessible" }),
    };
  };
  const gh = githubClient({ token: "t0k", repo: "o/r", fetchFn });
  await assert.rejects(
    gh.getRun(1),
    /403.*Resource not accessible.*lectura de Actions/,
  );
  assert.equal(calls[0].url, "https://api.github.com/repos/o/r/actions/runs/1");
  assert.equal(calls[0].auth, "Bearer t0k");
});

test("client: the artifact download reports progress and returns every byte", async () => {
  const parts = [
    new Uint8Array([1, 2]),
    new Uint8Array([3]),
    new Uint8Array([4, 5]),
  ];
  const fetchFn = async () => ({
    ok: true,
    headers: { get: (h) => (h === "content-length" ? "5" : null) },
    body: new ReadableStream({
      start(c) {
        parts.forEach((p) => c.enqueue(p));
        c.close();
      },
    }),
  });
  const gh = githubClient({ token: "t", repo: "o/r", fetchFn });
  const fractions = [];
  const bytes = await gh.downloadArtifact(
    { archive_download_url: "https://x.test/zip" },
    (f) => fractions.push(f),
  );
  assert.deepEqual([...bytes], [1, 2, 3, 4, 5]);
  assert.deepEqual(fractions, [0.4, 0.6, 1, 1]);
});

test("waitForRun: an already aborted signal stops the loop at once", async () => {
  const controller = new AbortController();
  controller.abort();
  const gh = { listRuns: async () => [], getJobs: async () => [] };
  await assert.rejects(
    waitForRun({ gh, workflowFile: "x.yml", dispatchedAtMs: 0, signal: controller.signal, pollMs: 1 }),
    { name: "AbortError" },
  );
});
