# State and Move Rules

## State Contract

- `State` contains at least board/piece information, `side_to_move`, and turn count.
- A state is deterministic and fully reproducible from its fields; no correctness-relevant information may live in an external cache.
- Equality, canonical keys, serialization/debug printing, and hashing cover every value-relevant field.
- The hash includes `side_to_move` and turn count because the 256-turn draw rule makes otherwise identical boards different states.
- Invalid or empty quantum masks are invariant failures or illegal branches; never repair or ignore them silently.

## Move Contract

- Internal `Move` is independent of the environment's external action index.
- Conversion is isolated in `io/action_codec`; rules and search do not include that codec.
- Search legal-move generation never depends on an external `action_mask`. The mask may only validate real environment input/output at the IO boundary.
- Move fields use typed squares, piece identities, and move kinds rather than packed magic numbers at public boundaries.

Bad: storing an environment action number in `Move` and decoding it in `apply_move`.

Good: decode once to a domain `Move`, validate through rules, and operate only on that type.

## Transition Order and Identity

The rule pipeline must document and test its exact order. Constraint propagation reaches a fixed point before canonicalization or evaluation. `apply_move` records enough information for `undo_move` to restore byte-for-byte logical state, equality, and the identical Zobrist key.

Required invariant test:

```text
before = state and hash
undo_move(state, apply_move(state, move))
assert state == before.state and hash == before.hash
```

Incrementing the turn count and switching `side_to_move` are part of the atomic transition and hash update.
