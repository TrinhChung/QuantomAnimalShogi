from __future__ import annotations

import json
import math
import random
import statistics
from collections import Counter
from pathlib import Path
from typing import Any

from .common import EvaluationError, atomic_write_json, repository_root, sha256_file, utc_now
from .referee import NativeReferee


CORPUS_REVISION = 1
CORPUS_SEED = 0x51415335


def corpus_root() -> Path:
    return repository_root() / "evaluation" / "corpus"


def fixture_record(
    snapshot: dict[str, Any], fixture_id: str, source: str, seed: int, sequence: list[int], labels: list[str]
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "corpus_revision": CORPUS_REVISION,
        "fixture_id": fixture_id,
        "full_serialized_state": snapshot["state_text"],
        "structural_hash": snapshot["state_hash"],
        "source": source,
        "generation_seed": seed,
        "generation_action_sequence": sequence,
        "turn": snapshot["turn"],
        "side_to_move": snapshot["side"],
        "remaining_horizon": snapshot["remaining_horizon"],
        "legal_action_count": snapshot["legal_count"],
        "uncertainty_score": snapshot["uncertainty_score"],
        "hand_piece_count": snapshot["hand_piece_count"],
        "lion_candidate_counts": snapshot["lion_candidate_counts"],
        "terminal_status": snapshot["terminal"],
        "category_labels": sorted(set(labels)),
    }


def _random_nonterminal_snapshot(
    referee: NativeReferee, seed: int, plies: int
) -> tuple[dict[str, Any], list[int]]:
    for attempt in range(1000):
        random_source = random.Random(seed + attempt * 0x9E3779B1)
        snapshot = referee.initial()
        sequence: list[int] = []
        for _ in range(plies):
            actions = list(snapshot["legal_actions"])
            random_source.shuffle(actions)
            if not actions:
                break
            snapshot = referee.apply(actions[0])
            sequence.append(actions[0])
            if snapshot["terminal"] != "none":
                break
        if len(sequence) == plies and snapshot["terminal"] == "none":
            return snapshot, sequence
    raise EvaluationError(f"could not generate deterministic nonterminal state at ply {plies}")


def _stage35_fixture_states() -> list[tuple[str, str]]:
    directory = repository_root() / "benchmarks" / "fixtures" / "stage35"
    selected = [
        "initial",
        "duplicate_hands",
        "many_hands",
        "high_uncertainty_midgame",
        "low_uncertainty_midgame",
        "near_catch",
        "near_try",
        "near_draw",
        "safe_4ply",
        "safe_8ply",
    ]
    fixtures: list[tuple[str, str]] = []
    for name in selected:
        path = directory / f"{name}.fixture"
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        begin = text.find("state_begin\n")
        end = text.find("state_end", begin)
        if begin >= 0 and end >= 0:
            fixtures.append((name, text[begin + len("state_begin\n") : end]))
    return fixtures


def generate_corpus(referee_executable: Path, output_root: Path | None = None) -> dict[str, Any]:
    root = output_root or corpus_root()
    root.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, Any]] = []
    hashes: set[int] = set()
    with NativeReferee(referee_executable) as referee:
        for name, state_text in _stage35_fixture_states():
            snapshot = referee.load(state_text)
            labels = ["correctness", "performance", "tactical"]
            if "initial" in name or "safe" in name:
                labels.append("openings")
            if "catch" in name or "try" in name:
                labels.append("tactical")
            record = fixture_record(snapshot, name, "stage35_trusted_fixture", CORPUS_SEED, [], labels)
            if record["structural_hash"] not in hashes:
                records.append(record)
                hashes.add(record["structural_hash"])

        target_count = 256
        index = 0
        plies = [2, 4, 6, 8, 12, 16, 20, 24]
        while len(records) < target_count:
            ply = plies[index % len(plies)]
            seed = CORPUS_SEED + index
            snapshot, sequence = _random_nonterminal_snapshot(referee, seed, ply)
            index += 1
            if snapshot["state_hash"] in hashes:
                continue
            labels = ["openings", "performance", f"ply_{ply}"]
            if snapshot["uncertainty_score"] >= 24:
                labels.append("high_uncertainty")
            if snapshot["uncertainty_score"] <= 16:
                labels.append("low_uncertainty")
            if snapshot["hand_piece_count"] >= 2:
                labels.append("many_hands")
            if snapshot["legal_count"] >= 20:
                labels.append("high_branching")
            if snapshot["legal_count"] <= 6:
                labels.append("low_branching")
            records.append(
                fixture_record(
                    snapshot,
                    f"random_ply{ply}_{seed:08x}",
                    "deterministic_authoritative_legal_playout",
                    seed,
                    sequence,
                    labels,
                )
            )
            hashes.add(snapshot["state_hash"])

    records.sort(key=lambda fixture: fixture["fixture_id"])
    fixtures_path = root / "corpus-v1.jsonl"
    fixtures_path.write_text(
        "".join(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n" for record in records),
        encoding="utf-8",
        newline="\n",
    )
    manifest = {
        "schema_version": 1,
        "corpus_revision": CORPUS_REVISION,
        "created_at_utc": utc_now(),
        "fixture_count": len(records),
        "fixtures_file": fixtures_path.name,
        "fixtures_sha256": sha256_file(fixtures_path),
        "generation_seed": CORPUS_SEED,
        "generator": "evaluation.tools.corpus.generate_corpus",
        "immutability_rule": "Adding or changing fixtures requires a new corpus revision.",
    }
    atomic_write_json(root / "manifest.json", manifest)
    return manifest


def load_corpus(root: Path | None = None) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    directory = root or corpus_root()
    manifest_path = directory / "manifest.json"
    if not manifest_path.is_file():
        raise EvaluationError("permanent corpus is missing; run the controlled corpus generator")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    fixture_path = directory / manifest["fixtures_file"]
    if sha256_file(fixture_path) != manifest["fixtures_sha256"]:
        raise EvaluationError("permanent corpus hash mismatch")
    fixtures = [json.loads(line) for line in fixture_path.read_text(encoding="utf-8").splitlines() if line]
    if len(fixtures) != manifest["fixture_count"]:
        raise EvaluationError("permanent corpus fixture count mismatch")
    return manifest, fixtures


def summarize_corpus(fixtures: list[dict[str, Any]]) -> dict[str, Any]:
    def distribution(field: str) -> dict[str, Any]:
        values = sorted(float(fixture[field]) for fixture in fixtures)
        index = lambda quantile: int((len(values) - 1) * quantile)  # noqa: E731
        return {
            "count": len(values),
            "minimum": values[0],
            "p10": values[index(0.10)],
            "median": statistics.median(values),
            "p90": values[index(0.90)],
            "maximum": values[-1],
            "mean": statistics.fmean(values),
        }

    labels = Counter(label for fixture in fixtures for label in fixture["category_labels"])
    identifiers = {fixture["fixture_id"] for fixture in fixtures}
    hashes = [fixture["structural_hash"] for fixture in fixtures]
    source_counts = Counter(fixture["source"] for fixture in fixtures)
    terminal_counts = Counter(fixture.get("terminal_status", "unknown") for fixture in fixtures)
    side_counts = Counter(fixture["side_to_move"] for fixture in fixtures)
    return {
        "total_positions": len(fixtures),
        "unique_positions": len(set(hashes)),
        "duplicate_positions": len(hashes) - len(set(hashes)),
        "source_distribution": dict(sorted(source_counts.items())),
        "terminal_distribution": dict(sorted(terminal_counts.items())),
        "side_to_move_distribution": dict(sorted(side_counts.items())),
        "reachable_with_recorded_sequence": sum(
            bool(fixture["generation_action_sequence"]) or fixture["fixture_id"] == "initial"
            for fixture in fixtures
        ),
        "reachability_unproven": [
            fixture["fixture_id"]
            for fixture in fixtures
            if not fixture["generation_action_sequence"] and fixture["fixture_id"] != "initial"
        ],
        "legal_action_count": distribution("legal_action_count"),
        "uncertainty_score": distribution("uncertainty_score"),
        "hand_piece_count": distribution("hand_piece_count"),
        "lion_candidates_south": _numeric_distribution(
            [float(fixture["lion_candidate_counts"][0]) for fixture in fixtures]
        ),
        "lion_candidates_north": _numeric_distribution(
            [float(fixture["lion_candidate_counts"][1]) for fixture in fixtures]
        ),
        "lion_candidates_total": _numeric_distribution(
            [float(sum(fixture["lion_candidate_counts"])) for fixture in fixtures]
        ),
        "remaining_horizon": distribution("remaining_horizon"),
        "turn": distribution("turn"),
        "category_counts": dict(sorted(labels.items())),
        "drop_available_positions": None,
        "many_lion_candidate_positions": sum(
            max(fixture["lion_candidate_counts"]) >= 4 for fixture in fixtures
        ),
        "near_256_positions": sum(int(fixture["remaining_horizon"]) <= 8 for fixture in fixtures),
        "leq_legal_threshold_positions": sum(
            int(fixture["legal_action_count"]) >= 24 for fixture in fixtures
        ),
        "named_coverage": {
            name: name in identifiers
            for name in (
                "initial",
                "duplicate_hands",
                "many_hands",
                "high_uncertainty_midgame",
                "low_uncertainty_midgame",
                "near_catch",
                "near_try",
                "near_draw",
            )
        },
        "dynamic_characterization": {
            "propagation_cost_distribution": None,
            "tt_reuse_potential": None,
            "leq_trigger_potential": None,
            "note": "Engine-dependent metrics are populated from diagnostic benchmark results, not fabricated in fixtures.",
        },
    }


def _numeric_distribution(values: list[float]) -> dict[str, Any] | None:
    finite = sorted(value for value in values if math.isfinite(value))
    if not finite:
        return None
    index = lambda quantile: int((len(finite) - 1) * quantile)  # noqa: E731
    return {
        "count": len(finite),
        "minimum": finite[0],
        "p10": finite[index(0.10)],
        "median": statistics.median(finite),
        "p90": finite[index(0.90)],
        "maximum": finite[-1],
        "mean": statistics.fmean(finite),
    }


def enrich_corpus_with_diagnostics(
    coverage: dict[str, Any],
    records: list[dict[str, Any]],
    candidate_version_id: str,
) -> dict[str, Any]:
    """Attach measured engine-dependent characterization without changing corpus fixtures."""
    unique: dict[tuple[Any, ...], dict[str, Any]] = {}
    for record in records:
        if not record.get("diagnostic") or record.get("version") != candidate_version_id:
            continue
        if record.get("error_type") or not record.get("legal_result"):
            continue
        key = (
            record.get("fixture_id"),
            record.get("stage"),
            record.get("depth"),
            record.get("budget_ms"),
            record.get("repetition"),
        )
        unique.setdefault(key, record)
    diagnostic = list(unique.values())

    def measured(field: str) -> dict[str, Any] | None:
        values = [
            float(record[field])
            for record in diagnostic
            if isinstance(record.get(field), (int, float))
        ]
        return _numeric_distribution(values)

    leq_triggered = [record for record in diagnostic if int(record.get("leq_calls") or 0) > 0]
    drop_fixtures = {
        record.get("fixture_id")
        for record in diagnostic
        if any(int(action) // 12 >= 12 for action in (record.get("legal_actions") or []))
    }
    measured_fixture_ids = {record.get("fixture_id") for record in diagnostic}
    coverage["drop_available_positions_observed_in_diagnostic_sample"] = len(drop_fixtures)
    coverage["dynamic_characterization"] = {
        "candidate_version": candidate_version_id,
        "diagnostic_run_count": len(diagnostic),
        "measured_fixture_count": len(measured_fixture_ids),
        "measured_fixture_ids": sorted(identifier for identifier in measured_fixture_ids if identifier),
        "propagation_calls_distribution": measured("propagation_calls"),
        "propagation_iterations_distribution": measured("propagation_iterations"),
        "propagation_cost_distribution_ms": measured("propagation_ms"),
        "tt_hit_rate_distribution": measured("tt_hit_rate"),
        "tt_probe_distribution": measured("tt_probes"),
        "leq_call_distribution": measured("leq_calls"),
        "leq_duplicate_ratio_distribution": measured("leq_duplicate_ratio"),
        "leq_triggered_runs": len(leq_triggered),
        "leq_trigger_rate": len(leq_triggered) / len(diagnostic) if diagnostic else None,
        "note": (
            "Measured only on the selected diagnostic sample; null means no diagnostic evidence, "
            "not a fabricated zero or a claim about the full immutable corpus."
        ),
    }
    return coverage


def select_openings(
    fixtures: list[dict[str, Any]],
    count: int,
    seed: int,
    allowed_plies: list[int] | None = None,
) -> list[dict[str, Any]]:
    allowed = set(allowed_plies or [])
    openings = [
        fixture
        for fixture in fixtures
        if "openings" in fixture["category_labels"]
        and (not allowed or int(fixture["turn"]) in allowed)
    ]
    if count > len(openings):
        raise EvaluationError(
            f"profile requires {count} openings but its ply filter leaves only {len(openings)}"
        )
    random_source = random.Random(seed)
    by_side = {
        side: [fixture for fixture in openings if fixture["side_to_move"] == side]
        for side in ("south", "north")
    }
    for values in by_side.values():
        random_source.shuffle(values)
    # Alternate sides while both are available. This cannot invent balance absent from the
    # immutable corpus, but it prevents the few opposite-side fixtures from being silently lost.
    selected: list[dict[str, Any]] = []
    while len(selected) < count and any(by_side.values()):
        for side in ("south", "north"):
            if by_side[side] and len(selected) < count:
                selected.append(by_side[side].pop())
    return selected


def select_performance(
    fixtures: list[dict[str, Any]],
    limit: int,
    seed: int = CORPUS_SEED,
    required_fixture_ids: list[str] | None = None,
    stratify_labels: list[str] | None = None,
) -> list[dict[str, Any]]:
    available = [fixture for fixture in fixtures if "performance" in fixture["category_labels"]]
    by_id = {fixture["fixture_id"]: fixture for fixture in available}
    required = list(required_fixture_ids or [])
    duplicate_required = sorted(
        fixture_id for fixture_id, count in Counter(required).items() if count > 1
    )
    if duplicate_required:
        raise EvaluationError(
            "performance selection repeats required fixture IDs: "
            + ", ".join(duplicate_required)
        )
    missing_required = [fixture_id for fixture_id in required if fixture_id not in by_id]
    if missing_required:
        raise EvaluationError(
            "required performance fixtures are missing or not performance-labelled: "
            + ", ".join(missing_required)
        )

    effective_limit = len(available) if limit <= 0 else min(limit, len(available))
    if len(required) > effective_limit:
        raise EvaluationError(
            f"performance fixture limit {effective_limit} is smaller than the "
            f"{len(required)} required fixtures: " + ", ".join(required)
        )
    if effective_limit == len(available):
        return available

    selected: list[dict[str, Any]] = []
    selected_ids: set[str] = set()

    def add(fixture: dict[str, Any]) -> None:
        if fixture["fixture_id"] not in selected_ids and len(selected) < effective_limit:
            selected.append(fixture)
            selected_ids.add(fixture["fixture_id"])

    for fixture_id in required:
        add(by_id[fixture_id])

    random_source = random.Random(seed)
    requested_strata = list(dict.fromkeys(stratify_labels or []))

    # First cover as many still-unrepresented strata as possible. A greedy set-cover pass
    # prefers positions carrying several requested labels, which avoids losing later strata
    # merely because the configured label list has more entries than the remaining slots.
    uncovered = {
        label
        for label in requested_strata
        if not any(label in fixture["category_labels"] for fixture in selected)
    }
    coverage_candidates = [
        fixture for fixture in available if fixture["fixture_id"] not in selected_ids
    ]
    random_source.shuffle(coverage_candidates)
    tie_break_order = {
        fixture["fixture_id"]: index for index, fixture in enumerate(coverage_candidates)
    }
    while uncovered and len(selected) < effective_limit:
        best = max(
            coverage_candidates,
            key=lambda fixture: (
                len(uncovered.intersection(fixture["category_labels"])),
                -tie_break_order[fixture["fixture_id"]],
            ),
            default=None,
        )
        if best is None:
            break
        covered = uncovered.intersection(best["category_labels"])
        if not covered:
            break
        add(best)
        coverage_candidates.remove(best)
        uncovered.difference_update(covered)

    strata: list[list[dict[str, Any]]] = []
    for label in requested_strata:
        values = [
            fixture
            for fixture in available
            if label in fixture["category_labels"] and fixture["fixture_id"] not in selected_ids
        ]
        random_source.shuffle(values)
        strata.append(values)
    random_source.shuffle(strata)
    while len(selected) < effective_limit and any(strata):
        for values in strata:
            while values and values[-1]["fixture_id"] in selected_ids:
                values.pop()
            if values:
                add(values.pop())
            if len(selected) >= effective_limit:
                break

    remainder = [fixture for fixture in available if fixture["fixture_id"] not in selected_ids]
    random_source.shuffle(remainder)
    for fixture in remainder:
        add(fixture)
    return selected
