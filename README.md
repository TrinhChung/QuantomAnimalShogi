# Quantum Animal Shogi Engine

CPU-only C++17 engine implementing the official rules and 240-action JSON-line
protocol in [RULE_GAME.md](docs/RULE_GAME.md).

Search is delivered in two verified stages:

1. Negamax Alpha-Beta with iterative deepening, handcrafted evaluation, move
   ordering, killer/history heuristics and a fixed-size transposition table.
2. L_eq canonical successor grouping, injected from `exact/` only after the
   baseline tests pass.
3. Resource-aware AB+TT+PVS with aspiration windows, strong ordering and runtime
   profiles.

No ML, GPU, network, paid dependency or background worker is used.

## Build

```powershell
scripts\build_version.ps1 -Version current -Configuration Release
```

Requirements: CMake 3.15+ and a C++17 compiler. A statically linked Windows build
is available at `build/current/Release/qas.exe` with Visual Studio generators. Direct
CMake usage must also keep the version level: `cmake -S . -B build/current`.

Every generated tree uses `build/<version>`. Rebuilding `current` reuses the same directory;
use another stable name only when a separate candidate or configuration must be preserved:

```powershell
scripts\build_version.ps1 -Version stage5-1 -Configuration Release
```

## Contest protocol

Run without arguments (or use `protocol`) and send one JSON value per line:

```powershell
.\build\current\Release\qas.exe
.\build\current\Release\qas.exe protocol 1000 64
.\build\current\Release\qas.exe --profile contest_safe
.\build\current\Release\qas.exe --profile contest_high_ram --tt-size-mb 2048
.\build\current\Release\qas.exe --profile low_ram --tt-size-mb 128
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
.\build\current\Release\qas.exe engine-demo
Get-Content state.txt | .\build\current\Release\qas.exe search 1000 8
Get-Content state.txt | .\build\current\Release\qas.exe search-leq 1000 8 12
Get-Content state.txt | .\build\current\Release\qas.exe legal
.\build\current\Release\qas_benchmark_all.exe 4
.\build\current\Release\qas_selfplay.exe 20 3
.\build\current\Release\qas_stage35_benchmark.exe --generate-fixtures --suite generate
.\build\current\Release\qas_stage35_benchmark.exe --suite smoke --repeats 7
.\build\current\Release\qas_stage35_benchmark.exe --suite fixed --depths 9,10,11,12 --repeats 7
.\build\current\Release\qas_stage35_benchmark.exe --suite iterative --time-limits-ms 5000,10000,30000 --repeats 7
.\build\current\Release\qas_stage35_benchmark.exe --suite tt --repeats 7
.\build\current\Release\qas_stage35_benchmark.exe --suite fixed --leq-threshold 24 --leq-min-depth 4 --leq-min-duplicate-ratio 0.25
.\build\current\Release\qas_stage35_benchmark.exe --suite fixed --depths 10 --eval-mode baseline --repeats 7
.\build\current\Release\qas_stage35_benchmark.exe --suite fixed --depths 10 --eval-mode optimized --repeats 7
```

The Stage 3.5 runner writes per-run and median rows, applies a hard timeout,
checks move/action legality and root hash restoration, and records benchmark-only
search/resource counters. Use `--position-set all` to include the 100 saved random
fixtures; the default `required` set contains the ten named positions. The full
all-mode/depth/time/TT matrix is intentionally not the default because it is a
multi-day sequential workload.

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

## Project structure

```text
QuantumShogiAnimal/
|-- build/                    Ignored versioned build trees (`build/<version>`)
|   |-- current/              Reusable active working-tree build
|   `-- <version>/            Preserved candidate or historical build
|-- include/                  Public C++ headers, grouped by owning module
|   |-- core/                 Domain types, configuration, state and shared timing
|   |-- rules/                State transitions and legal-move API
|   |-- search/               Alpha-Beta search API and search statistics
|   |-- exact/                Canonical successor-equivalence API
|   `-- io/                   Contest protocol and action-codec API
|-- src/                      C++ implementations matching include/
|   |-- core/
|   |-- rules/
|   |-- search/
|   |-- exact/
|   |-- io/
|   `-- main.cpp              Contest executable and development CLI boundary
|-- tests/                    Deterministic C++ regression and invariant tests
|   `-- evaluation/           Python tests for the evaluation framework
|-- benchmarks/               Repeatable search, memory and self-play benchmarks
|-- evaluation/               External version-evaluation orchestration
|   |-- cmake/                Evaluation build-manifest generation
|   |-- config/               Profiles and acceptance policy
|   |-- corpus/               Versioned correctness/performance position corpus
|   |-- native/               Native referee and benchmark adapters
|   |-- schemas/              JSON artifact schemas
|   |-- tools/                Pipeline, analysis and reporting commands
|   `-- versions/             Frozen accepted engine artifacts and registry
|-- scripts/                  Windows entry points for versioned builds and evaluation workflows
|-- docs/                     Rules, architecture and development policies
|-- CMakeLists.txt            Build targets and test registration
|-- engine_config.json        Runtime profiles and engine defaults
`-- README.md                 Build, usage and repository overview
```

Production dependency direction remains `core <- rules <- search`; `io` depends
on `core` and `rules`, while `exact` integrates through the search extension
boundary. `evaluation/` may consume production modules, but production code does
not depend on evaluation orchestration. Generated directories such as `build/`, `reports/` and
`local_reports/` are not version-controlled; `build/` is shown above only to document its
required runtime layout.

## Evidence

- [Architecture note](docs/architecture.md)
- [Stage 1 benchmark](reports/stage1-benchmark.md)
- [Stage 2 benchmark](reports/stage2-benchmark.md)
- [Stage 3 benchmark](reports/stage3-benchmark.md)
- [Stage 3.5 benchmark and tuning report](reports/stage35-benchmark.md)
- [Stage 4 evaluation optimization report](reports/stage4/summary.md)
- [RAM/TT benchmark](reports/ram-benchmark.md)
- [Stage 3 recommendation](docs/stage3-recommendation.md)

## Permanent version evaluation

All future candidates use the standardized system in
[evaluation/README.md](evaluation/README.md). After building all Release targets, run only:

```powershell
scripts\evaluate_built_candidate.bat ^
  --candidate-exe "build\current\Release\qas.exe" ^
  --candidate-name "Stage 5.1" ^
  --change-category performance_only ^
  --profile strength_candidate
```

Read the generated `report.md`; do not substitute manual benchmarks or ad-hoc self-play.

All tests pass under GCC 16.1 with
`-O3 -Wall -Wextra -Wpedantic -Werror`. The protocol generator/action-mask test
uses a complete initial-state fixture. No captured real contest message was
provided, so final validation against server-produced masks must still be run when
such fixtures are available.
