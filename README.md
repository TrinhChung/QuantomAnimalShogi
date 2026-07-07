# Quantum Animal Shogi Engine

CPU-only C++17 engine implementing the official rules and 240-action JSON-line
protocol in [RULE_GAME.md](docs/RULE_GAME.md).

Search is delivered in two verified stages:

1. Negamax Alpha-Beta with iterative deepening, handcrafted evaluation, move
   ordering, killer/history heuristics and a fixed-size transposition table.
2. L_eq canonical successor grouping, injected from `exact/` only after the
   baseline tests pass.

No ML, GPU, network, paid dependency or background worker is used.

## Build

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Requirements: CMake 3.15+ and a C++17 compiler. A statically linked Windows build
is available at `build/qas.exe`.

## Contest protocol

Run without arguments (or use `protocol`) and send one JSON value per line:

```powershell
.\build\qas.exe
.\build\qas.exe protocol 1000 64
```

- `get_action`: stdout contains exactly one integer in `0..239`.
- `end_game`: stdout contains `"OK"`.
- Diagnostics go only to stderr.
- The selected action is validated against the received `action_mask`; if search
  and environment disagree, the engine uses a mask-enabled legal fallback.
- While waiting for the next line, the process blocks on stdin and consumes no
  search CPU.

Official encoding:

```text
action = source_index * 12 + destination_index
source: board 0..11 or hand slot 12..19
destination: board 0..11
```

## Development CLI

```powershell
.\build\qas.exe engine-demo
Get-Content state.txt | .\build\qas.exe search 1000 8
Get-Content state.txt | .\build\qas.exe search-leq 1000 8 12
Get-Content state.txt | .\build\qas.exe legal
.\build\qas_benchmark_all.exe 4
.\build\qas_selfplay.exe 20 3
```

The debug state format starts with `side turn`, followed by eight lines:

```text
<piece-id> <owner> <origin> <square|Hslot> <CGELH-mask>
```

## Implemented rules

- Piece-centric state: `board[12]`, eight persistent positions/masks, owner and
  origin bits, side, turn, terminal result and Zobrist key.
- Official five-bit order: Chick, Giraffe, Elephant, Lion, Hen.
- Local move collapse and automatic Chick-to-Hen promotion on the opponent rank.
- Captured Hen demotes to Chick; a non-final captured Lion candidate loses `L`.
- Catch occurs when capturing the final Lion candidate of the opponent origin.
- Try accepts any piece that can be Lion and checks whether a legal immediate
  capture exists after propagation.
- Exact fixed-point lineage quotas `CH/G/E/L`, one per origin side.
- Draw exactly at turn 256; turn is part of Zobrist and canonical keys.

## Source layout

- `include/core`, `src/core`: fundamental identity/constraint types.
- `include/rules`, `src/rules`: state, move tables, legal moves, apply/undo,
  propagation and terminal rules.
- `include/search`, `src/search`: Stage 1 Alpha-Beta and TT.
- `include/exact`, `src/exact`: Stage 2 canonical successor equivalence.
- `include/io`, `src/io`: JSON protocol and official action codec.
- `tests`: deterministic rule/search/protocol/invariant tests.
- `benchmarks`: repeatable Stage 1, A/B L_eq and self-play programs.

## Evidence

- [Architecture note](docs/architecture.md)
- [Stage 1 benchmark](reports/stage1-benchmark.md)
- [Stage 2 benchmark](reports/stage2-benchmark.md)

All tests pass under GCC 16.1 with
`-O3 -Wall -Wextra -Wpedantic -Werror`. The protocol generator/action-mask test
uses a complete initial-state fixture. No captured real contest message was
provided, so final validation against server-produced masks must still be run when
such fixtures are available.
