from __future__ import annotations

import math
import random
import statistics
from collections import Counter, defaultdict
from statistics import NormalDist
from typing import Any, Iterable

from .common import percentile


def score_value(result: str) -> float:
    return {"win": 1.0, "draw": 0.5, "loss": 0.0}[result]


def _wilson_interval(mean: float, count: int, confidence: float) -> tuple[float, float]:
    """Conservative finite-sample envelope for bounded pair scores.

    Pair scores are fractional cluster observations rather than Bernoulli trials.  The Wilson
    expression is therefore used as a deliberately conservative width floor around the paired
    bootstrap, not as a replacement for the paired resampling distribution.
    """
    if count <= 0:
        return (0.0, 1.0)
    z = NormalDist().inv_cdf(0.5 + confidence / 2.0)
    z2 = z * z
    denominator = 1.0 + z2 / count
    center = (mean + z2 / (2.0 * count)) / denominator
    half_width = (
        z
        * math.sqrt(max(0.0, mean * (1.0 - mean) / count + z2 / (4.0 * count * count)))
        / denominator
    )
    return max(0.0, center - half_width), min(1.0, center + half_width)


def paired_bootstrap(
    pair_scores: list[float], samples: int, seed: int, confidence: float = 0.95
) -> dict[str, Any]:
    if not pair_scores:
        return {
            "method": "paired_bootstrap_with_wilson_width_floor",
            "seed": seed,
            "samples": samples,
            "pair_count": 0,
            "confidence": confidence,
            "mean": None,
            "lower": None,
            "upper": None,
            "empirical_lower": None,
            "empirical_upper": None,
            "finite_sample_lower": None,
            "finite_sample_upper": None,
            "unique_pair_scores": 0,
            "degenerate_input": False,
            "likelihood_of_superiority": None,
        }
    random_source = random.Random(seed)
    estimates = [
        statistics.fmean(random_source.choice(pair_scores) for _ in range(len(pair_scores)))
        for _ in range(samples)
    ]
    estimates.sort()
    tail = (1.0 - confidence) / 2.0
    empirical_lower = percentile(estimates, tail)
    empirical_upper = percentile(estimates, 1.0 - tail)
    mean = statistics.fmean(pair_scores)
    finite_lower, finite_upper = _wilson_interval(mean, len(pair_scores), confidence)
    lower = min(float(empirical_lower), finite_lower)
    upper = max(float(empirical_upper), finite_upper)
    greater = sum(value > 0.5 for value in estimates)
    equal = sum(value == 0.5 for value in estimates)
    return {
        "method": "paired_bootstrap_with_wilson_width_floor",
        "seed": seed,
        "samples": samples,
        "pair_count": len(pair_scores),
        "confidence": confidence,
        "mean": mean,
        "lower": lower,
        "upper": upper,
        "empirical_lower": empirical_lower,
        "empirical_upper": empirical_upper,
        "finite_sample_lower": finite_lower,
        "finite_sample_upper": finite_upper,
        "unique_pair_scores": len(set(pair_scores)),
        "degenerate_input": len(set(pair_scores)) == 1,
        "likelihood_of_superiority": (greater + 0.5 * equal) / len(estimates),
    }


def _estimated_additional_pairs(
    pair_count: int,
    lower: float | None,
    upper: float | None,
    *,
    minimum_pairs: int | dict[str, int] = 0,
    maximum_pairs: int | dict[str, int] | None = None,
    target_half_width: float = 0.02,
) -> int:
    minimum_needed = max(0, minimum_pairs - pair_count)
    if pair_count <= 0 or lower is None or upper is None:
        estimate = max(100, minimum_needed)
    else:
        half_width = max(0.0, (upper - lower) / 2.0)
        target_total = (
            pair_count
            if half_width <= target_half_width
            else math.ceil(pair_count * (half_width / target_half_width) ** 2)
        )
        estimate = max(minimum_needed, target_total - pair_count)
    if maximum_pairs is not None:
        estimate = min(estimate, max(0, maximum_pairs - pair_count))
    return max(0, min(10_000, estimate))


def _distribution(values: Iterable[float | int | None]) -> dict[str, Any] | None:
    numeric = sorted(float(value) for value in values if value is not None)
    if not numeric:
        return None
    return {
        "count": len(numeric),
        "minimum": numeric[0],
        "median": statistics.median(numeric),
        "p90": percentile(numeric, 0.90),
        "maximum": numeric[-1],
    }


def _elo(score: float | None) -> float | None:
    if score is None:
        return None
    bounded = min(1.0 - 1e-6, max(1e-6, score))
    return 400.0 * math.log10(bounded / (1.0 - bounded))


def _pair_key(pair: list[dict[str, Any]], candidate_id: str) -> str:
    by_role = {
        "first" if game["engine_first"] == candidate_id else "second": game["candidate_result"]
        for game in pair
    }
    abbreviate = {"win": "W", "draw": "D", "loss": "L"}
    return f"{abbreviate[by_role['first']]}_{abbreviate[by_role['second']]}"


def summarize_games(
    games: list[dict[str, Any]],
    moves: list[dict[str, Any]],
    candidate_id: str,
    bootstrap_samples: int,
    bootstrap_seed: int,
    bootstrap_confidence: float = 0.95,
    minimum_pairs: int = 0,
    maximum_pairs: int | None = None,
) -> dict[str, Any]:
    summaries: dict[str, Any] = {}
    by_opponent: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for game in games:
        by_opponent[game["opponent_version"]].append(game)
    for opponent, opponent_games in sorted(by_opponent.items()):
        opponent_minimum_pairs = (
            int(minimum_pairs.get(opponent, 0)) if isinstance(minimum_pairs, dict) else int(minimum_pairs)
        )
        opponent_maximum_pairs = (
            maximum_pairs.get(opponent)
            if isinstance(maximum_pairs, dict)
            else maximum_pairs
        )
        valid = [game for game in opponent_games if game.get("valid_strength_game")]
        grouped_pairs: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for game in valid:
            grouped_pairs[game["pair_id"]].append(game)
        complete_pairs = [
            pair
            for pair in grouped_pairs.values()
            if len(pair) == 2
            and sum(game["engine_first"] == candidate_id for game in pair) == 1
            and sum(game["engine_second"] == candidate_id for game in pair) == 1
        ]
        paired_games = [game for pair in complete_pairs for game in pair]
        wins = sum(game["candidate_result"] == "win" for game in paired_games)
        draws = sum(game["candidate_result"] == "draw" for game in paired_games)
        losses = sum(game["candidate_result"] == "loss" for game in paired_games)
        pair_scores = [
            statistics.fmean(score_value(game["candidate_result"]) for game in pair)
            for pair in complete_pairs
        ]
        bootstrap = paired_bootstrap(
            pair_scores,
            max(10_000, bootstrap_samples),
            bootstrap_seed,
            bootstrap_confidence,
        )
        candidate_first_games = [game for game in paired_games if game["engine_first"] == candidate_id]
        candidate_second_games = [game for game in paired_games if game["engine_second"] == candidate_id]
        first_player_scores = [
            score_value(game["candidate_result"])
            if game["engine_first"] == candidate_id
            else 1.0 - score_value(game["candidate_result"])
            for game in paired_games
        ]
        pair_outcomes = Counter(_pair_key(pair, candidate_id) for pair in complete_pairs)
        candidate_moves = [
            move
            for move in moves
            if move.get("opponent_id") == opponent and move.get("engine_version") == candidate_id
        ]
        move_times = sorted(float(move["elapsed_wall_ms"]) for move in candidate_moves)
        game_lengths = sorted(int(game["total_actions"]) for game in paired_games)
        decisive_lengths = sorted(
            int(game["total_actions"]) for game in paired_games if game["candidate_result"] != "draw"
        )
        draw_lengths = sorted(
            int(game["total_actions"]) for game in paired_games if game["candidate_result"] == "draw"
        )
        operational = [game for game in opponent_games if not game.get("valid_strength_game")]
        candidate_score = statistics.fmean(pair_scores) if pair_scores else None
        summary = {
            "opponent": opponent,
            "wins": wins,
            "draws": draws,
            "losses": losses,
            "valid_games": len(valid),
            "paired_valid_games": len(paired_games),
            "complete_pairs": len(complete_pairs),
            "incomplete_games": len(opponent_games) - len(valid),
            "unpaired_valid_games": len(valid) - len(paired_games),
            "candidate_score": candidate_score,
            "candidate_as_first_score": (
                statistics.fmean(score_value(game["candidate_result"]) for game in candidate_first_games)
                if candidate_first_games
                else None
            ),
            "candidate_as_second_score": (
                statistics.fmean(score_value(game["candidate_result"]) for game in candidate_second_games)
                if candidate_second_games
                else None
            ),
            "first_player_score": statistics.fmean(first_player_scores) if first_player_scores else None,
            "second_player_score": (
                1.0 - statistics.fmean(first_player_scores) if first_player_scores else None
            ),
            "first_player_bias": (
                statistics.fmean(first_player_scores) - 0.5 if first_player_scores else None
            ),
            "catch_wins": sum(
                game["candidate_result"] == "win" and game["terminal_type"] == "catch"
                for game in paired_games
            ),
            "try_wins": sum(
                game["candidate_result"] == "win" and game["terminal_type"] == "try"
                for game in paired_games
            ),
            "draw_rate": draws / len(paired_games) if paired_games else None,
            "average_game_length": statistics.fmean(game_lengths) if game_lengths else None,
            "median_game_length": statistics.median(game_lengths) if game_lengths else None,
            "decisive_game_length": _distribution(decisive_lengths),
            "draw_game_length": _distribution(draw_lengths),
            "median_move_time_ms": statistics.median(move_times) if move_times else None,
            "p90_move_time_ms": percentile(move_times, 0.90),
            "p95_move_time_ms": percentile(move_times, 0.95),
            "p99_move_time_ms": percentile(move_times, 0.99),
            "completed_depth_distribution": _distribution(
                move.get("completed_depth") for move in candidate_moves
            ),
            "node_distribution": _distribution(move.get("nodes") for move in candidate_moves),
            "tt_hit_distribution": _distribution(move.get("tt_hit_rate") for move in candidate_moves),
            "first_move_cutoff_distribution": _distribution(
                move.get("first_move_cutoff_rate") for move in candidate_moves
            ),
            "pair_outcomes": {key: pair_outcomes.get(key, 0) for key in (
                "W_W", "W_D", "W_L", "D_W", "D_D", "D_L", "L_W", "L_D", "L_L"
            )},
            "candidate_pair_sweeps": pair_outcomes.get("W_W", 0),
            "opponent_pair_sweeps": pair_outcomes.get("L_L", 0),
            "split_pairs": pair_outcomes.get("W_L", 0) + pair_outcomes.get("L_W", 0),
            "paired_double_losses": pair_outcomes.get("L_L", 0),
            "opening_pair_score_distribution": _distribution(pair_scores),
            "opening_bias_range": max(pair_scores) - min(pair_scores) if pair_scores else None,
            "illegal_actions": sum(game.get("illegal_engine") == candidate_id for game in opponent_games),
            "crashes": sum(game.get("crashed_engine") == candidate_id for game in opponent_games),
            "timeouts": sum(game.get("timed_out_engine") == candidate_id for game in opponent_games),
            "protocol_errors": sum(
                game.get("protocol_error_engine") == candidate_id for game in opponent_games
            ),
            "replay_failures": sum(not game.get("replay_passed", False) for game in opponent_games),
            "operational_failures": len(operational),
            "bootstrap": bootstrap,
            "minimum_pairs_required": opponent_minimum_pairs,
            "strength_sample_ready": len(complete_pairs) >= opponent_minimum_pairs,
            "elo_estimate": _elo(candidate_score),
            "elo_confidence_interval": {
                "lower": _elo(bootstrap["lower"]),
                "upper": _elo(bootstrap["upper"]),
            },
        }
        summary["additional_recommended_pairs"] = _estimated_additional_pairs(
            len(pair_scores),
            bootstrap["lower"],
            bootstrap["upper"],
            minimum_pairs=opponent_minimum_pairs,
            maximum_pairs=opponent_maximum_pairs,
        )
        summaries[opponent] = summary
    return summaries


def reliability_summary(game_summaries: dict[str, Any]) -> dict[str, Any]:
    return {
        "candidate_illegal_actions": sum(item["illegal_actions"] for item in game_summaries.values()),
        "candidate_crashes": sum(item["crashes"] for item in game_summaries.values()),
        "candidate_timeouts": sum(item["timeouts"] for item in game_summaries.values()),
        "candidate_protocol_errors": sum(item["protocol_errors"] for item in game_summaries.values()),
        "replay_failures": sum(item["replay_failures"] for item in game_summaries.values()),
        "operational_failures": sum(item["operational_failures"] for item in game_summaries.values()),
    }


def _permissions(profile: dict[str, Any]) -> set[str]:
    configured = profile.get("acceptance_permissions")
    if configured is not None:
        return set(configured)
    return {"performance", "strength", "equivalent"} if profile.get("strength_claim_allowed") else set()


def _finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _valid_fixed_depth_comparisons(summary: dict[str, Any]) -> list[dict[str, Any]]:
    comparisons = summary.get("comparisons")
    if not isinstance(comparisons, list) or not comparisons:
        return []
    declared_count = summary.get("comparison_count")
    if not isinstance(declared_count, int) or declared_count != len(comparisons):
        return []
    for row in comparisons:
        if not isinstance(row, dict) or not row.get("fixture_id"):
            return []
        if not _finite_number(row.get("depth")) or float(row["depth"]) <= 0:
            return []
        has_node_evidence = all(
            _finite_number(row.get(field))
            for field in ("candidate_median_nodes", "reference_median_nodes", "node_ratio")
        )
        has_timing_evidence = all(
            _finite_number(row.get(field))
            for field in (
                "candidate_median_elapsed_ms",
                "reference_median_elapsed_ms",
                "elapsed_ratio",
            )
        )
        if not has_node_evidence and not has_timing_evidence:
            return []
    return comparisons


def _valid_fixed_time_comparisons(summary: dict[str, Any]) -> list[dict[str, Any]]:
    comparisons = summary.get("comparisons")
    if not isinstance(comparisons, list) or not comparisons:
        return []
    declared_count = summary.get("comparison_count")
    if not isinstance(declared_count, int) or declared_count != len(comparisons):
        return []
    required = (
        "budget_ms",
        "candidate_median_completed_depth",
        "reference_median_completed_depth",
    )
    if any(
        not isinstance(row, dict)
        or not row.get("fixture_id")
        or not all(_finite_number(row.get(field)) for field in required)
        for row in comparisons
    ):
        return []
    return comparisons


def _completed_depth_truth_evidence(
    correctness: dict[str, Any], time_comparisons: list[dict[str, Any]]
) -> bool:
    explicit = correctness.get("completed_depth_truth_passed")
    if explicit is not None:
        return explicit is True
    if not time_comparisons:
        return False
    rate_fields = ("candidate_search_completed_rate", "reference_search_completed_rate")
    return all(
        all(
            _finite_number(row.get(field)) and 0.0 <= float(row[field]) <= 1.0
            for field in rate_fields
        )
        for row in time_comparisons
    )


def _blocking_divergence_classifications(divergences: Any) -> set[str]:
    if divergences is None:
        return set()
    entries: Any = divergences
    if isinstance(divergences, dict):
        if isinstance(divergences.get("divergences"), list):
            entries = divergences["divergences"]
        elif isinstance(divergences.get("items"), list):
            entries = divergences["items"]
        elif "classification" in divergences:
            entries = [divergences]
        else:
            entries = []
    if not isinstance(entries, list):
        return set()
    blocked = {"correctness_mismatch", "different_fixed_depth_score"}
    return {
        str(entry.get("classification"))
        for entry in entries
        if isinstance(entry, dict) and entry.get("classification") in blocked
    }


def _rate(summary: dict[str, Any], field: str) -> float:
    value = summary.get(field)
    return float(value) if _finite_number(value) else 0.0


def classify_candidate(
    *,
    change_category: str,
    profile: dict[str, Any],
    policy: dict[str, Any],
    correctness: dict[str, Any],
    reliability: dict[str, Any],
    performance: dict[str, Any],
    game_summaries: dict[str, Any],
    champion_id: str,
    anchor_id: str,
    time_performance: dict[str, Any] | None = None,
    divergences: list[dict[str, Any]] | dict[str, Any] | None = None,
) -> tuple[str, dict[str, bool]]:
    correctness_policy = policy["correctness"]
    reliability_policy = policy["reliability"]
    performance_policy = policy["performance_upgrade"]
    strength_policy = policy["strength_upgrade"]
    rejection_policy = policy["strength_rejection"]
    statistics_policy = policy.get("statistics", {})
    blocking_divergences = _blocking_divergence_classifications(divergences)
    divergence_gate = not blocking_divergences
    correctness_gate = (
        bool(correctness.get("passed"))
        and divergence_gate
        and reliability["candidate_illegal_actions"]
        <= correctness_policy["maximum_candidate_illegal_actions"]
    )
    reliability_gate = (
        reliability["candidate_crashes"] <= reliability_policy["maximum_candidate_crashes"]
        and reliability["candidate_timeouts"]
        <= reliability_policy["maximum_candidate_external_timeouts"]
        and reliability["candidate_protocol_errors"]
        <= reliability_policy["maximum_candidate_protocol_desynchronizations"]
        and reliability["replay_failures"] == 0
        and reliability.get("operational_failures", 0) == 0
    )
    gates: dict[str, bool] = {
        "correctness": correctness_gate,
        "reliability": reliability_gate,
        "divergence_consistency": divergence_gate,
        "artifact_binding": correctness.get("artifact_binding_verified") is True,
    }
    if not correctness_gate:
        return "REJECT_CORRECTNESS", gates
    if not reliability_gate:
        return "REJECT_RELIABILITY", gates
    if change_category == "rule_change":
        gates["separate_rule_audit_required"] = True
        return "INCONCLUSIVE", gates
    permissions = _permissions(profile)
    if not permissions:
        gates["profile_allows_acceptance"] = False
        return "INCONCLUSIVE", gates
    configuration_gate = change_category not in {"performance_only", "optimization_only"} or bool(
        correctness.get("configurations_equivalent", True)
    )
    gates["configuration_equivalence"] = configuration_gate
    if not configuration_gate:
        return "INCONCLUSIVE", gates

    champion_stats = game_summaries.get(champion_id, {})
    anchor_stats = game_summaries.get(anchor_id, champion_stats if anchor_id == champion_id else {})
    champion_bootstrap = champion_stats.get("bootstrap", {})
    champion_lower = champion_bootstrap.get("lower")
    champion_upper = champion_bootstrap.get("upper")
    anchor_lower = anchor_stats.get("bootstrap", {}).get("lower")
    minimum_pairs = int(
        profile.get(
            "minimum_pairs_for_strength",
            statistics_policy.get("minimum_valid_pairs_for_claim", 30),
        )
    )
    minimum_anchor_pairs = int(profile.get("minimum_anchor_pairs_for_strength", minimum_pairs))
    sample_gate = (
        int(champion_bootstrap.get("pair_count", 0)) >= minimum_pairs
        and int(anchor_stats.get("bootstrap", {}).get("pair_count", 0)) >= minimum_anchor_pairs
    )
    gates["minimum_strength_sample"] = sample_gate

    champion_performance = performance.get("opponents", {}).get(champion_id, {})
    fixed_comparisons = _valid_fixed_depth_comparisons(champion_performance)
    fixed_depth_evidence = bool(fixed_comparisons)
    gates["fixed_depth_performance_evidence"] = fixed_depth_evidence
    elapsed_ratio = champion_performance.get("geometric_mean_elapsed_ratio")
    node_ratio = champion_performance.get(
        "geometric_mean_stable_node_ratio",
        champion_performance.get("geometric_mean_node_ratio"),
    )
    reported_regressions = [
        *champion_performance.get("critical_regressions", []),
        *champion_performance.get("node_regressions", []),
        *champion_performance.get("node_cost_regressions", []),
    ]
    maximum_fixture_regression = float(
        performance_policy.get("maximum_critical_fixture_elapsed_regression", 0.05)
    )
    comparison_regression = any(
        (
            row.get("timing_authoritative") is True
            and _finite_number(row.get("elapsed_ratio"))
            and float(row["elapsed_ratio"]) > 1.0 + maximum_fixture_regression
        )
        or (
            _finite_number(row.get("node_ratio"))
            and float(row["node_ratio"]) > 1.0 + maximum_fixture_regression
        )
        or (
            row.get("timing_authoritative") is True
            and _finite_number(row.get("node_cost_ratio"))
            and float(row["node_cost_ratio"]) > 1.0 + maximum_fixture_regression
        )
        for row in fixed_comparisons
    )
    performance_non_regression = fixed_depth_evidence and not reported_regressions and not comparison_regression
    gates["fixed_depth_non_regression"] = performance_non_regression
    timing_quality = bool(
        champion_performance.get(
            "timing_quality_passed", champion_performance.get("comparison_count", 0) > 0
        )
    )
    stable_nodes = fixed_depth_evidence and all(
        row.get("candidate_node_count_stable") is True
        and row.get("reference_node_count_stable") is True
        for row in fixed_comparisons
    )
    noise_floor = float(champion_performance.get("timing_noise_floor") or 0.0)
    memory_ratio = champion_performance.get("maximum_peak_memory_ratio")
    timing_evidence = (
        fixed_depth_evidence
        and int(champion_performance.get("authoritative_timing_comparison_count", 0)) > 0
        and _finite_number(elapsed_ratio)
    )
    memory_evidence = (
        fixed_depth_evidence
        and _finite_number(memory_ratio)
        and all(_finite_number(row.get("peak_memory_ratio")) for row in fixed_comparisons)
    )
    memory_gate = memory_evidence and float(memory_ratio) <= 1.0 + float(
        strength_policy.get("maximum_peak_memory_regression", 0.20)
    )
    gates["timing_evidence"] = timing_evidence
    gates["memory_evidence"] = memory_evidence
    gates["memory_non_regression"] = memory_gate
    required_elapsed_improvement = max(
        float(performance_policy["minimum_elapsed_improvement"]), noise_floor
    )
    elapsed_improved = (
        timing_quality
        and elapsed_ratio is not None
        and elapsed_ratio <= 1.0 - required_elapsed_improvement
    )
    nodes_improved = (
        stable_nodes
        and node_ratio is not None
        and node_ratio <= 1.0 - performance_policy["minimum_node_improvement"]
    )
    performance_gate = (
        fixed_depth_evidence
        and (elapsed_improved or nodes_improved)
        and performance_non_regression
        and memory_gate
    )
    gates["timing_quality"] = timing_quality
    gates["stable_nodes"] = stable_nodes
    gates["performance"] = performance_gate

    category_policy = policy.get("category_requirements", {}).get(change_category, {})
    time_opponent = (time_performance or {}).get("opponents", {}).get(champion_id, {})
    time_comparisons = _valid_fixed_time_comparisons(time_opponent)
    fixed_time_evidence = bool(time_comparisons)
    fixed_time_policy = policy.get("fixed_time", {})
    candidate_fallback_rate = _rate(time_opponent, "candidate_fallback_rate")
    reference_fallback_rate = _rate(time_opponent, "reference_fallback_rate")
    candidate_hard_stop_rate = _rate(time_opponent, "candidate_timeout_rate")
    reference_hard_stop_rate = _rate(time_opponent, "reference_timeout_rate")
    fixed_time_required = bool(category_policy.get("require_fixed_time_non_regression"))
    fixed_time_gate = (
        fixed_time_evidence or not fixed_time_required
    ) and (not fixed_time_evidence or (
        int(time_opponent.get("depth_regression_count", 0))
        <= int(fixed_time_policy.get("maximum_depth_regressions", 0))
        and candidate_fallback_rate
        <= reference_fallback_rate
        + float(fixed_time_policy.get("maximum_fallback_rate_delta", 0.0))
        and candidate_hard_stop_rate
        <= reference_hard_stop_rate
        + float(fixed_time_policy.get("maximum_hard_stop_rate_delta", 0.0))
        and float(time_opponent.get("maximum_deadline_overshoot_ms") or 0.0)
        <= float(fixed_time_policy.get("maximum_hard_limit_overshoot_ms", 25.0))
    ))
    gates["fixed_time_evidence"] = fixed_time_evidence
    gates["fixed_time_non_regression"] = fixed_time_gate
    reference_gate = True
    if category_policy.get("require_reference_checks"):
        reference_gate = bool(correctness.get("reference_checks_passed"))
    gates["reference_checks"] = reference_gate
    completed_depth_truth_gate = (
        not category_policy.get("require_completed_depth_truth")
        or _completed_depth_truth_evidence(correctness, time_comparisons)
    )
    gates["completed_depth_truth"] = completed_depth_truth_gate
    strength_upgrade_required = bool(category_policy.get("require_strength_upgrade"))
    gates["category_allows_equivalent"] = not strength_upgrade_required

    lower_48 = sample_gate and champion_lower is not None and champion_lower >= performance_policy[
        "minimum_strength_lower_bound"
    ]
    strength_gate = (
        sample_gate
        and reference_gate
        and completed_depth_truth_gate
        and fixed_time_gate
        and fixed_depth_evidence
        and timing_evidence
        and performance_non_regression
        and memory_gate
        and champion_lower is not None
        and champion_lower > strength_policy["minimum_champion_lower_bound_exclusive"]
        and anchor_lower is not None
        and anchor_lower >= strength_policy["minimum_anchor_lower_bound"]
        and (
            float(elapsed_ratio)
            <= 1.0 + strength_policy["maximum_production_speed_regression"]
        )
    )
    gates["strength"] = strength_gate

    if not gates["artifact_binding"]:
        return "INCONCLUSIVE", gates

    optimization_categories = {"performance_only", "optimization_only"}
    if change_category in optimization_categories:
        if "performance" in permissions and performance_gate and lower_48:
            return "ACCEPT_PERFORMANCE_UPGRADE", gates
        if sample_gate and champion_upper is not None and champion_upper < rejection_policy["maximum_upper_bound"]:
            return "REJECT_STRENGTH", gates
        if (
            fixed_depth_evidence
            and sample_gate
            and not performance_gate
            and (timing_evidence or stable_nodes)
        ):
            return "REJECT_PERFORMANCE", gates
        return "INCONCLUSIVE", gates

    if "strength" in permissions and strength_gate:
        return "ACCEPT_STRENGTH_UPGRADE", gates
    if sample_gate and champion_upper is not None and champion_upper < rejection_policy["maximum_upper_bound"]:
        return "REJECT_STRENGTH", gates
    maximum_equivalence_width = float(statistics_policy.get("maximum_equivalence_ci_width", 0.08))
    if (
        "equivalent" in permissions
        and not strength_upgrade_required
        and sample_gate
        and reference_gate
        and completed_depth_truth_gate
        and fixed_time_gate
        and fixed_depth_evidence
        and timing_evidence
        and performance_non_regression
        and memory_gate
        and champion_lower is not None
        and champion_upper is not None
        and champion_lower <= 0.5 <= champion_upper
        and champion_upper - champion_lower <= maximum_equivalence_width
    ):
        return "ACCEPT_EQUIVALENT_REPLACEMENT", gates
    return "INCONCLUSIVE", gates
