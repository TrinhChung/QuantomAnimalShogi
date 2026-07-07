# Stage 2 L_eq A/B benchmark

Cùng setup Stage 1; median 7 lần, depth 4. So sánh trực tiếp với AB+TT.

| Position | AB+TT nodes/ms | L_eq always nodes/ms | L_eq trigger nodes/ms | Duplicate ratio |
|---|---:|---:|---:|---:|
| initial | 838 / 53.994 | 838 / 55.869 | 838 / 55.093 | 0.0% |
| safe_4ply | 325 / 12.680 | 325 / 13.136 | 325 / 12.600 | 0.0% |
| safe_8ply | 407 / 18.238 | 407 / 19.089 | 407 / 18.424 | 0.0% |
| duplicate_hands | 2400 / 240.225 | 1709 / 159.830 | 1709 / 160.561 | 6.6% |

Ở position có hai hand pieces tương đương, L_eq giảm khoảng 28.8% node và 33.2%
thời gian so với AB+TT. Ở ba position thường, always-on không giảm node và chậm
hơn do grouping. Trigger không chạy canonicalization ở các position đó
(`group_ms=0`) nhưng vẫn gộp duplicate_hands.

Kết luận: dùng trigger-based, không always-on. Default là `L >= 12` cộng duplicate
hand-piece hint. Root score và best move ổn định giữa mọi mode; unit test kiểm tra
depth 1/2 value equality, representative hợp lệ và từng skipped move có cùng
canonical successor sau propagation.
