# Stage 5.5 Current Ceiling Discovery

Date: 2026-07-09

## 1. Executive Summary

Production candidate tested:

- Alpha-beta/PVS with TT, existing move ordering, killer/history, current guarded L_eq trigger,
  propagation LUT, optimized evaluator.
- TT size for primary ceiling pass: 512 MiB requested table budget.
- Benchmark instrumentation enabled, so absolute elapsed time includes profiling overhead.

Current practical ceiling from completed-depth sweeps:

- Worst important fixture: `duplicate_hands`.
- Fixed-depth 30s ceiling on `duplicate_hands`: depth 10.
- Fixed-depth 60s local diagnostic ceiling on `duplicate_hands`: still depth 10; depth 11 timed
  out at 60s.
- Typical non-duplicate focused fixtures complete depth 12 or depth 13 under 30s.
- `initial` completes fixed depth 12 under 30s, but depth 13 fails under 60s.
- Iterative 30s median on the top-three ceiling fixtures:
  - `duplicate_hands`: depth 10.
  - `initial`: depth 12.
  - `high_uncertainty_midgame`: depth 10, because search reaches a mate-score stop condition.

Depth 10 is safe for the measured important fixtures. Depth 11 is not stable because
`duplicate_hands` fails. Depth 12 is realistic for many fixtures but not for duplicate-heavy hands.

Important scope note: the full generated corpus was screened at depth 8, and the top-three ceiling
fixtures were measured with 3-run medians. The full 237-fixture corpus was not run as a 3-run
depth-8..13 sweep because that is now a multi-hour offline job. The suite supports it, but this
report does not claim a full-corpus median ceiling beyond the completed runs listed here.

## 2. Correctness Summary

Commands:

```powershell
cmake --build build_stage5 --config Release --parallel
ctest --test-dir build_stage5 -C Release --output-on-failure
build_stage5\Release\qas_stage5_profile.exe 3
```

Results:

- Full CTest: 8/8 passed.
- Focused reference/LUT validation depth 1..3: score, node count, best move, and legal best move
  matched on all focused fixtures.
- Exhaustive propagation LUT/reference coverage remains in `game_rules_test.cpp`.
- Reachable-state propagation and legal-list equivalence test remains in `game_rules_test.cpp`.
- Illegal root best move count across all Stage 5.5 CSV rows generated in this pass: 0.
- Timed fixed-depth rows use `completed=false` when `depth_reached < requested_depth`; partial
  searches are not counted as completed depths.

## 3. Corpus

The Stage 5.5 harness generated 237 deterministic fixtures under:

```text
local_reports/stage55/
```

Each fixture file records name, group, seed, turn, side to move, hash, pseudo/legal move count,
uncertainty, lion candidates, hand count, duplicate-hand pairs, near-terminal flag, and full state.

Required fixed fixtures present:

```text
initial, duplicate_hands, high_uncertainty_midgame, many_hands,
low_uncertainty_midgame, near_try, near_catch, safe_4ply, safe_8ply,
near_draw, high_branching_hand_stack, many_duplicate_hands,
many_lion_candidates, forced_try_defense, forced_catch_defense,
low_uncertainty_endgame, maximal_uncertainty_midgame
```

Random groups present:

```text
random_ply4, random_ply8, random_ply16, random_ply32,
random_hand_pieces, random_duplicate_hands, random_high_uncertainty,
random_low_uncertainty, random_near_terminal, random_near_draw
```

The depth-8 full-corpus screening found no timeout at a 5s cap. The slowest depth-8 row was
`random_ply4_18` at 1064.4 ms, followed by `duplicate_hands` at 875.3 ms.

## 4. Fixed-Depth Ceiling

Single-run important fixture sweep, with deeper 60s diagnostic used to classify rows that were not
tested to depth 13 in the 30s pass:

| fixture | max_depth_under_30s | max_depth_under_60s | observed failure |
|---|---:|---:|---|
| initial | 12 | 12 | depth 13 timed out at 60s |
| duplicate_hands | 10 | 10 | depth 11 timed out at 30s and 60s |
| high_uncertainty_midgame | 13+ | 13+ | sweep ended at completed depth 13 |
| many_hands | 13+ | 13+ | sweep ended at completed depth 13 |
| near_try | 13+ | 13+ | terminal/mate dominated, easy |
| near_catch | 13+ | 13+ | terminal/mate dominated, easy |
| maximal_uncertainty_midgame | 13 | 13+ | depth 13 completed in 29.8s |
| high_branching_hand_stack | 13+ | 13+ | terminal/mate dominated, easy |
| many_duplicate_hands | 13+ | 13+ | terminal/mate dominated, easy |
| forced_try_defense | 13+ | 13+ | terminal/mate dominated, easy |

Top-three 30s fixed-depth medians:

| fixture | depth | median_ms | completed_runs | median_nodes | first_move_cutoff | avg_cutoff_rank | legal_avg |
|---|---:|---:|---:|---:|---:|---:|---:|
| duplicate_hands | 8 | 690.8 | 3/3 | 139226 | 0.874 | 1.24 | 19.87 |
| duplicate_hands | 9 | 5341.3 | 3/3 | 1094846 | 0.709 | 1.59 | 18.87 |
| duplicate_hands | 10 | 7676.7 | 3/3 | 1324683 | 0.760 | 1.39 | 19.82 |
| duplicate_hands | 11 | 30000.2 | 0/3 | 6826880 searched | 0.674 | 1.77 | 18.13 |
| initial | 8 | 229.7 | 3/3 | 55607 | 0.813 | 1.40 | 17.09 |
| initial | 9 | 609.1 | 3/3 | 165835 | 0.779 | 1.51 | 15.67 |
| initial | 10 | 1394.2 | 3/3 | 335227 | 0.813 | 1.37 | 17.36 |
| initial | 11 | 10474.4 | 3/3 | 2659918 | 0.713 | 1.62 | 16.68 |
| high_uncertainty_midgame | 8 | 170.3 | 3/3 | 49200 | 0.823 | 1.41 | 14.39 |
| high_uncertainty_midgame | 9 | 387.3 | 3/3 | 94396 | 0.882 | 1.23 | 14.89 |
| high_uncertainty_midgame | 10 | 759.6 | 3/3 | 240661 | 0.862 | 1.32 | 14.72 |
| high_uncertainty_midgame | 11 | 1531.0 | 3/3 | 395163 | 0.949 | 1.11 | 14.88 |

Node growth signals:

- `duplicate_hands`: depth 8->9 grows 7.86x; depth 9->10 grows 1.21x; depth 10->11 is already
  5.15x at timeout and incomplete.
- `initial`: depth 10->11 grows 7.94x and elapsed jumps to 10.5s.
- `high_uncertainty_midgame`: depth 10->11 grows 1.64x and remains easy.

## 5. Iterative-Deepening Ceiling

Top-three 3-run medians:

| fixture | 1s | 3s | 5s | 10s | 20s | 30s |
|---|---:|---:|---:|---:|---:|---:|
| duplicate_hands | 7 | 8 | 9 | 9 | 10 | 10 |
| initial | 9 | 10 | 10 | 11 | 12 | 12 |
| high_uncertainty_midgame | 10 | 10 | 10 | 10 | 10 | 10 |

Single-run important fixture observations:

- `maximal_uncertainty_midgame`: 5s depth 12, 10s depth 13, 30s depth 14.
- `many_hands`: reports depth 6 because mate-score termination stops iterative deepening early.
- `near_try`, `near_catch`, `high_branching_hand_stack`, `many_duplicate_hands`,
  `forced_try_defense`: report depth 1 because immediate decisive lines stop the search.

Conclusion: depth 10 is guaranteed for the top-three measured 30s cases except immediate mate
positions where search intentionally stops earlier. Depth 11 is fixture-dependent and not contest
safe due to `duplicate_hands`.

## 6. Top Bottlenecks

Hardest important row: `duplicate_hands` depth 11 timeout.

Median component profile at timeout:

| component | ms |
|---|---:|
| elapsed | 30000.2 |
| searched nodes | 6826880 |
| movegen total | 11278.9 |
| legal filter | 10773.9 |
| apply inclusive | 12988.1 |
| propagation | 4550.4 |
| terminal checks | 2505.2 |
| eval | 13083.5 |
| ordering | 1082.0 |
| L_eq | 44.7 |

Primary limiting factor: node explosion multiplied by high per-node transition/evaluation cost.
Move ordering degrades in the failing row, but it does not collapse catastrophically: median cutoff
rank remains 1.77 and first-move cutoff rate is 67.4%.

Full-corpus depth-8 screen, slowest rows:

| fixture | group | ms | nodes | legal_avg | first_move_cutoff | avg_cutoff_rank |
|---|---|---:|---:|---:|---:|---:|
| random_ply4_18 | random_ply4 | 1064.4 | 274429 | 17.5 | 0.81 | 1.39 |
| duplicate_hands | required | 875.3 | 139226 | 19.9 | 0.87 | 1.24 |
| random_hand_pieces_11 | random_hand_pieces | 782.8 | 315151 | 17.4 | 0.86 | 1.26 |
| random_hand_pieces_00 | random_hand_pieces | 733.3 | 182851 | 17.7 | 0.91 | 1.16 |
| random_hand_pieces_16 | random_hand_pieces | 635.9 | 168070 | 17.8 | 0.86 | 1.20 |
| many_lion_candidates | required | 531.0 | 119735 | 17.6 | 0.85 | 1.32 |
| random_duplicate_hands_02 | random_duplicate_hands | 494.5 | 142275 | 16.6 | 0.83 | 1.38 |
| random_hands_10 | random_hands | 359.3 | 116993 | 19.3 | 0.79 | 1.59 |
| random_ply4_10 | random_ply4 | 349.1 | 83441 | 16.3 | 0.88 | 1.25 |
| random_duplicate_hands_19 | random_duplicate_hands | 339.9 | 93029 | 15.0 | 0.97 | 1.04 |

## 7. TT Size Analysis

Reduced matrix on `initial`, `duplicate_hands`, and `high_uncertainty_midgame`:

| fixture | depth | best TT in tested row | best_ms | completed | tt_hit_rate |
|---|---:|---:|---:|---:|---:|
| duplicate_hands | 10 | 256 MiB | 7040.8 | yes | 0.118 |
| duplicate_hands | 11 | 256 MiB | 30000.0 | no | 0.156 |
| initial | 10 | 1024 MiB | 1302.5 | yes | 0.150 |
| initial | 11 | 64 MiB | 10046.7 | yes | 0.117 |
| high_uncertainty_midgame | 10 | 512 MiB | 719.0 | yes | 0.171 |
| high_uncertainty_midgame | 11 | 256 MiB | 1518.0 | yes | 0.160 |

TT size is not the current ceiling factor. Increasing above 256 MiB is not consistently beneficial
and does not make `duplicate_hands` depth 11 complete. Recommended:

- contest_safe: 256 MiB to 512 MiB.
- local_benchmark: 512 MiB is fine, but 1024 MiB is not justified by this matrix.
- diminishing return point: around 256 MiB for these tests.

## 8. L_eq Analysis

Depth-10 L_eq trigger matrix:

| fixture | mode | elapsed_ms | nodes | duplicate_ratio | L_eq_ms | rollback_count | delta_vs_off |
|---|---|---:|---:|---:|---:|---:|---:|
| duplicate_hands | off | 9561.3 | 1731515 | 0.000 | 0.0 | 0 | baseline |
| duplicate_hands | current | 7716.5 | 1324683 | 0.258 | 22.8 | 307 | -19.3% |
| duplicate_hands | relaxed | 5702.2 | 1050081 | 0.047 | 444.7 | 34651 | -40.4% |
| duplicate_hands | strict | 8892.1 | 1731515 | 0.216 | 0.4 | 17 | -7.0% |
| many_hands | current | 1494.7 | 624534 | 0.000 | 0.0 | 0 | +3.2% |
| high_branching_hand_stack | current | 71.5 | 12545 | 0.301 | 0.5 | 3 | -27.0% |

Interpretation:

- Current L_eq is meaningful on `duplicate_hands` and `high_branching_hand_stack`.
- Relaxed L_eq may be promising on `duplicate_hands`, but the rollback count and overhead are high
  and it hurts unrelated rows. Do not make it default from this single pass.
- There is enough redundancy to justify a later L_eq-trigger experiment, but not enough evidence to
  open EAQS-lite or full EAQS now.

## 9. Ordering Analysis

Ordering quality is mixed:

- `high_uncertainty_midgame` depth 11: first-move cutoff 94.9%, avg cutoff rank 1.11.
- `duplicate_hands` depth 11 timeout: first-move cutoff 67.4%, avg cutoff rank 1.77.
- `initial` depth 11: first-move cutoff 71.3%, avg cutoff rank 1.62.

The failing rows show ordering degradation, but the component profile still spends most time in
movegen/legal filtering/apply/eval. Ordering should be diagnosed further, but the next accepted
optimization should still reduce per-node cost unless a dedicated ordering experiment proves a
large node-count reduction.

## 10. Component Profile At Ceiling

For `duplicate_hands` depth 11 timeout:

- Node explosion is active: >6.8M nodes searched before timeout.
- Per-node costs dominate elapsed time:
  - eval 13.1s,
  - apply inclusive 13.0s,
  - movegen/legal filtering 11.3s/10.8s,
  - propagation 4.6s,
  - terminal checks 2.5s.
- TT is helpful but insufficient:
  - tt hit rate 16.2%,
  - tt cutoff rate 13.6%,
  - replacements 87.6k,
  - collisions 43.9k.
- L_eq overhead is low in current mode, but current trigger does not reduce enough depth-11 work.

## 11. Decision

Decision: **A. Continue per-node classical optimization.**

Reason: the ceiling row is dominated by apply/legal filtering/evaluation cost once node growth
pushes `duplicate_hands` above 6M nodes. Ordering and L_eq matter, but the measured component wall
is still per-node classical cost. Do not open full EAQS. Do not open EAQS-lite yet.

## 12. Next Prompt Recommendation

Next prompt:

```text
Perform Stage 5.6 per-node transition-cost optimization.
Use duplicate_hands depth 10/11, initial depth 11, and random_ply4_18 depth 8 as ceiling fixtures.
Do not change rules, evaluator weights/features, move ordering defaults, or L_eq defaults.
Measure and optimize only low-risk per-node costs:
- legal filtering state copy/restore overhead,
- apply_move validation and capture/promotion hot path,
- terminal Try safety reply cost,
- hash recompute cost,
- evaluator immediate-win transition cost.
Keep each optimization behind an option unless it is a pure refactor with exhaustive equivalence.
Require CTest, propagation/reference equivalence, legal-list equivalence, apply/undo/hash tests,
and fixed-depth score/best/node equality where ordering is unchanged.
Report before/after depth-10 and depth-11 component timings and whether duplicate_hands depth 11
gets closer to 30s.
```
