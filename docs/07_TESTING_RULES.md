# Testing Rules

## Required Coverage

Before dependent features are merged, provide tests for:

- legal move generation against the official `action_mask` at the IO validation boundary;
- apply/undo identity, including exact hash restoration;
- incremental hash versus full recomputation;
- propagation fixed point (running it twice changes nothing);
- no empty mask after valid propagation;
- Catch terminal, delayed Try after one opponent reply, and draw exactly at turn 256;
- surviving pending Try priority over Catch/draw and Catch priority over the turn-limit draw;
- seeded random playout invariants with full unwind;
- perft-style node counts from fixed named positions;
- search always returning a legal move;
- timeout fallback from the last completed depth;
- TT exact/lower/upper-bound correctness;
- future `L_eq` successor-equivalence against exhaustive small cases.

## Test Quality Rules

- Tests are deterministic: fixed seeds, controlled clocks, and no dependence on iteration order.
- Each rule test includes the smallest useful position and a descriptive name.
- Every bug fix begins with a failing regression test where feasible.
- Compare optimized logic with a slow, obviously correct reference on bounded inputs.
- Do not weaken assertions to make a test pass. Update expected behavior only with an explicit rule justification.
- Unit tests live beside the test target under `tests/`; benchmarks are not correctness tests.

Bad: `TEST(Game, Test1)` with a large unexplained setup.

Good: `TEST(TerminalDetection, TryWinsWhenLionReachesBackRankSafely)`.
