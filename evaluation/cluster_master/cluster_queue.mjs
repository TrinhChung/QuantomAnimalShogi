import { createClient } from "redis";

const queueKey = "qas:queue:shogi";
const defaultKokoroBusyKey = "qas:resource:kokoro:busy";

const queueScore = (priority, createdMs = Date.now()) =>
  Number(priority) * 10_000_000_000_000 + Number(createdMs);

export class RedisClusterQueue {
  constructor(client, options = {}) {
    this.client = client;
    this.kokoroBusyKey = options.kokoroBusyKey ?? defaultKokoroBusyKey;
    this.kokoroQueueKeys = options.kokoroQueueKeys ?? [];
  }

  async enqueue(entry) {
    await this.client.zAdd(queueKey, {
      score: queueScore(entry.priority, entry.createdMs),
      value: entry.id,
    });
  }

  async remove(publicId) {
    await this.client.zRem(queueKey, publicId);
  }

  async pop() {
    const entries = await this.client.zPopMin(queueKey, 1);
    return entries[0]?.value ?? null;
  }

  async recover(entries) {
    if (entries.length === 0) {
      return;
    }
    await this.client.zAdd(
      queueKey,
      entries.map((entry) => ({
        score: queueScore(entry.priority, entry.createdMs),
        value: entry.id,
      })),
    );
  }

  async isKokoroBusy() {
    if (await this.client.exists(this.kokoroBusyKey)) {
      return true;
    }
    for (const key of this.kokoroQueueKeys) {
      if (await this.client.exists(key)) {
        return true;
      }
    }
    return false;
  }

  async size() {
    return this.client.zCard(queueKey);
  }

  async ping() {
    return (await this.client.ping()) === "PONG";
  }

  async close() {
    if (this.client.isOpen) {
      await this.client.quit();
    }
  }
}

export const createRedisClusterQueueFromEnvironment = async (
  environment = process.env,
) => {
  const url = environment.QAS_REDIS_URL?.trim();
  if (!url) {
    return null;
  }
  const client = createClient({ url });
  client.on("error", (error) => {
    console.error(`Redis queue error: ${error.message}`);
  });
  await client.connect();
  const queue = new RedisClusterQueue(client, {
    kokoroBusyKey:
      environment.QAS_KOKORO_BUSY_KEY?.trim() || defaultKokoroBusyKey,
    kokoroQueueKeys: String(environment.QAS_KOKORO_QUEUE_KEYS ?? "")
      .split(",")
      .map((item) => item.trim())
      .filter(Boolean),
  });
  await queue.ping();
  return queue;
};

export class ClusterCoordinator {
  constructor(repository, queue) {
    this.repository = repository;
    this.queue = queue;
    this.isHydrated = false;
  }

  async hydrate() {
    const expired = await this.repository.requeueExpired();
    const queued = await this.repository.queuedEntries();
    await this.queue.recover([
      ...queued,
      ...expired.map((job) => ({
        id: job.id,
        priority: job.priority,
        createdMs: new Date(job.createdAt).getTime(),
      })),
    ]);
    this.isHydrated = true;
  }

  async enqueue(value) {
    const result = await this.repository.enqueue(value);
    if (result.job.status === "queued") {
      await this.queue.enqueue({
        id: result.job.id,
        priority: result.job.priority,
        createdMs: new Date(result.job.createdAt).getTime(),
      });
    }
    return result;
  }

  async heartbeatWorker(value) {
    return this.repository.heartbeatWorker(value);
  }

  async claim(workerValue) {
    const worker = await this.repository.heartbeatWorker(workerValue);
    if (!this.isHydrated) {
      await this.hydrate();
    }
    const sharesKokoroHost = worker.labels.shared_with_kokoro === "true";
    if (
      worker.resourceBusy ||
      (sharesKokoroHost && (await this.queue.isKokoroBusy()))
    ) {
      return { job: null, waitReason: "kokoro_priority" };
    }
    const deferred = [];
    for (let attempt = 0; attempt < 25; attempt += 1) {
      const publicId = await this.queue.pop();
      if (!publicId) {
        await this.queue.recover(deferred);
        return {
          job: null,
          waitReason: deferred.length ? "requirements_mismatch" : "queue_empty",
        };
      }
      const job = await this.repository.claim(publicId, worker.workerKey);
      if (job) {
        await this.queue.recover(deferred);
        return { job, waitReason: null };
      }
      const queuedJob = await this.repository.getJob(publicId);
      if (queuedJob.status === "queued") {
        deferred.push({
          id: publicId,
          priority: queuedJob.priority,
          createdMs: new Date(queuedJob.createdAt).getTime(),
        });
      }
    }
    await this.queue.recover(deferred);
    return { job: null, waitReason: "requirements_mismatch" };
  }

  async heartbeatJob(publicId, workerId, progress) {
    return this.repository.heartbeatJob(publicId, workerId, progress);
  }

  async finish(publicId, workerId, outcome) {
    const result = await this.repository.finish(publicId, workerId, outcome);
    if (result.requeued) {
      await this.queue.enqueue({
        id: result.job.id,
        priority: result.job.priority,
        createdMs: new Date(result.job.createdAt).getTime(),
      });
    }
    return result;
  }

  async cancel(publicId) {
    await this.queue.remove(publicId);
    return this.repository.cancel(publicId);
  }

  async state(limit) {
    const state = await this.repository.state(limit);
    return {
      ...state,
      queue: {
        connected: await this.queue.ping(),
        depth: await this.queue.size(),
        kokoroBusy: await this.queue.isKokoroBusy(),
      },
    };
  }

  async metrics() {
    const state = await this.state(500);
    const jobCounts = new Map();
    for (const job of state.jobs) {
      const key = `${job.kind}:${job.status}`;
      jobCounts.set(key, (jobCounts.get(key) ?? 0) + 1);
    }
    const lines = [
      "# HELP qas_cluster_queue_depth Number of Shogi jobs waiting in Redis.",
      "# TYPE qas_cluster_queue_depth gauge",
      `qas_cluster_queue_depth ${state.queue.depth}`,
      "# HELP qas_cluster_kokoro_busy Whether Kokoro currently owns shared master capacity.",
      "# TYPE qas_cluster_kokoro_busy gauge",
      `qas_cluster_kokoro_busy ${state.queue.kokoroBusy ? 1 : 0}`,
      "# HELP qas_cluster_workers Number of registered workers by status.",
      "# TYPE qas_cluster_workers gauge",
    ];
    for (const status of ["online", "offline"]) {
      lines.push(
        `qas_cluster_workers{status="${status}"} ${state.workers.filter((worker) => worker.status === status).length}`,
      );
    }
    lines.push(
      "# HELP qas_cluster_jobs Number of recent jobs by kind and status.",
      "# TYPE qas_cluster_jobs gauge",
    );
    for (const [key, count] of jobCounts) {
      const [kind, status] = key.split(":");
      lines.push(
        `qas_cluster_jobs{kind="${kind}",status="${status}"} ${count}`,
      );
    }
    return `${lines.join("\n")}\n`;
  }
}
