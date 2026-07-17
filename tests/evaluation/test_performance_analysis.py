from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from evaluation.tools.referee import EngineSpec
from evaluation.tools.run_performance import (
    run_fixed_depth,
    run_time_controlled,
    summarize_fixed,
    summarize_time_controlled,
)


def make_record(
    version: str,
    *,
    stage: str = "fixed",
    repetition: int = 0,
    elapsed_ms: float = 100.0,
    nodes: int = 100,
    depth: int = 6,
    budget_ms: int | None = None,
    action: int = 1,
    score: int = 10,
    pv: list[int] | None = None,
    diagnostic: bool = False,
    timeout: bool = False,
    fallback: bool = False,
    overshoot_ms: float = 0.0,
) -> dict[str, object]:
    return {
        "stage": stage,
        "version": version,
        "opponent": "baseline",
        "fixture_id": "fixture-a",
        "depth": depth,
        "budget_ms": budget_ms,
        "repetition": repetition,
        "diagnostic": diagnostic,
        "warmup": False,
        "best_action": action,
        "score": score,
        "completed": True,
        "search_completed": not timeout,
        "legal_result": True,
        "completed_depth": depth,
        "started_depth": depth,
        "timeout": timeout,
        "fallback_used": fallback,
        "elapsed_wall_ms": elapsed_ms,
        "nodes": nodes,
        "nps": nodes * 1000.0 / elapsed_ms,
        "pv_actions": pv if pv is not None else [action, action + 1],
        "deadline_overshoot_ms": overshoot_ms,
        "peak_memory_bytes": 1024,
        "minimum_sample_duration_ms": 50.0,
        "minimum_authoritative_repetitions": 2,
    }


def engine(version_id: str) -> EngineSpec:
    placeholder = Path("unused")
    return EngineSpec(version_id, placeholder, placeholder, "a", "b", None, placeholder)


def successful_driver(*_args: object, **_kwargs: object) -> dict[str, object]:
    return {
        "best_action": 1,
        "score": 10,
        "completed": True,
        "search_completed": True,
        "legal_result": True,
        "legal_actions": [1],
        "state_hash_before": 1,
        "state_hash_after": 2,
        "completed_depth": 6,
        "started_depth": 6,
        "elapsed_wall_ms": 100.0,
        "nodes": 100,
        "nps": 1000.0,
        "pv_actions": [1],
        "peak_memory_bytes": 1024,
    }


class PerformanceSummaryTests(unittest.TestCase):
    def test_clearly_faster_and_slower_node_cost_are_distinguished(self) -> None:
        faster: list[dict[str, object]] = []
        slower: list[dict[str, object]] = []
        for repetition in range(3):
            faster.extend(
                [
                    make_record("candidate", repetition=repetition, elapsed_ms=80, nodes=100),
                    make_record("baseline", repetition=repetition, elapsed_ms=100, nodes=100),
                ]
            )
            slower.extend(
                [
                    make_record("candidate", repetition=repetition, elapsed_ms=120, nodes=100),
                    make_record("baseline", repetition=repetition, elapsed_ms=100, nodes=100),
                ]
            )
        faster_summary = summarize_fixed(faster, "candidate", ["baseline"])["opponents"]["baseline"]
        slower_summary = summarize_fixed(slower, "candidate", ["baseline"])["opponents"]["baseline"]
        self.assertAlmostEqual(faster_summary["geometric_mean_elapsed_ratio"], 0.8)
        self.assertAlmostEqual(faster_summary["geometric_mean_node_ratio"], 1.0)
        self.assertAlmostEqual(faster_summary["geometric_mean_node_cost_ratio"], 0.8)
        self.assertAlmostEqual(slower_summary["geometric_mean_elapsed_ratio"], 1.2)
        self.assertAlmostEqual(slower_summary["geometric_mean_node_cost_ratio"], 1.2)

    def test_fewer_nodes_but_slower_total_time_is_not_called_cheaper(self) -> None:
        records: list[dict[str, object]] = []
        for repetition in range(3):
            records.extend(
                [
                    make_record("candidate", repetition=repetition, elapsed_ms=120, nodes=50),
                    make_record("baseline", repetition=repetition, elapsed_ms=100, nodes=100),
                ]
            )
        summary = summarize_fixed(records, "candidate", ["baseline"])["opponents"]["baseline"]
        self.assertAlmostEqual(summary["geometric_mean_node_ratio"], 0.5)
        self.assertAlmostEqual(summary["geometric_mean_elapsed_ratio"], 1.2)
        self.assertAlmostEqual(summary["geometric_mean_node_cost_ratio"], 2.4)
        self.assertTrue(summary["node_cost_regressions"])

    def test_one_repetition_has_no_cv_or_stability_claim(self) -> None:
        records = [make_record("candidate"), make_record("baseline")]
        summary = summarize_fixed(records, "candidate", ["baseline"])
        candidate = next(row for row in summary["groups"] if row["version"] == "candidate")
        self.assertIsNone(candidate["coefficient_of_variation"])
        self.assertIsNone(candidate["node_coefficient_of_variation"])
        self.assertIsNone(candidate["node_count_stable"])
        self.assertIsNone(candidate["best_action_stable"])
        self.assertFalse(candidate["timing_authoritative"])
        self.assertIsNone(summary["opponents"]["baseline"]["geometric_mean_elapsed_ratio"])

    def test_fixed_summary_separates_elapsed_nodes_and_node_cost(self) -> None:
        records: list[dict[str, object]] = []
        for repetition in range(2):
            records.append(make_record("candidate", repetition=repetition, nodes=50))
            records.append(make_record("baseline", repetition=repetition, nodes=100))
        summary = summarize_fixed(records, "candidate", ["baseline"])
        comparison = summary["opponents"]["baseline"]
        self.assertAlmostEqual(comparison["geometric_mean_elapsed_ratio"], 1.0)
        self.assertAlmostEqual(comparison["geometric_mean_node_ratio"], 0.5)
        self.assertAlmostEqual(comparison["geometric_mean_node_cost_ratio"], 2.0)
        self.assertEqual(len(comparison["node_cost_regressions"]), 1)
        self.assertEqual(comparison["authoritative_timing_comparison_count"], 1)

    def test_time_summary_reports_depth_stability_and_overshoot(self) -> None:
        records: list[dict[str, object]] = []
        for repetition in range(2):
            records.append(
                make_record(
                    "candidate",
                    stage="time",
                    repetition=repetition,
                    depth=8,
                    budget_ms=250,
                    action=4,
                    pv=[4, 5],
                    overshoot_ms=2.0,
                )
            )
            records.append(
                make_record(
                    "baseline",
                    stage="time",
                    repetition=repetition,
                    depth=7,
                    budget_ms=250,
                    action=3,
                    pv=[3, 2],
                    overshoot_ms=5.0,
                )
            )
        summary = summarize_time_controlled(records, "candidate", ["baseline"])
        opponent = summary["opponents"]["baseline"]
        self.assertEqual(opponent["median_completed_depth_delta"], 1.0)
        self.assertEqual(opponent["candidate_deeper_count"], 1)
        self.assertEqual(opponent["candidate_best_action_stability_rate"], 1.0)
        self.assertEqual(opponent["candidate_pv_stability_rate"], 1.0)
        self.assertEqual(opponent["median_deadline_overshoot_delta_ms"], -3.0)

    def test_missing_diagnostic_telemetry_remains_unavailable(self) -> None:
        records = [
            make_record("candidate", repetition=0),
            make_record("candidate", repetition=1),
            make_record("baseline", repetition=0),
            make_record("baseline", repetition=1),
            make_record("candidate", diagnostic=True),
            make_record("baseline", diagnostic=True),
        ]
        summary = summarize_fixed(records, "candidate", ["baseline"])
        telemetry = summary["diagnostics"]["telemetry_availability"]
        self.assertEqual(telemetry["propagation_ms"]["available_runs"], 0)
        self.assertIsNone(
            summary["diagnostics"]["instrumentation_overhead"][
                "geometric_mean_elapsed_ratio"
            ]
        )


class PerformanceScheduleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.candidate = engine("candidate")
        self.baseline = engine("baseline")
        self.fixture = {"fixture_id": "fixture-a", "full_serialized_state": "state"}

    def callbacks(self) -> tuple[list[str], dict[str, dict[str, object]], tuple[object, ...]]:
        started: list[str] = []
        completed: dict[str, dict[str, object]] = {}

        def is_complete(_task: str) -> bool:
            return False

        def cached_result(_task: str) -> dict[str, object]:
            raise AssertionError("unexpected cache read")

        def task_started(task: str) -> None:
            started.append(task)

        def mark_complete(task: str, record: dict[str, object]) -> None:
            completed[task] = record

        return started, completed, (is_complete, cached_result, task_started, mark_complete)

    def test_fixed_schedule_warms_up_reverses_ab_and_honors_diagnostics(self) -> None:
        profile = {
            "fixed_depths": [6],
            "fixed_hard_timeout_ms": 1000,
            "fixed_warmup_repetitions": 1,
            "fixed_repetitions": 3,
            "diagnostic_repetitions": 2,
            "tt_size_mb": 16,
        }
        started, _completed, callbacks = self.callbacks()
        with tempfile.TemporaryDirectory() as directory, patch(
            "evaluation.tools.run_performance._run_driver", side_effect=successful_driver
        ):
            records = run_fixed_depth(
                self.candidate,
                [self.baseline],
                [self.fixture],
                profile,
                Path(directory),
                *callbacks,
            )
        self.assertEqual(len(started), 12)
        self.assertEqual(len(records), 10)
        measured = [row for row in records if not row["diagnostic"]]
        self.assertEqual(
            [row["version"] for row in measured],
            ["candidate", "baseline", "baseline", "candidate", "candidate", "baseline"],
        )
        self.assertEqual([row["execution_order"] for row in records[:2]], [2, 3])

    def test_time_schedule_honors_all_repetition_controls(self) -> None:
        driver_calls: list[dict[str, object]] = []

        def recording_driver(*_args: object, **kwargs: object) -> dict[str, object]:
            driver_calls.append(kwargs)
            return successful_driver()

        profile = {
            "time_budgets_ms": [250],
            "time_hard_grace_ms": 20,
            "time_hard_multiplier": 1.05,
            "max_depth": 16,
            "time_warmup_repetitions": 1,
            "time_repetitions": 2,
            "time_diagnostic_repetitions": 2,
            "tt_size_mb": 16,
        }
        started, _completed, callbacks = self.callbacks()
        with tempfile.TemporaryDirectory() as directory, patch(
            "evaluation.tools.run_performance._run_driver", side_effect=recording_driver
        ):
            records = run_time_controlled(
                self.candidate,
                [self.baseline],
                [self.fixture],
                profile,
                Path(directory),
                *callbacks,
            )
        self.assertEqual(len(started), 10)
        self.assertEqual(len(records), 8)
        measured = [row for row in records if not row["diagnostic"]]
        self.assertEqual(
            [row["version"] for row in measured],
            ["candidate", "baseline", "baseline", "candidate"],
        )
        self.assertEqual({call["soft_ms"] for call in driver_calls}, {250})
        self.assertEqual({call["hard_ms"] for call in driver_calls}, {270})


if __name__ == "__main__":
    unittest.main()
