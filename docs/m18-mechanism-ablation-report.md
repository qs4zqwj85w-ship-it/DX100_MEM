# M18 — Mechanism ablation report

**Question:** does M17's mechanism-level finding — that page-aware request ordering and
concurrent page cohorts, not window capacity, produce the synthetic advantage — generalise to
real applications?

**Answer:** No. On real SpMV traces, turning page-aware scheduling off changes execution cycles
by **+0.5 % at most** and leaves the DRAM row-hit rate **unchanged to 0.1 pp**. The M17
mechanism is inert here, because the traces are already 82–95 % bank-busy and the gather
target footprint is small enough to be L2-resident.

---

## 1. Configurations compared

All cells are the same three-arm run, the same trace, the same environment; only the named
knob differs. Window = 2×8K throughout (the mandatory smaller-window configuration).

| Cell | Knob | Meaning |
|---|---|---|
| `full` | — | default: `page_sched=1`, `page_cohorts=2` |
| `nopage` | `page_sched=0` | byte-for-byte the M6 policy: one cursor over the window's lines |
| `coh1` | `page_cohorts=1` | page-aware ordering retained, **one** page run open at a time |
| `coh16` | `page_cohorts=16` | page-aware ordering, sixteen runs open (bcsstk25 only) |

### 1.1 The knobs are live — and their effect is binary

| bcsstk25 | cycles | DRAM reads | row-hit | dTLB miss | walks |
|---|---:|---:|---:|---:|---:|
| default (cohorts = 2) | 492 449 | 60 604 | 86.8 % | 2 068 | 787 |
| `page_cohorts=16` | **492 449** | 60 604 | 86.8 % | 2 068 | 787 |
| `page_cohorts=1` | 490 025 | 60 328 | 86.8 % | 2 066 | 787 |
| `page_sched=0` | **490 025** | 60 328 | 86.8 % | 2 066 | 787 |

`cohorts=16` reproduces the default to the cycle; `cohorts=1` reproduces the M6 cursor to the
cycle. So both knobs reach the scheduler (a dead knob would leave `coh1` equal to the
default), and for these traces the policy has exactly two behaviours: "several page runs" and
"one sequential pass", differing by 2 424 cycles (0.5 %).

| qc2534 | cycles | DRAM reads | row-hit | dTLB miss | walks |
|---|---:|---:|---:|---:|---:|
| default | 923 790 | 113 113 | 88.5 % | 3 715 | 1 655 |
| `page_cohorts=1` | 923 816 | 113 113 | 88.5 % | 3 721 | 1 655 |
| `page_sched=0` | 923 816 | 113 113 | 88.5 % | 3 721 | 1 655 |

Here the two policies are 26 cycles apart (0.003 %) with identical DRAM behaviour.

---

## 2. The measured mechanism table

For each ablation cell, the metrics the brief asks for. "Bank util" = Σ bank-busy cycles /
(cycles × 2 banks). "Row switches" is the row-conflict count (the model does not track a
distinct row-switch counter; a conflict *is* a switch to a different open row).

### 2.1 bcsstk25 (banded structural, 252 241 nonzeros, `x` = 123 KiB)

| Metric | A: SIMT | C: default | C: page off | C: one cohort |
|---|---:|---:|---:|---:|
| execution cycles | 461 688 | 492 449 | 490 025 | 490 025 |
| DRAM reads | 50 245 | 60 604 | 60 328 | 60 328 |
| DRAM row-hit rate | 80.0 % | 86.8 % | 86.8 % | 86.8 % |
| row switches (conflicts) | 10 072 | 8 007 | 7 944 | 7 944 |
| DRAM bank utilisation | 89.3 % | 95.9 % | 95.6 % | 95.6 % |
| dTLB misses | 1 575 | 2 068 | 2 066 | 2 066 |
| page walks | 788 | 787 | 787 | 787 |
| translation stalls (`tlb0_mshr_stalls`) | 889 880 | 642 250 | 640 897 | 640 883 |
| L1 stall cycles | 12 068 434 | 27 887 954 | 27 753 788 | 27 753 673 |
| scheduler: slot stalls | — | 261 299 | 267 998 | 267 925 |
| scheduler: index-wait stalls | — | 72 554 | 71 891 | 71 891 |
| buffer occupancy (peak) | — | 8 192 | 8 192 | 8 192 |
| organiser cycles | — | 126 121 | 126 121 | 126 121 |
| index acquisition (to first target) | — | 6 581 | 6 581 | 6 581 |
| target requests issued | — | 3 431 | 3 432 | 3 432 |
| duplicate merging | — | 248 810 | 248 809 | 248 809 |
| L1 hit rate | 35.1 % | 3.1 % | 3.1 % | 3.1 % |
| L2 hit rate | 9.3 % | 12.0 % | 12.0 % | 12.0 % |

### 2.2 qc2534 (near-dense, 463 360 nonzeros, `x` = 20 KiB)

| Metric | A: SIMT | C: default | C: page off | C: one cohort |
|---|---:|---:|---:|---:|
| execution cycles | 762 503 | 923 790 | 923 816 | 923 816 |
| DRAM reads | 87 447 | 113 113 | 113 113 | 113 113 |
| DRAM row-hit rate | 87.2 % | 88.5 % | 88.5 % | 88.5 % |
| row switches | 11 144 | 12 966 | 12 966 | 12 966 |
| DRAM bank utilisation | 89.1 % | 94.1 % | 94.1 % | 94.1 % |
| dTLB misses | 2 729 | 3 715 | 3 721 | 3 721 |
| page walks | 1 368 | 1 655 | 1 655 | 1 655 |
| translation stalls | 1 576 094 | 1 499 891 | 1 499 912 | 1 499 908 |
| L1 stall cycles | 24 688 176 | 50 712 756 | 50 721 548 | 50 721 339 |
| scheduler: slot stalls | — | 348 704 | 349 233 | 349 148 |
| scheduler: index-wait stalls | — | 140 615 | 140 647 | 140 647 |
| buffer occupancy (peak) | — | 8 192 | 8 192 | 8 192 |
| organiser cycles | — | 231 680 | 231 680 | 231 680 |
| index acquisition (to first target) | — | 6 499 | 6 499 | 6 499 |
| target requests issued | — | 2 212 | 2 212 | 2 212 |
| duplicate merging | — | 461 148 | 461 148 | 461 148 |
| L1 hit rate | 35.4 % | 1.5 % | 1.5 % | 1.5 % |
| L2 hit rate | 3.5 % | 6.9 % | 6.9 % | 6.9 % |

---

## 3. Separating ordering from concurrency

The brief asks not to attribute an improvement to page scheduling merely because the full
configuration is faster. Here the separation is easy, because **there is no improvement to
attribute**:

* **Ordering** (`page_sched` off vs on): −0.5 % cycles on bcsstk25 (page-aware is *slower*),
  +0.003 % on qc2534. Effectively zero, and the sign is not consistent.
* **Concurrency** (`cohorts` 1 vs 2 vs 16): +0 on bcsstk25 (cohorts ≥ 2 identical to the
  default), +0.003 % on qc2534. Zero.
* **Row locality**: 86.8 % vs 86.8 %, and 88.5 % vs 88.5 %. The ordering mechanism is
  designed to raise row hits and it changes them by 0.0 pp.

Why the mechanism is inert on these traces, stated as three measurements:

1. **The target footprint is tiny relative to the index stream.** The gathers reach 317 lines
   (qc2534), 493 (lnsp3937), 1 930 (bcsstk25) and 18 336 (dubcova3). A window of 8 192
   elements therefore collapses onto almost the same small set of lines every time
   (`bulk_dedup_merged` = 98.6 %, 99.5 %, 97.8 % of elements), and those lines are in L2
   after the first window. There is no DRAM row order left to improve.
2. **The DRAM is already saturated.** Bank utilisation is 89–96 % in the hybrid; the remaining
   headroom is 4–11 %, and the best possible use of it (eliminating every remaining row
   conflict) is worth ~15 % of DRAM time, not a factor.
3. **The traffic that dominates DRAM is issued by SIMT, not by the engine.** `value` and
   `colidx` are 8 B and 4 B per nonzero streams: 3–4× the element count of the gather. The
   engine's scheduler cannot reorder them, so even a perfect target ordering would touch only
   the minority of DRAM traffic.

Contrast with M17: the synthetic W1 trace's bulk-class elements were a random permutation of
a *footprint-sized* line set, so every window's targets spanned the whole page set and page
ordering converted a random walk into a sequential one (row hits 1.4 % → 93.6 % in the
κ sweep). A real SpMV's gather is not a random walk over a large footprint; it is a
many-to-few mapping onto a small vector. That structural difference is the whole result.

---

## 4. What the ablation does and does not establish

| Established | Not established |
|---|---|
| The two scheduler knobs are wired and reach the target stream | behaviour on a footprint larger than the window (Dubcova3 is the closest, and its ablation cell did not finish — see below) |
| On L2-resident targets, ordering changes nothing measurable | behaviour on multi-socket or multi-bank-count DRAM |
| Bank utilisation is 89–96 %, so scheduling headroom is small | behaviour with write streams present |

### 4.1 Dubcova3 ablation not completed

Dubcova3's prefix (1.59 M nonzeros, 4 945 pages of index array, 18 336 lines of `x`) is the one
trace where target ordering could plausibly matter. Its `page_sched=0` cell did not complete
within the run budget: the M6 single-cursor policy degenerates into a repeated ascending scan
of the window's page directory (≈ 5 000 pages × 8 192 entries per window × 195 windows), which
is a property of the *simulation* of that policy, not a hardware timing claim. The cell was
stopped after ten minutes and is **not** reported as a number. This is a gap in the ablation,
not evidence about the mechanism, and the conclusion above rests only on the two matrices that
did complete.

---

## 5. Limitations

1. Two matrices ablated (bcsstk25, qc2534); one further attempted and abandoned (§4.1).
2. `cohorts=16` was run on bcsstk25 only; it is identical to `cohorts=2`, so the cohort count
   above 2 is not characterised further.
3. No write streams, so the interaction between ordering and writeback is not covered.
4. The "row switches" column is the row-conflict count; the model has no separate row-switch
   counter.
5. Translation stalls are reported as `tlb0_mshr_stalls`, i.e. requests blocked because the
   TLB MSHRs were full; the model does not attribute a separate "TLB stall cycles" figure per
   client.
