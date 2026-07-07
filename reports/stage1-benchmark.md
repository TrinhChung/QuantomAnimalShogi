# Stage 1 benchmark

Environment: Windows x64, GCC 16.1, C++17, `-O3 -DNDEBUG`, static executable.
Mỗi số thời gian là median của 7 lần chạy, fixed depth 4 (iterative depths 1..4
được tính vào counters).

| Position | AB nodes | AB ms | AB+TT nodes | AB+TT ms | Score |
|---|---:|---:|---:|---:|---:|
| initial | 1257 | 80.886 | 838 | 53.994 | -245 |
| safe_4ply | 282 | 10.988 | 325 | 12.680 | 999997 |
| safe_8ply | 407 | 18.487 | 407 | 18.238 | -999996 |
| duplicate_hands | 2365 | 235.553 | 2400 | 240.225 | 36 |

Tổng: AB 4311 nodes / 345.914 ms; AB+TT 3970 nodes / 325.137 ms. TT giảm
khoảng 7.9% node và 6.0% thời gian trên tập nhỏ này. TT hit/probe của một lần chạy:
initial 47/247, safe_4ply 17/70, safe_8ply 34/114, duplicate_hands 97/749.

Correctness:

- Mọi output benchmark là legal (`legal=1`).
- AB và AB+TT giữ nguyên root score và best move trên cả bốn position.
- Apply/undo, hash, propagation, Catch/Try/draw và depth 1/2/3 đều qua unit test.
- AB+TT depth 3 thắng random-legal baseline 20-0, đổi bên mỗi game; 0 illegal,
  0 draw. Đây là sanity benchmark, không thay thế official sample player.
