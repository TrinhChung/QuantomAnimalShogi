# Training Data Contract

Last reviewed: 2026-07-23 (Asia/Tokyo).

Tài liệu này định nghĩa dữ liệu ván đấu nào được phép dùng cho phân tích và huấn luyện về sau.
Hệ thống hiện tại không chứa model hoặc code học máy; nó chỉ tạo trajectory cổ điển có provenance,
nhãn và hàng rào chất lượng rõ ràng.

## Nguyên tắc

1. Không tin observation, action, reward hoặc terminal label chỉ vì browser đã gửi lên.
2. Mỗi trajectory hoàn tất phải được replay bằng Rust/WASM rules artifact trước khi đủ điều kiện.
3. Bot version được định danh bằng `version_key`, SHA-256 `artifact_digest` và `policy_key`.
4. Transition và validation là append-only. Sửa nhãn bằng record có version mới, không ghi đè.
5. Legacy data không được tự động nâng cấp. Thiếu state/encoding/provenance thì giữ `quarantined`.
6. Elo, báo cáo trận và dataset export chỉ dùng match `train-eligible`.
7. Split dataset ở biên series; trajectory có cùng gameplay fingerprint chỉ giữ bản đầu tiên.

## Vòng đời chất lượng

```text
legacy row ------------------------------> quarantined

new match -> raw -> completed -> replay validator -> train-eligible -> dataset export
                                  |
                                  +-----------------> rejected

stopped / failed -----------------------------------> rejected
```

| `quality_status` | Ý nghĩa                                     | Được dùng cho Elo/report/export |
| ---------------- | ------------------------------------------- | ------------------------------: |
| `quarantined`    | Dữ liệu cũ hoặc không đủ contract v2        |                           Không |
| `raw`            | Đang ghi hoặc chờ validation                |                           Không |
| `train-eligible` | Replay và toàn bộ invariant đã pass         |                              Có |
| `rejected`       | Không hoàn tất hoặc có ít nhất một mismatch |                           Không |

Migration `004_training_data_integrity.sql` để toàn bộ row cũ ở `quarantined`. Rating là derived
data nên migration xóa `rating_events` cũ và đưa `bot_ratings` về 1500; bridge chỉ dựng lại rating
từ các ván `train-eligible`.

## Contract trajectory v2

Contract dùng các định danh bất biến sau:

| Trường                      | Giá trị hiện tại                        |
| --------------------------- | --------------------------------------- |
| `trajectory_schema_version` | `2`                                     |
| `ruleset_version`           | `quantum-animal-shogi-v1`               |
| `recorder_version`          | `web-trajectory-v2`                     |
| `validator_version`         | `wasm-replay-v1`                        |
| `observation_encoding`      | `qas-observation-20x9-v1:side-to-move`  |
| `action_encoding`           | `qas-action-mask-240-v1:side-to-move`   |
| `state_encoding`            | `qas-wasm-state-v1:side-to-move`        |
| `reward_encoding`           | `terminal-outcome-v1:actor-perspective` |

Match còn lưu SHA-256 của recorder build và rules artifact, RNG seed uint32, initial state đầy đủ,
initial-state hash và trajectory checksum. `data_source` phân biệt `web-human`, `web-tournament`
hoặc recorder tương lai.

Mỗi transition lưu:

- UUID, `ply`, actor seat và immutable bot-version foreign key;
- observation `20 x 9`, legal-action mask 240 phần tử và observation turn;
- internal action `(source, destination)` cùng official protocol action;
- state trước/sau và SHA-256 cho cả hai state;
- actor-perspective immediate reward, outcome, terminal flag và terminal reason;
- policy metadata schema v1, exclusion quality flags và transition checksum;
- think time nếu actor là engine và recorder có số đo.

State/observation luôn ở góc nhìn side-to-move. `reward_after` bằng 0 ở transition chưa terminal;
terminal reward là `+1`, `0` hoặc `-1` từ góc nhìn actor.

## Validation bằng replay

Khi match được finish `completed`, server chạy lại từ `getInitialState()` và kiểm tra từng ply:

1. Ply liên tục, seat đúng theo `first_seat`, không có move sau terminal.
2. State-before nối đúng state-after của transition trước.
3. Observation, turn và toàn bộ legal mask bằng kết quả Rust/WASM.
4. Action codec đúng, action có trong legal list và bit tương ứng bằng 1.
5. State-after bằng kết quả `getNextState` và hash khớp.
6. Reward/outcome/reason khớp Catch, Try hoặc max-turn draw.
7. Actor kind, bot/version/policy metadata khớp foreign key trong MySQL.
8. Với `wasm-random`, action khớp LCG sequence từ RNG seed đã lưu.
9. Không có exclusion quality flag; transition checksum và trajectory checksum khớp.

Mỗi lần kiểm tra tạo một row mới trong `trajectory_validations`. Export luôn replay lại một lần nữa,
vì vậy sửa DB sau lần validation đầu không thể âm thầm đi vào dataset.

## Policy metadata v1

Server tự lấy `botId`, `versionKey`, `policyKey` và configured depth từ bot version; client không thể
đổi provenance này. `decisionStats` là `null` khi bot chưa expose telemetry. Khi có, object chỉ nhận:

- `searchDepth`, `nodes`;
- `score` với `scorePerspective = actor`;
- `selectedActionProbability`, `policyEntropy`;
- `principalVariation` là tối đa 256 official action index trong `0..239`.

Thêm metric mới cần tăng schema hoặc cập nhật validator; không nhét field ad-hoc vào JSON.

## Nhãn mở rộng

`training_labels` target đúng một match UUID hoặc move UUID. Nhãn gồm `namespace`, `key`, version,
JSON value, producer và confidence tùy chọn. Cùng target/namespace/key/version/producer là duy nhất.
API và repository không có đường update/delete; sửa nhãn bằng version mới.

Ví dụ payload:

```json
{
  "moveId": "550e8400-e29b-41d4-a716-446655440000",
  "namespace": "search.analysis",
  "key": "tactical_motif",
  "version": 1,
  "value": { "motif": "forced-catch" },
  "producer": "offline-analyzer-v1",
  "confidence": 0.97
}
```

Gửi qua `POST /api/training-data/labels`. Không lưu username, IP, token hay định danh người chơi;
human actor chỉ dùng identity chung `local-human`.

## Dataset export

```powershell
cd quantum-animal-shogi/web
npm run dataset:validate -- --limit 1000
npm run dataset:export -- --output .\output\datasets --name qas-training-v1 --seed 20260723
```

Exporter chỉ query match `completed`, schema v2, `train-eligible` và có checksum. Nó replay lại,
chia `train/validation/test` theo hash của series với tỷ lệ 80/10/10, loại gameplay fingerprint
trùng, rồi ghi ba file JSONL và `manifest.json`. Mỗi file có SHA-256; MySQL lưu export, filter,
manifest, checksum, số match/sample và mapping match-to-split trong `dataset_export_items`.

Mỗi JSONL row chứa provenance, actor version, encoding, state-before, observation, legal mask,
action, immediate reward, state-after, terminal info, final return target, policy metadata, quality
flags và versioned annotations. Không split transition của cùng series sang nhiều tập.

## Bảng MySQL liên quan

- `bots`, `bot_versions`: identity và immutable artifact/policy provenance.
- `match_series`, `matches`, `match_moves`: episode và transition chuẩn hóa.
- `trajectory_validations`: audit log append-only của replay validator.
- `training_labels`: nhãn versioned append-only.
- `dataset_exports`, `dataset_export_items`: lineage của dataset đã xuất.
- `bot_ratings`, `rating_events`: projection chỉ từ trajectory đủ chuẩn.

`match_moves`, `trajectory_validations` và `training_labels` chỉ có thao tác append trong application
boundary. Export replay lại dữ liệu thay vì tin cờ chất lượng cũ, nên sửa trực tiếp bằng tài khoản DB
cũng bị phát hiện. Không đưa legacy JSONL vào database training-ready; giữ file nguồn ở archive ngoài
hoặc database quarantine riêng.
