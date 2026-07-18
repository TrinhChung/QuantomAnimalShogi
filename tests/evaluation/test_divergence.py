from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from evaluation.tools.analyze_divergence import _reanalysis_classification, analyze_divergences
from evaluation.tools.common import default_build_directory, repository_root, sha256_file
from evaluation.tools.corpus import load_corpus
from evaluation.tools.referee import EngineSpec


ROOT = repository_root()
REFEREE = default_build_directory() / "Release" / "qas_evaluation_referee.exe"
BENCHMARK = default_build_directory() / "Release" / "qas_evaluation_benchmark.exe"
CONFIG = ROOT / "engine_config.json"


@unittest.skipUnless(REFEREE.is_file() and BENCHMARK.is_file(), "native helpers are not built")
class DivergenceTests(unittest.TestCase):
    def test_matching_fixed_and_tt_off_reruns_classify_history_effect(self) -> None:
        found = {"candidate_action": 3, "opponent_action": 4}
        matching = {"best_action": 7, "score": 10, "completed_depth": 6}
        reanalysis = {
            "candidate_fixed": matching,
            "opponent_fixed": dict(matching),
            "candidate_tt_off": matching,
            "opponent_tt_off": dict(matching),
            "candidate_time": matching,
            "opponent_time": dict(matching),
        }
        self.assertEqual(
            _reanalysis_classification(found, reanalysis, "architecture_change"),
            "tt_history_effect",
        )

    def test_synthetic_fixed_divergence_is_saved_and_reproducible(self) -> None:
        _, fixtures = load_corpus()
        fixture = next(item for item in fixtures if item["fixture_id"] == "initial")
        candidate = EngineSpec(
            "candidate",
            BENCHMARK,
            CONFIG,
            sha256_file(BENCHMARK),
            sha256_file(CONFIG),
            None,
            BENCHMARK,
        )
        opponent = EngineSpec(
            "opponent",
            BENCHMARK,
            CONFIG,
            sha256_file(BENCHMARK),
            sha256_file(CONFIG),
            None,
            BENCHMARK,
        )
        records = [
            {
                "opponent": "opponent",
                "version": "candidate",
                "fixture_id": "initial",
                "depth": 4,
                "repetition": 0,
                "diagnostic": False,
                "best_action": 1,
                "score": 10,
                "completed_depth": 4,
            },
            {
                "opponent": "opponent",
                "version": "opponent",
                "fixture_id": "initial",
                "depth": 4,
                "repetition": 0,
                "diagnostic": False,
                "best_action": 2,
                "score": 10,
                "completed_depth": 4,
            },
        ]
        with tempfile.TemporaryDirectory() as name:
            output = Path(name)
            divergences = analyze_divergences(
                fixed_records=records,
                fixtures=[fixture],
                candidate=candidate,
                opponents=[opponent],
                change_category="move_ordering",
                referee_executable=REFEREE,
                output_directory=output,
                profile={"divergence_reanalysis": {"enabled": False}, "tt_size_mb": 16},
            )
            self.assertEqual(len(divergences), 1)
            self.assertEqual(divergences[0]["classification"], "equal_score_tiebreak")
            directory = output / "opponent_initial"
            self.assertTrue((directory / "comparison.json").is_file())
            self.assertTrue((directory / "state.txt").is_file())
            self.assertTrue((directory / "reproduce.bat").is_file())


if __name__ == "__main__":
    unittest.main()
