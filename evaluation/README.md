# Permanent Engine Evaluation System

This directory owns the only standard acceptance workflow for future Quantum Animal Shogi
engine versions. It evaluates an already-built executable; it never silently rebuilds the
candidate and never promotes it automatically.

## The complete future workflow

1. Build the candidate and all standard Release targets:

   ```powershell
   scripts\build_version.ps1 -Version current -Configuration Release
   ```

2. Run the single evaluation command:

   ```powershell
   scripts\evaluate_built_candidate.bat ^
     --candidate-exe "build\current\Release\qas.exe" ^
     --candidate-name "Stage 5.1" ^
     --candidate-version-id stage5-1 ^
     --change-category performance_only ^
     --profile strength_candidate
   ```

3. Inspect the printed `local_reports/evaluations/<run-id>/report.md`.

That command validates immutable hashes and the protocol, runs CTest, compares fixed-depth
correctness, runs authoritative and diagnostic performance measurements sequentially, runs
time-controlled searches, plays paired side-swapped two-engine games, replays saved games,
bootstraps opening pairs, analyzes the first fixed-depth divergence, applies the central gates,
and writes the report. Future agents must not replace these stages with ad-hoc tests.

The standard build creates `qas_evaluation_benchmark.exe` beside `qas.exe`. The one-command
pipeline locates it automatically. This companion links the exact candidate source and provides
cold-TT fixed-depth searches with iterative deepening and aspiration disabled; it does not alter
the contest executable's behavior. The companion is a JSON request/response worker: launching it
directly without stdin intentionally waits for a request and can look idle. Use the batch entry
point above; it prints every stage, completed/remaining benchmark count, completed/remaining game
count, elapsed time, and the final report path.

## Profiles and policy

- `config/evaluation_profiles.json` is the sole owner of fixture counts, depths, repetitions,
  time controls, TT size, opening-pair counts, and bootstrap sample counts.
- `config/acceptance_policy.json` is the sole owner of pass/fail thresholds.
- Diagnostic profiles have no acceptance permission and therefore cannot accept or promote a
  candidate. They normally finish `INCONCLUSIVE`, but correctness or reliability failures still
  produce the corresponding rejection.
- Changing a standard requires changing these version-controlled files and documenting why.

The required category is one of `performance_only`/`optimization_only`, `move_ordering`,
`search_control`, `selective_search`/`pruning_change`, `canonicalization_change`,
`evaluation_change`, `rule_change`, `architecture_change`, or `mixed`. A `rule_change` cannot
pass ordinary performance acceptance and requires a separate rule audit. Pruning and
canonicalization categories require the configured non-selective/reference check.

The twelve canonical profiles are below. Runtime is a host-dependent planning estimate, followed
by the configured hard planning ceiling. `pairs` means paired openings per normal opponent; the
promotion and architecture profiles use 100 additional anchor pairs when the anchor differs.

| profile | purpose | fixtures / fixed depth | equal-time budgets | warm-up / measured | pairs | telemetry | acceptance | expected / max |
|---|---|---|---|---|---:|---|---|---|
| `infrastructure_smoke` | wiring, hash, protocol, referee, replay | 2 / 2,3 | 1 fixture: 50 ms | 1/1 fixed; 0/1 time | 5 | split | none | 1 / 3 min |
| `correctness_regression` | rules and deterministic reference search | 16 / 4,5 | none | 0/1 | 0 | low overhead | none | 5 / 15 min |
| `fixed_depth_quick` | shallow nodes, node cost, timing variance | 8 / 5,6 | none | 1/5 | 0 | split | none | 10 / 30 min |
| `fixed_depth_deep` | authoritative deep fixed-depth evidence | 16 / 7,8,9 | none | 1/7 | 0 | split | none | 90 / 240 min |
| `fixed_time_quick` | reliable depth and move stability | 4 fixed; 8 time / 4 | 250, 1000 ms | 1/3 time | 0 | split | none | 5 / 15 min |
| `fixed_time_contest` | long/contest deadline behavior | 8 fixed; 12 time / 6 | 3, 5, 25 s | 1/3 | 0 | split | none | 75 / 180 min |
| `diagnostic_telemetry` | counters and instrumentation overhead | 8 / 6,7 | 4 fixtures: 1 s | 1/3 fixed; 1/1 time | 0 | diagnostic | none | 20 / 60 min |
| `strength_quick` | catastrophic strength regression only | 4 / 4 | 2 fixtures: 250 ms | 0/1 | 20 | low overhead | none | 20 / 60 min |
| `strength_candidate` | normal candidate acceptance evidence | 12 / 6,7,8 | 8 fixtures: 250 ms, 1 s, 3 s | 1/5 fixed; 1/3 time | 100 | split | performance, strength | 360 / 720 min |
| `promotion_test` | champion/anchor promotion evidence | 20 / 6..10 | 12 fixtures: 250 ms..25 s | 1/7 fixed; 1/3 time | 200 + anchor 100 | split | performance, strength, equivalent | 720 / 1440 min |
| `reliability_soak` | crash/timeout/protocol/replay soak | 4 / 4 | 4 fixtures: 250 ms | 0/1 | 200 | low overhead | none | 240 / 600 min |
| `architecture_change_full` | complete correctness/performance/strength/memory audit | 24 / 6..11 | 16 fixtures: 250 ms..25 s | 1/7 fixed; 1/3 time | 200 + anchor 100 | split | performance, strength, equivalent | 1080 / 2160 min |

All use deterministic stratified corpus selection, sequential fresh benchmark processes, cold TT,
single-core affinity when the OS permits it, normal process priority, and alternating A/B then B/A
measurement order. A fixed-time budget is the engine's soft stop; the centralized defaults set the
hard stop to the greater of `soft + 100 ms` and `1.10 * soft`, and both limits plus overshoot are
reported. The exact fixture IDs and fully resolved profile are saved in each run.

Run any canonical profile by changing only `--profile`:

```powershell
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile infrastructure_smoke
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile correctness_regression
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile fixed_depth_quick
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile fixed_depth_deep
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile fixed_time_quick
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile fixed_time_contest
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile diagnostic_telemetry
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile strength_quick
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile strength_candidate
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile promotion_test
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category performance_only --profile reliability_soak
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Candidate" --change-category architecture_change --profile architecture_change_full
```

Legacy names `smoke`, `quick`, `main`, `release`, and `overnight` remain compatible, but new work
should use the canonical names above.

## CLI and troubleshooting

If the benchmark/referee companions are missing, build only the required Release targets:

```powershell
cmake --build build/current --config Release --target qas qas_evaluation_benchmark qas_evaluation_referee
```

The pipeline normally discovers the companion next to `qas.exe`. Use
`--candidate-benchmark <path>` or `--referee <path>` only for a nonstandard layout. Other supported
controls are `--opponents id1,id2`, `--include-previous`, `--seed`, `--output-dir`, and
`--candidate-config`. `--allow-identical-binary` is restricted to framework validation;
`--extend-pairs` is normally emitted by an inconclusive report; `--replay-game` rechecks a saved
game. Use `scripts\resume_evaluation.bat <run-directory>` rather than manually reconstructing
resume arguments.

The required Stage 5 anchor self-check is:

```powershell
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" --candidate-name "Stage 5 anchor self-check" --change-category performance_only --profile infrastructure_smoke --opponents stage5-clean --allow-identical-binary
```

## Permanent corpus

`corpus/manifest.json` freezes corpus revision 1 and the SHA-256 of `corpus-v1.jsonl`. Each fixture
contains the complete native state, authoritative structural hash, deterministic seed and action
sequence, turn/horizon/side, legal count, uncertainty, hand count, Lion-candidate counts, source,
and category labels. The 256 positions include the trusted Stage 3.5 named positions and legal
seeded playouts from ply 2 through 24. Evaluations refuse to run if the corpus hash changes.

Do not edit a published corpus file in place. Reviewed tactical, divergence, timeout, or bug
fixtures are added only by creating a new corpus revision and updating the profile intentionally.
No reviewed historical crash/timeout fixture existed in the clean Stage 5.0 repository, so the
revision-1 historical-failure directories are intentionally empty rather than containing invented
evidence.

## Frozen versions

Freeze an accepted build once; normal evaluations always execute the saved bytes:

```powershell
scripts\freeze_version.bat ^
  --version-id stage5-1 ^
  --name "Stage 5.1" ^
  --exe "build\current\Release\qas.exe" ^
  --config "engine_config.json" ^
  --parent stage5-clean ^
  --change-category performance_only
```

The command refuses an existing ID, validates JSON, records Git/build/host/features metadata,
copies the benchmark companion, hashes every artifact, and makes frozen files read-only. A dirty
tree is rejected unless `--allow-dirty` is explicitly supplied and its reason is stored in
`--notes`.

Evaluate a frozen candidate with:

```powershell
scripts\evaluate_registered_version.bat stage5-1 --profile promotion_test
```

Default opponents are the current champion and the permanent `stage5-clean` anchor when they
differ. Use `--opponents stage5-clean,stage4-final` only for an intentional explicit comparison.

Promotion is a separate manual action and is never part of evaluation:

```powershell
scripts\promote_version.bat stage5-1 ^
  --report "local_reports\evaluations\<run-id>\report.md"
```

The registry refuses promotion unless the completed report manifest and `run_config.json` bind
the run to the exact frozen version ID, executable SHA-256, and config SHA-256. Missing legacy
metadata, mixed run IDs, or a hash mismatch are rejected. An override requires
`--override-reason`; it may override only the evaluation classification, never artifact identity,
and the reason, run ID, report/config metadata hashes, and promoted artifact hashes are permanently
recorded.

## Resume and extension

Every completed benchmark task and game is atomically recorded in `progress.json`; move JSONL is
flushed after every attempt. Resume without rerunning completed work:

```powershell
scripts\resume_evaluation.bat "local_reports\evaluations\<run-id>"
```

The first run snapshots the candidate executable, benchmark companion, and config into the run.
Resume executes those frozen snapshots even if the original build directory was removed, and
revalidates their hashes together with frozen opponents, referee, corpus, policy, and run config.
Incomplete games restart from their original opening with a new attempt ID. An inconclusive report
prints the exact extension command and a bounded recommended additional pair count when strength
extension is applicable.

## Artifact layout

Each unique run contains `manifest.json`, immutable input snapshots, `run_config.json`, atomic
`progress.json`, `correctness/`, `performance/`, `selfplay/`, per-game stdout/stderr/states/masks,
`divergence/`, `failures/`, and the final `report.md`. Operational failures are excluded from the
primary playing-strength sample and reported separately.

Protocol move logs deliberately store `null` for search telemetry the production contest protocol
does not expose. Fixed-depth and time-controlled benchmark logs contain the available search,
TT, cutoff, propagation, evaluation, CPU-time, and peak-memory telemetry. Low-overhead timing and
diagnostic telemetry are separate runs, and the report measures the diagnostic overhead. First
divergences are sourced from same-state match decisions, fixed-time results, or fixed-depth results
in that order and are automatically rerun deeper, longer, with TT off, and with diagnostics.
Values are never fabricated.

## Test the test system

```powershell
python -m unittest discover -s tests\evaluation -p "test_*.py" -v
ctest --test-dir build/current -C Release --output-on-failure
```

The tests include engines that return an illegal action, crash, time out, emit malformed output,
emit extra stdout, and play the first legal action.
