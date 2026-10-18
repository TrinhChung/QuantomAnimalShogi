# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` is the contributor entry point and its rules apply here too: read the relevant
`docs/00_*.md`–`docs/10_*.md` policy before changing a module, keep one coherent purpose per
patch, and do not add ML/neural-network code (classical-engine phase only).

## Project

CPU-only C++17 engine for Quantum Animal Shogi. It speaks a JSON-line protocol on stdin/stdout
(240-action encoding `action = source*12 + dest`, sources 0..11 board / 12..19 hand). Game
rules and protocol: `docs/RULE_GAME.md`. The authoritative rule order (ported from the
organizer's Rust engine) is in `docs/QUANTUM_ANIMAL_SHOGI_RULES_SOURCE_SPEC.md` and
`docs/04_STATE_AND_MOVE_RULES.md`. Accepted algorithm/stage status lives in
`docs/current_stage_algorithms.md`.

`quantum-animal-shogi/` (the Vue + Rust/WASM web app described in README.md) may be empty in a
given checkout.

## Build and test

Build trees must be `build/<version>`. CMake hard-fails on any other binary dir, and
`build_*`/`cmake-build-*` directories are disallowed.

```powershell
scripts\build_version.ps1 -Version current -Configuration Release            # configure + build + ctest
scripts\build_version.ps1 -Version current -Configuration Release -SkipTests
cmake -S . -B build/current                                                    # direct CMake, same layout
ctest --test-dir build/current -C Release --output-on-failure                 # all tests
ctest --test-dir build/current -C Release -R alpha_beta --output-on-failure   # one test
```

CTest names are `quantum_state`, `game_rules`, `alpha_beta`, `successor_equivalence`,
`dfpn_guard`, `position_book`, `contest_protocol`, `engine_config`, `stage35_fixtures`,
`evaluation_optimization`,
`evaluation_framework` (Python `unittest` over `tests/evaluation/`, only if Python 3 is found).
Each C++ test is a standalone executable with a hand-rolled `check()` and `main()`. There is no
gtest. To add a case, write a `test_*` function and call it from that file's `main()`. New test
files need an `add_executable` + `add_test` in `CMakeLists.txt`. The `stage35_fixtures` and
`evaluation_optimization` tests must run with the repo root as working directory (CTest sets this).
They read `benchmarks/fixtures/stage35`, which is gitignored. After a fresh clone or build, generate
it once from the repo root:
`.\build\current\Release\qas_stage35_benchmark.exe --generate-fixtures --suite generate`.

The repo is LF-only (`.editorconfig` sets `end_of_line = lf`). `evaluation_framework` hashes
`evaluation/corpus/*` byte-for-byte, so a CRLF checkout fails with "permanent corpus hash mismatch".
This clone sets `core.autocrlf=false` locally to override the system-wide `true`. Keep new files LF.

Format with the repo `.clang-format` (Google-based, 4-space indent, 100 columns):
`clang-format -i <files>`, or check with `clang-format --dry-run --Werror <file>`.

## Running the engine

```powershell
.\build\current\Release\qas.exe                          # contest protocol, uses engine_config.json
.\build\current\Release\qas.exe protocol 1000 64         # soft ms, max depth
.\build\current\Release\qas.exe --profile low_ram --tt-size-mb 128
Get-Content state.txt | .\build\current\Release\qas.exe search 1000 8
Get-Content state.txt | .\build\current\Release\qas.exe search-leq 1000 8 12
Get-Content state.txt | .\build\current\Release\qas.exe legal
# offline book analysis: positions file = "# label" + debug state blocks (protocol view, side S)
.\build\current\Release\qas_book_builder.exe positions.txt 60000 30000 31 engine_config.json
```

The contest decision path in `main.cpp::run_protocol` is: position-book lookup → otherwise
`guarded_find_best_move` (alpha-beta + concurrent df-pn) → `action_mask` validation fallback.
The book file (`book/position_book.txt`, copied next to `qas.exe` on build) is resolved from the
configured path, then relative to the executable. Relevant `engine_config.json` keys:
`qsearch_depth`, `dfpn_guard_enabled`, `dfpn_guard_max_ply`, `eval_v2_enabled`, `eval_v2_weights`,
`book_enabled`, `book_path`. eval_v2 (Texel weights) and the position book are enabled in `engine_config.json` (profile defaults
stay off). Without `--config`, `engine_config.json` is read from the working directory, then from next to
`qas.exe` (the build copies it there); stderr logs which file was used. `book/position_book.txt` is the
win-replay book built from tournament history (our old moves on won/drawn lines, verified deviations on
lost lines); `book/position_book_old40.txt` is the previous small book.

Debug state text format (`src/rules/state_text.cpp`): a header `S|N <turn>`, then 8 lines of
`<piece-id> <owner S|N> <origin S|N> <square|H[slot]> <CGELH-mask>`.

stdout is protocol-only: one integer per `get_action`, `"OK"` per `end_game`. All diagnostics
go to stderr. Never print elsewhere from rules/search code.

## Architecture

All library code builds into one static lib `qas_core`. Every executable (contest `qas`,
benchmarks, evaluation adapters, tests) links it. Dependency direction is enforced by policy:
`core <- rules <- search`. `io` depends on `core` + `rules`. `exact` plugs into search only
through `SearchOptions::successor_reducer` (a function pointer), so search never includes
`exact/`.

- **core**: `Animal`/`Mask` (5-bit Chick, Giraffe, Elephant, Lion, Hen), `EngineConfig`
  profiles + `engine_config.json` loading, RAM-based TT sizing. `QuantumState` is a standalone
  reference model used by tests and the `solve` CLI, not by search.
- **rules** (`game.cpp`): piece-centric `State` (`board[12]`, `pos[8]`, `mask[8]`, owner/origin
  bitsets, side, turn, terminal, Zobrist `hash`). `apply_move_internal` runs a fixed,
  spec-mandated pipeline: collapse mask by move geometry → promote → capture (Hen→Chick) →
  place → fixed-point lineage propagation → captured-Lion disambiguation → turn++/side swap →
  terminal check (surviving delayed Try > Catch > draw at turn 256) → rehash. Do not reorder
  these steps, and do not replace delayed Try with an immediate attack test. Legal moves =
  pseudo-legal moves that survive a full `apply_move`. `Undo` is a full `State` copy.
  Propagation has two modes: `LineageLut` (default; 2^16-entry table built from all 4!
  assignments) and `PermutationReference` (slow oracle for cross-checks).
- **search** (`alpha_beta.cpp`, class `AlphaBetaEngine`): negamax alpha-beta with iterative
  deepening (soft deadline stops new depths, hard deadline aborts; only fully completed depths
  replace the result, with a legal fallback set first), PVS, aspiration windows, direct-mapped
  TT with depth/age replacement and mate-distance normalization, move ordering (TT move,
  killers, history, captures, mask-collapse, Try threat; extra root-only ordering applies
  moves). `evaluate_optimized` must stay score-identical to `evaluate_baseline` (enforced by
  `evaluation_optimization` test). Optional quiescence (`SearchOptions::qsearch_depth`, captures
  and Lion-candidate Try entries with stand-pat; default 0 in `SearchOptions`, 4 in the contest
  config). Root moves can be excluded statically (`root_excluded`) or live through `RootGuard`.
- **search/eval_features** (`extract_eval_features`, `weighted_eval`): optional linear evaluation
  (25 side-to-move-relative terms, weights x8) used when `SearchOptions::eval_v2_enabled`;
  `legacy_eval_weights()` reproduces `evaluate_optimized` exactly (enforced by `alpha_beta` test).
  Tuned weights come from offline Texel tuning (stored in `engine_config.json`).
- **search/nnue** (`nnue_features.cpp`, `nnue.cpp`): optional residual NNUE correction added to eval_v2
  (`nnue_enabled`, `nnue_path`; off by default). 1664 sparse side-to-move features, 64-32-1 float network;
  a malformed weights file leaves it unloaded. Training tools live outside the repo.
- **search/diversify** (`diversified_move`): test-only opening randomization for benchmark opponents
  (`opening_random_plies/margin/depth`); never enable it for the contest engine.
- **book/forbidden_moves** (`ForbiddenMoves`): offline-proven losing moves excluded at the root via
  `root_excluded` (`forbidden_enabled`, `forbidden_path`; off — no measured benefit). The guard merges these
  static exclusions with its own refutations.

Rationale, measurements and status of all of the above: `docs/second_seat_improvements.md`.
- **search/dfpn** (`dfpn.cpp`, `DfpnSolver`): full-width df-pn for forced Catch/Try wins (two-way
  TT preferring proven entries, 1+epsilon, wall-clock deadline, stop flag); `verify_proof`
  re-checks a proof with rule functions only (a -1 result after TT eviction is not unsoundness).
- **search/guarded_search** (`guarded_find_best_move`): alpha-beta on the calling thread plus a
  df-pn worker thread that first tries an own forced win, then refutes root moves (current best
  first) and publishes proven-losing moves as live exclusions; a ~10% reserve re-searches if the
  final choice was refuted late. The worker is joined before returning (no CPU between moves).
- **book** (`position_book.cpp`): position→move table keyed by `PositionBook::key` (the
  label-independent `CanonKey`), moves stored by squares or drop origin+mask, returned only if
  legal. Keys must be built from the protocol-normalized view (side to move is always South,
  board rotated for the second player, owner relative, origin absolute).
- **exact** (`equivalence.cpp`): `L_eq`, which groups legal moves whose successors share a
  label-independent `CanonKey` and searches one representative per class. Gated by depth,
  move count, duplicate-hand hint, duplicate ratio and low time (thresholds in
  `engine_config.json` `search.l_eq_*`, wired in `main.cpp::search_options`).
- **io** (`protocol.cpp`): parses the 20×9 observation into `State`, encodes and decodes
  actions. The external `action_mask` is used only at this boundary to validate output. Search
  must never consult it. `main.cpp::run_protocol` falls back to the first mask-allowed legal
  move if the searched move is rejected.

Invariants that tests rely on: the hash and canonical keys include `turn` and side. A state is
fully determined by its fields (no correctness-relevant external caches). `undo_move` restores
the exact state and hash. Empty masks mean an illegal branch and must never be silently
repaired. Alpha-beta itself must be deterministic (stop policy is injectable via
`SearchOptions::stop_policy`); the df-pn guard is timing-dependent by design, so tests either
disable it or use positions whose outcome does not depend on thread timing.

## Evaluating engine changes

Benchmarks in `benchmarks/` are not correctness tests. Candidate acceptance goes only through
the standardized pipeline (`evaluation/README.md`). Do not substitute ad-hoc self-play or manual
benchmarks:

```powershell
scripts\evaluate_built_candidate.bat --candidate-exe "build\current\Release\qas.exe" ^
  --candidate-name "Stage 5.1" --change-category performance_only --profile strength_candidate
```

Profiles live in `evaluation/config/evaluation_profiles.json`, thresholds in
`evaluation/config/acceptance_policy.json`. Frozen accepted binaries are owned by
`evaluation/versions/` and must not be moved into `build/`. `--change-category` must reflect
the change (e.g. `rule_change` requires a separate rule audit).
