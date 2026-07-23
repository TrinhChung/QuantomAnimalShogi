# Technical architecture

## State and rules

`State` là piece-centric: `board[12]`, `pos[8]`, `mask[8]`, owner/origin bitsets,
side, turn, terminal/winner và Zobrist hash. `Undo` giữ snapshot nhỏ của state;
ưu tiên correctness và đảm bảo apply/undo phục hồi chính xác cả hash.

Move tables được precompute cho 5 form, 2 hướng và 12 square. Hand positions giữ
official source slot `12..19`. Search sinh trực tiếp từ quân trên bàn và trong tay;
không quét 240 raw action. `apply_move` thực hiện local collapse, automatic
promotion, capture, propagation, Catch/Try/draw rồi cập nhật hash. Turn count có
257 Zobrist entries riêng.

Propagation gom Chick và Hen thành lineage CH. Với từng origin side, engine duyệt
tối đa 24 hoán vị CH/G/E/L, giữ các lineage có support và lặp đến fixed point.

## Stage 1 search

Negamax Alpha-Beta chạy iterative deepening. Timeout được kiểm tra định kỳ; chỉ
depth hoàn tất mới thay root move. Evaluation gồm terminal, expected material theo
mask, Lion safety/pressure, Catch/Try threat, mobility, uncertainty, risky Lion
collapse và tiến độ Try.

Ordering: TT move, immediate Catch/Try, defense trước immediate loss, capture,
promotion, Lion-candidate reduction, mask collapse, Try progress, killer/history
và evaluation preview. TT là vector fixed-size, lưu key/depth/score/bound/best/age.

## Dependency boundaries

`core <- rules <- search`; `io` chỉ phụ thuộc core/rules. `exact` phụ thuộc
core/rules/search và inject một bounded successor reducer qua `SearchOptions`, nên
Stage 1 không include hoặc gọi trực tiếp code L_eq.

## Stage 4 evaluation hot path

Search owns one reusable `EvalScratch` containing 12-bit occupancy/attack masks,
mobility and Lion-candidate counters, and reserved pseudo-move/candidate buffers.
Material values and mask popcounts are 32-entry compile-time tables. Immediate
Catch/Try evaluation filters candidates by necessary conditions before calling
the unchanged rule transition; successful probes are restored through `Undo`.
`SearchOptions::optimized_eval_enabled` retains the previous evaluator for
deterministic differential testing. Benchmark-only component timers remain off
in competition search.

## Stage 2 L_eq

Mỗi legal move được apply và propagate trước khi tạo `CanonKey`. Key chứa board,
các tuple position/owner/origin/mask, side sau move, turn, terminal và winner.
Piece labels được sort trong từng origin group; hand-slot labels được canonicalize
vì không ảnh hưởng luật. Hai physical labels chỉ được gộp khi toàn bộ successor
semantics trùng nhau. Mỗi class giữ một legal representative để chuyển lại official
external action `source*12+destination`.

Always-on không phù hợp: các vị trí thông thường không có duplicate nhưng vẫn trả
chi phí canonicalization. Default dùng trigger:

1. raw branching `L >= threshold` (production mặc định 24),
2. remaining depth ít nhất 4,
3. side-to-move có ít nhất hai hand pieces cùng origin/owner/mask,
4. measured successor reduction ít nhất 25%, và
5. engine không ở low-time mode.

`threshold=0` chỉ tắt raw-branching gate cho A/B test; các gate cấu hình khác vẫn
explicit. Generation cache chưa được thêm: benchmark
cho thấy trigger rẻ đã loại overhead ở node không có duplicate, còn cache sẽ tăng
memory/invalidations mà chưa có bằng chứng mang lại lợi ích.

# Evaluation infrastructure boundary

The versioned `evaluation/` tree is external orchestration, not a production engine module.
Python owns immutable artifacts, process control, logging, statistics, resume state, and reports.
It does not implement or approximate game rules.

Two application-boundary executables link `qas_core`:

- `qas_evaluation_referee` is the single match/corpus transition authority. It generates the
  side-normalized official observation and action mask, maps the chosen public action back to the
  native state, and calls `rules::apply_move`.
- `qas_evaluation_benchmark` runs the exact version's search implementation with explicit fixed or
  time-controlled options and emits machine-readable telemetry. It is frozen beside each accepted
  `qas.exe`.

The dependency remains one-way: evaluation tools may depend on core, rules, search, exact, and IO;
production modules never depend on evaluation code. Contest stdout and search semantics are
unchanged.

## Web solo boundary

`quantum-animal-shogi/` is a local companion application, not a dependency of the C++ contest
engine or evaluation pipeline. It contains a small Rust reference core exposed through WASM and a
Vue application boundary. The browser owns local match orchestration for human-versus-engine and
engine-versus-engine play. Packaged Alpha-Beta and random bots run without Python. An allowlisted
Node bridge is the only native boundary and may launch immutable C++ Stage binaries; it cannot
accept arbitrary executable paths. The same bridge is the persistence boundary for the local
MySQL 8 database; the browser never connects to MySQL directly.

Persistence keeps bot identity separate from immutable bot versions, then groups games as
`match_series -> matches -> match_moves`. Each completed move
is committed with its seat, action coordinates, official protocol action, observation/action mask,
state before/after, actor-perspective reward, policy provenance, checksum, and think time. A stopped process therefore leaves an explicitly running/stopped
match with all previously committed moves rather than silently losing the game. Versioned SQL files
under `web/server/migrations/` are applied before the bridge starts, and their checksums prevent an
already-applied migration from being edited in place. The report/history views read an aggregate
projection through `/api/report`; they retain seed data only as a visible fallback when MySQL is
unavailable. This storage/reporting boundary does not implement search, training, a model, or Python
runtime behavior; replay validation calls the existing Rust/WASM rules implementation.

Trajectory schema v2 gives each match explicit rules/recorder digests, encodings, RNG seed,
initial state/hash and a quality lifecycle. Legacy rows remain `quarantined`; new rows begin `raw`
and become `train-eligible` only after a deterministic replay verifies every state, observation,
legal mask, action, reward, terminal label, actor provenance and checksum. Failed or incomplete
matches become `rejected`. Validation events, move transitions and versioned labels are append-only.
Elo/report projections and dataset exports only consume `train-eligible` rows. Export performs a
fresh replay, splits at the series boundary, removes duplicate gameplay fingerprints and records
JSONL file hashes plus match membership in MySQL. The precise contract is documented in
`docs/training_data_contract.md`.

Bot rating and tournament state are normalized separately. `bot_ratings` stores the current
version-level Elo, while `rating_events` is an idempotent ledger keyed by completed match. A
tournament snapshots that Elo into immutable `seed_number`, `seed_group`, and `seed_rating`
fields. Rounds re-pair the surviving highest and lowest frozen seeds; Elo changes from games affect
future tournaments, never the active tournament's seed order. Tournament, entry, round, pairing,
and game-series records remain independently queryable for future formats.

Benchmark persistence is a separate projection rooted at immutable bot versions.
`bot_version_algorithm_profiles` records the reviewed algorithm/configuration description;
`benchmark_suites` defines a comparable corpus and metric contract; `benchmark_runs` stores the
environment and settings provenance; `benchmark_cases` stores raw per-position measurements; and
`benchmark_findings` stores reviewed conclusions. The bridge exposes this data through
`GET /api/benchmarks`, includes it in `GET /api/report`, and accepts bounded, validated, idempotent
bundles at `POST /api/benchmarks/runs`. Report diagnostics are derived from the raw cases rather than
persisted as an opaque score: coverage, deadline failures, dominant component timing, transposition
table reuse and branching reduction remain individually auditable. Elo is displayed beside these
metrics but is never mathematically combined with them.

Historical standalone `qas.exe` files are archival artifacts under `evaluation/versions/legacy-*`.
The local bridge discovers only explicitly configured artifacts and validates every returned action
against the browser-generated official action mask.
