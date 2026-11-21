# M20 — SIMT Width Sweep Results

**Configuration.** Three primary arms per cell, from one three-arm M9 run:
A = Vortex-calibrated SIMT (no Bulk), C with Bulk 2×8K, C with Bulk 2×16K. Bulk is
untouched: the window is the only capacity difference and both settings run at
every width. No per-workload or per-width tuning. `--set lanes=L` is the only knob
the experiment adds; it is required because the trace loader validates that the
trace's lane width matches the configuration (`workload.cpp:61`), which is what
made the first sweep attempt fail on every width ≠ 4.

**Traces.** Built with the M19-corrected builder (`rowptr` no longer aliases `x`).
Dubcova3 is a 45 000-row prefix (30.7 % of rows) with the complete `x` vector, and
is marked as a row-prefix experiment everywhere. The three square matrices rebuild
byte-identically to M18.

**Labels** (from `m20-simt-width-model-validation.md`): 4 lanes = calibrated
reference · 8 lanes = controlled extrapolation · 16 and 32 lanes =
**architectural sensitivity, not a GPU prediction**.

---

## 1. Summary: does Bulk win at each width?

`S_faithful` = A(full)/C(noidx) — each arm pays for its own index acquisition.
`S_same` = A(full)/C(full) — the index stream is charged twice (the M18/M19
conservative bound). All values are 2×8K unless marked (16K).

| matrix | 4 lanes | 8 lanes | 16 lanes | 32 lanes |
|---|---:|---:|---:|---:|
| bcsstk25 | **1.182** win | **1.100** win | **1.065** win | **1.058** win |
| bcsstk25 (16K) | 1.177 win | 1.100 win | 1.064 win | 1.058 win |
| qc2534 | **1.047** win | **0.942** LOSS | **0.930** LOSS | **0.930** LOSS |
| qc2534 (16K) | 1.025 win | 0.922 LOSS | 0.910 LOSS | 0.911 LOSS |
| lnsp3937 | **1.236** win | **1.188** win | **1.160** win | **1.160** win |
| lnsp3937 (16K) | 1.237 win | 1.193 win | 1.158 win | 1.158 win |
| dubcova3¹ | **1.180** win | **1.135** win | **1.125** win | **1.124** win |
| dubcova3¹ (16K) | 1.177 win | 1.132 win | 1.121 win | 1.120 win |

Same-trace pairing (`S_same`), for reference — the hybrid loses everywhere except
at 4 lanes on bcsstk25 and on all widths of lnsp3937:

| matrix | 4 | 8 | 16 | 32 |
|---|---:|---:|---:|---:|
| bcsstk25 | 0.938 / 0.914 | 1.065 / 0.921 | 0.968 / 0.884 | 0.964 / 0.876 |
| qc2534 | 0.825 / 0.826 | 0.793 / 0.763 | 0.710 / 0.706 | 0.709 / 0.704 |
| lnsp3937 | 1.180 / 1.183 | 1.158 / 1.163 | 1.131 / 1.133 | 1.131 / 1.133 |
| dubcova3¹ | 0.880 / 0.880 | 0.895 / 0.896 | 0.885 / 0.884 | 0.881 / 0.880 |

¹ row-prefix experiment: 45 000 of 114 599 rows, complete `x`.

The 4-lane column reproduces M18/M19 published values exactly on every matrix
(dubcova3 at the M19-corrected 1.180/1.177), which is the control showing the
width sweep changed nothing but `lanes`.

## 2. Answer

**The M18 advantage does not survive wider SIMT in general.** With the brief's
four permitted conclusions, the measured outcome is:

* **qc2534: the advantage disappears and inverts at 8 lanes.** 1.047 → 0.942 →
  0.930 → 0.930. From 8 lanes up the Bulk hybrid is 6–7 % *slower* than the plain
  baseline under the faithful pairing, and 21–30 % slower under the same-trace
  pairing.
* **bcsstk25: shrinks by more than half but survives.** 1.182 → 1.058 (−55 % of
  the surplus in ratio terms, −70 % in absolute cycles).
* **lnsp3937: shrinks by a third, survives comfortably.** 1.236 → 1.160.
* **dubcova3 (prefix): shrinks by a third, survives.** 1.180 → 1.135 → 1.125 →
  1.124, i.e. −31 % of the surplus, and it keeps the largest residual advantage of
  the four at 32 lanes.
* **No workload improves with width**, and nothing crosses back.

So the honest overall verdict is **option 2, "shrinks substantially", with a
qualifier**: on one of four workloads it is stronger than that — the advantage is
not merely reduced but reversed. A single-sentence conclusion for the M18 claim:
*Bulk's advantage at the Vortex-calibrated 4-lane baseline is not a property of
Bulk; it is a property of the baseline being request-starved, and it survives only
where the index stream stays expensive at wide warps.*

## 3. Absolute numbers (2×8K window, M20 traces)

### 3.1 bcsstk25 (15 439², 252 241 nnz)

| width | A cyc | A req | A DRAM | C(noidx) cyc | req | DRAM | C(full) cyc | req | DRAM | S_f | S_same |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 461 688 | 323 809 | 50 245 | 390 495 | 126 692 | 50 246 | 492 449 | 206 260 | 60 604 | 1.182 | 0.938 |
| 8 | 422 756 | 254 854 | 50 245 | 384 324 | 101 035 | 50 246 | 396 901 | 153 042 | 50 681 | 1.100 | 1.065 |
| 16 | 409 708 | 217 549 | 50 245 | 384 663 | 87 984 | 50 246 | 423 380 | 126 060 | 55 064 | 1.065 | 0.968 |
| 32 | 407 291 | 197 927 | 50 245 | 384 957 | 80 988 | 50 246 | 422 560 | 111 552 | 54 981 | 1.058 | 0.964 |

### 3.2 qc2534 (2 534², 463 360 nnz)

| width | A cyc | A req | A DRAM | C(noidx) cyc | req | DRAM | C(full) cyc | req | DRAM | S_f | S_same |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 762 503 | 455 640 | 87 447 | 728 062 | 191 697 | 87 449 | 923 790 | 328 896 | 113 113 | 1.047 | 0.825 |
| 8 | 682 956 | 301 609 | 87 447 | 725 237 | 141 741 | 87 449 | 861 007 | 224 821 | 106 818 | 0.942 | 0.793 |
| 16 | 674 541 | 229 944 | 87 447 | 725 261 | 117 849 | 87 449 | 950 360 | 175 057 | 115 289 | 0.930 | 0.710 |
| 32 | 674 694 | 190 842 | 87 447 | 725 201 | 104 817 | 87 449 | 951 187 | 147 901 | 115 283 | 0.930 | 0.709 |

### 3.3 lnsp3937 (3 937², 22 704 nnz)

| width | A cyc | A req | A DRAM | C(noidx) cyc | req | DRAM | C(full) cyc | req | DRAM | S_f | S_same |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 57 014 | 40 106 | 5 511 | 46 113 | 16 508 | 5 512 | 48 298 | 25 605 | 5 512 | 1.236 | 1.180 |
| 8 | 54 057 | 34 284 | 5 511 | 45 491 | 14 150 | 5 512 | 46 675 | 20 721 | 5 512 | 1.188 | 1.158 |
| 16 | 51 970 | 31 266 | 5 511 | 44 789 | 12 951 | 5 512 | 45 956 | 18 228 | 5 512 | 1.160 | 1.131 |
| 32 | 51 970 | 31 266 | 5 511 | 44 789 | 12 951 | 5 512 | 45 956 | 18 228 | 5 512 | 1.160 | 1.131 |

### 3.4 dubcova3 prefix (45 000 of 114 599 rows, 146 689 cols, 1 619 539 nnz)

| width | A cyc | A req | A DRAM | C(noidx) cyc | req | DRAM | C(full) cyc | S_f | S_same |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 3 531 289 | 1 948 034 | 383 142 | 2 992 191 | 840 395 | 381 455 | 4 014 770 | 1.180 | 0.880 |
| 8 | 3 397 398 | 1 500 305 | 383 141 | 2 994 097 | 669 800 | 382 159 | 3 797 476 | 1.135 | 0.895 |
| 16 | 3 367 312 | 1 254 783 | 383 143 | 2 993 283 | 570 802 | 382 604 | 3 806 560 | 1.125 | 0.885 |
| 32 | 3 366 104 | 1 105 025 | 383 148 | 2 994 887 | 514 217 | 382 702 | 3 822 470 | 1.124 | 0.881 |

Three facts hold on every workload:

1. **DRAM traffic is width-invariant.** A's DRAM reads are identical at all four
   widths on the square matrices (50 245 / 87 447 / 5 511); C(noidx) differs by
   exactly one line (50 246 / 87 449 / 5 512 — the engine's descriptor read). On
   the dubcova3 prefix the spread is 6 lines in 383 000 (383 142 / 383 141 /
   383 143 / 383 148), i.e. rounding. Widening SIMT changes how requests are
   formed, never what must be fetched, so every cycle difference below is a
   request-path difference. (The dubcova3 merge also *reduces* DRAM reads,
   383 142 → 381 455 / 382 159, because its 1.17 MB `x` is the only one of the four
   that exceeds the 1 MB L2, so duplicate gathers would otherwise have missed.)
2. **The baseline's request count falls far faster than its cycles.** bcsstk25:
   −39 % requests, −11.8 % cycles. qc2534: −58 % requests, −11.5 % cycles.
   lnsp3937: −22 % requests, −8.9 % cycles. dubcova3: −43 % requests, −4.7 %
   cycles. The machine is memory-bound; the front end was never the bottleneck it
   appeared to be at 4 lanes.
3. **The hybrid's cycles barely move**: bcsstk25 −1.4 %, qc2534 −0.4 %, lnsp3937
   −2.9 %, dubcova3 +0.09 % for `C(noidx)` from 4 to 32 lanes. The rest of the
   sweep is the story of why: the hybrid's critical path is not the SIMT issue
   path.

## 4. What each arm pays, per width

`ΔA_idx` = A(full) − A(noidx): the index stream's marginal cost in the baseline.
`ΔC_eng` = C(noidx) − A(noidx): the engine's entire overhead relative to doing the
same gathers in SIMT. `S_f = 1 + (ΔA_idx − ΔC_eng)/C(noidx)` (M19 §1).

| dataset | width | ΔA_idx | ΔC_eng | surplus | S_f |
|---|---:|---:|---:|---:|---:|
| bcsstk25 | 4 | 152 984 | 81 791 | +71 193 | 1.182 |
| bcsstk25 | 8 | 136 251 | 97 819 | +38 432 | 1.100 |
| bcsstk25 | 16 | 132 543 | 107 498 | +25 045 | 1.065 |
| bcsstk25 | 32 | 133 710 | 111 376 | +22 334 | 1.058 |
| qc2534 | 4 | 315 903 | 281 462 | +34 441 | 1.047 |
| qc2534 | 8 | 248 407 | 290 688 | −42 281 | 0.942 |
| qc2534 | 16 | 240 027 | 290 747 | −50 720 | 0.930 |
| qc2534 | 32 | 240 217 | 290 724 | −50 507 | 0.930 |
| lnsp3937 | 4 | 19 592 | 8 691 | +10 901 | 1.236 |
| lnsp3937 | 8 | 18 696 | 10 130 | +8 566 | 1.188 |
| lnsp3937 | 16 | 18 690 | 9 947 | +8 743 | 1.160 |
| lnsp3937 | 32 | 18 690 | 9 947 | +8 743 | 1.160 |
| dubcova3¹ | 4 | 1 056 991 | 517 893 | +539 098 | 1.180 |
| dubcova3¹ | 8 | 967 695 | 564 394 | +403 301 | 1.135 |
| dubcova3¹ | 16 | 944 027 | 569 998 | +374 029 | 1.125 |
| dubcova3¹ | 32 | 944 348 | 573 131 | +371 217 | 1.124 |

**The prize shrinks while the price grows.** `ΔA_idx` falls 4–24 % as wider warps
coalesce the index stream better; `ΔC_eng` does not fall at all — it rises 10–36 %
— because the engine's overhead is `slot_stall` + `idx_wait` + gather misses, and
neither term improves with SIMT width. qc2534 is where the two lines cross: its
`ΔA_idx` loses 67 496 cycles between 4 and 8 lanes while `ΔC_eng` gains 9 226, and
the surplus flips from +34 441 to −42 281.

Index share of the baseline's runtime (`ΔA_idx/A`): bcsstk25 33 %, qc2534 41 %,
lnsp3937 34 %, dubcova3 30 %. That share is the size of the prize; it does not
grow with width, which is why nothing crosses back.

## 5. Required metrics, per width

Full tables: `tools/m20_extract.py memory` and `... mechanism`. Behaviour summary:

| metric | across 4 → 32 lanes (bcsstk25, 2×8K) | interpretation |
|---|---|---|
| cycles | A 461 688 → 407 291 (−11.8 %), C(noidx) 390 495 → 384 957 (−1.4 %) | asymmetry is the result |
| speedup | 1.182 → 1.058 | §2 |
| DRAM reads | A 50 245 constant; C 50 246 constant | traffic is width-invariant |
| DRAM writes | 0 everywhere | read-only traces; no write path (M19 I3) |
| row-hit rate | A 80.0 → 82.4 %; C 86.8 → 88.8 % | engine gains locality (merged, ordered); SIMT gains a little from bigger windows |
| L1 reads / hit | A 323 809/35.1 % → 197 927/33.2 %; C 206 260/3.1 % → 111 552/1.0 % | fewer, coarser accesses; the hybrid's L1 hit rate falls because its requests are already deduplicated by the engine |
| L2 reads / hit | A 55 407/9.3 % → 55 355/9.2 %; C 68 894/12.0 % → 68 951/20.3 % | the hybrid's L2 hit rate *rises*: it re-references fewer distinct lines |
| TLB misses / walks | A 1 575/788 constant; C 2 068/787 → 1 954/787 | pages are touched once either way; the engine pays ~30 % more first-touch misses (2 068 vs 1 575) because its access order is line-dense |
| index requests | 15 766 at every width | the engine's acquisition is width-invariant by construction |
| index phase | 268 236 (l4) → 290 869 (l32) | the engine's acquisition *lengthens* 8 % and then plateaus |
| target requests | 3 431 at every width, 1 931 DRAM | the merge is width-invariant |
| target phase | 269 584 → 293 771 | follows the index phase |
| occupancy | `peak_total_outstanding` = 128 at every width | the pool is saturated throughout |
| win occupancy | `bulk_win_occ_max` = 8192 / 16384 | the window fills to capacity; it is not under-filled |
| coalescer staging (peak) | 4 (l4) → 8 → 13 → 18 | the latch holds a growing partial group; at 32 lanes it never reaches 32, i.e. a full group is never formed |
| stalls | `bulk_slot_stall` 348 704 (qc2534 l4) → 696 816 (l8) | the engine loses residency slots as SIMT widens |
| bytes | 64 B per request, no sectors | M20 validation §2 |

qc2534's stall and phase numbers (the workload that inverts):

| width | idx wait | slot stall | win-full stall | AG cycles | C(full) cycles |
|---:|---:|---:|---:|---:|---:|
| 4 | 140 615 | 348 704 | 86 303 | 484 276 | 923 790 |
| 8 | 108 263 | 696 816 | 94 176 | 861 007 | 861 007 |
| 16 | 83 461 | 660 554 | 151 441 | 950 360 | 950 360 |
| 32 | 84 252 | 660 373 | 151 609 | 951 187 | 951 187 |

`AG cycles` equal to the total cycle count means the engine is active for the
entire run: at 4 lanes it finishes its index acquisition and merge in the first
half (484 276 of 923 790) and then waits; at 8 lanes and above there is no such
tail. This is quantified in `m20-mechanism-analysis.md` §4.

## 6. Reproduce

```
cd DX100_MEM
bash build/wm/m20_matrix.sh          # 64 cells
python3 tools/m20_extract.py summary # cycles and both speedups per cell
python3 tools/m20_extract.py memory  # DRAM / cache / TLB per arm
python3 tools/m20_extract.py mechanism
python3 tools/m20_extract.py split
```

Determinism: the model has no RNG on the traced path (`seed` only parameterises
the synthetic generator). Three repeat runs of one cell
(bcsstk25, l16, 2×8K, full) give bit-identical output: 409 708 / 0 / 423 380
cycles with identical completion hashes and identical engine counters
(`bulk_elems_done` 252 241, `bulk_idx_lines_req` 15 766). Where randomness does
not exist, three paired seeds cannot be run; three identical repeats is the
stronger statement and it is what was done.
