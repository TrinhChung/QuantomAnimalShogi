# Stage 5.6 Overnight Duplicate-Hands Ceiling Reduction Audit

Date: 2026-07-10

## 1. Executive Summary

Production candidate tested: alpha-beta + TT + PVS + existing ordering + current guarded L_eq trigger + propagation LUT + optimized evaluator, with 512 MiB TT unless a TT matrix row says otherwise.

Main result: `duplicate_hands` remains the practical fixed-depth wall.

| Fixture / corpus view | 30s fixed ceiling | 60s diagnostic ceiling | Notes |
|---|---:|---:|---|
| Full 237-fixture corpus | depth 10 safe for all 237 | not run for all fixtures | 237/237 completed depth 10 in all 3 runs |
| Full corpus depth 11 | 236/237 completed all 3 runs | top hard only | only `duplicate_hands` failed depth 11 under 30s |
| Full corpus depth 12 | 231/237 completed all 3 runs | top hard mostly completed | 5 non-duplicate fixtures timed out at depth 12 under 30s |
| `duplicate_hands` | depth 10 | depth 11 | d11 median 49.98s under 60s; d12 failed 60s |
| `initial` | depth 12 | depth 12 | focused d13 failed 60s |

The next lever should be **B. Move to ordering/branching reduction**. TT size is not the ceiling factor. Current L_eq is useful, but relaxed/targeted variants did not make `duplicate_hands` depth 11 fit under 30s and carried rollback/regression risk.

## 2. Correctness Summary

Correctness guard passed before and after the benchmark-harness changes.

| Check | Result |
|---|---|
| Full CTest | 8/8 passed |
| Stage 5 reference/LUT profile check | passed |
| Shallow reference-vs-LUT search equality | passed for depth 1, 2, 3 |
| Focus fixtures | `initial`, `duplicate_hands`, `high_uncertainty_midgame`, `high_branching_hand_stack`, `random_ply4_18`, `random_hand_pieces_11`, `random_duplicate_hands_02` |
| Equality fields | score, best move, node count, legal best move |
| Illegal move count | 0 across 4,726 benchmark result rows |
| Timeout handling | 521 timed-out rows; none counted as completed fixed-depth rows |

Raw logs:

- `local_reports/stage56_logs/ctest.log`
- `local_reports/stage56_logs/ctest_final.log`
- `local_reports/stage56_logs/stage5_profile_depth3.log`
- `local_reports/stage56_validate/shallow_validation.csv`
- `local_reports/stage56_validate_final/shallow_validation.csv`

## 3. Harness Changes

Only benchmark code changed in `benchmarks/stage55_ceiling.cpp`.

- Added explicit `validate` mode for shallow PermutationReference vs LineageLut search equality.
- Added `--fixtures`, `--shard-count`, and `--shard-index` for safe parallel overnight jobs.
- Added L_eq experiment modes: `target_a`, `target_b`, `target_c`, and `tactical_rep`.
- Expanded CSV output with TT move rates, canonicalization timing, undo/hash timing, evaluator subcomponent timing, L_eq saved-child estimate, and ordering diagnostics.
- Added `estimated_exclusive_*` timing columns derived from existing counters.

No production search default, rule logic, propagation semantics, evaluator weight, move encoding, or default L_eq trigger was changed. True nested exclusive timers were not added because that would require more invasive timer plumbing through nested rule/eval/search calls; the report uses inclusive timings as authoritative and labels derived estimates explicitly.

## 4. Full-Corpus Fixed-Depth Results

Raw CSVs and summaries:

- `local_reports/stage56_fixed_full30/shard*/fixed_depth.csv`
- `local_reports/stage56_fixed_full30_d11plus/shard*/fixed_depth.csv`
- `local_reports/stage56_analysis/fixed_depth_merged.csv`
- `local_reports/stage56_analysis/fixed_depth_summary.csv`
- `local_reports/stage56_analysis/top20_hard_fixed.csv`

Top hard fixtures by 30s fixed-depth ceiling:

| Fixture | Max 30s depth | d10 ms | d11 ms/timeout | d11 completed | d11 nodes | d11 first cutoff | d11 avg cutoff rank |
|---|---:|---:|---:|---:|---:|---:|---:|
| `duplicate_hands` | 10 | 6,531 | 30,000 | 0/3 | 7,665,280 partial | 0.675 | 1.763 |
| `random_hand_pieces_00` | 11 | 4,333 | 16,536 | 3/3 | 7,403,880 | 0.937 | 1.114 |
| `random_ply4_18` | 11 | 7,811 | 27,999 | 3/3 | 7,295,329 | 0.772 | 1.510 |
| `random_high_uncertainty_09` | 11 | 4,193 | 10,781 | 3/3 | 3,589,496 | 0.795 | 1.456 |
| `many_lion_candidates` | 11 | 3,143 | 10,415 | 3/3 | 3,180,069 | 0.779 | 1.505 |

No generated random fixture overtook `duplicate_hands` for the 30s fixed-depth wall. The closest random fixture was `random_ply4_18`, which completed depth 11 in 27.999s median but timed out at depth 12 under 30s.

60s top-hard diagnostic:

| Fixture | Depth | Completed | Median ms | Median nodes |
|---|---:|---:|---:|---:|
| `duplicate_hands` | 11 | 3/3 | 49,981 | 12,895,640 |
| `duplicate_hands` | 12 | 0/3 | 60,000 | 13,628,608 partial |
| `random_ply4_18` | 12 | 3/3 | 52,649 | 15,200,420 |
| `random_hand_pieces_00` | 12 | 3/3 | 36,705 | 11,689,029 |
| `many_lion_candidates` | 12 | 3/3 | 39,766 | 10,567,325 |

Focused depth 13:

| Fixture | d13 60s completed | Elapsed ms |
|---|---:|---:|
| `initial` | no | 60,000 |
| `high_uncertainty_midgame` | yes | 8,324 |
| `many_hands` | yes | 6,658 |
| `maximal_uncertainty_midgame` | yes | 24,915 |

## 5. Iterative-Deepening Results

Raw CSVs:

- `local_reports/stage56_iterative_top20_30s/part*/iterative.csv`
- `local_reports/stage56_iterative_top5_60s/iterative.csv`
- `local_reports/stage56_analysis/iterative_summary.csv`

Median last completed depths:

| Fixture | 1s | 3s | 5s | 10s | 20s | 30s | 60s |
|---|---:|---:|---:|---:|---:|---:|---:|
| `duplicate_hands` | 7 | 8 | 9 | 9 | 10 | 11 | 11 |
| `initial` | 9 | 10 | 10 | 11 | 12 | 12 | not run |
| `random_ply4_18` | 8 | 10 | 10 | 10 | 11 | 11 | 12 |
| `random_hand_pieces_00` | 7 | 7 | 7 | 7 | 7 | 7 | 7 |

`random_hand_pieces_00` is a raw low-depth iterative row, but it terminates quickly around 0.3s due mate-score/early convergence. It is not a timeout wall. The time-consuming contest wall remains `duplicate_hands`.

Depth 10 is safe on this corpus. Depth 11 is fixture-dependent: iterative deepening can report depth 11 on `duplicate_hands` at 30s with TT warmed by prior depths, but fixed depth 11 from a cold table does not complete under 30s.

## 6. L_eq Trigger Experiment

Raw CSVs:

- `local_reports/stage56_leq_broad/part*/leq_matrix.csv`
- `local_reports/stage56_leq_duplicate_3run/leq_matrix.csv`
- `local_reports/stage56_analysis/leq_merged.csv`
- `local_reports/stage56_analysis/leq_duplicate_3run_summary.csv`
- `local_reports/stage56_analysis/leq_broad_d10_summary.csv`

`duplicate_hands` fixed depth 10, 3-run medians:

| Mode | Completed | ms | Nodes | Delta vs off | L_eq ms | Dup ratio | Rollbacks |
|---|---:|---:|---:|---:|---:|---:|---:|
| off | 3/3 | 7,844 | 1,731,515 | baseline | 0 | 0.000 | 0 |
| current | 3/3 | 6,308 | 1,324,683 | -19.6% time, -23.5% nodes | 17 | 0.258 | 307 |
| relaxed | 3/3 | 5,039 | 1,050,081 | -35.8% time, -39.4% nodes | 385 | 0.047 | 34,651 |
| strict | 3/3 | 7,999 | 1,731,515 | regression | 0 | 0.216 | 17 |
| target_a | 3/3 | 6,296 | 1,324,683 | about current | 36 | 0.123 | 1,534 |
| target_b | 3/3 | 7,495 | 1,627,357 | worse than current | 34 | 0.238 | 0 |
| target_c | 3/3 | 6,237 | 1,308,969 | -1.1% vs current time | 35 | 0.129 | 1,188 |
| tactical_rep | 3/3 | 6,297 | 1,324,683 | about current | 20 | 0.258 | 307 |

`duplicate_hands` fixed depth 11 under 30s:

| Mode | Completed | Median nodes before timeout | L_eq ms | Rollbacks | Result |
|---|---:|---:|---:|---:|---|
| off | 0/3 | 8,259,520 | 0 | 0 | timeout |
| current | 0/3 | 7,934,400 | 41 | 908 | timeout |
| relaxed | 0/3 | 7,674,176 | 1,544 | 118,716 | timeout |
| target_c | 0/3 | 8,220,096 | 129 | 6,224 | timeout |
| tactical_rep | 0/3 | 7,959,744 | 48 | 908 | timeout |

Regression checks at fixed depth 10:

| Fixture | off ms | current ms | relaxed ms | Notes |
|---|---:|---:|---:|---|
| `initial` | 1,227 | 1,253 | 1,385 | relaxed rollback-heavy |
| `high_uncertainty_midgame` | 674 | 687 | 730 | same score/best, time regression |
| `many_hands` | 1,337 | 1,357 | 1,463 | no useful duplicate reduction |
| `high_branching_hand_stack` | 91 | 68 | 87 | current helps here |

Recommendation: keep the current L_eq default. Relaxed L_eq is promising on `duplicate_hands` depth 10, but it did not make depth 11 complete under 30s and it has high rollback overhead plus broad regression risk. Tactical representative selection did not improve nodes, cutoff quality, or time.

## 7. Ordering Diagnosis

`duplicate_hands` ordering quality degrades at the depth-11 wall:

| Depth | Median nodes | Node growth | First-move cutoff | Avg cutoff rank | Legal avg |
|---:|---:|---:|---:|---:|---:|
| 9 | 1,094,846 | 7.86x from d8 | 0.709 | 1.589 | 18.87 |
| 10 | 1,324,683 | 1.21x from d9 | 0.760 | 1.386 | 19.82 |
| 11 30s partial | 7,665,280 | 5.79x from d10 partial comparison | 0.675 | 1.763 | 18.24 |
| 11 60s complete | 12,895,640 | 9.73x from d10 complete nodes | not in summary table | not in summary table | not in summary table |

This is not a total ordering collapse, but the drop from 76.0% first-move cutoffs to 67.5% and the rank increase from 1.39 to 1.76 are large enough to explain much of the depth-11 cliff. Hand/drop and duplicate-successor neighborhoods should be inspected next. The current diagnostics do not isolate a single least-useful heuristic; optional ordering experiments were not made production defaults.

## 8. Per-Node Cost Audit

`duplicate_hands` fixed depth 11, 30s partial median:

| Component | Inclusive / estimated ms | Notes |
|---|---:|---|
| eval | 12,983 | largest single measured hot path |
| apply_move inclusive | 13,182 | overlaps propagation, terminal, hash |
| movegen inclusive | 11,425 | includes legal filtering |
| legal_filter inclusive | 10,911 | apply-heavy |
| propagation | 4,354 | LUT path, no longer main wall |
| terminal check | 2,549 | Try/Catch still material |
| ordering | 1,048 | not dominant in wall-clock cost |
| L_eq grouping | 43 | current trigger overhead is small |
| hash recompute | 1,119 | included in apply pipeline |

Derived exclusive estimates for the same row:

| Estimated exclusive component | ms |
|---|---:|
| eval total | 12,983 |
| apply excluding propagation/terminal/hash | 5,162 |
| propagation | 4,354 |
| terminal | 2,549 |
| legal filter loop overhead | 986 |
| pseudo move generation | 371 |

Evaluator subcomponents on the same row:

| Eval subcomponent | ms |
|---|---:|
| immediate transition | 5,840 |
| immediate movegen | 2,203 |
| immediate filter | 1,074 |
| mobility | 582 |
| immediate setup | 327 |
| lion safety | 300 |
| material/mask | 158 |
| terminal | 133 |

Per-node optimization remains valuable, but it is unlikely to close `duplicate_hands` fixed depth 11 alone. The 60s completed fixed-depth d11 median is 49.98s; fitting under 30s needs roughly a 40% total reduction. A 20% to 30% cut in one component would not be enough unless paired with node reduction.

## 9. TT Sanity

Raw CSV:

- `local_reports/stage56_tt_hard_subset/tt_matrix.csv`
- `local_reports/stage56_analysis/tt_hard_summary.csv`

`duplicate_hands` fixed-depth TT matrix:

| TT MiB | d10 ms | d11 completed | d11 nodes partial | TT hit d11 | Replacements d11 | Peak RSS MiB |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 6,455 | no | 7,989,376 | 0.146 | 395,716 | 53 |
| 128 | 6,248 | no | 8,015,808 | 0.149 | 252,588 | 101 |
| 256 | 6,296 | no | 7,938,816 | 0.153 | 151,468 | 197 |
| 512 | 6,291 | no | 7,936,256 | 0.154 | 102,162 | 389 |
| 1024 | 6,294 | no | 7,957,440 | 0.151 | 73,540 | 774 |

TT size is not the current ceiling factor. Larger tables reduce replacements but do not complete depth 11 under 30s and do not materially improve elapsed time. Recommended contest-safe TT remains 512 MiB when memory allows; 256 MiB is a reasonable lower-memory fallback. 1024 MiB has no clear benefit here. 2048 MiB was not run because the machine had about 16 GiB RAM and only about 2.5 GiB free when long jobs started.

## 10. Decision

Decision: **B. Move to ordering/branching reduction.**

Reasoning:

- `duplicate_hands` fixed depth 11 needs about 40% total improvement to fit under 30s.
- TT size does not move the wall.
- Current L_eq helps, but no tested trigger or representative variant safely closes depth 11.
- Per-node costs are still high, especially eval immediate-win transition and apply/terminal/hash, but single-component optimization alone is unlikely to close the gap.
- Ordering quality degrades meaningfully at the wall, and node growth from d10 to completed d11 is the dominant failure mode.

## 11. Next Prompt Recommendation

Next task should be a Stage 5.7 ordering/branching audit focused on `duplicate_hands` depth 11.

Recommended prompt:

```text
Run a Stage 5.7 duplicate_hands ordering and branching reduction audit.
Do not change rules, evaluator weights, propagation, protocol encoding, or production defaults.
Use Stage 5.6 data as baseline.

Primary target:
- Reduce duplicate_hands fixed depth 11 from ~49.98s local 60s completion toward <30s.

Required work:
1. Add root and interior move-order diagnostics by depth and ply:
   - cutoff rank histograms by ply
   - hand/drop move ranks
   - TT move availability and cutoff rate by ply
   - immediate-win/prevent-loss/capture/collapse/lion-reduction contribution counts
   - root static score vs searched score correlation
2. Run flagged experiments only:
   - hand/drop ordering boost
   - immediate defense boost
   - lion-candidate reduction boost
   - capture/collapse rebalance
   - L_eq tactical representative only if grouping semantics stay unchanged
3. For every experiment:
   - keep production default unchanged
   - compare fixed depth 10 and 11 on duplicate_hands
   - compare initial, high_uncertainty_midgame, random_ply4_18, random_hand_pieces_00
   - verify score/best stability where expected, legal best move, illegal_move_count=0
   - report nodes, elapsed, first-move cutoff, avg cutoff rank, and component timings

Decision:
- Adopt no default unless it improves duplicate_hands depth 11 without broad regressions.
- If no ordering experiment reduces nodes materially, return to per-node eval/apply optimization.
```

