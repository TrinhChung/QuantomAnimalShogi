from __future__ import annotations

import json
import platform
import shutil
from pathlib import Path
from typing import Any

from .common import (
    EvaluationError,
    atomic_write_json,
    build_directory_for_executable,
    compiler_version,
    compiler_flags as detected_compiler_flags,
    copy_read_only,
    cpu_model,
    git_metadata,
    load_json,
    locate_companion,
    repository_root,
    sha256_file,
    utc_now,
    validate_change_category,
    validate_version_id,
)


_ACCEPTED_CLASSIFICATIONS = {
    "ACCEPT_STRENGTH_UPGRADE",
    "ACCEPT_PERFORMANCE_UPGRADE",
    "ACCEPT_EQUIVALENT_REPLACEMENT",
}


def _require_promotion_value(
    metadata: dict[str, Any], *, field: str, expected: str, source: Path
) -> None:
    actual = metadata.get(field)
    if actual is None:
        raise EvaluationError(f"promotion metadata is missing {field!r}: {source}")
    if actual != expected:
        raise EvaluationError(
            f"promotion metadata {field!r} does not match the frozen version: "
            f"{source} has {actual!r}, expected {expected!r}"
        )


def _load_promotion_metadata(path: Path, description: str) -> dict[str, Any]:
    if not path.is_file():
        raise EvaluationError(f"{description} is required: {path}")
    metadata = load_json(path)
    if not isinstance(metadata, dict):
        raise EvaluationError(f"{description} must contain a JSON object: {path}")
    return metadata


def _load_completed_promotion_run(
    report_path: Path,
) -> tuple[Path, Path, dict[str, Any], dict[str, Any]]:
    if not report_path.is_file():
        raise EvaluationError(f"completed evaluation report is required: {report_path}")
    report_metadata_path = report_path.parent / "manifest.json"
    run_metadata_path = report_path.parent / "run_config.json"
    report_metadata = _load_promotion_metadata(
        report_metadata_path, "completed evaluation manifest"
    )
    run_metadata = _load_promotion_metadata(run_metadata_path, "evaluation run metadata")
    if report_metadata.get("complete") is not True:
        raise EvaluationError("promotion refused because the evaluation run is not complete")
    return report_metadata_path, run_metadata_path, report_metadata, run_metadata


def _validated_promotion_decision(
    report_metadata: dict[str, Any], override_reason: str | None
) -> tuple[Any, str | None]:
    classification = report_metadata.get("classification")
    normalized_override = (override_reason.strip() or None) if override_reason is not None else None
    if classification not in _ACCEPTED_CLASSIFICATIONS and not normalized_override:
        raise EvaluationError(f"promotion refused for classification {classification!r}")
    return classification, normalized_override


def _validate_promotion_artifact_binding(
    *,
    version_id: str,
    version: dict[str, Any],
    report_metadata_path: Path,
    run_metadata_path: Path,
    report_metadata: dict[str, Any],
    run_metadata: dict[str, Any],
) -> dict[str, str]:
    report_run_id = report_metadata.get("run_id")
    if not isinstance(report_run_id, str) or not report_run_id:
        raise EvaluationError(
            f"promotion metadata is missing a valid 'run_id': {report_metadata_path}"
        )
    _require_promotion_value(
        run_metadata, field="run_id", expected=report_run_id, source=run_metadata_path
    )
    expected_binary_hash = version["manifest"]["binary_sha256"]
    expected_config_hash = version["manifest"]["config_sha256"]
    _require_promotion_value(
        report_metadata,
        field="candidate_binary_sha256",
        expected=expected_binary_hash,
        source=report_metadata_path,
    )
    for optional_field, expected in (
        ("candidate_version_id", version_id),
        ("candidate_config_sha256", expected_config_hash),
    ):
        if optional_field in report_metadata:
            _require_promotion_value(
                report_metadata,
                field=optional_field,
                expected=expected,
                source=report_metadata_path,
            )
    for field, expected in (
        ("candidate_version_id", version_id),
        ("candidate_binary_sha256", expected_binary_hash),
        ("candidate_config_sha256", expected_config_hash),
    ):
        _require_promotion_value(
            run_metadata, field=field, expected=expected, source=run_metadata_path
        )
    return {
        "run_id": report_run_id,
        "binary_sha256": expected_binary_hash,
        "config_sha256": expected_config_hash,
    }


class VersionRegistry:
    def __init__(self, registry_path: Path | None = None) -> None:
        self.path = registry_path or repository_root() / "evaluation" / "versions" / "versions.json"
        self.root = self.path.parent

    def load(self) -> dict[str, Any]:
        registry = load_json(self.path)
        if registry.get("schema_version") != 1 or not isinstance(registry.get("versions"), list):
            raise EvaluationError(f"unsupported or malformed version registry: {self.path}")
        identifiers = [version.get("version_id") for version in registry["versions"]]
        if len(identifiers) != len(set(identifiers)):
            raise EvaluationError("version registry contains duplicate version IDs")
        return registry

    def find(self, version_id: str) -> dict[str, Any]:
        for version in self.load()["versions"]:
            if version.get("version_id") == version_id:
                return version
        raise EvaluationError(f"version is not registered: {version_id}")

    def verify(self, version_id: str) -> dict[str, Any]:
        version = self.find(version_id)
        directory = self.root / version_id
        executable = directory / "qas.exe"
        config = directory / "engine_config.json"
        manifest_path = directory / "manifest.json"
        for path in (executable, config, manifest_path, directory / "source_commit.txt"):
            if not path.is_file():
                raise EvaluationError(f"frozen artifact is missing: {path}")
        manifest = load_json(manifest_path)
        if manifest.get("version_id") != version_id:
            raise EvaluationError(f"manifest version ID mismatch: {manifest_path}")
        if not version.get("manifest_sha256") or sha256_file(manifest_path) != version["manifest_sha256"]:
            raise EvaluationError(f"frozen manifest hash mismatch: {manifest_path}")
        if (
            version.get("binary_sha256") != manifest.get("binary_sha256")
            or version.get("config_sha256") != manifest.get("config_sha256")
        ):
            raise EvaluationError(f"registry and frozen manifest disagree: {version_id}")
        expected_executable_hash = manifest.get("binary_sha256")
        expected_config_hash = manifest.get("config_sha256")
        if sha256_file(executable) != expected_executable_hash:
            raise EvaluationError(f"frozen binary hash mismatch: {executable}")
        if sha256_file(config) != expected_config_hash:
            raise EvaluationError(f"frozen config hash mismatch: {config}")
        benchmark = directory / "benchmark.exe"
        if manifest.get("benchmark_binary_sha256"):
            if not benchmark.is_file() or sha256_file(benchmark) != manifest["benchmark_binary_sha256"]:
                raise EvaluationError(f"frozen benchmark companion hash mismatch: {benchmark}")
        return {
            **version,
            "directory": str(directory.resolve()),
            "executable": str(executable.resolve()),
            "config": str(config.resolve()),
            "benchmark_executable": str(benchmark.resolve()) if benchmark.is_file() else None,
            "manifest": manifest,
        }

    def select_opponents(
        self, explicit: list[str] | None = None, include_previous: bool = False
    ) -> list[dict[str, Any]]:
        registry = self.load()
        identifiers: list[str] = []
        if explicit:
            identifiers.extend(explicit)
        else:
            identifiers.append(registry["current_champion"])
            if registry["stable_anchor"] != registry["current_champion"]:
                identifiers.append(registry["stable_anchor"])
            if include_previous:
                accepted = [v for v in registry["versions"] if v.get("accepted")]
                if accepted:
                    identifiers.append(accepted[-1]["version_id"])
        unique = list(dict.fromkeys(identifiers))
        if not unique:
            raise EvaluationError("opponent selection produced no versions")
        return [self.verify(identifier) for identifier in unique]

    def freeze(
        self,
        *,
        version_id: str,
        display_name: str,
        executable: Path,
        config: Path,
        parent_version: str | None,
        change_category: str,
        benchmark_executable: Path | None = None,
        accepted: bool = False,
        notes: str = "",
        allow_dirty: bool = False,
        build_command: str = "user-provided Release build",
        compiler_flags: str = "auto",
    ) -> dict[str, Any]:
        validate_version_id(version_id)
        validate_change_category(change_category)
        executable = executable.resolve()
        config = config.resolve()
        if not executable.is_file() or executable.stat().st_size == 0:
            raise EvaluationError(f"candidate executable is missing or empty: {executable}")
        if not config.is_file():
            raise EvaluationError(f"candidate config is missing: {config}")
        json.loads(config.read_text(encoding="utf-8"))
        registry = self.load()
        if accepted and (
            registry["versions"] or version_id != registry.get("stable_anchor")
        ):
            raise EvaluationError(
                "--accepted is reserved for one-time empty-registry stable-anchor bootstrap; use promotion"
            )
        if any(version.get("version_id") == version_id for version in registry["versions"]):
            raise EvaluationError(f"refusing to overwrite registered version: {version_id}")
        destination = self.root / version_id
        if destination.exists():
            raise EvaluationError(f"refusing to overwrite frozen directory: {destination}")
        metadata = git_metadata(repository_root())
        if metadata["dirty"] and not allow_dirty:
            raise EvaluationError("worktree is dirty; pass --allow-dirty with a permanent reason if intentional")
        if metadata["dirty"] and allow_dirty and not notes.strip():
            raise EvaluationError("--allow-dirty requires a non-empty permanent --notes reason")
        if parent_version is not None:
            self.find(parent_version)
        benchmark_executable = benchmark_executable or locate_companion(
            executable, "qas_evaluation_benchmark"
        )
        build_directory = build_directory_for_executable(executable)
        if compiler_flags == "auto":
            compiler_flags = detected_compiler_flags("Release", build_directory)
        destination.mkdir(parents=True)
        try:
            copy_read_only(executable, destination / "qas.exe")
            copy_read_only(config, destination / "engine_config.json")
            benchmark_hash = None
            if benchmark_executable is not None:
                benchmark_executable = benchmark_executable.resolve()
                copy_read_only(benchmark_executable, destination / "benchmark.exe")
                benchmark_hash = sha256_file(destination / "benchmark.exe")
            manifest = {
                "schema_version": 1,
                "version_id": version_id,
                "display_name": display_name,
                "commit": metadata["commit"],
                "branch": metadata["branch"],
                "dirty": metadata["dirty"],
                "dirty_paths": metadata["status"],
                "source_tree_path": str(repository_root()),
                "compiler": compiler_version(build_directory),
                "compiler_flags": compiler_flags,
                "build_type": "Release",
                "build_command": build_command,
                "binary_path": "qas.exe",
                "binary_sha256": sha256_file(destination / "qas.exe"),
                "config_path": "engine_config.json",
                "config_sha256": sha256_file(destination / "engine_config.json"),
                "benchmark_binary_path": "benchmark.exe" if benchmark_hash else None,
                "benchmark_binary_sha256": benchmark_hash,
                "build_timestamp_utc": utc_now(),
                "operating_system": platform.platform(),
                "cpu_model": cpu_model(),
                "enabled_engine_features": load_json(destination / "engine_config.json"),
                "parent_version": parent_version,
                "change_category": change_category,
                "experiment_description": notes,
                "accepted": accepted,
            }
            atomic_write_json(destination / "manifest.json", manifest)
            (destination / "manifest.json").chmod(0o444)
            (destination / "source_commit.txt").write_text(
                (metadata["commit"] or "unknown") + "\n", encoding="utf-8"
            )
            (destination / "source_commit.txt").chmod(0o444)
            (destination / "README.md").write_text(
                f"# {display_name}\n\nImmutable evaluation artifact. Do not modify or rebuild in place.\n",
                encoding="utf-8",
            )
            (destination / "README.md").chmod(0o444)
            entry = {
                "version_id": version_id,
                "display_name": display_name,
                "commit": metadata["commit"],
                "branch": metadata["branch"],
                "dirty": metadata["dirty"],
                "created_at_utc": manifest["build_timestamp_utc"],
                "binary_path": f"{version_id}/qas.exe",
                "binary_sha256": manifest["binary_sha256"],
                "config_path": f"{version_id}/engine_config.json",
                "config_sha256": manifest["config_sha256"],
                "manifest_sha256": sha256_file(destination / "manifest.json"),
                "compiler": manifest["compiler"],
                "compiler_flags": compiler_flags,
                "build_type": "Release",
                "accepted": accepted,
                "parent_version": parent_version,
                "change_category": change_category,
                "notes": notes,
            }
            registry["versions"].append(entry)
            atomic_write_json(self.path, registry)
            return self.verify(version_id)
        except BaseException:
            for path in (destination.rglob("*") if destination.exists() else []):
                if path.is_file():
                    path.chmod(0o666)
            shutil.rmtree(destination, ignore_errors=True)
            raise

    def promote(
        self, version_id: str, report_path: Path, override_reason: str | None = None
    ) -> None:
        version = self.verify(version_id)
        report_path = report_path.resolve()
        report_metadata_path, run_metadata_path, report_metadata, run_metadata = (
            _load_completed_promotion_run(report_path)
        )
        classification, normalized_override = _validated_promotion_decision(
            report_metadata, override_reason
        )
        binding = _validate_promotion_artifact_binding(
            version_id=version_id,
            version=version,
            report_metadata_path=report_metadata_path,
            run_metadata_path=run_metadata_path,
            report_metadata=report_metadata,
            run_metadata=run_metadata,
        )

        registry = self.load()
        registry["current_champion"] = version_id
        for entry in registry["versions"]:
            if entry["version_id"] == version_id:
                entry["accepted"] = True
                entry.setdefault("promotions", []).append(
                    {
                        "timestamp_utc": utc_now(),
                        "report_path": str(report_path.resolve()),
                        "classification": classification,
                        "override_reason": normalized_override,
                        "run_id": binding["run_id"],
                        "candidate_version_id": version_id,
                        "binary_sha256": binding["binary_sha256"],
                        "config_sha256": binding["config_sha256"],
                        "report_manifest_sha256": sha256_file(report_metadata_path),
                        "run_config_sha256": sha256_file(run_metadata_path),
                    }
                )
        atomic_write_json(self.path, registry)
