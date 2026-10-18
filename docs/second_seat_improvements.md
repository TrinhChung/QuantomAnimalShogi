# Các thay đổi trong đợt cải thiện engine (10/2026)

Tài liệu này giải thích từng thay đổi chưa commit trên nhánh làm việc: thay đổi là gì, gồm những gì, giải quyết
vấn đề gì, vấn đề đó ảnh hưởng ra sao, và đã giải quyết tới đâu (kèm số đo). Thứ tự đi từ hạ tầng kiểm thử, tới
tìm kiếm, hàm chấm điểm, dữ liệu tính trước, rồi các phần còn ở dạng thử nghiệm.

Quy ước số đo:

- **Elo**: so sánh sức mạnh giữa hai phiên bản (thắng 1, hoà 0,5, thua 0), kèm khoảng tin cậy 95% trong `[ ]`.
- **Điểm theo luật giải**: thắng +1, hoà −0,5, thua −1 (docs/RULE_GAME.md mục 8), tính trung bình mỗi ván.
- **Trận chuẩn khi đi sau**: bot luôn cầm phía đi sau, xuất phát từ thế khởi đầu, 5 s/nước, gặp hai đối thủ
  (bản hàm chấm điểm cũ và bản hiện tại) có đa dạng hoá 3 nước đầu; 50 ván mỗi đối thủ.

## Tổng quan trạng thái

| # | Thay đổi | Trạng thái trong `engine_config.json` |
|---|---|---|
| 1 | Fixture và corpus theo luật mới | — (hạ tầng kiểm thử) |
| 2 | Sửa referee: cờ gốc quân tuyệt đối | — (hạ tầng kiểm thử) |
| 3 | Điểm theo luật giải trong báo cáo trận | — (hạ tầng kiểm thử) |
| 4 | Quiescence search | **Bật** (`qsearch_depth: 4`) |
| 5 | df-pn và guard song song | **Bật** (`dfpn_guard_enabled`, `dfpn_guard_max_ply: 23`) |
| 6 | Hàm chấm điểm tuyến tính v2 (Texel) | **Bật** (`eval_v2_enabled`, `eval_v2_weights`) |
| 7 | Sổ lặp lại ván thắng | **Bật** (`book_enabled`, `book/position_book.txt`) |
| 8 | Tìm `engine_config.json` cạnh exe | Luôn có hiệu lực |
| 9 | Danh sách nước cấm | **Tắt** (`forbidden_enabled: false`) |
| 10 | NNUE (phần hiệu chỉnh trên v2) | **Tắt** (`nnue_enabled: false`) |
| 11 | Đa dạng hoá khai cuộc cho đối thủ thử nghiệm | **Tắt**, chỉ dùng khi đo |
| 12 | Điểm hoà trong tìm kiếm giữ bằng 0 | Quyết định ghi lại, không đổi hành vi |
| 13 | Tài liệu và quy ước (`CLAUDE.md`, `AGENTS.md`) | — |

Mặc định của profile `contest_safe` (khi không có file cấu hình) vẫn **tắt** eval_v2, sổ, danh sách nước cấm và
NNUE; chỉ quiescence và guard bật. Vì vậy thay đổi 8 rất quan trọng.

---

## 1. Fixture và corpus theo luật mới

**1. Thay đổi này là gì**
Cập nhật dữ liệu kiểm thử (fixture) và bộ thế cờ khai cuộc dùng trong đánh giá (corpus) cho khớp với luật hiện
hành.

**2. Nội dung**
- `benchmarks/stage35_fixture.cpp`: bộ sinh fixture kiểm tra thêm tính chất bắt buộc (`require_property`,
  `first_matching_fixture`), nhận biết cặp quân trên tay giống nhau và thế có Try cưỡng bức.
- `benchmarks/equivalence_benchmark.cpp`: quân trên tay mất khả năng là Sư tử, đúng luật mới.
- `evaluation/tools/corpus.py`: `CORPUS_REVISION = 2`, file `corpus-v2.jsonl` có kèm chuỗi nước đi;
  `evaluation/corpus/manifest.json`, `evaluation/README.md`, `tests/evaluation/test_framework.py` đọc tên file
  corpus từ manifest.

**3. Giải quyết vấn đề gì**
Fixture và corpus cũ được sinh theo luật trước khi đổi (Catch, Try trễ, quân Sư tử trên tay).

**4. Vấn đề và ảnh hưởng**
Bộ sinh fixture âm thầm chọn những thế không còn đúng tính chất cần kiểm tra; corpus cũ chứa thế cờ không hợp lệ
theo luật mới. Kết quả là test và các trận đánh giá đo trên dữ liệu sai, có thể cho kết luận sai về engine.

**5. Đã giải quyết thế nào**
Fixture được sinh lại và tự kiểm tra tính chất, corpus v2 được sinh lại theo luật mới. Toàn bộ test hiện đạt
(12/12 CTest, 56 test Python của framework đánh giá).

---

## 2. Sửa referee: cờ gốc quân tuyệt đối

**1. Thay đổi này là gì**
Sửa `evaluation/native/referee_adapter.cpp`, referee dùng cho các trận đấu kiểm chứng cục bộ.

**2. Nội dung**
Trong thông điệp gửi engine, hai trường `is_from_first_player` / `is_from_second_player` giờ là **tuyệt đối**
(quân xuất phát từ người đi trước = 1), đúng docs/RULE_GAME.md mục 4.1. Chỉ hướng bàn cờ và cờ sở hữu là tương đối
theo bên đang đi. Có test hồi quy trong `tests/evaluation/test_framework.py`.

**3. Giải quyết vấn đề gì**
Referee trước đây ghi gốc quân theo bên đang đi, trong khi engine (và ban tổ chức) hiểu là tuyệt đối.

**4. Vấn đề và ảnh hưởng**
Khi bên đi trước tới lượt, hai cách hiểu trùng nhau nên lỗi bị che. Khi bên đi sau tới lượt, mọi gốc quân bị đảo.
Cách chơi của engine không đổi (đảo toàn bộ chỉ là đổi nhãn hai nhóm), nhưng mọi thứ dùng khoá thế cờ có gốc quân
đều không khớp: trận "có danh sách nước cấm vs không" đầu tiên chạy 20 phút mà bảng không loại được nước nào, nên
không đo được gì.

**5. Đã giải quyết thế nào**
Sau khi sửa, bảng nước cấm được áp dụng ngay ở ván đầu. Test hồi quy thất bại với referee cũ và đạt với referee
mới. Các trận cũ đo sức chơi (solo, eval_v2) vẫn giữ giá trị vì cách chơi không bị ảnh hưởng.

---

## 3. Điểm theo luật giải trong báo cáo trận

**1. Thay đổi này là gì**
Thêm chỉ số "điểm theo luật giải" vào báo cáo kết quả trận.

**2. Nội dung**
`evaluation/tools/summarize_results.py` thêm `candidate_contest_reward` và `opponent_contest_reward`
(thắng +1, hoà −0,5, thua −1). Elo và các ngưỡng chấp nhận vẫn tính như cũ.

**3. Giải quyết vấn đề gì**
Báo cáo chỉ có Elo, tính hoà bằng nửa điểm.

**4. Vấn đề và ảnh hưởng**
Luật giải tính hoà −0,5 cho cả hai bên, gần như thua. Một phiên bản hoà nhiều có thể trông ổn theo Elo nhưng mất
điểm thật khi thi. Không thể dùng thẳng −0,5 cho Elo vì khi đó tổng điểm hai bên không cố định và phép so sánh sức
mạnh sai.

**5. Đã giải quyết thế nào**
Báo cáo hiển thị song song Elo (so sức mạnh) và điểm theo luật giải (điểm thật). Các quyết định gần đây đều xem cả
hai, ví dụ NNUE vòng 1 tăng điểm khi đi sau chủ yếu nhờ chuyển thua thành hoà.

---

## 4. Quiescence search

**1. Thay đổi này là gì**
Thêm tìm kiếm tĩnh lặng (quiescence) ở cuối nhánh alpha-beta.

**2. Nội dung**
`AlphaBetaEngine::quiescence` trong `src/search/alpha_beta.cpp`: khi hết độ sâu, tiếp tục xét các nước bắt quân
và nước đưa ứng viên Sư tử vào hàng Try, có stand-pat, tối đa `qsearch_depth` lớp (cấu hình thi đấu: 4).

**3. Giải quyết vấn đề gì**
Hiệu ứng chân trời: chấm điểm thế cờ ngay giữa một chuỗi đổi quân.

**4. Vấn đề và ảnh hưởng**
Engine cũ dừng đúng lúc vừa bắt quân và tưởng mình lời, trong khi nước sau đối thủ bắt lại hoặc Try. Kết quả là
chọn nước thua quân hoặc thua ván do không thấy đòn đáp trả ngay sau chân trời.

**5. Đã giải quyết thế nào**
So với bản trước đó: +130 Elo ở 0,2 s/nước và +91 Elo [+41, +145] ở 3 s/nước (188 ván). Có test xác nhận
quiescence thấy được nước bắt lại và không làm thay đổi trạng thái gốc.

---

## 5. df-pn và guard song song

**1. Thay đổi này là gì**
Thêm bộ chứng minh df-pn và một luồng "guard" chạy song song với alpha-beta.

**2. Nội dung**
- `src/search/dfpn.cpp`, `include/search/dfpn.hpp`: df-pn đủ bề rộng cho thắng cưỡng bức (Catch/Try), bảng băm
  hai ngăn ưu tiên mục đã chứng minh, kỹ thuật 1+ε, giới hạn theo đồng hồ, cờ dừng; `verify_proof` kiểm lại chứng
  minh chỉ bằng hàm luật.
- `src/search/guarded_search.cpp`, `include/search/guarded_search.hpp`: luồng phụ thử tìm thắng cưỡng bức của
  mình, rồi lần lượt chứng minh nước nào ở gốc để đối thủ thắng cưỡng bức và loại nó khỏi alpha-beta ngay khi
  đang tìm; giữ khoảng 10% thời gian để tìm lại nếu nước đã chọn bị bác bỏ muộn.
- `SearchOptions::root_excluded` và `RootGuard` trong `alpha_beta`: cơ chế loại nước ở gốc.
- Sửa lỗi: khi tìm lại ở phút cuối, guard trước đây ghi đè danh sách loại tĩnh (ví dụ danh sách nước cấm); giờ hai
  danh sách được gộp, và luồng phụ không tốn thời gian chứng minh lại các nước đã bị loại sẵn.

**3. Giải quyết vấn đề gì**
Những đòn ép buộc dài mà alpha-beta với hàm chấm điểm không nhìn thấy.

**4. Vấn đề và ảnh hưởng**
Phân tích log giải 0917/0928 cho thấy nhiều ván thua do bot đi vào thế mà đối thủ có chuỗi ép buộc dẫn tới bắt Sư
tử; bot vẫn chấm thế đó là cân bằng cho tới khi quá muộn.

**5. Đã giải quyết thế nào**
Trên 41 thế cờ chưa thua sẵn lấy từ các ván thua, số lần bot chọn nước đã bị chứng minh thua giảm từ 9 xuống 2.
Thời gian quyết định tối đa đo được 28,9 s (giới hạn 30 s). Quiescence + guard so với bản gốc 3bf1758 (10 s/nước,
120 ván qua referee): **+89 Elo [+27, +157]**, đi trước +168, đi sau +17. Giới hạn: guard cần ít nhất 2 nhân CPU,
và chỉ thấy được chuỗi trong khoảng 23 nước.

---

## 6. Hàm chấm điểm tuyến tính v2 (Texel)

**1. Thay đổi này là gì**
Thay hàm chấm điểm cũ bằng hàm tuyến tính 25 yếu tố có trọng số dò từ dữ liệu.

**2. Nội dung**
- `src/search/eval_features.cpp`, `include/search/eval_features.hpp`: 25 yếu tố tính theo góc nhìn bên đang đi:
  vật chất, số danh tính còn khả năng, độ cơ động, Sư tử bị tấn công, tiến độ Try, số ứng viên Sư tử, tách riêng
  "mình thắng ngay" và "đối thủ doạ thắng", và các yếu tố an toàn Sư tử mới (ô thoát, quân bảo vệ, áp lực quanh Sư
  tử, doạ thả quân), quân bị treo, quân trên tay, lượt đi.
- `legacy_eval_weights()` cho ra kết quả trùng từng điểm với hàm cũ (có test).
- Trọng số đang dùng trong `engine_config.json` được dò bằng Texel tuning trên 3000 ván tự đấu.

**3. Giải quyết vấn đề gì**
Hàm chấm điểm cũ gần như không nhận ra thế cờ đã thua ngầm.

**4. Vấn đề và ảnh hưởng**
Trên các thế cờ df-pn đã chứng minh thua, hàm cũ vẫn chấm gần cân bằng. Các yếu tố an toàn Sư tử (ô thoát, quân áp
sát) phân biệt thế thua với thế chưa thua rõ nhất, nhưng hàm cũ không đo chúng. Ngoài ra, cộng ±3500 cho "thắng
ngay" mà không phân biệt bên đang đi làm điểm dao động theo lượt.

**5. Đã giải quyết thế nào**
Log-loss dự đoán kết quả giảm từ 0,664 xuống 0,618. Đấu với hàm cũ: +138 Elo (0,2 s), +97 Elo (1 s), và qua
referee ở 10 s/nước với guard bật **+86 Elo [+24, +154]** (đi trước +260, đi sau −53 [−147, +35]). Vòng dò thứ hai
chỉ thêm +2 Elo, tức trọng số tuyến tính đã hội tụ. Giới hạn: không cải thiện rõ khi đi sau.

---

## 7. Sổ lặp lại ván thắng

**1. Thay đổi này là gì**
Một sổ khai cuộc dựng từ chính lịch sử thi đấu, giúp bot lặp lại các ván đã thắng và rời các đường đã thua.

**2. Nội dung**
- `src/book/position_book.cpp`, `include/book/position_book.hpp`: bảng thế cờ → nước đi, khoá không phụ thuộc cách
  đánh số quân, chỉ trả về nước hợp lệ; đường quyết định trong `main.cpp`: sổ → guard → kiểm tra action mask.
- `book/position_book.txt` (1910 thế cờ, dựng từ 93 ván khác nhau của giải 0917 và 0928):
  - ván thắng và ván hoà: mọi nước của mình đúng như trong ván cũ;
  - ván thua: đi lại nước cũ tới điểm muộn nhất còn rẽ an toàn được, rồi đi nước đã được df-pn (45 nước, 60 s)
    xác nhận không bị chứng minh thua; không bao giờ rẽ ở thế cờ nằm trên đường của một ván thắng/hoà;
  - thế cờ mà bot cũ từng đi nhiều nước khác nhau: chọn nước giữ được nhiều ván thắng nhất.
- `book/position_book_old40.txt`: sổ cũ 40 thế, giữ để tham khảo.
- `tools/book_builder.cpp`: công cụ dựng sổ theo phân tích sâu (dạng cũ).
- Công cụ dựng sổ lặp lại ván thắng hiện nằm ngoài repo (`qas-experiments/evalv2`: `make_history.py`,
  `history_book.cpp`, `line_fix.cpp`, `deviation_candidates.py`, `history_check.py`).

**3. Giải quyết vấn đề gì**
Giữ các kết quả tốt đã có trước các đối thủ đã biết, và sửa các ván đã thua.

**4. Vấn đề và ảnh hưởng**
- Đối thủ trong giải là bot đi nước cố định, và hai giải dùng cùng bộ bot: 86/90 ván giống hệt nhau giữa 0917 và
  0928. Bot gốc thắng 84% khi đi trước (38/3/4) và 67% khi đi sau (30/4/11).
- Mọi cải tiến (quiescence, guard, v2) đều làm bot đi khác bot cũ. Không có sổ, bot mới rẽ khỏi lịch sử ngay ở
  lượt 2 khi đi trước, nên kết quả của toàn bộ các ván từng thắng không còn được bảo đảm.
- Ngay cả bot gốc chạy trên máy khác cũng không lặp lại được ván cũ: nó tìm kiếm theo giờ, máy nhanh chậm khác nhau
  thì độ sâu và nước đi khác nhau (đã đo: chỉ 8/90 ván lặp lại trong 40 nước đầu).

**5. Đã giải quyết thế nào**
Kiểm chứng bằng cách cho chính `qas.exe` (cấu hình thi đấu) đi lại toàn bộ lịch sử:

| Nhóm | Kết quả |
|---|---|
| Ván thắng lặp lại trọn vẹn | 57/70 |
| Ván thắng rẽ ở thế bot cũ từng đi khác nhau (không tránh được) | 13/70 |
| Ván hoà lặp lại trọn vẹn | 7/7 |
| Ván thua rời đường thua cũ | 14/16 |
| Ván thua giữ nguyên (QEH, ZYA) | 2/16, để bảo vệ hai ván thắng OYV và TYM đi qua cùng thế cờ |

Trong quá trình làm đã sửa một lỗi: hàm khôi phục nước từ log bỏ qua gốc quân, nên khi trên tay có hai quân cùng
danh tính khác gốc thì chọn nhầm. Giới hạn: sổ chỉ có tác dụng nếu giải sau vẫn dùng bộ bot cũ; nếu đối thủ đổi,
thế cờ không khớp và bot tự đánh như bình thường.

---

## 8. Tìm `engine_config.json` cạnh exe

**1. Thay đổi này là gì**
Sửa cách `qas.exe` tìm file cấu hình khi không có tham số `--config`.

**2. Nội dung**
`src/main.cpp`: tìm `engine_config.json` ở thư mục hiện tại, nếu không có thì tìm bên cạnh `qas.exe`; log
`config=...` cho biết đã dùng file nào. `CMakeLists.txt` chép `engine_config.json` và thư mục `book/` ra cạnh
`qas.exe` khi build. Mặc định của profile tắt sổ cho khớp với cấu hình (`include/core/config.hpp`).

**3. Giải quyết vấn đề gì**
Engine chỉ tìm cấu hình ở thư mục hiện tại và im lặng dùng mặc định nếu không thấy.

**4. Vấn đề và ảnh hưởng**
Nếu bot được khởi chạy từ thư mục không chứa `engine_config.json`, engine chạy với mặc định: tắt eval_v2, tắt sổ,
tắt danh sách nước cấm. Toàn bộ cải tiến ở mục 6 và 7 mất tác dụng mà không có thông báo lỗi.

**5. Đã giải quyết thế nào**
Đã thử chạy `qas.exe` từ thư mục build và từ một thư mục bất kỳ: cả hai đều nạp đúng cấu hình
(`eval_v2=1`, `book entries=1910`). Vẫn nên hỏi ban tổ chức cách khởi chạy bot để chắc chắn.

---

## 9. Danh sách nước cấm (đang tắt)

**1. Thay đổi này là gì**
Danh sách các nước đã được chứng minh thua ở giai đoạn đầu khi đi sau, loại khỏi lựa chọn ở gốc.

**2. Nội dung**
- `src/book/forbidden_moves.cpp`, `include/book/forbidden_moves.hpp`: đọc danh sách `<khoá thế cờ> <nước>`,
  trả về các nước hợp lệ bị cấm; `main.cpp` đưa chúng vào `root_excluded`. Không bao giờ ép một nước cụ thể; nếu mọi
  nước đều bị cấm thì giữ tất cả.
- `book/forbidden_moves.txt`: 3035 nước cấm, dựng bằng df-pn ngoại tuyến (35–45 nước) trên cây khai cuộc khi đi sau
  tới lượt 15, cộng các đường thua thật trong giải.
- Khoá cấu hình `forbidden_enabled`, `forbidden_path` (mặc định tắt).

**3. Giải quyết vấn đề gì**
Các ván đi sau bắt đầu thua rất sớm (lượt 3–13) bằng chuỗi dài hơn tầm nhìn của guard lúc thi.

**4. Vấn đề và ảnh hưởng**
df-pn ngoại tuyến cho thấy 9/11 ván thua khi đi sau bắt đầu thua ở lượt 7–13, bằng những nước mà guard (23 nước,
khoảng 1–2 s) không chứng minh được khi đấu.

**5. Đã giải quyết tới đâu**
Danh sách phủ cả 9 kịch bản thua thật đã biết (kiểm chứng độc lập). Nhưng trong trận chuẩn khi đi sau, bật danh
sách không làm tăng điểm: −0,22 [−0,55, +0,11] trước đối thủ yếu và +0,06 [−0,04, +0,16] trước đối thủ ngang sức.
Nguyên nhân hợp lý: ở thế đã thua sẵn, cấm nước này chỉ đẩy sang nước thua khác. Vì không có bằng chứng cải thiện,
danh sách đang **tắt**. Phần lớn tác dụng của nó với các đường thua đã biết nay do sổ lặp lại ván thắng đảm nhận.

---

## 10. NNUE (đang tắt)

**1. Thay đổi này là gì**
Một mạng nơ-ron nhỏ cộng thêm phần hiệu chỉnh vào điểm của eval_v2. Nhóm đã đồng ý cho dùng (xem `AGENTS.md`).

**2. Nội dung**
- `src/search/nnue_features.cpp`, `include/search/nnue_features.hpp`: mã hoá thế cờ từ góc nhìn bên đi, mỗi quân
  một đặc trưng (vị trí × chủ × gốc × tập danh tính), 1664 đặc trưng, mỗi thế bật đúng 8.
- `src/search/nnue.cpp`, `include/search/nnue.hpp`: mạng 1664 → 64 → 32 → 1, tính bằng số thực trên CPU, nạp từ file
  văn bản, từ chối file sai định dạng. Chỉ có tác dụng khi eval_v2 đang bật.
- Khoá cấu hình `nnue_enabled`, `nnue_path` (mặc định tắt). Có test: mã hoá đối xứng hai phía, nạp file, mạng toàn
  số 0 cho kết quả tìm kiếm trùng v2.
- Công cụ sinh dữ liệu và huấn luyện nằm ngoài repo (`qas-experiments/evalv2`: `nnue_gen*.cpp`, `nnue_train.py`).

**3. Giải quyết vấn đề gì**
Hàm chấm điểm tuyến tính đã chạm trần và vẫn kém trong việc nhận ra thế nguy hiểm.

**4. Vấn đề và ảnh hưởng**
Bot đi sau gặp đối thủ ngang sức thua nhiều; các thế thua ngầm ở ngoài vùng tính trước không được nhận ra.

**5. Đã giải quyết tới đâu**
- Vòng 1 (6000 ván tự đấu): trong trận chuẩn khi đi sau, trước đối thủ ngang sức **+0,15 điểm/ván [+0,06, +0,24]**,
  trước đối thủ yếu +0,12 [−0,21, +0,45]; số ván thua giảm từ 52 xuống 25 trên 100 ván. Trên 9 đường thua thật ở
  25 s/nước, tự tránh được 8/9 (v2: 6/9).
- Nhưng trên thế cờ của ván giải thật (18 ván để riêng), NNUE vòng 1 không dự đoán tốt hơn v2 (log-loss 0,652 so với
  0,641).
- Vòng 2 (15.000 ván trộn engine mạnh yếu khác nhau) thất bại: dự đoán tự đấu tốt hơn nhưng đánh tệ hơn nhiều
  (−0,91 điểm/ván). Giả thuyết: kết quả ván phụ thuộc vào engine cầm quân, mạng học sai tín hiệu.
- Chưa bật vì cần trận xác nhận ở 25 s/nước, đánh cả hai phía.

---

## 11. Đa dạng hoá khai cuộc cho đối thủ thử nghiệm (chỉ dùng khi đo)

**1. Thay đổi này là gì**
Tuỳ chọn cấu hình chỉ dành cho đối thủ trong các trận đo, không bao giờ bật cho bản thi.

**2. Nội dung**
`src/search/diversify.cpp`, `include/search/diversify.hpp`; khoá `opening_random_plies`, `opening_random_margin`,
`opening_random_depth`. Trong vài nước đầu, chọn ngẫu nhiên trong các nước kém nước tốt nhất không quá một ngưỡng
điểm (đang dùng 100).

**3. Giải quyết vấn đề gì**
Trận đo xuất phát từ thế khởi đầu luôn lặp lại y hệt vì engine đi nước cố định.

**4. Vấn đề và ảnh hưởng**
Không có đa dạng hoá thì 100 ván chỉ là vài ván lặp lại, kết quả không có ý nghĩa thống kê. Thử chọn ngẫu nhiên
trong mọi nước hợp lệ thì đối thủ đi quá tệ, bot thắng 95% và trận đo không phân biệt được phiên bản nào.

**5. Đã giải quyết thế nào**
Trận chuẩn khi đi sau dùng tuỳ chọn này cho đối thủ, nên mỗi ván khác nhau mà đối thủ vẫn đánh hợp lý. Có test xác
nhận nước chọn luôn hợp lệ và ngưỡng 0 chỉ giữ các nước tốt nhất.

---

## 12. Điểm hoà trong tìm kiếm giữ bằng 0

**1. Thay đổi này là gì**
Một quyết định thiết kế được ghi lại trong `terminal_score` (`src/search/alpha_beta.cpp`), không đổi hành vi.

**2. Nội dung**
Chú thích giải thích vì sao engine vẫn chấm hoà = 0 dù luật giải tính hoà −0,5.

**3. Giải quyết vấn đề gì**
Một điểm trông như mâu thuẫn với luật giải.

**4. Vấn đề và ảnh hưởng**
Đã cân nhắc thêm "contempt" (coi hoà là xấu). Hoà chỉ xảy ra ở lượt 256 nên contempt chỉ ảnh hưởng vài chục nước
cuối của các ván rất dài, và có rủi ro biến hoà thành thua.

**5. Đã giải quyết thế nào**
Quyết định không dùng contempt; ghi chú tại chỗ để người đọc code không tưởng là quên.

---

## 13. Tài liệu và quy ước

**1. Thay đổi này là gì**
Cập nhật tài liệu cho người phát triển và quy ước dự án.

**2. Nội dung**
- `CLAUDE.md` (mới): lệnh build/test, kiến trúc, các khoá cấu hình, đường quyết định của engine.
- `AGENTS.md`: thay dòng cấm học máy bằng điều kiện cho phép NNUE (chỉ CPU, bật/tắt được, luôn có hàm chấm điểm cổ
  điển dự phòng, chỉ dùng khi thắng bản hiện tại).
- `docs/current_stage_algorithms.md`: bảng các thành phần đang phát triển và kết quả đo.
- Tài liệu này.

**3–5.** Giúp người mới đọc code biết thành phần nào đang bật, vì sao, và đã được đo ra sao, thay vì phải lần lại
lịch sử trao đổi.

---

## Bản build, gói phát hành và cách dùng

**Nộp bài thi** cần đúng ba thứ đặt chung một thư mục: `qas.exe` (đổi tên tuỳ ý), `engine_config.json` và thư mục
`book/`. CMake tự chép `engine_config.json` và `book/` ra cạnh `qas.exe` khi build
(`build/current/Release/`). Khi chạy, bot in ra stderr:

```
config=...\engine_config.json        file cấu hình đã nạp; "(none found, profile defaults)" là thiếu file
profile=contest_safe ... eval_v2=1 dfpn_guard=on
book entries=1910                     sổ lặp lại ván thắng đã được nạp
```

**Trọng tài cục bộ** `qas_evaluation_referee.exe` cũng được build cùng dự án. Nó chỉ dùng để tự cho các bản bot đấu
với nhau trên máy (các script trong `evaluation/tools` gọi nó); khi thi, ban tổ chức dùng trọng tài riêng, nên không
nộp file này. Chỉ bản build từ commit có thay đổi 2 trở về sau mới gửi đúng gốc quân cho phía đi sau; trọng tài
build từ commit cũ (ví dụ bản gốc 3bf1758) vẫn còn lỗi, không nên dùng để đo.

**Gói phát hành 18/10/2026** (ngoài repo, để chia sẻ trong nhóm): `qas-release/20261018_8ccd3ef/` và file nén
`qas_release_20261018_8ccd3ef.zip`, build từ commit `8ccd3ef`:

| Đường dẫn trong gói | Nội dung |
|---|---|
| `bot/qas_v2.exe` | bot thi đấu (cùng bản build với `qas.exe`, chỉ đổi tên) |
| `bot/engine_config.json`, `bot/book/` | cấu hình và dữ liệu, bắt buộc đặt cạnh exe |
| `tools/qas_evaluation_referee_v2.exe` | trọng tài cục bộ đã có thay đổi 2 |
| `README.md` | hướng dẫn ngắn cho nhóm |

Máy chạy cần Windows x64 và Microsoft Visual C++ Redistributable 2015–2022 (x64), vì các exe được build bằng MSVC với
runtime động.

## Việc còn mở

1. **Trận xác nhận NNUE vòng 1 vs v2 ở 25 s/nước**, cả hai phía, trước khi quyết định bật NNUE.
2. **Mốc dừng cứng 29 s**: có 2/400 ván thua vì quá giờ trong trận đo (trễ 2–4 s do máy quá tải; chạy lại thì engine
   luôn dừng đúng giờ). Đề xuất hạ xuống 27 s để có biên an toàn 3 giây; chưa đổi vì cần nhóm quyết định.
3. **Đưa công cụ dựng sổ và dữ liệu NNUE vào repo** (`tools/`), để cập nhật sổ khi có log giải mới.
4. **Hỏi ban tổ chức**: bot được khởi chạy thế nào (thư mục hiện tại, tham số), có cho dùng nhiều luồng không.
