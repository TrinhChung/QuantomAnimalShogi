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

The organizer's Rust engine defines the terminal and captured-identity order:

1. Constrain the moving piece, demote a captured Hen, transfer the captured piece, promote a
   moving Chick on the back rank, and propagate identity constraints.
2. When the opponent still owns any Lion candidate, remove Lion from every Lion-capable hand
   piece owned by the mover, then propagate again.
3. Switch the side to move and increment the turn.
4. Award Try only when the new side to move already has an owned Lion candidate on its target
   back rank. This means entering the rank starts a pending Try; the opponent receives one reply.
5. Otherwise award Catch when the previous mover owns a Lion-capable hand piece, then apply the
   256-turn draw if neither win condition applies.

Terminal priority after a reply is therefore surviving pending Try, Catch by the replying mover,
then the turn-limit draw. Do not replace the delayed Try rule with an immediate attack-map test.

Required invariant test:

```text
before = state and hash
undo_move(state, apply_move(state, move))
assert state == before.state and hash == before.hash
```

Incrementing the turn count and switching `side_to_move` are part of the atomic transition and hash update.
