# M18 — Workload characterization report

**Question:** which measurable property of a real sparse workload predicts whether the Bulk
hybrid helps?

**Answer:** not the properties M17 used (κ, duplicate concentration, page locality). On real
CSR data the duplicate fraction is essentially constant (0.93–0.94 of accesses land on an
already-requested line in every matrix), and the gather's DRAM row-hit rate is already 70–87 %
in the baseline. What predicts the outcome is **what fraction of the baseline's cycles the
column-index stream costs**, minus **what the engine costs in stall cycles to take it over**.

κ is not used anywhere in this report. It is a property of the synthetic generator; a real
matrix has no κ.

---

## 1. The four workloads, in the terms that matter

| Property | bcsstk25 | qc2534 | lnsp3937 | dubcova3¹ |
|---|---:|---:|---:|---:|
| rows | 15 439 | 2 534 | 3 937 | 45 000 (of 146 689) |
| nonzeros | 252 241 | 463 360 | 25 407 | 1 589 539 |
| row length mean / CV | 16.3 / 0.43 | 182.9 / 0.002 | 6.5 / 0.47 | 35.3 |
| **`x` footprint** | 123 KiB | **20 KiB** | 31 KiB | **1.12 MiB** |
| `x` lines / pages | 1 930 / 31 | 317 / 5 | 493 / 8 | 18 336 / 287 |
| index array lines / pages | 15 766 / 786 | 28 960 / 1 366 | 1 588 / 88 | 99 347 / 4 945 |
| duplicate line fraction (all accesses) | 0.936 | 0.937 | 0.935 | 0.935 |
| gather elements merged by the engine | 98.6 % | 99.5 % | 97.8 % | 93.0 % |
| target requests actually issued | 3 431 | 2 212 | 560 | 111 279 |
| baseline DRAM row-hit rate | 80.0 % | 87.2 % | 74.8 % | 70.2 % |
| hybrid DRAM row-hit rate | 86.8 % | 88.5 % | 87.7 % | 83.8 % |
| baseline bank utilisation | 89.3 % | 89.1 % | 82.3 % | 95.4 % |
| baseline L1 hit rate | 35.1 % | 35.4 % | 35.5 % | 30.9 % |
| baseline dTLB misses | 1 575 | 2 729 | 179 | 82 107 |
| hybrid dTLB misses | 2 068 | 3 715 | 227 | 22 319 |
| **index stream as % of baseline cycles** | 33.1 % | **41.4 %** | 34.4 % | 30.1 % |
| **engine's own added cost** | 17.7 % | **36.9 %** | 15.2 % | 15.0 % |
| **net speedup (faithful)** | 1.182× | **1.047×** | 1.236× | 1.179× |

¹ row prefix; `x` is the full 146 689-column vector, which is what exceeds L2.

Two definitions used throughout, both measured directly from the paired cells:

```
index stream %    = (A_full − A_noidx) / A_full     the colidx instruction stream's cost
engine added cost = (C_noidx − A_noidx) / A_full    what the engine costs vs free addresses
speedup           = 1 / (1 − (index % − engine %))
```

Both terms use the baseline's cycles as the denominator, so their difference is the fraction
of the baseline the hybrid removes. Check: bcsstk25 33.1 % − 17.7 % = 15.4 % → 1/(1−0.154) =
1.182×; qc2534 41.4 % − 36.9 % = 4.5 % → 1.047×; lnsp3937 34.4 % − 15.2 % = 19.1 % → 1.236×;
dubcova3 30.1 % − 15.0 % = 15.1 % → 1.179×.

---

## 2. Correlations with the candidate predictors

| Candidate predictor | Range across the four | Correlation with speedup | Verdict |
|---|---|---|---|
| unique target lines (`x` footprint) | 317 – 18 336 | none (1.182× at 1 930 lines, 1.179× at 18 336, 1.047× at 317) | **not** predictive |
| duplicate concentration | 0.930 – 0.995 | none (0.995 → worst result) | **not** predictive |
| page locality of the gather | 5 – 287 pages | none | **not** predictive |
| baseline row-hit rate | 70.2 – 87.2 % | none (worst predictor direction) | **not** predictive |
| baseline bank utilisation | 82.3 – 95.4 % | weak negative (most headroom, 1.236×) | secondary |
| **index-stream share of baseline cycles** | 30.1 – 41.4 % | **positive** | **necessary, not sufficient** |
| **engine added cost (slot/fill stalls)** | 15.0 – 36.9 % | **strongly negative** | **the limiting term** |

The two terms trade off, and the net is their difference. That is why `qc2534` — the matrix
with the *largest* index-stream share (41.4 %) — has almost the *smallest* gain (1.047×): its
engine cost is 36.9 %, because 463 360 gathers across 57 windows contend for the shared
R = 128 slot pool and stall (`bulk_slot_stall` 348 704 cycles, `bulk_idx_wait_stall` 140 615).

### 2.1 Why the usual locality metrics fail here

A synthetic W1 trace draws from a footprint-sized line set, so its *page set* is the whole
address range and its duplicate structure is tunable. A real CSR matrix has a fixed structure
in which:

* the gather is a many-to-few map (`nnz` references onto `m` elements), so duplicate
  concentration is high and essentially identical everywhere;
* the DRAM traffic is dominated by the *sequential* `value` and `colidx` streams
  (12 B per nonzero against 4–8 B per gather), which the engine never touches;
* therefore the gather's page locality is not what determines the runtime.

Measured consequence: the hybrid raises the row-hit rate by 6.8 pp (bcsstk25), 1.3 pp
(qc2534), 12.9 pp (lnsp3937) and 13.6 pp (dubcova3) — but the *net* effect on cycles is set by
the index-stream offload and the engine's stalls, not by the row-hit gain. The ablation report
shows the same thing from the other side: forcing the ordering policy off changes nothing.

---

## 3. Index acquisition versus target access

The brief asks how much time goes to acquiring indices versus accessing target data. Measured
as phase boundaries inside the hybrid (`first index request`, `first target issue`,
`bulk_last_done_cycle`):

| Matrix | index phase (to first target) | % of hybrid cycles | target phase (first issue → last done) | % | organiser cycles | fan-out cycles |
|---|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 6 581 | 1.7 % | 269 679 | 69.1 % | 126 121 | 63 327 |
| qc2534 | 6 499 | 0.9 % | 477 776 | 65.5 % | 231 680 | 115 928 |
| lnsp3937 | 5 982 | 13.0 % | 19 721 | 42.8 % | 12 704 | 6 404 |
| dubcova3¹ | 15 362 | 0.5 % | 2 972 759 | 99.5 % | 794 770 | 423 659 |

Index acquisition is **not** the bottleneck: it is 0.5–13 % of the hybrid's runtime, and its
absolute size (6–15 k cycles) is a fixed pipeline depth, not a function of the matrix. The
target stream dominates. The engine's cost is therefore in *delivering targets*, not in
reading its descriptor.

Note the interaction with the ablation: the target stream is where the engine spends 65–99 %
of its time, and yet reordering that stream by page changes cycles by ≤ 0.5 %. Those two
statements are consistent only if the target stream is *bandwidth*-bound rather than
*order*-bound — which the 89–96 % bank utilisation confirms.

---

## 4. The predictor, stated

From these four workloads, the sign and rough size of the effect is predicted by:

```
benefit  ≈  (fraction of baseline cycles spent on the index stream)
          − (fraction of hybrid cycles the engine loses to slot/fill stalls)
```

with the second term growing with `windows × (distinct target lines)` — i.e. with the number
of times the engine must refill and re-request a target set.

* **Helps** when the index stream is a third of the baseline and the target set is small
  enough to dedup almost entirely (lnsp3937: 1.24×).
* **Helps modestly** at larger scale if the engine's stall share stays near 15 %
  (bcsstk25 1.18×, dubcova3 1.18×).
* **Barely helps** when the engine's stall share approaches the index-stream share
  (qc2534: 1.05×).

### 4.1 Workload classes where conventional SIMT is already sufficient

Yes, and they are the majority:

1. **Near-dense sparse matrices** (density ≥ 5 %, rows of uniform length). `qc2534` is the
   example: 183 nonzeros in every row over 2 534 columns. The gather is trivially local, the
   index stream is the only movable cost, and moving it buys < 5 %.
2. **Matrices whose `x` fits in L1.** For `qc2534` and `lnsp3937`, `x` is 20–31 KiB and 82–98 %
   of gathers merge onto lines already resident. There is nothing for a gather accelerator to
   do that the L1 is not already doing.
3. **Any workload where the irregular stream is a minority of DRAM traffic.** In all four
   matrices the sequential `value`/`colidx` streams are 3–4× the gather's element count. An
   accelerator that touches only the gather cannot move the bandwidth bottleneck.

### 4.2 What the engine is actually buying

Not locality, and not gather latency: **issue-slot relief**. In every matrix the hybrid
removes 30–41 % of the baseline's cycles' worth of index-instruction work from a 4-lane SIMT
front-end and performs it out of band at line granularity. That is a real property of this
architecture, but it is a property of the *narrow* Vortex calibration, and it is the single
biggest caveat on every number in this milestone (see the combined report §5).

---

## 5. Limitations

1. **n = 4.** No regression, no confidence interval, and no claim that the rule in §4 holds
   outside these four structures. It is a description of four measurements, not a model.
2. **One matrix per class** (banded structural, near-dense, irregular CFD, large PDE prefix).
3. **No write streams**, so the characterization excludes the scatter/accumulate behaviour
   that a real SpMV kernel also has.
4. **The `x` footprint is the only cache-capacity axis varied.** All four matrices have
   `nnz >> m`, which is typical but not universal (tall-skinny matrices would differ).
5. **`kappa` is deliberately absent.** Real matrices have no regularity parameter, and M17's
   κ does not map onto any measured property here.
