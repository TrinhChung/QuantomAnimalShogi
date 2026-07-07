# Refactoring Rules

## Triggers

- Review a source file at 500 lines; split it when it owns multiple responsibilities. Generated tables are exempt but must be clearly marked.
- Review functions above 40 lines and split functions above 80 unless a written justification survives review.
- Extract rule-specific helpers when branches represent distinct rules or terminal cases.
- Remove duplication only after the duplicated behavior is understood and tested; do not create a generic abstraction for two superficially similar cases.

## Behavior Preservation

- A refactor keeps observable behavior identical unless the behavior change is separately documented and tested.
- Preserve tests throughout; add characterization tests before changing unclear legacy code.
- Avoid large rewrites. Use reviewable stages that compile and test independently.
- Do not combine broad formatting or renaming with semantic changes.

When adding or changing a state field, update all of:

- `State` construction and equality;
- full and incremental hash;
- canonical/equivalence key;
- apply/undo and its undo record;
- tests and fixtures;
- serialization and debug printer.

Bad TODO: `// TODO: fix later`.

Allowed TODO: `// TODO(alex, 2026-07-07): replace linear probe after TT benchmark shows collision cost.` A TODO requires owner, date, and reason; otherwise resolve it or remove it.
