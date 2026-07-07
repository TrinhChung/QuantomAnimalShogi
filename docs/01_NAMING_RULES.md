# Naming Rules

## Standard Forms

| Item | Form | Good | Bad |
|---|---|---|---|
| Files/directories | `lower_snake_case` | `move_generator.cpp` | `MoveGen.cpp` |
| Classes/structs/type aliases | `PascalCase` | `SearchResult` | `search_result` |
| Enum types | `PascalCase` | `TerminalReason` | `term_t` |
| Enum values | `PascalCase` | `TerminalReason::TryWin` | `WIN_2` |
| Functions | `lower_snake_case` | `generate_legal_moves` | `genMv` |
| Variables/parameters | `lower_snake_case` | `side_to_move` | `stm` |
| Constants | `kPascalCase` | `kMaximumTurnCount` | `MAXT` |
| Test files | `<subject>_test.cpp` | `state_hash_test.cpp` | `test1.cpp` |
| Benchmarks | `<subject>_benchmark.cpp` | `move_generation_benchmark.cpp` | `speed.cpp` |

Public and private code use the same convention. Private data members end in `_`, for example `entry_count_`.

## Domain-Specific Names

- Bitmask types end in `Mask`; constants state their meaning: `PieceMask`, `kMaskLion`, `kMaskChick`, `kMaskHen`. Do not use `bits` when a domain name is available.
- Zobrist material uses `ZobristKey`, `zobrist_key`, and tables such as `piece_square_keys`; never `hash_data`.
- Internal move types use `Move`; specialized forms use names such as `DropMove` only when distinct types are necessary. External integers are `ActionIndex`, never `Move`.
- Search scores use `SearchValue`, `alpha`, `beta`, `best_value`, and named constants such as `kMateValue`; do not mix them with evaluation units or raw `int` without a documented boundary.

Avoid `tmp`, `data`, `value`, `flag`, `obj`, and one-letter names unless scope is a few obvious lines (for example a loop coordinate). Boolean names read as predicates: `is_terminal`, `has_capture`. Prefer `enum class BoundType` over numeric flags.
