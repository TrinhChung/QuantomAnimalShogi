# Optimization Rules

## Admission Gate

An optimization is accepted only when:

1. Existing correctness tests pass.
2. A repeatable benchmark identifies the affected workload.
3. Before/after results include build type, position set, depth or node count, time, and memory where relevant.
4. Semantics are unchanged or an intentional change is separately specified and tested.

Remove optimizations that add material complexity without a repeatable benefit.

## Hot-Path Policy

- Prefer precomputed board-geometry and move-mask tables.
- Prefer fixed-size arrays and bounded stack/value storage over heap containers in recursive search.
- Avoid dynamic allocation, full-state copying, virtual dispatch, and exceptions in measured hot paths.
- Prefer apply/undo over copying complete state.
- Avoid `unordered_map` in deep search unless profiling demonstrates it is appropriate; use a fixed-size transposition table with a documented replacement policy.
- Use bit operations for masks and occupancy when they remain clear and tested.
- Cache only with predictable memory use. Every cache key includes every state field that affects its result, including side and turn count.

Bad: packing all state into opaque bit tricks before a correct reference exists.

Good: benchmark a tested fixed-array move list against the baseline, then adopt it with equivalent-result tests.

Performance claims must be reproducible in `benchmarks/`, not based on a single interactive run.
