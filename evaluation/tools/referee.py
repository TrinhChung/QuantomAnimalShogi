from __future__ import annotations

import json
import os
import queue
import re
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, TextIO

from .common import EvaluationError, append_jsonl, atomic_write_json, sha256_bytes, utc_now


INTEGER_LINE = re.compile(r"^(0|[1-9][0-9]*)$")


def terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        try:
            os.killpg(process.pid, 15)
        except (ProcessLookupError, PermissionError):
            process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


class NativeReferee:
    def __init__(self, executable: Path, command_timeout_seconds: float = 5.0) -> None:
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        self.process = subprocess.Popen(
            [str(executable), "serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
            creationflags=creation_flags,
            start_new_session=os.name != "nt",
        )
        self.command_timeout_seconds = command_timeout_seconds
        self._stdout_queue: queue.Queue[str | None] = queue.Queue()
        self._stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self._stdout_thread.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self._stdout_queue.put(line)
        self._stdout_queue.put(None)

    def command(self, command: str) -> dict[str, Any]:
        if self.process.poll() is not None or self.process.stdin is None:
            raise EvaluationError("authoritative referee is not running")
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        try:
            line = self._stdout_queue.get(timeout=self.command_timeout_seconds)
        except queue.Empty as error:
            terminate_process_tree(self.process)
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise EvaluationError(
                f"authoritative referee timed out after {self.command_timeout_seconds:g}s: {stderr}"
            ) from error
        if not line:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise EvaluationError(f"authoritative referee stopped unexpectedly: {stderr}")
        try:
            result = json.loads(line)
        except json.JSONDecodeError as error:
            raise EvaluationError(f"authoritative referee returned malformed JSON: {line!r}") from error
        if not result.get("ok"):
            raise EvaluationError(
                f"authoritative referee {result.get('error_type')}: {result.get('error_message')}"
            )
        return result

    def initial(self) -> dict[str, Any]:
        return self.command("INITIAL")

    def load(self, state_text: str) -> dict[str, Any]:
        return self.command("LOAD_HEX " + state_text.encode("utf-8").hex())

    def snapshot(self) -> dict[str, Any]:
        return self.command("SNAPSHOT")

    def apply(self, action: int) -> dict[str, Any]:
        return self.command(f"APPLY {action}")

    def close(self) -> None:
        if self.process.poll() is None and self.process.stdin:
            try:
                self.process.stdin.write("QUIT\n")
                self.process.stdin.flush()
                self.process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                terminate_process_tree(self.process)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        self._stdout_thread.join(timeout=1)

    def __enter__(self) -> "NativeReferee":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


@dataclass(frozen=True)
class EngineSpec:
    version_id: str
    executable: Path
    config: Path
    binary_sha256: str
    config_sha256: str
    commit: str | None
    benchmark_executable: Path | None = None
    command_prefix: tuple[str, ...] | None = None


class EngineSession:
    def __init__(
        self,
        spec: EngineSpec,
        tt_size_mb: int,
        stdout_path: Path,
        stderr_path: Path,
        rng_seed: int | None = None,
    ) -> None:
        self.spec = spec
        self.stdout_path = stdout_path
        self.stderr_path = stderr_path
        stdout_path.parent.mkdir(parents=True, exist_ok=True)
        stderr_path.parent.mkdir(parents=True, exist_ok=True)
        self.stdout_file = stdout_path.open("w", encoding="utf-8", newline="\n")
        self.stderr_file = stderr_path.open("w", encoding="utf-8", newline="\n")
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        self.command_line = [
            *(spec.command_prefix or (str(spec.executable),)),
            "--config",
            str(spec.config),
            "--tt-size-mb",
            str(tt_size_mb),
        ]
        self.started_at_utc = utc_now()
        environment = os.environ.copy()
        if rng_seed is not None:
            environment["QAS_EVALUATION_SEED"] = str(rng_seed)
        self.process = subprocess.Popen(
            self.command_line,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
            creationflags=creation_flags,
            start_new_session=os.name != "nt",
            env=environment,
        )
        self.output_queue: queue.Queue[str] = queue.Queue()
        self._stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self._stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.stdout_file.write(line)
            self.stdout_file.flush()
            self.output_queue.put(line.rstrip("\r\n"))

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for line in self.process.stderr:
            self.stderr_file.write(line)
            self.stderr_file.flush()

    def request_action(self, protocol_request: str, hard_timeout_ms: int) -> dict[str, Any]:
        started = time.perf_counter()
        if self.process.poll() is not None:
            return self._failure("process_crash", started, raw_line=None)
        assert self.process.stdin is not None
        try:
            self.process.stdin.write(protocol_request + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError):
            return self._failure("process_crash", started, raw_line=None)
        try:
            raw_line = self.output_queue.get(timeout=hard_timeout_ms / 1000.0)
        except queue.Empty:
            status = "process_crash" if self.process.poll() is not None else "external_timeout"
            if status == "external_timeout":
                terminate_process_tree(self.process)
            return self._failure(status, started, raw_line=None)
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        if not INTEGER_LINE.fullmatch(raw_line):
            return {
                **self._base_response(elapsed_ms),
                "response_status": "malformed_output",
                "raw_stdout_line": raw_line,
                "submitted_action": None,
                "error_type": "malformed_output",
                "error_message": "stdout response is not one canonical integer",
            }
        action = int(raw_line)
        time.sleep(0.01)
        extra_lines: list[str] = []
        while True:
            try:
                extra_lines.append(self.output_queue.get_nowait())
            except queue.Empty:
                break
        if extra_lines:
            return {
                **self._base_response(elapsed_ms),
                "response_status": "protocol_desynchronization",
                "raw_stdout_line": raw_line,
                "extra_stdout_lines": extra_lines,
                "submitted_action": action,
                "error_type": "extra_stdout",
                "error_message": "engine printed unexpected extra stdout",
            }
        return {
            **self._base_response(elapsed_ms),
            "response_status": "valid_response",
            "raw_stdout_line": raw_line,
            "submitted_action": action,
            "error_type": None,
            "error_message": None,
        }

    def _base_response(self, elapsed_ms: float) -> dict[str, Any]:
        return {
            "elapsed_wall_ms": elapsed_ms,
            "elapsed_process_cpu_ms": None,
            "process_exit_code": self.process.poll(),
            "completed_depth": None,
            "started_depth": None,
            "search_completed": None,
            "score": None,
            "pv_actions": None,
            "pv_decoded": None,
            "nodes": None,
            "nps": None,
            "tt_probes": None,
            "tt_hits": None,
            "tt_hit_rate": None,
            "tt_exact_hits": None,
            "tt_lower_hits": None,
            "tt_upper_hits": None,
            "tt_cutoffs": None,
            "tt_replacements": None,
            "tt_collisions": None,
            "cutoffs": None,
            "first_move_cutoffs": None,
            "first_move_cutoff_rate": None,
            "average_cutoff_rank": None,
            "leq_calls": None,
            "leq_input_moves": None,
            "leq_output_representatives": None,
            "leq_duplicate_ratio": None,
            "leq_grouping_ms": None,
            "propagation_calls": None,
            "propagation_ms": None,
            "evaluation_calls": None,
            "evaluation_ms": None,
            "fallback_used": None,
            "fallback_reason": None,
        }

    def _failure(self, status: str, started: float, raw_line: str | None) -> dict[str, Any]:
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        return {
            **self._base_response(elapsed_ms),
            "response_status": status,
            "raw_stdout_line": raw_line,
            "submitted_action": None,
            "error_type": status,
            "error_message": f"engine response failed: {status}",
            "process_exit_code": self.process.poll(),
        }

    def close(self) -> None:
        terminate_process_tree(self.process)
        self._stdout_thread.join(timeout=1)
        self._stderr_thread.join(timeout=1)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        self.stdout_file.close()
        self.stderr_file.close()


def engine_spec(version: dict[str, Any]) -> EngineSpec:
    manifest = version.get("manifest", version)
    return EngineSpec(
        version_id=version["version_id"],
        executable=Path(version["executable"]),
        config=Path(version["config"]),
        binary_sha256=manifest["binary_sha256"],
        config_sha256=manifest["config_sha256"],
        commit=manifest.get("commit"),
        benchmark_executable=(
            Path(version["benchmark_executable"]) if version.get("benchmark_executable") else None
        ),
        command_prefix=None,
    )


def action_mask_hash(snapshot: dict[str, Any]) -> str:
    return sha256_bytes(snapshot["action_mask"].encode("ascii"))
