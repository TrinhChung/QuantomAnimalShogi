# Stage 3 recommendation

> Historical tuning note. Trạng thái accepted hiện tại và các thuật toán đang bật được theo dõi tại
> [Current Stage Algorithm Status](current_stage_algorithms.md).

- Default profile: `contest_safe`.
- Practical TT target: 256 MiB requested (192 MiB allocated after power-of-two
  rounding with the current 24-byte entry). Larger tables showed no depth-4 gain.
- Main depth booster: PVS plus TT move, killer/history and static ordering.
- Aspiration: enabled; depth 1–3 equivalence tests are stable, with bounded retries
  and full-window fallback.
- L_eq: trigger-based only (`depth>=4`, `L>=24`, duplicate hand hint,
  duplicate reduction `>=0.25`, not low-time). Seven-run Stage 3.5 A/B reduced
  aggregate depth-10 nodes by 12.6% across seven workloads while eliminating the
  observed high-uncertainty node regression. Never enable always-on by default.
- High RAM: supported through auto-sizing, but no measured strength gain yet.
- Competition mode: RAM is detected once at startup; resource profiling, CSV,
  skipped-move verification and expensive assertions remain disabled in search.

Normal search reuses per-ply move/candidate/score buffers reserved at startup.
Stage 4 optimized evaluation with reusable scratch storage, mask tables, 12-bit
occupancy/attack summaries, and an exact Catch/Try candidate prefilter. On the
five-position seven-run depth-10 suite this reduced aggregate eval time by 79.5%
and total time by 56.8%, while preserving nodes, moves, scores, and TT/cutoff
metrics. Next work is focused move-generation optimization, then targeted
`duplicate_hands` ordering analysis. Generation/L_eq caches and the full quotient
branch stay disabled until focused evidence justifies them.
