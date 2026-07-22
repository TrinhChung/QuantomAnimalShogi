import assert from "node:assert/strict";
import test from "node:test";

import { ClusterCoordinator, RedisClusterQueue } from "./cluster_queue.mjs";
import {
  validateJobSubmission,
  validateWorkerHeartbeat,
} from "./cluster_contract.mjs";
import { clusterTokenFromEnvironment } from "./cluster_routes.mjs";
import { ClusterRepository } from "./cluster_repository.mjs";
import { databaseConfigFromEnvironment } from "./database.mjs";

const commit = "5e096227947cf53c760a027318e26183863483f3";

const benchmarkJob = () => ({
  kind: "benchmark",
  gitCommit: commit,
  payload: {
    profile: "fixed_depth_quick",
    candidateName: "Candidate",
    changeCategory: "performance_only",
    opponents: [],
  },
});

const workerHeartbeat = (overrides = {}) => ({
  workerId: "worker-one",
  hostname: "worker-one",
  platform: "linux",
  architecture: "x64",
  cpuThreads: 4,
  memoryMb: 8192,
  labels: { shared_with_kokoro: "true" },
  capabilities: ["benchmark", "tournament"],
  ...overrides,
});

test("job validation accepts typed benchmark metadata", () => {
  const job = validateJobSubmission(benchmarkJob());
  assert.equal(job.kind, "benchmark");
  assert.equal(job.gitCommit, commit);
  assert.equal(job.payload.profile, "fixed_depth_quick");
  assert.deepEqual(job.requirements.capabilities, ["benchmark"]);
});

test("job validation rejects arbitrary commands and incompatible profiles", () => {
  assert.throws(
    () =>
      validateJobSubmission({
        ...benchmarkJob(),
        kind: "shell",
        command: "whoami",
      }),
    /kind is invalid/,
  );
  const invalidProfile = benchmarkJob();
  invalidProfile.payload.profile = "strength_candidate";
  assert.throws(
    () => validateJobSubmission(invalidProfile),
    /profile is not allowed/,
  );
  assert.throws(
    () => validateJobSubmission({ ...benchmarkJob(), gitCommit: "main" }),
    /gitCommit is invalid/,
  );
});

test("tournament validation binds a portable opponent commit", () => {
  const job = validateJobSubmission({
    ...benchmarkJob(),
    kind: "tournament",
    payload: {
      ...benchmarkJob().payload,
      profile: "strength_quick",
      opponentGitCommit: commit,
      opponentName: "stage5-clean",
    },
  });
  assert.equal(job.payload.opponentGitCommit, commit);
  assert.equal(job.payload.opponentName, "stage5-clean");
});

test("worker heartbeat records the Kokoro resource gate", () => {
  const worker = validateWorkerHeartbeat(
    workerHeartbeat({ resourceBusy: true }),
  );
  assert.equal(worker.resourceBusy, true);
  assert.equal(worker.labels.shared_with_kokoro, "true");
  assert(worker.capabilities.includes("benchmark"));
});

test("cluster token requires a non-trivial secret", () => {
  assert.equal(clusterTokenFromEnvironment({}), null);
  assert.throws(
    () => clusterTokenFromEnvironment({ QAS_CLUSTER_TOKEN: "short" }),
    /between 32 and 512/,
  );
  assert.equal(
    clusterTokenFromEnvironment({ QAS_CLUSTER_TOKEN: "x".repeat(32) }),
    "x".repeat(32),
  );
});

test("master database configuration requires an explicit central database", () => {
  assert.throws(
    () => databaseConfigFromEnvironment({}),
    /QAS_DB_NAME is required/,
  );
  assert.deepEqual(
    databaseConfigFromEnvironment({
      QAS_DB_HOST: "127.0.0.1",
      QAS_DB_PORT: "3306",
      QAS_DB_USER: "qas_cluster",
      QAS_DB_PASSWORD: "secret",
      QAS_DB_NAME: "quantum_animal_shogi",
    }),
    {
      host: "127.0.0.1",
      port: 3306,
      user: "qas_cluster",
      password: "secret",
      database: "quantum_animal_shogi",
    },
  );
});

test("coordinator defers a shared worker while Kokoro is busy", async () => {
  let popCount = 0;
  const repository = {
    heartbeatWorker: async (value) => validateWorkerHeartbeat(value),
  };
  const queue = {
    isKokoroBusy: async () => true,
    pop: async () => {
      popCount += 1;
      return "job";
    },
  };
  const coordinator = new ClusterCoordinator(repository, queue);
  coordinator.isHydrated = true;
  const result = await coordinator.claim(workerHeartbeat());
  assert.equal(result.job, null);
  assert.equal(result.waitReason, "kokoro_priority");
  assert.equal(popCount, 0);
});

test("coordinator restores durable queued jobs into Redis", async () => {
  const recovered = [];
  const repository = {
    requeueExpired: async () => [],
    queuedEntries: async () => [{ id: "job-one", priority: 100, createdMs: 7 }],
  };
  const queue = {
    recover: async (entries) => recovered.push(...entries),
  };
  const coordinator = new ClusterCoordinator(repository, queue);
  await coordinator.hydrate();
  assert.equal(coordinator.isHydrated, true);
  assert.deepEqual(recovered, [{ id: "job-one", priority: 100, createdMs: 7 }]);
});

test("an empty Redis sorted set returns no job", async () => {
  const queue = new RedisClusterQueue({ zPopMin: async () => null });
  assert.equal(await queue.pop(), null);
});

test("Redis pop returns the job identifier from the current client shape", async () => {
  const queue = new RedisClusterQueue({
    zPopMin: async () => ({ value: "job-one", score: 1 }),
  });
  assert.equal(await queue.pop(), "job-one");
});

test("repository state accepts a bounded result limit", async () => {
  const repository = new ClusterRepository({ query: async () => [[]] });
  assert.deepEqual(await repository.state(100), { workers: [], jobs: [] });
});

test("an incompatible head job does not block eligible work behind it", async () => {
  const recovered = [];
  const pending = ["windows-only", "portable"];
  const repository = {
    heartbeatWorker: async (value) => validateWorkerHeartbeat(value),
    claim: async (publicId) =>
      publicId === "portable" ? { id: publicId, kind: "benchmark" } : null,
    getJob: async () => ({
      id: "windows-only",
      status: "queued",
      priority: 100,
      createdAt: "2026-07-23T00:00:00Z",
    }),
  };
  const queue = {
    isKokoroBusy: async () => false,
    pop: async () => pending.shift() ?? null,
    recover: async (entries) => recovered.push(...entries),
  };
  const coordinator = new ClusterCoordinator(repository, queue);
  coordinator.isHydrated = true;
  const result = await coordinator.claim(
    workerHeartbeat({ labels: { shared_with_kokoro: "false" } }),
  );
  assert.equal(result.job.id, "portable");
  assert.equal(recovered[0].id, "windows-only");
});
