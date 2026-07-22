import {
  parseJson,
  validateJobSubmission,
  validateWorkerHeartbeat,
} from "./cluster_contract.mjs";

const publicJob = (row) => ({
  id: row.public_id,
  idempotencyKey: row.idempotency_key,
  kind: row.kind,
  priority: Number(row.priority),
  status: row.status,
  gitCommit: row.git_commit,
  gitRemote: row.git_remote,
  payload: parseJson(row.payload_json, {}),
  requirements: parseJson(row.requirements_json, {}),
  progress: parseJson(row.progress_json, {}),
  result: parseJson(row.result_json, null),
  workerId: row.worker_key ?? null,
  attemptCount: Number(row.attempt_count),
  maxAttempts: Number(row.max_attempts),
  errorMessage: row.error_message,
  createdAt: row.created_at,
  claimedAt: row.claimed_at,
  startedAt: row.started_at,
  finishedAt: row.finished_at,
  leaseExpiresAt: row.lease_expires_at,
});

const workerCanRun = (worker, job) => {
  const capabilities = new Set(parseJson(worker.capabilities_json, []));
  const labels = parseJson(worker.labels_json, {});
  const requirements = parseJson(job.requirements_json, {});
  return (
    (requirements.capabilities ?? []).every((item) => capabilities.has(item)) &&
    Object.entries(requirements.labels ?? {}).every(
      ([key, value]) => labels[key] === value,
    )
  );
};

export class ClusterRepository {
  constructor(pool) {
    this.pool = pool;
  }

  async enqueue(value) {
    const job = validateJobSubmission(value);
    try {
      await this.pool.execute(
        `INSERT INTO cluster_jobs
           (public_id, idempotency_key, kind, priority, git_commit, git_remote,
            payload_json, requirements_json, progress_json, requested_by, max_attempts)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, JSON_OBJECT(), ?, ?)`,
        [
          job.publicId,
          job.idempotencyKey,
          job.kind,
          job.priority,
          job.gitCommit,
          job.gitRemote,
          JSON.stringify(job.payload),
          JSON.stringify(job.requirements),
          job.requestedBy,
          job.maxAttempts,
        ],
      );
      await this.pool.execute(
        `INSERT INTO cluster_job_events (job_id, event_type, detail_json)
         SELECT id, 'queued', JSON_OBJECT('requested_by', ?) FROM cluster_jobs WHERE public_id = ?`,
        [job.requestedBy, job.publicId],
      );
      return { created: true, job: await this.getJob(job.publicId) };
    } catch (error) {
      if (error?.code !== "ER_DUP_ENTRY") {
        throw error;
      }
      const existing = await this.getJobByIdempotencyKey(job.idempotencyKey);
      return { created: false, job: existing };
    }
  }

  async heartbeatWorker(value) {
    const worker = validateWorkerHeartbeat(value);
    await this.pool.execute(
      `INSERT INTO cluster_workers
         (worker_key, display_name, hostname, platform, architecture, cpu_threads,
          memory_mb, status, resource_busy, labels_json, capabilities_json,
          progress_json, current_git_commit)
       VALUES (?, ?, ?, ?, ?, ?, ?, 'online', ?, ?, ?, ?, ?)
       ON DUPLICATE KEY UPDATE display_name = VALUES(display_name), hostname = VALUES(hostname),
         platform = VALUES(platform), architecture = VALUES(architecture),
         cpu_threads = VALUES(cpu_threads), memory_mb = VALUES(memory_mb),
         status = 'online', resource_busy = VALUES(resource_busy),
         labels_json = VALUES(labels_json), capabilities_json = VALUES(capabilities_json),
         progress_json = VALUES(progress_json), current_git_commit = VALUES(current_git_commit),
         last_seen_at = CURRENT_TIMESTAMP(3)`,
      [
        worker.workerKey,
        worker.displayName,
        worker.hostname,
        worker.platform,
        worker.architecture,
        worker.cpuThreads,
        worker.memoryMb,
        worker.resourceBusy,
        JSON.stringify(worker.labels),
        JSON.stringify(worker.capabilities),
        JSON.stringify(worker.progress),
        worker.currentGitCommit,
      ],
    );
    return worker;
  }

  async queuedEntries() {
    const [rows] = await this.pool.query(
      `SELECT public_id, priority, UNIX_TIMESTAMP(created_at) * 1000 AS created_ms
       FROM cluster_jobs WHERE status = 'queued' ORDER BY priority, created_at`,
    );
    return rows.map((row) => ({
      id: row.public_id,
      priority: Number(row.priority),
      createdMs: Number(row.created_ms),
    }));
  }

  async claim(publicId, workerKey, leaseSeconds = 300) {
    const connection = await this.pool.getConnection();
    try {
      await connection.beginTransaction();
      const [workers] = await connection.execute(
        "SELECT * FROM cluster_workers WHERE worker_key = ? FOR UPDATE",
        [workerKey],
      );
      const [jobs] = await connection.execute(
        "SELECT * FROM cluster_jobs WHERE public_id = ? FOR UPDATE",
        [publicId],
      );
      const worker = workers[0];
      const job = jobs[0];
      if (
        !worker ||
        !job ||
        job.status !== "queued" ||
        !workerCanRun(worker, job)
      ) {
        await connection.rollback();
        return null;
      }
      await connection.execute(
        `UPDATE cluster_jobs SET status = 'claimed', worker_id = ?, attempt_count = attempt_count + 1,
           claimed_at = CURRENT_TIMESTAMP(3), lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(3), INTERVAL ? SECOND),
           error_message = NULL WHERE id = ?`,
        [worker.id, leaseSeconds, job.id],
      );
      await connection.execute(
        `INSERT INTO cluster_job_events (job_id, worker_id, event_type, detail_json)
         VALUES (?, ?, 'claimed', JSON_OBJECT('lease_seconds', ?))`,
        [job.id, worker.id, leaseSeconds],
      );
      await connection.commit();
      return this.getJob(publicId);
    } catch (error) {
      await connection.rollback();
      throw error;
    } finally {
      connection.release();
    }
  }

  async heartbeatJob(publicId, workerKey, progress, leaseSeconds = 120) {
    const [result] = await this.pool.execute(
      `UPDATE cluster_jobs j JOIN cluster_workers w ON w.id = j.worker_id
       SET j.status = 'running', j.progress_json = ?,
           j.started_at = COALESCE(j.started_at, CURRENT_TIMESTAMP(3)),
           j.lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(3), INTERVAL ? SECOND),
           w.progress_json = ?, w.last_seen_at = CURRENT_TIMESTAMP(3), w.status = 'online'
       WHERE j.public_id = ? AND w.worker_key = ? AND j.status IN ('claimed', 'running')`,
      [
        JSON.stringify(progress ?? {}),
        leaseSeconds,
        JSON.stringify(progress ?? {}),
        publicId,
        workerKey,
      ],
    );
    if (result.affectedRows !== 1) {
      throw new Error("job lease is unavailable");
    }
    return this.getJob(publicId);
  }

  async finish(publicId, workerKey, outcome) {
    const connection = await this.pool.getConnection();
    try {
      await connection.beginTransaction();
      const [rows] = await connection.execute(
        `SELECT j.*, w.worker_key FROM cluster_jobs j
         JOIN cluster_workers w ON w.id = j.worker_id
         WHERE j.public_id = ? FOR UPDATE`,
        [publicId],
      );
      const job = rows[0];
      if (
        !job ||
        job.worker_key !== workerKey ||
        !["claimed", "running"].includes(job.status)
      ) {
        throw new Error("job lease is unavailable");
      }
      const shouldRetry =
        !outcome.completed && job.attempt_count < job.max_attempts;
      const status = outcome.completed
        ? "completed"
        : shouldRetry
          ? "queued"
          : "failed";
      await connection.execute(
        `UPDATE cluster_jobs SET status = ?, progress_json = ?, result_json = ?,
           error_message = ?, worker_id = IF(? = 'queued', NULL, worker_id),
           lease_expires_at = NULL, finished_at = IF(? IN ('completed', 'failed'), CURRENT_TIMESTAMP(3), NULL)
         WHERE id = ?`,
        [
          status,
          JSON.stringify(outcome.progress ?? {}),
          JSON.stringify(outcome.result ?? {}),
          outcome.errorMessage ?? null,
          status,
          status,
          job.id,
        ],
      );
      await connection.execute(
        `INSERT INTO cluster_job_events (job_id, worker_id, event_type, detail_json)
         VALUES (?, ?, ?, ?)`,
        [
          job.id,
          job.worker_id,
          status,
          JSON.stringify({ error: outcome.errorMessage ?? null }),
        ],
      );
      await connection.commit();
      return { requeued: shouldRetry, job: await this.getJob(publicId) };
    } catch (error) {
      await connection.rollback();
      throw error;
    } finally {
      connection.release();
    }
  }

  async cancel(publicId) {
    const [result] = await this.pool.execute(
      `UPDATE cluster_jobs SET status = 'cancelled', lease_expires_at = NULL,
         finished_at = CURRENT_TIMESTAMP(3)
       WHERE public_id = ? AND status IN ('queued', 'claimed', 'running')`,
      [publicId],
    );
    if (result.affectedRows !== 1) {
      throw new Error("job cannot be cancelled");
    }
    await this.pool.execute(
      `INSERT INTO cluster_job_events (job_id, worker_id, event_type, detail_json)
       SELECT id, worker_id, 'cancelled', JSON_OBJECT() FROM cluster_jobs WHERE public_id = ?`,
      [publicId],
    );
    return this.getJob(publicId);
  }

  async requeueExpired() {
    const [rows] = await this.pool.query(
      `SELECT id, public_id, worker_id FROM cluster_jobs
       WHERE status IN ('claimed', 'running') AND lease_expires_at < CURRENT_TIMESTAMP(3)`,
    );
    const requeued = [];
    for (const row of rows) {
      const [result] = await this.pool.execute(
        `UPDATE cluster_jobs SET
           status = IF(attempt_count < max_attempts, 'queued', 'failed'),
           worker_id = IF(attempt_count < max_attempts, NULL, worker_id),
           lease_expires_at = NULL,
           error_message = 'worker lease expired',
           finished_at = IF(attempt_count < max_attempts, NULL, CURRENT_TIMESTAMP(3))
         WHERE public_id = ? AND status IN ('claimed', 'running')
           AND lease_expires_at < CURRENT_TIMESTAMP(3)`,
        [row.public_id],
      );
      if (result.affectedRows === 1) {
        const job = await this.getJob(row.public_id);
        await this.pool.execute(
          `INSERT INTO cluster_job_events (job_id, worker_id, event_type, detail_json)
           VALUES (?, ?, ?, JSON_OBJECT('reason', 'worker lease expired'))`,
          [
            row.id,
            row.worker_id,
            job.status === "queued" ? "lease_requeued" : "lease_failed",
          ],
        );
        if (job.status === "queued") {
          requeued.push(job);
        }
      }
    }
    return requeued;
  }

  async getJob(publicId) {
    const [rows] = await this.pool.execute(
      `SELECT j.*, w.worker_key FROM cluster_jobs j
       LEFT JOIN cluster_workers w ON w.id = j.worker_id WHERE j.public_id = ?`,
      [publicId],
    );
    if (!rows[0]) {
      throw new Error("job was not found");
    }
    return publicJob(rows[0]);
  }

  async getJobByIdempotencyKey(idempotencyKey) {
    const [rows] = await this.pool.execute(
      `SELECT j.*, w.worker_key FROM cluster_jobs j
       LEFT JOIN cluster_workers w ON w.id = j.worker_id WHERE j.idempotency_key = ?`,
      [idempotencyKey],
    );
    return rows[0] ? publicJob(rows[0]) : null;
  }

  async state(limit = 100) {
    const boundedLimit = boundedInteger(limit, "limit", 1, 500);
    const [workers] = await this.pool.query(
      `SELECT worker_key, display_name, hostname, platform, architecture, cpu_threads,
         memory_mb, resource_busy, labels_json, capabilities_json, progress_json,
         current_git_commit, registered_at, last_seen_at,
         IF(last_seen_at < DATE_SUB(CURRENT_TIMESTAMP(3), INTERVAL 120 SECOND), 'offline', status) AS status
       FROM cluster_workers ORDER BY last_seen_at DESC`,
    );
    const [jobs] = await this.pool.query(
      `SELECT j.*, w.worker_key FROM cluster_jobs j LEFT JOIN cluster_workers w ON w.id = j.worker_id
       ORDER BY j.created_at DESC LIMIT ${boundedLimit}`,
    );
    return {
      workers: workers.map((row) => ({
        id: row.worker_key,
        displayName: row.display_name,
        hostname: row.hostname,
        platform: row.platform,
        architecture: row.architecture,
        cpuThreads: Number(row.cpu_threads),
        memoryMb: Number(row.memory_mb),
        status: row.status,
        resourceBusy: Boolean(row.resource_busy),
        labels: parseJson(row.labels_json, {}),
        capabilities: parseJson(row.capabilities_json, []),
        progress: parseJson(row.progress_json, {}),
        currentGitCommit: row.current_git_commit,
        registeredAt: row.registered_at,
        lastSeenAt: row.last_seen_at,
      })),
      jobs: jobs.map(publicJob),
    };
  }
}
