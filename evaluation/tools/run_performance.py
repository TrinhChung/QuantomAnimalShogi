from __future__ import annotations

import ctypes
import json
import math
import os
import statistics
import subprocess
import time
from collections import Counter, defaultdict
from itertools import count
from pathlib import Path
from typing import Any, Iterable, Sequence

from .common import percentile, process_affinity, write_csv
from .referee import EngineSpec


RAW_FIELDS = [
    "stage",
    "version",
    "opponent",
    "fixture_id",
    "depth",
    "budget_ms",
    "repetition",
    "diagnostic",
    "warmup",
    "pair_order",
    "order_in_repetition",
    "execution_order",
    "cpu_affinity_core",
    "cpu_affinity_controlled",
    "cpu_affinity_reason",
    "process_priority",
    "process_priority_controlled",
    "process_priority_reason",
    "requested_depth",
    "requested_soft_limit_ms",
    "requested_hard_limit_ms",
    "minimum_sample_duration_ms",
    "minimum_authoritative_repetitions",
    "best_action",
    "score",
    "completed",
    "search_completed",
    "legal_result",
    "legal_count",
    "legal_actions",
    "terminal_result",
    "state_hash_before",
    "state_hash_after",
    "completed_depth",
    "started_depth",
    "timeout",
    "fallback_used",
    "fallback_reason",
    "elapsed_wall_ms",
    "elapsed_external_ms",
    "process_envelope_ms",
    "external_timing_valid",
    "benchmark_schema_version",
    "benchmark_semantics_valid",
    "elapsed_process_cpu_ms",
    "nodes",
    "nps",
    "pv_actions",
    "instrumentation_enabled",
    "soft_limit_ms",
    "hard_limit_ms",
    "soft_limit_reached",
    "hard_limit_reached",
    "deadline_overshoot_ms",
    "timeout_depth",
    "completed_depth_reports",
    "expanded_nodes",
    "generated_legal_moves",
    "max_legal_moves",
    "equivalent_successor_moves",
    "tt_probes",
    "tt_hits",
    "tt_hit_rate",
    "tt_exact_hits",
    "tt_lower_hits",
    "tt_upper_hits",
    "tt_cutoffs",
    "tt_replacements",
    "tt_collisions",
    "tt_age_replacements",
    "tt_depth_replacements",
    "tt_move_used",
    "cutoffs",
    "first_move_cutoffs",
    "first_move_cutoff_rate",
    "average_cutoff_rank",
    "killer_cutoffs",
    "history_cutoffs",
    "pvs_researches",
    "aspiration_retries",
    "leq_calls",
    "leq_input_moves",
    "leq_output_representatives",
    "leq_duplicate_ratio",
    "leq_grouping_ms",
    "leq_skipped_moves",
    "canonicalize_calls",
    "canonicalize_ms",
    "propagation_calls",
    "propagation_iterations",
    "propagation_ms",
    "movegen_calls",
    "movegen_ms",
    "move_order_calls",
    "move_order_ms",
    "evaluation_calls",
    "evaluation_ms",
    "rule_metrics",
    "evaluation_components",
    "peak_memory_bytes",
    "error_type",
    "error_message",
]


OPTIONAL_NATIVE_FIELDS = {
    "nps",
    "instrumentation_enabled",
    "soft_limit_ms",
    "hard_limit_ms",
    "soft_limit_reached",
    "hard_limit_reached",
    "deadline_overshoot_ms",
    "timeout_depth",
    "completed_depth_reports",
    "expanded_nodes",
    "generated_legal_moves",
    "max_legal_moves",
    "equivalent_successor_moves",
    "tt_probes",
    "tt_hits",
    "tt_hit_rate",
    "tt_exact_hits",
    "tt_lower_hits",
    "tt_upper_hits",
    "tt_cutoffs",
    "tt_replacements",
    "tt_collisions",
    "tt_age_replacements",
    "tt_depth_replacements",
    "tt_move_used",
    "cutoffs",
    "first_move_cutoffs",
    "first_move_cutoff_rate",
    "average_cutoff_rank",
    "killer_cutoffs",
    "history_cutoffs",
    "pvs_researches",
    "aspiration_retries",
    "leq_calls",
    "leq_input_moves",
    "leq_output_representatives",
    "leq_duplicate_ratio",
    "leq_grouping_ms",
    "leq_skipped_moves",
    "canonicalize_calls",
    "canonicalize_ms",
    "propagation_calls",
    "propagation_iterations",
    "propagation_ms",
    "movegen_calls",
    "movegen_ms",
    "move_order_calls",
    "move_order_ms",
    "evaluation_calls",
    "evaluation_ms",
    "rule_metrics",
    "evaluation_components",
}


DIAGNOSTIC_METRICS = (
    "expanded_nodes",
    "generated_legal_moves",
    "max_legal_moves",
    "equivalent_successor_moves",
    "tt_probes",
    "tt_hits",
    "tt_exact_hits",
    "tt_lower_hits",
    "tt_upper_hits",
    "tt_cutoffs",
    "tt_replacements",
    "tt_collisions",
    "tt_age_replacements",
    "tt_depth_replacements",
    "tt_move_used",
    "cutoffs",
    "first_move_cutoffs",
    "average_cutoff_rank",
    "killer_cutoffs",
    "history_cutoffs",
    "pvs_researches",
    "aspiration_retries",
    "leq_calls",
    "leq_input_moves",
    "leq_output_representatives",
    "leq_skipped_moves",
    "leq_grouping_ms",
    "canonicalize_calls",
    "canonicalize_ms",
    "propagation_calls",
    "propagation_iterations",
    "propagation_ms",
    "movegen_calls",
    "movegen_ms",
    "move_order_calls",
    "move_order_ms",
    "evaluation_calls",
    "evaluation_ms",
)


def _priority_creation_flags(priority: str | None) -> tuple[int, bool, str]:
    if priority is None:
        return 0, False, "not_requested"
    normalized = priority.lower()
    if os.name != "nt":
        return 0, False, "configured_after_launch"
    mapping = {
        "idle": getattr(subprocess, "IDLE_PRIORITY_CLASS", 0),
        "below_normal": getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0),
        "normal": getattr(subprocess, "NORMAL_PRIORITY_CLASS", 0),
        "above_normal": getattr(subprocess, "ABOVE_NORMAL_PRIORITY_CLASS", 0),
        "high": getattr(subprocess, "HIGH_PRIORITY_CLASS", 0),
    }
    if normalized not in mapping or mapping[normalized] == 0:
        return 0, False, f"unsupported_priority:{priority}"
    return mapping[normalized], True, "controlled"


def _set_process_affinity(process: subprocess.Popen[str], core: int | None) -> tuple[bool, str]:
    if core is None:
        return False, "not_requested"
    if core < 0:
        return False, "invalid_core"
    try:
        if os.name == "nt":
            mask = ctypes.c_size_t(1 << core)
            if not ctypes.windll.kernel32.SetProcessAffinityMask(process._handle, mask):  # type: ignore[attr-defined]
                return False, f"windows_error:{ctypes.get_last_error()}"
        elif hasattr(os, "sched_setaffinity"):
            os.sched_setaffinity(process.pid, {core})
        else:
            return False, "platform_does_not_support_affinity"
    except (AttributeError, OSError, OverflowError, ValueError) as error:
        return False, f"affinity_failed:{error}"
    return True, "controlled"


def _set_posix_priority(process: subprocess.Popen[str], priority: str | None) -> tuple[bool, str]:
    if priority is None:
        return False, "not_requested"
    if os.name == "nt":
        _, controlled, reason = _priority_creation_flags(priority)
        return controlled, reason
    if not hasattr(os, "setpriority"):
        return False, "platform_does_not_support_priority"
    mapping = {"idle": 19, "below_normal": 10, "normal": 0, "above_normal": -5, "high": -10}
    normalized = priority.lower()
    if normalized not in mapping:
        return False, f"unsupported_priority:{priority}"
    try:
        os.setpriority(os.PRIO_PROCESS, process.pid, mapping[normalized])
    except OSError as error:
        return False, f"priority_failed:{error}"
    return True, "controlled"


def _run_controlled_process(
    command: list[str],
    input_text: str,
    timeout_seconds: float,
    cpu_affinity_core: int | None,
    process_priority: str | None,
) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
    external_started = time.perf_counter()
    creation_flags, windows_priority_controlled, windows_priority_reason = (
        _priority_creation_flags(process_priority)
    )
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        creationflags=creation_flags,
    )
    affinity_controlled, affinity_reason = _set_process_affinity(process, cpu_affinity_core)
    if os.name == "nt":
        priority_controlled, priority_reason = (
            windows_priority_controlled,
            windows_priority_reason,
        )
    else:
        priority_controlled, priority_reason = _set_posix_priority(process, process_priority)
    controls = {
        "cpu_affinity_core": cpu_affinity_core,
        "cpu_affinity_controlled": affinity_controlled,
        "cpu_affinity_reason": affinity_reason,
        "process_priority": process_priority,
        "process_priority_controlled": priority_controlled,
        "process_priority_reason": priority_reason,
    }
    try:
        stdout, stderr = process.communicate(input=input_text, timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        process.kill()
        process.communicate()
        error.benchmark_controls = controls  # type: ignore[attr-defined]
        raise
    controls["elapsed_external_ms"] = (time.perf_counter() - external_started) * 1000.0
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr), controls


def _run_driver(
    engine: EngineSpec,
    fixture: dict[str, Any],
    *,
    mode: str,
    depth: int,
    soft_ms: int,
    hard_ms: int,
    tt_size_mb: int,
    diagnostic: bool,
    cpu_affinity_core: int | None = None,
    process_priority: str | None = None,
) -> dict[str, Any]:
    if engine.benchmark_executable is None or not engine.benchmark_executable.is_file():
        return {
            "error_type": "missing_benchmark_companion",
            "error_message": "version lacks its frozen qas_evaluation_benchmark companion",
            "completed": False,
            "legal_result": False,
            "cpu_affinity_core": cpu_affinity_core,
            "cpu_affinity_controlled": False,
            "cpu_affinity_reason": "process_not_launched",
            "process_priority": process_priority,
            "process_priority_controlled": False,
            "process_priority_reason": "process_not_launched",
        }
    command = [
        str(engine.benchmark_executable),
        "--mode",
        mode,
        "--config",
        str(engine.config),
        "--depth",
        str(depth),
        "--soft-ms",
        str(soft_ms),
        "--hard-ms",
        str(hard_ms),
        "--tt-size-mb",
        str(tt_size_mb),
    ]
    if diagnostic:
        command.append("--diagnostic")
    try:
        completed, controls = _run_controlled_process(
            command,
            fixture["full_serialized_state"],
            max(5.0, hard_ms / 1000.0 + 5.0),
            cpu_affinity_core,
            process_priority,
        )
    except subprocess.TimeoutExpired as error:
        controls = getattr(error, "benchmark_controls", {})
        return {
            "error_type": "external_timeout",
            "error_message": str(error),
            "completed": False,
            "legal_result": False,
            "timeout": True,
            "cpu_affinity_core": cpu_affinity_core,
            "cpu_affinity_controlled": controls.get("cpu_affinity_controlled", False),
            "cpu_affinity_reason": controls.get("cpu_affinity_reason", "external_timeout"),
            "process_priority": process_priority,
            "process_priority_controlled": controls.get("process_priority_controlled", False),
            "process_priority_reason": controls.get("process_priority_reason", "external_timeout"),
        }
    if completed.returncode != 0:
        return {
            "error_type": "benchmark_process_failure",
            "error_message": completed.stderr.strip(),
            "completed": False,
            "legal_result": False,
            **controls,
        }
    lines = completed.stdout.strip().splitlines()
    if len(lines) != 1:
        return {
            "error_type": "benchmark_protocol_failure",
            "error_message": f"expected one JSON line, received {len(lines)}",
            "completed": False,
            "legal_result": False,
            **controls,
        }
    try:
        result = json.loads(lines[0])
    except json.JSONDecodeError as error:
        return {
            "error_type": "benchmark_protocol_failure",
            "error_message": str(error),
            "completed": False,
            "legal_result": False,
            **controls,
        }
    schema_version = result.get("schema_version")
    if schema_version not in (1, 2):
        return {
            "error_type": "benchmark_protocol_failure",
            "error_message": f"unsupported benchmark schema_version: {schema_version!r}",
            "completed": False,
            "legal_result": False,
            **controls,
        }
    required = {
        "best_action",
        "score",
        "completed",
        "search_completed",
        "legal_result",
        "legal_actions",
        "state_hash_before",
        "state_hash_after",
        "completed_depth",
        "started_depth",
        "elapsed_wall_ms",
        "nodes",
        "peak_memory_bytes",
    }
    missing = sorted(required - result.keys())
    if missing:
        return {
            "error_type": "benchmark_protocol_failure",
            "error_message": "missing benchmark fields: " + ", ".join(missing),
            "completed": False,
            "legal_result": False,
            **controls,
        }
    semantic_failures: list[str] = []
    if result.get("mode") not in (None, mode):
        semantic_failures.append("mode")
    if int(result.get("state_hash_before", -1)) != int(fixture["structural_hash"]):
        semantic_failures.append("state_hash_before")
    legal_actions = result.get("legal_actions")
    if not isinstance(legal_actions, list) or int(result.get("legal_count", -1)) != len(legal_actions):
        semantic_failures.append("legal_action_set")
    if result.get("best_action") not in (legal_actions or []):
        semantic_failures.append("best_action_legality")
    if not result.get("legal_result"):
        semantic_failures.append("legal_result")
    internal_elapsed = result.get("elapsed_wall_ms")
    external_elapsed = controls.get("elapsed_external_ms")
    external_timing_valid = (
        isinstance(internal_elapsed, (int, float))
        and isinstance(external_elapsed, (int, float))
        and internal_elapsed >= 0.0
        and internal_elapsed <= external_elapsed + 2.0
    )
    if not external_timing_valid:
        semantic_failures.append("elapsed_wall_ms")
    if semantic_failures:
        return {
            "error_type": "benchmark_semantic_failure",
            "error_message": "invalid benchmark fields: " + ", ".join(semantic_failures),
            "completed": False,
            "legal_result": False,
            "benchmark_schema_version": schema_version,
            "benchmark_semantics_valid": False,
            **controls,
        }
    for field in OPTIONAL_NATIVE_FIELDS:
        result.setdefault(field, None)
    result["benchmark_schema_version"] = schema_version
    result["benchmark_semantics_valid"] = True
    result["external_timing_valid"] = external_timing_valid
    result["elapsed_external_ms"] = external_elapsed
    result["process_envelope_ms"] = external_elapsed - float(internal_elapsed)
    result.update(controls)
    return result


def run_benchmark_once(
    engine: EngineSpec,
    fixture: dict[str, Any],
    *,
    mode: str,
    depth: int,
    soft_ms: int,
    hard_ms: int,
    tt_size_mb: int,
    diagnostic: bool = False,
    cpu_affinity_core: int | None = None,
    process_priority: str | None = None,
) -> dict[str, Any]:
    """Run one companion search for audits such as pruning-off reference checks."""
    return _run_driver(
        engine,
        fixture,
        mode=mode,
        depth=depth,
        soft_ms=soft_ms,
        hard_ms=hard_ms,
        tt_size_mb=tt_size_mb,
        diagnostic=diagnostic,
        cpu_affinity_core=cpu_affinity_core,
        process_priority=process_priority,
    )


def _decorate(
    result: dict[str, Any],
    *,
    stage: str,
    engine: EngineSpec,
    opponent: str,
    fixture_id: str,
    depth: int,
    budget_ms: int | None,
    repetition: int,
    diagnostic: bool,
    warmup: bool,
    pair_order: str,
    order_in_repetition: int,
    execution_order: int,
    requested_soft_limit_ms: int,
    requested_hard_limit_ms: int,
    minimum_sample_duration_ms: float,
    minimum_authoritative_repetitions: int,
) -> dict[str, Any]:
    return {
        "stage": stage,
        "version": engine.version_id,
        "opponent": opponent,
        "fixture_id": fixture_id,
        "depth": depth,
        "budget_ms": budget_ms,
        "repetition": repetition,
        "diagnostic": diagnostic,
        "warmup": warmup,
        "pair_order": pair_order,
        "order_in_repetition": order_in_repetition,
        "execution_order": execution_order,
        "requested_depth": depth,
        "requested_soft_limit_ms": requested_soft_limit_ms,
        "requested_hard_limit_ms": requested_hard_limit_ms,
        "minimum_sample_duration_ms": minimum_sample_duration_ms,
        "minimum_authoritative_repetitions": minimum_authoritative_repetitions,
        **result,
    }


def _profile_count(profile: dict[str, Any], *names: str, default: int = 0) -> int:
    for name in names:
        if name in profile:
            return max(0, int(profile[name]))
    return max(0, default)


def _minimum_timing_policy(profile: dict[str, Any]) -> tuple[float, int]:
    return (
        max(0.0, float(profile.get("minimum_sample_duration_ms", 0.0))),
        max(1, int(profile.get("minimum_authoritative_repetitions", 2))),
    )


def _profile_affinity_core(profile: dict[str, Any]) -> int | None:
    explicit = profile.get("cpu_affinity_core")
    if explicit is not None:
        return int(explicit)
    if profile.get("cpu_affinity") != "single_core":
        return None
    available = process_affinity()
    return available[0] if available else None


def _engine_order(
    candidate: EngineSpec, opponent: EngineSpec, repetition: int
) -> tuple[tuple[EngineSpec, EngineSpec], str]:
    if repetition % 2 == 0:
        return (candidate, opponent), "candidate_then_opponent"
    return (opponent, candidate), "opponent_then_candidate"


def _cached_task(
    task: str,
    legacy_tasks: Sequence[str],
    is_complete: callable,
    cached_result: callable,
) -> dict[str, Any] | None:
    for candidate_task in (task, *legacy_tasks):
        if is_complete(candidate_task):
            return dict(cached_result(candidate_task))
    return None


def _execute_task(
    *,
    task: str,
    legacy_tasks: Sequence[str],
    engine: EngineSpec,
    opponent_id: str,
    fixture: dict[str, Any],
    stage: str,
    mode: str,
    depth: int,
    budget_ms: int | None,
    soft_ms: int,
    hard_ms: int,
    tt_size_mb: int,
    repetition: int,
    diagnostic: bool,
    warmup: bool,
    pair_order: str,
    order_in_repetition: int,
    execution_order: int,
    cpu_affinity_core: int | None,
    process_priority: str | None,
    minimum_sample_duration_ms: float,
    minimum_authoritative_repetitions: int,
    is_complete: callable,
    cached_result: callable,
    task_started: callable,
    mark_complete: callable,
) -> dict[str, Any]:
    cached = _cached_task(task, legacy_tasks, is_complete, cached_result)
    if cached is not None:
        for field in OPTIONAL_NATIVE_FIELDS:
            cached.setdefault(field, None)
        cached.setdefault("warmup", warmup)
        cached.setdefault("pair_order", pair_order)
        cached.setdefault("order_in_repetition", order_in_repetition)
        cached.setdefault("execution_order", execution_order)
        cached.setdefault("requested_depth", depth)
        cached.setdefault("requested_soft_limit_ms", soft_ms)
        cached.setdefault("requested_hard_limit_ms", hard_ms)
        cached.setdefault("cpu_affinity_core", cpu_affinity_core)
        cached.setdefault("cpu_affinity_controlled", None)
        cached.setdefault("cpu_affinity_reason", "cached_result_control_unknown")
        cached.setdefault("process_priority", process_priority)
        cached.setdefault("process_priority_controlled", None)
        cached.setdefault("process_priority_reason", "cached_result_control_unknown")
        cached.setdefault("minimum_sample_duration_ms", minimum_sample_duration_ms)
        cached.setdefault("minimum_authoritative_repetitions", minimum_authoritative_repetitions)
        return cached
    task_started(task)
    result = _run_driver(
        engine,
        fixture,
        mode=mode,
        depth=depth,
        soft_ms=soft_ms,
        hard_ms=hard_ms,
        tt_size_mb=tt_size_mb,
        diagnostic=diagnostic,
        cpu_affinity_core=cpu_affinity_core,
        process_priority=process_priority,
    )
    record = _decorate(
        result,
        stage=stage,
        engine=engine,
        opponent=opponent_id,
        fixture_id=fixture["fixture_id"],
        depth=depth,
        budget_ms=budget_ms,
        repetition=repetition,
        diagnostic=diagnostic,
        warmup=warmup,
        pair_order=pair_order,
        order_in_repetition=order_in_repetition,
        execution_order=execution_order,
        requested_soft_limit_ms=soft_ms,
        requested_hard_limit_ms=hard_ms,
        minimum_sample_duration_ms=minimum_sample_duration_ms,
        minimum_authoritative_repetitions=minimum_authoritative_repetitions,
    )
    mark_complete(task, record)
    return record


def run_fixed_depth(
    candidate: EngineSpec,
    opponents: list[EngineSpec],
    fixtures: list[dict[str, Any]],
    profile: dict[str, Any],
    output_directory: Path,
    is_complete: callable,
    cached_result: callable,
    task_started: callable,
    mark_complete: callable,
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    warmup_records: list[dict[str, Any]] = []
    warmups = _profile_count(
        profile, "fixed_warmup_repetitions", "fixed_warmups", "warmup_repetitions"
    )
    repetitions = _profile_count(profile, "fixed_repetitions", default=1)
    diagnostic_repetitions = _profile_count(
        profile, "fixed_diagnostic_repetitions", "diagnostic_repetitions"
    )
    minimum_duration_ms, minimum_repetitions = _minimum_timing_policy(profile)
    execution_orders = count()
    cpu_affinity_core = _profile_affinity_core(profile)
    process_priority = profile.get("process_priority")
    for opponent in opponents:
        for fixture in fixtures:
            for depth in profile["fixed_depths"]:
                for repetition in range(warmups):
                    engines, pair_order = _engine_order(candidate, opponent, repetition)
                    for order_index, engine in enumerate(engines):
                        task = (
                            f"fixed-warmup:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{depth}:{repetition}:{engine.version_id}"
                        )
                        warmup_records.append(
                            _execute_task(
                                task=task,
                                legacy_tasks=(),
                                engine=engine,
                                opponent_id=opponent.version_id,
                                fixture=fixture,
                                stage="fixed_warmup",
                                mode="fixed",
                                depth=depth,
                                budget_ms=None,
                                soft_ms=profile["fixed_hard_timeout_ms"],
                                hard_ms=profile["fixed_hard_timeout_ms"],
                                tt_size_mb=profile["tt_size_mb"],
                                repetition=repetition,
                                diagnostic=False,
                                warmup=True,
                                pair_order=pair_order,
                                order_in_repetition=order_index,
                                execution_order=next(execution_orders),
                                cpu_affinity_core=cpu_affinity_core,
                                process_priority=process_priority,
                                minimum_sample_duration_ms=minimum_duration_ms,
                                minimum_authoritative_repetitions=minimum_repetitions,
                                is_complete=is_complete,
                                cached_result=cached_result,
                                task_started=task_started,
                                mark_complete=mark_complete,
                            )
                        )
                for repetition in range(repetitions):
                    engines, pair_order = _engine_order(candidate, opponent, repetition)
                    for order_index, engine in enumerate(engines):
                        task = (
                            f"fixed:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{depth}:{repetition}:{engine.version_id}"
                        )
                        records.append(
                            _execute_task(
                                task=task,
                                legacy_tasks=(),
                                engine=engine,
                                opponent_id=opponent.version_id,
                                fixture=fixture,
                                stage="fixed",
                                mode="fixed",
                                depth=depth,
                                budget_ms=None,
                                soft_ms=profile["fixed_hard_timeout_ms"],
                                hard_ms=profile["fixed_hard_timeout_ms"],
                                tt_size_mb=profile["tt_size_mb"],
                                repetition=repetition,
                                diagnostic=False,
                                warmup=False,
                                pair_order=pair_order,
                                order_in_repetition=order_index,
                                execution_order=next(execution_orders),
                                cpu_affinity_core=cpu_affinity_core,
                                process_priority=process_priority,
                                minimum_sample_duration_ms=minimum_duration_ms,
                                minimum_authoritative_repetitions=minimum_repetitions,
                                is_complete=is_complete,
                                cached_result=cached_result,
                                task_started=task_started,
                                mark_complete=mark_complete,
                            )
                        )
                for repetition in range(diagnostic_repetitions):
                    engines, pair_order = _engine_order(candidate, opponent, repetition)
                    for order_index, engine in enumerate(engines):
                        task = (
                            f"fixed-diagnostic:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{depth}:{repetition}:{engine.version_id}"
                        )
                        legacy = (
                            f"fixed-diagnostic:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{depth}:{engine.version_id}",
                        ) if repetition == 0 else ()
                        records.append(
                            _execute_task(
                                task=task,
                                legacy_tasks=legacy,
                                engine=engine,
                                opponent_id=opponent.version_id,
                                fixture=fixture,
                                stage="fixed",
                                mode="fixed",
                                depth=depth,
                                budget_ms=None,
                                soft_ms=profile["fixed_hard_timeout_ms"],
                                hard_ms=profile["fixed_hard_timeout_ms"],
                                tt_size_mb=profile["tt_size_mb"],
                                repetition=repetition,
                                diagnostic=True,
                                warmup=False,
                                pair_order=pair_order,
                                order_in_repetition=order_index,
                                execution_order=next(execution_orders),
                                cpu_affinity_core=cpu_affinity_core,
                                process_priority=process_priority,
                                minimum_sample_duration_ms=minimum_duration_ms,
                                minimum_authoritative_repetitions=minimum_repetitions,
                                is_complete=is_complete,
                                cached_result=cached_result,
                                task_started=task_started,
                                mark_complete=mark_complete,
                            )
                        )
    if warmup_records:
        write_csv(output_directory / "fixed_depth_warmups.csv", warmup_records, RAW_FIELDS)
    write_csv(output_directory / "raw_runs.csv", records, RAW_FIELDS)
    return records


def run_time_controlled(
    candidate: EngineSpec,
    opponents: list[EngineSpec],
    fixtures: list[dict[str, Any]],
    profile: dict[str, Any],
    output_directory: Path,
    is_complete: callable,
    cached_result: callable,
    task_started: callable,
    mark_complete: callable,
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    warmup_records: list[dict[str, Any]] = []
    warmups = _profile_count(profile, "time_warmup_repetitions", "time_warmups")
    repetitions = _profile_count(profile, "time_repetitions", default=1)
    diagnostic_repetitions = _profile_count(
        profile,
        "time_diagnostic_repetitions",
        "fixed_time_diagnostic_repetitions",
        "diagnostic_repetitions",
    )
    minimum_duration_ms, minimum_repetitions = _minimum_timing_policy(profile)
    execution_orders = count()
    cpu_affinity_core = _profile_affinity_core(profile)
    process_priority = profile.get("process_priority")
    for opponent in opponents:
        for fixture in fixtures:
            for budget in profile["time_budgets_ms"]:
                hard_grace_ms = int(profile.get("time_hard_grace_ms", 100))
                hard_multiplier = float(profile.get("time_hard_multiplier", 1.10))
                hard_ms = max(
                    budget + hard_grace_ms,
                    int(math.ceil(budget * hard_multiplier)),
                )
                for repetition in range(warmups):
                    engines, pair_order = _engine_order(candidate, opponent, repetition)
                    for order_index, engine in enumerate(engines):
                        task = (
                            f"time-warmup:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{budget}:{repetition}:{engine.version_id}"
                        )
                        warmup_records.append(
                            _execute_task(
                                task=task,
                                legacy_tasks=(),
                                engine=engine,
                                opponent_id=opponent.version_id,
                                fixture=fixture,
                                stage="time_warmup",
                                mode="time",
                                depth=profile["max_depth"],
                                budget_ms=budget,
                                soft_ms=budget,
                                hard_ms=hard_ms,
                                tt_size_mb=profile["tt_size_mb"],
                                repetition=repetition,
                                diagnostic=False,
                                warmup=True,
                                pair_order=pair_order,
                                order_in_repetition=order_index,
                                execution_order=next(execution_orders),
                                cpu_affinity_core=cpu_affinity_core,
                                process_priority=process_priority,
                                minimum_sample_duration_ms=minimum_duration_ms,
                                minimum_authoritative_repetitions=minimum_repetitions,
                                is_complete=is_complete,
                                cached_result=cached_result,
                                task_started=task_started,
                                mark_complete=mark_complete,
                            )
                        )
                for repetition in range(repetitions):
                    engines, pair_order = _engine_order(candidate, opponent, repetition)
                    for order_index, engine in enumerate(engines):
                        task = (
                            f"time:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{budget}:{repetition}:{engine.version_id}"
                        )
                        legacy = (
                            f"time:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{budget}:{engine.version_id}",
                        ) if repetition == 0 else ()
                        records.append(
                            _execute_task(
                                task=task,
                                legacy_tasks=legacy,
                                engine=engine,
                                opponent_id=opponent.version_id,
                                fixture=fixture,
                                stage="time",
                                mode="time",
                                depth=profile["max_depth"],
                                budget_ms=budget,
                                soft_ms=budget,
                                hard_ms=hard_ms,
                                tt_size_mb=profile["tt_size_mb"],
                                repetition=repetition,
                                diagnostic=False,
                                warmup=False,
                                pair_order=pair_order,
                                order_in_repetition=order_index,
                                execution_order=next(execution_orders),
                                cpu_affinity_core=cpu_affinity_core,
                                process_priority=process_priority,
                                minimum_sample_duration_ms=minimum_duration_ms,
                                minimum_authoritative_repetitions=minimum_repetitions,
                                is_complete=is_complete,
                                cached_result=cached_result,
                                task_started=task_started,
                                mark_complete=mark_complete,
                            )
                        )
                for repetition in range(diagnostic_repetitions):
                    engines, pair_order = _engine_order(candidate, opponent, repetition)
                    for order_index, engine in enumerate(engines):
                        task = (
                            f"time-diagnostic:{opponent.version_id}:{fixture['fixture_id']}:"
                            f"{budget}:{repetition}:{engine.version_id}"
                        )
                        records.append(
                            _execute_task(
                                task=task,
                                legacy_tasks=(),
                                engine=engine,
                                opponent_id=opponent.version_id,
                                fixture=fixture,
                                stage="time",
                                mode="time",
                                depth=profile["max_depth"],
                                budget_ms=budget,
                                soft_ms=budget,
                                hard_ms=hard_ms,
                                tt_size_mb=profile["tt_size_mb"],
                                repetition=repetition,
                                diagnostic=True,
                                warmup=False,
                                pair_order=pair_order,
                                order_in_repetition=order_index,
                                execution_order=next(execution_orders),
                                cpu_affinity_core=cpu_affinity_core,
                                process_priority=process_priority,
                                minimum_sample_duration_ms=minimum_duration_ms,
                                minimum_authoritative_repetitions=minimum_repetitions,
                                is_complete=is_complete,
                                cached_result=cached_result,
                                task_started=task_started,
                                mark_complete=mark_complete,
                            )
                        )
    if warmup_records:
        write_csv(output_directory / "time_controlled_warmups.csv", warmup_records, RAW_FIELDS)
    write_csv(output_directory / "time_controlled.csv", records, RAW_FIELDS)
    return records


def _geometric_mean(values: Iterable[float]) -> float | None:
    positive = [value for value in values if value > 0 and math.isfinite(value)]
    if not positive:
        return None
    return math.exp(sum(math.log(value) for value in positive) / len(positive))


def _numeric_value(value: Any) -> float | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _numbers(records: Iterable[dict[str, Any]], field: str) -> list[float]:
    values = (_numeric_value(record.get(field)) for record in records)
    return sorted(value for value in values if value is not None)


def _coefficient_of_variation(values: Sequence[float]) -> float | None:
    if len(values) < 2:
        return None
    mean = statistics.fmean(values)
    return statistics.pstdev(values) / mean if mean else None


def _sample_variance(values: Sequence[float]) -> float | None:
    return statistics.variance(values) if len(values) >= 2 else None


def _stability(values: Sequence[Any]) -> bool | None:
    if len(values) < 2:
        return None
    return len(set(values)) == 1


def _representative(values: Sequence[Any]) -> Any:
    if not values:
        return None
    counts = Counter(values)
    return min(counts, key=lambda value: (-counts[value], repr(value)))


def _ratio(numerator: Any, denominator: Any) -> float | None:
    top = _numeric_value(numerator)
    bottom = _numeric_value(denominator)
    if top is None or bottom is None or bottom <= 0.0:
        return None
    return top / bottom


def _deadline_overshoot(record: dict[str, Any]) -> float | None:
    reported = _numeric_value(record.get("deadline_overshoot_ms"))
    if reported is not None:
        return max(0.0, reported)
    elapsed = _numeric_value(record.get("elapsed_wall_ms"))
    hard_limit = _numeric_value(record.get("requested_hard_limit_ms"))
    if elapsed is None or hard_limit is None:
        return None
    return max(0.0, elapsed - hard_limit)


def _policy_value(
    records: Sequence[dict[str, Any]],
    explicit: float | int | None,
    field: str,
    fallback: float | int,
) -> float:
    if explicit is not None:
        return float(explicit)
    configured = [_numeric_value(record.get(field)) for record in records]
    return max((value for value in configured if value is not None), default=float(fallback))


def _group_summary(
    key: tuple[Any, ...],
    key_fields: Sequence[str],
    group: Sequence[dict[str, Any]],
    minimum_sample_duration_ms: float,
    minimum_authoritative_repetitions: int,
) -> dict[str, Any]:
    valid = [row for row in group if not row.get("error_type")]
    elapsed = _numbers(valid, "elapsed_wall_ms")
    external_elapsed = _numbers(valid, "elapsed_external_ms")
    process_envelope = _numbers(valid, "process_envelope_ms")
    process_cpu = _numbers(valid, "elapsed_process_cpu_ms")
    nodes = _numbers(valid, "nodes")
    nps = _numbers(valid, "nps")
    node_costs = sorted(
        elapsed_ms * 1_000_000.0 / node_count
        for row in valid
        if (elapsed_ms := _numeric_value(row.get("elapsed_wall_ms"))) is not None
        and (node_count := _numeric_value(row.get("nodes"))) is not None
        and node_count > 0.0
    )
    actions = [row["best_action"] for row in valid if row.get("best_action") is not None]
    scores = [row["score"] for row in valid if row.get("score") is not None]
    pvs = [tuple(row.get("pv_actions") or ()) for row in valid if row.get("pv_actions") is not None]
    depths = [row["completed_depth"] for row in valid if row.get("completed_depth") is not None]
    overshoots = sorted(
        value for row in valid if (value := _deadline_overshoot(row)) is not None
    )
    median_elapsed = statistics.median(elapsed) if elapsed else None
    timing_authoritative = (
        len(elapsed) >= minimum_authoritative_repetitions
        and median_elapsed is not None
        and median_elapsed >= minimum_sample_duration_ms
    )
    summary = {field: value for field, value in zip(key_fields, key)}
    summary.update(
        {
            "repetitions": len(group),
            "valid_repetitions": len(valid),
            "timing_repetitions": len(elapsed),
            "minimum_sample_duration_ms": minimum_sample_duration_ms,
            "minimum_authoritative_repetitions": minimum_authoritative_repetitions,
            "timing_authoritative": timing_authoritative,
            "median_elapsed_ms": median_elapsed,
            "median_external_elapsed_ms": (
                statistics.median(external_elapsed) if external_elapsed else None
            ),
            "median_process_envelope_ms": (
                statistics.median(process_envelope) if process_envelope else None
            ),
            "external_timing_valid_rate": (
                sum(row.get("external_timing_valid") is True for row in valid) / len(valid)
                if valid
                else None
            ),
            "p10_elapsed_ms": percentile(elapsed, 0.10),
            "p90_elapsed_ms": percentile(elapsed, 0.90),
            "coefficient_of_variation": _coefficient_of_variation(elapsed),
            "elapsed_variance_ms2": _sample_variance(elapsed),
            "minimum_elapsed_ms": min(elapsed) if elapsed else None,
            "maximum_elapsed_ms": max(elapsed) if elapsed else None,
            "median_process_cpu_ms": statistics.median(process_cpu) if process_cpu else None,
            "median_nodes": statistics.median(nodes) if nodes else None,
            "p10_nodes": percentile(nodes, 0.10),
            "p90_nodes": percentile(nodes, 0.90),
            "node_coefficient_of_variation": _coefficient_of_variation(nodes),
            "node_variance": _sample_variance(nodes),
            "node_count_stable": _stability(nodes),
            "unique_node_counts": len(set(nodes)),
            "median_nps": statistics.median(nps) if nps else None,
            "median_node_cost_ns": statistics.median(node_costs) if node_costs else None,
            "best_action_stable": _stability(actions),
            "unique_best_actions": len(set(actions)),
            "representative_best_action": _representative(actions),
            "score_stable": _stability(scores),
            "unique_scores": len(set(scores)),
            "representative_score": _representative(scores),
            "pv_stable": _stability(pvs),
            "unique_pvs": len(set(pvs)),
            "representative_pv": list(_representative(pvs) or ()),
            "completed_depth_stable": _stability(depths),
            "median_completed_depth": statistics.median(depths) if depths else None,
            "completed_depth_rate": sum(bool(row.get("completed")) for row in group) / len(group),
            "search_completed_rate": (
                sum(bool(row.get("search_completed")) for row in group) / len(group)
            ),
            "timeout_rate": sum(bool(row.get("timeout")) for row in group) / len(group),
            "fallback_rate": sum(bool(row.get("fallback_used")) for row in group) / len(group),
            "median_deadline_overshoot_ms": (
                statistics.median(overshoots) if overshoots else None
            ),
            "maximum_deadline_overshoot_ms": max(overshoots) if overshoots else None,
            "peak_memory_bytes": max(
                (
                    int(value)
                    for row in valid
                    if (value := _numeric_value(row.get("peak_memory_bytes"))) is not None
                ),
                default=None,
            ),
            "error_count": sum(bool(row.get("error_type")) for row in group),
        }
    )
    return summary


def _diagnostic_summary(
    records: Sequence[dict[str, Any]],
    key_fields: Sequence[str],
    timing_groups: Sequence[dict[str, Any]],
    minimum_sample_duration_ms: float,
    minimum_authoritative_repetitions: int,
) -> dict[str, Any]:
    diagnostic_records = [row for row in records if row.get("diagnostic")]
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in diagnostic_records:
        grouped[tuple(row.get(field) for field in key_fields)].append(row)
    groups: list[dict[str, Any]] = []
    availability = {metric: 0 for metric in DIAGNOSTIC_METRICS}
    for key, rows in sorted(grouped.items()):
        summary = _group_summary(
            key, key_fields, rows, minimum_sample_duration_ms, minimum_authoritative_repetitions
        )
        telemetry: dict[str, dict[str, Any]] = {}
        for metric in DIAGNOSTIC_METRICS:
            values = _numbers(rows, metric)
            availability[metric] += len(values)
            telemetry[metric] = {
                "available_repetitions": len(values),
                "median": statistics.median(values) if values else None,
            }
        summary["telemetry"] = telemetry
        groups.append(summary)
    timing_by_key = {tuple(row.get(field) for field in key_fields): row for row in timing_groups}
    overhead: list[dict[str, Any]] = []
    for diagnostic in groups:
        key = tuple(diagnostic.get(field) for field in key_fields)
        timing = timing_by_key.get(key)
        if timing is None:
            continue
        elapsed_ratio = _ratio(diagnostic["median_elapsed_ms"], timing["median_elapsed_ms"])
        overhead.append(
            {
                **{field: diagnostic.get(field) for field in key_fields},
                "elapsed_ratio": elapsed_ratio,
                "node_ratio": _ratio(diagnostic["median_nodes"], timing["median_nodes"]),
                "timing_authoritative": bool(
                    diagnostic["timing_authoritative"] and timing["timing_authoritative"]
                ),
            }
        )
    authoritative_overhead = [
        row["elapsed_ratio"]
        for row in overhead
        if row["timing_authoritative"] and row["elapsed_ratio"] is not None
    ]
    all_overhead = [row["elapsed_ratio"] for row in overhead if row["elapsed_ratio"] is not None]
    return {
        "runs": len(diagnostic_records),
        "groups": groups,
        "telemetry_availability": {
            metric: {
                "available_runs": count,
                "total_runs": len(diagnostic_records),
                "availability_rate": count / len(diagnostic_records) if diagnostic_records else None,
            }
            for metric, count in availability.items()
        },
        "instrumentation_overhead": {
            "comparisons": overhead,
            "geometric_mean_elapsed_ratio": _geometric_mean(authoritative_overhead),
            "geometric_mean_elapsed_ratio_all_samples": _geometric_mean(all_overhead),
            "authoritative_comparison_count": len(authoritative_overhead),
        },
    }


def _fixed_comparison(
    candidate: dict[str, Any], reference: dict[str, Any]
) -> dict[str, Any]:
    return {
        "fixture_id": candidate["fixture_id"],
        "depth": candidate["depth"],
        "candidate_median_elapsed_ms": candidate["median_elapsed_ms"],
        "reference_median_elapsed_ms": reference["median_elapsed_ms"],
        "elapsed_ratio": _ratio(candidate["median_elapsed_ms"], reference["median_elapsed_ms"]),
        "external_elapsed_ratio": _ratio(
            candidate["median_external_elapsed_ms"], reference["median_external_elapsed_ms"]
        ),
        "candidate_external_timing_valid_rate": candidate["external_timing_valid_rate"],
        "reference_external_timing_valid_rate": reference["external_timing_valid_rate"],
        "candidate_median_nodes": candidate["median_nodes"],
        "reference_median_nodes": reference["median_nodes"],
        "node_ratio": _ratio(candidate["median_nodes"], reference["median_nodes"]),
        "candidate_median_node_cost_ns": candidate["median_node_cost_ns"],
        "reference_median_node_cost_ns": reference["median_node_cost_ns"],
        "node_cost_ratio": _ratio(
            candidate["median_node_cost_ns"], reference["median_node_cost_ns"]
        ),
        "nps_ratio": _ratio(candidate["median_nps"], reference["median_nps"]),
        "peak_memory_ratio": _ratio(candidate["peak_memory_bytes"], reference["peak_memory_bytes"]),
        "timing_authoritative": bool(
            candidate["timing_authoritative"] and reference["timing_authoritative"]
        ),
        "candidate_coefficient_of_variation": candidate["coefficient_of_variation"],
        "reference_coefficient_of_variation": reference["coefficient_of_variation"],
        "candidate_node_count_stable": candidate["node_count_stable"],
        "reference_node_count_stable": reference["node_count_stable"],
        "candidate_best_action_stable": candidate["best_action_stable"],
        "reference_best_action_stable": reference["best_action_stable"],
        "candidate_representative_score": candidate["representative_score"],
        "reference_representative_score": reference["representative_score"],
        "score_agreement": (
            candidate["representative_score"] == reference["representative_score"]
            if candidate["representative_score"] is not None
            and reference["representative_score"] is not None
            else None
        ),
        "candidate_score_stable": candidate["score_stable"],
        "reference_score_stable": reference["score_stable"],
        "candidate_score_stable": candidate["score_stable"],
        "reference_score_stable": reference["score_stable"],
        "candidate_pv_stable": candidate["pv_stable"],
        "reference_pv_stable": reference["pv_stable"],
    }


def _regression_lists(
    comparisons: Sequence[dict[str, Any]],
    elapsed_threshold: float,
    node_threshold: float,
    node_cost_threshold: float,
) -> dict[str, list[dict[str, Any]]]:
    result: dict[str, list[dict[str, Any]]] = {
        "critical_regressions": [],
        "node_regressions": [],
        "node_cost_regressions": [],
        "outlier_regressions": [],
        "node_stability_failures": [],
    }
    for comparison in comparisons:
        identity = {key: comparison[key] for key in ("fixture_id", "depth")}
        elapsed_ratio = comparison["elapsed_ratio"]
        if elapsed_ratio is not None and elapsed_ratio > 1.0 + elapsed_threshold:
            outlier = {
                **identity,
                "metric": "elapsed",
                "ratio": elapsed_ratio,
                "authoritative": comparison["timing_authoritative"],
            }
            result["outlier_regressions"].append(outlier)
            if comparison["timing_authoritative"]:
                result["critical_regressions"].append({**identity, "ratio": elapsed_ratio})
        node_ratio = comparison["node_ratio"]
        if node_ratio is not None and node_ratio > 1.0 + node_threshold:
            regression = {**identity, "ratio": node_ratio}
            result["node_regressions"].append(regression)
            result["outlier_regressions"].append(
                {**regression, "metric": "nodes", "authoritative": True}
            )
        node_cost_ratio = comparison["node_cost_ratio"]
        if (
            comparison["timing_authoritative"]
            and node_cost_ratio is not None
            and node_cost_ratio > 1.0 + node_cost_threshold
        ):
            regression = {**identity, "ratio": node_cost_ratio}
            result["node_cost_regressions"].append(regression)
            result["outlier_regressions"].append(
                {**regression, "metric": "node_cost", "authoritative": True}
            )
        if (
            comparison["candidate_node_count_stable"] is False
            or comparison["reference_node_count_stable"] is False
        ):
            result["node_stability_failures"].append(
                {
                    **identity,
                    "candidate_stable": comparison["candidate_node_count_stable"],
                    "reference_stable": comparison["reference_node_count_stable"],
                }
            )
    return result


def _summarize_fixed_opponent(
    summaries: Sequence[dict[str, Any]],
    candidate_id: str,
    opponent: str,
    elapsed_threshold: float,
    node_threshold: float,
    node_cost_threshold: float,
) -> dict[str, Any]:
    candidate_rows = {
        (row["fixture_id"], row["depth"]): row
        for row in summaries
        if row["opponent"] == opponent and row["version"] == candidate_id
    }
    opponent_rows = {
        (row["fixture_id"], row["depth"]): row
        for row in summaries
        if row["opponent"] == opponent and row["version"] == opponent
    }
    comparisons = [
        _fixed_comparison(candidate_rows[key], opponent_rows[key])
        for key in sorted(candidate_rows.keys() & opponent_rows.keys())
    ]
    elapsed_ratios = [
        row["elapsed_ratio"]
        for row in comparisons
        if row["timing_authoritative"] and row["elapsed_ratio"] is not None
    ]
    all_elapsed_ratios = [
        row["elapsed_ratio"] for row in comparisons if row["elapsed_ratio"] is not None
    ]
    node_ratios = [row["node_ratio"] for row in comparisons if row["node_ratio"] is not None]
    stable_node_ratios = [
        row["node_ratio"]
        for row in comparisons
        if row["candidate_node_count_stable"] is True
        and row["reference_node_count_stable"] is True
        and row["node_ratio"] is not None
    ]
    node_cost_ratios = [
        row["node_cost_ratio"]
        for row in comparisons
        if row["timing_authoritative"] and row["node_cost_ratio"] is not None
    ]
    nps_ratios = [
        row["nps_ratio"]
        for row in comparisons
        if row["timing_authoritative"] and row["nps_ratio"] is not None
    ]
    memory_ratios = [
        row["peak_memory_ratio"] for row in comparisons if row["peak_memory_ratio"] is not None
    ]
    regressions = _regression_lists(
        comparisons, elapsed_threshold, node_threshold, node_cost_threshold
    )
    timing_noise_values = [
        value
        for row in comparisons
        for field in (
            "candidate_coefficient_of_variation",
            "reference_coefficient_of_variation",
        )
        if (value := _numeric_value(row[field])) is not None
    ]
    authoritative_count = len(elapsed_ratios)
    return {
        "geometric_mean_elapsed_ratio": _geometric_mean(elapsed_ratios),
        "geometric_mean_elapsed_ratio_all_samples": _geometric_mean(all_elapsed_ratios),
        "geometric_mean_node_ratio": _geometric_mean(node_ratios),
        "geometric_mean_stable_node_ratio": _geometric_mean(stable_node_ratios),
        "geometric_mean_node_cost_ratio": _geometric_mean(node_cost_ratios),
        "geometric_mean_nps_ratio": _geometric_mean(nps_ratios),
        "geometric_mean_peak_memory_ratio": _geometric_mean(memory_ratios),
        "maximum_peak_memory_ratio": max(memory_ratios, default=None),
        **regressions,
        "unstable_node_groups": regressions["node_stability_failures"],
        "timing_quality_passed": bool(comparisons)
        and authoritative_count == len(comparisons),
        "timing_noise_floor": max(timing_noise_values, default=None),
        "comparisons": comparisons,
        "comparison_count": len(comparisons),
        "authoritative_timing_comparison_count": authoritative_count,
    }


def summarize_fixed(
    records: list[dict[str, Any]],
    candidate_id: str,
    opponents: list[str],
    maximum_critical_regression: float = 0.05,
    minimum_sample_duration_ms: float | None = None,
    minimum_authoritative_repetitions: int | None = None,
    maximum_node_regression: float | None = None,
    maximum_node_cost_regression: float | None = None,
) -> dict[str, Any]:
    authoritative = [
        record for record in records if not record.get("diagnostic") and not record.get("warmup")
    ]
    minimum_duration = max(
        0.0,
        _policy_value(
            authoritative,
            minimum_sample_duration_ms,
            "minimum_sample_duration_ms",
            0.0,
        ),
    )
    minimum_repetitions = max(
        1,
        int(
            _policy_value(
                authoritative,
                minimum_authoritative_repetitions,
                "minimum_authoritative_repetitions",
                2,
            )
        ),
    )
    node_threshold = (
        maximum_critical_regression
        if maximum_node_regression is None
        else maximum_node_regression
    )
    node_cost_threshold = (
        maximum_critical_regression
        if maximum_node_cost_regression is None
        else maximum_node_cost_regression
    )
    grouped: dict[tuple[str, str, str, int], list[dict[str, Any]]] = defaultdict(list)
    for record in authoritative:
        grouped[
            (record["opponent"], record["version"], record["fixture_id"], int(record["depth"]))
        ].append(record)
    key_fields = ("opponent", "version", "fixture_id", "depth")
    summaries = [
        _group_summary(key, key_fields, group, minimum_duration, minimum_repetitions)
        for key, group in sorted(grouped.items())
    ]
    return {
        "timing_policy": {
            "minimum_sample_duration_ms": minimum_duration,
            "minimum_authoritative_repetitions": minimum_repetitions,
        },
        "groups": summaries,
        "opponents": {
            opponent: _summarize_fixed_opponent(
                summaries,
                candidate_id,
                opponent,
                maximum_critical_regression,
                node_threshold,
                node_cost_threshold,
            )
            for opponent in opponents
        },
        "diagnostics": _diagnostic_summary(
            records, key_fields, summaries, minimum_duration, minimum_repetitions
        ),
    }


def _time_comparison(candidate: dict[str, Any], reference: dict[str, Any]) -> dict[str, Any]:
    candidate_depth = _numeric_value(candidate["median_completed_depth"])
    reference_depth = _numeric_value(reference["median_completed_depth"])
    depth_delta = (
        candidate_depth - reference_depth
        if candidate_depth is not None and reference_depth is not None
        else None
    )
    candidate_overshoot = _numeric_value(candidate["median_deadline_overshoot_ms"])
    reference_overshoot = _numeric_value(reference["median_deadline_overshoot_ms"])
    overshoot_delta = (
        candidate_overshoot - reference_overshoot
        if candidate_overshoot is not None and reference_overshoot is not None
        else None
    )
    candidate_action = candidate["representative_best_action"]
    reference_action = reference["representative_best_action"]
    candidate_score = candidate["representative_score"]
    reference_score = reference["representative_score"]
    candidate_pv = candidate["representative_pv"]
    reference_pv = reference["representative_pv"]
    return {
        "fixture_id": candidate["fixture_id"],
        "budget_ms": candidate["budget_ms"],
        "candidate_median_completed_depth": candidate_depth,
        "reference_median_completed_depth": reference_depth,
        "completed_depth_delta": depth_delta,
        "candidate_representative_best_action": candidate_action,
        "reference_representative_best_action": reference_action,
        "best_action_agreement": (
            candidate_action == reference_action
            if candidate_action is not None and reference_action is not None
            else None
        ),
        "candidate_best_action_stable": candidate["best_action_stable"],
        "reference_best_action_stable": reference["best_action_stable"],
        "candidate_representative_score": candidate_score,
        "reference_representative_score": reference_score,
        "score_agreement": (
            candidate_score == reference_score
            if candidate_score is not None and reference_score is not None
            else None
        ),
        "candidate_score_stable": candidate["score_stable"],
        "reference_score_stable": reference["score_stable"],
        "pv_agreement": (
            candidate_pv == reference_pv if candidate_pv and reference_pv else None
        ),
        "candidate_pv_stable": candidate["pv_stable"],
        "reference_pv_stable": reference["pv_stable"],
        "candidate_timeout_rate": candidate["timeout_rate"],
        "reference_timeout_rate": reference["timeout_rate"],
        "candidate_fallback_rate": candidate["fallback_rate"],
        "reference_fallback_rate": reference["fallback_rate"],
        "candidate_search_completed_rate": candidate["search_completed_rate"],
        "reference_search_completed_rate": reference["search_completed_rate"],
        "candidate_median_deadline_overshoot_ms": candidate_overshoot,
        "reference_median_deadline_overshoot_ms": reference_overshoot,
        "candidate_maximum_deadline_overshoot_ms": candidate["maximum_deadline_overshoot_ms"],
        "reference_maximum_deadline_overshoot_ms": reference["maximum_deadline_overshoot_ms"],
        "deadline_overshoot_delta_ms": overshoot_delta,
        "node_ratio": _ratio(candidate["median_nodes"], reference["median_nodes"]),
        "elapsed_ratio": _ratio(candidate["median_elapsed_ms"], reference["median_elapsed_ms"]),
    }


def _boolean_rate(values: Iterable[bool | None]) -> float | None:
    available = [value for value in values if value is not None]
    return sum(available) / len(available) if available else None


def _mean_comparison_rate(comparisons: Sequence[dict[str, Any]], field: str) -> float | None:
    values = [_numeric_value(row.get(field)) for row in comparisons]
    available = [value for value in values if value is not None]
    return statistics.fmean(available) if available else None


def _summarize_time_opponent(
    groups: Sequence[dict[str, Any]], candidate_id: str, opponent: str
) -> dict[str, Any]:
    candidate_groups = {
        (row["fixture_id"], row["budget_ms"]): row
        for row in groups
        if row["opponent"] == opponent and row["version"] == candidate_id
    }
    reference_groups = {
        (row["fixture_id"], row["budget_ms"]): row
        for row in groups
        if row["opponent"] == opponent and row["version"] == opponent
    }
    comparisons = [
        _time_comparison(candidate_groups[key], reference_groups[key])
        for key in sorted(candidate_groups.keys() & reference_groups.keys())
    ]
    depth_deltas = [
        row["completed_depth_delta"]
        for row in comparisons
        if row["completed_depth_delta"] is not None
    ]
    overshoot_deltas = [
        row["deadline_overshoot_delta_ms"]
        for row in comparisons
        if row["deadline_overshoot_delta_ms"] is not None
    ]
    node_ratios = [row["node_ratio"] for row in comparisons if row["node_ratio"] is not None]
    candidate_timeout_rate = _mean_comparison_rate(comparisons, "candidate_timeout_rate")
    candidate_fallback_rate = _mean_comparison_rate(comparisons, "candidate_fallback_rate")
    candidate_shallower_count = sum(delta < 0 for delta in depth_deltas)
    candidate_maximum_overshoot = max(
        (
            float(row["candidate_maximum_deadline_overshoot_ms"])
            for row in comparisons
            if row.get("candidate_maximum_deadline_overshoot_ms") is not None
        ),
        default=None,
    )
    return {
        "comparisons": comparisons,
        "comparison_count": len(comparisons),
        "mean_completed_depth_delta": statistics.fmean(depth_deltas) if depth_deltas else None,
        "median_completed_depth_delta": statistics.median(depth_deltas) if depth_deltas else None,
        "candidate_deeper_count": sum(delta > 0 for delta in depth_deltas),
        "equal_depth_count": sum(delta == 0 for delta in depth_deltas),
        "candidate_shallower_count": candidate_shallower_count,
        "depth_regression_count": candidate_shallower_count,
        "best_action_agreement_rate": _boolean_rate(
            row["best_action_agreement"] for row in comparisons
        ),
        "pv_agreement_rate": _boolean_rate(row["pv_agreement"] for row in comparisons),
        "score_agreement_rate": _boolean_rate(row["score_agreement"] for row in comparisons),
        "candidate_best_action_stability_rate": _boolean_rate(
            row["candidate_best_action_stable"] for row in comparisons
        ),
        "reference_best_action_stability_rate": _boolean_rate(
            row["reference_best_action_stable"] for row in comparisons
        ),
        "candidate_score_stability_rate": _boolean_rate(
            row["candidate_score_stable"] for row in comparisons
        ),
        "reference_score_stability_rate": _boolean_rate(
            row["reference_score_stable"] for row in comparisons
        ),
        "candidate_pv_stability_rate": _boolean_rate(
            row["candidate_pv_stable"] for row in comparisons
        ),
        "reference_pv_stability_rate": _boolean_rate(
            row["reference_pv_stable"] for row in comparisons
        ),
        "candidate_timeout_rate": candidate_timeout_rate,
        "timeout_rate": candidate_timeout_rate,
        "reference_timeout_rate": _mean_comparison_rate(comparisons, "reference_timeout_rate"),
        "candidate_fallback_rate": candidate_fallback_rate,
        "fallback_rate": candidate_fallback_rate,
        "reference_fallback_rate": _mean_comparison_rate(comparisons, "reference_fallback_rate"),
        "median_deadline_overshoot_delta_ms": (
            statistics.median(overshoot_deltas) if overshoot_deltas else None
        ),
        "maximum_deadline_overshoot_ms": candidate_maximum_overshoot,
        "geometric_mean_node_ratio": _geometric_mean(node_ratios),
    }


def summarize_time_controlled(
    records: list[dict[str, Any]],
    candidate_id: str,
    opponents: list[str],
    minimum_sample_duration_ms: float | None = None,
    minimum_authoritative_repetitions: int | None = None,
) -> dict[str, Any]:
    authoritative = [
        record for record in records if not record.get("diagnostic") and not record.get("warmup")
    ]
    minimum_duration = max(
        0.0,
        _policy_value(
            authoritative,
            minimum_sample_duration_ms,
            "minimum_sample_duration_ms",
            0.0,
        ),
    )
    minimum_repetitions = max(
        1,
        int(
            _policy_value(
                authoritative,
                minimum_authoritative_repetitions,
                "minimum_authoritative_repetitions",
                2,
            )
        ),
    )
    key_fields = ("opponent", "version", "fixture_id", "budget_ms")
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in authoritative:
        grouped[tuple(row.get(field) for field in key_fields)].append(row)
    groups = [
        _group_summary(key, key_fields, rows, minimum_duration, minimum_repetitions)
        for key, rows in sorted(grouped.items())
    ]
    return {
        "timing_policy": {
            "minimum_sample_duration_ms": minimum_duration,
            "minimum_authoritative_repetitions": minimum_repetitions,
        },
        "groups": groups,
        "opponents": {
            opponent: _summarize_time_opponent(groups, candidate_id, opponent)
            for opponent in opponents
        },
        "diagnostics": _diagnostic_summary(
            records, key_fields, groups, minimum_duration, minimum_repetitions
        ),
    }


def _csv_value(value: Any) -> Any:
    return json.dumps(value, sort_keys=True) if isinstance(value, (dict, list)) else value


def _summary_rows(summary: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        {"opponent": opponent, **{key: _csv_value(value) for key, value in values.items()}}
        for opponent, values in summary["opponents"].items()
    ]


def write_fixed_summaries(output_directory: Path, summary: dict[str, Any]) -> None:
    groups = summary["groups"]
    if groups:
        write_csv(output_directory / "fixed_depth.csv", groups, list(groups[0]))
    opponent_rows = _summary_rows(summary)
    if opponent_rows:
        write_csv(output_directory / "summary.csv", opponent_rows, list(opponent_rows[0]))


def write_time_summaries(output_directory: Path, summary: dict[str, Any]) -> None:
    groups = summary["groups"]
    if groups:
        write_csv(output_directory / "time_controlled_summary.csv", groups, list(groups[0]))
    opponent_rows = _summary_rows(summary)
    if opponent_rows:
        write_csv(
            output_directory / "time_controlled_comparison.csv",
            opponent_rows,
            list(opponent_rows[0]),
        )
