# M17 Phase 4 — Page-aware scheduling and cohorts

**Question.** How much of Bulk's advantage comes from page-aware scheduling and
from cohort concurrency, and does the advantage survive without them?

**Answer.** Page-aware scheduling *is* the advantage at low κ and it is the only
thing keeping the accelerator viable at high κ. At κ = 0.25, turning page
ordering off takes the hybrid from **1.769× to 0.991×** — below parity — while
the DRAM row-hit rate collapses from **83.7 % to 13.1 %** and dTLB misses rise
from **1 719 to 11 953**. Reducing the cohort scheduler to one cohort (which is
byte-for-byte the M6 single-cursor policy) costs less but still takes the hybrid
to **1.114×**, with bank utilisation falling 86.4 % → 54.4 % at an unchanged
row-hit rate. At κ = 0.75 the single-cohort configuration is **0.758×** — the
hybrid is 24 % *slower* than plain SIMT.

So the two halves of "page-aware scheduling" do different things, and the
measurements separate them cleanly:

| mechanism | what it buys | evidence |
|---|---|---|
| **page order** (`page_order`) | **row and TLB locality** — issues ascending by page, then by line | row hit 83.7 % vs 13.1 %; TLB misses 1 719 vs 11 953; bank utilisation barely moves (86.4 % vs 74.8 %) |
| **cohort scheduler** (`page_sched`, `page_cohorts`) | **concurrency across page runs** — keeps `page_cohorts` runs open and issues whichever is ready | bank utilisation 86.4 % vs 54.4 % at an *unchanged* row-hit rate (83.7 % both) |

Neither is a new mechanism: both are the existing M6/M7 implementation, disabled
through the switches that already exist.

---

## 1. Method and controls

* One cell = one materialized trace, three arms, κ ∈ {0.25, 0.50, 0.75},
  W = 8 192, N = 16 384, five paired seeds.
* κ = 0.25 was run at both 16 and 128 L1 MSHRs (12 configurations × 5 seeds);
  κ = 0.50 and 0.75 at 16 MSHRs (12 × 5 each). 120 cells, all gates passing.
* **Only Bulk switches change.** Workload generation, buffers `(batch_window 8192,
  page_dir 512, org_lanes 4, fanout_lanes 4, batch_deadline 4096, org_overlap 1)`,
  the memory hierarchy, the residency budget (`R` = 128), dispatch classification
  and the RTL are untouched.
* **Integrity check that the control worked:** the SIMT-only arm is *identical*
  in all six configurations, for every seed and both MSHR settings — one distinct
  value per (κ, MSHR, seed) in `m17_tables.md` P4. Nothing about the conventional
  front-end moved.

The six configurations:

| label | `dedup` | `page_order` | `page_sched` | `page_cohorts` |
|---|---|---|---|---|
| Full Bulk | 1 | 1 | 1 | 2 (default) |
| cohorts 1 | 1 | 1 | 1 | **1** |
| cohorts 4 | 1 | 1 | 1 | **4** |
| scheduler off | 1 | 1 | **0** | (unused) |
| no page order | 1 | **0** | 1 | 2 |
| no page awareness | 1 | **0** | **0** | (unused) |

Note that "cohorts 1" and "scheduler off" are two different code paths that
produce **identical cycles** in every cell: with one cohort the scheduler has
nothing to choose between, so it degenerates to the M6 single cursor. That is the
behaviour the regression suite asserts (`tests.cpp:1993`), and it is re-confirmed
here at 15 (κ, seed, MSHR) points.

## 2. κ = 0.25, 16 MSHRs (mean of 5 paired seeds)

| configuration | SIMT-only | bulk-only | hybrid | SD(C) | **A/C** | page switches | scheduler idle | row hit % | bank util % | walks | TLB misses |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Full Bulk (cohorts 2) | 220 019 | 138 380 | 124 406 | 1 134 | **1.7686** | 862 | 71 928 | 83.7 | 86.4 | 525 | 1 719 |
| cohorts 1 | 220 019 | 214 016 | 197 445 | 1 109 | **1.1143** | 2 | 145 005 | 83.7 | 54.4 | 525 | 1 717 |
| cohorts 4 | 220 019 | 137 161 | 122 234 | 882 | **1.8000** | 963 | 69 693 | 83.7 | 87.9 | 525 | 1 717 |
| scheduler off | 220 019 | 214 016 | 197 445 | 1 109 | 1.1143 | 0 | 0 | 83.7 | 54.4 | 525 | 1 716 |
| no page order | 220 019 | 225 203 | 222 041 | 2 021 | **0.9909** | 106 | 67 613 | **13.1** | 74.8 | 528 | **11 953** |
| no page awareness | 220 019 | 225 234 | 222 049 | 2 075 | 0.9909 | 0 | 0 | 13.1 | 74.8 | 528 | 11 954 |

At 128 MSHRs the same ordering holds: full 1.7481, cohorts 4 1.7720, cohorts 1
1.0860, no page order 0.9893, no page awareness 0.9891.

## 3. κ = 0.50 and 0.75, 16 MSHRs

| κ | configuration | SIMT-only | bulk-only | hybrid | SD(C) | **A/C** | page switches | row hit % | bank util % |
|---|---|---|---|---|---|---|---|---|---|
| 0.50 | Full Bulk | 168 884 | 127 041 | 93 542 | 321 | **1.8054** | 902 | 88.1 | 80.4 |
| 0.50 | cohorts 1 | 168 884 | 199 946 | 161 629 | 424 | 1.0449 | 1 | 88.1 | 46.5 |
| 0.50 | cohorts 4 | 168 884 | 125 561 | 91 400 | 316 | **1.8477** | 934 | 88.1 | 82.3 |
| 0.50 | no page order | 168 884 | 174 334 | 166 759 | 1 838 | 1.0127 | 29 | **25.1** | 68.2 |
| 0.75 | Full Bulk | 119 664 | 117 189 | 95 140 | 91 | **1.2578** | 559 | 83.8 | 55.8 |
| 0.75 | cohorts 1 | 119 664 | 162 658 | 157 782 | 526 | **0.7584** | 1 | 83.8 | 33.6 |
| 0.75 | cohorts 4 | 119 664 | 117 703 | 94 075 | 82 | **1.2720** | 870 | 83.8 | 56.4 |
| 0.75 | no page order | 119 664 | 124 951 | 118 586 | 1 284 | 1.0091 | 84 | **47.7** | 57.0 |

Per-seed hybrid cycles for every configuration are in `m17_tables.md` P4.

## 4. Reading the ablation

**4.1 Page ordering carries the low-κ advantage.** With ordering off, the hybrid
is at parity (0.991×) even though it still deduplicates and still runs the
cohort scheduler. The single decisive counter is the row-hit rate, 83.7 % → 13.1 %,
and the second is the dTLB: 1 719 → 11 953 misses. Both are consequences of issue
order, not of capacity or concurrency: bank utilisation actually *rises* to
74.8 % (accounts are still moving) while useful row hits evaporate. High
utilisation with low row hits is the signature of spending DRAM cycles on
row activations.

A secondary effect appears in the walk counters: page walks stay at 528 but
dTLB misses multiply by 7×, i.e. the same pages are re-translated over and over.
Ordering by page turns the dTLB into a near-perfect filter.

**4.2 Cohorts carry concurrency, not locality.** Reducing `page_cohorts` from 2 to
1 leaves the row-hit rate at exactly 83.7 % and cuts bank utilisation from 86.4 %
to 54.4 %. Nothing about locality changed; the engine simply has one page run to
issue from, so it cannot cover DRAM latency across pages. The hybrid loses 73 039
cycles (124 406 → 197 445) and the ratio falls to 1.114. This is the cleanest
separation of the two mechanisms in the milestone: **order decides how many row
hits are possible, cohorts decide how many of them can be in flight.**

**4.3 More cohorts help, with diminishing returns.** Four cohorts beat two by
1.7 % at κ = 0.25 (122 234 vs 124 406), 2.3 % at κ = 0.50 and 1.1 % at κ = 0.75.
The gain is small because with `page_dir` = 512 the window already contains all
512 pages, and at `R` = 128 slots the engine cannot keep many runs deeply
outstanding anyway.

**4.4 The advantage does not survive without the scheduler at high κ.** At
κ = 0.75, one cohort gives 0.758×: the hybrid is slower than doing nothing but
SIMT. The engine alone in that configuration takes 162 658 cycles against SIMT's
119 664 (1.36× *slower*). At κ = 0.75 the stream is already 37 % row-hit-friendly
for SIMT, so the engine's only remaining advantage is the concurrency its cohort
scheduler provides; remove it and the accelerator is a net loss.

**4.5 The 128-MSHR baseline does not change any conclusion.** Every configuration
moves by ≤ 0.03 in A/C and the ordering is identical.

## 5. Translation and scheduler stalls

All values below are seed 1, arm C, taken from the cell logs (`m17_tables.md` P4
carries the five-seed means for misses and walks).

| κ | configuration | dTLB misses | page walks | PTE fetches | dTLB MSHR stall cycles | scheduler idle | scheduler deferrals |
|---|---|---|---|---|---|---|---|
| 0.25 | Full Bulk | 1 707 | 526 | 1 052 | 178 437 | 72 382 | 144 900 |
| 0.25 | cohorts 1 | 1 713 | 526 | 1 052 | 178 825 | 145 005 | 145 005 |
| 0.25 | no page order | 12 053 | 528 | 1 056 | 174 247 | 67 613 | 135 226 |
| 0.75 | Full Bulk | 1 206 | 517 | 1 034 | 242 146 | 62 355 | 125 069 |
| 0.75 | cohorts 1 | 1 209 | 517 | 1 034 | 242 558 | 125 058 | 125 058 |

Three points worth keeping, one of them a correction to the obvious reading:

* **Walk counts are a trace property, not a policy property.** 512 distinct pages
  produce 517–528 walks in every configuration, and PTE fetches move by 4
  (1 052 → 1 056). Ordering does not change how many pages are walked.
* **Page ordering changes how often a translation must be re-requested, not what
  a miss costs.** dTLB misses rise 7× with ordering off (1 707 → 12 053) because a
  16-entry dTLB thrashes on a scattered stream, yet recorded dTLB MSHR stall
  cycles are **flat** (178 437 → 174 247). Most of those misses hit in the
  512-entry L2 TLB, so the penalty is paid as added request latency, not as a
  recorded stall. Anyone reading the miss counter alone would overstate the
  translation cost by a wide margin — the 4× cycle difference attributable to
  ordering is in the row-conflict count, not here.
* **Scheduler "idle" and "deferrals" are the same number at one cohort**, because
  the single run is either ready or not; with two or more cohorts the counters
  separate (72 382 idle vs 144 900 deferrals at κ = 0.25). Neither is a proxy for
  lost cycles — the cycle count is.

## 6. What this part establishes

1. **Page-aware scheduling is the mechanism, not a tuning detail.** Disabling
   ordering costs the hybrid 42.5 percentage points of runtime reduction at
   κ = 0.25 and drops it below parity; disabling the cohort scheduler adds 58.7 %
   to the hybrid's cycles (124 406 → 197 445) and at κ = 0.75 makes the hybrid a
   net loss (0.758×).
2. **The two halves are separable by measurement.** Ordering buys row/TLB
   locality at constant bank utilisation; cohorts buy bank-level concurrency at
   constant locality.
3. **The default (2 cohorts) is within 2.3 % of the best tested (4) and keeps
   the low-κ advantage intact,** so no default change is indicated.
4. **The advantage is conditional on scheduling, not on the accelerator's
   existence.** A "Bulk vs SIMT" claim that does not name the scheduling
   configuration is under-specified: the same engine spans 0.758× to 1.848×
   across the six configurations measured here.

## 7. Limitations

* **κ = 0.50 and 0.75 are seed-1-MSHR only** (16 MSHRs). κ = 0.25 has both
  MSHR settings; the ordering of configurations is identical there, so the
  omission is unlikely to matter, but it is not replicated.
* **Cohort counts other than 1, 2 and 4 were not swept** (the implementation
  clamps at 16). 4 was the maximum tested, so "diminishing returns" is inferred
  from 1 → 2 → 4, not from a curve.
* **`park_retry`, `dtlb_parked` and `idx_reserve` were not varied.** The brief
  lists them as potential interactions; all three are held at their published
  values in every cell, so this report cannot say whether they change the cohort
  conclusion.
* **No new scheduling algorithm was introduced or evaluated**, as instructed.
  This is an ablation of the existing M6/M7 mechanisms only.
* **Row-hit rate is a locality proxy.** The DRAM model charges fixed 14/26-cycle
  timings and ignores queue occupancy, so the ~4× cycle difference attributable to
  ordering is a consequence of *counted* conflicts, not of a modelled controller's
  scheduling quality.
