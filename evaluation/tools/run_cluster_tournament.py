from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from .common import append_jsonl, atomic_write_json, sha256_file, utc_now
from .corpus import load_corpus, select_openings
from .referee import EngineSpec
from .run_pipeline import _profiles
from .run_selfplay import play_game
from .summarize_results import summarize_games


ALLOWED_PROFILES = {
    "strength_quick",
    "strength_candidate",
    "promotion_test",
    "reliability_soak",
}


def _engine(
    version_id: str,
    executable: Path,
    config: Path,
    benchmark: Path | None,
    commit: str,
) -> EngineSpec:
    return EngineSpec(
        version_id=version_id,
        executable=executable.resolve(),
        config=config.resolve(),
        binary_sha256=sha256_file(executable),
        config_sha256=sha256_file(config),
        commit=commit,
        benchmark_executable=benchmark.resolve() if benchmark else None,
    )


def _read_json_lines(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def run(arguments: argparse.Namespace) -> Path:
    profile = _profiles()[arguments.profile]
    _, fixtures = load_corpus()
    pair_count = arguments.pairs or int(profile["opening_pairs"])
    openings = select_openings(
        fixtures,
        pair_count,
        arguments.seed,
        profile.get("opening_plies"),
    )
    output = Path(arguments.output_dir).resolve()
    (output / "games").mkdir(parents=True, exist_ok=False)
    (output / "selfplay").mkdir()
    candidate = _engine(
        arguments.candidate_id,
        Path(arguments.candidate_exe),
        Path(arguments.candidate_config),
        Path(arguments.candidate_benchmark) if arguments.candidate_benchmark else None,
        arguments.candidate_commit,
    )
    opponent = _engine(
        arguments.opponent_id,
        Path(arguments.opponent_exe),
        Path(arguments.opponent_config),
        Path(arguments.opponent_benchmark) if arguments.opponent_benchmark else None,
        arguments.opponent_commit,
    )
    games_path = output / "selfplay" / "games.jsonl"
    games: list[dict[str, Any]] = []
    for pair_index, opening in enumerate(openings):
        for candidate_first in (True, False):
            role = "first" if candidate_first else "second"
            pair_id = f"{opponent.version_id}_pair_{pair_index:04d}"
            game = play_game(
                run_id=output.name,
                game_id=f"{pair_id}_candidate_{role}",
                attempt_id=1,
                pair_id=pair_id,
                opening=opening,
                candidate=candidate,
                opponent=opponent,
                candidate_first=candidate_first,
                referee_executable=Path(arguments.referee).resolve(),
                move_soft_ms=int(profile["move_soft_ms"]),
                move_hard_ms=int(profile["move_hard_ms"]),
                tt_size_mb=int(profile["tt_size_mb"]),
                run_directory=output,
            )
            append_jsonl(games_path, game)
            games.append(game)
            print(
                f"completed {len(games)}/{pair_count * 2} games: "
                f"{game['game_id']} {game['candidate_result']}",
                flush=True,
            )
    moves = _read_json_lines(output / "selfplay" / "moves.jsonl")
    summary = summarize_games(
        games,
        moves,
        candidate.version_id,
        int(profile["bootstrap_samples"]),
        arguments.seed,
        minimum_pairs=0,
        maximum_pairs=pair_count,
    )
    summary_path = output / "summary.json"
    atomic_write_json(
        summary_path,
        {
            "schema_version": 1,
            "created_at_utc": utc_now(),
            "profile": arguments.profile,
            "pair_count": pair_count,
            "candidate": {
                "version_id": candidate.version_id,
                "commit": candidate.commit,
                "binary_sha256": candidate.binary_sha256,
            },
            "opponent": {
                "version_id": opponent.version_id,
                "commit": opponent.commit,
                "binary_sha256": opponent.binary_sha256,
            },
            "summary": summary,
        },
    )
    return summary_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a portable paired cluster tournament")
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--candidate-exe", required=True)
    parser.add_argument("--candidate-config", required=True)
    parser.add_argument("--candidate-benchmark")
    parser.add_argument("--candidate-commit", required=True)
    parser.add_argument("--opponent-id", required=True)
    parser.add_argument("--opponent-exe", required=True)
    parser.add_argument("--opponent-config", required=True)
    parser.add_argument("--opponent-benchmark")
    parser.add_argument("--opponent-commit", required=True)
    parser.add_argument("--referee", required=True)
    parser.add_argument("--profile", choices=sorted(ALLOWED_PROFILES), required=True)
    parser.add_argument("--pairs", type=int)
    parser.add_argument("--seed", type=int, default=0x51415335)
    parser.add_argument("--output-dir", required=True)
    arguments = parser.parse_args(argv)
    if arguments.pairs is not None and not 1 <= arguments.pairs <= 500:
        parser.error("--pairs must be between 1 and 500")
    print(run(arguments))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
