from __future__ import annotations

import unittest

from evaluation.tools.common import EvaluationError
from evaluation.tools.corpus import (
    enrich_corpus_with_diagnostics,
    load_corpus,
    select_performance,
    summarize_corpus,
)
from evaluation.tools.run_pipeline import _profiles


class ProfilesAndCorpusTests(unittest.TestCase):
    def test_required_profile_hierarchy_is_complete_and_documented(self) -> None:
        profiles = _profiles()
        required = {
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
        self.assertTrue(required <= profiles.keys())
        for name in required:
            profile = profiles[name]
            self.assertTrue(profile["purpose"])
            self.assertGreater(profile["maximum_expected_runtime_minutes"], 0)
            self.assertIn("stopping_rule", profile)
            self.assertIn("acceptance_permissions", profile)
            self.assertIn("rejection_conditions", profile)

    def test_stratified_selection_is_deterministic_and_not_alphabetical_prefix(self) -> None:
        _, fixtures = load_corpus()
        selection = {
            "required_fixture_ids": ["initial", "duplicate_hands", "near_catch"],
            "stratify_labels": ["high_uncertainty", "many_hands", "high_branching"],
        }
        first = select_performance(fixtures, 8, 42, **selection)
        second = select_performance(fixtures, 8, 42, **selection)
        self.assertEqual(first, second)
        self.assertEqual([row["fixture_id"] for row in first[:3]], selection["required_fixture_ids"])
        self.assertNotEqual(first, [row for row in fixtures if "performance" in row["category_labels"]][:8])

    def test_performance_selection_rejects_missing_required_fixture(self) -> None:
        fixtures = [
            {"fixture_id": "present", "category_labels": ["performance"]},
        ]
        with self.assertRaisesRegex(EvaluationError, "required performance fixtures.*missing"):
            select_performance(
                fixtures,
                1,
                required_fixture_ids=["missing"],
            )

    def test_performance_selection_rejects_required_set_larger_than_limit(self) -> None:
        fixtures = [
            {"fixture_id": fixture_id, "category_labels": ["performance"]}
            for fixture_id in ("required-a", "required-b", "optional")
        ]
        with self.assertRaisesRegex(EvaluationError, "limit 1 is smaller than the 2 required"):
            select_performance(
                fixtures,
                1,
                required_fixture_ids=["required-a", "required-b"],
            )

    def test_stratified_selection_uses_multi_label_fixture_to_improve_coverage(self) -> None:
        fixtures = [
            {"fixture_id": "required", "category_labels": ["performance"]},
            {"fixture_id": "covers-a-and-b", "category_labels": ["performance", "a", "b"]},
            {"fixture_id": "covers-a", "category_labels": ["performance", "a"]},
            {"fixture_id": "covers-b", "category_labels": ["performance", "b"]},
            {"fixture_id": "covers-c", "category_labels": ["performance", "c"]},
        ]
        selected = select_performance(
            fixtures,
            3,
            seed=17,
            required_fixture_ids=["required"],
            stratify_labels=["a", "b", "c"],
        )
        covered = {
            label
            for fixture in selected
            for label in fixture["category_labels"]
            if label in {"a", "b", "c"}
        }
        self.assertEqual(selected[0]["fixture_id"], "required")
        self.assertEqual(covered, {"a", "b", "c"})

    def test_corpus_summary_never_fabricates_dynamic_metrics(self) -> None:
        _, fixtures = load_corpus()
        summary = summarize_corpus(fixtures)
        self.assertEqual(summary["total_positions"], 256)
        self.assertEqual(summary["unique_positions"], 256)
        self.assertEqual(summary["duplicate_positions"], 0)
        self.assertIsNone(summary["drop_available_positions"])
        self.assertIsNone(summary["dynamic_characterization"]["tt_reuse_potential"])
        self.assertEqual(summary["lion_candidates_total"]["count"], 256)

    def test_diagnostic_corpus_characterization_is_measured_and_scoped(self) -> None:
        _, fixtures = load_corpus()
        summary = summarize_corpus(fixtures)
        records = [
            {
                "diagnostic": True,
                "version": "candidate",
                "fixture_id": "duplicate_hands",
                "stage": "fixed",
                "depth": 6,
                "budget_ms": None,
                "repetition": 0,
                "legal_result": True,
                "legal_actions": [1, 145],
                "propagation_calls": 100,
                "propagation_iterations": 140,
                "propagation_ms": 2.5,
                "tt_hit_rate": 0.25,
                "tt_probes": 80,
                "leq_calls": 2,
                "leq_duplicate_ratio": 0.20,
            },
            # A frozen opponent's missing counters must not dilute candidate evidence.
            {
                "diagnostic": True,
                "version": "baseline",
                "fixture_id": "duplicate_hands",
                "stage": "fixed",
                "depth": 6,
                "budget_ms": None,
                "repetition": 0,
                "legal_result": True,
                "propagation_ms": None,
            },
        ]
        enriched = enrich_corpus_with_diagnostics(summary, records, "candidate")
        dynamic = enriched["dynamic_characterization"]
        self.assertEqual(dynamic["diagnostic_run_count"], 1)
        self.assertEqual(dynamic["propagation_cost_distribution_ms"]["median"], 2.5)
        self.assertEqual(dynamic["leq_trigger_rate"], 1.0)
        self.assertEqual(enriched["drop_available_positions_observed_in_diagnostic_sample"], 1)


if __name__ == "__main__":
    unittest.main()
