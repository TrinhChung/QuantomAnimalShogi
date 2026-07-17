from __future__ import annotations

import json
import subprocess
from collections import defaultdict
from pathlib import Path
from typing import Any

from .common import EvaluationError, atomic_write_json, repository_root, write_csv
from .referee import EngineSession, EngineSpec, NativeReferee


def run_ctest(build_directory: Path, output_directory: Path) -> dict[str, Any]:
    command = ["ctest", "--test-dir", str(build_directory), "-C", "Release", "--output-on-failure"]
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
    result = {
        "command": subprocess.list2cmdline(command),
        "scope": "current_source_tree_build_and_evaluation_infrastructure",
        "candidate_binary_coverage": "protocol_and_fixed_search_only",
        "exit_code": completed.returncode,
        "passed": completed.returncode == 0,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }
    atomic_write_json(output_directory / "tests.json", result)
    (output_directory / "ctest.stdout.txt").write_text(completed.stdout, encoding="utf-8")
    (output_directory / "ctest.stderr.txt").write_text(completed.stderr, encoding="utf-8")
    return result


def protocol_probe(
    engine: EngineSpec, referee_executable: Path, hard_timeout_ms: int, output_directory: Path
) -> dict[str, Any]:
    with NativeReferee(referee_executable) as referee:
        snapshot = referee.initial()
    session = EngineSession(
        engine,
        16,
        output_directory / f"{engine.version_id}.probe.stdout.txt",
        output_directory / f"{engine.version_id}.probe.stderr.txt",
    )
    try:
        response = session.request_action(snapshot["protocol_request"], hard_timeout_ms)
    finally:
        session.close()
    response["allowed"] = response.get("submitted_action") in snapshot["legal_actions"]
    response["passed"] = response["response_status"] == "valid_response" and response["allowed"]
    return response


def compare_fixed_depth(
    records: list[dict[str, Any]],
    candidate_id: str,
    opponents: list[str],
    change_category: str,
    output_path: Path,
) -> dict[str, Any]:
    authoritative = [record for record in records if not record.get("diagnostic")]
    grouped: dict[tuple[str, str, int, int], dict[str, dict[str, Any]]] = defaultdict(dict)
    for record in authoritative:
        key = (
            record["opponent"],
            record["fixture_id"],
            int(record["depth"]),
            int(record["repetition"]),
        )
        grouped[key][record["version"]] = record
    rows: list[dict[str, Any]] = []
    strict_scores = change_category in {
        "optimization_only",
        "performance_only",
        "move_ordering",
        "search_control",
        "selective_search",
        "pruning_change",
        "canonicalization_change",
    }
    mismatch_count = 0
    for (opponent, fixture_id, depth, repetition), versions in sorted(grouped.items()):
        candidate = versions.get(candidate_id)
        reference = versions.get(opponent)
        if not candidate or not reference:
            classification = "unknown"
        elif candidate.get("error_type") or reference.get("error_type"):
            classification = "protocol_failure"
        elif not candidate.get("legal_result") or not reference.get("legal_result"):
            classification = "illegal_action"
        elif sorted(candidate.get("legal_actions") or []) != sorted(reference.get("legal_actions") or []):
            classification = "different_legal_actions"
        elif not candidate.get("completed") or not reference.get("completed"):
            classification = "incomplete_search"
        elif candidate.get("terminal_result") != reference.get("terminal_result"):
            classification = "different_terminal_result"
        elif candidate.get("score") != reference.get("score"):
            classification = "different_score"
        elif candidate.get("best_action") != reference.get("best_action"):
            classification = "equal_score_tiebreak"
        elif candidate.get("state_hash_after") != reference.get("state_hash_after"):
            classification = "different_transition"
        else:
            classification = "identical"
        is_failure = classification in {
            "different_terminal_result",
            "incomplete_search",
            "illegal_action",
            "protocol_failure",
            "different_legal_actions",
            "different_transition",
            "unknown",
        } or (strict_scores and classification == "different_score")
        mismatch_count += int(is_failure)
        rows.append(
            {
                "opponent": opponent,
                "fixture_id": fixture_id,
                "depth": depth,
                "repetition": repetition,
                "candidate_action": candidate.get("best_action") if candidate else None,
                "opponent_action": reference.get("best_action") if reference else None,
                "candidate_score": candidate.get("score") if candidate else None,
                "opponent_score": reference.get("score") if reference else None,
                "candidate_completed": candidate.get("completed") if candidate else None,
                "opponent_completed": reference.get("completed") if reference else None,
                "classification": classification,
                "correctness_failure": is_failure,
            }
        )
    fields = list(rows[0]) if rows else ["classification"]
    write_csv(output_path, rows, fields)
    return {"comparisons": len(rows), "mismatches": mismatch_count, "passed": mismatch_count == 0}
