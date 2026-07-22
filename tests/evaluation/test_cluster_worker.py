from __future__ import annotations

import argparse
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from evaluation.cluster_worker import ClusterWorker
from evaluation.cluster_worker_support import (
    BuildArtifacts,
    GitWorkspace,
    WorkerError,
    _kokoro_busy,
    _labels,
)


class ClusterWorkerTests(unittest.TestCase):
    def test_labels_are_explicit_key_value_pairs(self) -> None:
        self.assertEqual(
            _labels("os=windows,role=daytime-slave"),
            {"os": "windows", "role": "daytime-slave"},
        )
        with self.assertRaisesRegex(WorkerError, "invalid worker label"):
            _labels("shared_with_kokoro")

    @mock.patch("evaluation.cluster_worker_support.shutil.which", return_value="docker")
    @mock.patch("evaluation.cluster_worker_support.subprocess.run")
    def test_kokoro_cpu_threshold_blocks_new_work(self, run: mock.Mock, _: mock.Mock) -> None:
        run.return_value = argparse.Namespace(returncode=0, stdout="17.25%\n")
        self.assertTrue(_kokoro_busy("kokoro", 5.0))
        self.assertFalse(_kokoro_busy("kokoro", 20.0))

    def test_workspace_rejects_non_commit_job_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workspace = GitWorkspace(Path(directory) / "worker", "https://example.invalid/repo.git")
            with self.assertRaisesRegex(WorkerError, "git commit is invalid"):
                workspace.prepare("main")

    def test_benchmark_generates_fixtures_and_returns_csv(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worker = object.__new__(ClusterWorker)
            worker.workspace = argparse.Namespace(path=root)
            executable = root / "qas_stage35_benchmark"
            executable.write_text("binary", encoding="utf-8")

            def run_process(_: dict, command: list[str], __: Path) -> Path:
                self.assertIn("--generate-fixtures", command)
                output = Path(command[command.index("--output") + 1])
                output.parent.mkdir(parents=True)
                output.write_text("suite,elapsed_ms\nsmoke,1\n", encoding="utf-8")
                log = output.parent / "worker.log"
                log.write_text("benchmark complete\n", encoding="utf-8")
                return log

            worker._run_process = run_process
            artifacts = BuildArtifacts(executable, executable, executable, executable)
            result = worker._run_benchmark(
                {"id": "job-one", "payload": {"profile": "infrastructure_smoke"}},
                artifacts,
            )
            self.assertIn("smoke,1", result["benchmarkCsv"])
            self.assertEqual(len(result["csvSha256"]), 64)

    @mock.patch("evaluation.cluster_worker_support._run_checked")
    def test_workspace_retries_an_empty_failed_clone(self, run_checked: mock.Mock) -> None:
        commit = "a" * 40
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "worker"
            path.mkdir()

            def run(command: list[str], _: Path) -> str:
                if command[:2] == ["git", "clone"]:
                    (path / ".git").mkdir(parents=True)
                return ""

            run_checked.side_effect = run
            GitWorkspace(path, "git@example.invalid:repo.git").prepare(commit)
            self.assertEqual(run_checked.call_args_list[0].args[0][:2], ["git", "clone"])
            self.assertNotIn("--no-checkout", run_checked.call_args_list[0].args[0])


if __name__ == "__main__":
    unittest.main()
