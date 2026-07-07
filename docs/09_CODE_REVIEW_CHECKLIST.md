# Code Review Checklist

Complete applicable items before merge. A checked item means the reviewer inspected evidence, not merely that the author asserted it.

## Design and Clarity

- [ ] Are names clear, English, domain-specific, and convention-compliant?
- [ ] Does each file and function have one responsibility and acceptable size?
- [ ] Are module dependencies allowed and free of cycles?
- [ ] Is data flow explicit, with no hidden mutable global state?
- [ ] Are failure modes and invariants represented explicitly?

## Rules and State

- [ ] Is every mutation visible in the function name and contract?
- [ ] Does each apply operation have a complete undo operation?
- [ ] Are hash, equality, canonical key, side to move, and turn count updated correctly?
- [ ] Is propagation called before canonicalization/evaluation and run to a fixed point?
- [ ] Can an invalid/empty mask be silently accepted? It must not be.
- [ ] Are Catch, Try, draw, and changed rule cases tested?

## Search and IO

- [ ] Does search use internal legal moves rather than external `action_mask`?
- [ ] Does root search always return a legal fallback, including timeout paths?
- [ ] Are TT bound/depth/key semantics correct and tested?
- [ ] Is stdout limited to official action output?
- [ ] Could the change obstruct future `L_eq` or exact solving by losing state information or conflating successors?

## Evidence and Maintenance

- [ ] Do new/changed complex rules have focused and invariant tests?
- [ ] Do all relevant tests pass deterministically?
- [ ] Does each optimization include reproducible before/after benchmark data?
- [ ] Are allocations and memory bounds acceptable for measured hot paths?
- [ ] Are docs updated for architecture or invariant changes?
- [ ] Are TODOs owned, dated, and justified?

Reject the patch when a correctness-critical item is unanswered; create a follow-up only for genuinely non-blocking maintenance.
