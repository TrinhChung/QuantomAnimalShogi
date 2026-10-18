# Current Stage Algorithm Status

Last reviewed: 2026-07-22 (Asia/Tokyo).

Trang này là điểm bắt đầu để theo dõi thuật toán của stage hiện tại. Nội dung mô tả trạng thái đã
được chấp nhận, không dùng tên file binary hoặc số stage để suy đoán tính năng.

## Current Snapshot

| Mục | Trạng thái hiện tại |
|---|---|
| Champion đã được chấp nhận | `stage5-clean` — **Stage 5.0 Clean** |
| Stable anchor | `stage5-clean` |
| Source commit của artifact | `5e096227947cf53c760a027318e26183863483f3` |
| Runtime profile | `contest_safe` |
| Registry | [`evaluation/versions/versions.json`](../evaluation/versions/versions.json) |
| Cấu hình đã đóng băng | [`evaluation/versions/stage5-clean/engine_config.json`](../evaluation/versions/stage5-clean/engine_config.json) |
| Trạng thái worktree hiện tại | Chưa phải một version được đăng ký hoặc champion mới |

`build/current/Release/qas.exe` là binary của source đang làm việc. Nó chỉ là candidate cục bộ cho
đến khi được đóng băng, đánh giá và promote qua pipeline trong
[`evaluation/README.md`](../evaluation/README.md). Các artifact `legacy-stage3.5`, `legacy-stage5`
và `legacy-stage5.7` chỉ phục vụ so sánh; chúng không nằm trong registry được chấp nhận.

Khi source và artifact khác nhau, registry cùng config đóng băng quyết định trạng thái **đã được
chấp nhận**; source hiện tại quyết định hành vi của **C++ Current**. Không dùng config của worktree
để mô tả ngược lại một artifact cũ.

## Pipeline Thuật Toán Đang Hoạt Động

Một lượt chọn nước của Stage 5.0 Clean đi qua các lớp sau:

1. `rules` sinh nước hợp lệ và áp dụng đầy đủ collapse, capture, promotion, Catch/Try/draw cùng
   propagation đến fixed point.
2. Propagation mặc định dùng bảng tra cứu lineage `CH/G/E/L`; bản duyệt tối đa 24 hoán vị vẫn được
   giữ làm reference để kiểm tra tương đương.
3. `L_eq` chỉ thử gộp các nước tạo cùng canonical successor khi tất cả trigger đều đạt.
4. Các nước còn lại được xếp thứ tự trước khi tìm kiếm.
5. Iterative deepening gọi Negamax Alpha-Beta/PVS từ độ sâu nông đến sâu.
6. Aspiration window dùng điểm của độ sâu trước làm cửa sổ ban đầu và mở rộng khi fail-low hoặc
   fail-high.
7. Transposition table tái sử dụng kết quả theo Zobrist key và ngữ nghĩa bound.
8. Ở lá tìm kiếm, evaluator tối ưu chấm điểm trạng thái; bản evaluator cũ vẫn tồn tại cho
   differential test.
9. Nếu hết thời gian giữa một iteration, engine trả nước hợp lệ của độ sâu hoàn tất gần nhất.

## Bảng Bật/Tắt Production

Các giá trị dưới đây lấy từ cấu hình đóng băng của `stage5-clean`.

| Thành phần | Trạng thái | Thiết lập hoặc vai trò |
|---|---|---|
| Negamax Alpha-Beta | Bật | Search lõi, điểm nhìn đổi dấu theo bên đi |
| Iterative deepening | Bật | `max_depth=64`, soft deadline 25 s, hard deadline 29 s |
| PVS | Bật | Full window cho nước đầu; null window cho các nước sau, re-search khi cần |
| Aspiration | Bật | Cửa sổ đầu `50`, tối đa `3` lần retry rồi quay về full window |
| Transposition table | Bật | Yêu cầu 256 MiB, auto-size, tối đa 25% RAM trong giới hạn profile |
| TT replacement | Bật | Ưu tiên depth/age; mỗi entry không quá 24 byte |
| Move ordering | Bật | TT move, thắng ngay, phòng thua, capture, Lion reduction, Try, mask collapse |
| Killer/history | Bật | Học thứ tự các quiet move gây cutoff trong cùng lần tìm kiếm |
| Optimized evaluator | Bật | Lookup mask, 12-bit occupancy/attack, reusable scratch, Catch/Try prefilter |
| `L_eq` | Bật có điều kiện | `depth>=4`, `legal_count>=24`, duplicate hand hint, reduction `>=25%`, không low-time |
| Lineage LUT propagation | Bật | Lookup toàn bộ tuple bốn lineage thay cho duyệt hoán vị ở hot path |
| Generation cache | Tắt | Chưa có bằng chứng benchmark để nhận thêm độ phức tạp/bộ nhớ |
| `L_eq` cache | Tắt | Chưa có bằng chứng benchmark để bật |
| Benchmark/resource profiling | Tắt | Không chạy trong competition search |
| Legal fallback | Bật | Luôn khởi tạo một nước hợp lệ trước khi có thể timeout |

TT 256 MiB được làm tròn xuống 8,388,608 entry, tương đương 192 MiB với entry 24 byte. Benchmark
hiện có chưa chứng minh bảng lớn hơn cải thiện depth hoặc sức chơi, vì vậy đây vẫn là target thực tế.

## Các Stage Đã Đóng Góp Gì

| Stage | Thay đổi thuật toán chính | Trạng thái bằng chứng |
|---|---|---|
| 1 | Negamax Alpha-Beta, iterative deepening, evaluator thủ công, TT | Baseline đúng và deterministic; có benchmark riêng |
| 2 | Canonical successor grouping `L_eq` | Chỉ có lợi rõ trên trạng thái duplicate; chọn trigger thay vì always-on |
| 3 | PVS, aspiration, move ordering, killer/history | Kết quả được đối chiếu với Alpha-Beta ở độ sâu nhỏ |
| 3.5 | Tuning tài nguyên và trigger `L_eq` `24/4/0.25` | Bộ fixture và báo cáo A/B nhiều workload |
| 4 | Tối ưu evaluator nhưng giữ nguyên điểm số/ngữ nghĩa | Accepted; eval time giảm 79.5% và tổng thời gian giảm 56.8% trên focused suite |
| 5 | Thay propagation hoán vị bằng lineage lookup table | `LineageLut` là mặc định; reference path và exhaustive equivalence test được giữ lại |
| 5.0 Clean | Đóng băng Stage 5 thành champion/stable anchor | `accepted=true` trong permanent version registry |

Tài liệu bằng chứng: [Stage 1](../reports/stage1-benchmark.md),
[Stage 2](../reports/stage2-benchmark.md), [Stage 3](../reports/stage3-benchmark.md),
[Stage 3.5](../reports/stage35-benchmark.md), [Stage 4](../reports/stage4/summary.md) và
[RAM/TT](../reports/ram-benchmark.md). Stage 5 có
[`qas_stage5_profile`](../benchmarks/stage5_profile.cpp) và
[`game_rules_test`](../tests/game_rules_test.cpp) so sánh LUT với reference; chưa có báo cáo hiệu
năng Stage 5 riêng được check in, vì vậy không ghi một con số speedup chưa được lưu làm bằng chứng.

## Diễn Giải Các Thành Phần Chính

- **Negamax Alpha-Beta/PVS** quyết định nước đi và cắt các nhánh không thể cải thiện kết quả hiện
  tại.
- **Evaluator** chỉ ước lượng trạng thái ở lá chưa kết thúc; nó không thay thế luật Catch/Try/draw.
- **TT** tránh tìm lại cùng một state và lưu `Exact`, `LowerBound` hoặc `UpperBound` theo đúng độ
  sâu.
- **Move ordering** không đổi kết quả minimax khi search hoàn tất; mục tiêu của nó là tạo cutoff
  sớm hơn.
- **`L_eq`** không phải heuristic bỏ nước. Nó chỉ giữ một legal representative khi các nước sau
  propagation có cùng canonical successor semantics.
- **Lineage LUT** là tối ưu rules hot path, không phải mô hình học máy. Bảng được xây từ toàn bộ
  assignment hợp lệ và trả đúng tập lineage support như reference hoán vị.

Giải thích nhập môn và ánh xạ từng thành phần vào bot web/native nằm tại
[`bot_algorithms.md`](bot_algorithms.md).

## Ứng Viên Đang Phát Triển (chưa promote)

Working tree hiện có bốn thành phần mới, tắt/bật được qua cấu hình: quiescence và df-pn guard **bật**,
eval_v2 và position book **bật** trong `engine_config.json` (mặc định profile vẫn tắt). Book hiện là
"sổ lặp lại ván thắng" dựng từ log giải 0917/0928 (đối thủ đi tất định: 86/90 ván trùng nhau giữa hai giải). Chúng **chưa** đi qua pipeline đánh giá chuẩn (cần baseline theo luật mới trước).

| Thành phần | Khoá cấu hình | Vai trò | Bằng chứng thử nghiệm (ngoài pipeline) |
|---|---|---|---|
| Quiescence | `qsearch_depth=4` | Ở horizon chỉ xét capture và nước đưa ứng viên Lion vào hàng Try, có stand-pat | +130 Elo (0,2 s/nước) và +91 Elo [+41, +145] (3 s/nước, 188 ván, cấu hình contest) so với bản hiện tại. Quiescence + guard vs bản gốc 3bf1758, 10 s/nước, 120 ván qua referee: +89 Elo [+27, +157] (đi trước +168, đi sau +17) |
| df-pn guard song song | `dfpn_guard_enabled`, `dfpn_guard_max_ply=23` | Luồng df-pn chứng minh nước gốc nào để đối thủ thắng cưỡng bức và loại nó khỏi alpha-beta; dự trữ ~10% để search lại | Trên 75 thế cờ từ các ván thua: lựa chọn đã được chứng minh thua giảm từ 9 xuống 2 (trong 41 thế cờ chưa thua sẵn); thời gian quyết định tối đa 28,9 s |
| Position book | `book_enabled`, `book_path` | Bảng thế cờ→nước tính trước ngoại tuyến (`tools/book_builder.cpp`), khoá chuẩn độc lập đánh số quân | Sinh từ thế cờ của giải 0917/0928; chỉ lưu các nước khác với nước bot đã đi. Bản mở rộng cho lượt đi sau không cải thiện khi kiểm chứng nên đang **tắt** |
| Hàm chấm điểm tuyến tính v2 | `eval_v2_enabled`, `eval_v2_weights` | 25 yếu tố theo góc nhìn bên đi (thêm an toàn Sư tử: ô thoát, bảo vệ, áp lực, doạ thả quân; tách "thắng ngay"/"bị doạ"), trọng số dò bằng Texel trên tự đấu | Trọng số legacy khớp từng điểm với hàm cũ (test). Trọng số Texel: +138 Elo (0,2 s) và +97 Elo [+47, +151] (1 s) trong đấu nhanh không guard. Qua referee (10 s/nước, guard bật, 120 ván): +86 Elo [+24, +154], đi trước +260, đi sau −53 [−147, +35] |

Giới hạn đã biết: df-pn guard cần luồng thứ hai (máy chấm cần ≥2 nhân để không chia thời gian với
alpha-beta); phần lớn các ván thua ở giải được định đoạt bởi chuỗi dài hơn 25–35 nước, ngoài tầm
của cả hai cơ chế trong 29 s. Hoà được ban tổ chức tính −0,5 cho cả hai bên; engine vẫn chấm hoà = 0
(contempt đã cân nhắc và quyết định không dùng vì hoà chỉ xảy ra ở lượt 256).

## Việc Tiếp Theo Và Điều Kiện Promote

Hiện không có candidate mới trong permanent registry. `Stage 5.1` trong ví dụ lệnh build/evaluation
chỉ là tên mẫu cho version kế tiếp, không phải stage đã hoàn thành.

Các hướng đang để mở:

- đo và tối ưu move generation bằng benchmark tập trung;
- phân tích ordering của workload `duplicate_hands` nếu vẫn không ổn định ở depth 11;
- chỉ xem xét generation cache, `L_eq` cache hoặc full quotient khi có benchmark chứng minh lợi ích.

Một thay đổi thuật toán chỉ được ghi là stage/champion mới sau khi:

1. Có test đối chiếu với baseline hoặc reference đúng.
2. Build Release và toàn bộ test liên quan đều pass.
3. Có benchmark lặp lại được cho workload bị tác động.
4. Candidate được freeze với binary, config, manifest và source commit bất biến.
5. Permanent evaluation report đạt acceptance policy.
6. `current_champion` trong registry được cập nhật bằng quy trình promote.

## Cách Cập Nhật Trang Này

Khi thuật toán hoặc version thay đổi, cập nhật cùng patch theo thứ tự:

1. Ngày `Last reviewed` và bảng Current Snapshot.
2. Bảng bật/tắt theo **config của artifact**, không chỉ theo default trong source.
3. Dòng stage mới cùng link tới test, benchmark và evaluation report.
4. Các giới hạn hoặc tính năng vẫn tắt.
5. Link từ [`README.md`](../README.md) và trang giải thích bot nếu catalog thay đổi.
