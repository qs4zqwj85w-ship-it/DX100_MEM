# M21 — Performance Boundaries

All results are 4 lanes (the only calibrated configuration), 2×8K unless stated,
`S_faithful` = A(full)/C(noidx) with each arm paying for its own index
acquisition, `S_same` = A(full)/C(full) charging the index stream twice.

---

## 1. The boundary surface

| workload | target set | S_f 8K (s1/s2/s3) | S_f 16K | S_same 8K | verdict |
|---|---|---:|---:|---:|---|
| w4_duplow | 8 MiB, 2 048 pages, no reuse | **2.994** / 2.997 / 2.995 | **5.655** | 2.943 | large win |
| w8_wslarge | 2 MiB, 512 pages, reuse 0.21 | **1.532** / 1.527 / 1.525 | **1.690** | 1.429 | large win |
| w1_uniform | 128 KiB, 32 pages, reuse 0.97 | 1.159 / 1.160 / 1.159 | 1.164 | 0.928 | win |
| w5_sequential | 1 MiB, 256 pages, ordered | 1.132 / 1.132 / 1.132 | 1.138 | 0.984 | win |
| w7_wssmall | 16 KiB, 4 pages | 1.087 / 1.079 / 1.079 | 1.097 | 1.054 | win |
| w6_zipf | 128 KiB, 32 pages, skewed | 1.074 / 1.075 / 1.077 | 1.081 | 0.883 | win |
| w9_seqsmall | 16 KiB, 4 pages, ordered | 1.064 | — | 1.040 | win (marginal) |
| w10_runresid | 8 KiB, 2 pages | 1.041 | — | 0.997 | parity |
| w3_duphigh | 4 KiB, 1 page | 1.039 / 1.039 / 1.038 | 1.039 | 0.990 | parity |
| w2_clustered | 128 KiB, 90 % in 6.4 KiB | 1.038 / 1.039 / 1.038 | 1.048 | 0.873 | parity |

Across three seeds the largest spread is 0.008 (w7); the ranking of the ten
workloads is identical at every seed.

## 2. Two regimes, and the quantities that separate them

**Regime A — target set exceeds L2 (1 MiB).** The gather is DRAM traffic, and the
baseline issues it in program order, which is arbitrary. The engine reorders it by
line and page. The win is 1.5–3.0× and grows with the footprint and with the
absence of reuse: w4 (8 MiB, zero reuse) 2.99×, w8 (2 MiB, 21 % reuse) 1.53×.

**Regime B — target set fits in L2.** Ordering has nothing to restore; the only
benefit left is consolidating the index stream, which the baseline reads 4 B at a
time. The win is 1.04–1.16×, and it shrinks as the target set and the reuse
distance shrink: w1 (128 KiB) 1.159 → w7 (16 KiB) 1.087 → w10 (8 KiB) 1.041 →
w3 (4 KiB) 1.039.

The two probes w9/w10 were added specifically to find the floor of regime B, and
they put it at **1.04**, not at 1.00 — see §4 for why the floor is where it is.

## 3. Cycle-level decomposition

`dA_idx` = A(full) − A(noidx): the index stream's marginal cost inside the SIMT
path. `dC_eng` = C(noidx) − A(noidx): the engine's target-side cost with the index
stream already free (negative = the engine is *faster* even before index
consolidation). `surplus` = A(full) − C(noidx) = dA_idx − dC_eng.

| workload | A full | A noidx | C noidx | dA_idx | dC_eng | surplus | S_f |
|---|---:|---:|---:|---:|---:|---:|---:|
| w4_duplow | 14 897 749 | 14 863 864 | 4 976 052 | 33 885 | **−9 887 812** | 9 921 697 | 2.994 |
| w8_wslarge | 1 436 874 | 1 290 955 | 937 675 | 145 919 | **−353 280** | 499 199 | 1.532 |
| w5_sequential | 361 952 | 262 099 | 319 738 | 99 853 | +57 639 | 42 214 | 1.132 |
| w1_uniform | 261 745 | 195 272 | 225 811 | 66 473 | +30 539 | 35 934 | 1.159 |
| w7_wssmall | 211 443 | 141 723 | 194 504 | 69 720 | +52 781 | 16 939 | 1.087 |
| w6_zipf | 241 199 | 171 326 | 224 668 | 69 873 | +53 342 | 16 531 | 1.074 |
| w9_seqsmall | 208 452 | 141 789 | 195 995 | 66 663 | +54 206 | 12 457 | 1.064 |
| w2_clustered | 232 026 | 161 952 | 223 476 | 70 074 | +61 524 | 8 550 | 1.038 |
| w3_duphigh | 198 261 | 137 699 | 190 877 | 60 562 | +53 178 | 7 384 | 1.039 |
| w10_runresid | 199 455 | 138 589 | 191 601 | 60 866 | +53 012 | 7 854 | 1.041 |

Read this table as the arithmetic of the boundary:

* **The engine's fixed cost is remarkably constant in regime B**: `dC_eng` = +52 781
  to +61 524 across w7, w6, w9, w2, w3, w10, and +30 539 on w1 (where the gather
  still misses enough to help). That is the organizer (65 536 cycles, §6 of the
  ablation report), address generation, descriptor traffic and buffer turnover,
  minus whatever the merge saves.
* **The prize is the index stream**, and it is roughly constant too: 60 562 to
  99 853 cycles for a 512 KiB, 4 B-strided index array read four lanes at a time.
* **The surplus is the difference of two nearly constant quantities**, which is why
  regime B clusters at 1.04–1.16 and why the corner is fragile: w3 clears its
  engine cost by only 7 384 cycles (3.7 % of the runtime).
* **In regime A the second term changes sign.** On w4 the engine is 9.89 M cycles
  faster than SIMT *with the index stream already removed* — 3× — purely from
  target ordering. Index consolidation contributes 33 885 cycles there, i.e.
  0.3 % of the win.

So "does Bulk help?" is not one question. In regime A it is a memory-ordering
question. In regime B it is a request-count question about a 512 KiB array, and
the answer is "yes, by 4–16 %, with a 4 % margin over the engine's own cost."

## 4. Where a regression comes from

`S_f < 1` requires `dC_eng > dA_idx`. Both sides are measured, so the condition
can be checked rather than speculated:

| quantity | measured range on this matrix |
|---|---|
| `dA_idx` (dense 4 B index stream, 131 072 elements, 4 lanes) | 33 885 – 145 919 (60 562 – 70 074 in regime B) |
| `dC_eng` (engine fixed cost, regime B) | +30 539 – +61 524 |
| smallest measured margin | **7 384 cycles** (w3), 3.7 % of `C(noidx)` |

The matrix therefore contains no faithful regression: the worst case is 1.038. Two
things put that corner within reach, and both are measurable before running:

1. **A cheaper index stream.** Halving the index element size (2 B indices, a
   256 KiB array) roughly halves `dA_idx` to ~30–35 k, which is *below* the
   engine's ~53 k fixed cost in regime B — that would be a regression of about
   0.90–0.95 on w3/w10. This is arithmetic on measured components, **not a
   measurement**: it was not run, and it would require an index-array format the
   engine does not currently have, so it is not proposed as a configuration.
2. **SIMT that is wider than 4 lanes.** M20 measured this directly on qc2534:
   1.047 at 4 lanes → 0.942 / 0.930 / 0.930 at 8 / 16 / 32 lanes, because wider
   warps cut `dA_idx` (248 407 → 240 027) while `dC_eng` did not fall. That is a
   *faithful, measured* Bulk regression, and it is the cleanest confirmation that
   the boundary condition `dC_eng > dA_idx` is the right one.

Under the conservative same-trace pairing (`S_same`) the regression region is
already visible inside this matrix: w2 0.873, w6 0.883, w1 0.928, w5 0.984, w10
0.997. Those are the regime-B workloads, exactly as the condition predicts.

## 5. Window sensitivity

| workload | 8K | 16K | Δ |
|---|---:|---:|---:|
| w4_duplow | 2.994 | 5.655 | +2.66 |
| w8_wslarge | 1.532 | 1.690 | +0.16 |
| w1_uniform | 1.159 | 1.164 | +0.005 |
| w5_sequential | 1.132 | 1.138 | +0.006 |
| w7_wssmall | 1.087 | 1.097 | +0.010 |
| w6_zipf | 1.074 | 1.081 | +0.007 |
| w2_clustered | 1.038 | 1.048 | +0.010 |
| w3_duphigh | 1.039 | 1.039 | 0.000 |

The window is a *regime A* instrument: doubling it doubles the length of the
ordered run, which is worth +2.66 on w4 and +0.16 on w8, and nothing at all on the
one-page case. It is not a regime-B instrument. This is the same conclusion M20
reached from the other direction (2×16K was never better than 2×8K at wide
lanes): the window buys ordering length, and ordering length only matters when
there is disorder to absorb. Both windows are reported for every workload and
neither changes a verdict.

## 6. Concurrency is not where the win comes from

| workload | A peak outstanding | C peak outstanding | A row-hit | C row-hit | A walks | C walks |
|---|---:|---:|---:|---:|---:|---:|
| w4_duplow | 128 | 128 | 0.1 % | 45.5 % | 99 317 | 32 740 |
| w8_wslarge | 128 | 128 | 6.2 % | 67.5 % | 2 302 | 1 613 |
| w1_uniform | 128 | 128 | 76.3 % | 84.1 % | 425 | 426 |
| w3_duphigh | 128 | 128 | 86.8 % | 90.7 % | 394 | 395 |

Both arms saturate the same 128-slot residency budget in every workload, and the
engine spends much of its time waiting for slots (`bulk_slot_stall` 239 121 on w4,
841 902 on w8). Bulk does not win by having more requests in flight; it wins by
making the *same* number of in-flight requests count for more — row-hit 0.1 % →
45.5 % and walks 99 317 → 32 740 at identical occupancy. Where it cannot improve
the ordering (w3, one page), it does not win, at the same occupancy.

## 7. Bytes and transactions

Constant across the matrix by construction, which is what makes the comparison
clean:

| stream | bytes | requests (4 lanes) | note |
|---|---:|---:|---|
| index | 512 KiB | 32 768 | identical in every workload |
| value | 1 MiB | 32 768 | dense, identical |
| row pointer | 64 KiB | 8 704 | two 4 B reads per row |
| target | 1 MiB touched | 32 768 – 131 072 | varies 4× with the distribution |

The engine's descriptor and index traffic is charged in the C arms only; the
`noidx` pairing moves the index stream into the engine rather than deleting it.
