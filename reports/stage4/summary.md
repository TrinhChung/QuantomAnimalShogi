# Stage 4 - Evaluation Hot Path Optimization

## Result

Stage 4 is accepted. The optimized evaluator preserves the existing search
semantics and exceeds both performance targets on the focused depth-10 suite.
All figures below are seven-run medians with TT requested at 256 MiB and the
production L_eq trigger fixed at `L>=24`, `depth>=4`, and duplicate ratio
`>=0.25`.

- Aggregate eval time: 18,653.747 ms -> 3,825.968 ms (-79.5%).
- Aggregate elapsed time: 26,559.851 ms -> 11,478.892 ms (-56.8%).
- `duplicate_hands` elapsed time: 14,695.661 ms -> 7,136.383 ms (-51.4%).
- No focused position slowed down; best move, score, nodes, TT metrics, and
  cutoff metrics were identical before and after.

## What changed

The old evaluator remains available through `SearchOptions::optimized_eval_enabled`
for same-binary A/B checks. The production path now:

- reuses a fixed-size `EvalScratch` instead of allocating move vectors per leaf;
- uses precomputed 32-entry mask popcount and expected-value tables;
- builds side occupancy once and computes mobility with 12-bit masks;
- builds attack and Lion-candidate summaries in one piece scan;
- generates immediate-win pseudo moves into reused storage;
- simulates only moves that can possibly Catch a Lion or Try on the back rank;
- restores successful transitions with undo instead of copying and rehashing the
  complete state after every attempted move.

The immediate-win filter is a necessary-condition filter, not a heuristic:
Catch still requires a Lion-bearing target and Try still requires a Lion-bearing
mover reaching the back rank. Every retained candidate is passed through the
unchanged rule transition and propagation code.

The current evaluator has no independent general attack/threat or hand-potential
term: attack data is part of Lion safety, hand/drop mobility is part of mobility,
and Catch/Try threats are the immediate-win components. Stage 4 did not add or
reweight any feature.

## Eval component profile

The component totals are sums of the five per-position median rows. Percentages
are relative to instrumented eval time; residual time includes function/control
overhead and nested timer overhead.

| Component | Before ms | After ms | Main effect |
|---|---:|---:|---|
| Terminal | 54.099 | 52.115 | unchanged semantics |
| Material/mask | 122.837 | 58.389 | lookup tables |
| Mobility | 314.109 | 166.809 | occupancy bitmasks |
| Lion safety/attacks | 220.027 | 95.935 | one attack-mask scan |
| Immediate setup | 1,834.710 | 128.770 | no per-move reset/rehash |
| Immediate movegen | 570.665 | 412.620 | reused vectors |
| Candidate filter | 0.000 | 221.453 | new exact prefilter cost |
| Immediate transitions | 11,521.100 | 1,874.435 | 37.35M -> 3.04M calls |
| Unattributed eval overhead | 4,016.200 | 815.442 | fewer allocations/copies |

Raw profiles: [before](eval_component_profile_before.csv) and
[after](eval_component_profile_after.csv).

## Focused A/B benchmark

| Position | Before ms | After ms | Total delta | Eval delta | Nodes | Move / score |
|---|---:|---:|---:|---:|---:|---|
| initial | 3,339.124 | 1,364.779 | -59.1% | -80.7% | 335,244 | `p3:7->4` / -212 |
| duplicate_hands | 14,695.661 | 7,136.383 | -51.4% | -75.5% | 1,323,724 | `p3:7->4` / -53 |
| high_uncertainty_midgame | 4,714.951 | 1,330.412 | -71.8% | -85.1% | 486,114 | `p5:1->2` / -999992 |
| many_hands | 2,356.094 | 1,224.176 | -48.0% | -84.0% | 437,073 | `p5:4->1` / 999991 |
| low_uncertainty_midgame | 1,454.021 | 423.142 | -70.9% | -87.3% | 174,686 | `p5:0->3` / -668 |

Full median rows: [before](focused_benchmark_before.csv) and
[after](focused_benchmark_after.csv). The raw seven-run CSVs remain in the parent
`reports` directory for repeat/min/max audit.

## Correctness

- CTest: 8/8 passed, including quantum state, rules/propagation, apply/undo/hash,
  protocol/action checks, TT/PVS timeout behavior, L_eq legality, deterministic
  fixtures, and the new evaluator behavior test.
- Focused fixtures at depths 1, 2, and 3: exact score and node-count equality;
  both modes returned legal moves. Equal-score move ties are explicitly accepted
  by the test, though none appeared in the final depth-10 data.
- Depth 10: exact best-move, root-score, node-count, TT-rate, and cutoff equality
  on all five positions; illegal move count remained zero.
- All final seven-run groups were stable and below the 20% noise threshold.

## Optional depth-11 probe

One optimized run was made only after the depth-10 acceptance targets passed:

- `initial`: completed in 10,191.935 ms.
- `high_uncertainty_midgame`: completed in 3,799.009 ms.
- `duplicate_hands`: timed out at 30,000.272 ms; its partial move/score is not a
  completed-depth result and is discarded.

## Risk assessment

The main risk is equivalence of the immediate Catch/Try prefilter on unusual
states. It is bounded by the necessary-condition argument above and by shallow
A/B, full depth-10 tree equality, and existing rule tests. The baseline path is
retained as an independent runtime fallback for future differential checks.
Memory use is unchanged at about 197.6 MiB RSS in the benchmark profile.

## Decision and next stage

Proceed with **B. focused move-generation optimization**, followed by
`duplicate_hands` ordering analysis if that fixture still fails depth 11. Eval is
no longer the dominant cost on most focused positions; move generation is now a
larger visible share, while `duplicate_hands` still has 1.32M nodes at depth 10.
Do not open the quotient/assignment-set branch yet: first reduce remaining
movegen cost and determine whether the stress case is dominated by branching or
per-node rule work.

## Stage 4 changed source files

- `include/search/alpha_beta.hpp`
- `src/search/alpha_beta.cpp`
- `benchmarks/stage35_benchmark.cpp`
- `tests/evaluation_optimization_test.cpp`
- `CMakeLists.txt`
- `README.md`
- `docs/architecture.md`
- `docs/stage3-recommendation.md`
- `reports/stage4/*`
