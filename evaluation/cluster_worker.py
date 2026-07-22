from __future__ import annotations

import argparse
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from .cluster_worker_support import (
    ApiClient,
    BuildArtifacts,
    GitWorkspace,
    WorkerError,
    _build_workspace,
    _json_file,
    _kokoro_busy,
    _labels,
    _memory_mb,
    _sha256,
    _terminate_process,
)

DEFAULT_REMOTE_URL = "https://github.com/TrinhChung/QuantomAnimalShogi.git"
DEFAULT_CAPABILITIES = "git_update,benchmark,tournament,evaluation"

class ClusterWorker:
    def __init__(self, arguments: argparse.Namespace) -> None:
        self.arguments = arguments
        self.api = ApiClient(arguments.master_url, arguments.token)
        self.workspace = GitWorkspace(arguments.workspace, arguments.remote_url)
        self.labels = _labels(arguments.labels)
        self.capabilities = [item for item in arguments.capabilities.split(",") if item]

    def heartbeat(self, progress: dict[str, Any] | None = None) -> dict[str, Any]:
        resource_busy = _kokoro_busy(
            self.arguments.kokoro_container,
            self.arguments.kokoro_cpu_threshold,
        )
        return {
            "workerId": self.arguments.worker_id,
            "displayName": self.arguments.display_name,
            "hostname": socket.gethostname(),
            "platform": platform.system().lower(),
            "architecture": platform.machine().lower(),
            "cpuThreads": os.cpu_count() or 1,
            "memoryMb": _memory_mb(),
            "resourceBusy": resource_busy,
            "labels": self.labels,
            "capabilities": self.capabilities,
            "progress": progress or {},
            "currentGitCommit": self.workspace.current_commit(),
        }

    def claim(self) -> dict[str, Any] | None:
        result = self.api.request("POST", "/api/cluster/jobs/claim", self.heartbeat())
        if result.get("job") is None:
            print(f"waiting: {result.get('waitReason', 'queue_empty')}", flush=True)
            return None
        return result["job"]

    def _progress(self, job: dict[str, Any], stage: str, **detail: Any) -> None:
        self.api.request(
            "POST",
            f"/api/cluster/jobs/{job['id']}/heartbeat",
            {
                "workerId": self.arguments.worker_id,
                "progress": {"stage": stage, **detail},
            },
        )

    def _run_process(self, job: dict[str, Any], command: list[str], directory: Path) -> Path:
        log_directory = directory / "local_reports" / "cluster" / job["id"]
        log_directory.mkdir(parents=True, exist_ok=True)
        log_path = log_directory / "worker.log"
        effective = command
        creation_flags = 0
        if os.name == "nt":
            creation_flags = subprocess.BELOW_NORMAL_PRIORITY_CLASS
        elif shutil.which("nice"):
            effective = ["nice", "-n", "15", *command]
        with log_path.open("w", encoding="utf-8", newline="\n") as log:
            process = subprocess.Popen(
                effective,
                cwd=directory,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                creationflags=creation_flags,
                start_new_session=os.name != "nt",
            )
            started = time.monotonic()
            try:
                while process.poll() is None:
                    elapsed = round(time.monotonic() - started, 1)
                    self._progress(job, "running", elapsedSeconds=elapsed)
                    time.sleep(self.arguments.heartbeat_seconds)
            finally:
                if process.poll() is None:
                    _terminate_process(process)
        if process.returncode != 0:
            tail = log_path.read_text(encoding="utf-8", errors="replace")[-8_000:]
            raise WorkerError(f"job process failed ({process.returncode})\n{tail}")
        return log_path

    def _run_benchmark(self, job: dict[str, Any], artifacts: BuildArtifacts) -> dict[str, Any]:
        profile = job["payload"]["profile"]
        suite_arguments = {
            "infrastructure_smoke": ["--suite", "smoke", "--repeats", "3"],
            "correctness_regression": ["--suite", "fixed", "--depths", "4,5", "--repeats", "3"],
            "fixed_depth_quick": ["--suite", "fixed", "--depths", "5,6", "--repeats", "5"],
            "fixed_depth_deep": ["--suite", "fixed", "--depths", "7,8,9", "--repeats", "7"],
            "fixed_time_quick": ["--suite", "iterative", "--time-limits-ms", "250,1000", "--repeats", "3"],
            "fixed_time_contest": ["--suite", "iterative", "--time-limits-ms", "3000,5000,25000", "--repeats", "3"],
            "diagnostic_telemetry": ["--suite", "fixed", "--depths", "6,7", "--repeats", "3"],
        }[profile]
        log = self._run_process(
            job,
            [str(artifacts.stage35_benchmark), *suite_arguments],
            self.workspace.path,
        )
        return {"profile": profile, "logSha256": _sha256(log), "logTail": log.read_text(encoding="utf-8")[-100_000:]}

    def _opponent_workspace(self, commit: str) -> GitWorkspace:
        path = self.arguments.workspace.parent / "opponents" / commit
        workspace = GitWorkspace(path, self.arguments.remote_url)
        workspace.prepare(commit)
        return workspace

    def _run_tournament(self, job: dict[str, Any], candidate: BuildArtifacts) -> dict[str, Any]:
        payload = job["payload"]
        self._progress(job, "building_opponent", commit=payload["opponentGitCommit"])
        opponent_workspace = self._opponent_workspace(payload["opponentGitCommit"])
        opponent = _build_workspace(
            opponent_workspace,
            self.arguments.maximum_cores,
            heartbeat=lambda: self._progress(
                job,
                "building_opponent",
                commit=payload["opponentGitCommit"],
            ),
        )
        output = self.workspace.path / "local_reports" / "cluster" / job["id"] / "tournament"
        command = [
            sys.executable,
            "-m",
            "evaluation.tools.run_cluster_tournament",
            "--candidate-id",
            payload.get("candidateVersionId") or "candidate",
            "--candidate-exe",
            str(candidate.executable),
            "--candidate-config",
            str(self.workspace.path / "engine_config.json"),
            "--candidate-benchmark",
            str(candidate.benchmark),
            "--candidate-commit",
            job["gitCommit"],
            "--opponent-id",
            payload["opponentName"],
            "--opponent-exe",
            str(opponent.executable),
            "--opponent-config",
            str(opponent_workspace.path / "engine_config.json"),
            "--opponent-benchmark",
            str(opponent.benchmark),
            "--opponent-commit",
            payload["opponentGitCommit"],
            "--referee",
            str(candidate.referee),
            "--profile",
            payload["profile"],
            "--seed",
            str(payload["seed"]),
            "--output-dir",
            str(output),
        ]
        log = self._run_process(job, command, self.workspace.path)
        summary = _json_file(output / "summary.json")
        return {"summary": summary, "logSha256": _sha256(log)}

    def _run_evaluation(self, job: dict[str, Any], artifacts: BuildArtifacts) -> dict[str, Any]:
        if os.name != "nt":
            raise WorkerError("full evaluation jobs currently require a Windows worker")
        payload = job["payload"]
        output = self.workspace.path / "local_reports" / "cluster" / job["id"] / "evaluation"
        command = [
            sys.executable,
            "-m",
            "evaluation.tools.run_pipeline",
            "--candidate-exe",
            str(artifacts.executable),
            "--candidate-name",
            payload["candidateName"],
            "--change-category",
            payload["changeCategory"],
            "--profile",
            payload["profile"],
            "--output-dir",
            str(output),
            "--build-dir",
            str(self.workspace.path / "build" / "cluster-worker"),
        ]
        if payload["candidateVersionId"]:
            command.extend(["--candidate-version-id", payload["candidateVersionId"]])
        if payload["opponents"]:
            command.extend(["--opponents", ",".join(payload["opponents"])])
        log = self._run_process(job, command, self.workspace.path)
        reports = sorted(output.glob("*/report.md"), key=lambda path: path.stat().st_mtime)
        report = reports[-1] if reports else None
        manifest = _json_file(report.parent / "manifest.json") if report else None
        progress = _json_file(report.parent / "progress.json") if report else None
        return {
            "manifest": manifest,
            "progress": progress,
            "reportSha256": _sha256(report) if report else None,
            "report": report.read_text(encoding="utf-8")[-500_000:] if report else None,
            "logSha256": _sha256(log),
        }

    def execute(self, job: dict[str, Any]) -> dict[str, Any]:
        self._progress(job, "updating_git", commit=job["gitCommit"])
        self.workspace.prepare(job["gitCommit"], job.get("gitRemote", "origin"))
        if job["kind"] == "git_update":
            return {"gitCommit": self.workspace.current_commit()}
        self._progress(job, "building_candidate", commit=job["gitCommit"])
        artifacts = _build_workspace(
            self.workspace,
            self.arguments.maximum_cores,
            heartbeat=lambda: self._progress(
                job,
                "building_candidate",
                commit=job["gitCommit"],
            ),
        )
        if job["kind"] == "benchmark":
            return self._run_benchmark(job, artifacts)
        if job["kind"] == "tournament":
            return self._run_tournament(job, artifacts)
        if job["kind"] == "evaluation":
            return self._run_evaluation(job, artifacts)
        raise WorkerError(f"unsupported job kind: {job['kind']}")

    def run_once(self) -> bool:
        job = self.claim()
        if job is None:
            return False
        print(f"claimed {job['id']} ({job['kind']}) at {job['gitCommit'][:12]}", flush=True)
        try:
            result = self.execute(job)
            self.api.request(
                "POST",
                f"/api/cluster/jobs/{job['id']}/complete",
                {
                    "workerId": self.arguments.worker_id,
                    "progress": {"stage": "complete"},
                    "result": result,
                },
            )
            print(f"completed {job['id']}", flush=True)
        except Exception as error:
            try:
                self.api.request(
                    "POST",
                    f"/api/cluster/jobs/{job['id']}/fail",
                    {
                        "workerId": self.arguments.worker_id,
                        "progress": {"stage": "failed"},
                        "errorMessage": str(error)[-8_000:],
                    },
                )
            except WorkerError as reporting_error:
                print(f"could not report failure: {reporting_error}", file=sys.stderr, flush=True)
            print(f"failed {job['id']}: {error}", file=sys.stderr, flush=True)
        return True


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Quantum Animal Shogi cluster worker")
    parser.add_argument("--master-url", default=os.environ.get("QAS_CLUSTER_URL"))
    parser.add_argument("--token", default=os.environ.get("QAS_CLUSTER_TOKEN"))
    parser.add_argument(
        "--workspace",
        type=Path,
        default=Path(os.environ.get("QAS_WORKER_WORKSPACE", Path.home() / "qas-cluster" / "workspace")),
    )
    parser.add_argument("--remote-url", default=os.environ.get("QAS_GIT_REMOTE_URL", DEFAULT_REMOTE_URL))
    parser.add_argument("--worker-id", default=os.environ.get("QAS_WORKER_ID", socket.gethostname().lower()))
    parser.add_argument("--display-name", default=os.environ.get("QAS_WORKER_NAME", socket.gethostname()))
    parser.add_argument("--labels", default=os.environ.get("QAS_WORKER_LABELS", ""))
    parser.add_argument("--capabilities", default=os.environ.get("QAS_WORKER_CAPABILITIES", DEFAULT_CAPABILITIES))
    parser.add_argument("--kokoro-container", default=os.environ.get("QAS_KOKORO_CONTAINER"))
    parser.add_argument(
        "--kokoro-cpu-threshold",
        type=float,
        default=float(os.environ.get("QAS_KOKORO_CPU_THRESHOLD", "5")),
    )
    parser.add_argument(
        "--maximum-cores",
        type=int,
        default=int(os.environ.get("QAS_WORKER_MAX_CORES", "1")),
    )
    parser.add_argument("--heartbeat-seconds", type=int, default=15)
    parser.add_argument("--poll-seconds", type=int, default=30)
    parser.add_argument("--once", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if not arguments.master_url or not arguments.token:
        parser.error("--master-url and --token (or QAS_CLUSTER_URL/QAS_CLUSTER_TOKEN) are required")
    if not 1 <= arguments.maximum_cores <= (os.cpu_count() or 1):
        parser.error("--maximum-cores is outside the host CPU range")
    if arguments.heartbeat_seconds < 5 or arguments.poll_seconds < 5:
        parser.error("heartbeat and poll intervals must be at least 5 seconds")
    worker = ClusterWorker(arguments)
    while True:
        handled = worker.run_once()
        if arguments.once:
            return 0 if handled else 3
        if not handled:
            time.sleep(arguments.poll_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
