# Architecture Rules

## Modules and Ownership

- `core/`: fundamental types, constants, board geometry, piece model, move model, and state representation. It contains no game-policy search.
- `rules/`: legal move generation, move application/undo, terminal detection, and quantum constraint propagation.
- `search/`: negamax alpha-beta/PVS, iterative deepening, transposition table, move ordering, and evaluation.
- `exact/`: future exact solver and successor-equivalence (`L_eq`) search.
- `io/`: contest protocol and `ActionIndex` conversion.
- `tests/`: tests across module boundaries.
- `benchmarks/`: repeatable performance measurements.

Put public headers under matching `include/<module>/` paths and implementations under `src/<module>/`. A source file owns one cohesive concept.

## Dependency Direction

Allowed production dependencies are:

```text
core <- rules <- search
  ^       ^
  +-------io
core,rules,search <- exact (as its design requires)
all production modules <- tests/benchmarks
```

- `core` depends on no higher module.
- `rules` depends only on `core`.
- `search` depends only on `core` and `rules`.
- `io` depends only on `core` and `rules`; search orchestration must live at an application boundary, not in codecs.
- No circular dependencies. Shared lower-level concepts move downward only if they are genuinely domain-neutral.
- Production modules never include files from `tests/` or `benchmarks/`.

Bad: `rules/legal_moves.cpp` checking a search transposition table.

Good: search calls the rules API and caches the returned result itself.

Cross-module APIs must expose domain types and documented invariants, not container internals. New top-level modules require an architecture-doc update in the same patch.
