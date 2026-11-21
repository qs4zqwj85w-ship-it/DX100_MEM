# M24-G — M24 Bulk resource and scaling sensitivity

**Scope.** A sensitivity study of the production M24 Bulk architecture (M23-G6D S1) against
resource-matched M0-RM. No architecture was changed, no policy was tuned, no production
configuration was edited, and every historical result is used as read-only input.

**Panel.** Seven traces, seven knobs of common resources, eight Bulk-specific capacities, two
combinations, **220 measured cells** (a paired cell runs both arms on the identical workload). Raw logs, the machine-readable matrix and the command lines are in
`build/wm/m24g/`; the four figures are in `docs/figs/m24g-*.svg`.

## 0. Answer

**The sign is robust; the magnitude is not.** Across all 11 common-resource settings (plus the
diagnostic translation point) no non-Bulk-specific knob moved a trace the wrong way across
parity: the two winning traces stayed wins at every setting that completed (1.28×–5.78×), the
L2-resident tie
stayed a tie, and the two losing traces stayed losses **except** when the DRAM was widened to
16 banks, where both reached parity or better (1.013× and 2.135×). What the knobs change is *how much* the architecture wins, and three of them
change it by more than a factor of two:

| what moves | effect on the Bulk/M0-RM ratio | why |
|---|---|---|
| **DRAM banks 2 → 16** | **+0.78 median** (1.74 → 2.91); the two losses flip to wins (n1 0.975 → 2.135, n3 0.823 → 1.013) | Bulk's page order needs banks to exploit it; SIMT on `w4` is walk-bound and cannot use them at all (identical cycles at 2 and 16 banks, `3 785 634` / `3 785 614`) |
| **translation concurrency 8 → 16/16/16** | **−0.55 median**; `w4` 2.504 → 1.283, `criteo_2k` 3.280 → 2.166 | given 16 concurrent walks, M0-RM stops being walk-bound on `w4` (−48.8 %), and Bulk, which was never walk-bound there, gains 0.0 % |
| **translation concurrency 8 → 2 walkers** | **+0.46 median**; `w4` 4.028, `criteo_2k` 4.122 | both arms lose translation throughput, SIMT loses 3.9× and Bulk 2.4× because Bulk issues 4–6× fewer walks |

The shared request budget and the cache MSHR files, which the brief names first, are the
*least* significant: `R` 64 and 192 move the median by −0.002 and +0.004; `l2_mshr` 128 by
−0.004; the L2-TLB MSHR file at 16 with 8 walkers changes nothing at all (identical cycles on
three of four traces). **The shared request path is not where the sensitivity lives** — and of
the Bulk-specific structures only the window is (§5).

The Bulk-specific structures are mostly *past* their knee, not at it: 16 cohorts is within
0.5 % of the shipped 8 on every trace, the page directory is measurably dead (identical cycles
at 128, 256 and 1 024 entries), and the ready pool is **8× oversized** — 512 entries is 11 % *faster* on
`criteo_2k` than the shipped 4 096, and 256 is faster still. The window is the one capacity
with real headroom: 32 K elements is 20–23 % faster than 16 K, and the supported maximum
(64 K) is faster again, at 1.02 MiB of SRAM — the same size as the L2.

## 1. Audit: what was already swept, and what this milestone adds

| source | swept | configuration it was swept on | reused how |
|---|---|---|---|
| **M23-F1** §4 | `walkers` × `dtlb_mshr` at five points (2/4, 16/4, 8/8, 16/16, 32/32) | the M23 `dx` configuration: 4 lanes, `mem_path = l1`, 2 DDR banks, 2×16K | the *finding* (effective concurrency = `min(walkers, dtlb_mshr, l2tlb_mshr)`) is re-derived here on the M24-D pair; the cycles are not reused, the pair differs |
| **M23-G2A** | `l2_mshr` 64 / 128 / 256 | the M23 `m0` arm, ideal translation, unbounded queue | re-measured here on **both** arms of the M24-D pair, with real translation |
| **M23-G2C** | DRAM queue depth as a diagnostic (`dram_diag`) | M23 `m0` | not reused; the queue-depth *sweep* (8 / 64 / unbounded) is new |
| **M23-E** | κ sweep, eleven points × four configurations | M21 synthetic | not a resource sweep — no overlap |
| **M24-A** | four organizations × five address maps | HBM and DDR | the map axis is M24-A's; this milestone holds the map at M0 |
| **M24-B** §10 | 16 vs 32 HBM banks, page size | the M24-B HBM hierarchy | the bank axis here is the DDR model's (2 → 16), because the M24-D pair is `dram_kind = ddr` |
| **M24-C / M24-D** | how `R`, MSHRs and every shared resource were matched | — | the pair itself: `build/wm/m24d/m24d_{bulk,simt}.cfg` |

**Nothing was re-run that already answered its question**, but nothing measured on an older
configuration is quoted as a number either: M23-F1's concurrency table is a *design* result
(it identifies which resource binds), and the sensitivity of the ratio depends on the pair,
so the sweep had to be repeated on the resource-matched pair. That is 220 new cells and zero
re-measurements of a published cell.

## 2. Method

**The pair.** `m24d_bulk.cfg` (G6D S1) against `m24d_simt.cfg` (M0-RM), audited in M24-D §2:
94 identical keys, 8 differing architecture fields, every shared resource byte-identical.
Each common-resource cell passes the *same* `--set` to both arms, so a pair differs only in
those eight fields.

**The panel.** Four primary traces carry every sweep: `w4_duplow` (order-bound win, 2.504×),
`criteo_2k` (translation-bound win, 3.280×), `n1_seq8m` (sequential loss, 0.975×) and
`n3_clu4m` (reuse loss, 0.823×) — the four regimes M24-F identified. Three secondary traces
(`w1_uniform` tie, `orkut_hub` graph win, `n4_uni4m` order-bound win) carry the five knobs
that moved the primary set.

**The grid.** 11 common settings across 7 knobs, 8 Bulk-specific capacities (22 settings), one
3-knob diagnostic point, 2 combinations, 6 probe cells outside the planned grid. Every cell's
exact command line is `build/wm/m24g/cmds.txt`; `build/wm/m24g_run.sh all` reproduces the
matrix and skips finished cells.

**What "paired" means for the two kinds of sweep.** A *common-resource* cell runs both arms at
the same value of the knob: the reported speedup is M0-RM cycles / Bulk cycles at that value.
A *Bulk-specific* cell runs the Bulk arm only — SIMT has no page cohort, window or ready pool —
and its denominator is the unchanged M24-D SIMT cell, which no Bulk-specific key can reach
(all eight are inert while `dispatch = 0`). The report never compares a re-provisioned Bulk arm
against a stock SIMT arm.

**Figures.** Every point in all four is a measured cell; nothing is fitted or interpolated.

* [`figs/m24g-common.svg`](figs/m24g-common.svg) — walkers, dTLB MSHRs, `R` and DRAM banks:
  speedup against the resource level, one line per primary trace, dashed line = the unmodified
  pair.
* [`figs/m24g-common-b.svg`](figs/m24g-common-b.svg) — the three inert knobs and the diagnostic
  16/16/16 point on the same axes.
* [`figs/m24g-bulk.svg`](figs/m24g-bulk.svg) — cohorts, window, ready pool and lanes: Bulk
  cycles relative to production (a deadlocked cell is the open square).
* [`figs/m24g-frontier.svg`](figs/m24g-frontier.svg) — the resource/performance frontier:
  speedup against engine SRAM bytes, from the 8 K window to the 64 K probe.

**Two structural limits were found before the sweep was run**, and both are reported rather
than worked around:

* `R > 255` is not constructible. The fused window stores the slot id in 8 bits, so the engine
  refuses it (`win_fused requires R <= 255 (8-bit slot)`, `bulk.cpp:329`). The grid uses 192 as
  the point above production.
* `page_cohorts = 0` is not constructible either (`bulk_org = 8` requires at least one cohort),
  so the cohort sweep starts at 1 and that point is a liveness boundary, not a slow point (§8).

## 3. Common resources

`Δ` is the change in the Bulk/M0-RM ratio against the unmodified pair. Speedup is
M0-RM cycles / Bulk cycles at the same setting.

| trace | base | walkers 2 | 4 | dTLB MSHR 4 | 16 | R 64 | 192 | l2_mshr 128 | q 8 | q 64 | banks 16 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| w4_duplow | 2.504 | **4.028** | 4.900 | **5.011** | 2.457 | 2.457 | 2.507 | 2.493 | 2.403 | 2.504 | **5.780** |
| criteo_2k | 3.280 | 4.122 | 3.831 | 4.004 | 3.044 | 3.260 | 3.285 | —† | 3.226 | 3.280 | 3.687 |
| n1_seq8m | 0.975 | 0.988 | 0.989 | 0.989 | 0.990 | 0.991 | 0.991 | 0.980 | 0.970 | 0.990 | **2.135** |
| n3_clu4m | 0.823 | 0.905 | 0.847 | 0.952 | 0.813 | 0.842 | 0.802 | 0.819 | 0.872 | 0.823 | **1.013** |

† the `l2_mshr = 128` Bulk cell on `criteo_2k` does not terminate (§8); it is not reported as
a ratio.

| trace | base | walkers 2 | dTLB 16 | R 192 | l2_mshr 128 | banks 16 |
|---|---:|---:|---:|---:|---:|---:|
| w1_uniform | 1.052 | 1.074 | 1.052 | 1.045 | 1.050 | 1.355 |
| orkut_hub | 2.934 | 2.307 | 2.918 | 2.934 | 2.906 | 5.082 |
| n4_uni4m | 2.186 | **4.489** | 2.108 | 2.188 | 2.181 | **6.585** |

**Aggregated over the four primary traces** (median, then the range across them):

| sweep | level | median | min | max | median Δ vs base |
|---|---|---:|---:|---:|---:|
| walkers | 2 | 2.508 | 0.905 | 4.122 | **+0.462** |
| walkers | 4 | 2.410 | 0.847 | 4.900 | +0.287 |
| dtlb_mshr | 4 | 2.496 | 0.952 | 5.011 | **+0.426** |
| dtlb_mshr | 16 | 1.723 | 0.813 | 3.044 | −0.029 |
| l2tlb_mshr | 16 | 1.747 | 0.823 | 3.280 | +0.000 |
| translation 16/16/16 | 16 | 1.136 | 0.928 | 2.166 | **−0.550** |
| R | 64 | 1.724 | 0.842 | 3.260 | −0.002 |
| R | 192 | 1.749 | 0.802 | 3.285 | +0.004 |
| l2_mshr | 128 | 0.980 | 0.819 | 2.493 | −0.004 |
| dram_q | 8 | 1.687 | 0.872 | 3.226 | −0.029 |
| dram_q | 64 | 1.747 | 0.823 | 3.280 | +0.000 |
| banks | 16 | **2.911** | **1.013** | **5.780** | **+0.783** |

The baseline median of the same four traces is **1.740**.

**Reading the table.**

*Housekeeping resources are free.* `R`, the L2 MSHR file and the queue depth move the median
by ≤ 0.03 and never change a regime. `dram_q = 64` reproduces the unbounded production queue
exactly (`2.504 / 3.280 / 0.990 / 0.823` = the baseline column), so the unbounded setting is
not hiding anything: Bulk's peak queue is 62–63 entries, and 8 is already close (63 % of the
cycles run with a full 8-entry queue, but the cost is only 4 % on `w4`).

*Translation is the axis the brief cares about, and it cuts both ways.* Removing translation
capacity helps Bulk's ratio; adding it hurts. The mechanism is visible in the walk counts:
`w4`'s Bulk arm issues 16 895 walks where SIMT issues 99 024, so walk throughput paces SIMT
4–6× harder than it paces the engine. At 2 walkers `w4`'s SIMT arm takes 14.86 M cycles against
a 3.79 M baseline (+292 %), Bulk takes 3.69 M against 1.51 M (+144 %), and the ratio rises to
4.03. Give SIMT 16/16/16 and it drops to 1.94 M (−49 %) while Bulk stays at 1.51 M (0.0 %),
and the ratio falls to 1.28 (its 1.94 M cycles against Bulk's 1.51 M).

*DRAM parallelism is where the architecture's own mechanism lives.* At 16 banks `w4`'s Bulk arm
drops to 655 k cycles (−57 %) and `n4`'s to 386 k (−67 %), while SIMT does not move at all on
`w4` (3 785 614 vs 3 785 634) — it is walk-bound and has no row hits to exploit (0.2 %). The two
losses flip: `n1` 0.975 → 2.135, `n3` 0.823 → 1.013. Doubling the banks is not free, and §6
accounts for it, but the measured statement is unambiguous: **the advantage grows with the
number of independent DRAM banks; it does not shrink.**

## 4. Where the bottleneck goes when translation is widened

The diagnostic point (16 walkers, 16 dTLB MSHRs, 16 L2-TLB MSHRs) is beyond the requested grid
and is included because it is the only place the L2-TLB MSHR file is not inert.

| point | trace | arm | walker_max | walker util | pte fetches | refuse_full | l2tlb_walk_stalls |
|---|---|---|---:|---:|---:|---:|---:|
| baseline | w4_duplow | simt | 8 | 98.1 % | 198 048 | 14 791 016 | 0 |
| dtlb 16 | w4_duplow | simt | 8 | 99.5 % | 197 076 | 14 280 339 | **7 281 191** |
| 16/16/16 | w4_duplow | simt | **16** | 95.3 % | 197 168 | 5 652 591 | 0 |
| baseline | w4_duplow | bulk | 8 | 41.9 % | 33 790 | 93 771 | 21 322 |
| 16/16/16 | w4_duplow | bulk | 11 | 20.9 % | 33 778 | 0 | 0 |

Three things are worth stating plainly.

1. **`dtlb_mshr = 16` alone buys nothing and moves the bottleneck**: with 8 walkers the
   effective concurrency stays 8, and the 16-entry dTLB file now admits more pages than the
   8-entry L2-TLB MSHR file can serve, so `l2tlb_walk_stalls` goes from 0 to 7.28 M on `w4`.
   That is the "shifts the bottleneck elsewhere" case the brief asks about, measured.
2. **Effective concurrency is `min(walkers, dtlb_mshr, l2tlb_mshr)`**, exactly as M23-F1 found
   on the older configuration: at 16/16/16 the SIMT arm reaches `walker_max = 16` and 95.3 %
   utilization.
3. **Bulk never saturates the widened machine.** At 16/16/16 its `walker_max` is 11 and its
   walker utilization 20.9 % on `w4`: the engine does not need the translation throughput that
   the ratio's decay is buying for SIMT.

## 5. Bulk-specific capacities

Cycles relative to the production Bulk cell for the same trace (1.000 = unchanged, lower is
better). These run the Bulk arm only.

| capacity | trace | 1 | 2 | 4 | 8 (prod) | 16 | 32 | 64 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| page cohorts | w4_duplow | 3.279 | 1.689 | 1.000 | 1.000 | 1.000 | — | — |
| page cohorts | criteo_2k | † | 1.627 | 1.133 | 1.000 | 1.005 | — | — |
| page cohorts | n3_clu4m | 1.658 | 1.156 | 1.026 | 1.000 | 1.002 | — | — |
| window (elements) | w4_duplow | — | — | — | 1.000 (16K) | 1.125 (8K) | 0.887 (32K) | **0.808 (64K)** |
| window (elements) | criteo_2k | — | — | — | 1.000 (16K) | 1.367 (8K) | 0.769 (32K) | **0.657 (64K)** |
| window (elements) | n3_clu4m | — | — | — | 1.000 (16K) | 1.123 (8K) | 0.798 (32K) | — |
| window (elements) | n1_seq8m | — | — | — | 1.000 (16K) | 0.992 (8K) | 0.972 (32K) | — |
| ready pool | w4_duplow | — | — | — | 1.000 (4 096) | 1.000 (512) | 1.000 (1 024) | 1.000 (2 048) |
| ready pool | criteo_2k | — | — | — | 1.000 (4 096) | **0.892** (512) | 0.899 (1 024) | 0.921 (2 048) |
| ready pool | n3_clu4m | — | — | — | 1.000 (4 096) | 0.991 (512) | 0.986 (1 024) | 0.981 (2 048) |

† the one-cohort cell on `criteo_2k` deadlocks (§8).

Probes outside the planned grid (same pair, same traces): window 64 K is `0.808` on `w4` and
`0.657` on `criteo_2k`; ready pool 256 is `1.000` on `w4` and `0.880` on `criteo_2k`; ready pool
128 is `1.000` on `w4` and `0.883` on `criteo_2k`.

**Five capacities are inert.** Three of them reproduce the production cycle count exactly, at
every tested value, on every trace; the two lane counts move it by 0.005–0.02 %. None of them
changes a regime, and none is worth a transistor:

| capacity | tested | cycles on w4_duplow | why it cannot bind |
|---|---|---|---|
| `page_dir` (directory entries) | 128 / 256 / 512 / 1 024 | 1 511 665 at every value | no issue decision reads the directory; page runs come from the sealed order (M22 §6.1 predicted this from the source, this measures it) |
| `org_lanes` | 2 / 4 / 8 | 1 511 545 / 1 511 665 / 1 511 748 (±0.007 %) | the seal is hidden behind the other buffer's drain (M22 §2) |
| `fanout_lanes` | 1 / 2 / 4 / 8 | 1 511 665 at every value | the streaming path retires elements from the fill's fanout job, and one lane already outruns the DRAM |
| `idx_mshr` | 16 / 64 / 128 | 1 511 665 at every value | the index stream is not the constraint at any depth |
| `ag_lanes` | 4 / 8 / 16 | 1 511 762 / 1 511 665 / 1 511 907 (±0.02 %) | 8 multiplies per cycle already exceed the issue rate |

The page-directory result is the important one: it is a **measured null** on the same
structure M22 condemned from the source. 16 384 B of SRAM for two buffers, and 0.000 % of the
cycles.

**Cohorts: the knee is 4 on `w4` and `n3`, 8 on `criteo_2k`.** `w4` is flat from 4 up; `criteo_2k`
improves 13.3 % from 4 to 8 and then loses 0.5 % at 16; `n3` improves 2.6 % from 4 to 8 and
loses 0.2 % at 16. Below the
knee the cost is large (2 cohorts: +63–69 %; 1 cohort: +66 % on `n3` and a deadlock on
`criteo_2k`). The shipped 8 is one step above the knee — defensible, and 16 is a waste of 32 B
per cohort.

**The ready pool is 8–16× oversized.** On `criteo_2k` the shipped 4 096 entries cost 12 % more
cycles than 256; the curve is monotone from 4 096 down to 256 (242 935 / 223 713 / 218 313 /
216 817 / 213 801) and flat from 256 to 128. A smaller pool shortens the translation→DRAM
distance, and on a two-bank DRAM nothing is lost. On `w4` and `n1` the pool is irrelevant at
every size (identical cycles), which is what the 4 096 was sized for.

## 6. Storage, logic and timing cost of each swept capacity

Bytes are the simulator's own accounting (`bulk_win_bytes`, `bulk_sched_bytes`,
`bulk_g6b_pa_bytes`), which M22 §3 cross-checked against the declared record widths. Area
assumes M22's nominal 0.045 µm²/bit; ratios are more trustworthy than absolutes.

| structure | per unit | at production | what the sweep says it is worth |
|---|---:|---:|---|
| fused window, both buffers | 16 B per element (+ 16 384 B directory + cohorts) | 279 040 B (16 K) | **not saturated**: 32 K is 20–23 % faster, 64 K 19–34 % faster, at 541 184 B and 1 065 472 B |
| page cohorts | 32 B per cohort (2 buffers) | 256 B (8) | knee 4; 8 costs +128 B for +13 % on `criteo_2k`, 16 costs +256 B for −0.5 % |
| ready pool | 16 B per entry | 65 536 B (4 096) | **oversized**: 8 192 B (512) is 11 % faster on `criteo_2k`, 4 096 B (256) 12 % faster |
| page directory | 16 B per entry × 2 buffers | 16 384 B (512) | **zero**: measured null at 128/256/1 024 |
| index ring, request records, queues | fixed | ≈ 25 KB (M22 §4) | unaffected by any swept knob |

**Total engine SRAM** and what it buys (speedup over the unchanged M0-RM cell):

| configuration | SRAM | w4_duplow | criteo_2k | n3_clu4m |
|---|---:|---:|---:|---:|
| production (16 K, 8 cohorts, pool 4 096) | 344 832 B | 2.504 | 3.280 | 0.823 |
| pool 512 | **287 488 B** | 2.505 | **3.675** | 0.830 |
| pool 256 (probe) | 283 392 B | 2.505 | **3.727** | — |
| cohorts 4 | 344 448 B | 2.503 | 2.895 | 0.803 |
| window 8 K | **213 760 B** | 2.226 | 2.400 | 0.733 |
| window 32 K | 606 976 B | **2.824** | **4.268** | **1.032** |
| window 64 K (probe) | 1 131 264 B | 3.098 | 4.995 | — |
| 16 banks (paired, both arms) | 344 832 B | 5.780 | 3.687 | 1.013 |
| 16 banks + pool 512 | 287 488 B | 5.663 | 3.721 | 1.239 |
| 32 K window + pool 512 | 549 632 B | 2.824 | 4.531 | 1.033 |

Two frontier statements follow, and they are the deliverable the brief asks for.

* **The production point is dominated.** Ready pool 512 delivers the same cycles on `w4`
  (1 511 501 vs 1 511 665) and 10.7 % fewer on `criteo_2k` (216 817 vs 242 935), for 57 344 B
  less SRAM. Nothing in the sweep pays for the 4 096 entries. The combination cells confirm the
  downsize survives a wider DRAM (16 banks + pool 512: 214 132 on `criteo_2k`, 0.9 % better than
  16 banks alone) and a larger window (32 K + pool 512: 175 866, 5.8 % better than 32 K alone).
* **The window is the only capacity with real headroom, and it is expensive.** Every element
  costs 16 B across both buffers, so 64 K costs 1.02 MiB — the area of the L2 the engine sits
  beside — for 19–34 % on the two traces that need it and 0 % on the ones that do not. M22's
  judgement (16 K is a mode bit, not a second array) is unchanged by this sweep; what is new is
  that 32 K is strictly better than 16 K on all four primary traces, including the two losses
  (`n3` 0.823 → 1.032, from a loss to a win).

## 7. Diminishing returns and interactions

| lever | 2× the resource buys | verdict |
|---|---|---|
| walkers 8 → 16 (with MSHRs) | SIMT −48.8 % on `w4`, Bulk 0.0 % | **pure loss** for Bulk's ratio; the engine was never walk-bound |
| dTLB MSHRs 8 → 16 alone | 0.0 % (walkers still cap at 8) + 7.28 M L2-TLB stalls | **negative**: moves the bottleneck |
| L2-TLB MSHRs 8 → 16 alone | 0.000 % (identical cycles on 3 of 4 traces) | inert below 16 walkers |
| R 128 → 192 | +0.004 median | inert at production |
| L2 MSHRs 64 → 128 | −0.004 median, one deadlock | inert, and unsafe on `criteo_2k` |
| DRAM banks 2 → 16 | +0.78 median, all losses flip | **the largest headroom in the study** |
| window 16 K → 32 K | −20.2 % (`n3`), −23.1 % (`criteo_2k`), −11.3 % (`w4`), −2.8 % (`n1`) | still paying at 64 K |
| cohorts 8 → 16 | −0.5 % `criteo_2k`, +0.2 % `n3` | past the knee |
| pool 4 096 → 2 048 → 512 → 256 | 0 %, −8 %, −11 %, −12 % on `criteo_2k` | smaller is better; floor at 256 |

The interactions are consistent in direction: **nothing that helps the engine helps SIMT more.**
The 16-bank result is the clearest case — SIMT is walk-bound on `w4` at both bank counts, so the
extra banks are only usable by the arm that already orders its traffic. The converse also holds:
widening translation helps SIMT exclusively, because on the traces it wins the engine's walk
count is 4–6× lower.

## 8. Liveness

Two of the 220 cells do not terminate. Both are on `criteo_2k`, both are Bulk-arm cells, and
both were run to the configuration's own 500 M-cycle guard, at which point the model reported
the reason rather than a number.

| cell | what happened | evidence |
|---|---|---|
| `page_cohorts = 1` | the engine stops half way: **16 382 of 34 816 elements** done, its descriptor never completes | `bulk_desc_completed = 0`, `bulk_tgt_req_gen = issued = done = 6 120`, `bulk_slot_stall = 39 644`, `bulk_unissued_max = 64` |
| `l2_mshr = 128` | the engine finishes everything (34 816 / 34 816, 1 / 1 descriptors) and the **SIMT** side strands | model diagnosis: `SIMT: issued != completed (work left in flight)`, `slots still held`, hierarchy still in flight |

They are two different failures, not one: the first is an engine-side stall with work in
flight, the second is the dispatched-SIMT half of a hybrid run failing to drain. Both are
instances of the class M24-D documented for one Criteo mapping cell (map M3, seed 3); this
milestone shows the same class is **reachable by configuration alone, on the production map**.

Probes bound the scope (all on `criteo_2k`, Bulk arm, 20 M-cycle cap):

| `l2_mshr` | 64 (production) | 96 | 128 | 160 |
|---|---|---|---|---|
| outcome | completes, 242 935 cycles | deadlock guard | deadlock guard | deadlock guard |

So the shipped configuration sits on the **safe side of a sharp edge**: the Bulk arm on
`criteo_2k` terminates at the production L2 MSHR depth of 64 and hangs at every larger value
tested. That is a limitation of the design as implemented, not a property of this sweep, and it
means `l2_mshr` cannot be used as a scaling lever for this workload until the defect is fixed.

No deadlock, stall or monotonicity violation appeared in any other cell: no request was
stranded, `bulk_elems_done = bulk_elems_total` in all 218 cells that terminated, and the DRAM
row identity holds in every one of them.

## 9. Limitations

1. **One simulator seed, one trace seed per workload.** The M24-F panel was already shown to be
   seed-stable to ±0.006 on eight two-seed families, but the sweep itself has one seed.
2. **The maps are M0 only.** M24-A showed the organization interacts with the physical map; this
   milestone holds the map at production and therefore says nothing about how the capacity
   knees move under M1/M2/M3.
3. **The bank axis is the DDR model's 2 → 16.** M24-B measured 16 → 32 HBM banks; that
   hierarchy is not the one the M24-D pair instantiates.
4. **Combinations are two, not a search.** 16 banks + pool 512 and 32 K window + pool 512 were
   measured because they are the two knobs that individually pay; a full configuration search
   would need the window × pool × banks cube (24 combinations at the tested levels, per trace).
5. **The ready-pool result is bounded to the production memory system and two widths.** It holds
   at 2 and 16 DDR banks and at 16 K and 32 K windows, on three traces; it is not proven for the
   HBM hierarchy or for other workloads.
6. **Storage numbers are the model's, not a synthesis.** M22 §5 is the authority on what a real
   build costs (critical paths, wire risk for fanout, SRAM bit cells); this report reuses its
   nominal 0.045 µm²/bit and does not re-derive it.
7. **Two cells cannot be reported as performance.** They are excluded from every speedup table
   and reported in §8 instead.

## 10. Conclusion

The architecture's advantage is **robust in sign and configuration-dependent in size**, and the
configuration dependence is almost entirely in the memory system, not in the engine.

* **No common-resource setting reverses a win or a tie.** Across 11 settings the four primary
  traces span 1.01×–5.78×; the only direction change is a wider DRAM lifting both losses to
  parity or better.
* **The advantage scales *up* with DRAM bank parallelism** (+0.78 median from 2 to 16 banks) and
  **down with translation concurrency** (−0.55 median from 8 to 16/16/16, +0.46 from 8 to 2).
  Both follow from its walk count (4–6× lower than SIMT's on the winning traces) and its
  page-ordered release: it needs less translation and can use more memory parallelism than the
  coalesced SIMT path.
* **The Bulk-specific capacities are at or past their knee, except the window.** 16 cohorts is
  within 1 % of 8 on every trace and 4 is within 3 % on two of three; the page directory is
  measurably dead; the ready pool is 8–16× oversized and
  the shipped value is dominated on both SRAM and cycles; the window is the only capacity where
  more hardware buys more performance, and it buys it at 16 B per element.
* **The production point is not the frontier point**: 287 488 B (pool 512) matches or beats
  344 832 B (production) on every trace measured, and that is the one configuration change the
  evidence supports without qualification.
* **The design has a sharp liveness edge on one workload** (Criteo at `l2_mshr > 64`, and at one
  page cohort). It is a defect to fix, not a tuning knob to avoid, and it is the one finding here
  that argues against treating the capacity sweep as a free design space.

The honest envelope: **M24 Bulk's measured advantage depends most on how much DRAM parallelism
the machine has and how little translation throughput the baseline gets.** Neither is a property
the architecture controls, so the correct statement is not "Bulk is 2.5× faster" but "Bulk is
2.5× faster on a two-bank DDR machine with eight walkers, 5.8× faster with sixteen banks, and
1.3× faster when the baseline is given sixteen concurrent walks."

## Reproduce

```
cd DX100_MEM
python3 tools/m24g_plan.py                    # cells.tsv + cmds.txt (220 cells)
PAR=6 ./build/wm/m24g_run.sh all              # ~12 min, skips finished cells
python3 tools/m24g_extract.py                 # cells.csv, pairs.csv, checks.txt, report.txt
python3 tools/m24g_plot.py                    # docs/figs/m24g-*.svg
python3 tools/m24g_verify_doc.py              # every figure in this file, recomputed
(cd sim && ./bin/dx100_tests)                 # 4 924 checks, 0 failures
```

Artifacts in `build/wm/m24g/`: `cells.tsv`, `cmds.txt`, 220 cell logs under `log/`, 6 probe
logs, `cells.csv`, `pairs.csv`, `checks.txt`, `report.txt`, `run.log`. Figures in
`docs/figs/`: `m24g-common.svg`, `m24g-common-b.svg`, `m24g-bulk.svg`, `m24g-frontier.svg`.
Scripts: `build/wm/m24g_run.sh`, `tools/m24g_{plan,extract,plot,verify_doc}.py`. The production
configuration files (`build/wm/m24d/m24d_*.cfg`, `sim/configs/*`) were not modified.
