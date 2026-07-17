from __future__ import annotations

import unittest

from evaluation.tools.common import load_json, repository_root
from evaluation.tools.summarize_results import (
    classify_candidate,
    paired_bootstrap,
    reliability_summary,
    summarize_games,
)


ROOT = repository_root()
POLICY = load_json(ROOT / "evaluation" / "config" / "acceptance_policy.json")


def game(pair: int, role: str, result: str, *, terminal: str = "catch", length: int = 20) -> dict:
    candidate_first = role == "first"
    return {
        "opponent_version": "champion",
        "valid_strength_game": True,
        "candidate_result": result,
        "engine_first": "candidate" if candidate_first else "champion",
        "engine_second": "champion" if candidate_first else "candidate",
        "pair_id": f"p{pair}",
        "terminal_type": terminal,
        "total_actions": length,
        "illegal_engine": None,
        "crashed_engine": None,
        "timed_out_engine": None,
        "protocol_error_engine": None,
        "replay_passed": True,
    }


def reliability() -> dict:
    return {
        "candidate_illegal_actions": 0,
        "candidate_crashes": 0,
        "candidate_timeouts": 0,
        "candidate_protocol_errors": 0,
        "replay_failures": 0,
        "operational_failures": 0,
    }


def fixed_performance(
    *,
    elapsed_ratio: float = 1.0,
    node_ratio: float = 1.0,
    node_cost_ratio: float = 1.0,
    memory_ratio: float | None = 1.0,
    authoritative: bool = True,
    regressions: dict[str, list[dict]] | None = None,
) -> dict:
    row = {
        "fixture_id": "fixture-1",
        "depth": 6,
        "candidate_median_elapsed_ms": 10.0 * elapsed_ratio,
        "reference_median_elapsed_ms": 10.0,
        "elapsed_ratio": elapsed_ratio,
        "candidate_median_nodes": 1000.0 * node_ratio,
        "reference_median_nodes": 1000.0,
        "node_ratio": node_ratio,
        "node_cost_ratio": node_cost_ratio,
        "peak_memory_ratio": memory_ratio,
        "timing_authoritative": authoritative,
        "candidate_node_count_stable": True,
        "reference_node_count_stable": True,
    }
    regression_lists = {
        "critical_regressions": [],
        "node_regressions": [],
        "node_cost_regressions": [],
    }
    regression_lists.update(regressions or {})
    return {
        "opponents": {
            "champion": {
                "comparison_count": 1,
                "authoritative_timing_comparison_count": int(authoritative),
                "timing_quality_passed": authoritative,
                "timing_noise_floor": 0.01 if authoritative else None,
                "geometric_mean_elapsed_ratio": elapsed_ratio if authoritative else None,
                "geometric_mean_stable_node_ratio": node_ratio,
                "maximum_peak_memory_ratio": memory_ratio,
                "comparisons": [row],
                **regression_lists,
            }
        }
    }


def fixed_time_performance(*, comparison_count: int = 1, include_truth: bool = True) -> dict:
    comparisons = []
    if comparison_count:
        row = {
            "fixture_id": "fixture-1",
            "budget_ms": 250,
            "candidate_median_completed_depth": 6,
            "reference_median_completed_depth": 6,
        }
        if include_truth:
            row.update(
                candidate_search_completed_rate=0.0,
                reference_search_completed_rate=0.0,
            )
        comparisons.append(row)
    return {
        "opponents": {
            "champion": {
                "comparison_count": comparison_count,
                "comparisons": comparisons,
                "depth_regression_count": 0,
                "candidate_fallback_rate": 0.0,
                "reference_fallback_rate": 0.0,
                "candidate_timeout_rate": 0.0,
                "reference_timeout_rate": 0.0,
                "maximum_deadline_overshoot_ms": 1.0,
            }
        }
    }


def synthetic_summary(*, lower: float, upper: float, pairs: int = 100) -> dict:
    return {
        "champion": {
            "bootstrap": {"pair_count": pairs, "lower": lower, "upper": upper},
        }
    }


class PairedStatisticsSafetyTests(unittest.TestCase):
    def test_performance_gate_requires_quality_and_strength_non_regression(self) -> None:
        games = [
            game(pair, role, "win" if pair < 60 else "loss")
            for pair in range(100)
            for role in ("first", "second")
        ]
        summary = summarize_games(games, [], "candidate", 10_000, 17, minimum_pairs=100)
        profile = {
            "acceptance_permissions": ["performance"],
            "minimum_pairs_for_strength": 100,
            "minimum_anchor_pairs_for_strength": 100,
        }
        faster = fixed_performance(elapsed_ratio=0.90)
        accepted, _ = classify_candidate(
            change_category="performance_only",
            profile=profile,
            policy=POLICY,
            correctness={
                "passed": True,
                "configurations_equivalent": True,
                "artifact_binding_verified": True,
            },
            reliability=reliability(),
            performance=faster,
            game_summaries=summary,
            champion_id="champion",
            anchor_id="champion",
        )
        self.assertEqual(accepted, "ACCEPT_PERFORMANCE_UPGRADE")
        slower = fixed_performance(elapsed_ratio=1.10)
        rejected, _ = classify_candidate(
            change_category="performance_only",
            profile=profile,
            policy=POLICY,
            correctness={
                "passed": True,
                "configurations_equivalent": True,
                "artifact_binding_verified": True,
            },
            reliability=reliability(),
            performance=slower,
            game_summaries=summary,
            champion_id="champion",
            anchor_id="champion",
        )
        self.assertEqual(rejected, "REJECT_PERFORMANCE")

    def test_degenerate_bootstrap_has_finite_sample_width(self) -> None:
        one_draw = paired_bootstrap([0.5], 10_000, 7)
        self.assertTrue(one_draw["degenerate_input"])
        self.assertLess(one_draw["lower"], 0.5)
        self.assertGreater(one_draw["upper"], 0.5)
        one_sweep = paired_bootstrap([1.0], 10_000, 7)
        self.assertLess(one_sweep["lower"], 1.0)
        self.assertEqual(one_sweep["empirical_lower"], 1.0)

    def test_pair_categories_bias_and_lengths_are_not_conflated(self) -> None:
        games = [
            game(0, "first", "win", length=12),
            game(0, "second", "loss", length=14),
            game(1, "first", "loss", length=16),
            game(1, "second", "draw", terminal="draw", length=252),
            game(2, "first", "draw", terminal="draw", length=252),
            game(2, "second", "draw", terminal="draw", length=252),
        ]
        summary = summarize_games(games, [], "candidate", 10_000, 3, minimum_pairs=10)["champion"]
        self.assertEqual(summary["pair_outcomes"]["W_L"], 1)
        self.assertEqual(summary["pair_outcomes"]["L_D"], 1)
        self.assertEqual(summary["pair_outcomes"]["D_D"], 1)
        self.assertEqual(summary["split_pairs"], 1)
        self.assertEqual(summary["decisive_game_length"]["count"], 3)
        self.assertEqual(summary["draw_game_length"]["count"], 3)
        self.assertFalse(summary["strength_sample_ready"])
        self.assertGreaterEqual(summary["additional_recommended_pairs"], 7)

    def test_one_pair_can_never_pass_a_strength_gate(self) -> None:
        summary = summarize_games(
            [game(0, "first", "win"), game(0, "second", "win")],
            [],
            "candidate",
            10_000,
            9,
            minimum_pairs=30,
        )
        classification, gates = classify_candidate(
            change_category="evaluation_change",
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 30,
                "minimum_anchor_pairs_for_strength": 30,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=summary,
            champion_id="champion",
            anchor_id="champion",
        )
        self.assertEqual(classification, "INCONCLUSIVE")
        self.assertFalse(gates["minimum_strength_sample"])

    def test_large_clear_sweep_can_pass_strength(self) -> None:
        games = [
            game(pair, role, "win")
            for pair in range(100)
            for role in ("first", "second")
        ]
        summary = summarize_games(games, [], "candidate", 10_000, 11, minimum_pairs=100)
        classification, gates = classify_candidate(
            change_category="evaluation_change",
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=summary,
            champion_id="champion",
            anchor_id="champion",
        )
        self.assertEqual(classification, "ACCEPT_STRENGTH_UPGRADE")
        self.assertTrue(gates["minimum_strength_sample"])

    def test_operational_failure_fails_reliability(self) -> None:
        summaries = {
            "champion": {
                "illegal_actions": 0,
                "crashes": 0,
                "timeouts": 0,
                "protocol_errors": 0,
                "replay_failures": 0,
                "operational_failures": 1,
            }
        }
        self.assertEqual(reliability_summary(summaries)["operational_failures"], 1)

    def test_fixed_time_gate_compares_expected_hard_stops_to_reference(self) -> None:
        games = [
            game(pair, role, "win")
            for pair in range(100)
            for role in ("first", "second")
        ]
        summary = summarize_games(games, [], "candidate", 10_000, 23, minimum_pairs=100)
        common = dict(
            depth_regression_count=0,
            candidate_fallback_rate=0.0,
            reference_fallback_rate=0.0,
            reference_timeout_rate=0.40,
            maximum_deadline_overshoot_ms=1.0,
        )
        arguments = dict(
            change_category="architecture_change",
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=summary,
            champion_id="champion",
            anchor_id="champion",
        )
        accepted, accepted_gates = classify_candidate(
            **arguments,
            time_performance={"opponents": {"champion": {
                **common,
                "candidate_timeout_rate": 0.42,
                "comparison_count": 1,
                "comparisons": [{
                    "fixture_id": "fixture-1",
                    "budget_ms": 250,
                    "candidate_median_completed_depth": 6,
                    "reference_median_completed_depth": 6,
                }],
            }}},
        )
        self.assertEqual(accepted, "ACCEPT_STRENGTH_UPGRADE")
        self.assertTrue(accepted_gates["fixed_time_non_regression"])
        inconclusive, regressed_gates = classify_candidate(
            **arguments,
            time_performance={"opponents": {"champion": {
                **common,
                "candidate_timeout_rate": 0.60,
                "comparison_count": 1,
                "comparisons": [{
                    "fixture_id": "fixture-1",
                    "budget_ms": 250,
                    "candidate_median_completed_depth": 6,
                    "reference_median_completed_depth": 6,
                }],
            }}},
        )
        self.assertEqual(inconclusive, "INCONCLUSIVE")
        self.assertFalse(regressed_gates["fixed_time_non_regression"])

    def test_strength_and_equivalence_require_real_fixed_depth_rows(self) -> None:
        strength_arguments = dict(
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance={
                "opponents": {
                    "champion": {
                        "comparison_count": 1,
                        "geometric_mean_elapsed_ratio": 1.0,
                        "maximum_peak_memory_ratio": 1.0,
                    }
                }
            },
            game_summaries=synthetic_summary(lower=0.60, upper=0.70),
            champion_id="champion",
            anchor_id="champion",
        )
        classification, gates = classify_candidate(
            change_category="evaluation_change", **strength_arguments
        )
        self.assertEqual(classification, "INCONCLUSIVE")
        self.assertFalse(gates["fixed_depth_performance_evidence"])

        equivalence_arguments = {
            **strength_arguments,
            "profile": {
                **strength_arguments["profile"],
                "acceptance_permissions": ["equivalent"],
            },
            "game_summaries": synthetic_summary(lower=0.48, upper=0.52),
        }
        classification, gates = classify_candidate(
            change_category="move_ordering", **equivalence_arguments
        )
        self.assertEqual(classification, "INCONCLUSIVE")
        self.assertFalse(gates["fixed_depth_performance_evidence"])

    def test_fixed_depth_regressions_block_strength_and_equivalence(self) -> None:
        regressed = fixed_performance(
            regressions={"node_regressions": [{"fixture_id": "fixture-1", "ratio": 1.06}]}
        )
        common = dict(
            profile={
                "acceptance_permissions": ["strength", "equivalent"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=regressed,
            champion_id="champion",
            anchor_id="champion",
        )
        strength, strength_gates = classify_candidate(
            change_category="evaluation_change",
            game_summaries=synthetic_summary(lower=0.60, upper=0.70),
            **common,
        )
        self.assertEqual(strength, "INCONCLUSIVE")
        self.assertFalse(strength_gates["fixed_depth_non_regression"])
        equivalent, equivalent_gates = classify_candidate(
            change_category="move_ordering",
            game_summaries=synthetic_summary(lower=0.48, upper=0.52),
            **common,
        )
        self.assertEqual(equivalent, "INCONCLUSIVE")
        self.assertFalse(equivalent_gates["fixed_depth_non_regression"])

    def test_strength_requires_authoritative_timing_and_memory_evidence(self) -> None:
        common = dict(
            change_category="evaluation_change",
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            game_summaries=synthetic_summary(lower=0.60, upper=0.70),
            champion_id="champion",
            anchor_id="champion",
        )
        no_timing, timing_gates = classify_candidate(
            performance=fixed_performance(authoritative=False), **common
        )
        self.assertEqual(no_timing, "INCONCLUSIVE")
        self.assertFalse(timing_gates["timing_evidence"])
        no_memory, memory_gates = classify_candidate(
            performance=fixed_performance(memory_ratio=None), **common
        )
        self.assertEqual(no_memory, "INCONCLUSIVE")
        self.assertFalse(memory_gates["memory_evidence"])

    def test_architecture_requires_nonempty_fixed_time_comparisons(self) -> None:
        common = dict(
            change_category="architecture_change",
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=synthetic_summary(lower=0.60, upper=0.70),
            champion_id="champion",
            anchor_id="champion",
        )
        missing, gates = classify_candidate(
            time_performance=fixed_time_performance(comparison_count=0), **common
        )
        self.assertEqual(missing, "INCONCLUSIVE")
        self.assertFalse(gates["fixed_time_evidence"])
        accepted, gates = classify_candidate(
            time_performance=fixed_time_performance(), **common
        )
        self.assertEqual(accepted, "ACCEPT_STRENGTH_UPGRADE")
        self.assertTrue(gates["fixed_time_evidence"])

    def test_strength_only_category_cannot_fall_back_to_equivalent(self) -> None:
        classification, gates = classify_candidate(
            change_category="evaluation_change",
            profile={
                "acceptance_permissions": ["equivalent"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=synthetic_summary(lower=0.48, upper=0.52),
            champion_id="champion",
            anchor_id="champion",
        )
        self.assertEqual(classification, "INCONCLUSIVE")
        self.assertFalse(gates["category_allows_equivalent"])

    def test_search_control_requires_completed_depth_truth(self) -> None:
        common = dict(
            change_category="search_control",
            profile={
                "acceptance_permissions": ["equivalent"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=synthetic_summary(lower=0.48, upper=0.52),
            champion_id="champion",
            anchor_id="champion",
        )
        missing, gates = classify_candidate(**common)
        self.assertEqual(missing, "INCONCLUSIVE")
        self.assertFalse(gates["completed_depth_truth"])
        accepted, gates = classify_candidate(
            time_performance=fixed_time_performance(include_truth=True), **common
        )
        self.assertEqual(accepted, "ACCEPT_EQUIVALENT_REPLACEMENT")
        self.assertTrue(gates["completed_depth_truth"])

    def test_correctness_divergence_blocks_acceptance(self) -> None:
        common = dict(
            change_category="evaluation_change",
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            policy=POLICY,
            correctness={"passed": True, "artifact_binding_verified": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=synthetic_summary(lower=0.60, upper=0.70),
            champion_id="champion",
            anchor_id="champion",
        )
        for divergence_class in ("correctness_mismatch", "different_fixed_depth_score"):
            with self.subTest(divergence_class=divergence_class):
                classification, gates = classify_candidate(
                    divergences=[{"classification": divergence_class}], **common
                )
                self.assertEqual(classification, "REJECT_CORRECTNESS")
                self.assertFalse(gates["divergence_consistency"])

    def test_every_accept_requires_artifact_binding_but_diagnostics_stay_inconclusive(self) -> None:
        common = dict(
            change_category="evaluation_change",
            policy=POLICY,
            correctness={"passed": True},
            reliability=reliability(),
            performance=fixed_performance(),
            game_summaries=synthetic_summary(lower=0.60, upper=0.70),
            champion_id="champion",
            anchor_id="champion",
        )
        unbound, gates = classify_candidate(
            profile={
                "acceptance_permissions": ["strength"],
                "minimum_pairs_for_strength": 100,
                "minimum_anchor_pairs_for_strength": 100,
            },
            **common,
        )
        self.assertEqual(unbound, "INCONCLUSIVE")
        self.assertFalse(gates["artifact_binding"])
        diagnostic, diagnostic_gates = classify_candidate(
            profile={"acceptance_permissions": []}, **common
        )
        self.assertEqual(diagnostic, "INCONCLUSIVE")
        self.assertFalse(diagnostic_gates["artifact_binding"])


if __name__ == "__main__":
    unittest.main()
