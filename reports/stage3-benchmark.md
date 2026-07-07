# Stage 3 search benchmark

Windows x64, GCC 16.1, C++17 `-O3 -DNDEBUG`, fixed depth 4, 262144-entry
TT (6 MiB). All modes returned the same legal best move and root score per
position.

| Mode | Initial nodes/ms | Duplicate-hands nodes/ms | First-cutoff rate (duplicate) | Avg cutoff rank |
|---|---:|---:|---:|---:|
| AB | 1381 / 13.50 | 2622 / 32.19 | 0.810 | 1.904 |
| AB+TT | 890 / 8.52 | 2762 / 31.39 | 0.805 | 1.852 |
| AB+TT+PVS | 690 / 6.49 | 2370 / 27.01 | 0.795 | 1.956 |
| AB+TT+PVS+ordering | 702 / 6.92 | 2104 / 24.22 | 0.834 | 1.553 |
| AB+TT+PVS+ordering+L_eq | 702 / 7.01 | 1583 / 19.72 | 0.823 | 1.601 |

PVS reduced initial nodes by 22.5% relative to AB+TT. Strong ordering materially
improved cutoff quality on the duplicate-hand position. L_eq made zero calls on
ordinary positions and grouped only the duplicate-hand workload (46 calls,
duplicate ratio 4.6%). It therefore remains trigger-only.

Depth probes with AB+TT+PVS+ordering:

| Position | Depth 7 | Depth 8 |
|---|---:|---:|
| initial | 19,007 nodes / 165 ms | 43,807 / 352 ms |
| duplicate_hands | 83,201 / 900 ms | 180,296 / 1,754 ms |
| near_draw | 1,293 / 6 ms | 1,570 / 7 ms |

PVS-disabled/enabled scores match at depths 1–3. Aspiration-disabled/enabled
results also match after retry. Timeout tests use an injected deterministic stop
policy and retain only the last fully completed depth.
