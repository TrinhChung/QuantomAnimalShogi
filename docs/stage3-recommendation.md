# Stage 3 recommendation

- Default profile: `contest_safe`.
- Practical TT target: 256 MiB requested (192 MiB allocated after power-of-two
  rounding with the current 24-byte entry). Larger tables showed no depth-4 gain.
- Main depth booster: PVS plus TT move, killer/history and static ordering.
- Aspiration: enabled; depth 1–3 equivalence tests are stable, with bounded retries
  and full-window fallback.
- L_eq: trigger-based only (`depth>=2`, `L>=12`, duplicate hand hint, not low-time).
  Never enable always-on by default.
- High RAM: supported through auto-sizing, but no measured strength gain yet.
- Competition mode: RAM is detected once at startup; resource profiling, CSV,
  skipped-move verification and expensive assertions remain disabled in search.

Normal search reuses per-ply move/candidate/score buffers reserved at startup.
Next work should benchmark deeper non-terminal position suites and replace the
rare L_eq grouping allocations with a fixed-capacity representation if profiling
justifies it. Generation/L_eq caches stay disabled until benchmarks justify them.
