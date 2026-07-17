from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path

from evaluation.tools.common import EvaluationError, atomic_write_json, load_json, sha256_file
from evaluation.tools.version_registry import VersionRegistry


class PromotionBindingTests(unittest.TestCase):
    def _add_frozen_version(
        self,
        root: Path,
        registry_data: dict,
        version_id: str,
        binary_contents: bytes,
        config_contents: str,
    ) -> dict:
        directory = root / version_id
        directory.mkdir()
        executable = directory / "qas.exe"
        config = directory / "engine_config.json"
        executable.write_bytes(binary_contents)
        config.write_text(config_contents, encoding="utf-8")
        (directory / "source_commit.txt").write_text("test-commit\n", encoding="utf-8")
        manifest = {
            "schema_version": 1,
            "version_id": version_id,
            "binary_sha256": sha256_file(executable),
            "config_sha256": sha256_file(config),
        }
        manifest_path = directory / "manifest.json"
        atomic_write_json(manifest_path, manifest)
        registry_data["versions"].append(
            {
                "version_id": version_id,
                "display_name": version_id,
                "accepted": version_id == "anchor",
                "binary_sha256": manifest["binary_sha256"],
                "config_sha256": manifest["config_sha256"],
                "manifest_sha256": sha256_file(manifest_path),
            }
        )
        return manifest

    def _make_registry(self, directory: Path) -> tuple[VersionRegistry, dict]:
        registry_data = {
            "schema_version": 1,
            "stable_anchor": "anchor",
            "current_champion": "anchor",
            "versions": [],
        }
        self._add_frozen_version(directory, registry_data, "anchor", b"anchor", "{}\n")
        candidate = self._add_frozen_version(
            directory, registry_data, "candidate", b"candidate", '{"search": true}\n'
        )
        registry_path = directory / "versions.json"
        atomic_write_json(registry_path, registry_data)
        return VersionRegistry(registry_path), candidate

    def _make_report(
        self,
        directory: Path,
        candidate: dict,
        *,
        classification: str = "ACCEPT_PERFORMANCE_UPGRADE",
        suffix: str = "valid",
    ) -> tuple[Path, dict, dict]:
        report_directory = directory / f"report-{suffix}"
        report_directory.mkdir()
        report_path = report_directory / "report.md"
        report_path.write_text("# Test report\n", encoding="utf-8")
        run_id = f"run-{suffix}"
        report_manifest = {
            "schema_version": 1,
            "run_id": run_id,
            "complete": True,
            "classification": classification,
            "candidate_version_id": "candidate",
            "candidate_binary_sha256": candidate["binary_sha256"],
            "candidate_config_sha256": candidate["config_sha256"],
        }
        run_metadata = {
            "schema_version": 1,
            "run_id": run_id,
            "candidate_version_id": "candidate",
            "candidate_binary_sha256": candidate["binary_sha256"],
            "candidate_config_sha256": candidate["config_sha256"],
        }
        atomic_write_json(report_directory / "manifest.json", report_manifest)
        atomic_write_json(report_directory / "run_config.json", run_metadata)
        return report_path, report_manifest, run_metadata

    def _rewrite_report_metadata(
        self, report_path: Path, report_manifest: dict, run_metadata: dict
    ) -> None:
        atomic_write_json(report_path.parent / "manifest.json", report_manifest)
        atomic_write_json(report_path.parent / "run_config.json", run_metadata)

    def test_exact_accept_report_promotes_and_logs_artifact_binding(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            registry, candidate = self._make_registry(directory)
            report_path, _, _ = self._make_report(directory, candidate)

            registry.promote("candidate", report_path)

            registry_data = registry.load()
            self.assertEqual(registry_data["current_champion"], "candidate")
            entry = next(item for item in registry_data["versions"] if item["version_id"] == "candidate")
            promotion = entry["promotions"][-1]
            self.assertTrue(entry["accepted"])
            self.assertEqual(promotion["run_id"], "run-valid")
            self.assertEqual(promotion["candidate_version_id"], "candidate")
            self.assertEqual(promotion["binary_sha256"], candidate["binary_sha256"])
            self.assertEqual(promotion["config_sha256"], candidate["config_sha256"])
            self.assertEqual(
                promotion["report_manifest_sha256"], sha256_file(report_path.parent / "manifest.json")
            )
            self.assertEqual(
                promotion["run_config_sha256"], sha256_file(report_path.parent / "run_config.json")
            )

    def test_accept_report_rejects_each_mismatched_candidate_identity_field(self) -> None:
        cases = (
            ("report version", "manifest", "candidate_version_id", "different-version"),
            ("report binary", "manifest", "candidate_binary_sha256", "0" * 64),
            ("report config", "manifest", "candidate_config_sha256", "3" * 64),
            ("run version", "run", "candidate_version_id", "different-version"),
            ("run binary", "run", "candidate_binary_sha256", "1" * 64),
            ("run config", "run", "candidate_config_sha256", "2" * 64),
            ("run id", "run", "run_id", "different-run"),
        )
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            registry, candidate = self._make_registry(directory)
            for index, (label, owner, field, value) in enumerate(cases):
                with self.subTest(label=label):
                    report_path, report_manifest, run_metadata = self._make_report(
                        directory, candidate, suffix=f"mismatch-{index}"
                    )
                    target = report_manifest if owner == "manifest" else run_metadata
                    target[field] = value
                    self._rewrite_report_metadata(report_path, report_manifest, run_metadata)
                    with self.assertRaises(EvaluationError):
                        registry.promote("candidate", report_path)
                    self.assertEqual(registry.load()["current_champion"], "anchor")

    def test_old_or_incomplete_accept_report_cannot_promote(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            registry, candidate = self._make_registry(directory)
            report_path, report_manifest, run_metadata = self._make_report(
                directory, candidate, suffix="missing"
            )

            for owner, field in (
                ("manifest", "run_id"),
                ("manifest", "candidate_binary_sha256"),
                ("run", "candidate_version_id"),
                ("run", "candidate_binary_sha256"),
                ("run", "candidate_config_sha256"),
            ):
                with self.subTest(owner=owner, field=field):
                    changed_manifest = copy.deepcopy(report_manifest)
                    changed_run = copy.deepcopy(run_metadata)
                    target = changed_manifest if owner == "manifest" else changed_run
                    del target[field]
                    self._rewrite_report_metadata(report_path, changed_manifest, changed_run)
                    with self.assertRaises(EvaluationError):
                        registry.promote("candidate", report_path)

            incomplete_manifest = copy.deepcopy(report_manifest)
            incomplete_manifest["complete"] = False
            self._rewrite_report_metadata(report_path, incomplete_manifest, run_metadata)
            with self.assertRaises(EvaluationError):
                registry.promote("candidate", report_path)

            self._rewrite_report_metadata(report_path, report_manifest, run_metadata)
            (report_path.parent / "run_config.json").unlink()
            with self.assertRaises(EvaluationError):
                registry.promote("candidate", report_path)
            self.assertEqual(registry.load()["current_champion"], "anchor")

    def test_override_bypasses_classification_only_and_is_logged(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            registry, candidate = self._make_registry(directory)
            report_path, report_manifest, run_metadata = self._make_report(
                directory,
                candidate,
                classification="REJECT_STRENGTH",
                suffix="override",
            )
            with self.assertRaises(EvaluationError):
                registry.promote("candidate", report_path)
            with self.assertRaises(EvaluationError):
                registry.promote("candidate", report_path, "   ")

            mismatched_run = copy.deepcopy(run_metadata)
            mismatched_run["candidate_config_sha256"] = "f" * 64
            self._rewrite_report_metadata(report_path, report_manifest, mismatched_run)
            with self.assertRaises(EvaluationError):
                registry.promote("candidate", report_path, "manual rule audit approved")

            self._rewrite_report_metadata(report_path, report_manifest, run_metadata)
            registry.promote("candidate", report_path, "manual rule audit approved")
            entry = next(
                item for item in registry.load()["versions"] if item["version_id"] == "candidate"
            )
            self.assertEqual(entry["promotions"][-1]["override_reason"], "manual rule audit approved")


if __name__ == "__main__":
    unittest.main()
