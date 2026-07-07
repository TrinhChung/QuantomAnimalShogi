# RAM and transposition-table benchmark

`TTEntry` is 24 bytes. Requested sizes round down to a power-of-two entry count.
Depth-4 initial-state measurements:

| Requested | Allocated | Entries | Nodes | Time ms | TT hit/probe |
|---:|---:|---:|---:|---:|---:|
| 64 MiB | 48 MiB | 2,097,152 | 702 | 10.85 | 49/205 |
| 128 MiB | 96 MiB | 4,194,304 | 702 | 10.57 | 49/205 |
| 256 MiB | 192 MiB | 8,388,608 | 702 | 10.47 | 49/205 |

Calculated-only levels (not allocated, to avoid unsafe multi-gigabyte benchmark
allocation on the current host): 512→384 MiB, 1024→768 MiB, 2048→1536 MiB,
4096→3072 MiB.

No depth-4 benefit appeared above 64 MiB; differences are timing noise. Defaults:

- `low_ram`: requested 128 MiB, fixed.
- `local_debug`: 64 MiB, fixed.
- `contest_safe`: auto-size at most 25% of capped RAM after a 512 MiB reserve.
- `contest_high_ram`: auto-size at most 35% after a 1024 MiB reserve.

High RAM has not demonstrated a speed/strength gain yet. Use 256 MiB as the
practical contest-safe target until depth/time-limited match benchmarks prove a
larger TT improves hit rate. OS RAM is queried once during initialization, never
during search.
