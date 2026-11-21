# M21 — Combined Findings

**Question.** Which workload characteristics make the current Bulk engine useful,
and which make it ineffective?

**Answer.** Two measurable characteristics, and only two, decide it: **whether the
target set exceeds L2**, and **how much address-order entropy the gather has**. A
third quantity — the size of the index stream relative to the engine's fixed cost
— decides whether the win survives at all, and it is the one that can turn a win
into a regression.

Duplicate rate, the characteristic this study was originally framed around, is
**not** predictive and is anti-correlated with the measured result.

---

## 1. The predictor table

All thresholds are stated in quantities computable from the index vector alone,
before simulation (`tools/m21_reuse.py`).

| outcome | measurable condition | measured on | S_f |
|---|---|---|---|
| **Bulk win (large, 1.5–3.0×)** | target WS > 1 MiB **and** reuse-within-8K < 0.25 | w4 (8 MiB, 0.00), w8 (2 MiB, 0.21) | 2.994, 1.532 |
| **Bulk win (moderate, 1.06–1.16×)** | target WS ≤ 1 MiB **and** reuse ≥ 0.85 | w1, w5, w7, w6, w9 | 1.159, 1.132, 1.087, 1.074, 1.064 |
| **Parity (1.00–1.05×)** | target WS ≤ 16 KiB **or** duplicate rate ≥ 0.999 with a resident set | w3, w10, w2 | 1.039, 1.041, 1.038 |
| **Bulk regression (< 1.00×)** | engine fixed cost exceeds the index-stream prize: `dC_eng > dA_idx` | not reached in M21 (min margin 7 384 cycles); **reached in M20** at ≥ 8 lanes (qc2534 0.93) and in M21 under the double-charged pairing (w2 0.873, w6 0.883, w1 0.928) | 0.87–0.99 |

The boundaries are sharp in the data: reuse 0.21 → 1.53×, reuse 0.87 → 1.13×,
reuse ≥ 0.93 → ≤ 1.16× and falling; the working-set threshold at 1 MiB (L2) is
where the target stream stops touching DRAM at all.

## 2. The mechanism, stated as three separable benefits and four costs

Separation is measured, not assumed — each term is a comparison of the four
arms (A and C, each on `full` and `noidx` traces).

**Benefits**

1. **Index acquisition consolidation.** The SIMT front end reads the 512 KiB index
   array 16 B per instruction (4 lanes × 4 B) and forms 32 768 requests; the engine
   streams it as 8 192 dense lines. Worth **33 885–145 919 cycles** in this matrix
   (60 562–70 074 in the cache-resident regime). It is a request-count effect and
   is present in every workload.
2. **Duplicate merging.** Reduces target requests by up to 1 995:1 (w3). Worth
   **almost nothing** when the target set is cache-resident (w3 obtains its entire
   1.039 from benefit 1) and nothing at all when there are no duplicates (w4).
   The merge is only valuable when the requests it removes would have missed.
3. **Page ordering.** Converts an arbitrary line order into a page-then-line
   order; the measured effects are DRAM row-hit 0.1 % → 45.5 % (w4) and 6.2 % →
   67.5 % (w8), plus DTLB behaviour (143 165 → 62 955 misses, 99 317 → 32 740
   walks on w4). Worth **0.0× on a one-page set (w3), 0.0× on an already-ordered
   stream (w5), 0.035 on a clustered set (w2), and 1.99× on an 8 MiB scattered set
   (w4: 2.994 → 1.001 with `page_order=0`)**. It is the dominant mechanism, and it
   is *not* the one the engine's name suggests.

**Costs** (all measured on the same runs)

4. **Organizer/descriptor tax**: exactly **65 536 cycles** in every workload
   (8 per row × 8 192 rows), independent of the target distribution.
5. **Address generation and fanout**: 32 768 cycles at minimum, 130 877 on w4 —
   proportional to distinct lines per window.
6. **Translation**: the engine's line-ordered walk turns 2 048 pages into 32 740
   walks instead of 99 317, so ordering *pays* for translation here; in the
   cache-resident regime it is neutral (±1 walk).
7. **Buffering/concurrency**: no benefit — both arms hold the same 128 outstanding
   slots and the engine spends much of its run slot-starved (`bulk_slot_stall`
   239 121 on w4). Bulk converts the same concurrency into more useful requests.

Because 4–5 are nearly constant and 1 is nearly constant, the cache-resident
regime lands at a nearly constant 1.04–1.16, and the margin over the engine's own
cost is as small as 3.7 % (w3).

## 3. What this means for the earlier results

* **M18/M20's real matrices** are regime B: bcsstk25 (x = 123 KiB), qc2534
  (20 KiB), lnsp3937 (31 KiB), and even the dubcova3 prefix (1.17 MB, just over
  L2). Their 1.05–1.24 wins are index-consolidation wins, which is exactly why M20
  found the advantage evaporating as SIMT widened — wider warps attacked benefit
  1 and left benefit 3 untouched, because those matrices had almost no disorder to
  order.
* **M21 explains why M20's qc2534 inverted** and why it was the one that did: it
  has the largest index array (1.85 MB) and the largest measured index share
  (41 %), so it was the most exposed to benefit 1 disappearing — and its `x` was
  still L2-resident, so benefit 3 could not compensate.
* **The M18 claim "Bulk accelerates the irregular gather"** is correct only in
  regime A. In regime B the engine accelerates the *index array*, and the gather
  contributes nothing.

## 4. Characteristics that predict each outcome (final form)

**Predicts a Bulk win**: `target_working_set_bytes > 1 MiB` and the fraction of
target accesses recurring within the window < 0.25. The larger the footprint and
the lower the reuse, the bigger the win — up to the 2.99× measured at 8 MiB.

**Predicts parity**: target set ≤ 16 KiB (fits L1), or ≥ L2-resident with a
duplicate rate ≥ 0.999. The engine's fixed 65 536-cycle organizer tax plus address
generation is then almost exactly what the index stream is worth.

**Predicts a regression**: `dC_eng > dA_idx`, i.e. the index stream is cheap
relative to the engine's fixed cost. Measured instances: M20's qc2534 at ≥ 8 lanes
(0.93) and, in M21, every regime-B workload under the conservative double-charged
pairing (0.87–0.99). M21's own faithful pairing never regresses; its floor is
1.038 with a 7 384-cycle margin, which is thin enough that shrinking the index
stream (2 B indices ⇒ `dA_idx` ≈ 30–35 k against a ~53 k engine cost) would flip
w3 and w10 to ~0.90–0.95. That extrapolation is arithmetic on measured
components, is **not** a measurement, and is not a configuration the current
engine supports.

## 5. Ablations (details in `m21-mechanism-ablation-report.md`)

| knob | effect on w4 (2.994) | effect on w2 (1.038) | effect on w5 (1.132) | effect on w3 (1.039) |
|---|---|---|---|---|
| `page_order=0` | **1.001** | 1.003 | 1.131 | 1.038 |
| `page_cohorts=1` | 1.833 | 1.038 | 1.136 | 1.039 |
| `page_cohorts=2` (default, named) | 2.994 (= control) | 1.038 | 1.132 | 1.039 |
| `page_cohorts=4` | 3.023 | 1.035 | 1.126 | 1.039 |

Every knob is verified live: naming the default cohort count reproduces the
control *exactly*, cohort switches are monotone in the cohort count (16 → 25 267 →
54 668 on w4), and disabling page ordering moves counters by 3×. The null results
on w3 and w5 are predicted by structure (one page; already ordered), not dead
knobs.

## 6. Limitations

1. **Synthetic, single-level, read-only.** These are controlled indirect-access
   patterns with a dense 4 B index array and 8 B elements, not applications. No
   claim of general GPU application speedup follows from them; in particular the
   2.99× on w4 is a property of an 8 MiB randomly-ordered target set, which real
   applications reach only through explicit index permutation.
2. **Four lanes only.** M20 established that wider widths are formation-only
   models, so the whole matrix stays at the calibrated baseline. The M20 evidence
   that widening erodes the win is cited but not re-measured here.
3. **BFS is excluded** — chained indirection and a write path are both absent, so
   a Bulk BFS number would not be a Bulk comparison.
4. **One generator, one row shape.** 16 elements per row and a 4 B index type are
   held fixed; both affect `dA_idx` and therefore the parity corner. The row-shape
   sensitivity is bounded by M20's dubcova3 (36 nnz/row) and bcsstk25 (16.3), which
   showed the same qualitative behaviour.
5. **Determinism, not statistical significance.** The model has no randomness off
   the traced path; the three seeds vary the *workload*, and they agree within
   0.008 in S_f. There is no run-to-run noise to average out.
6. **The regression corner is extrapolated, not measured** (§4). The only measured
   faithful regression in this line of work is M20's qc2534 at ≥ 8 lanes.

## 7. Deliverables

| file | contents |
|---|---|
| `m21-workload-matrix-report.md` | the ten workloads, generation parameters, hashes, seeds, controls |
| `m21-locality-and-reuse-report.md` | unique lines/pages, duplicate rate, working set, reuse-distance histograms, and the demonstration that duplicate rate does not predict |
| `m21-mechanism-ablation-report.md` | page-order and cohort ablations on four structurally chosen workloads, with knob liveness |
| `m21-performance-boundaries.md` | the S_f surface, cycle-level decomposition, the two regimes, the regression condition, window sensitivity |
| `m21-combined-findings.md` | this file: predictor table, benefits and costs, implications, limitations |
| `tools/build_trace.py synth` | the generator (seeded, parameters and hashes recorded per trace) |
| `tools/m21_reuse.py` | offline structure and reuse-distance analysis |
| `tools/m21_extract.py` | `structure`/`boundary`/`memory`/`mechanism`/`ablation` tables |
| `build/wm/m21_matrix.sh` | 64 primary cells + 40 ablation cells |
