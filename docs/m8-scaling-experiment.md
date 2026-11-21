# M8 Follow-up Test 1 — Workload Scaling and Pipeline Saturation

**Scope.** Measurement only. No simulator source, default configuration, architecture or
parameter was changed. Every run below is `status: OK` (the diag identity checks pass), and the
simulator's full test suite is unchanged from the M8 state (3,979 checks, 0 failures, 57 groups;
no source file was touched for this report).

**Question.** Does the M8 double-buffered, fine-grained index-acquisition pipeline scale as
indirect workloads grow?

**Answer, up front.** It scales **only up to the point where the index array exceeds the 128-slot
ring, which is N ≈ 16 K elements**. Beyond that its advantage inverts: M8 is 11.2 % slower than
M7 at N = 65 536 and 12.7 % slower at N = 262 144 on W1, because a bounded ring cannot
front-load an index array that is 32–128× its own capacity, while M7's unaccounted `std::map`
could. On W0, whose index array is the same size but whose target stream is short and
translation-clean, M8 wins at **every** size measured. The two buffers are sufficient only while
`ceil(N · idx_bytes / 64) ≤ ~128`; past that, *capacity*, not double buffering, is the limit.

---

## 1. Scaling protocol

The generator's address set is fixed by `Lambda` (footprint lines) and `P` (pages), **not** by
`N`; `N` only sets how many elements are drawn from that fixed set. Left alone, growing `N` beyond
Λ recycles the same lines, which produces duplicate addresses and (correctly) breaks the diag
identities at N ≥ 32 768. Scaling therefore required one explicit, global rule:

| | W1 (`kappa = 0`) | W0 (`kappa = 1`) |
|---|---|---|
| `Lambda` | `N` | `N` |
| `P` | `N / 32` | `N / 64` |
| `page_dir` | `N / 32` | `N / 32` |
| `bulk_index_base` | `8 MiB · N / 16 384` | same |
| `bulk_desc_line` | `98 304 · N / 16 384` | same |
| `bulk_max_elems` | 262 144 (raised from 65 536) | same |

Ratios preserved at every size: W1 keeps **1 element per line, 32 lines per page, 512 pages per
16 K elements**; W0 keeps its 4-elements-per-line streaming footprint and a page directory twice
its page count. Index and descriptor bases scale with the footprint so the target region can never
alias the index array (`bulk_data_base = 0` would otherwise overlap at N ≥ 65 536).

**Validation of the rule.** At N = 16 384 the rule reproduces the published baselines *exactly*:
W1 = 270 753 (SIMT) / 233 475 (M6) / 156 912 (M7) / 156 419 (M8); W0 = 30 573 / 39 322 / 39 530 /
38 775. Nothing in the sweep is tuned per point; the same rule generates all eight configurations.

Arm definitions (unchanged from M8): **A** SIMT, **M6** `batch_window=8192, page_sched=0,
idx_mode=0`, **M7** `page_sched=1, idx_mode=0`, **M8** `page_sched=1, idx_mode=2` (double
buffering + fine-grained readiness, `idx_buf_lines=64`, `idx_reserve=8`).

Resource limits, explicit at every size: `R = 128` total residency (unchanged); index ring 256
slots / 128 live (5.1 KB, unchanged); `idx_reserve = 8` slots withheld from targets (unchanged);
L1 16 MSHR, L2 64 MSHR, dTLB 16 × 4-way / 4 MSHR / 2 parked, L2 TLB 512 × 4-way, DRAM 2 banks.
**Only `N`, the address set and the block base addresses scale.**

---

## 2. Results

### 2.1 Total cycles

| W1 | A SIMT | M6 | M7 | **M8** | M8 vs A | M8 vs M7 |
|---|---:|---:|---:|---:|---:|---:|
| 4 096 | 66 013 | 42 824 | 36 141 | **35 881** | 1.84× | −0.72 % |
| 16 384 | 270 753 | 233 475 | 156 912 | **156 419** | 1.73× | −0.31 % |
| 65 536 | 7 461 064 | 4 117 296 | 2 626 525 | 2 920 204 | 2.55× | **+11.2 %** |
| 262 144 | 36 901 839 | 26 940 049 | 25 327 321 | 28 538 534 | 1.29× | **+12.7 %** |

| W0 | A SIMT | M6 | M7 | **M8** | M8 vs A | M8 vs M7 |
|---|---:|---:|---:|---:|---:|---:|
| 4 096 | 7 865 | 10 408 | 10 504 | **10 188** | 0.77× | −3.01 % |
| 16 384 | 30 573 | 39 322 | 39 530 | **38 775** | 0.79× | −1.91 % |
| 65 536 | 121 353 | 155 429 | 156 134 | **154 687** | 0.78× | −0.93 % |
| 262 144 | 484 473 | 621 163 | 624 051 | **618 081** | 0.78× | −0.96 % |

### 2.2 Cycles per element

| N | W1 A | W1 M7 | W1 M8 | W0 A | W0 M7 | W0 M8 |
|---:|---:|---:|---:|---:|---:|---:|
| 4 096 | 16.1 | 8.82 | 8.76 | 1.92 | 2.56 | 2.49 |
| 16 384 | 16.5 | 9.58 | 9.55 | 1.87 | 2.41 | 2.37 |
| 65 536 | 113.8 | 40.1 | 44.6 | 1.85 | 2.38 | 2.36 |
| 262 144 | 140.8 | 96.6 | 108.9 | 1.85 | 2.38 | 2.36 |

**W0 is flat at 2.36 cycles/element from 65 K to 262 K — a stable throughput limit.** W1 is not:
it climbs 6× between 16 K and 262 K, and *none* of that climb is index acquisition.

### 2.3 Index acquisition, occupancy, starvation (M8 unless noted)

| W1 N | idx lines | `wait` | `wait%` | `bufStall` | slot stall | peak in flight | lastIdxDone / cycles |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 096 | 256 | 1 439 | 4.0 % | 0 | 0 | 64 | 2 538 / 35 881 (7 %) |
| 16 384 | 1 024 | 5 981 | 3.8 % | 0 | 0 | 64 | 87 242 / 156 419 (56 %) |
| 65 536 | 4 096 | 23 567 | 0.8 % | 0 | 0 | 64 | 2 419 264 / 2 920 204 (83 %) |
| 262 144 | 16 384 | 133 538 | 0.5 % | 0 | 0 | 64 | 27 311 890 / 28 538 534 (96 %) |

M7's corresponding `slotStall` (the reason the M8 reserve exists): 26 554 / 71 769 / 0 / 0 —
the reserve matters at small and medium N, and becomes irrelevant once translation, not
arbitration, is the stall source.

| W0 N | idx lines | `wait` (M8) | `wait` (M7) | buffer stall (M7) | lastIdxDone / cycles |
|---:|---:|---:|---:|---:|---:|
| 4 096 | 256 | 4 723 | 4 650 | 0 | 2 225 / 10 188 |
| 16 384 | 1 024 | 5 210 | 3 983 | 14 113 | 24 552 / 38 775 |
| 65 536 | 4 096 | 28 175 | 6 509 | 102 951 | 136 052 / 154 687 |
| 262 144 | 16 384 | 119 833 | 6 509 | 528 641 | 599 076 / 618 081 |

**W0 is the clearest evidence for the pipe**: M7's buffer starvation explodes (528 641 cycles at
262 K) while M8's is structurally zero, and M8 still wins the run — the ring converts a stalled
buffer into steady demand-paced acquisition.

### 2.4 Throughput, translation and DRAM (M8 / M7 where they differ)

| W1 N | index lines/cycle (to last) | target req/cycle (issue span) | walks M6 / M7 / M8 | L2 TLB pages vs 512 | DRAM reads/elem | row-hit % | bank util |
|---:|---:|---:|---|---|---:|---:|---:|
| 4 096 | 0.1009 | 0.108 | 133 / 133 / 133 | 128 pages | 1.065 | 93.6 | 90.0 % |
| 16 384 | 0.0117 | 0.108 | 529 / 533 / 544 | 512 pages | 1.065 | 81.6 | 90.4 % |
| 65 536 | 0.0017 | 0.083 | 16 661 / 17 348 / 19 324 | 2 048 pages | 1.068 | 52.8 | 23.6 % |
| 262 144 | 0.0006 | 0.0092 | 168 518 / 168 794 / 190 195 | 8 192 pages | 1.074 | 19.3 | 11.7 % |

| W0 N | index lines/cycle | target req/cycle | walks | DRAM reads/elem | row-hit % | bank util |
|---:|---:|---:|---:|---:|---:|---:|
| 4 096 | 0.115 | 0.133 | 21 | 0.314 | 92.3 | 94.1 % |
| 16 384 | 0.0417 | 0.144 | 81 | 0.313 | 91.8 | 99.1 % |
| 65 536 | 0.0301 | 0.156 | 321 | 0.313 | 91.6 | 99.5 % |
| 262 144 | 0.0273 | 0.166 | 1 281 | 0.313 | 91.4 | 99.7 % |

Target throughput on W1 **falls** with N (0.108 → 0.0092 requests/cycle) exactly as walks rise,
while W0's *rises* (0.133 → 0.166) as the sweep amortises translation. That is the whole story of
the two workloads in one line.

### 2.5 Overlap (acquisition hidden by other work)

Defined as `1 − lastIdxDone / total cycles`.

| N | W1 M7 | W1 M8 | W0 M7 | W0 M8 |
|---:|---:|---:|---:|---:|
| 4 096 | 93.0 % | 92.9 % | 78.2 % | 78.2 % |
| 16 384 | 91.9 % | 44.2 % | 49.1 % | 36.7 % |
| 65 536 | 98.5 % | 17.2 % | 25.8 % | 12.0 % |
| 262 144 | 99.4 % | 4.3 % | 19.5 % | 3.1 % |

This metric is the mechanism behind §2.1: M7's unbounded completed-line map lets the index stream
run to completion early and stay out of the way (99.4 % hidden at 262 K), while the bounded ring
spreads acquisition across the entire run (4.3 % hidden). **The ring is not slower at acquiring;
it is slower because it defers acquisition into the congested part of the run.**

---

## 3. Answers to the questions

**Q1 — Does M8's advantage increase with workload size?** No. On W1 it *decreases and inverts*:
−0.72 % at 4 K, −0.31 % at 16 K, **+11.2 %** at 65 K, **+12.7 %** at 262 K (M8 slower). On W0 the
advantage is smaller but positive and roughly size-invariant (−3.0 %, −1.9 %, −0.9 %, −1.0 %).
The crossover is the point where the index array stops fitting in the ring.

**Q2 — At what size does index acquisition become a bottleneck?** Between **16 384 and 65 536
elements**. Measured markers: index ring coverage falls from 128/1 024 lines (12.5 %) to
128/4 096 (3.1 %); M8's overlap collapses 44.2 % → 17.2 %; M8 vs M7 flips −0.3 % → +11.2 %. At
N = 16 384 the ring holds one eighth of the index array and the pipeline is still free; at
65 536 it holds one thirty-second and it is not.

**Q3 — Do the two buffers remain sufficient?** Two buffers are sufficient *as a pipeline* at every
size (no deadlock, `bufStall = 0`, correct output, `idx_reserve` working). They cease to be
sufficient *as a capacity* beyond ~2 048 index lines. The requirement is not more buffers in the
double-buffering sense — it is a deeper ring: at 262 144 the array is 16 384 lines and the ring
holds 128 (0.8 %), so acquisition can never run ahead. M8 already showed the lever (E1 recovered
202 755 → 198 816 going from 64 to 128 lines/buffer); this experiment says the needed depth grows
with N, i.e. the policy must be *descriptor-sized*, not fixed.

**Q4 — What is the engine limited by?** In order of measured severity:
1. **Translation, at N ≥ 65 536.** Page set exceeds the 512-entry L2 TLB (2 048 and 8 192 pages);
   walks rise 32× (529 → 168 794) and bank utilisation collapses from 90 % to 12–24 %. SIMT
   suffers the same cliff far worse (A: 16.5 → 140.8 cycles/element), which is why *reported
   speedup vs SIMT rises* (1.73× → 2.55×) even though the engine is getting slower — the speedup
   at large N is a translation artifact, not an accelerator gain, and must not be quoted as one.
2. **Target memory / row locality, secondarily.** Row-hit rate falls 81.6 % → 19.3 % on W1 as the
   working set outgrows the open rows available; M8's row hits are *worse* than M7's at
   262 144 (54 389 vs 67 834) because its window composition shifts.
3. **Index bandwidth: never.** `wait` is ≤ 4 % of cycles everywhere, `bufStall = 0` in every M8
   run, and index lines/cycle falls only because consumption falls.
4. **Scheduling (M7 cohorts):** measured, but no longer the dominant term beyond 16 K.

**Q5 — Does performance approach a stable throughput limit?** **W0 yes, W1 no.** W0 flattens at
**2.36 cycles/element** (154 687 → 618 081 for 4× the elements) and is translation-clean (1 281
walks at 262 K, 99.7 % bank utilisation): that is the engine's steady state, and it is 1.28×
*slower* than SIMT at that limit because W0's streaming pattern needs no organisation. W1 has no
stable limit in this range — 6× worse per element between 16 K and 262 K — because every size
crosses a new translation boundary. A stable W1 limit would require holding the page set under the
L2 TLB capacity while growing the element count, which this experiment deliberately does not do
(§1), and which is the correct experiment for a *different* question.

---

## 4. Limitations

* **The large-N comparison is translation-dominated for both arms.** At 65 K and 262 K, W1's
  numbers measure the L2 TLB capacity boundary as much as the acquisition pipeline. The clean
  pipeline comparison is at 4 K and 16 K, where M8 wins. This is stated rather than corrected: a
  fixed-page-set variant would answer "does the pipe scale" without the translation confound, and
  is the obvious next experiment.
* `bulk_max_elems` was raised 65 536 → 262 144 and the block base addresses were scaled so the
  index array cannot alias the target region. Both are measurement-enabling and are reported in
  §1; neither changes any resource limit inside the engine.
* Only two sizes of index *width* were available (4 B here, 8 B in M8's E1). The ring-depth effect
  this report identifies is the same one E1 exposed, so the two are consistent, not independent.
* `status: OK` covers the simulator's internal identities (requests vs DRAM reads, index-array
  request count, element correctness, residency ≤ R). No new correctness test was added.
* Cycles per element is computed against total cycles including descriptor fetch and drain; the
  descriptor is a fixed 1-cycle-cost item, so its share shrinks with N and slightly flatters large
  N for all arms equally.
