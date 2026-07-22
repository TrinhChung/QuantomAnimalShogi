from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


GIT_COMMIT = re.compile(r"^[0-9a-f]{40}$")
DEFAULT_REMOTE_URL = "https://github.com/TrinhChung/QuantomAnimalShogi.git"
DEFAULT_CAPABILITIES = "git_update,benchmark,tournament,evaluation"


class WorkerError(RuntimeError):
    pass


def _memory_mb() -> int:
    if os.name == "nt":
        import ctypes

        class MemoryStatus(ctypes.Structure):
            _fields_ = [
                ("length", ctypes.c_ulong),
                ("memory_load", ctypes.c_ulong),
                ("total_physical", ctypes.c_ulonglong),
                ("available_physical", ctypes.c_ulonglong),
                ("total_page_file", ctypes.c_ulonglong),
                ("available_page_file", ctypes.c_ulonglong),
                ("total_virtual", ctypes.c_ulonglong),
                ("available_virtual", ctypes.c_ulonglong),
                ("available_extended_virtual", ctypes.c_ulonglong),
            ]

        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        return max(128, status.total_physical // (1024 * 1024))
    page_size = os.sysconf("SC_PAGE_SIZE")
    pages = os.sysconf("SC_PHYS_PAGES")
    return max(128, page_size * pages // (1024 * 1024))


def _labels(source: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in source.split(","):
        if not item.strip():
            continue
        key, separator, value = item.partition("=")
        if not separator or not re.fullmatch(r"[a-zA-Z0-9._-]+", key.strip()):
            raise WorkerError(f"invalid worker label: {item}")
        result[key.strip()] = value.strip()
    return result


def _run_checked(
    command: list[str],
    directory: Path,
    *,
    low_priority: bool = False,
    heartbeat: Callable[[], None] | None = None,
) -> str:
    effective = command
    creation_flags = 0
    if low_priority and os.name == "nt":
        creation_flags = subprocess.BELOW_NORMAL_PRIORITY_CLASS
    elif low_priority and shutil.which("nice"):
        effective = ["nice", "-n", "15", *command]
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as output:
        process = subprocess.Popen(
            effective,
            cwd=directory,
            stdout=output,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=creation_flags,
            start_new_session=os.name != "nt",
        )
        try:
            while process.poll() is None:
                if heartbeat:
                    heartbeat()
                process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            while process.poll() is None:
                if heartbeat:
                    heartbeat()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    continue
        finally:
            if process.poll() is None:
                _terminate_process(process)
        output.seek(0)
        captured = output.read()
    if process.returncode != 0:
        raise WorkerError(
            f"command failed ({process.returncode}): {' '.join(command)}\n{captured[-8_000:]}",
        )
    return captured.strip()


def _terminate_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    else:
        try:
            os.killpg(process.pid, 15)
        except ProcessLookupError:
            return
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


@dataclass
class ApiClient:
    base_url: str
    token: str
    timeout_seconds: int = 30

    def request(self, method: str, route: str, value: dict[str, Any] | None = None) -> dict[str, Any]:
        body = json.dumps(value).encode("utf-8") if value is not None else None
        request = urllib.request.Request(
            f"{self.base_url.rstrip('/')}{route}",
            data=body,
            method=method,
            headers={
                "authorization": f"Bearer {self.token}",
                "content-type": "application/json",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            raise WorkerError(f"master returned HTTP {error.code}: {detail}") from error
        except urllib.error.URLError as error:
            raise WorkerError(f"cannot reach cluster master: {error.reason}") from error


class GitWorkspace:
    def __init__(self, path: Path, remote_url: str) -> None:
        self.path = path.resolve()
        self.remote_url = remote_url

    def prepare(self, commit: str, remote: str = "origin") -> None:
        if not GIT_COMMIT.fullmatch(commit):
            raise WorkerError("job git commit is invalid")
        if self.path.exists() and not (self.path / ".git").exists():
            if self.path.is_dir() and not any(self.path.iterdir()):
                self.path.rmdir()
            else:
                raise WorkerError(f"worker workspace is not a Git clone: {self.path}")
        if not self.path.exists():
            self.path.parent.mkdir(parents=True, exist_ok=True)
            _run_checked(
                ["git", "clone", "--no-checkout", self.remote_url, str(self.path)],
                self.path.parent,
            )
        if not (self.path / ".git").exists():
            raise WorkerError(f"worker workspace clone failed: {self.path}")
        status = _run_checked(["git", "status", "--porcelain"], self.path)
        if status:
            raise WorkerError("worker workspace has uncommitted changes")
        _run_checked(["git", "fetch", "--prune", remote, commit], self.path)
        _run_checked(["git", "cat-file", "-e", f"{commit}^{{commit}}"], self.path)
        _run_checked(["git", "checkout", "--detach", commit], self.path)

    def current_commit(self) -> str | None:
        if not (self.path / ".git").exists():
            return None
        value = _run_checked(["git", "rev-parse", "HEAD"], self.path)
        return value if GIT_COMMIT.fullmatch(value) else None


@dataclass(frozen=True)
class BuildArtifacts:
    executable: Path
    benchmark: Path
    referee: Path
    stage35_benchmark: Path


def _build_workspace(
    workspace: GitWorkspace,
    maximum_cores: int,
    heartbeat: Callable[[], None] | None = None,
) -> BuildArtifacts:
    version = "cluster-worker"
    if os.name == "nt":
        _run_checked(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "scripts/build_version.ps1",
                "-Version",
                version,
                "-Configuration",
                "Release",
            ],
            workspace.path,
            low_priority=True,
            heartbeat=heartbeat,
        )
        output = workspace.path / "build" / version / "Release"
        suffix = ".exe"
    else:
        build = workspace.path / "build" / version
        _run_checked(
            ["cmake", "-S", ".", "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
            workspace.path,
            low_priority=True,
            heartbeat=heartbeat,
        )
        _run_checked(
            [
                "cmake",
                "--build",
                str(build),
                "--parallel",
                str(maximum_cores),
                "--target",
                "qas",
                "qas_evaluation_benchmark",
                "qas_evaluation_referee",
                "qas_stage35_benchmark",
            ],
            workspace.path,
            low_priority=True,
            heartbeat=heartbeat,
        )
        output = build
        suffix = ""
    artifacts = BuildArtifacts(
        executable=output / f"qas{suffix}",
        benchmark=output / f"qas_evaluation_benchmark{suffix}",
        referee=output / f"qas_evaluation_referee{suffix}",
        stage35_benchmark=output / f"qas_stage35_benchmark{suffix}",
    )
    missing = [str(path) for path in artifacts.__dict__.values() if not path.is_file()]
    if missing:
        raise WorkerError("build did not produce required artifacts: " + ", ".join(missing))
    return artifacts


def _kokoro_busy(container: str | None, threshold_percent: float) -> bool:
    if not container or not shutil.which("docker"):
        return False
    completed = subprocess.run(
        ["docker", "stats", "--no-stream", "--format", "{{.CPUPerc}}", container],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if completed.returncode != 0:
        return False
    try:
        usage = float(completed.stdout.strip().rstrip("%"))
    except ValueError:
        return False
    return usage >= threshold_percent


def _json_file(path: Path, maximum_bytes: int = 2_000_000) -> Any:
    if not path.is_file() or path.stat().st_size > maximum_bytes:
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def _sha256(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
