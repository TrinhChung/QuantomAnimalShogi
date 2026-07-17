from __future__ import annotations

import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Any

from .common import atomic_write_json, load_json, sha256_file
from .referee import EngineSpec, NativeReferee
from .run_performance import run_benchmark_once


def _classification(candidate: dict[str, Any], opponent: dict[str, Any], change_category: str) -> str:
    if candidate.get("error_type") or opponent.get("error_type"):
        return "unknown"
    if candidate.get("timeout") != opponent.get("timeout"):
        return "timeout_difference"
    if candidate.get("fallback_used") != opponent.get("fallback_used"):
        return "fallback_difference"
    if candidate.get("completed_depth") != opponent.get("completed_depth"):
        return "different_completed_depth"
    if candidate.get("score") != opponent.get("score"):
        if change_category == "evaluation_change":
            return "evaluation_difference"
        if change_category in {"selective_search", "pruning_change"}:
            return "selective_search_difference"
        return "different_fixed_depth_score"
    if candidate.get("best_action") != opponent.get("best_action"):
        return "equal_score_tiebreak"
    return "unknown"


def _reanalysis_classification(
    found: dict[str, Any], reanalysis: dict[str, Any], change_category: str
) -> str:
    candidate_fixed = reanalysis["candidate_fixed"]
    opponent_fixed = reanalysis["opponent_fixed"]
    fixed = _classification(candidate_fixed, opponent_fixed, change_category)
    if fixed != "unknown":
        return fixed
    fixed_agrees = (
        candidate_fixed.get("best_action") == opponent_fixed.get("best_action")
        and candidate_fixed.get("score") == opponent_fixed.get("score")
    )
    candidate_tt_off = reanalysis.get("candidate_tt_off", {})
    opponent_tt_off = reanalysis.get("opponent_tt_off", {})
    tt_off_agrees = (
        candidate_tt_off
        and opponent_tt_off
        and candidate_tt_off.get("best_action") == opponent_tt_off.get("best_action")
        and candidate_tt_off.get("score") == opponent_tt_off.get("score")
    )
    if fixed_agrees and tt_off_agrees and found["candidate_action"] != found["opponent_action"]:
        return "tt_history_effect"
    time_classification = _classification(
        reanalysis.get("candidate_time", {}),
        reanalysis.get("opponent_time", {}),
        change_category,
    )
    return time_classification


def _same_state_move_divergence(
    moves: list[dict[str, Any]], candidate_id: str, opponent_id: str
) -> dict[str, Any] | None:
    grouped: dict[tuple[str, int], dict[str, list[dict[str, Any]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for move in moves:
        if move.get("opponent_id") != opponent_id or move.get("response_status") != "valid_response":
            continue
        version = move.get("engine_version")
        if version not in {candidate_id, opponent_id}:
            continue
        grouped[(move.get("opening_id", "unknown"), int(move["state_hash_before"]))][version].append(move)
    candidates: list[dict[str, Any]] = []
    for (opening_id, state_hash), versions in grouped.items():
        candidate_rows = versions.get(candidate_id, [])
        opponent_rows = versions.get(opponent_id, [])
        if not candidate_rows or not opponent_rows:
            continue
        candidate_row = min(candidate_rows, key=lambda row: (int(row.get("turn_before", 0)), int(row["ply"])))
        opponent_row = min(opponent_rows, key=lambda row: (int(row.get("turn_before", 0)), int(row["ply"])))
        if candidate_row.get("submitted_action") == opponent_row.get("submitted_action"):
            continue
        candidates.append(
            {
                "source": "paired_match_same_state",
                "opening_id": opening_id,
                "fixture_id": opening_id,
                "state_hash": state_hash,
                "state_text_path": candidate_row.get("full_state_path") or opponent_row.get("full_state_path"),
                "turn": candidate_row.get("turn_before"),
                "side_to_move": candidate_row.get("side"),
                "candidate_action": candidate_row.get("submitted_action"),
                "opponent_action": opponent_row.get("submitted_action"),
                "candidate_score": candidate_row.get("score"),
                "opponent_score": opponent_row.get("score"),
                "candidate_row": candidate_row,
                "opponent_row": opponent_row,
                "sort_key": (int(candidate_row.get("turn_before", 0)), opening_id, state_hash),
            }
        )
    return min(candidates, key=lambda item: item["sort_key"]) if candidates else None


def _record_divergence(
    records: list[dict[str, Any]], candidate_id: str, opponent_id: str, source: str
) -> dict[str, Any] | None:
    grouped: dict[tuple[str, int | None], dict[str, list[dict[str, Any]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for record in records:
        if record.get("opponent") != opponent_id or record.get("diagnostic") or record.get("warmup"):
            continue
        key = (record["fixture_id"], record.get("budget_ms") if source == "fixed_time" else record.get("depth"))
        grouped[key][record["version"]].append(record)
    candidates: list[dict[str, Any]] = []
    for (fixture_id, condition), versions in grouped.items():
        candidate_rows = versions.get(candidate_id, [])
        opponent_rows = versions.get(opponent_id, [])
        if not candidate_rows or not opponent_rows:
            continue
        candidate_row = min(candidate_rows, key=lambda row: int(row.get("repetition", 0)))
        opponent_row = min(opponent_rows, key=lambda row: int(row.get("repetition", 0)))
        if candidate_row.get("best_action") == opponent_row.get("best_action"):
            continue
        candidates.append(
            {
                "source": source,
                "opening_id": fixture_id,
                "fixture_id": fixture_id,
                "condition": condition,
                "candidate_action": candidate_row.get("best_action"),
                "opponent_action": opponent_row.get("best_action"),
                "candidate_score": candidate_row.get("score"),
                "opponent_score": opponent_row.get("score"),
                "candidate_row": candidate_row,
                "opponent_row": opponent_row,
                "sort_key": (fixture_id, condition or 0),
            }
        )
    return min(candidates, key=lambda item: item["sort_key"]) if candidates else None


def _tt_off_engine(engine: EngineSpec, directory: Path) -> EngineSpec:
    value = load_json(engine.config)
    value.setdefault("tt", {})["tt_enabled"] = False
    value["tt"]["tt_auto_size_enabled"] = False
    path = directory / f"{engine.version_id}.tt_off.json"
    atomic_write_json(path, value)
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


def _rerun(
    engine: EngineSpec,
    fixture: dict[str, Any],
    profile: dict[str, Any],
    *,
    mode: str,
    depth: int,
    budget_ms: int,
    diagnostic: bool,
) -> dict[str, Any]:
    hard_ms = (
        int(profile.get("fixed_hard_timeout_ms", 600000))
        if mode == "fixed"
        else max(budget_ms + 100, int(budget_ms * 1.10))
    )
    return run_benchmark_once(
        engine,
        fixture,
        mode=mode,
        depth=depth,
        soft_ms=hard_ms if mode == "fixed" else budget_ms,
        hard_ms=hard_ms,
        tt_size_mb=int(profile.get("tt_size_mb", 64)),
        diagnostic=diagnostic,
        cpu_affinity_core=profile.get("cpu_affinity_core"),
        process_priority=profile.get("process_priority"),
    )


def analyze_divergences(
    *,
    fixed_records: list[dict[str, Any]],
    fixtures: list[dict[str, Any]],
    candidate: EngineSpec,
    opponents: list[EngineSpec],
    change_category: str,
    referee_executable: Path,
    output_directory: Path,
    time_records: list[dict[str, Any]] | None = None,
    move_records: list[dict[str, Any]] | None = None,
    profile: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    fixture_by_id = {fixture["fixture_id"]: fixture for fixture in fixtures}
    profile = profile or {}
    settings = profile.get("divergence_reanalysis", {})
    divergences: list[dict[str, Any]] = []
    output_directory.mkdir(parents=True, exist_ok=True)
    for opponent in opponents:
        found = _same_state_move_divergence(
            move_records or [], candidate.version_id, opponent.version_id
        )
        if found is None:
            found = _record_divergence(
                time_records or [], candidate.version_id, opponent.version_id, "fixed_time"
            )
        if found is None:
            found = _record_divergence(
                fixed_records, candidate.version_id, opponent.version_id, "fixed_depth"
            )
        if found is None:
            continue

        fixture = fixture_by_id.get(found["fixture_id"])
        if found.get("state_text_path"):
            state_text = Path(found["state_text_path"]).read_text(encoding="utf-8")
            with NativeReferee(referee_executable) as referee:
                snapshot = referee.load(state_text)
            fixture = {
                "fixture_id": found["fixture_id"],
                "full_serialized_state": snapshot["state_text"],
                "structural_hash": snapshot["state_hash"],
                "turn": snapshot["turn"],
                "side_to_move": snapshot["side"],
            }
        if fixture is None:
            continue
        directory = output_directory / f"{opponent.version_id}_{found['fixture_id']}"
        directory.mkdir(parents=True, exist_ok=True)
        atomic_write_json(directory / "state.json", fixture)
        state_path = directory / "state.txt"
        state_path.write_text(fixture["full_serialized_state"], encoding="utf-8")
        with NativeReferee(referee_executable) as referee:
            snapshot = referee.load(fixture["full_serialized_state"])
        atomic_write_json(
            directory / "legal_actions.json",
            {"legal_actions": snapshot["legal_actions"], "action_mask": snapshot["action_mask"]},
        )

        reanalysis: dict[str, Any] = {}
        if settings.get("enabled", False):
            base_depths = [int(row.get("depth") or 0) for row in fixed_records]
            depth = max(base_depths or [4]) + int(settings.get("depth_extension", 2))
            budget = int(settings.get("fixed_time_ms", 3000))
            for label, engine in (("candidate", candidate), ("opponent", opponent)):
                reanalysis[f"{label}_fixed"] = _rerun(
                    engine, fixture, profile, mode="fixed", depth=depth, budget_ms=budget,
                    diagnostic=bool(settings.get("diagnostic", True)),
                )
                reanalysis[f"{label}_time"] = _rerun(
                    engine, fixture, profile, mode="time", depth=int(profile.get("max_depth", 64)),
                    budget_ms=budget, diagnostic=bool(settings.get("diagnostic", True)),
                )
                if settings.get("tt_off", True):
                    reanalysis[f"{label}_tt_off"] = _rerun(
                        _tt_off_engine(engine, directory), fixture, profile, mode="fixed", depth=depth,
                        budget_ms=budget, diagnostic=bool(settings.get("diagnostic", True)),
                    )
            classification = _reanalysis_classification(found, reanalysis, change_category)
        else:
            classification = _classification(
                found["candidate_row"], found["opponent_row"], change_category
            )

        comparison = {
            "opponent": opponent.version_id,
            "fixture_id": found["fixture_id"],
            "source": found["source"],
            "turn": found.get("turn", fixture.get("turn")),
            "side_to_move": found.get("side_to_move", fixture.get("side_to_move")),
            "classification": classification,
            "candidate_action": found["candidate_action"],
            "opponent_action": found["opponent_action"],
            "candidate_score": found.get("candidate_score"),
            "opponent_score": found.get("opponent_score"),
            "candidate_original": found["candidate_row"],
            "opponent_original": found["opponent_row"],
            "reanalysis": reanalysis,
        }
        atomic_write_json(directory / "comparison.json", comparison)
        with (directory / "reanalysis.csv").open("w", encoding="utf-8", newline="") as output:
            rows = [{"run": key, **value} for key, value in reanalysis.items()]
            if rows:
                writer = csv.DictWriter(output, fieldnames=sorted({key for row in rows for key in row}))
                writer.writeheader()
                writer.writerows(rows)
        reproduce = directory / "reproduce.bat"
        depth = max([int(row.get("depth") or 0) for row in fixed_records] or [4]) + int(
            settings.get("depth_extension", 2)
        )
        reproduce.write_text(
            "@echo off\nsetlocal\n"
            f'"{candidate.benchmark_executable}" --mode fixed --config "{candidate.config}" '
            f'--depth {depth} --soft-ms 600000 --hard-ms 600000 --tt-size-mb {profile.get("tt_size_mb", 64)} --diagnostic < "{state_path}"\n'
            f'"{opponent.benchmark_executable}" --mode fixed --config "{opponent.config}" '
            f'--depth {depth} --soft-ms 600000 --hard-ms 600000 --tt-size-mb {profile.get("tt_size_mb", 64)} --diagnostic < "{state_path}"\n',
            encoding="utf-8",
        )
        divergences.append(comparison)
    atomic_write_json(output_directory / "summary.json", divergences)
    return divergences
