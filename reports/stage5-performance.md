# Stage 5 Classical Performance Audit

Date: 2026-07-09

## 1. Executive Summary

The measured bottleneck was rules transition cost inside legal filtering, dominated by fixed-point
identity propagation. The Stage 5 branch keeps the search graph, rules, protocol encoding, evaluator
features, and L_eq policy unchanged, and uses the exact lineage propagation LUT path with the
24-permutation implementation retained as `PropagationMode::PermutationReference`.

Depth-8, 3-run median over the focused seven-position set:

- Reference propagation total: 3002.8 ms, 390608 nodes.
- LUT propagation total: 1586.8 ms, 390608 nodes.
- Total time reduction: 47.2% (1.89x faster).
- Propagation time reduction: 1698.5 ms to 308.1 ms, 81.9% lower.
- Scores, best moves, and node counts were identical for reference vs LUT in every depth-8 row.

Decision: continue classical optimization. Do not open full EAQS yet.

## 2. Baseline Profile And Hot Path

Observed call chain:

1. `AlphaBetaEngine::find_best_move` calls `search_root`, then recursive `negamax`.
2. `generate_search_moves` calls `generate_legal_moves`.
3. `generate_legal_moves` calls `generate_pseudo_legal_moves`, then filters by `apply_move`.
4. `order_moves` scores legal moves with TT/killer/history/capture/mask/try features and root-only
   tactical previews.
5. `apply_move` validates source/owner/target, performs local collapse, capture,
   promotion/demotion, turn/side update, propagation, terminal detection, and hash recompute.
6. `propagate` reaches a fixed point by filtering each origin side against all-different lineage
   support.
7. Terminal detection preserves Catch, Try, Draw, and Illegal behavior.
8. Hash content still covers piece position, mask, owner, origin, side, turn, terminal, and winner.
9. L_eq remains optional and only runs behind its configured reducer trigger.

Depth-8 baseline with reference propagation spent most time in movegen/legal filtering. Across the
focused set, movegen/legal filtering consumed 2101.5 ms of 3002.8 ms; propagation alone consumed
1698.5 ms.

## 3. Bottleneck Diagnosis

The data rejected legal-precheck as a primary target: focused positions generated very few pseudo
moves that failed filtering. Most pseudo moves were retained, so a reject-only precheck would not
remove enough `apply_move` calls.

Propagation was the high-confidence target:

- Each legal filtering candidate reaches `apply_move`.
- `apply_move` calls propagation before terminal/hash completion.
- The old origin-side filter checked the same 24 lineage permutations repeatedly.
- Propagation was 56.6% of total instrumented reference time at depth 8.

## 4. Chosen Optimization And Rejected Alternatives

Chosen optimization: exact origin-side propagation LUT.

- Input: four lineage masks, each 0..15.
- Size: `16^4 = 65536` entries.
- Build rule: same 24-permutation all-different semantics as the reference path.
- Output: supported lineage masks for each of the four origin pieces.
- Fallback: `PropagationMode::PermutationReference`.
- Default optimized path: `PropagationMode::LineageLut`.

Rejected for this patch:

- Legal filtering precheck: pseudo reject rates were low.
- Apply-move refactor: higher semantic risk around Catch/Try/capture ordering.
- Terminal fast path: not the dominant measured cost after the LUT.
- Move ordering changes: would alter node counts and make this no longer a pure per-node hot-path
  optimization.
- Evaluator changes: explicitly out of scope for Stage 5.

## 5. Semantic Safety

The LUT does not infer from Zobrist keys and does not change public move/action encoding. It stores
only the exact support set produced by the old 24-permutation relation. `apply_move` still performs
the same validation, mutation order, propagation, terminal checks, and full hash recompute.

Additional instrumentation is passive:

- `SearchOptions::benchmark_instrumentation_enabled` remains false by default.
- Rules metrics are only populated through profiled calls.
- The profile executable is separate from the contest binary.

## 6. Files Changed

- `include/rules/game.hpp`: expanded `RuleMetrics`.
- `include/search/alpha_beta.hpp`: expanded search, TT, undo, and L_eq profiling counters.
- `src/rules/game.cpp`: propagation detail counters, legal-filter counters, terminal timing.
- `src/search/alpha_beta.cpp`: metric aggregation, TT cutoff counts, undo timing, ordering counters.
- `src/exact/equivalence.cpp`: L_eq attempt/rollback profiling.
- `benchmarks/stage5_profile.cpp`: near-try/near-catch fixtures and detailed profile output.
- `tests/game_rules_test.cpp`: exhaustive `16^4` propagation comparison and reachable-state
  propagation/legal-list equivalence.

## 7. Correctness Results

Commands:

```powershell
cmake --build build_stage5 --config Release --parallel
ctest --test-dir build_stage5 -C Release --output-on-failure
```

Result: 8/8 CTest tests passed.

Additional profile validation:

- Reference/LUT depth 1..3: score, node count, best move, and legal best move all matched on
  initial, duplicate_hands, high_uncertainty_midgame, many_hands, low_uncertainty_midgame,
  near_try, and near_catch.
- Exhaustive propagation equivalence now covers all `16^4` lineage inputs.
- Deterministic reachable playout compares full propagation and final legal move lists between
  reference and LUT.
- Depth-10 smoke profile preserved score, best move, legal-best status, and node count for
  reference vs LUT on all seven focused fixtures.

## 8. Depth-8 Benchmark

3-run median, Release build, TT `1 << 19`, PVS on, aspiration off, fixed depth 8,
L_eq off for before/after propagation comparison.

| position | depth | before_ms | after_ms | delta_% | nodes_before | nodes_after | score_before | score_after | best_before | best_after | movegen_ms | legal_filter_ms | apply_ms | propagation_ms | ordering_ms | L_eq_ms | illegal_moves |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---:|---:|---:|
| initial | 8 | 415.5 | 216.3 | -47.9 | 55648 | 55648 | -235 | -235 | p3:7->4 | p3:7->4 | 112.0 | 106.3 | 114.9 | 42.3 | 10.4 | 0.0 | 0 |
| duplicate_hands | 8 | 1492.7 | 727.4 | -51.3 | 145863 | 145863 | -163 | -163 | p3:7->4 | p3:7->4 | 428.8 | 410.0 | 435.1 | 152.6 | 37.9 | 0.0 | 0 |
| high_uncertainty_midgame | 8 | 254.3 | 156.3 | -38.5 | 49204 | 49204 | -6065 | -6065 | p5:1->0 | p5:1->0 | 66.5 | 62.6 | 73.5 | 26.3 | 6.1 | 0.0 | 0 |
| many_hands | 8 | 311.7 | 209.6 | -32.8 | 87849 | 87849 | -999994 | -999994 | p6:5->1 | p6:5->1 | 70.8 | 67.0 | 83.0 | 25.7 | 4.9 | 0.0 | 0 |
| low_uncertainty_midgame | 8 | 21.7 | 13.2 | -39.2 | 5402 | 5402 | -3382 | -3382 | p5:3->4 | p5:3->4 | 6.8 | 6.3 | 7.0 | 2.2 | 0.7 | 0.0 | 0 |
| near_try | 8 | 19.7 | 10.2 | -48.2 | 1555 | 1555 | 999999 | 999999 | p5:1->5 | p5:1->5 | 8.5 | 8.2 | 7.9 | 2.3 | 0.5 | 0.0 | 0 |
| near_catch | 8 | 487.2 | 253.8 | -47.9 | 45087 | 45087 | 999999 | 999999 | p0:4->1 | p0:4->1 | 205.7 | 198.5 | 189.3 | 56.7 | 11.1 | 0.0 | 0 |

The node count did not change. The speedup is lower per-node cost, not fewer searched nodes.

With L_eq enabled and LUT propagation, total depth-8 median was 1567.3 ms and 383878 nodes.
That is separate from the propagation optimization; L_eq remains optional.

## 9. Depth-10 Smoke

Single Release run, same fixed-depth settings:

- Reference and LUT scores matched on all seven fixtures.
- Reference and LUT best moves matched on all seven fixtures.
- Reference and LUT node counts matched on all seven fixtures.
- All reported best moves were legal.

The largest row, duplicate_hands, went from 20684.4 ms reference to 10004.1 ms LUT with identical
1877314 nodes, score `-53`, and best move `p3:7->4`.

## 10. Risk Assessment

Risk is bounded by:

- Reference propagation mode kept in production code.
- Exhaustive LUT/reference propagation comparison.
- Reachable-state legal-list comparison.
- Fixed-depth node-count equality when ordering is unchanged.
- Full CTest pass, including protocol/action encoding and apply/undo/hash invariants.

Residual risk:

- Instrumented timings include timer overhead and should not be compared directly to contest
  no-instrumentation wall time.
- The LUT table is a local static initialized on first use; this is acceptable for the engine path
  but should be considered if startup latency becomes important.

## 11. Remaining Bottlenecks

After LUT propagation, depth-8 focused medians show:

- Movegen/legal filtering remains the largest component: 899.1 ms of 1586.8 ms.
- Propagation is reduced but still meaningful: 308.1 ms.
- Evaluation is now comparable to rules cost in several positions: 409.9 ms total.
- Terminal detection and hash recompute are visible but not first-order bottlenecks.
- L_eq overhead is negligible under the current trigger; it provides modest node reduction in
  duplicate-heavy cases and should remain guarded.

## 12. Next Recommendation

Continue classical optimization. The next safe measurement target is apply/legal filtering
microstructure after the LUT:

- Separate legal-filter state-copy/undo cost from transition cost.
- Measure Try safety reply generation cost on near_try and near_catch.
- Consider a guarded terminal candidate prefilter only if terminal timing dominates a future
  profile.
- Do not open full EAQS until movegen/apply/propagation costs are further reduced and benchmarked.
