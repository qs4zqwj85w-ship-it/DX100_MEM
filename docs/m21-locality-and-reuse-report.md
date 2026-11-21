# M21 — Locality and Reuse Report

What each workload's target stream actually looks like, measured offline from
the trace (`tools/m21_reuse.py`) and then confirmed against the simulator's own
cache, DRAM and TLB counters. The point of this report is to establish which
structural numbers predict performance — and which do not.

---

## 1. Structural measurements (seed 1, 4 lanes)

| workload | tgt lines | tgt pages | WS | dup(line) | dup(word) | reuse < 8K | idx req | tgt req |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| w1_uniform | 2 048 | 32 | 128 KiB | 0.9844 | 0.8750 | 0.967 | 32 768 | 130 972 |
| w2_clustered | 2 044 | 32 | 128 KiB | 0.9844 | 0.9289 | 0.935 | 32 768 | 129 501 |
| w3_duphigh | 64 | 1 | 4 KiB | 0.9995 | 0.9975 | 1.000 | 32 768 | 127 673 |
| w4_duplow | 131 072 | 2 048 | 8 192 KiB | 0.0000 | 0.0000 | 0.000 | 32 768 | 131 072 |
| w5_sequential | 16 384 | 256 | 1 024 KiB | 0.8750 | 0.0000 | 0.875 | 32 768 | 32 768 |
| w6_zipf | 2 048 | 32 | 128 KiB | 0.9844 | 0.9002 | 0.925 | 32 768 | 117 813 |
| w7_wssmall | 256 | 4 | 16 KiB | 0.9980 | 0.9844 | 0.998 | 32 768 | 130 285 |
| w8_wslarge | 32 206 | 512 | 2 013 KiB | 0.7543 | 0.2124 | 0.214 | 32 768 | 131 065 |
| w9_seqsmall | 256 | 4 | 16 KiB | 0.9980 | 0.9844 | 0.998 | 32 768 | 32 768 |
| w10_runresid | 128 | 2 | 8 KiB | 0.9990 | 0.9922 | 0.999 | 32 768 | 129 555 |

`tgt req` is the number of 64 B line requests the four-lane SIMT coalescer forms
for the gather stream — the quantity the engine's window can reduce. `idx req` is
the index-stream request count, **32 768 in every workload** (131 072 elements /
4 lanes = one 16 B request per instruction, and no two lanes of a group ever share
a line because the index array is dense and 4 B-strided).

Two structural facts to carry forward:

* the index stream is a constant, so it cannot explain any difference;
* `dup(line)` ranges from 0.000 to 0.9995 and is **not** monotone with the
  measured speedup (§4).

## 2. Reuse-distance distributions

Access-granular stack distance over the target stream (number of *target*
accesses since this line was last touched). Bins are small distances first.

| workload | cold | 1–16 | 17–128 | 129–1K | 1K–8K | 8K–64K | >64K |
|---|---:|---:|---:|---:|---:|---:|---:|
| w1_uniform | 2 048 | 1 051 | 7 026 | 43 634 | 75 041 | 2 272 | 0 |
| w2_clustered | 2 044 | 15 776 | 65 219 | 38 174 | 3 364 | 6 317 | 178 |
| w3_duphigh | 64 | 32 104 | 81 949 | 16 937 | 18 | 0 | 0 |
| w4_duplow | 131 072 | 0 | 0 | 0 | 0 | 0 | 0 |
| w5_sequential | 16 384 | 114 688 | 0 | 0 | 0 | 0 | 0 |
| w6_zipf | 2 048 | 47 959 | 25 403 | 25 627 | 22 251 | 7 735 | 49 |
| w7_wssmall | 256 | 8 068 | 43 667 | 76 682 | 2 399 | 0 | 0 |
| w8_wslarge | 32 206 | 73 | 445 | 3 381 | 24 136 | 65 771 | 5 060 |
| w9_seqsmall | 256 | 114 688 | 0 | 0 | 16 128 | 0 | 0 |
| w10_runresid | 128 | 15 438 | 67 634 | 47 837 | 35 | 0 | 0 |

The distributions separate the families cleanly and independently of any
performance number:

* **w4** is pure cold traffic: 131 072 accesses, every one a first touch. There is
  nothing to merge, by construction.
* **w5** is the other extreme of the same statistic: 16 384 cold touches (one per
  line) and 114 688 hits at distance ≤ 16 — maximal spatial reuse, zero word
  duplicates.
* **w3/w7** have reuse concentrated inside 128 accesses (the working set is
  smaller than a window).
* **w8** has 32 206 cold lines and a long-distance body (65 771 at 8K–64K): a
  large set with almost no short-range reuse — 21.4 % of accesses recur within an
  8K window.
* **w1** is the "textbook uniform random" shape: reuse distributed over 129–8K,
  with 96.7 % of accesses recurring within 8 192 accesses.

## 3. Confirmation from the simulator's own counters

The trace-side numbers are only useful if the machine agrees with them. It does.
Per-arm measurements at 8K, the `full` and `noidx` variants (`tools/m21_extract.py
memory`):

| workload | arm | DRAM rd | row-hit | L1 hit | L2 hit | TLB miss | walks |
|---|---|---:|---:|---:|---:|---:|---:|
| w1_uniform | A | 27 165 | 76.3 % | 9.2 % | 80.7 % | 92 134 | 425 |
| w1_uniform | C | 30 231 | 84.1 % | 1.5 % | 54.3 % | 1 950 | 426 |
| w3_duphigh | A | 25 181 | 86.8 % | 64.6 % | 3.0 % | 786 | 394 |
| w3_duphigh | C | 25 246 | 90.7 % | 4.5 % | 27.9 % | 1 037 | 395 |
| w4_duplow | A | 156 321 | **0.1 %** | 4.0 % | 55.9 % | **143 165** | **99 317** |
| w4_duplow | C | 163 513 | **45.5 %** | 1.5 % | 28.8 % | **62 955** | **32 740** |
| w5_sequential | A | 41 515 | 78.6 % | 3.6 % | 2.9 % | 1 296 | 649 |
| w5_sequential | C | 46 244 | 85.4 % | 1.7 % | 9.1 % | 1 465 | 650 |
| w7_wssmall | A | 25 372 | 84.9 % | 47.4 % | 54.5 % | 792 | 397 |
| w7_wssmall | C | 25 374 | 90.6 % | 4.2 % | 33.4 % | 1 139 | 398 |
| w8_wslarge | A | 107 460 | **6.2 %** | 3.2 % | 32.7 % | 149 961 | 2 302 |
| w8_wslarge | C | 108 552 | **67.5 %** | 1.2 % | 28.8 % | 12 991 | 1 613 |

Three structural signatures, each measured rather than assumed:

1. **DRAM row locality is the big differentiator.** The two workloads with a
   footprint far above L2 are exactly the two where the baseline's row-hit rate
   collapses (w4 0.1 %, w8 6.2 %) and the hybrid restores it (45.5 %, 67.5 %).
   Every cache-resident workload sits at 76–87 % for both arms, where there is
   nothing to restore.
2. **Translation pressure follows the page count, not the duplicate rate.** w4
   touches 2 048 pages against a 16-entry DTLB and 512-entry L2 TLB: the baseline
   takes 143 165 DTLB misses and 99 317 walks. w1 touches only 32 pages, so its
   baselines takes 92 134 DTLB misses but only 425 walks — the L2 TLB absorbs
   them. w3 touches one page and takes 786 misses.
3. **L1 hit rate is a poor guide here.** The hybrid's L1 hit rate is *lower* than
   the baseline's in every workload (its requests are already deduplicated and
   line-ordered, so fewer of them repeat), yet it is faster wherever the target
   set exceeds L2. Optimising for L1 hit rate would point the wrong way.

## 4. Duplicate rate does not predict the result

| workload | dup(line) | tgt req / unique lines | S_f |
|---|---:|---:|---:|
| w4_duplow | 0.0000 | 1.00 | **2.994** |
| w8_wslarge | 0.7543 | 4.07 | **1.532** |
| w1_uniform | 0.9844 | 63.95 | 1.159 |
| w5_sequential | 0.8750 | 2.00 | 1.132 |
| w7_wssmall | 0.9980 | 509 | 1.087 |
| w3_duphigh | 0.9995 | 1 995 | 1.039 |
| w2_clustered | 0.9844 | 63.4 | 1.038 |
| w10_runresid | 0.9990 | 1 012 | 1.041 |

The correlation between duplicate rate and speedup is **negative**: the workload
with no duplicates at all is the largest win (2.99×) and the workload with a
99.95 % duplicate rate is near parity (1.04×). The reason is visible in the same
table: when duplicates are plentiful (w3, w7, w10) the whole target set fits in
L1/L2, so both arms are already fast and the merge only removes accesses that
would have hit in cache. When duplicates are absent (w4) the target set is 8 MiB,
both arms are DRAM-bound, and the engine's contribution is not merging but
*ordering* — which the ablation in `m21-mechanism-ablation-report.md` isolates
exactly (`page_order=0` takes w4 from 2.994 to 1.001).

What predicts performance instead is the pair
**(target working set relative to L2, target address entropy)**:

| regime | condition (measured) | workloads | S_f |
|---|---|---|---:|
| ordered-DRAM regime | `tgt WS` > 1 MiB and reuse < 0.25 | w4 (8 MiB, 0.000), w8 (2 MiB, 0.214) | 2.99, 1.53 |
| mixed | WS 1 MiB, ordered | w5 | 1.13 |
| cache-resident regime | `tgt WS` ≤ 128 KiB, reuse ≥ 0.92 | w1, w2, w6, w7, w10, w3 | 1.04–1.16 |

The mixed row is what makes the pair — not the working set alone — the predictor:
w5 has a 1 MiB working set (equal to L2) but perfect spatial order, so it lands
between the regimes at 1.13, close to the cache-resident cluster despite its size.

## 5. What this means for prediction

A workload's position is computable from its index vector alone, before any
simulation: unique target lines and pages, working-set bytes, duplicate rate, and
the reuse-distance histogram. Of those, three quantities carry all the predictive
power measured here:

1. **`target_working_set_bytes` vs 1 MiB (L2)** — tells you whether the gather
   touches DRAM at all.
2. **`fraction of target accesses recurring within 8 192 accesses`** (the window)
   — tells you whether ordering has anything to reorder, and how much merging is
   available.
3. **duplicate rate ≥ ~0.99 with a resident working set** — the marker of the
   *parity* corner: the merge works, but there is nothing left to save.

Duplicate rate on its own is not a predictor, and using it as one is what would
have produced the wrong expectation for this matrix (a "duplicate-optimised"
study would have predicted w3 to win and w4 to lose; both predictions are
backwards).
