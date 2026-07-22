from __future__ import annotations

import argparse
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from evaluation.cluster_worker_support import GitWorkspace, WorkerError, _kokoro_busy, _labels


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


if __name__ == "__main__":
    unittest.main()
