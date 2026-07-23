# Các Thuật Toán Cơ Bản Được Dùng Cho Bot

Trang này giải thích bot chọn nước như thế nào và thuật toán nào thực sự được áp dụng cho từng bot
trong Web Solo. Luật sinh nước và chuyển trạng thái vẫn thuộc `rules`; thuật toán bot chỉ chọn trong
tập nước hợp lệ đó.

Trạng thái chi tiết của engine C++ được theo dõi tại
[`current_stage_algorithms.md`](current_stage_algorithms.md).

## Bot Hiện Có

| Bot                        | Nơi chạy      | Thuật toán quyết định                                                 | Giới hạn chính                                       |
| -------------------------- | ------------- | --------------------------------------------------------------------- | ---------------------------------------------------- |
| WASM Alpha-Beta · Nhanh    | Trình duyệt   | Negamax Alpha-Beta fixed depth                                        | Depth 2                                              |
| WASM Alpha-Beta · Cân bằng | Trình duyệt   | Cùng thuật toán và evaluator                                          | Depth 4                                              |
| Alpha-Beta · Chuyên sâu    | Trình duyệt   | Cùng thuật toán và evaluator                                          | Depth 8                                              |
| Uniform Random             | Trình duyệt   | Chọn index bằng bộ sinh số giả ngẫu nhiên deterministic               | Không nhìn trước                                     |
| C++ Stage 5 Clean          | Native bridge | Stage 5.0 Clean: ID + Alpha-Beta/PVS + TT + ordering + `L_eq` trigger | Profile `contest_safe`                               |
| C++ Current                | Native bridge | Search C++ của source/build hiện tại                                  | Soft limit 1 giây, hard limit 2 giây, depth tối đa 8 |
| C++ Stage 3.5/5/5.7        | Native bridge | Artifact legacy dùng protocol chung                                   | Không đủ manifest nguồn để khẳng định feature nội bộ |

Ba bot WASM Alpha-Beta không phải ba thuật toán khác nhau. Chúng dùng cùng code Rust và chỉ đổi độ
sâu, nên depth lớn thường nhìn xa hơn nhưng số node có thể tăng theo cấp số nhân.

## 1. Cây Trò Chơi Và Minimax

Từ một state, mỗi nước hợp lệ tạo một state con. Tiếp tục như vậy tạo thành cây trò chơi:

- tầng hiện tại là lượt của bot;
- tầng kế tiếp là lượt đối thủ;
- lá là state kết thúc hoặc state tại giới hạn độ sâu;
- state kết thúc dùng kết quả thắng/thua/hòa, còn lá chưa kết thúc dùng evaluator.

Minimax giả định cả hai bên đều chọn nước tốt nhất. Bot tối đa hóa điểm của mình và giả định đối thủ
sẽ tối thiểu hóa điểm đó. Nếu branching factor trung bình là `b` và độ sâu là `d`, duyệt đầy đủ có
độ phức tạp xấp xỉ `O(b^d)`.

## 2. Negamax

Quantum Animal Shogi là game zero-sum: lợi thế của một bên là bất lợi của bên kia. Negamax viết
Minimax gọn hơn bằng quy tắc:

```text
score(state) = max(-score(child))
```

Sau mỗi nước, góc nhìn đổi sang đối thủ nên điểm của state con được đổi dấu. Cả bot WASM và engine
C++ đều dùng cách biểu diễn này.

## 3. Alpha-Beta Pruning

Alpha-Beta giữ hai biên:

- `alpha`: kết quả tốt nhất bên đang tìm đã chắc chắn đạt được;
- `beta`: giới hạn mà phía đối diện đã có thể ép xuống.

Pseudo-code tương ứng với bot WASM:

```text
alpha_beta(state, depth, alpha, beta):
    nếu terminal: trả điểm thắng/thua/hòa
    nếu depth == 0: trả evaluate(state)

    cho mỗi legal move:
        child = apply(state, move)
        score = -alpha_beta(child, depth - 1, -beta, -alpha)
        alpha = max(alpha, score)
        nếu alpha >= beta: dừng các move còn lại

    trả alpha
```

Nhánh bị cắt không thể làm thay đổi quyết định của node hiện tại. Khi hoàn tất cùng độ sâu và dùng
cùng evaluator, Alpha-Beta cho kết quả Minimax nhưng thường duyệt ít node hơn. Xếp nước tốt lên đầu
không đổi kết quả; nó làm điều kiện cutoff xuất hiện sớm hơn.

## 4. Evaluator: Chấm Điểm Lá Chưa Kết Thúc

### Bot WASM

Evaluator Rust cộng trọng số cho mọi dạng con vật vẫn còn có thể nằm trong mask:

| Dạng     | Trọng số |
| -------- | -------: |
| Chick    |        1 |
| Giraffe  |        4 |
| Elephant |        5 |
| Lion     |      100 |
| Hen      |       10 |

Điểm cuối là tổng quân bên đang sở hữu trừ tổng quân đối phương. Win trả `1000 + depth`, loss trả
`-1000 - depth`, còn draw trả `-500`. Việc cộng phần `depth` khiến bot ưu tiên thắng sớm và trì hoãn
thua. Đây là evaluator nhỏ để chạy trong browser; nó không có mobility, TT, iterative deepening hay
time control.

### Engine C++

Evaluator native dùng nhiều tín hiệu hơn:

- expected material theo quantum mask và độ bất định của mask;
- mobility;
- số Lion candidate, Lion safety/pressure và tiến độ Try;
- nguy cơ Lion thật nằm trong hand;
- khả năng Catch/Try ngay ở lượt kế tiếp.

Stage 4 tối ưu cách tính bằng lookup table, occupancy/attack mask 12-bit, buffer tái sử dụng và
prefilter candidate. Các tối ưu này giữ nguyên ngữ nghĩa/điểm so với evaluator reference đã dùng để
A/B test.

Evaluator chỉ được gọi khi search chưa đi tới terminal. Luật thắng, thua, hòa và propagation không
được xấp xỉ trong evaluator.

## 5. Iterative Deepening Và Quản Lý Thời Gian

Engine C++ tìm lần lượt depth 1, 2, 3, ... thay vì nhảy thẳng tới depth tối đa. Kết quả depth nông:

- tạo một nước fallback hợp lệ;
- cung cấp điểm cho aspiration window;
- làm ấm TT và giúp ordering ở depth sâu hơn.

Chỉ iteration hoàn tất mới được công bố. Nếu hard deadline ngắt depth đang chạy, engine trả kết quả
của depth hoàn tất gần nhất. Bot WASM không dùng cơ chế này: nó luôn chạy đúng fixed depth được chọn.

## 6. Transposition Table Và Zobrist Hash

Nhiều chuỗi nước có thể đi tới cùng một state. Engine C++ dùng Zobrist hash làm key cho
transposition table (TT), lưu:

- key đầy đủ;
- độ sâu đã tìm;
- score;
- loại bound `Exact`, `Lower` hoặc `Upper`;
- best move;
- age để hỗ trợ replacement.

TT có hai lợi ích: dùng lại kết quả đủ sâu và thử best move cũ trước. Hash bao gồm mọi field ảnh
hưởng kết quả, kể cả `side_to_move` và turn vì luật hòa ở turn 256. Bot WASM không có TT.

## 7. Move Ordering, Killer Và History

Engine C++ ưu tiên gần đúng theo nhóm:

1. TT move.
2. Nước thắng ngay hoặc ngăn thua ngay.
3. Capture quan trọng, đặc biệt Lion.
4. Nước làm giảm Lion candidate, tạo Try progress hoặc collapse mask.
5. Killer move và history score của quiet move từng gây beta cutoff.
6. Evaluation preview tại root.

Killer heuristic ghi nhớ quiet move gây cutoff ở cùng ply. History heuristic cộng điểm cho cặp
`from -> to` thường gây cutoff và decay định kỳ. Chúng chỉ thay thứ tự thử nước, không tự loại một
nước hợp lệ.

## 8. PVS Và Aspiration Window

Sau khi ordering tốt, Principal Variation Search (PVS) giả định nước đầu là tốt nhất:

- nước đầu được tìm với full alpha-beta window;
- các nước sau thử bằng null window hẹp;
- nếu một nước sau có vẻ tốt hơn, engine re-search bằng full window.

Aspiration dùng score depth trước làm tâm cho một cửa sổ hẹp ở depth mới. Fail-low/fail-high làm cửa
sổ mở rộng; sau số retry giới hạn, engine quay về full window. Hai kỹ thuật giảm công việc trung
bình nhưng vẫn có đường fallback giữ kết quả Alpha-Beta.

## 9. `L_eq`: Gộp Successor Tương Đương

Trong quantum state, hai physical piece khác nhau đôi khi tạo các nước có cùng ý nghĩa sau khi apply
move và propagation. `L_eq` tạo canonical key cho successor và chỉ giữ một legal representative của
mỗi class.

Đây là phép giảm chính xác, không phải đoán nước yếu. Tuy vậy canonicalization có chi phí, nên
production chỉ bật khi:

- còn ít nhất 4 ply;
- có ít nhất 24 legal move;
- có dấu hiệu duplicate hand piece;
- tỷ lệ giảm đo được ít nhất 25%;
- engine chưa vào low-time mode.

Bot WASM không dùng `L_eq`.

## 10. Lineage Lookup Table

Mỗi origin có bốn lineage `CH/G/E/L` cần được gán nhất quán. Reference thử tối đa `4! = 24` hoán vị
để tìm lineage còn support. Stage 5 precompute kết quả cho toàn bộ key 16-bit của bốn lineage mask
và lookup trong hot path, sau đó vẫn lặp đến fixed point.

Lookup table chỉ thay cách tính propagation; nó không đổi luật. Test exhaustive so sánh LUT với
reference hoán vị, và reference vẫn có thể được chọn trong profiling/differential test.

## 11. Uniform Random Có Seed Cố Định

Bot Random lấy danh sách legal action rồi cập nhật một linear congruential generator:

```text
state = (1664525 * state + 1013904223) mod 2^32
move = legal_moves[state mod legal_move_count]
```

Mỗi lần reset game tạo một seed uint32 mới bằng Web Crypto rồi lưu seed đó cùng trajectory. Vì vậy
dataset có thể phát lại chính xác cùng chuỗi lựa chọn; replay validator cũng kiểm tra action Random
theo seed đã lưu. Bot này không đánh giá bàn cờ, không nhìn trước và không học từ các ván đã chơi.

## Cách Chọn Bot Để Thử Nghiệm

- Chọn **Uniform Random** làm baseline/sanity check, không dùng để đo chiến thuật.
- Chọn **WASM depth 2** để UI phản hồi nhanh.
- Chọn **WASM depth 4** cho cân bằng tốc độ và chất lượng trong browser.
- Chọn **WASM depth 8** khi chấp nhận chờ lâu hơn nhưng vẫn muốn chạy hoàn toàn trong browser.
- Chọn **Stage 5 Clean** để đo accepted native engine.
- Chọn **C++ Current** để thử candidate local; không gọi nó là tốt hơn champion trước khi permanent
  evaluation hoàn tất.
- Chỉ dùng các **legacy Stage** cho so sánh lịch sử. Kết quả self-play không đủ để suy ngược chính
  xác feature nằm trong binary.

Catalog bot được định nghĩa tại
[`quantum-animal-shogi/web/src/game/bots.ts`](../quantum-animal-shogi/web/src/game/bots.ts) và
allowlist native tại
[`quantum-animal-shogi/web/server/native_bot_bridge.mjs`](../quantum-animal-shogi/web/server/native_bot_bridge.mjs).
