# Distributed Evaluation Cluster

## Purpose and boundaries

The cluster runs reproducible benchmark and paired-tournament workloads on spare machines. It is
external orchestration under `evaluation/` and the web application boundary. It does not move game
rules, search, or protocol logic out of their owning C++ modules.

```text
Remote browser
      |
      v
phuong:8331 / Nginx
      |
      +--> QAS cluster API + static dashboard (127.0.0.1:8766)
      |          |                         |
      |          v                         v
      |      MySQL 8                   Redis
      |   durable state             priority queue
      |
      +--> Grafana (127.0.0.1:3200) <-- Prometheus <-- node_exporter / QAS metrics

Workers (phuong spare, Windows local, other SSH hosts)
      |   HTTPS/API heartbeat, lease, result
      +-------------------------------------> master
      |
      +--> Git fetch of the exact job commit --> build/<version> --> evaluation artifacts
```

MySQL is authoritative for jobs, attempts, workers, progress, results, and the event audit trail.
Redis stores only queue order and fast resource signals. At master restart, every queued MySQL job
is restored to Redis. A claimed job has a 90-second lease; if heartbeats stop, it is returned to the
queue until `max_attempts` is reached.

The API never accepts an arbitrary command line or executable path. A job is one of
`git_update`, `benchmark`, `tournament`, or `evaluation`, with an allowlisted profile and an exact
40-character Git commit. Workers refuse a dirty checkout before changing commits.

## Kokoro priority on `phuong`

Kokoro TTS, MySQL, Redis, Nginx, SSH, Prometheus, Grafana, `node_exporter`, Docker, and containerd
are protected workloads. The resource audit script refuses to stop their known service/container
names.

The `phuong` worker is limited to one CPU and 2304 MB, runs child work at low OS priority, and is
labelled `shared_with_kokoro=true`. It does not claim a new Shogi job when either condition is true:

- Redis contains `qas:resource:kokoro:busy` or any configured Kokoro queue key.
- Kokoro's Docker CPU usage is at or above `QAS_KOKORO_CPU_THRESHOLD` (5% by default).

The CPU probe makes the policy useful without modifying the existing Kokoro web API. For exact
request-boundary priority, the Kokoro producer may set `qas:resource:kokoro:busy` with a short TTL
while a request is active. The Shogi process stays at low priority if a Kokoro request arrives after
the Shogi job started. Other slave machines do not pause because they do not share Kokoro's host.

## Deploy the master

The deployment uses Docker because `phuong` currently has host Node 18 and no CMake. It does not
replace or restart the existing Kokoro, MySQL, or Redis services.

1. Commit and push the cluster change. Deployment refuses a commit that is not on `origin`.
2. From the repository root, deploy the exact commit:

   ```powershell
   scripts\deploy_cluster_master.bat -SshHost phuong -Commit <40-character-commit>
   ```

3. The first deployment creates the `quantum_animal_shogi` database, least-privilege application
   and backup users, `/etc/qas/*.env` secrets, QAS master/worker containers, Prometheus, Grafana,
   and the Nginx listener on port 8331.
4. Open `http://37.44.244.139:8331/` and enter the bearer token from `/etc/qas/cluster.env`.
   Grafana is under `/grafana/`; its generated administrator password is in
   `/etc/qas/grafana.env`.

Place TLS or a VPN in front of port 8331 before treating it as an Internet-facing control plane.
MySQL and Redis must remain bound to localhost/private networking; workers communicate through the
authenticated master API, not by opening those database ports.

## Run a Windows slave

Use a dedicated worker workspace. The worker creates it automatically and never switches the
developer checkout.

```powershell
$env:QAS_CLUSTER_URL = 'http://37.44.244.139:8331'
$env:QAS_CLUSTER_TOKEN = '<token from phuong>'
$env:QAS_WORKER_WORKSPACE = 'D:\QASCluster\workspace'
$env:QAS_WORKER_ID = 'windows-local-daytime'
$env:QAS_WORKER_NAME = 'Windows local daytime'
$env:QAS_WORKER_LABELS = 'os=windows,role=daytime-slave'
scripts\cluster_worker.bat
```

The same command works on another Windows server. On Linux, run
`python3 -m evaluation.cluster_worker` from a bootstrap checkout or use the worker container. To
make this PC a worker only while idle, persist the URL/token as user environment variables and
register the supplied low-priority task:

```powershell
[Environment]::SetEnvironmentVariable('QAS_CLUSTER_URL', 'http://37.44.244.139:8331', 'User')
[Environment]::SetEnvironmentVariable('QAS_CLUSTER_TOKEN', '<token from phuong>', 'User')
scripts\register_cluster_worker_task.bat -IdleMinutes 10
```

The task stops when keyboard/mouse activity resumes. The lease returns its interrupted job to the
queue, so no manual recovery is required.

## Queue work from batch files

All commands use `QAS_CLUSTER_URL` and `QAS_CLUSTER_TOKEN`.

```powershell
# Benchmark the local HEAD on one worker.
scripts\cluster_benchmark.bat fixed_depth_quick --candidate-name "Stage 5.1"

# Paired, side-swapped tournament against Stage 5 Clean rebuilt on the same worker.
scripts\cluster_tournament.bat strength_quick --candidate-name "Stage 5.1"

# Inspect workers, Redis state, leases, and recent jobs.
scripts\cluster_status.bat

# Safe local source update: clean tree + fetch + fast-forward only.
scripts\update_cluster_git.bat
scripts\update_cluster_git.bat -Commit <40-character-commit>
```

Benchmark jobs use the native Stage 3.5 benchmark harness. Tournament jobs build both Git commits
on the same machine, use the native referee, play paired side-swapped openings, and store the
statistical summary in MySQL. Full permanent `evaluation` jobs currently require a Windows worker
because the frozen accepted artifacts in `evaluation/versions/` are Windows executables.

## Local MySQL copy

The central database is the source of truth. A local audit copy is refreshed over SSH without
exposing MySQL publicly. The target database is replaced, so use a dedicated replica name.

```powershell
$env:QAS_LOCAL_DB_USER = 'local_replica_admin'
$env:QAS_LOCAL_DB_PASSWORD = '<local password>'
scripts\sync_cluster_database.bat -SshHost phuong -LocalDatabase quantum_animal_shogi_replica -Confirm
```

The script obtains a consistent `mysqldump` through SSH, verifies that it is non-empty, imports it
into the explicitly named local database, then removes the temporary dump. Schedule this command
when the local PC is online. It is deliberately one-way: local edits never flow back to the master.

## Resource audit and recoverable stops

Read-only audit:

```powershell
scripts\audit_master.bat -SshHost phuong
```

An explicit stop remains recoverable and requires PowerShell confirmation:

```powershell
scripts\audit_master.bat -SshHost phuong -StopPm2Application Template1 -Confirm
```

The script never disables or removes a service. It also refuses protected names, including the
Kokoro container and the monitoring/data/control-plane services.
