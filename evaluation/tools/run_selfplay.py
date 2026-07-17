from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any

from .common import append_jsonl, atomic_write_json, utc_now
from .referee import (
    EngineSession,
    EngineSpec,
    NativeReferee,
    action_mask_hash,
)


def _side_number(side: str) -> int:
    return 0 if side == "south" else 1


def _engine_for_side(
    side: str,
    first_side: str,
    engine_first: EngineSpec,
    engine_second: EngineSpec,
    first_session: EngineSession,
    second_session: EngineSession,
) -> tuple[EngineSpec, EngineSession]:
    if side == first_side:
        return engine_first, first_session
    return engine_second, second_session


def _nullable_telemetry(response: dict[str, Any]) -> dict[str, Any]:
    fields = [
        "elapsed_process_cpu_ms",
        "completed_depth",
        "started_depth",
        "search_completed",
        "score",
        "pv_actions",
        "pv_decoded",
        "nodes",
        "nps",
        "tt_probes",
        "tt_hits",
        "tt_hit_rate",
        "tt_exact_hits",
        "tt_lower_hits",
        "tt_upper_hits",
        "tt_cutoffs",
        "tt_replacements",
        "tt_collisions",
        "cutoffs",
        "first_move_cutoffs",
        "first_move_cutoff_rate",
        "average_cutoff_rank",
        "leq_calls",
        "leq_input_moves",
        "leq_output_representatives",
        "leq_duplicate_ratio",
        "leq_grouping_ms",
        "propagation_calls",
        "propagation_ms",
        "evaluation_calls",
        "evaluation_ms",
        "fallback_used",
        "fallback_reason",
    ]
    return {field: response.get(field) for field in fields}


def play_game(
    *,
    run_id: str,
    game_id: str,
    attempt_id: int,
    pair_id: str,
    opening: dict[str, Any],
    candidate: EngineSpec,
    opponent: EngineSpec,
    candidate_first: bool,
    referee_executable: Path,
    move_soft_ms: int,
    move_hard_ms: int,
    tt_size_mb: int,
    run_directory: Path,
    move_completed: callable | None = None,
) -> dict[str, Any]:
    game_directory = run_directory / "games" / game_id / f"attempt_{attempt_id}"
    states_directory = game_directory / "states"
    masks_directory = game_directory / "legal_masks"
    stdout_directory = game_directory / "stdout"
    stderr_directory = game_directory / "stderr"
    for directory in (states_directory, masks_directory, stdout_directory, stderr_directory):
        directory.mkdir(parents=True, exist_ok=True)
    engine_first = candidate if candidate_first else opponent
    engine_second = opponent if candidate_first else candidate
    first_session = EngineSession(
        engine_first,
        tt_size_mb,
        stdout_directory / f"{engine_first.version_id}.txt",
        stderr_directory / f"{engine_first.version_id}.txt",
        int(opening["generation_seed"]),
    )
    second_session = EngineSession(
        engine_second,
        tt_size_mb,
        stdout_directory / f"{engine_second.version_id}.txt",
        stderr_directory / f"{engine_second.version_id}.txt",
        int(opening["generation_seed"]),
    )
    game_started = time.perf_counter()
    moves_path = game_directory / "moves.jsonl"
    global_moves_path = run_directory / "selfplay" / "moves.jsonl"
    snapshots: list[dict[str, Any]] = []
    candidate_time = 0.0
    opponent_time = 0.0
    candidate_nodes = 0
    opponent_nodes = 0
    failure_engine: str | None = None
    failure_status: str | None = None
    candidate_result = "draw"
    terminal_type = "aborted_or_incomplete"
    winner_engine: str | None = None
    final_snapshot: dict[str, Any] | None = None
    try:
        with NativeReferee(referee_executable) as referee:
            snapshot = referee.load(opening["full_serialized_state"])
            opening_snapshot = snapshot
            first_side = snapshot["side"]
            (game_directory / "opening_state.txt").write_text(snapshot["state_text"], encoding="utf-8")
            atomic_write_json(game_directory / "opening.json", opening)
            for ply in range(snapshot["remaining_horizon"]):
                if snapshot["terminal"] != "none":
                    break
                state_path = states_directory / f"ply_{ply:03d}.txt"
                state_path.write_text(snapshot["state_text"], encoding="utf-8")
                mask_path = masks_directory / f"ply_{ply:03d}.txt"
                mask_path.write_text(snapshot["action_mask"] + "\n", encoding="ascii")
                engine, session = _engine_for_side(
                    snapshot["side"],
                    first_side,
                    engine_first,
                    engine_second,
                    first_session,
                    second_session,
                )
                response = session.request_action(snapshot["protocol_request"], move_hard_ms)
                submitted_action = response.get("submitted_action")
                response_status = response["response_status"]
                if response_status == "valid_response" and submitted_action not in snapshot["legal_actions"]:
                    response_status = "illegal_action"
                    response["error_type"] = "illegal_action"
                    response["error_message"] = "submitted action is disabled by authoritative legal mask"
                move_record = {
                    "schema_version": 1,
                    "run_id": run_id,
                    "game_id": game_id,
                    "attempt_id": attempt_id,
                    "pair_id": pair_id,
                    "opening_id": opening["fixture_id"],
                    "opening_seed": opening["generation_seed"],
                    "opponent_id": opponent.version_id,
                    "candidate_version": candidate.version_id,
                    "engine_version": engine.version_id,
                    "engine_commit": engine.commit,
                    "engine_binary_sha256": engine.binary_sha256,
                    "engine_config_sha256": engine.config_sha256,
                    "timestamp_utc": utc_now(),
                    "side": snapshot["side"],
                    "ply": ply,
                    "turn_before": snapshot["turn"],
                    "remaining_horizon_before": snapshot["remaining_horizon"],
                    "state_hash_before": snapshot["state_hash"],
                    "full_state_path": str(state_path.resolve()),
                    "legal_count": snapshot["legal_count"],
                    "action_mask_hash": action_mask_hash(snapshot),
                    "response_status": response_status,
                    "raw_stdout_line": response.get("raw_stdout_line"),
                    "raw_search_action": submitted_action,
                    "submitted_action": submitted_action,
                    "decoded_move": None,
                    "elapsed_wall_ms": response["elapsed_wall_ms"],
                    "soft_timeout_ms": move_soft_ms,
                    "hard_timeout_ms": move_hard_ms,
                    "timeout": response_status == "external_timeout",
                    "process_exit_code": response.get("process_exit_code"),
                    **_nullable_telemetry(response),
                    "state_hash_after": None,
                    "terminal_after": None,
                    "terminal_type": None,
                    "winner_after": None,
                    "error_type": response.get("error_type"),
                    "error_message": response.get("error_message"),
                }
                if engine.version_id == candidate.version_id:
                    candidate_time += float(response["elapsed_wall_ms"])
                    candidate_nodes += int(response.get("nodes") or 0)
                else:
                    opponent_time += float(response["elapsed_wall_ms"])
                    opponent_nodes += int(response.get("nodes") or 0)
                if response_status != "valid_response":
                    failure_engine = engine.version_id
                    failure_status = response_status
                    move_record["terminal_after"] = True
                    move_record["terminal_type"] = f"{response_status}_loss"
                    other = opponent if engine.version_id == candidate.version_id else candidate
                    winner_engine = other.version_id
                    candidate_result = "loss" if engine.version_id == candidate.version_id else "win"
                    terminal_type = f"{response_status}_loss"
                    append_jsonl(moves_path, move_record)
                    append_jsonl(global_moves_path, move_record)
                    snapshots.append(move_record)
                    if move_completed is not None:
                        move_completed(move_record)
                    break
                after = referee.apply(int(submitted_action))
                move_record["state_hash_after"] = after["state_hash"]
                move_record["terminal_after"] = after["terminal"] != "none"
                move_record["terminal_type"] = after["terminal"]
                move_record["winner_after"] = after["winner"]
                append_jsonl(moves_path, move_record)
                append_jsonl(global_moves_path, move_record)
                snapshots.append(move_record)
                if move_completed is not None:
                    move_completed(move_record)
                snapshot = after
                final_snapshot = snapshot
                if snapshot["terminal"] != "none":
                    terminal_type = snapshot["terminal"]
                    if terminal_type == "draw":
                        candidate_result = "draw"
                    else:
                        winner_side = "south" if snapshot["winner"] == 0 else "north"
                        winner_engine = (
                            engine_first.version_id if winner_side == first_side else engine_second.version_id
                        )
                        candidate_result = "win" if winner_engine == candidate.version_id else "loss"
                    break
            else:
                terminal_type = "aborted_or_incomplete"
    except Exception as error:
        failure_status = "referee_failure"
        terminal_type = "referee_failure"
        (game_directory / "referee_failure.txt").write_text(str(error), encoding="utf-8")
    finally:
        first_session.close()
        second_session.close()

    valid_strength_game = terminal_type in {"catch", "try", "draw"}
    game_result = {
        "schema_version": 1,
        "run_id": run_id,
        "game_id": game_id,
        "attempt_id": attempt_id,
        "pair_id": pair_id,
        "opening_id": opening["fixture_id"],
        "engine_rng_seed": int(opening["generation_seed"]),
        "candidate_version": candidate.version_id,
        "opponent_version": opponent.version_id,
        "engine_first": engine_first.version_id,
        "engine_second": engine_second.version_id,
        "first_side": opening_snapshot["side"] if "opening_snapshot" in locals() else None,
        "winner": winner_engine,
        "candidate_result": candidate_result,
        "terminal_type": terminal_type,
        "total_plies": len(snapshots),
        "total_actions": len([move for move in snapshots if move["response_status"] == "valid_response"]),
        "first_divergence_ply": None,
        "illegal_engine": failure_engine if failure_status == "illegal_action" else None,
        "crashed_engine": failure_engine if failure_status == "process_crash" else None,
        "timed_out_engine": failure_engine if failure_status == "external_timeout" else None,
        "protocol_error_engine": (
            failure_engine
            if failure_status in {"malformed_output", "protocol_desynchronization"}
            else None
        ),
        "wall_time_ms": (time.perf_counter() - game_started) * 1000.0,
        "candidate_total_time_ms": candidate_time,
        "opponent_total_time_ms": opponent_time,
        "candidate_total_nodes": candidate_nodes or None,
        "opponent_total_nodes": opponent_nodes or None,
        "complete": valid_strength_game or failure_status is not None,
        "valid_strength_game": valid_strength_game,
        "final_state_hash": final_snapshot["state_hash"] if final_snapshot else None,
        "reproducible_command": (
            f'scripts\\resume_evaluation.bat "{run_directory}" --replay-game {game_id}'
        ),
        "move_log_path": str(moves_path.resolve()),
        "stdout_paths": {
            engine_first.version_id: str((stdout_directory / f"{engine_first.version_id}.txt").resolve()),
            engine_second.version_id: str(
                (stdout_directory / f"{engine_second.version_id}.txt").resolve()
            ),
        },
        "stderr_paths": {
            engine_first.version_id: str((stderr_directory / f"{engine_first.version_id}.txt").resolve()),
            engine_second.version_id: str(
                (stderr_directory / f"{engine_second.version_id}.txt").resolve()
            ),
        },
        "engine_commands": {
            engine_first.version_id: first_session.command_line,
            engine_second.version_id: second_session.command_line,
        },
        "environment": {key: os.environ.get(key) for key in ("PATH", "NUMBER_OF_PROCESSORS")},
        "process_started_at_utc": {
            engine_first.version_id: first_session.started_at_utc,
            engine_second.version_id: second_session.started_at_utc,
        },
        "process_stopped_at_utc": utc_now(),
    }
    atomic_write_json(game_directory / "game.json", game_result)
    return game_result


def replay_game(game_result: dict[str, Any], referee_executable: Path) -> bool:
    game_path = Path(game_result["move_log_path"])
    opening_path = game_path.parent / "opening.json"
    opening = json.loads(opening_path.read_text(encoding="utf-8"))
    moves = [json.loads(line) for line in game_path.read_text(encoding="utf-8").splitlines() if line]
    with NativeReferee(referee_executable) as referee:
        snapshot = referee.load(opening["full_serialized_state"])
        for move in moves:
            if move["response_status"] != "valid_response":
                break
            if move["state_hash_before"] != snapshot["state_hash"]:
                return False
            if move.get("submitted_action") not in snapshot["legal_actions"]:
                return False
            snapshot = referee.apply(int(move["submitted_action"]))
            if move["state_hash_after"] != snapshot["state_hash"]:
                return False
            if move.get("terminal_after") != (snapshot["terminal"] != "none"):
                return False
            if move.get("terminal_type") != snapshot["terminal"]:
                return False
            if move.get("winner_after") != snapshot["winner"]:
                return False
    expected = game_result.get("final_state_hash")
    if expected is not None and snapshot["state_hash"] != expected:
        return False
    if game_result.get("valid_strength_game"):
        if snapshot["terminal"] != game_result.get("terminal_type"):
            return False
        if len([move for move in moves if move["response_status"] == "valid_response"]) != int(
            game_result.get("total_actions", -1)
        ):
            return False
    return True


def run_paired_matches(
    *,
    run_id: str,
    candidate: EngineSpec,
    opponents: list[EngineSpec],
    openings: list[dict[str, Any]] | dict[str, list[dict[str, Any]]],
    referee_executable: Path,
    profile: dict[str, Any],
    run_directory: Path,
    attempt_id: int,
    is_complete: callable,
    task_started: callable,
    move_completed: callable,
    mark_complete: callable,
) -> list[dict[str, Any]]:
    selfplay_directory = run_directory / "selfplay"
    selfplay_directory.mkdir(parents=True, exist_ok=True)
    openings_path = selfplay_directory / "openings.jsonl"
    existing_opening_ids: set[str] = set()
    if openings_path.is_file():
        existing_opening_ids = {
            json.loads(line)["fixture_id"]
            for line in openings_path.read_text(encoding="utf-8").splitlines()
            if line
        }
    all_openings = (
        list({item["fixture_id"]: item for values in openings.values() for item in values}.values())
        if isinstance(openings, dict)
        else openings
    )
    for opening in all_openings:
        if opening["fixture_id"] not in existing_opening_ids:
            append_jsonl(openings_path, opening)
            existing_opening_ids.add(opening["fixture_id"])
    games_path = selfplay_directory / "games.jsonl"
    existing: dict[str, dict[str, Any]] = {}
    if games_path.is_file():
        for line in games_path.read_text(encoding="utf-8").splitlines():
            if line:
                game = json.loads(line)
                existing[game["game_id"]] = game
    results = list(existing.values())
    for opponent in opponents:
        opponent_openings = openings.get(opponent.version_id, []) if isinstance(openings, dict) else openings
        for pair_index, opening in enumerate(opponent_openings):
            pair_id = f"{opponent.version_id}_pair_{pair_index:04d}"
            role_order = (True, False) if pair_index % 2 == 0 else (False, True)
            for candidate_first in role_order:
                role = "candidate_first" if candidate_first else "candidate_second"
                game_id = f"{pair_id}_{role}"
                task = f"game:{game_id}"
                if is_complete(task) and game_id in existing:
                    continue
                task_started(task)
                game = play_game(
                    run_id=run_id,
                    game_id=game_id,
                    attempt_id=attempt_id,
                    pair_id=pair_id,
                    opening=opening,
                    candidate=candidate,
                    opponent=opponent,
                    candidate_first=candidate_first,
                    referee_executable=referee_executable,
                    move_soft_ms=profile["move_soft_ms"],
                    move_hard_ms=profile["move_hard_ms"],
                    tt_size_mb=profile["tt_size_mb"],
                    run_directory=run_directory,
                    move_completed=move_completed,
                )
                game["replay_passed"] = replay_game(game, referee_executable)
                atomic_write_json(
                    Path(game["move_log_path"]).parent / "game.json",
                    game,
                )
                append_jsonl(games_path, game)
                existing[game_id] = game
                results.append(game)
                mark_complete(task, game)
    return list(existing.values())
