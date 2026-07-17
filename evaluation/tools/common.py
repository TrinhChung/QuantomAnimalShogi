from __future__ import annotations

import csv
import ctypes
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping


SCHEMA_VERSION = 1
VERSION_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
CHANGE_CATEGORIES = {
    "optimization_only",
    "performance_only",
    "move_ordering",
    "search_control",
    "selective_search",
    "pruning_change",
    "canonicalization_change",
    "evaluation_change",
    "rule_change",
    "architecture_change",
    "mixed",
}


class EvaluationError(RuntimeError):
    """An infrastructure or validation failure that makes a run invalid."""


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def timestamp_id() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def canonical_json_hash(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return sha256_bytes(encoded)


def load_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as source:
            return json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise EvaluationError(f"cannot load JSON {path}: {error}") from error


def _replace_with_retry(source: Path, destination: Path) -> None:
    """Atomically replace a file, tolerating short-lived Windows reader locks."""
    deadline = time.monotonic() + 2.0
    delay = 0.01
    while True:
        try:
            os.replace(source, destination)
            return
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(delay)
            delay = min(delay * 2.0, 0.1)


def atomic_write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent)
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(text)
            output.flush()
            os.fsync(output.fileno())
        _replace_with_retry(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def atomic_write_json(path: Path, value: Any) -> None:
    atomic_write_text(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def append_jsonl(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    with path.open("a", encoding="utf-8", newline="\n") as output:
        output.write(line)
        output.flush()
        os.fsync(output.fileno())


def write_csv(path: Path, rows: Iterable[Mapping[str, Any]], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent)
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=fieldnames, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)
            output.flush()
            os.fsync(output.fileno())
        _replace_with_retry(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def run_capture(arguments: list[str], cwd: Path | None = None, timeout: float = 30.0) -> str:
    try:
        completed = subprocess.run(
            arguments,
            cwd=cwd,
            check=True,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise EvaluationError(f"command failed: {subprocess.list2cmdline(arguments)}: {error}") from error
    return completed.stdout.strip()


def git_metadata(root: Path) -> dict[str, Any]:
    def git(*arguments: str) -> str:
        return run_capture(["git", *arguments], cwd=root)

    try:
        status = git("status", "--porcelain")
        return {
            "commit": git("rev-parse", "HEAD"),
            "branch": git("branch", "--show-current") or None,
            "dirty": bool(status),
            "status": status.splitlines(),
        }
    except EvaluationError:
        return {"commit": None, "branch": None, "dirty": None, "status": []}


def compiler_version() -> str:
    compiler_files = sorted(
        (repository_root() / "build" / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")
    )
    for compiler_file in reversed(compiler_files):
        text = compiler_file.read_text(encoding="utf-8", errors="replace")
        identifier = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]+)"\)', text)
        version = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"\)', text)
        if identifier and version:
            return f"{identifier.group(1)} {version.group(1)}"
    candidates = (["cl"], ["g++", "--version"], ["clang++", "--version"])
    for arguments in candidates:
        try:
            completed = subprocess.run(arguments, capture_output=True, text=True, timeout=5)
        except OSError:
            continue
        combined = (completed.stdout + completed.stderr).strip()
        if combined:
            return combined.splitlines()[0]
    return "unknown"


def compiler_flags(build_type: str = "Release") -> str:
    cache = repository_root() / "build" / "CMakeCache.txt"
    if not cache.is_file():
        return "unknown"
    entries: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" not in line or "=" not in line:
            continue
        key = line.split(":", 1)[0]
        entries[key] = line.split("=", 1)[1]
    values = [entries.get("CMAKE_CXX_FLAGS", ""), entries.get(f"CMAKE_CXX_FLAGS_{build_type.upper()}", "")]
    return " ".join(value.strip() for value in values if value.strip()) or "unknown"


def cpu_model() -> str:
    if platform.system() == "Windows":
        value = platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER", "")
        if value:
            return value
    return platform.processor() or "unknown"


def process_affinity() -> list[int] | None:
    if hasattr(os, "sched_getaffinity"):
        return sorted(os.sched_getaffinity(0))
    if os.name == "nt":
        process_mask = ctypes.c_size_t()
        system_mask = ctypes.c_size_t()
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        kernel32.GetProcessAffinityMask.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        if kernel32.GetProcessAffinityMask(
            kernel32.GetCurrentProcess(), ctypes.byref(process_mask), ctypes.byref(system_mask)
        ):
            return [
                index
                for index in range(process_mask.value.bit_length())
                if process_mask.value & (1 << index)
            ]
    return None


def apply_benchmark_controls(affinity_mode: str, priority: str) -> dict[str, Any]:
    """Apply best-effort parent controls inherited by sequential benchmark children."""
    before = process_affinity()
    affinity_applied = False
    affinity_error: str | None = None
    priority_applied = priority == "normal"
    priority_error: str | None = None
    try:
        if affinity_mode == "single_core" and before:
            selected = before[0]
            if os.name == "nt":
                kernel32 = ctypes.windll.kernel32
                kernel32.GetCurrentProcess.restype = ctypes.c_void_p
                kernel32.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
                kernel32.SetProcessAffinityMask.restype = ctypes.c_int
                if not kernel32.SetProcessAffinityMask(
                    kernel32.GetCurrentProcess(), ctypes.c_size_t(1 << selected)
                ):
                    raise OSError(ctypes.get_last_error(), "SetProcessAffinityMask failed")
            elif hasattr(os, "sched_setaffinity"):
                os.sched_setaffinity(0, {selected})
            else:
                raise OSError("process affinity control is unavailable")
            affinity_applied = process_affinity() == [selected]
        elif affinity_mode in {"inherit", "all_available"}:
            affinity_applied = True
        else:
            affinity_error = f"unsupported affinity mode: {affinity_mode}"
    except (OSError, AttributeError) as error:
        affinity_error = str(error)
    try:
        if priority != "normal":
            if os.name == "nt":
                classes = {
                    "above_normal": 0x00008000,
                    "high": 0x00000080,
                }
                if priority not in classes:
                    raise OSError(f"unsupported priority: {priority}")
                kernel32 = ctypes.windll.kernel32
                kernel32.GetCurrentProcess.restype = ctypes.c_void_p
                kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
                kernel32.SetPriorityClass.restype = ctypes.c_int
                if not kernel32.SetPriorityClass(
                    kernel32.GetCurrentProcess(), classes[priority]
                ):
                    raise OSError(ctypes.get_last_error(), "SetPriorityClass failed")
                priority_applied = True
            else:
                priority_error = "non-default priority is not applied without elevated privileges"
    except (OSError, AttributeError) as error:
        priority_error = str(error)
    return {
        "requested_affinity": affinity_mode,
        "affinity_before": before,
        "affinity_after": process_affinity(),
        "affinity_applied": affinity_applied,
        "affinity_error": affinity_error,
        "requested_priority": priority,
        "priority_applied": priority_applied,
        "priority_error": priority_error,
    }


def validate_version_id(version_id: str) -> None:
    if not VERSION_ID_PATTERN.fullmatch(version_id):
        raise EvaluationError(
            "version ID must be 1-64 lowercase letters, digits, '.', '_' or '-', starting alphanumeric"
        )


def validate_change_category(category: str) -> None:
    if category not in CHANGE_CATEGORIES:
        raise EvaluationError(f"unsupported change category: {category}")


def locate_companion(executable: Path, name: str) -> Path | None:
    suffix = ".exe" if os.name == "nt" else ""
    candidates = [
        executable.parent / f"{name}{suffix}",
        executable.parent.parent / f"{name}{suffix}",
        repository_root() / "build" / "Release" / f"{name}{suffix}",
        repository_root() / "build" / f"{name}{suffix}",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def copy_read_only(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    destination.chmod(0o444)


def percentile(sorted_values: list[float], probability: float) -> float | None:
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]
    position = probability * (len(sorted_values) - 1)
    lower = int(position)
    upper = min(lower + 1, len(sorted_values) - 1)
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction
