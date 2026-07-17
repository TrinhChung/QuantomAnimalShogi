from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .common import EvaluationError, repository_root
from .version_registry import VersionRegistry


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Manage immutable QAS evaluation versions")
    subparsers = parser.add_subparsers(dest="command", required=True)
    freeze = subparsers.add_parser("freeze")
    freeze.add_argument("--version-id", required=True)
    freeze.add_argument("--name", required=True)
    freeze.add_argument("--exe", required=True)
    freeze.add_argument("--config", default=str(repository_root() / "engine_config.json"))
    freeze.add_argument("--benchmark-exe")
    freeze.add_argument("--parent")
    freeze.add_argument("--change-category", required=True)
    freeze.add_argument("--accepted", action="store_true")
    freeze.add_argument("--notes", default="")
    freeze.add_argument("--allow-dirty", action="store_true")
    freeze.add_argument("--build-command", default="user-provided Release build")
    freeze.add_argument("--compiler-flags", default="auto")
    promote = subparsers.add_parser("promote")
    promote.add_argument("version_id")
    promote.add_argument("--report", required=True)
    promote.add_argument("--override-reason")
    verify = subparsers.add_parser("verify")
    verify.add_argument("version_id")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    registry = VersionRegistry()
    try:
        if arguments.command == "freeze":
            version = registry.freeze(
                version_id=arguments.version_id,
                display_name=arguments.name,
                executable=Path(arguments.exe),
                config=Path(arguments.config),
                benchmark_executable=(Path(arguments.benchmark_exe) if arguments.benchmark_exe else None),
                parent_version=arguments.parent,
                change_category=arguments.change_category,
                accepted=arguments.accepted,
                notes=arguments.notes,
                allow_dirty=arguments.allow_dirty,
                build_command=arguments.build_command,
                compiler_flags=arguments.compiler_flags,
            )
            print(f"Frozen immutable version: {version['directory']}")
        elif arguments.command == "promote":
            registry.promote(
                arguments.version_id, Path(arguments.report), arguments.override_reason
            )
            print(f"Promoted current champion: {arguments.version_id}")
        else:
            version = registry.verify(arguments.version_id)
            print(f"Verified {arguments.version_id}: {version['manifest']['binary_sha256']}")
        return 0
    except EvaluationError as error:
        print(f"Version registry error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
