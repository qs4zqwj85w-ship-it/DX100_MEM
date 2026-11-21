# M21 — Mechanism Ablation Report

Four ablations on four structurally representative workloads, plus a liveness
verification for every knob. The purpose is to attribute the measured speedups to
a mechanism rather than to Bulk in general.

---

## 1. Which workloads, and why those

Selection was made from the structural measurements in
`m21-locality-and-reuse-report.md`, before looking at any ablation result, and
spans the page structure axis that the knobs act on:

| workload | target pages | duplicate rate | structure that motivates it |
|---|---:|---:|---|
| w3_duphigh | 1 | 0.9995 | one page: page ordering has nothing to order, merge has everything to merge — the null case |
| w4_duplow | 2 048 | 0.0000 | maximum page spread, zero merging possible — the ordering case |
| w2_clustered | 32 (90 % of accesses in 816 columns = 6.4 KiB, inside 2 real pages) | 0.9844 | many pages but a hot subset — the case cohorts are for |
| w5_sequential | 256 | 0.8750 (0.000 word) | already in ascending line order — ordering should be a no-op |

## 2. Knobs, defaults and expected control

| configuration | settings | role |
|---|---|---|
| `ctrl` | `page_sched=1`, `page_order=1`, `page_cohorts=2` (default) | the primary comparison arm |
| `noorder` | `page_order=0`, cohorts still default | page ordering removed, dedup untouched |
| `coh1` | `page_cohorts=1` | one page run open at a time |
| `coh2` | `page_cohorts=2` named **explicitly** | must reproduce `ctrl` exactly |
| `coh4` | `page_cohorts=4` | more runs open, direction check |

`page_cohorts` defaults to 2 (`sim.hpp:211`), so `ctrl` *is* "default cohort
count". Naming the default explicitly (`coh2`) is how that configuration is
tested rather than assumed.

## 3. Results (2×8K window, seed 1, `S_faithful` = A(full)/C(noidx))

| workload | config | A cyc | C(noidx) | S_f | C(full) | S_same | sched switches |
|---|---|---:|---:|---:|---:|---:|---:|
| w3_duphigh | ctrl | 198 261 | 190 877 | 1.039 | 200 301 | 0.990 | 16 |
| w3_duphigh | noorder | 198 261 | 190 973 | 1.038 | 200 442 | 0.989 | 16 |
| w3_duphigh | coh1 | 198 261 | 190 877 | 1.039 | 200 301 | 0.990 | 16 |
| w3_duphigh | coh2 | 198 261 | 190 877 | 1.039 | 200 301 | 0.990 | 16 |
| w3_duphigh | coh4 | 198 261 | 190 877 | 1.039 | 200 301 | 0.990 | 16 |
| w4_duplow | ctrl | 14 897 749 | 4 976 052 | **2.994** | 5 062 847 | 2.943 | 25 267 |
| w4_duplow | noorder | 14 897 749 | 14 882 131 | **1.001** | 14 980 138 | 0.995 | 98 489 |
| w4_duplow | coh1 | 14 897 749 | 8 128 111 | **1.833** | 8 191 469 | 1.819 | 16 |
| w4_duplow | coh2 | 14 897 749 | 4 976 052 | **2.994** | 5 062 847 | 2.943 | 25 267 |
| w4_duplow | coh4 | 14 897 749 | 4 927 730 | **3.023** | 5 195 467 | 2.867 | 54 668 |
| w2_clustered | ctrl | 232 026 | 223 476 | 1.038 | 265 675 | 0.873 | 871 |
| w2_clustered | noorder | 232 026 | 231 229 | **1.003** | 275 484 | 0.842 | 1 122 |
| w2_clustered | coh1 | 232 026 | 223 455 | 1.038 | 266 614 | 0.870 | 16 |
| w2_clustered | coh2 | 232 026 | 223 476 | 1.038 | 265 675 | 0.873 | 871 |
| w2_clustered | coh4 | 232 026 | 224 202 | 1.035 | 264 634 | 0.877 | 879 |
| w5_sequential | ctrl | 361 952 | 319 738 | 1.132 | 367 937 | 0.984 | 474 |
| w5_sequential | noorder | 361 952 | 320 126 | 1.131 | 368 270 | 0.983 | 503 |
| w5_sequential | coh1 | 361 952 | 318 698 | 1.136 | 366 694 | 0.987 | 16 |
| w5_sequential | coh2 | 361 952 | 319 738 | 1.132 | 367 937 | 0.984 | 474 |
| w5_sequential | coh4 | 361 952 | 321 491 | 1.126 | 371 916 | 0.973 | 510 |

## 4. Is every knob live?

Yes, and the tests are positive rather than assumed:

1. **`coh2 == ctrl` byte for byte.** A cycles, C(noidx), C(full) and all three
   scheduler counters are identical in all four workloads. Naming the default
   cohort count explicitly therefore reproduces the control configuration
   exactly — the control is not accidentally a different configuration.
2. **`page_cohorts` is live and monotone.** Scheduler switches on w4: 16 (coh1) →
   25 267 (ctrl/coh2) → 54 668 (coh4). One cohort means one run is open at a
   time (16 switches in the whole run); four cohorts more than doubles the switch
   count. The switch counter is the direct evidence that the knob reached the
   scheduler.
3. **`page_order` is live.** On w4, `noorder` changes switches 25 267 → 98 489,
   deferrals 9 546 515 → 29 194 248, and C(full) 5 062 847 → 14 980 138 — a 3×
   cycle change. A knob that is not wired cannot move a counter by 3×.
4. **The three null results are structural, not dead knobs.** w3 shows 16 switches
   in *every* configuration because its target set is one page: there is never a
   second run to switch to. w5 shows ~474 switches and identical cycles with or
   without ordering because its line order is already ascending. Those are
   predicted nulls (from `tgt pages` and the reuse histogram), which is why they
   validate rather than weaken the ablation.

## 5. Attribution

### 5.1 Page ordering is the entire large win

w4: `ctrl` 2.994 → `noorder` 1.001. Removing ordering removes **100 %** of the
advantage on the workload where the advantage is largest. Nothing else changes —
same window, same dedup, same descriptors, same index stream. The mechanism is
visible in the memory counters
(`m21-locality-and-reuse-report.md` §3): the baseline's DRAM row-hit rate on w4 is
**0.1 %** and the hybrid's is **45.5 %**; the baseline takes 143 165 DTLB misses
and 99 317 page walks, the hybrid 62 955 and 32 740.

w2: `ctrl` 1.038 → `noorder` 1.003. Ordering is worth 3.5 points even when the
target set is L2-resident, because 90 % of accesses sit in 816 columns (6.4 KiB)
and the
ordering turns that into runs.

w5: 1.132 → 1.131 — nothing, as predicted, because the stream already arrives in
page order.

w3: 1.039 → 1.038 — nothing, as predicted, because there is one page.

> **Attribution: page ordering is worth 0.0–1.99× depending on page spread, and it
> is the dominant term whenever the target set exceeds L2.**

### 5.2 Cohorts buy overlap, and only when there are runs to overlap

w4: coh1 1.833 → coh2 2.994. One open run serialises: the next page run cannot
start until the current one is exhausted, so the window cannot keep the DRAM busy
across a page boundary. Two cohorts recover the remaining 63 %. Four cohorts add
only 0.03 on `S_f` (3.023) and actually *cost* on the double-charged pairing
(S_same 2.943 → 2.867, C(full) 5 062 847 → 5 195 467): more open runs means more
descriptor traffic and more partially-filled windows.

w2: coh1 1.038 ≈ ctrl 1.038 ≈ coh4 1.035 — the hot page subset is small enough
that a single run already saturates it.

w5: coh4 is the worst (1.126 vs 1.132): with an already-ordered stream, extra
cohorts only add window turnover.

> **Attribution: the default of two cohorts is at or near the optimum on all four
> workloads; with one cohort and a spread target set, only 42 % of the surplus
> survives (1.833 against 2.994).**

### 5.3 What the ablation says about merging

w3 is the merge-only case: 127 673 target requests against 64 unique lines
(1 995 : 1), no page spread, ordering a no-op. Its entire advantage is
1.039 — and §4 of the performance report shows that advantage is the *index
stream* (60 562 cycles) minus the engine's overhead (53 178). Deduplicating 1 995×
buys almost nothing in cycles because all 64 lines live in L1: the merged requests
would have been L1 hits.

That is the cleanest single result in M21: **the merge is a request-count
optimisation that pays only when the requests it removes would have missed.**

## 6. Cost side, measured on the same runs

The engine's fixed costs, from `tools/m21_extract.py mechanism` (8K, `full`):

| workload | organizer cycles | fanout cycles | index wait | window-full stall | AG cycles / total |
|---|---:|---:|---:|---:|---:|
| w1_uniform | 65 536 | 34 298 | 12 097 | 36 665 | 282 091 / 282 091 |
| w2_clustered | 65 536 | 35 227 | 11 382 | 33 149 | 265 675 / 265 675 |
| w3_duphigh | 65 536 | 32 800 | 40 341 | 10 599 | 115 569 / 200 301 |
| w4_duplow | 65 536 | 130 877 | 49 776 | 8 518 | 5 062 847 / 5 062 847 |
| w5_sequential | 65 536 | 32 768 | 44 809 | 11 672 | 367 937 / 367 937 |
| w8_wslarge | 65 536 | 115 088 | 66 521 | 7 986 | 1 005 349 / 1 005 349 |

* **The organizer tax is exactly 65 536 cycles in every workload** — 8 entries per
  row × 8 192 rows, independent of the target distribution. It is the price of
  running the engine at all, and it is why the parity corner sits at ~1.04 rather
  than at 1.00.
* **Fanout scales with distinct lines per window** (32 768 fixed-cost → 130 877 on
  w4), i.e. it is proportional to how much work the merge actually did.
* **AG cycles equal the total cycle count** on w1, w4, w5 and w8: the hybrid is
  engine-limited for its whole runtime. Being faster does not mean having slack.
* On w3 the engine is active for only 58 % of the run (115 569 of 200 301) — the
  one workload in the matrix where the SIMT path, not the engine, is the limit.
