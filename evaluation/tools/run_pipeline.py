from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path
from typing import Any

from .analyze_divergence import analyze_divergences
from .common import (
    EvaluationError,
    apply_benchmark_controls,
    atomic_write_json,
    canonical_json_hash,
    copy_read_only,
    cpu_model,
    default_build_directory,
    git_metadata,
    load_json,
    locate_companion,
    process_affinity,
    repository_root,
    sha256_file,
    timestamp_id,
    utc_now,
    validate_change_category,
    validate_version_id,
    write_csv,
)
from .corpus import (
    enrich_corpus_with_diagnostics,
    load_corpus,
    select_openings,
    select_performance,
    summarize_corpus,
)
from .generate_report import generate_report
from .referee import EngineSpec, engine_spec
from .run_correctness import compare_fixed_depth, protocol_probe, run_ctest
from .run_performance import (
    run_benchmark_once,
    run_fixed_depth,
    run_time_controlled,
    summarize_fixed,
    summarize_time_controlled,
    write_fixed_summaries,
    write_time_summaries,
)
from .run_selfplay import replay_game, run_paired_matches
from .summarize_results import classify_candidate, reliability_summary, summarize_games
from .version_registry import VersionRegistry


STAGE_LABELS = {
    "infrastructure_validation": "1/8 infrastructure validation",
    "unit_and_rule_tests": "2/8 unit and rule tests",
    "fixed_depth_correctness_and_performance": "3/8 fixed-depth correctness/performance",
    "time_controlled_performance": "4/8 time-controlled performance",
    "paired_selfplay": "5/8 paired engine matches",
    "divergence_analysis": "6/8 divergence analysis",
    "statistical_summary": "7/8 statistical summary",
    "report_generation": "8/8 report generation",
    "complete": "complete",
}


class Progress:
    def __init__(self, path: Path, initial: dict[str, Any] | None = None) -> None:
        self.path = path
        self.live_path = path.with_name("live_progress.json")
        self.started_at = time.monotonic()
        if path.is_file():
            self.value = load_json(path)
            hashes = self.value.get("task_result_hashes")
            if hashes is None:
                self.value["task_result_hashes"] = {
                    task: canonical_json_hash(result)
                    for task, result in self.value.get("task_results", {}).items()
                }
                self.value["integrity_hashes_migrated"] = True
                self.save()
            self._validate_cached_results()
        elif initial is not None:
            self.value = initial
            self.save()
        else:
            raise EvaluationError(f"resume progress is missing: {path}")

    def save(self) -> None:
        self.value["updated_at_utc"] = utc_now()
        atomic_write_json(self.path, self.value)

    def _validate_cached_results(self) -> None:
        results = self.value.get("task_results", {})
        hashes = self.value.get("task_result_hashes", {})
        for task in self.value.get("completed_tasks", []):
            if task not in results or hashes.get(task) != canonical_json_hash(results[task]):
                raise EvaluationError(f"progress cached-result integrity failure: {task}")

    def log(self, message: str) -> None:
        elapsed = int(time.monotonic() - self.started_at)
        hours, remainder = divmod(elapsed, 3600)
        minutes, seconds = divmod(remainder, 60)
        print(f"[QAS {hours:02d}:{minutes:02d}:{seconds:02d}] {message}", flush=True)

    def announce(self, run_config: dict[str, Any], run_directory: Path, is_resume: bool) -> None:
        mode = "RESUME" if is_resume else "START"
        self.log(
            f"{mode} run={run_config['run_id']} profile={run_config['profile']} "
            f"candidate={run_config['candidate_version_id']} "
            f"opponents={','.join(run_config['opponent_ids'])}"
        )
        self.log(f"Artifacts: {run_directory}")
        benchmark_total = len(self.value.get("planned_benchmark_tasks", []))
        game_total = len(self.value.get("planned_game_ids", []))
        if is_resume:
            self.log(
                f"Cached progress: benchmarks {self._completed_benchmarks()}/{benchmark_total}, "
                f"games {len(self.value.get('completed_game_ids', []))}/{game_total}"
            )

    def _completed_benchmarks(self) -> int:
        planned = set(self.value.get("planned_benchmark_tasks", []))
        return sum(task in planned for task in self.value.get("completed_tasks", []))

    def _write_live(self, active_task: str | None) -> None:
        atomic_write_json(
            self.live_path,
            {
                "schema_version": 1,
                "timestamp_utc": utc_now(),
                "current_stage": self.value.get("current_stage"),
                "active_task": active_task,
                "completed_benchmarks": self._completed_benchmarks(),
                "planned_benchmarks": len(self.value.get("planned_benchmark_tasks", [])),
                "completed_games": len(self.value.get("completed_game_ids", [])),
                "planned_games": len(self.value.get("planned_game_ids", [])),
                "last_flushed_move": self.value.get("last_flushed_move"),
            },
        )

    def task_started(self, task: str) -> None:
        planned_benchmarks = self.value.get("planned_benchmark_tasks", [])
        planned_games = self.value.get("planned_game_ids", [])
        if task in planned_benchmarks:
            complete = self._completed_benchmarks()
            self.log(
                f"RUN benchmark {complete + 1}/{len(planned_benchmarks)} "
                f"(remaining {len(planned_benchmarks) - complete}): {task}"
            )
        elif task.startswith("game:"):
            complete = len(self.value.get("completed_game_ids", []))
            self.log(
                f"RUN game {complete + 1}/{len(planned_games)} "
                f"(remaining {len(planned_games) - complete}): {task[5:]}"
            )
        else:
            self.log(f"RUN {task}")
        self._write_live(task)

    def record_move(self, record: dict[str, Any], interval: int) -> None:
        last_move = {
            "game_id": record["game_id"],
            "attempt_id": record["attempt_id"],
            "ply": record["ply"],
            "turn_before": record["turn_before"],
            "engine_version": record["engine_version"],
            "response_status": record["response_status"],
        }
        self.value["last_flushed_move"] = last_move
        should_print = (
            record["response_status"] != "valid_response"
            or bool(record.get("terminal_after"))
            or (record["ply"] + 1) % max(1, interval) == 0
        )
        self._write_live(f"game:{record['game_id']}")
        if should_print:
            self.log(
                f"MOVE game={record['game_id']} ply={record['ply'] + 1} "
                f"engine={record['engine_version']} status={record['response_status']} "
                f"elapsed={record['elapsed_wall_ms']:.1f}ms terminal={record.get('terminal_type')}"
            )
            self.save()

    def is_complete(self, task: str) -> bool:
        return task in self.value["completed_tasks"]

    def cached_result(self, task: str) -> dict[str, Any]:
        result = self.value["task_results"][task]
        if self.value.get("task_result_hashes", {}).get(task) != canonical_json_hash(result):
            raise EvaluationError(f"progress cached-result integrity failure: {task}")
        return result

    def mark_complete(self, task: str, result: dict[str, Any]) -> None:
        was_complete = task in self.value["completed_tasks"]
        if not was_complete:
            self.value["completed_tasks"].append(task)
        self.value["task_results"][task] = result
        self.value.setdefault("task_result_hashes", {})[task] = canonical_json_hash(result)
        if task.startswith("game:"):
            game_id = task[5:]
            if game_id not in self.value["completed_game_ids"]:
                self.value["completed_game_ids"].append(game_id)
            self.value["incomplete_game_ids"] = [
                item for item in self.value["incomplete_game_ids"] if item != game_id
            ]
            failed_games = self.value.setdefault("failed_game_ids", [])
            if result.get("valid_strength_game") is False and game_id not in failed_games:
                failed_games.append(game_id)
            move_log = Path(result["move_log_path"]) if result.get("move_log_path") else None
            if move_log is not None and move_log.is_file():
                lines = move_log.read_text(encoding="utf-8").splitlines()
                if lines:
                    last_move = json.loads(lines[-1])
                    self.value["last_flushed_move"] = {
                        "game_id": game_id,
                        "attempt_id": last_move["attempt_id"],
                        "ply": last_move["ply"],
                    }
        self.save()
        self._write_live(None)
        if was_complete:
            return
        if task in self.value.get("planned_benchmark_tasks", []):
            status = "FAIL" if result.get("error_type") else "DONE"
            self.log(
                f"{status} benchmark {self._completed_benchmarks()}/"
                f"{len(self.value['planned_benchmark_tasks'])}: {task} "
                f"elapsed={result.get('elapsed_wall_ms')}ms nodes={result.get('nodes')}"
            )
        elif task.startswith("game:"):
            self.log(
                f"DONE game {len(self.value['completed_game_ids'])}/"
                f"{len(self.value.get('planned_game_ids', []))}: {task[5:]} "
                f"result={result.get('candidate_result')} terminal={result.get('terminal_type')}"
            )
        elif task == "ctest":
            self.log(f"DONE ctest passed={result.get('passed')} exit={result.get('exit_code')}")
        elif task.startswith("protocol-probe:"):
            self.log(f"DONE {task} passed={result.get('passed')}")

    def stage(self, name: str) -> None:
        self.value["current_stage"] = name
        self.save()
        self._write_live(None)
        self.log(
            f"STAGE {STAGE_LABELS.get(name, name)} | "
            f"benchmarks {self._completed_benchmarks()}/"
            f"{len(self.value.get('planned_benchmark_tasks', []))} | "
            f"games {len(self.value.get('completed_game_ids', []))}/"
            f"{len(self.value.get('planned_game_ids', []))}"
        )


def _slug(value: str) -> str:
    normalized = re.sub(r"[^a-z0-9._-]+", "-", value.lower()).strip("-._")
    return normalized[:64] or "candidate"


def _profiles() -> dict[str, Any]:
    path = repository_root() / "evaluation" / "config" / "evaluation_profiles.json"
    document = load_json(path)
    defaults = document.get("profile_defaults", {})
    raw_profiles = document["profiles"]
    resolved: dict[str, Any] = {}

    def merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
        result = dict(base)
        for key, value in override.items():
            if key == "inherits":
                continue
            if isinstance(value, dict) and isinstance(result.get(key), dict):
                result[key] = merge(result[key], value)
            else:
                result[key] = value
        return result

    def resolve(name: str, stack: tuple[str, ...] = ()) -> dict[str, Any]:
        if name in resolved:
            return resolved[name]
        if name in stack:
            raise EvaluationError("cyclic evaluation profile inheritance: " + " -> ".join((*stack, name)))
        if name not in raw_profiles:
            raise EvaluationError(f"profile inherits unknown profile: {name}")
        raw = raw_profiles[name]
        parent = raw.get("inherits")
        base = resolve(parent, (*stack, name)) if parent else defaults
        resolved[name] = merge(base, raw)
        return resolved[name]

    for profile_name in raw_profiles:
        resolve(profile_name)
    required_profiles = {
        "infrastructure_smoke",
        "correctness_regression",
        "fixed_depth_quick",
        "fixed_depth_deep",
        "fixed_time_quick",
        "fixed_time_contest",
        "diagnostic_telemetry",
        "strength_quick",
        "strength_candidate",
        "promotion_test",
        "reliability_soak",
        "architecture_change_full",
    }
    missing_profiles = required_profiles - resolved.keys()
    if missing_profiles:
        raise EvaluationError("required evaluation profiles are missing: " + ", ".join(sorted(missing_profiles)))
    required_fields = {
        "purpose",
        "opening_pairs",
        "fixed_depths",
        "fixed_warmup_repetitions",
        "fixed_repetitions",
        "time_budgets_ms",
        "time_warmup_repetitions",
        "time_repetitions",
        "diagnostic_repetitions",
        "telemetry_mode",
        "stopping_rule",
        "maximum_expected_runtime_minutes",
        "acceptance_permissions",
        "rejection_conditions",
    }
    for name, profile in resolved.items():
        missing = required_fields - profile.keys()
        if missing:
            raise EvaluationError(f"profile {name} is missing fields: {', '.join(sorted(missing))}")
    return resolved


def _policy() -> dict[str, Any]:
    return load_json(repository_root() / "evaluation" / "config" / "acceptance_policy.json")


def _validated_build_binding(executable: Path, benchmark: Path | None) -> tuple[Path | None, dict[str, Any] | None]:
    if benchmark is None:
        return None, None
    path = executable.parent / "qas_evaluation_build_manifest.json"
    if not path.is_file():
        return None, None
    value = load_json(path)
    if int(value.get("schema_version", 0)) != 1:
        raise EvaluationError(f"unsupported evaluation build manifest schema: {path}")
    if value.get("qas_sha256") != sha256_file(executable):
        raise EvaluationError("evaluation build manifest does not bind the candidate executable")
    if value.get("benchmark_sha256") != sha256_file(benchmark):
        raise EvaluationError("evaluation build manifest does not bind the benchmark companion")
    if str(value.get("build_configuration", "")).lower() != "release":
        raise EvaluationError("candidate evaluation build manifest is not a Release build")
    return path, value


def _candidate_manifest(
    *,
    version_id: str,
    name: str,
    executable: Path,
    config: Path,
    benchmark: Path | None,
    change_category: str,
) -> dict[str, Any]:
    metadata = git_metadata(repository_root())
    build_manifest_path, build_manifest = _validated_build_binding(executable, benchmark)
    configured_dirty = (build_manifest or {}).get("git_dirty_at_configure")
    if isinstance(configured_dirty, str):
        configured_dirty = configured_dirty.lower() == "true" if configured_dirty.lower() in {"true", "false"} else None
    return {
        "schema_version": 1,
        "version_id": version_id,
        "display_name": name,
        "binary_sha256": sha256_file(executable),
        "config_sha256": sha256_file(config),
        "benchmark_binary_sha256": sha256_file(benchmark) if benchmark else None,
        "original_executable_path": str(executable.resolve()),
        "original_config_path": str(config.resolve()),
        "original_benchmark_path": str(benchmark.resolve()) if benchmark else None,
        "artifact_binding_verified": build_manifest is not None,
        "build_manifest_sha256": sha256_file(build_manifest_path) if build_manifest_path else None,
        "original_build_manifest_path": str(build_manifest_path.resolve()) if build_manifest_path else None,
        "commit": (build_manifest or {}).get("git_commit"),
        "branch": None,
        "dirty": configured_dirty,
        "dirty_paths": None,
        "build_type": (build_manifest or {}).get("build_configuration"),
        "compiler": (
            f"{build_manifest.get('compiler_id')} {build_manifest.get('compiler_version')}"
            if build_manifest
            else None
        ),
        "compiler_flags": None,
        "evaluation_worktree_commit": metadata["commit"],
        "evaluation_worktree_branch": metadata["branch"],
        "evaluation_worktree_dirty": metadata["dirty"],
        "evaluation_worktree_dirty_paths": metadata["status"],
        "enabled_engine_features": load_json(config),
        "change_category": change_category,
        "snapshotted_at_utc": utc_now(),
    }


def _snapshot_candidate(
    run_directory: Path,
    executable: Path,
    config: Path,
    benchmark: Path | None,
    manifest: dict[str, Any],
) -> dict[str, Any]:
    directory = run_directory / "candidate"
    directory.mkdir(parents=True, exist_ok=False)
    shutil.copy2(executable, directory / "qas.exe")
    shutil.copy2(config, directory / "engine_config.json")
    benchmark_destination = None
    build_manifest_destination = None
    if benchmark:
        benchmark_destination = directory / "benchmark.exe"
        shutil.copy2(benchmark, benchmark_destination)
    if manifest.get("original_build_manifest_path"):
        build_manifest_destination = directory / "build_manifest.json"
        shutil.copy2(Path(manifest["original_build_manifest_path"]), build_manifest_destination)
    atomic_write_json(directory / "manifest.json", manifest)
    return {
        "version_id": manifest["version_id"],
        "executable": str((directory / "qas.exe").resolve()),
        "config": str((directory / "engine_config.json").resolve()),
        "benchmark_executable": str(benchmark_destination.resolve()) if benchmark_destination else None,
        "build_manifest": str(build_manifest_destination.resolve()) if build_manifest_destination else None,
        "manifest": manifest,
    }


def _snapshot_opponent(directory: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    executable = directory / "qas.exe"
    config = directory / "engine_config.json"
    benchmark = directory / "benchmark.exe"
    return {
        "version_id": manifest["version_id"],
        "directory": str(directory.resolve()),
        "executable": str(executable.resolve()),
        "config": str(config.resolve()),
        "benchmark_executable": str(benchmark.resolve()) if benchmark.is_file() else None,
        "manifest": manifest,
    }


def _copy_opponents(run_directory: Path, opponents: list[dict[str, Any]]) -> list[dict[str, Any]]:
    snapshots: list[dict[str, Any]] = []
    for opponent in opponents:
        destination = run_directory / "opponents" / opponent["version_id"]
        destination.mkdir(parents=True, exist_ok=False)
        atomic_write_json(destination / "manifest.json", opponent["manifest"])
        shutil.copy2(opponent["config"], destination / "engine_config.json")
        shutil.copy2(opponent["executable"], destination / "qas.exe")
        if opponent.get("benchmark_executable"):
            shutil.copy2(opponent["benchmark_executable"], destination / "benchmark.exe")
        snapshots.append(_snapshot_opponent(destination, opponent["manifest"]))
    return snapshots


def _runtime_engine(engine: EngineSpec, profile: dict[str, Any], directory: Path) -> EngineSpec:
    config = load_json(engine.config)
    config.setdefault("search", {})["soft_time_limit_ms"] = profile["move_soft_ms"]
    external_hard_ms = profile["move_hard_ms"]
    internal_grace_ms = min(100, max(1, external_hard_ms - profile["move_soft_ms"]))
    config["search"]["hard_time_limit_ms"] = max(
        profile["move_soft_ms"], external_hard_ms - internal_grace_ms
    )
    config["search"]["max_depth"] = profile["max_depth"]
    config.setdefault("tt", {})["tt_size_mb"] = profile["tt_size_mb"]
    config["tt"]["tt_auto_size_enabled"] = False
    config.setdefault("instrumentation", {})["stderr_log_enabled"] = False
    path = directory / f"{engine.version_id}.runtime_config.json"
    atomic_write_json(path, config)
    return EngineSpec(
        version_id=engine.version_id,
        executable=engine.executable,
        config=path,
        binary_sha256=engine.binary_sha256,
        config_sha256=sha256_file(path),
        commit=engine.commit,
        benchmark_executable=engine.benchmark_executable,
        command_prefix=engine.command_prefix,
    )


def _verify_original_candidate(manifest: dict[str, Any]) -> None:
    paths = [
        (Path(manifest["original_executable_path"]), manifest["binary_sha256"], "candidate executable"),
        (Path(manifest["original_config_path"]), manifest["config_sha256"], "candidate config"),
    ]
    if manifest.get("original_benchmark_path"):
        paths.append(
            (
                Path(manifest["original_benchmark_path"]),
                manifest["benchmark_binary_sha256"],
                "candidate benchmark companion",
            )
        )
    for path, expected, label in paths:
        if not path.is_file() or sha256_file(path) != expected:
            raise EvaluationError(f"{label} changed or disappeared during evaluation: {path}")


def _verify_all_inputs(
    candidate: dict[str, Any],
    candidate_manifest: dict[str, Any],
    opponent_ids: list[str],
    require_original_candidate: bool = True,
) -> None:
    if require_original_candidate:
        _verify_original_candidate(candidate_manifest)
    snapshot_paths = [
        (Path(candidate["executable"]), candidate_manifest["binary_sha256"]),
        (Path(candidate["config"]), candidate_manifest["config_sha256"]),
        (Path(candidate["benchmark_executable"]), candidate_manifest["benchmark_binary_sha256"]),
    ]
    if candidate_manifest.get("build_manifest_sha256"):
        snapshot_paths.append(
            (
                Path(candidate.get("build_manifest") or ""),
                candidate_manifest["build_manifest_sha256"],
            )
        )
    for path, expected in snapshot_paths:
        if not path.is_file() or sha256_file(path) != expected:
            raise EvaluationError(f"candidate run snapshot hash mismatch: {path}")
    run_directory = Path(candidate["executable"]).parent.parent
    run_config_path = run_directory / "run_config.json"
    run_config = load_json(run_config_path) if run_config_path.is_file() else {}
    expected_opponent_binaries = run_config.get("opponent_binary_hashes", {})
    expected_opponent_configs = run_config.get("opponent_config_hashes", {})
    for version_id in opponent_ids:
        directory = run_directory / "opponents" / version_id
        manifest_path = directory / "manifest.json"
        if not manifest_path.is_file():
            raise EvaluationError(f"opponent run snapshot is missing: {version_id}")
        manifest = load_json(manifest_path)
        if (
            manifest.get("binary_sha256") != expected_opponent_binaries.get(version_id)
            or manifest.get("config_sha256") != expected_opponent_configs.get(version_id)
        ):
            raise EvaluationError(f"opponent snapshot disagrees with run configuration: {version_id}")
        paths = [
            (directory / "qas.exe", manifest.get("binary_sha256"), "binary"),
            (directory / "engine_config.json", manifest.get("config_sha256"), "config"),
            (
                directory / "benchmark.exe",
                manifest.get("benchmark_binary_sha256"),
                "benchmark companion",
            ),
        ]
        for path, expected, label in paths:
            if expected is None or not path.is_file() or sha256_file(path) != expected:
                raise EvaluationError(f"opponent {label} run snapshot hash mismatch: {version_id}")
    referee_path = Path(run_config.get("referee_path", ""))
    referee_hash = run_config.get("referee_sha256")
    if referee_hash and (not referee_path.is_file() or sha256_file(referee_path) != referee_hash):
        raise EvaluationError("authoritative referee changed during evaluation")


def _load_moves(run_directory: Path) -> list[dict[str, Any]]:
    path = run_directory / "selfplay" / "moves.jsonl"
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def _configuration_differences(left: Any, right: Any, path: str = "") -> list[dict[str, Any]]:
    if isinstance(left, dict) and isinstance(right, dict):
        differences: list[dict[str, Any]] = []
        for key in sorted(left.keys() | right.keys()):
            child = f"{path}.{key}" if path else key
            if key not in left or key not in right:
                differences.append({"path": child, "candidate": left.get(key), "opponent": right.get(key)})
            else:
                differences.extend(_configuration_differences(left[key], right[key], child))
        return differences
    if left != right:
        return [{"path": path, "candidate": left, "opponent": right}]
    return []


def _run_reference_checks(
    *,
    candidate: EngineSpec,
    fixtures: list[dict[str, Any]],
    profile: dict[str, Any],
    change_category: str,
    policy: dict[str, Any],
    output_directory: Path,
    progress: Progress,
) -> dict[str, Any]:
    category_policy = policy.get("category_requirements", {}).get(change_category, {})
    required = bool(category_policy.get("require_reference_checks"))
    if not required:
        return {"required": False, "available": True, "passed": True, "comparisons": []}
    config = load_json(candidate.config)
    search = config.setdefault("search", {})
    available = bool(search.get("l_eq_enabled") or search.get("l_eq_trigger_enabled"))
    if not available:
        result = {
            "required": True,
            "available": False,
            "passed": False,
            "reason": "no configured selective L_eq mode can be disabled for a reference search",
            "comparisons": [],
        }
        atomic_write_json(output_directory / "reference_checks.json", result)
        return result
    search["l_eq_enabled"] = False
    search["l_eq_trigger_enabled"] = False
    reference_config = output_directory / "candidate.non_selective_reference.json"
    atomic_write_json(reference_config, config)
    reference = EngineSpec(
        version_id=f"{candidate.version_id}-non-selective-reference",
        executable=candidate.executable,
        config=reference_config,
        binary_sha256=candidate.binary_sha256,
        config_sha256=sha256_file(reference_config),
        commit=candidate.commit,
        benchmark_executable=candidate.benchmark_executable,
        command_prefix=candidate.command_prefix,
    )
    depth = int(profile.get("reference_check_depth", 4))
    limit = int(profile.get("reference_check_fixture_limit", 4))
    comparisons: list[dict[str, Any]] = []
    for fixture in fixtures[:limit]:
        rows: dict[str, dict[str, Any]] = {}
        for label, engine in (("active", candidate), ("reference", reference)):
            task = f"reference-{label}:{fixture['fixture_id']}:{depth}"
            if progress.is_complete(task):
                rows[label] = progress.cached_result(task)
                continue
            progress.task_started(task)
            rows[label] = run_benchmark_once(
                engine,
                fixture,
                mode="fixed",
                depth=depth,
                soft_ms=int(profile["fixed_hard_timeout_ms"]),
                hard_ms=int(profile["fixed_hard_timeout_ms"]),
                tt_size_mb=int(profile["tt_size_mb"]),
                diagnostic=False,
                cpu_affinity_core=profile.get("cpu_affinity_core"),
                process_priority=profile.get("process_priority"),
            )
            progress.mark_complete(task, rows[label])
        active, baseline = rows["active"], rows["reference"]
        classification = "identical"
        if active.get("error_type") or baseline.get("error_type"):
            classification = "protocol_failure"
        elif not active.get("completed") or not baseline.get("completed"):
            classification = "incomplete_search"
        elif sorted(active.get("legal_actions") or []) != sorted(baseline.get("legal_actions") or []):
            classification = "different_legal_actions"
        elif active.get("terminal_result") != baseline.get("terminal_result"):
            classification = "different_terminal_result"
        elif active.get("score") != baseline.get("score"):
            classification = "different_score"
        elif active.get("best_action") != baseline.get("best_action"):
            classification = "equal_score_tiebreak"
        comparisons.append(
            {
                "fixture_id": fixture["fixture_id"],
                "depth": depth,
                "classification": classification,
                "active_action": active.get("best_action"),
                "reference_action": baseline.get("best_action"),
                "active_score": active.get("score"),
                "reference_score": baseline.get("score"),
                "passed": classification in {"identical", "equal_score_tiebreak"},
            }
        )
    result = {
        "required": True,
        "available": True,
        "mode": "candidate L_eq on versus the same binary with L_eq disabled",
        "depth": depth,
        "comparisons": comparisons,
        "passed": bool(comparisons) and all(row["passed"] for row in comparisons),
    }
    atomic_write_json(output_directory / "reference_checks.json", result)
    return result


def _initialize_run(arguments: argparse.Namespace) -> tuple[Path, dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    root = repository_root()
    registry = VersionRegistry()
    registered: dict[str, Any] | None = None
    if arguments.registered_candidate:
        registered = registry.verify(arguments.registered_candidate)
        arguments.candidate_exe = registered["executable"]
        arguments.candidate_config = registered["config"]
        arguments.candidate_benchmark = registered.get("benchmark_executable")
        arguments.candidate_name = registered["display_name"]
        # Preserve the registry identity so an acceptance report can be cryptographically
        # bound to the exact version later presented to the promotion command.
        arguments.candidate_version_id = registered["version_id"]
        arguments.change_category = registered.get("change_category", "mixed")
    required = {
        "--candidate-exe": arguments.candidate_exe,
        "--candidate-name": arguments.candidate_name,
        "--change-category": arguments.change_category,
    }
    missing = [name for name, value in required.items() if not value]
    if missing:
        raise EvaluationError("missing required arguments: " + ", ".join(missing))
    validate_change_category(arguments.change_category)
    executable = Path(arguments.candidate_exe).resolve()
    config = Path(arguments.candidate_config or root / "engine_config.json").resolve()
    if not executable.is_file() or executable.stat().st_size == 0:
        raise EvaluationError(f"candidate executable is missing or empty: {executable}")
    if not config.is_file():
        raise EvaluationError(f"candidate config is missing: {config}")
    load_json(config)
    benchmark = (
        Path(arguments.candidate_benchmark).resolve()
        if arguments.candidate_benchmark
        else locate_companion(executable, "qas_evaluation_benchmark")
    )
    if benchmark is None:
        raise EvaluationError(
            "candidate benchmark companion is missing; build the standard all-target Release configuration"
        )
    version_id = arguments.candidate_version_id or _slug(arguments.candidate_name)
    validate_version_id(version_id)
    explicit = [item for item in (arguments.opponents or "").split(",") if item] or None
    opponents = registry.select_opponents(explicit, arguments.include_previous)
    missing_benchmarks = [
        opponent["version_id"] for opponent in opponents if not opponent.get("benchmark_executable")
    ]
    if missing_benchmarks:
        raise EvaluationError(
            "frozen opponents lack benchmark companions: " + ", ".join(missing_benchmarks)
        )
    manifest = _candidate_manifest(
        version_id=version_id,
        name=arguments.candidate_name,
        executable=executable,
        config=config,
        benchmark=benchmark,
        change_category=arguments.change_category,
    )
    if registered is not None:
        registered_manifest = registered["manifest"]
        manifest.update(
            {
                "artifact_binding_verified": bool(
                    registered_manifest.get("binary_sha256") == manifest["binary_sha256"]
                    and registered_manifest.get("benchmark_binary_sha256")
                    == manifest["benchmark_binary_sha256"]
                ),
                "registered_source_manifest_sha256": sha256_file(
                    Path(registered["directory"]) / "manifest.json"
                ),
                "commit": registered_manifest.get("commit"),
                "branch": registered_manifest.get("branch"),
                "dirty": registered_manifest.get("dirty"),
                "dirty_paths": registered_manifest.get("dirty_paths"),
                "build_type": registered_manifest.get("build_type"),
                "compiler": registered_manifest.get("compiler"),
                "compiler_flags": registered_manifest.get("compiler_flags"),
            }
        )
    if not arguments.allow_identical_binary:
        identical = [
            opponent["version_id"]
            for opponent in opponents
            if opponent["manifest"]["binary_sha256"] == manifest["binary_sha256"]
        ]
        if identical:
            raise EvaluationError(
                "candidate unexpectedly matches frozen opponent binary hash: " + ", ".join(identical)
            )
    opponent_names = "-".join(opponent["version_id"] for opponent in opponents)
    run_id = f"{timestamp_id()}_{version_id}_vs_{opponent_names}"
    base = Path(arguments.output_dir or root / "local_reports" / "evaluations").resolve()
    run_directory = base / run_id
    run_directory.mkdir(parents=True, exist_ok=False)
    for name in ("correctness", "performance", "selfplay", "games", "divergence", "failures"):
        (run_directory / name).mkdir()
    candidate = _snapshot_candidate(run_directory, executable, config, benchmark, manifest)
    opponent_snapshots = _copy_opponents(run_directory, opponents)
    return run_directory, candidate, manifest, opponent_snapshots


def _resume_run(run_directory: Path) -> tuple[dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    run_config = load_json(run_directory / "run_config.json")
    candidate_manifest = load_json(run_directory / "candidate" / "manifest.json")
    candidate = {
        "version_id": candidate_manifest["version_id"],
        "executable": str((run_directory / "candidate" / "qas.exe").resolve()),
        "config": str((run_directory / "candidate" / "engine_config.json").resolve()),
        "benchmark_executable": str((run_directory / "candidate" / "benchmark.exe").resolve()),
        "build_manifest": (
            str((run_directory / "candidate" / "build_manifest.json").resolve())
            if (run_directory / "candidate" / "build_manifest.json").is_file()
            else None
        ),
        "manifest": candidate_manifest,
    }
    registry = VersionRegistry()
    opponents: list[dict[str, Any]] = []
    for version_id in run_config["opponent_ids"]:
        directory = run_directory / "opponents" / version_id
        manifest = load_json(directory / "manifest.json")
        # Migrate runs made before opponent executables were snapshotted, but only from a
        # registry entry that still matches the hashes frozen into this run.
        if not (directory / "qas.exe").is_file() or not (directory / "benchmark.exe").is_file():
            frozen = registry.verify(version_id)
            if (
                frozen["manifest"]["binary_sha256"]
                != run_config["opponent_binary_hashes"][version_id]
                or frozen["manifest"]["config_sha256"]
                != run_config["opponent_config_hashes"][version_id]
            ):
                raise EvaluationError(f"cannot migrate changed frozen opponent: {version_id}")
            shutil.copy2(frozen["executable"], directory / "qas.exe")
            shutil.copy2(frozen["benchmark_executable"], directory / "benchmark.exe")
        opponents.append(_snapshot_opponent(directory, manifest))
    return candidate, candidate_manifest, opponents


def run_pipeline(arguments: argparse.Namespace) -> Path:
    profiles = _profiles()
    if arguments.profile not in profiles:
        raise EvaluationError(f"unknown evaluation profile: {arguments.profile}")
    profile = profiles[arguments.profile]
    policy = _policy()
    corpus_manifest, fixtures = load_corpus()
    referee_executable = (
        Path(arguments.referee).resolve()
        if arguments.referee
        else locate_companion(Path(sys.executable), "qas_evaluation_referee")
    )
    if referee_executable is None:
        referee_executable = locate_companion(
            default_build_directory() / "Release" / "qas.exe", "qas_evaluation_referee"
        )
    if referee_executable is None or not referee_executable.is_file():
        raise EvaluationError("authoritative qas_evaluation_referee executable is missing")

    if arguments.resume and arguments.replay_game:
        run_directory = Path(arguments.resume).resolve()
        replay_config = load_json(run_directory / "run_config.json")
        referee_executable = Path(replay_config["referee_path"])
        if (
            not referee_executable.is_file()
            or sha256_file(referee_executable) != replay_config.get("referee_sha256")
        ):
            raise EvaluationError("saved run referee hash mismatch")
        games_path = run_directory / "selfplay" / "games.jsonl"
        if not games_path.is_file():
            raise EvaluationError("saved game list is missing")
        matches = [
            json.loads(line)
            for line in games_path.read_text(encoding="utf-8").splitlines()
            if line and json.loads(line).get("game_id") == arguments.replay_game
        ]
        if not matches or not replay_game(matches[-1], referee_executable):
            raise EvaluationError(f"saved game replay failed: {arguments.replay_game}")
        return run_directory / "report.md"

    if arguments.resume:
        run_directory = Path(arguments.resume).resolve()
        candidate, candidate_manifest, opponents = _resume_run(run_directory)
        run_config = load_json(run_directory / "run_config.json")
        if run_config["profile"] != arguments.profile and arguments.profile != "smoke":
            raise EvaluationError("resume profile does not match original run")
        profile = run_config["profile_config"]
        benchmark_controls = apply_benchmark_controls(
            profile.get("cpu_affinity", "inherit"), profile.get("process_priority", "normal")
        )
        if len(benchmark_controls.get("affinity_after") or []) == 1:
            profile = {**profile, "cpu_affinity_core": benchmark_controls["affinity_after"][0]}
        recorded_referee = Path(run_config["referee_path"])
        expected_referee_hash = run_config.get("referee_sha256")
        if (
            not recorded_referee.is_file()
            or expected_referee_hash is None
            or sha256_file(recorded_referee) != expected_referee_hash
        ):
            raise EvaluationError("the authoritative referee changed or disappeared since run creation")
        referee_executable = recorded_referee
    else:
        run_directory, candidate, candidate_manifest, opponents = _initialize_run(arguments)
        referee_directory = run_directory / "referee"
        referee_directory.mkdir()
        referee_snapshot = referee_directory / "qas_evaluation_referee.exe"
        shutil.copy2(referee_executable, referee_snapshot)
        referee_executable = referee_snapshot.resolve()
        registry_data = VersionRegistry().load()
        benchmark_controls = apply_benchmark_controls(
            profile.get("cpu_affinity", "inherit"), profile.get("process_priority", "normal")
        )
        if len(benchmark_controls.get("affinity_after") or []) == 1:
            profile = {**profile, "cpu_affinity_core": benchmark_controls["affinity_after"][0]}
        run_config = {
            "schema_version": 2,
            "run_id": run_directory.name,
            "created_at_utc": utc_now(),
            "profile": arguments.profile,
            "profile_config": profile,
            "policy_sha256": canonical_json_hash(policy),
            "change_category": arguments.change_category,
            "seed": arguments.seed,
            "candidate_version_id": candidate["version_id"],
            "candidate_original_executable": candidate_manifest["original_executable_path"],
            "candidate_binary_sha256": candidate_manifest["binary_sha256"],
            "candidate_config_sha256": candidate_manifest["config_sha256"],
            "candidate_config_canonical_sha256": canonical_json_hash(load_json(Path(candidate["config"]))),
            "opponent_ids": [opponent["version_id"] for opponent in opponents],
            "opponent_binary_hashes": {
                opponent["version_id"]: opponent["manifest"]["binary_sha256"]
                for opponent in opponents
            },
            "opponent_config_hashes": {
                opponent["version_id"]: opponent["manifest"]["config_sha256"]
                for opponent in opponents
            },
            "opponent_config_canonical_hashes": {
                opponent["version_id"]: canonical_json_hash(load_json(Path(opponent["config"])))
                for opponent in opponents
            },
            "stable_anchor": registry_data["stable_anchor"],
            "current_champion": registry_data["current_champion"],
            "corpus_revision": corpus_manifest["corpus_revision"],
            "corpus_sha256": corpus_manifest["fixtures_sha256"],
            "referee_path": str(referee_executable),
            "referee_sha256": sha256_file(referee_executable),
            "host_cpu": cpu_model(),
            "host_os": os.name,
            "host_load_average": list(os.getloadavg()) if hasattr(os, "getloadavg") else None,
            "process_affinity": process_affinity(),
            "benchmark_controls": benchmark_controls,
            "power_mode": "unavailable",
        }
        run_config["run_config_hash"] = canonical_json_hash(run_config)
        atomic_write_json(run_directory / "run_config.json", run_config)
    if canonical_json_hash({key: value for key, value in run_config.items() if key != "run_config_hash"}) != run_config[
        "run_config_hash"
    ]:
        raise EvaluationError("run configuration hash mismatch")
    if run_config["corpus_sha256"] != corpus_manifest["fixtures_sha256"]:
        raise EvaluationError("corpus changed since run creation")
    if run_config["policy_sha256"] != canonical_json_hash(policy):
        raise EvaluationError("acceptance policy changed since run creation")
    corpus_coverage = summarize_corpus(fixtures)
    atomic_write_json(run_directory / "correctness" / "corpus_coverage.json", corpus_coverage)
    _verify_all_inputs(
        candidate,
        candidate_manifest,
        run_config["opponent_ids"],
        require_original_candidate=not bool(arguments.resume),
    )
    progress = Progress(
        run_directory / "progress.json",
        {
            "schema_version": 2,
            "run_config_hash": run_config["run_config_hash"],
            "candidate_binary_hash": candidate_manifest["binary_sha256"],
            "candidate_config_hash": candidate_manifest["config_sha256"],
            "opponent_binary_hashes": run_config["opponent_binary_hashes"],
            "opponent_config_hashes": run_config["opponent_config_hashes"],
            "corpus_hash": run_config["corpus_sha256"],
            "planned_benchmark_tasks": [],
            "completed_tasks": [],
            "task_results": {},
            "task_result_hashes": {},
            "planned_game_ids": [],
            "completed_game_ids": [],
            "incomplete_game_ids": [],
            "failed_game_ids": [],
            "current_attempt": 1,
            "last_flushed_move": None,
            "report_generation_state": "pending",
            "current_stage": "validation",
        },
    )
    if progress.value["run_config_hash"] != run_config["run_config_hash"]:
        raise EvaluationError("resume progress belongs to a different run configuration")
    progress_bindings = {
        "candidate_binary_hash": run_config["candidate_binary_sha256"],
        "candidate_config_hash": run_config["candidate_config_sha256"],
        "opponent_binary_hashes": run_config["opponent_binary_hashes"],
        "opponent_config_hashes": run_config["opponent_config_hashes"],
        "corpus_hash": run_config["corpus_sha256"],
    }
    for field, expected in progress_bindings.items():
        if progress.value.get(field) != expected:
            raise EvaluationError(f"resume progress binding mismatch: {field}")
    attempt_id = int(progress.value["current_attempt"])
    if arguments.resume:
        attempt_id += 1
        progress.value["current_attempt"] = attempt_id
        progress.value["extension_pairs"] = int(progress.value.get("extension_pairs", 0)) + max(
            0, arguments.extend_pairs
        )
        progress.save()
    progress.announce(run_config, run_directory, bool(arguments.resume))

    candidate_spec = engine_spec(candidate)
    opponent_specs = [engine_spec(opponent) for opponent in opponents]
    candidate_config_value = load_json(candidate_spec.config)
    configuration_comparisons = {
        opponent.version_id: {
            "canonical_equal": canonical_json_hash(candidate_config_value)
            == canonical_json_hash(load_json(opponent.config)),
            "differences": _configuration_differences(
                candidate_config_value, load_json(opponent.config)
            ),
        }
        for opponent in opponent_specs
    }
    atomic_write_json(
        run_directory / "correctness" / "configuration_comparison.json",
        configuration_comparisons,
    )
    runtime_directory = run_directory / "runtime_configs"
    runtime_directory.mkdir(exist_ok=True)
    runtime_candidate = _runtime_engine(candidate_spec, profile, runtime_directory)
    runtime_opponents = [
        _runtime_engine(opponent, profile, runtime_directory) for opponent in opponent_specs
    ]

    progress.stage("infrastructure_validation")
    probes = {}
    for engine in [runtime_candidate, *runtime_opponents]:
        _verify_all_inputs(
            candidate,
            candidate_manifest,
            run_config["opponent_ids"],
            require_original_candidate=not bool(arguments.resume),
        )
        task = f"protocol-probe:{engine.version_id}"
        if progress.is_complete(task):
            probes[engine.version_id] = progress.cached_result(task)
        else:
            progress.task_started(task)
            probes[engine.version_id] = protocol_probe(
                engine, referee_executable, profile["move_hard_ms"], run_directory / "correctness"
            )
            progress.mark_complete(task, probes[engine.version_id])
    if not all(probe["passed"] for probe in probes.values()):
        raise EvaluationError("protocol compatibility validation failed")
    atomic_write_json(run_directory / "correctness" / "protocol_probes.json", probes)

    progress.stage("unit_and_rule_tests")
    _verify_all_inputs(candidate, candidate_manifest, run_config["opponent_ids"], not bool(arguments.resume))
    if progress.is_complete("ctest"):
        ctest_result = progress.cached_result("ctest")
    else:
        progress.task_started("ctest")
        ctest_result = run_ctest(Path(arguments.build_dir).resolve(), run_directory / "correctness")
        progress.mark_complete("ctest", ctest_result)

    selection = profile.get("performance_selection", {})
    performance_fixtures = select_performance(
        fixtures,
        profile["fixed_fixture_limit"],
        run_config["seed"],
        selection.get("required_fixture_ids"),
        selection.get("stratify_labels"),
    ) if profile["fixed_depths"] else []
    time_fixtures = select_performance(
        fixtures,
        profile["time_fixture_limit"],
        run_config["seed"] ^ 0x54494D45,
        selection.get("required_fixture_ids"),
        selection.get("stratify_labels"),
    ) if profile["time_budgets_ms"] else []
    atomic_write_json(
        run_directory / "performance" / "fixture_selection.json",
        {
            "strategy": selection.get("strategy", "stratified"),
            "fixed_fixture_ids": [fixture["fixture_id"] for fixture in performance_fixtures],
            "time_fixture_ids": [fixture["fixture_id"] for fixture in time_fixtures],
            "fixed_category_counts": {
                label: sum(label in fixture["category_labels"] for fixture in performance_fixtures)
                for label in selection.get("stratify_labels", [])
            },
            "time_category_counts": {
                label: sum(label in fixture["category_labels"] for fixture in time_fixtures)
                for label in selection.get("stratify_labels", [])
            },
        },
    )
    if not progress.value["planned_benchmark_tasks"]:
        planned: list[str] = []
        for opponent in opponent_specs:
            for fixture in performance_fixtures:
                for depth in profile["fixed_depths"]:
                    for repetition in range(int(profile.get("fixed_warmup_repetitions", 0))):
                        for version_id in (candidate_spec.version_id, opponent.version_id):
                            planned.append(
                                f"fixed-warmup:{opponent.version_id}:{fixture['fixture_id']}:{depth}:{repetition}:{version_id}"
                            )
                    for repetition in range(profile["fixed_repetitions"]):
                        for version_id in (candidate_spec.version_id, opponent.version_id):
                            planned.append(
                                f"fixed:{opponent.version_id}:{fixture['fixture_id']}:{depth}:{repetition}:{version_id}"
                            )
                    for repetition in range(int(profile.get("diagnostic_repetitions", 0))):
                        for version_id in (candidate_spec.version_id, opponent.version_id):
                            planned.append(
                                f"fixed-diagnostic:{opponent.version_id}:{fixture['fixture_id']}:{depth}:{repetition}:{version_id}"
                            )
            for fixture in time_fixtures:
                for budget in profile["time_budgets_ms"]:
                    for repetition in range(int(profile.get("time_warmup_repetitions", 0))):
                        for version_id in (candidate_spec.version_id, opponent.version_id):
                            planned.append(
                                f"time-warmup:{opponent.version_id}:{fixture['fixture_id']}:{budget}:{repetition}:{version_id}"
                            )
                    for repetition in range(int(profile.get("time_repetitions", 1))):
                        for version_id in (candidate_spec.version_id, opponent.version_id):
                            planned.append(
                                f"time:{opponent.version_id}:{fixture['fixture_id']}:{budget}:{repetition}:{version_id}"
                            )
                    for repetition in range(int(profile.get("time_diagnostic_repetitions", 0))):
                        for version_id in (candidate_spec.version_id, opponent.version_id):
                            planned.append(
                                f"time-diagnostic:{opponent.version_id}:{fixture['fixture_id']}:{budget}:{repetition}:{version_id}"
                            )
        if policy.get("category_requirements", {}).get(
            run_config["change_category"], {}
        ).get("require_reference_checks"):
            reference_depth = int(profile.get("reference_check_depth", 4))
            for fixture in performance_fixtures[: int(profile.get("reference_check_fixture_limit", 4))]:
                for label in ("active", "reference"):
                    planned.append(f"reference-{label}:{fixture['fixture_id']}:{reference_depth}")
        progress.value["planned_benchmark_tasks"] = planned
        progress.save()
    progress.stage("fixed_depth_correctness_and_performance")
    _verify_all_inputs(candidate, candidate_manifest, run_config["opponent_ids"], not bool(arguments.resume))
    fixed_records = run_fixed_depth(
        candidate_spec,
        opponent_specs,
        performance_fixtures,
        profile,
        run_directory / "performance",
        progress.is_complete,
        progress.cached_result,
        progress.task_started,
        progress.mark_complete,
    )
    fixed_correctness = compare_fixed_depth(
        fixed_records,
        candidate_spec.version_id,
        [opponent.version_id for opponent in opponent_specs],
        run_config["change_category"],
        run_directory / "correctness" / "fixed_depth_comparison.csv",
    )
    reference_checks = _run_reference_checks(
        candidate=candidate_spec,
        fixtures=performance_fixtures,
        profile=profile,
        change_category=run_config["change_category"],
        policy=policy,
        output_directory=run_directory / "correctness",
        progress=progress,
    )
    correctness = {
        "artifact_binding_verified": bool(candidate_manifest.get("artifact_binding_verified")),
        "ctest_passed": ctest_result["passed"],
        "protocol_probes_passed": all(probe["passed"] for probe in probes.values()),
        "fixed_depth": fixed_correctness,
        "reference_checks": reference_checks,
        "reference_checks_passed": reference_checks["passed"],
        "configurations_equivalent": all(
            comparison["canonical_equal"] for comparison in configuration_comparisons.values()
        ),
        "configuration_comparisons": configuration_comparisons,
        "passed": ctest_result["passed"]
        and all(probe["passed"] for probe in probes.values())
        and fixed_correctness["passed"]
        and reference_checks["passed"],
    }
    atomic_write_json(run_directory / "correctness" / "summary.json", correctness)
    performance_summary = summarize_fixed(
        fixed_records,
        candidate_spec.version_id,
        [opponent.version_id for opponent in opponent_specs],
        policy["performance_upgrade"]["maximum_critical_fixture_elapsed_regression"],
    )
    atomic_write_json(run_directory / "performance" / "summary.json", performance_summary)
    write_fixed_summaries(run_directory / "performance", performance_summary)

    progress.stage("time_controlled_performance")
    _verify_all_inputs(candidate, candidate_manifest, run_config["opponent_ids"], not bool(arguments.resume))
    time_records = run_time_controlled(
        candidate_spec,
        opponent_specs,
        time_fixtures,
        profile,
        run_directory / "performance",
        progress.is_complete,
        progress.cached_result,
        progress.task_started,
        progress.mark_complete,
    )
    time_summary = summarize_time_controlled(
        time_records,
        candidate_spec.version_id,
        [opponent.version_id for opponent in opponent_specs],
        float(profile.get("minimum_sample_duration_ms", 0.0)),
        int(profile.get("minimum_authoritative_repetitions", 1)),
    )
    atomic_write_json(run_directory / "performance" / "time_summary.json", time_summary)
    write_time_summaries(run_directory / "performance", time_summary)
    corpus_coverage = enrich_corpus_with_diagnostics(
        corpus_coverage,
        [*fixed_records, *time_records],
        candidate_spec.version_id,
    )
    atomic_write_json(run_directory / "correctness" / "corpus_coverage.json", corpus_coverage)

    _verify_all_inputs(candidate, candidate_manifest, run_config["opponent_ids"], not bool(arguments.resume))
    extension_pairs = int(progress.value.get("extension_pairs", 0))
    openings_by_opponent: dict[str, list[dict[str, Any]]] = {}
    for opponent in runtime_opponents:
        base_count = int(profile["opening_pairs"])
        if (
            opponent.version_id == run_config["stable_anchor"]
            and run_config["stable_anchor"] != run_config["current_champion"]
            and int(profile.get("anchor_opening_pairs", 0)) > 0
        ):
            base_count = int(profile["anchor_opening_pairs"])
        openings_by_opponent[opponent.version_id] = select_openings(
            fixtures,
            base_count + extension_pairs,
            run_config["seed"],
            profile.get("opening_plies"),
        )
    planned_games = [
        f"{opponent.version_id}_pair_{pair_index:04d}_{role}"
        for opponent in runtime_opponents
        for pair_index in range(len(openings_by_opponent[opponent.version_id]))
        for role in ("candidate_first", "candidate_second")
    ]
    progress.value["planned_game_ids"] = planned_games
    progress.value["incomplete_game_ids"] = [
        game_id
        for game_id in planned_games
        if game_id not in progress.value["completed_game_ids"]
    ]
    progress.save()
    progress.stage("paired_selfplay")
    games = run_paired_matches(
        run_id=run_config["run_id"],
        candidate=runtime_candidate,
        opponents=runtime_opponents,
        openings=openings_by_opponent,
        referee_executable=referee_executable,
        profile=profile,
        run_directory=run_directory,
        attempt_id=attempt_id,
        is_complete=progress.is_complete,
        task_started=progress.task_started,
        move_completed=lambda record: progress.record_move(
            record, int(profile.get("progress_ply_interval", 5))
        ),
        mark_complete=progress.mark_complete,
    )

    progress.stage("divergence_analysis")
    moves = _load_moves(run_directory)
    divergences = analyze_divergences(
        fixed_records=fixed_records,
        time_records=time_records,
        move_records=moves,
        fixtures=fixtures,
        candidate=candidate_spec,
        opponents=opponent_specs,
        change_category=run_config["change_category"],
        referee_executable=referee_executable,
        output_directory=run_directory / "divergence",
        profile=profile,
    )

    progress.stage("statistical_summary")
    game_summaries = summarize_games(
        games,
        moves,
        candidate_spec.version_id,
        max(profile["bootstrap_samples"], policy["minimum_bootstrap_samples"]),
        policy["bootstrap_seed"] ^ run_config["seed"],
        policy["bootstrap_confidence"],
        minimum_pairs={
            opponent.version_id: (
                int(profile.get("minimum_anchor_pairs_for_strength", 0))
                if opponent.version_id == run_config["stable_anchor"]
                and run_config["stable_anchor"] != run_config["current_champion"]
                else int(profile.get("minimum_pairs_for_strength", 0))
            )
            for opponent in runtime_opponents
        },
        maximum_pairs={
            opponent.version_id: sum(
                "openings" in fixture["category_labels"]
                and (
                    not profile.get("opening_plies")
                    or int(fixture["turn"]) in set(profile["opening_plies"])
                )
                for fixture in fixtures
            )
            for opponent in runtime_opponents
        },
    )
    reliability = reliability_summary(game_summaries)
    _verify_all_inputs(
        candidate,
        candidate_manifest,
        run_config["opponent_ids"],
        require_original_candidate=not bool(arguments.resume),
    )
    pair_rows = []
    for game in games:
        pair_rows.append(
            {
                "opponent": game["opponent_version"],
                "pair_id": game["pair_id"],
                "game_id": game["game_id"],
                "candidate_first": game["engine_first"] == candidate_spec.version_id,
                "candidate_result": game["candidate_result"],
                "valid_strength_game": game["valid_strength_game"],
                "terminal_type": game["terminal_type"],
            }
        )
    if pair_rows:
        write_csv(run_directory / "selfplay" / "pair_results.csv", pair_rows, list(pair_rows[0]))
    engine_rows = list(game_summaries.values())
    if engine_rows:
        flattened = [
            {
                **{key: value for key, value in row.items() if key != "bootstrap"},
                "bootstrap": json.dumps(row["bootstrap"], sort_keys=True),
            }
            for row in engine_rows
        ]
        write_csv(run_directory / "selfplay" / "engine_results.csv", flattened, list(flattened[0]))
    classification, gates = classify_candidate(
        change_category=run_config["change_category"],
        profile=profile,
        policy=policy,
        correctness=correctness,
        reliability=reliability,
        performance=performance_summary,
        game_summaries=game_summaries,
        champion_id=run_config["current_champion"],
        anchor_id=run_config["stable_anchor"],
        time_performance=time_summary,
    )
    atomic_write_json(run_directory / "selfplay" / "summary.json", game_summaries)
    atomic_write_json(run_directory / "selfplay" / "reliability.json", reliability)

    progress.stage("report_generation")
    report = generate_report(
        run_directory=run_directory,
        run_config=run_config,
        candidate=candidate_manifest,
        opponents=[opponent["manifest"] for opponent in opponents],
        correctness=correctness,
        performance=performance_summary,
        time_controlled=time_records,
        time_summary=time_summary,
        game_summaries=game_summaries,
        reliability=reliability,
        divergences=divergences,
        classification=classification,
        gates=gates,
        corpus_coverage=corpus_coverage,
    )
    manifest = {
        "schema_version": 1,
        "run_id": run_config["run_id"],
        "completed_at_utc": utc_now(),
        "complete": True,
        "classification": classification,
        "gates": gates,
        "report_path": str(report.resolve()),
        "candidate_binary_sha256": candidate_manifest["binary_sha256"],
        "corpus_sha256": run_config["corpus_sha256"],
    }
    atomic_write_json(run_directory / "manifest.json", manifest)
    progress.value["report_generation_state"] = "complete"
    progress.stage("complete")
    progress.log(f"RESULT classification={classification} report={report}")
    return report


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the permanent QAS version-evaluation pipeline")
    parser.add_argument("--candidate-exe")
    parser.add_argument("--candidate-name")
    parser.add_argument("--change-category")
    parser.add_argument("--profile", default="smoke", choices=sorted(_profiles()))
    parser.add_argument("--candidate-config")
    parser.add_argument("--candidate-version-id")
    parser.add_argument("--candidate-benchmark")
    parser.add_argument("--registered-candidate")
    parser.add_argument("--opponents")
    parser.add_argument("--include-previous", action="store_true")
    parser.add_argument("--output-dir")
    parser.add_argument("--seed", type=int, default=0x51415335)
    parser.add_argument("--resume")
    parser.add_argument("--referee")
    parser.add_argument("--build-dir", default=str(default_build_directory()))
    parser.add_argument("--allow-identical-binary", action="store_true")
    parser.add_argument("--extend-pairs", type=int, default=0)
    parser.add_argument("--replay-game")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        arguments = parse_arguments(argv)
        report = run_pipeline(arguments)
        print(f"Evaluation report: {report}")
        return 0
    except KeyboardInterrupt:
        print("Evaluation interrupted; completed progress was saved atomically.", file=sys.stderr)
        return 130
    except EvaluationError as error:
        print(f"Evaluation infrastructure failure: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
