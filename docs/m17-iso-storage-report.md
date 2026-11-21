# M17 Phase 5 — Iso-storage L2 comparison

**Question.** Can an enlarged conventional L2, given storage equal to the Bulk
window's realized footprint, reproduce the accelerator's benefit?

**Answer.** No. Giving the SIMT-only baseline **901 824 B** of extra L2 — exactly
the window storage the implementation realizes at κ = 0.25, N = 16 384 — makes it
**0.57 %** faster (220 019 → 218 771 cycles) and moves the speedup from
**1.769× to 1.777×**. Giving it 1.5 MiB extra stops helping entirely (2.5 MiB is
identical to 1.875 MiB, with L2 evictions at zero). At κ = 0.50 and 0.75 the extra
capacity changes the ratio by less than 0.005 in the *other* direction.

The reason is measurable and not a matter of capacity: at the largest L2 the
SIMT arm issues **12 559 DRAM reads against 12 636 distinct lines in the trace**,
i.e. almost exactly one fetch per distinct line. Those are compulsory misses; no
amount of cache removes them. What separates the two arms is the **order** in
which the same misses are issued — 83.7 % row hits for the hybrid against 3.9 %
for SIMT at κ = 0.25 — and ordering is not a capacity.

---

## 1. Method, and what "iso-storage" is allowed to mean

The M16 iso-storage methodology is reused unchanged: vary only `l2_lines`,
keeping the workload, the timing model, the associativity and the accounting
rules identical to every other M17 cell.

| item | value |
|---|---|
| cells | 80 (κ ∈ {0.25, 0.50, 0.75} × 4 capacities × 5 seeds, at 16 MSHRs; κ = 0.25 also at 128 MSHRs) |
| capacities | 16 384 (1 MiB, published), 20 480 (1.25 MiB), 30 720 (1.875 MiB), 40 960 (2.5 MiB) |
| L2 | 8-way in every cell, 64 MSHRs, 4-cycle latency |
| window | 2×8K, `dedup` 1, `page_order` 1, `page_sched` 1, cohorts 2 |
| seeds | 5 paired |

**Two different things are being equated, and the report keeps them apart.**

* *Cache capacity* is tag-checked storage that serves arbitrary addresses and
  must be searched. Its value is that a hit avoids a DRAM access.
* *Total accelerator storage* is the window's own SRAM (elements, issue order,
  cohort records) plus the index buffer and descriptor. Its value is that the
  engine already knows, without a lookup, which lines the current batch needs and
  in what order to fetch them.

Matching bytes matches neither function nor access cost. The comparison below is
therefore a **generous upper bound on what capacity can buy**: the SIMT arm gets
the full byte budget as cache, with no tag or bandwidth cost charged for the
increase, and the accelerator's own bookkeeping is not subtracted from it.

## 2. Storage accounting (measured, not estimated)

| quantity | value | source |
|---|---|---|
| Bulk window storage, realized, κ = 0.25, 2×8K, N = 16 384 | **901 824 B** | `bulk_win_vec_bytes`, exported by every cell |
| fused-window storage, same cell | 0 B | `bulk_win_fused_vec_bytes` (fused representation off) |
| window storage, N = 65 536, 2×8K (M16 measurement) | 933 856 B | M16 Part C; **not** used here |
| index buffer | 4 096 B (64 lines × 64 B) | `idx_buf_lines` = 64 |
| descriptor | 64 B (1 line) | `bulk_desc_line_bytes` |
| L2 baseline | 1 048 576 B (16 384 lines) | `l2_lines`, 8-way |

Two corrections to the naive estimate are worth stating because they change the
number a reader might guess:

1. **The realized window is 901 824 B, not 2 × 8 192 × 8 B = 131 072 B.** The
   window stores per-element records plus issue order and cohort records; the
   design's 8 B/entry charge is a floor, not the realized size.
2. **It is not the M16 figure either.** M16 measured 933 856 B at N = 65 536;
   at N = 16 384 the same configuration realizes 901 824 B, and the 2×16K
   *layout* realizes 893 632 B — fewer, larger windows means less per-window
   slack. Using M16's number here would have overstated the budget by 3.6 %.

Capacity points, and what each brackets:

| L2 lines | size | extra over 1 MiB | what it is |
|---|---|---|---|
| 16 384 | 1 024 KiB | — | published baseline |
| 20 480 | 1 280 KiB | 262 144 B | M16's fused-window bracket; over-covers the 245 744 B fused footprint measured in M16 |
| 30 720 | 1 875 KiB | 917 504 B | **iso-storage point**: matches the measured 901 824 B window to within 1.7 % |
| 40 960 | 2 560 KiB | 1 572 864 B | deliberate overshoot (M16 bracket) |

## 3. Results

κ = 0.25, 16 MSHRs (mean of five paired seeds):

| L2 lines | L2 size | SIMT-only | SD | hybrid | SD | **A/C** | SIMT vs 1 MiB | L2 hit % (A) | L2 hit % (C) | DRAM reads (A) | DRAM reads (C) | evictions (C) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 16 384 | 1 024 KiB | 220 019 | 1 710 | 124 406 | 1 134 | **1.7686** | 1.0000 | 11.3 | 10.6 | 12 662 | 13 469 | 4 881 |
| 20 480 | 1 280 KiB | 219 340 | 1 841 | 123 656 | 1 148 | 1.7738 | 1.0031 | 11.7 | 11.2 | 12 603 | 13 380 | 2 842 |
| 30 720 | 1 875 KiB | 218 771 | 1 855 | 123 114 | 1 166 | **1.7770** | 1.0057 | 12.0 | 11.6 | 12 559 | 13 329 | 272 |
| 40 960 | 2 560 KiB | 218 766 | 1 845 | 123 114 | 1 166 | 1.7769 | 1.0057 | 12.0 | 11.6 | 12 558 | 13 327 | 0 |

At 128 MSHRs the same pattern, one step lower: 1.7481 → 1.7540 → 1.7582 → 1.7582.

κ = 0.50 and κ = 0.75, 16 MSHRs:

| κ | L2 lines | SIMT-only | hybrid | A/C | SIMT vs 1 MiB | evictions (C) |
|---|---|---|---|---|---|---|
| 0.50 | 16 384 | 168 884 | 93 542 | **1.8054** | 1.0000 | 1 589 |
| 0.50 | 20 480 | 168 544 | 93 523 | 1.8022 | 1.0020 | 568 |
| 0.50 | 30 720 | 168 445 | 93 513 | 1.8013 | 1.0026 | 21 |
| 0.50 | 40 960 | 168 440 | 93 513 | 1.8012 | 1.0026 | 0 |
| 0.75 | 16 384 | 119 664 | 95 140 | **1.2578** | 1.0000 | 151 |
| 0.75 | 20 480 | 119 635 | 95 140 | 1.2575 | 1.0002 | 25 |
| 0.75 | 30 720 | 119 630 | 95 140 | 1.2574 | 1.0003 | 1 |
| 0.75 | 40 960 | 119 630 | 95 140 | 1.2574 | 1.0003 | 0 |

Per-seed ratios are in `m17_tables.md` P5.

## 4. Why capacity cannot substitute

**4.1 The misses are compulsory.** At the iso-storage point the SIMT arm issues
12 559 DRAM reads and the trace has 12 636 distinct lines: 99.4 % of the reads
are first touches. The engine issues 13 329 — 5.5 % *more than the distinct-line
count*, because of the index array and descriptor — and is 1.78× faster. Fewer
misses is not what the accelerator delivers; better-ordered misses is.

**4.2 The capacity that was added is already enough to eliminate evictions.**
Hybrid L2 evictions fall 4 881 → 272 → 0 across the sweep, and the L2 hit rate
moves by 1.0 percentage point (10.6 % → 11.6 %). The extra 0.9 MiB removes the
capacity pressure completely — and buys 1.04 % of the hybrid's runtime.

**4.3 Locality, not retention, is the binding constraint.** At κ = 0.25 the SIMT
arm has a 790 KiB working set that fits in the 1 MiB L2, yet its row-hit rate is
3.9 % and its dTLB miss count is 12 206. Both are ordering problems. A bigger L2
cannot reorder a request stream.

**4.4 At high κ the result is reversed and equally small.** At κ = 0.75 the extra
0.9 MiB makes the SIMT arm 0.03 % faster and the ratio moves from 1.2578 to
1.2574 — the hybrid gains nothing at all. There is nothing left to buy: the trace
already fits and the arms are distinguished by row locality, not retention.

## 5. Assumptions and what this comparison does not show

| assumption | direction of the bias |
|---|---|
| the extra L2 is free (no tag area, no added latency, no bandwidth cost) | favours SIMT |
| associativity held at 8-way across capacities | neutral to slightly favours SIMT (a real 2.5 MiB cache might use different geometry) |
| the accelerator's index buffer (4 KiB) and descriptor (64 B) are **not** charged against the SIMT budget | favours SIMT (the accelerator pays them and still wins) |
| window bookkeeping (cohort records, page directory) is not quantified here | small, but nonzero; not subtracted |
| both arms share the same L2 in the hybrid | this is the published model, not a simplification |

What it does **not** show: that a larger L2 is worthless in general. Under a
capacity-bound workload (footprint > L2, high reuse) it is exactly the right
resource — M15's L2 diagnostic at κ = 0.75, N = 65 536 shows SIMT improving
28.8 % from 8 192 → 32 768 lines (334 076 → 237 765 cycles). This report shows only that, **at the footprints and
reuse distances where the accelerator wins**, capacity is not the resource that
explains the win.

## 6. Methods note: the two arms are not physically identical

The brief asks that the implementations not be described as identical because
capacities match. They are not:

| | SIMT + enlarged L2 | Bulk window |
|---|---|---|
| storage kind | cache (tag check, set index, replacement) | SRAM holding the current batch's elements and their order |
| what it buys | a hit avoids a DRAM access | the engine knows the batch's lines and their page order without a lookup |
| cost model here | free capacity increase | 901 824 B realized + tag-free access |
| ordering | none added (the request stream is the trace's order) | ascending by page, then by line |

The comparison is one of **storage budget**, not of equivalent hardware, and the
conclusion ("capacity does not reproduce the benefit") is a statement about the
budget, not about cache design.

## 7. Limitations

* **One geometry.** All cells are W1 with Λ = 16 384, P = 512, N = 16 384 except
  where stated. A footprint that exceeded the 2.5 MiB bracket would change the
  picture, and this sweep does not reach one.
* **Associativity held fixed at 8-way**, as the brief requests "where feasible";
  it is feasible here and is held fixed, so a fully-associative ideal cache is not
  among the points.
* **The 20 480 point brackets the fused representation, not the 2×8K window.**
  It is retained for comparability with M16 and is labelled as such rather than
  presented as a measured iso-storage match.
* **No DRAM controller model for the extra capacity.** A real 2.5 MiB last-level
  cache would add latency and area; the model charges neither.
