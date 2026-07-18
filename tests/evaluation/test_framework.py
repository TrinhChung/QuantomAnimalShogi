from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from evaluation.tools.common import (
    EvaluationError,
    append_jsonl,
    atomic_write_json,
    default_build_directory,
    load_json,
    repository_root,
    sha256_file,
)
from evaluation.tools.corpus import generate_corpus, load_corpus, select_openings
from evaluation.tools.referee import (
    EngineSession,
    EngineSpec,
    NativeReferee,
    terminate_process_tree,
)
from evaluation.tools.run_pipeline import Progress, _verify_all_inputs, _verify_original_candidate
from evaluation.tools.run_selfplay import play_game, replay_game, run_paired_matches
from evaluation.tools.summarize_results import (
    classify_candidate,
    paired_bootstrap,
    reliability_summary,
    summarize_games,
)
from evaluation.tools.version_registry import VersionRegistry


ROOT = repository_root()
REFEREE = default_build_directory() / "Release" / "qas_evaluation_referee.exe"
DUMMIES = ROOT / "tests" / "evaluation" / "dummy_engines"


def dummy_spec(name: str, temporary: Path, version_id: str | None = None) -> EngineSpec:
    config = temporary / f"{name}.json"
    config.write_text("{}\n", encoding="utf-8")
    script = DUMMIES / f"{name}.py"
    return EngineSpec(
        version_id=version_id or name,
        executable=Path(sys.executable),
        config=config,
        binary_sha256=sha256_file(Path(sys.executable)),
        config_sha256=sha256_file(config),
        commit=None,
        command_prefix=(sys.executable, str(script)),
    )


@unittest.skipUnless(REFEREE.is_file(), "native evaluation referee has not been built")
class NativeBoundaryTests(unittest.TestCase):
    def test_action_codec_state_roundtrip_and_side_orientation(self) -> None:
        for action in range(240):
            source, destination = divmod(action, 12)
            self.assertEqual(source * 12 + destination, action)
        with NativeReferee(REFEREE) as referee:
            initial = referee.initial()
            self.assertEqual(initial["legal_count"], 9)
            self.assertEqual(len(initial["action_mask"]), 240)
            restored = referee.load(initial["state_text"])
            self.assertEqual(restored["state_hash"], initial["state_hash"])
            after = referee.apply(initial["legal_actions"][0])
            self.assertEqual(after["turn"], 1)
            self.assertEqual(after["side"], "north")
            self.assertEqual(len(after["protocol_request"]), len(after["protocol_request"]))

    def test_corpus_generation_is_deterministic_and_openings_are_paired(self) -> None:
        with tempfile.TemporaryDirectory() as first_name, tempfile.TemporaryDirectory() as second_name:
            first = generate_corpus(REFEREE, Path(first_name))
            second = generate_corpus(REFEREE, Path(second_name))
            self.assertEqual(first["fixtures_sha256"], second["fixtures_sha256"])
            _, fixtures = load_corpus(Path(first_name))
            openings_first = select_openings(fixtures, 5, 123)
            openings_second = select_openings(fixtures, 5, 123)
            self.assertEqual(openings_first, openings_second)
            extended = select_openings(fixtures, 10, 123)
            self.assertEqual(openings_first, extended[:5])
            game_roles = [(opening["fixture_id"], role) for opening in openings_first for role in (0, 1)]
            self.assertEqual(len(game_roles), 10)
            self.assertTrue(all(game_roles[index][0] == game_roles[index + 1][0] for index in range(0, 10, 2)))


class PersistenceAndRegistryTests(unittest.TestCase):
    def test_corpus_hash_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            copied = Path(name) / "corpus"
            shutil.copytree(ROOT / "evaluation" / "corpus", copied)
            fixture_path = copied / "corpus-v1.jsonl"
            fixture_path.write_text(
                fixture_path.read_text(encoding="utf-8") + "\n", encoding="utf-8"
            )
            with self.assertRaises(EvaluationError):
                load_corpus(copied)

    def test_atomic_progress_jsonl_and_resume(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            progress = Progress(
                directory / "progress.json",
                {
                    "completed_tasks": [],
                    "task_results": {},
                    "completed_game_ids": [],
                    "incomplete_game_ids": ["g1"],
                },
            )
            progress.mark_complete("game:g1", {"game_id": "g1"})
            progress.mark_complete("game:g1", {"game_id": "g1"})
            resumed = Progress(directory / "progress.json")
            self.assertTrue(resumed.is_complete("game:g1"))
            self.assertEqual(resumed.value["completed_game_ids"], ["g1"])
            live = load_json(directory / "live_progress.json")
            self.assertEqual(live["completed_games"], 1)
            self.assertIsNone(live["active_task"])
            log = directory / "records.jsonl"
            append_jsonl(log, {"id": 1})
            append_jsonl(log, {"id": 2})
            self.assertEqual([json.loads(line)["id"] for line in log.read_text().splitlines()], [1, 2])
            atomic_write_json(directory / "atomic.json", {"complete": True})
            self.assertTrue(load_json(directory / "atomic.json")["complete"])

    def test_atomic_write_retries_a_transient_windows_reader_lock(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            destination = Path(name) / "progress.json"
            real_replace = os.replace
            attempts = 0

            def intermittently_locked(source: Path, target: Path) -> None:
                nonlocal attempts
                attempts += 1
                if attempts < 3:
                    raise PermissionError(5, "file is temporarily locked", str(target))
                real_replace(source, target)

            with patch("evaluation.tools.common.os.replace", side_effect=intermittently_locked):
                atomic_write_json(destination, {"complete": True})

            self.assertEqual(attempts, 3)
            self.assertTrue(load_json(destination)["complete"])

    def test_candidate_hash_change_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            executable = directory / "candidate.exe"
            config = directory / "config.json"
            benchmark = directory / "benchmark.exe"
            executable.write_bytes(b"candidate")
            config.write_text("{}", encoding="utf-8")
            benchmark.write_bytes(b"benchmark")
            manifest = {
                "original_executable_path": str(executable),
                "original_config_path": str(config),
                "original_benchmark_path": str(benchmark),
                "binary_sha256": sha256_file(executable),
                "config_sha256": sha256_file(config),
                "benchmark_binary_sha256": sha256_file(benchmark),
            }
            _verify_original_candidate(manifest)
            executable.write_bytes(b"changed")
            with self.assertRaises(EvaluationError):
                _verify_original_candidate(manifest)

    def test_resume_uses_run_snapshot_when_original_candidate_is_gone(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            executable = directory / "qas.exe"
            config = directory / "engine_config.json"
            benchmark = directory / "benchmark.exe"
            executable.write_bytes(b"candidate")
            config.write_text("{}", encoding="utf-8")
            benchmark.write_bytes(b"benchmark")
            manifest = {
                "original_executable_path": str(directory / "missing-original.exe"),
                "original_config_path": str(directory / "missing-original.json"),
                "original_benchmark_path": str(directory / "missing-benchmark.exe"),
                "binary_sha256": sha256_file(executable),
                "config_sha256": sha256_file(config),
                "benchmark_binary_sha256": sha256_file(benchmark),
            }
            snapshot = {
                "executable": str(executable),
                "config": str(config),
                "benchmark_executable": str(benchmark),
            }
            _verify_all_inputs(snapshot, manifest, [], require_original_candidate=False)
            with self.assertRaises(EvaluationError):
                _verify_all_inputs(snapshot, manifest, [], require_original_candidate=True)

    def test_version_registry_immutability_hashes_selection_and_promotion_gate(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            registry_path = directory / "versions.json"
            atomic_write_json(
                registry_path,
                {
                    "schema_version": 1,
                    "stable_anchor": "v1",
                    "current_champion": "v1",
                    "versions": [],
                },
            )
            executable = directory / "source.exe"
            executable.write_bytes(b"dummy-binary")
            config = directory / "source.json"
            config.write_text("{}\n", encoding="utf-8")
            benchmark = directory / "benchmark.exe"
            benchmark.write_bytes(b"dummy-benchmark")
            registry = VersionRegistry(registry_path)
            frozen = registry.freeze(
                version_id="v1",
                display_name="Version One",
                executable=executable,
                config=config,
                benchmark_executable=benchmark,
                parent_version=None,
                change_category="performance_only",
                accepted=True,
                allow_dirty=True,
                notes="test fixture intentionally freezes while the repository is dirty",
            )
            self.assertEqual(registry.select_opponents()[0]["version_id"], "v1")
            report_directory = directory / "report"
            report_directory.mkdir()
            (report_directory / "report.md").write_text("rejected", encoding="utf-8")
            atomic_write_json(report_directory / "manifest.json", {"classification": "REJECT_CORRECTNESS"})
            with self.assertRaises(EvaluationError):
                registry.promote("v1", report_directory / "report.md")
            frozen_executable = Path(frozen["executable"])
            frozen_executable.chmod(0o666)
            frozen_executable.write_bytes(b"changed")
            with self.assertRaises(EvaluationError):
                registry.verify("v1")


@unittest.skipUnless(REFEREE.is_file(), "native evaluation referee has not been built")
class ProcessAndGameTests(unittest.TestCase):
    def _request(self, dummy: str, timeout_ms: int = 250) -> dict:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            spec = dummy_spec(dummy, directory)
            with NativeReferee(REFEREE) as referee:
                snapshot = referee.initial()
            session = EngineSession(spec, 1, directory / "out.txt", directory / "err.txt")
            try:
                return session.request_action(snapshot["protocol_request"], timeout_ms)
            finally:
                session.close()

    def test_dummy_engine_response_classifications(self) -> None:
        self.assertEqual(self._request("first_legal")["response_status"], "valid_response")
        self.assertEqual(self._request("crash")["response_status"], "process_crash")
        self.assertEqual(self._request("timeout", 30)["response_status"], "external_timeout")
        self.assertEqual(self._request("malformed")["response_status"], "malformed_output")
        self.assertEqual(self._request("extra_stdout")["response_status"], "protocol_desynchronization")

    def test_illegal_action_game_classification_and_replay(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            candidate = dummy_spec("illegal", directory, "candidate")
            opponent = dummy_spec("first_legal", directory, "opponent")
            with NativeReferee(REFEREE) as referee:
                initial = referee.initial()
            opening = {
                "fixture_id": "initial",
                "generation_seed": 1,
                "full_serialized_state": initial["state_text"],
            }
            game = play_game(
                run_id="test",
                game_id="g1",
                attempt_id=1,
                pair_id="p1",
                opening=opening,
                candidate=candidate,
                opponent=opponent,
                candidate_first=True,
                referee_executable=REFEREE,
                move_soft_ms=10,
                move_hard_ms=100,
                tt_size_mb=1,
                run_directory=directory,
            )
            self.assertEqual(game["illegal_engine"], "candidate")
            self.assertEqual(game["terminal_type"], "illegal_action_loss")
            self.assertTrue(replay_game(game, REFEREE))

    def test_complete_process_tree_termination(self) -> None:
        process = subprocess.Popen(
            [sys.executable, str(DUMMIES / "spawn_child.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            creationflags=(subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0),
            start_new_session=os.name != "nt",
        )
        assert process.stdout is not None
        child_pid = int(process.stdout.readline())
        terminate_process_tree(process)
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream is not None:
                stream.close()
        self.assertIsNotNone(process.poll())
        if os.name == "nt":
            result = subprocess.run(
                ["tasklist", "/FI", f"PID eq {child_pid}", "/FO", "CSV", "/NH"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotIn(f'"{child_pid}"', result.stdout)
        else:
            with self.assertRaises(ProcessLookupError):
                os.kill(child_pid, 0)

    def test_keyboard_interrupt_leaves_game_incomplete_for_resume(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            candidate = dummy_spec("first_legal", directory, "candidate")
            opponent = dummy_spec("first_legal", directory, "opponent")
            with NativeReferee(REFEREE) as referee:
                initial = referee.initial()
            opening = {
                "fixture_id": "initial",
                "generation_seed": 1,
                "full_serialized_state": initial["state_text"],
            }

            def interrupt(_record: dict) -> None:
                raise KeyboardInterrupt

            with self.assertRaises(KeyboardInterrupt):
                play_game(
                    run_id="interrupt",
                    game_id="g1",
                    attempt_id=1,
                    pair_id="p1",
                    opening=opening,
                    candidate=candidate,
                    opponent=opponent,
                    candidate_first=True,
                    referee_executable=REFEREE,
                    move_soft_ms=10,
                    move_hard_ms=100,
                    tt_size_mb=1,
                    run_directory=directory,
                    move_completed=interrupt,
                )
            self.assertFalse((directory / "games" / "g1" / "attempt_1" / "game.json").exists())
            resumed = play_game(
                run_id="interrupt",
                game_id="g1",
                attempt_id=2,
                pair_id="p1",
                opening=opening,
                candidate=candidate,
                opponent=opponent,
                candidate_first=True,
                referee_executable=REFEREE,
                move_soft_ms=10,
                move_hard_ms=100,
                tt_size_mb=1,
                run_directory=directory,
            )
            self.assertTrue(resumed["complete"])
            self.assertTrue(replay_game(resumed, REFEREE))
            move_path = Path(resumed["move_log_path"])
            move_rows = [json.loads(line) for line in move_path.read_text(encoding="utf-8").splitlines()]
            move_rows[0]["state_hash_after"] ^= 1
            move_path.write_text(
                "".join(json.dumps(row) + "\n" for row in move_rows), encoding="utf-8"
            )
            self.assertFalse(replay_game(resumed, REFEREE))

    def test_pair_extension_does_not_duplicate_completed_games(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            candidate = dummy_spec("first_legal", directory, "candidate")
            opponent = dummy_spec("first_legal", directory, "opponent")
            openings = [
                {"fixture_id": f"o{index}", "generation_seed": index, "full_serialized_state": "x"}
                for index in range(2)
            ]
            completed: set[str] = set()

            def fake_game(**kwargs: object) -> dict:
                game_id = str(kwargs["game_id"])
                attempt = int(kwargs["attempt_id"])
                move_directory = directory / "games" / game_id / f"attempt_{attempt}"
                move_directory.mkdir(parents=True, exist_ok=True)
                moves = move_directory / "moves.jsonl"
                moves.write_text("", encoding="utf-8")
                return {
                    "game_id": game_id,
                    "pair_id": kwargs["pair_id"],
                    "opponent_version": "opponent",
                    "candidate_result": "draw",
                    "valid_strength_game": True,
                    "terminal_type": "draw",
                    "move_log_path": str(moves),
                }

            def mark(task: str, _game: dict) -> None:
                completed.add(task)

            common = {
                "run_id": "extend",
                "candidate": candidate,
                "opponents": [opponent],
                "referee_executable": REFEREE,
                "profile": {"move_soft_ms": 10, "move_hard_ms": 100, "tt_size_mb": 1},
                "run_directory": directory,
                "attempt_id": 1,
                "is_complete": completed.__contains__,
                "task_started": lambda _task: None,
                "move_completed": lambda _move: None,
                "mark_complete": mark,
            }
            with patch("evaluation.tools.run_selfplay.play_game", side_effect=fake_game), patch(
                "evaluation.tools.run_selfplay.replay_game", return_value=True
            ):
                first = run_paired_matches(openings=openings[:1], **common)
                second = run_paired_matches(openings=openings, **common)
            self.assertEqual(len(first), 2)
            self.assertEqual(len(second), 4)
            lines = (directory / "selfplay" / "games.jsonl").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 4)
            self.assertEqual(len({json.loads(line)["game_id"] for line in lines}), 4)


class StatisticalAndPolicyTests(unittest.TestCase):
    def test_paired_bootstrap_is_reproducible(self) -> None:
        first = paired_bootstrap([0.0, 0.5, 1.0, 0.5], 10_000, 42)
        second = paired_bootstrap([0.0, 0.5, 1.0, 0.5], 10_000, 42)
        self.assertEqual(first, second)
        self.assertEqual(first["pair_count"], 4)

    def test_small_all_draw_sample_cannot_drive_acceptance(self) -> None:
        games = []
        for pair in range(8):
            for role in range(2):
                games.append(
                    {
                        "opponent_version": "champion",
                        "valid_strength_game": True,
                        "candidate_result": "draw",
                        "engine_first": "candidate" if role == 0 else "champion",
                        "engine_second": "champion" if role == 0 else "candidate",
                        "pair_id": f"p{pair}",
                        "terminal_type": "draw",
                        "total_actions": 20,
                        "illegal_engine": None,
                        "crashed_engine": None,
                        "timed_out_engine": None,
                        "protocol_error_engine": None,
                        "replay_passed": True,
                    }
                )
        summary = summarize_games(games, [], "candidate", 10_000, 7)
        self.assertEqual(summary["champion"]["candidate_score"], 0.5)
        reliability = reliability_summary(summary)
        classification, gates = classify_candidate(
            change_category="performance_only",
            profile={"strength_claim_allowed": True},
            policy=load_json(ROOT / "evaluation" / "config" / "acceptance_policy.json"),
            correctness={"passed": True},
            reliability=reliability,
            performance={
                "opponents": {
                    "champion": {
                        "geometric_mean_elapsed_ratio": 0.90,
                        "geometric_mean_node_ratio": 1.0,
                        "critical_regressions": [],
                    }
                }
            },
            game_summaries=summary,
            champion_id="champion",
            anchor_id="champion",
        )
        self.assertEqual(classification, "INCONCLUSIVE")
        self.assertFalse(gates["minimum_strength_sample"])
        self.assertFalse(gates["performance"])
        self.assertFalse(gates["timing_quality"])


if __name__ == "__main__":
    unittest.main()
