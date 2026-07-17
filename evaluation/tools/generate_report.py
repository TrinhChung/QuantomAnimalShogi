from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .common import atomic_write_text


def _json_block(value: Any) -> str:
    return "```json\n" + json.dumps(value, indent=2, sort_keys=True) + "\n```\n"


def _without_keys(value: dict[str, Any] | None, *excluded: str) -> dict[str, Any]:
    if not value:
        return {}
    return {key: item for key, item in value.items() if key not in excluded}


def _compact_time_summary(value: dict[str, Any] | None) -> dict[str, Any]:
    if not value:
        return {}
    return {
        "timing_policy": value.get("timing_policy"),
        "opponents": {
            opponent: _without_keys(summary, "comparisons")
            for opponent, summary in value.get("opponents", {}).items()
        },
    }


def _compact_diagnostics(value: dict[str, Any] | None) -> dict[str, Any]:
    if not value:
        return {}
    overhead = _without_keys(value.get("instrumentation_overhead"), "comparisons")
    return {
        "runs": value.get("runs"),
        "instrumentation_overhead": overhead,
        "telemetry_availability": value.get("telemetry_availability"),
    }


def _compact_divergences(divergences: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result_fields = (
        "best_action",
        "score",
        "completed_depth",
        "nodes",
        "elapsed_wall_ms",
        "timeout",
        "fallback_used",
    )
    compact: list[dict[str, Any]] = []
    for divergence in divergences:
        reanalysis = {
            name: {field: record.get(field) for field in result_fields}
            for name, record in divergence.get("reanalysis", {}).items()
            if isinstance(record, dict)
        }
        artifact_directory = (
            f"divergence/{divergence.get('opponent')}_{divergence.get('fixture_id')}"
        )
        compact.append(
            {
                key: divergence.get(key)
                for key in (
                    "source",
                    "opponent",
                    "fixture_id",
                    "turn",
                    "side_to_move",
                    "candidate_action",
                    "opponent_action",
                    "candidate_score",
                    "opponent_score",
                    "classification",
                )
            }
            | {"artifact_directory": artifact_directory, "reanalysis": reanalysis}
        )
    return compact


def generate_report(
    *,
    run_directory: Path,
    run_config: dict[str, Any],
    candidate: dict[str, Any],
    opponents: list[dict[str, Any]],
    correctness: dict[str, Any],
    performance: dict[str, Any],
    time_controlled: list[dict[str, Any]],
    time_summary: dict[str, Any] | None = None,
    game_summaries: dict[str, Any],
    reliability: dict[str, Any],
    divergences: list[dict[str, Any]],
    classification: str,
    gates: dict[str, bool],
    corpus_coverage: dict[str, Any] | None = None,
) -> Path:
    report_path = run_directory / "report.md"
    resume_command = f'scripts\\resume_evaluation.bat "{run_directory}"'
    additional_pairs = max(
        (int(summary.get("additional_recommended_pairs") or 0) for summary in game_summaries.values()),
        default=0,
    )
    extension_command = (
        f"{resume_command} --extend-pairs {additional_pairs}" if additional_pairs else None
    )
    evaluation_arguments = [
        "scripts\\evaluate_built_candidate.bat",
        f'--candidate-exe "{candidate.get("original_executable_path")}"',
        f'--candidate-config "{candidate.get("original_config_path")}"',
        f'--candidate-name "{candidate.get("display_name")}"',
        f'--candidate-version-id {candidate.get("version_id")}',
        f'--change-category {run_config["change_category"]}',
        f'--profile {run_config["profile"]}',
        f'--opponents {",".join(run_config.get("opponent_ids", []))}',
        f'--seed {run_config.get("seed")}',
    ]
    if candidate.get("original_benchmark_path"):
        evaluation_arguments.insert(
            2, f'--candidate-benchmark "{candidate.get("original_benchmark_path")}"'
        )
    if run_config.get("candidate_binary_sha256") in set(
        run_config.get("opponent_binary_hashes", {}).values()
    ):
        evaluation_arguments.append("--allow-identical-binary")
    evaluation_command = " ".join(evaluation_arguments)
    fixed_opponents = performance.get("opponents", {})
    fixed_quality = {
        opponent: {
            key: values.get(key)
            for key in (
                "comparison_count",
                "authoritative_timing_comparison_count",
                "timing_quality_passed",
                "timing_noise_floor",
                "geometric_mean_elapsed_ratio",
                "geometric_mean_node_ratio",
                "geometric_mean_node_cost_ratio",
                "geometric_mean_peak_memory_ratio",
                "maximum_peak_memory_ratio",
                "critical_regressions",
                "node_regressions",
                "node_cost_regressions",
                "node_stability_failures",
            )
        }
        for opponent, values in fixed_opponents.items()
    }
    warnings: list[str] = []
    if not run_config["profile_config"].get("acceptance_permissions"):
        warnings.append("This profile is diagnostic-only and cannot accept or promote a candidate.")
    if any(not values.get("timing_quality_passed", False) for values in fixed_opponents.values()):
        warnings.append(
            "At least one fixed-depth timing aggregate lacks the configured duration/repetition quality; "
            "its elapsed ratio is excluded from acceptance."
        )
    if any(summary.get("bootstrap", {}).get("degenerate_input") for summary in game_summaries.values()):
        warnings.append(
            "Observed pair scores are degenerate; the finite-sample interval floor prevents a false zero-width CI."
        )
    sections = [
        "# Engine Version Evaluation Report\n",
        "## 1. Executive verdict\n\n"
        f"**{classification}**\n\nThis verdict is generated from centralized, category-specific gates.\n",
        "## 2. Evidence warnings\n\n"
        + ("\n".join(f"- {warning}" for warning in warnings) if warnings else "No evidence-quality warning.")
        + "\n",
        "## 3. Resolved benchmark profile\n\n" + _json_block(
            {"profile": run_config["profile"], **run_config["profile_config"]}
        ),
        "## 4. Candidate metadata\n\n" + _json_block(candidate),
        "## 5. Opponent metadata\n\n" + _json_block(opponents),
        "## 6. Reproducibility and measurement controls\n\n"
        f"Run configuration hash: `{run_config['run_config_hash']}`  \n"
        f"Corpus revision/hash: `{run_config['corpus_revision']}` / `{run_config['corpus_sha256']}`  \n"
        f"Referee hash: `{run_config.get('referee_sha256')}`\n\n"
        + _json_block(run_config.get("benchmark_controls")),
        f"## 7. Change category\n\n`{run_config['change_category']}`\n",
        "## 8. Correctness and configuration comparison\n\n" + _json_block(correctness),
        "## 9. Corpus coverage\n\n" + _json_block(corpus_coverage),
        "## 10. Fixed-depth methodology and aggregate results\n\n"
        + _json_block({"timing_policy": performance.get("timing_policy"), "opponents": fixed_quality}),
        "Per-position samples and p10/median/p90/CV/variance/stability: "
        "`performance/fixed_depth.csv`. Every measured invocation: `performance/raw_runs.csv`.\n",
        "## 11. Node reduction, node cost, and elapsed time\n\n"
        "These are separate metrics; NPS is not treated as a strength proxy.\n\n"
        + _json_block(fixed_quality),
        "## 12. Fixed-time reliable-depth analysis\n\n"
        f"Recorded runs: {len(time_controlled)}. Raw rows: `performance/time_controlled.csv`.\n\n"
        + _json_block(_compact_time_summary(time_summary)),
        "The configured budget is the soft stop; the resolved profile also records the hard-deadline "
        "grace/multiplier. Per-condition distributions and every requested soft/hard limit are in "
        "`performance/time_summary.json` and `performance/time_controlled.csv`.\n",
        "## 13. Diagnostic telemetry and overhead\n\n"
        "Timing runs remain low-overhead; diagnostic counters are separate. Missing match telemetry is `null`, not zero.\n\n"
        + _json_block(
            {
                "fixed_depth": _compact_diagnostics(performance.get("diagnostics")),
                "fixed_time": _compact_diagnostics((time_summary or {}).get("diagnostics")),
            }
        ),
        "## 14. Candidate versus current champion\n\n"
        + _json_block(game_summaries.get(run_config["current_champion"], {})),
        "## 15. Candidate versus permanent anchor\n\n"
        + _json_block(game_summaries.get(run_config["stable_anchor"], {})),
        "## 16. Paired openings, role balance, and opening bias\n\n" + _json_block(game_summaries),
        "## 17. Decisive and draw length / move-time analysis\n\n"
        + _json_block(
            {
                opponent: {
                    key: summary.get(key)
                    for key in (
                        "decisive_game_length",
                        "draw_game_length",
                        "median_move_time_ms",
                        "p90_move_time_ms",
                        "p95_move_time_ms",
                        "p99_move_time_ms",
                    )
                }
                for opponent, summary in game_summaries.items()
            }
        ),
        "## 18. Reliability\n\n" + _json_block(reliability),
        "## 19. First-divergence and diagnostic replay\n\n"
        + _json_block(_compact_divergences(divergences)),
        "## 20. Acceptance gates\n\n" + _json_block(gates),
        f"## 21. Final classification\n\n`{classification}`\n",
        "## 22. Resume, extend, and reproduce\n\n"
        f"Original command: `{evaluation_command}`\n\n"
        f"Resume: `{resume_command}`\n\n"
        + (f"Extend: `{extension_command}`" if extension_command else "No extension currently recommended.")
        + "\n\nDivergence reproduction commands are stored beside each divergence.\n",
    ]
    atomic_write_text(report_path, "\n".join(sections))
    return report_path
