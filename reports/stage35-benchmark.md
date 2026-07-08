# Stage 3.5 benchmark and resource-tuning report

Run date: 2026-07-08 (Asia/Tokyo). Host: Windows x64, 15.4 GiB physical RAM,
GCC 16.1.0, C++17 Release (`-O3 -DNDEBUG`). Search timing includes
benchmark-only counters and an external 10 ms RSS sampler. Competition profiles
do not enable this profiling.

## 1. Executive summary

- Best aggregate mode in the depth-9 pilot: `AB+TT+PVS+ordering+L_eq_trigger`.
- Safe production recommendation now:
  `AB+TT+PVS+ordering+L_eq_trigger`, 256 MiB requested TT (192 MiB allocated),
  iterative deepening, aspiration enabled. The tuned L_eq trigger is
  `legal_count>=24`, `depth_remaining>=4`, and measured duplicate ratio `>=0.25`.
- `contest_safe` remains the recommended contest profile.
- Stable 30-second expectation: depth 10 in the worst required stress position;
  depth 11 or better on normal positions. `initial` completed depth 12 in 30
  seconds during iterative deepening. `duplicate_hands` completed depth 10.
- Do **not** open `feature/full-quotient-assignment-set` yet. Evaluation cost is
  the dominant bottleneck, and the remaining exhaustive matrix is not worth its
  execution cost before evaluation tuning.
- Stage 3.5 infrastructure is implemented, but the exhaustive acceptance matrix
  is not complete. The missing executions are listed in section 10.

## 2. Correctness summary

The final CMake Release build completed and all seven test executables passed:

- quantum state, propagation, and contradictions;
- game rules, apply/undo, hash restoration, Catch, Try, and turn-256 draw;
- Alpha-Beta/PVS/TT/aspiration and timeout fallback;
- successor equivalence and representative legality;
- contest action codec and action-mask validation;
- config/resource profiles;
- all 110 Stage 3.5 fixtures, legal counts, hashes, and action round trips.

Every executed benchmark case checked the returned move against the internal
generator and saved action mask, encoded and decoded the external action, and
verified the fixture state and Zobrist hash after search. No guard failed.
Completed AB/PVS scores matched in the depth-9 mode pilot. `safe_8ply` and
`near_draw` had different legal best moves with equal root scores, which is a
tie-breaking difference rather than a value mismatch. No TT size changed a
completed best move or score.

Timeout safety held: an interrupted iteration retained only the last fully
completed depth. `initial` fixed depth 12 completed in six of seven runs; its
group is therefore flagged unstable even though the median run completed.

## 3. Fixed-depth benchmark

Seven fresh runs per case, 256 MiB requested TT, production candidate mode.
Times are medians. The 30-second hard limit applies to every run.

| Position | Depth | Done | Nodes | ms | NPS | Score | Best move | TT hit | First cutoff | Avg rank | L_eq dup | RSS MiB |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|
| initial | 9 | 1 | 163,993 | 1,244.8 | 131,740 | 748 | p3:7->4 | .097 | .779 | 1.51 | .253 | 197.8 |
| initial | 10 | 1 | 399,657 | 2,880.6 | 138,739 | -212 | p3:7->4 | .155 | .798 | 1.41 | .254 | 197.9 |
| initial | 11 | 1 | 2,612,536 | 20,005.0 | 130,594 | 868 | p3:7->4 | .119 | .697 | 1.65 | .265 | 197.9 |
| initial | 12 | 1* | 3,982,854 | 27,940.0 | 142,550 | -184 | p3:7->4 | .123 | .755 | 1.51 | .274 | 198.1 |
| safe_4ply | 9 | 1 | 12,964 | 89.6 | 144,667 | 999997 | p0:9->7 | .058 | 1.000 | 1.00 | .222 | 198.1 |
| safe_4ply | 10 | 1 | 30,841 | 159.4 | 193,534 | 999997 | p0:9->7 | .175 | 1.000 | 1.00 | .237 | 198.1 |
| safe_4ply | 11 | 1 | 113,935 | 640.7 | 177,827 | 999997 | p0:9->7 | .182 | 1.000 | 1.00 | .246 | 198.1 |
| safe_4ply | 12 | 1 | 306,722 | 1,333.9 | 229,941 | 999997 | p0:9->7 | .218 | 1.000 | 1.00 | .272 | 198.2 |
| safe_8ply | 9 | 1 | 26,304 | 154.0 | 170,810 | -999996 | p2:11->8 | .133 | .942 | 1.14 | .265 | 198.2 |
| safe_8ply | 10 | 1 | 63,693 | 379.7 | 167,728 | -999996 | p2:11->8 | .109 | .916 | 1.22 | .248 | 198.2 |
| safe_8ply | 11 | 1 | 166,453 | 772.6 | 215,448 | -999996 | p2:11->8 | .150 | .935 | 1.15 | .288 | 198.2 |
| safe_8ply | 12 | 1 | 1,214,983 | 5,971.5 | 203,465 | -999996 | p2:11->8 | .148 | .932 | 1.18 | .283 | 198.2 |
| duplicate_hands | 9 | 1 | 756,593 | 7,888.7 | 95,908 | 2953 | p3:7->4 | .147 | .749 | 1.53 | .267 | 198.2 |
| duplicate_hands | 10 | 1 | 987,481 | 8,729.9 | 113,115 | -53 | p3:7->4 | .125 | .795 | 1.35 | .276 | 198.2 |
| duplicate_hands | 11 | 0 | 3,226,560 | 30,000.2 | 107,551 | partial | fallback | .155 | .701 | 1.69 | .272 | 198.3 |
| duplicate_hands | 12 | 0 | 3,882,368 | 30,000.4 | 129,411 | partial | fallback | .152 | .739 | 1.55 | .265 | 198.2 |
| near_draw | 9–12 | 1 | 261 | 1.1–1.4 | — | 0 | p3:7->4 | .000 | .969 | 1.03 | .000 | 198.3 |
| near_catch | 9 | 1 | 113,247 | 662.5 | 170,939 | 999999 | p0:4->1 | .372 | 1.000 | 1.00 | .000 | 198.3 |
| near_catch | 10 | 1 | 129,267 | 457.8 | 282,335 | 999999 | p0:4->1 | .553 | 1.000 | 1.00 | .000 | 198.3 |
| near_catch | 11 | 1 | 201,995 | 858.6 | 235,256 | 999999 | p0:4->1 | .583 | 1.000 | 1.00 | .000 | 198.3 |
| near_catch | 12 | 1 | 212,723 | 535.7 | 397,113 | 999999 | p0:4->1 | .658 | 1.000 | 1.00 | .000 | 198.3 |
| near_try | 9–12 | 1 | 26 | 0.2–0.4 | — | 999999 | p7:6->4 | .000 | 1.000 | 1.00 | .000 | 198.3 |
| many_hands | 9 | 1 | 274,518 | 1,551.6 | 176,926 | 999991 | p5:4->1 | .117 | .867 | 1.21 | .000 | 198.3 |
| many_hands | 10 | 1 | 437,073 | 1,697.0 | 257,562 | 999991 | p5:4->1 | .217 | .963 | 1.05 | .000 | 198.3 |
| many_hands | 11 | 1 | 1,831,396 | 8,178.0 | 223,943 | 999991 | p5:4->1 | .211 | .968 | 1.05 | .000 | 198.3 |
| many_hands | 12 | 1 | 2,538,993 | 7,034.5 | 360,935 | 999991 | p5:4->1 | .368 | .994 | 1.01 | .000 | 198.3 |
| high_uncertainty_midgame | 9 | 1 | 111,144 | 699.9 | 158,798 | -999992 | p5:1->2 | .118 | .907 | 1.20 | .261 | 198.3 |
| high_uncertainty_midgame | 10 | 1 | 687,701 | 4,413.4 | 155,823 | -999992 | p5:1->2 | .131 | .887 | 1.25 | .243 | 198.3 |
| high_uncertainty_midgame | 11 | 1 | 1,267,477 | 6,334.0 | 200,106 | -999992 | p5:1->2 | .211 | .920 | 1.16 | .269 | 198.3 |
| high_uncertainty_midgame | 12 | 1 | 3,953,846 | 22,878.6 | 172,818 | -999992 | p5:1->2 | .180 | .868 | 1.31 | .270 | 198.3 |
| low_uncertainty_midgame | 9 | 1 | 75,704 | 385.4 | 196,444 | 1486 | p7:5->4 | .226 | .816 | 1.43 | .000 | 198.4 |
| low_uncertainty_midgame | 10 | 1 | 174,686 | 877.7 | 199,037 | -668 | p5:0->3 | .153 | .849 | 1.38 | .000 | 198.4 |
| low_uncertainty_midgame | 11 | 1 | 596,213 | 2,847.6 | 209,372 | 3964 | p5:0->3 | .211 | .799 | 1.46 | .000 | 198.4 |
| low_uncertainty_midgame | 12 | 1 | 2,167,253 | 10,862.9 | 199,509 | -634 | p5:0->3 | .190 | .763 | 1.57 | .000 | 198.4 |

`*` `initial` depth 12 completed in six of seven runs; one run timed out at
30,000 ms. All other completed groups had stable moves and scores.

Depth classes: A=9 positions, B=0, C=1 (`duplicate_hands`), D=0, E=0.
The median and 90th-percentile fixed-depth class both reach depth 12. The stable
production guarantee over this required set is depth 10; depth 11 is stable for
9/10 positions.

## 4. Iterative-deepening benchmark

Seven fresh runs per case. Exact mate/draw scores stop iterative deepening early,
so a low completed depth on those rows is a solved result, not a timeout.

| Position | Limit ms | Last depth | Started | Nodes | Median ms | Best move | Score | TT hit | RSS MiB |
|---|---:|---:|---:|---:|---:|---|---:|---:|---:|
| initial | 5,000 | 9 | 10 | 644,224 | 5,000.3 | p3:7->4 | 748 | .284 | 197.7 |
| initial | 10,000 | 10 | 11 | 1,284,032 | 10,000.4 | p3:7->4 | -212 | .304 | 197.8 |
| initial | 30,000 | 12 | 13 | 4,135,296 | 30,000.2 | p3:7->4 | -184 | .372 | 197.9 |
| duplicate_hands | 5,000 | 8 | 9 | 462,336 | 5,000.3 | p3:7->4 | -163 | .357 | 198.2 |
| duplicate_hands | 10,000 | 9 | 10 | 1,030,720 | 10,000.2 | p3:7->4 | 2953 | .383 | 198.2 |
| duplicate_hands | 30,000 | 10 | 11 | 3,071,616 | 30,000.1 | p3:7->4 | -53 | .357 | 198.2 |
| low_uncertainty_midgame | 5,000 | 12 | 13 | 964,480 | 5,000.1 | p5:0->3 | -634 | .347 | 198.2 |
| low_uncertainty_midgame | 10,000 | 13 | 14 | 1,892,672 | 10,000.1 | p5:0->3 | 3910 | .365 | 198.2 |
| low_uncertainty_midgame | 30,000 | 14 | 15 | 6,753,920 | 30,000.2 | p5:0->3 | -294 | .313 | 198.2 |
| safe_4ply | all | 3 exact | 3 | 437 | 4.3–4.9 | p2:11->7 | 999997 | .681 | 198.1 |
| safe_8ply | all | 4 exact | 4 | 626 | 5.3–6.7 | p2:11->8 | -999996 | .750 | 198.1 |
| near_draw | all | 64 | 64 | 17,137 | 66–78 | p0:9->6 | 0 | .983 | 198.2 |
| near_catch | all | 1 exact | 1 | 34 | 0.8–1.0 | p0:4->1 | 999999 | .000 | 198.2 |
| near_try | all | 1 exact | 1 | 14 | 0.2 | p7:6->4 | 999999 | .000 | 198.2 |
| many_hands | all | 9 exact | 9 | 318,883 | 1,637–1,650 | p5:4->1 | 999991 | .469 | 198.2 |
| high_uncertainty_midgame | all | 8 exact | 8 | 36,369 | 212–233 | p5:1->2 | -999992 | .560 | 198.2 |

All iterative groups had stable completed moves/scores across repeats. The
unsolved time-bound subset (`initial`, `duplicate_hands`, low uncertainty) has a
30-second median depth of 12. Across all ten rows, exact early termination makes
the raw depth median 8.5 and is not a useful strength statistic.

## 5. TT size matrix

Depth 10 uses seven repeats over all ten required positions. `Sum ms` is the sum
of per-position medians. Depth 11 and 30-second columns are single-run probes.

| Requested MiB | Allocated MiB | Entries | D10 sum ms | Avg hit | Max RSS MiB | D11 completed sum ms | 30s depths initial/dup |
|---:|---:|---:|---:|---:|---:|---:|---|
| 6 | 6 | 262,144 | 19,248.7 | .155 | 12.1 | — | — |
| 16 | 12 | 524,288 | 19,712.9 | .158 | 18.1 | — | — |
| 32 | 24 | 1,048,576 | 20,493.0 | .160 | 30.1 | — | — |
| 64 | 48 | 2,097,152 | 19,807.5 | .161 | 54.1 | 37,675.3 | 12 / 10 |
| 128 | 96 | 4,194,304 | 19,115.6 | .162 | 102.1 | 37,072.4 | 12 / 10 |
| 256 | 192 | 8,388,608 | 19,899.5 | .162 | 198.1 | 36,912.7 | 12 / 10 |
| 512 | 384 | 16,777,216 | 19,364.5 | .162 | 390.1 | 36,538.0 | 12 / 10 |
| 1024 | 768 | 33,554,432 | 18,905.3 | .162 | 774.0 | 36,669.0 | 12 / 10 |
| 2048 | 1536 | 67,108,864 | 19,557.9 | .162 | 1542.1 | — | — |
| 4096 | 3072 | 134,217,728 | 19,192.0 | .162 | 3078.0 | — | — |

All depth-10 sizes produced identical completed moves and scores. The apparent
depth-10 advantage at 1024 MiB over 128 MiB is only 1.1%, while RSS is 7.6×.
At depth 11, 512 MiB was best but only 1.0% faster than 256 MiB; 1024 MiB was
slower than 512 MiB. Every 30-second probe reached the same completed depth.
This is diminishing return, not evidence for a multi-gigabyte default.

## 6. L_eq trigger report

Seven-run depth-10 A/B, 256 MiB requested TT:

| Position | Off ms | Trigger ms | Time change | Off nodes | Trigger nodes | Node change | Calls | Dup ratio |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| initial | 2,386.0 | 2,880.6 | +20.7% | 335,244 | 399,657 | +19.2% | 1,884 | .254 |
| duplicate_hands | 13,914.1 | 8,729.9 | -37.3% | 1,732,633 | 987,481 | -43.0% | 15,769 | .276 |
| many_hands | 1,652.3 | 1,697.0 | +2.7% | 437,073 | 437,073 | 0.0% | 0 | .000 |

The trigger is beneficial on the deliberately duplicated state but not selective
enough: it activates in duplicate-hand descendants of ordinary `initial` and
materially worsens search. `many_hands` does not meet the duplicate hint and pays
only callback-check/noise-level overhead. Across fixed depths 9–12, grouping time
was 2,722 ms (1.3% of elapsed), of which canonicalization was 732 ms; the larger
cost is the changed move-order/search-tree shape.

Continuation tuning used seven-repeat depth-10 A/B across seven nontrivial
required workloads. The selected trigger (`24/4/0.25`) produced:

| Position class | Node effect | Median-time effect | Accepted calls |
|---|---:|---:|---:|
| initial | 0.0% | +2.5% | 15 |
| safe_4ply | 0.0% | +3.3% | 14 |
| safe_8ply | -3.1% | -0.3% | 253 |
| duplicate_hands | -23.6% | -19.5% | 772 |
| many_hands | 0.0% | +2.7% | 0 |
| high_uncertainty_midgame | 0.0% | +1.0% | 0 |
| low_uncertainty_midgame | 0.0% | +0.8% | 0 |
| **aggregate** | **-12.6%** | **-11.5%** | — |

Scores and selected moves matched the L_eq-off baseline. Threshold-only variants
could not prevent the high-uncertainty regression; the minimum duplicate-ratio
gate was required. Based on this multi-workload median and the user's explicit
cost constraint, deeper repeats are inferred rather than rerun. The production
default is tightened to `24/4/0.25`.

Recommendation: keep L_eq trigger-only, never always-on, and do not generalize
it to all hand-heavy states.

## 7. RAM/profile recommendation

| Profile | Recommended requested TT | Current action |
|---|---:|---|
| low_ram | 128 MiB (96 allocated) | Keep fixed; generation/L_eq caches off. |
| local_debug | 64 MiB (48 allocated) | Keep fixed; profiling/assertions outside search. |
| local_benchmark | 512 MiB (384 allocated) | Good collision margin without multi-GiB cost. |
| contest_safe | 256 MiB (192 allocated) | Best RAM/performance balance; retain 25% auto-size ceiling. |
| contest_high_ram | 512 MiB target | Keep 2048 MiB cap configurable, but do not select 1–2 GiB by default. |

## 8. Bottleneck analysis

Over the 40 fixed-depth median rows, inclusive measured time was:

- evaluation: 136,707 ms (65.6% of elapsed);
- move generation: 44,175 ms (21.2%);
- profiled propagation: 10,750 ms (5.2%);
- L_eq grouping: 2,722 ms (1.3%);
- canonicalization alone: 732 ms (0.4%).

Evaluation is the dominant bottleneck. It currently performs expensive mobility
and immediate-win work, including rule transitions, at every leaf and during
strong root ordering. Move ordering quality is generally high (many first-cutoff
rates above .8), but ordering regressed `duplicate_hands` relative to basic PVS
in the depth-9 pilot. TT hit quality plateaus around 128–512 MiB; reducing
collisions further does not reliably reduce elapsed time. Tables above 512 MiB
show a cache-locality/RAM warning because speed does not improve with footprint.

## 9. Decision

**A. Stay on Stage 3 and tune more.**

Do not open the full quotient branch yet. Correctness and timeout safety are
stable, depth 10–11 is practical, and the tightened L_eq trigger removes the
observed high-uncertainty node regression. Evaluation still dominates runtime.
Opening a representation-level quotient now would add a much larger correctness
surface before the known evaluation bottleneck is addressed.

Recommended production policy: iterative deepening with a 30-second hard limit,
target depth 10 as the guaranteed stress-case result, continue toward depth 11+
when time remains, 256 MiB requested TT, and enable only the tightened L_eq
trigger (`24/4/0.25`).

## 10. Coverage status and next prompt

Executed with seven-repeat medians:

- production fixed depth 9–12 on all ten required fixtures;
- production iterative 5/10/30 seconds on all ten required fixtures;
- TT depth 10 from 6 MiB through 4096 MiB on all ten required fixtures;
- focused L_eq on/off A/B;
- smoke fixtures and all correctness guards.

Executed as single-run pilots:

- all five modes at depth 9 on all ten required fixtures;
- TT depth 11 at 64–1024 MiB;
- TT 30-second iterative at 64–1024 MiB on `initial` and `duplicate_hands`.

Not yet executed, so Stage 3.5 exhaustive acceptance is **not claimed**:

- seven-repeat all-mode fixed depths 9–12;
- seven-repeat depth-11 and 30-second TT matrices for every required matrix
  position;
- deep runs over all 100 deterministic random fixtures.

Next prompt recommendation:

> Continue Stage 3 tuning without changing rules or implementing full quotient.
> Characterize and optimize the evaluation hot path, which accounts for about
> 66% of measured search time. Preserve evaluation/search scores at depths 1–3,
> then run focused seven-repeat depth-10 A/B on initial, duplicate-hands, and
> high-uncertainty fixtures only. Keep TT at 256 MiB and L_eq trigger at
> 24/4/0.25 unless focused median data proves a change.

Raw data: `stage35-fixed-production.csv`, `stage35-iterative.csv`,
`stage35-modes-depth9-pilot.csv`, `stage35-tt-depth10-*.csv`,
`stage35-tt-depth11-*-pilot.csv`, `stage35-tt-iterative30-*-pilot.csv`, and
`stage35-leq-off-rerun-*.csv` / `stage35-leq-ratio-0_25-*.csv` in this directory.
