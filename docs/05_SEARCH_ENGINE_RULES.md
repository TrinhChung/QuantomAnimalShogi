# Search Engine Rules

## Mandatory Implementation Order

1. Plain negamax alpha-beta with a clear reference implementation.
2. Iterative deepening and deadline handling.
3. Transposition table, only after apply/undo and incremental-hash tests pass.
4. PVS, only after alpha-beta results are stable and cross-checked.
5. `L_eq`, only after baseline alpha-beta plus TT is correct.

Each step keeps tests comparing it with the previous baseline on small positions.

## Search Contracts

- Root search always initializes a legal fallback move before work that may time out.
- Timeout returns the best move from the last fully completed depth; an incomplete iteration must not replace it.
- Recursive search uses internal legal moves, never external action masks.
- Terminal detection belongs to `rules`; score interpretation belongs to `search`.
- Search has no stdout output except the application boundary's official action. Diagnostics use `stderr` or a controlled, disabled-by-default logger.
- Search values use named bounds and avoid overflow around mate scores.
- TT entries state depth, value, key, best move, and `Exact`/`LowerBound`/`UpperBound`; probing must respect bound semantics.
- Stop/deadline checks must be deterministic in tests through an injectable clock or stop policy.

Bad: return the current iteration's partially searched first move on timeout.

Good: retain the completed-depth result separately and return it when interrupted.
