# M18 — Combined findings: does the page-aware Bulk engine help real irregular workloads?

**Milestone:** M18, real-world irregular workload validation.
**Scope:** four SuiteSparse matrices (SpMV), two SNAP graphs (BFS), one Vortex-calibrated
simulator, 34 three-arm cells, all gates passed.
**Answer in one paragraph:** on real SpMV the hybrid is **4.7 %–23.6 % faster** than the
Vortex-calibrated baseline when each architecture is charged for the index acquisition it
actually performs, and **17.5 % slower to 18.3 % faster** when both arms are forced onto the
identical instruction stream. The gain that does survive is *not* M17's page-aware scheduling:
it is **index-stream consolidation** — moving the column-index read off a 4-lane SIMT
front-end and performing it out of band at line granularity. Page ordering changes cycles by
≤ 0.5 % and the DRAM row-hit rate by 0.0 pp on these traces, and the 2×16K window is never
better than 2×8K. BFS cannot be evaluated at all: its workload is a chained gather plus a
scatter, which the engine does not have. The ceiling for a one-level engine on BFS is 19.9–26.3
%, before the missing mechanism is counted.

---

## 1. What was asked, and what was done

| Phase | Deliverable | Status |
|---|---|---|
| 0 | simulator compatibility, without distorting applications | done — `m18-workload-selection-report.md §2` |
| 1 | public datasets with measured structure | done — 4 matrices, 4 graphs, §3 of the same report |
| 2 | reproducible workload adapter | done — `tools/build_trace.py`, `M18TRC1` trace path |
| 3 | three primary configurations | done — 28 cells (4 matrices × {full, noidx} × {8K, 16K}) |
| 4 | mechanism ablation | done for 2 matrices; 1 abandoned (§6.4) |
| 5 | workload characterization | done — `m18-workload-characterization-report.md` |
| 6 | statistical and correctness validation | done — §5 |

Deliverables: this file, `m18-workload-selection-report.md`, `m18-spmv-validation-report.md`,
`m18-bfs-validation-report.md`, `m18-mechanism-ablation-report.md`,
`m18-workload-characterization-report.md`, and `m18-results.svg`.

Nothing in the M5–M17 paths changed. The historical anchors were re-run after the M18 edits
and reproduce exactly (§5.4).

---

## 2. The result, in one table

`S faithful` = baseline(SIMT, full trace) / hybrid(index charged once, noidx trace).
`S same-trace` = both arms on the identical trace, which charges the hybrid's index stream
twice.

| Workload | SIMT cycles | 2×8K cycles | S faithful 8K | S same-trace 8K | S faithful 16K | Verdict |
|---|---:|---:|---:|---:|---:|---|
| bcsstk25 (banded, 252 k nnz) | 461 688 | 390 495 | **1.182×** | 0.938× | 1.177× | small win |
| qc2534 (near-dense, 463 k nnz) | 762 503 | 728 062 | **1.047×** | 0.825× | 1.025× | marginal |
| lnsp3937 (irregular CFD, 25 k nnz) | 57 014 | 46 113 | **1.236×** | 1.180× | 1.237× | clear win |
| dubcova3 (PDE prefix, 1.59 M nnz) | 3 522 146 | 2 988 123 | **1.179×** | 0.878× | 1.175× | small win |
| BFS `facebook_combined` | 142 264 | not evaluable | — | — | — | ceiling 19.9 % |
| BFS `email-Enron` | 376 124 | not evaluable | — | — | — | ceiling 26.3 % |

Execution-time reduction where it is positive: 3.6 % (qc2534) to 19.1 % (lnsp3937).

No claim of universality is made: the answer differs by workload, by window size, and — most
importantly — by which arm is charged for the index read.

---

## 3. The ten questions

### 3.1 Does Bulk outperform SIMT on real SpMV workloads?

**Modestly, and conditionally.** 1.047×–1.236× when each architecture performs its own index
acquisition. Identical-trace accounting gives 0.825×–1.183×, because that accounting makes
the hybrid read the column-index array twice (once as its engine index stream, once as a
SIMT-class instruction it would not execute in a real hybrid).

Neither number is a trick; they bracket the answer, and the difference is measured: on
Dubcova3 the duplicated index stream is 99 347 lines and the hybrid's DRAM-read excess over
the baseline is 100 451. The faithful pairing is the one that matches the brief's requirement
to charge index acquisition *consistently*.

It is **not** the M17 result. Against the synthetic W1 workload the same engine gave 1.75× /
2.07× at matched κ. Real SpMV gives a fifth to a tenth of that.

### 3.2 Does it outperform SIMT on real BFS workloads?

**Not answerable with this engine, and no result is fabricated.** BFS issues a chained gather
(`frontier → rowptr[v] → adj → dist[u]`) and a scatter (`dist[u] = d+1`). The engine has one
indirection level and no writes; the work set is data-dependent, so it is not even a
contiguous index window.

What can be said is a bound: removing the `dist` probe stream entirely — impossible for a
real implementation — saves only 19.9 % (`facebook_combined`) and 26.3 % (`email-Enron`) of
cycles. The remaining 74–80 % is adjacency traversal, frontier maintenance and loop work that a
gather engine does not touch. See `m18-bfs-validation-report.md`.

### 3.3 Does the advantage survive 128 L1 MSHRs?

**Yes.** Raising the baseline's MSHRs to 128 makes the baseline 1.3–5.4 % faster (these
traces are not MSHR-limited), and the faithful advantage erodes only slightly:

| | 16 MSHR | 128 MSHR |
|---|---:|---:|
| bcsstk25 | 1.182× | 1.175× |
| qc2534 | 1.047× | 1.020× |
| lnsp3937 | 1.236× | 1.220× |
| dubcova3 | 1.179× | 1.122× |

The ordering of the workloads is unchanged. This is a genuine robustness result: it is not an
artefact of a starved baseline.

### 3.4 Does 2×16K consistently outperform 2×8K?

**No — it is never better, and is usually slightly worse.**

| | 2×8K | 2×16K | Δ |
|---|---:|---:|---:|
| bcsstk25 | 1.182× | 1.177× | −0.4 % |
| qc2534 | 1.047× | 1.025× | −2.1 % |
| lnsp3937 | 1.236× | 1.237× | +0.1 % |
| dubcova3 | 1.179× | 1.175× | −0.3 % |

The additional capacity buys nothing because the gather already dedups 93–99.5 % of its
elements into the 8K window, and the larger window only delays the first target issue
(index phase 6 498 → 10 788 cycles on qc2534). On real SpMV at this scale, **2×16K costs twice
the storage for no measured benefit**, which is the opposite of what a capacity argument would
predict.

### 3.5 Does page-aware scheduling remain the primary contributor?

**No. It stops contributing almost entirely on real data.**

| bcsstk25, 2×8K | cycles | row-hit |
|---|---:|---:|
| default (page-aware, 2 cohorts) | 492 449 | 86.8 % |
| page ordering off | 490 025 | 86.8 % |
| one cohort | 490 025 | 86.8 % |
| sixteen cohorts | 492 449 | 86.8 % |

The knobs are demonstrably live (cohorts≥2 reproduces the default to the cycle; cohorts=1
reproduces the M6 cursor to the cycle), and their entire range spans 0.5 %, with the
page-aware policy *slower* on this matrix. On `qc2534` the two policies are 26 cycles apart.
The row-hit rate — the quantity the mechanism exists to raise — does not move at all.

M17 found this mechanism worth 1.4 % → 93.6 % row hits on synthetic data because there the
bulk-class stream was a random permutation of a footprint-sized line set. A real CSR gather is
a many-to-few map onto a small vector: there is no long random walk for page ordering to fix.

### 3.6 Does the benefit correlate with DRAM row locality?

**No.** The hybrid raises the row-hit rate by 1.3 pp (qc2534), 6.8 pp (bcsstk25), 12.9 pp
(lnsp3937) and 13.6 pp (dubcova3), and the speedups are 1.047×, 1.182×, 1.236×, 1.179× — i.e.
the *smallest* row-hit gain (qc2534) and one of the largest (dubcova3) land within 1 % of each
other, and the ordering that produces the row-hit gains can be disabled with no effect.

The reason is measurable: these traces are already **82–96 % bank-busy**, and the traffic that
dominates DRAM is the sequential `value`/`colidx` stream (12 B per nonzero, 3–4× the gather's
element count), which the engine never reorders. Row locality is real but it is not the lever.

### 3.7 Which workloads do not benefit, and why?

**`qc2534` — near-dense (183 nonzeros in every row over 2 534 columns).** Its index stream is
the *largest* share of baseline cycles (41.4 %), so the offload should help most. It gains
1.047× because the engine's own cost is 36.9 %: 463 360 gathers across 57 windows contend for
the shared R = 128 slot pool (`bulk_slot_stall` 348 704 cycles, `bulk_idx_wait_stall` 140 615)
while the target set is only 317 lines — the engine is all overhead and almost no work.

The general shape (§4 of the characterization report): benefit ≈ index-stream share − engine
stall share. Workloads where the second term approaches the first do not benefit.

Also not benefiting: any workload whose irregular stream is a minority of DRAM traffic, and any
workload whose gather target fits in L1 (20–31 KiB in two of these four matrices).

### 3.8 How much time is spent acquiring indices versus accessing target data?

Measured phase boundaries inside the hybrid:

| Matrix | to first target (index) | % of hybrid | first issue → last done (targets) | % |
|---|---:|---:|---:|---:|
| bcsstk25 | 6 581 | 1.7 % | 269 679 | 69.1 % |
| qc2534 | 6 499 | 0.9 % | 477 776 | 65.5 % |
| lnsp3937 | 5 982 | 13.0 % | 19 721 | 42.8 % |
| dubcova3 | 15 362 | 0.5 % | 2 972 759 | 99.5 % |

Index acquisition is a **fixed pipeline depth** (6–15 k cycles, essentially independent of
matrix size), not a bottleneck. The target stream dominates. Yet reordering that target stream
by page does nothing (§3.5) — because it is bandwidth-bound, not order-bound.

On the baseline side the index stream is very expensive: 30–41 % of baseline cycles, because a
4-lane front-end reads 16 B of `colidx` per instruction.

### 3.9 Are there workload classes where conventional SIMT is already sufficient?

**Yes, and they are the common case.**

1. Near-dense sparse matrices (density ≥ 5 %, uniform row length): 1.02–1.05× at best.
2. Matrices whose gathered vector fits in L1 (≤ 32 KiB): 82–98 % of gathers merge onto
   resident lines; there is nothing left to accelerate.
3. Any kernel where the irregular stream is a minority of DRAM traffic — true for all four
   matrices here, where the sequential streams are 3–4× the gather per element.

For these, the engine's slot contention and window-fill latency cost more than the offload
saves. A conventional SIMT baseline with 128 MSHRs is within 5 % of the best Bulk
configuration on the near-dense matrix.

### 3.10 What limitations prevent the simulator from predicting real GPU performance?

1. **`lanes = 4`** (Vortex NUM_THREADS = 4). This is the largest single caveat: the measured
   advantage is substantially an index-consolidation effect that scales with how badly a warp
   fetches contiguous indices. A 32-lane warp reads 128 B per instruction instead of 16 B and
   would shrink the 30–41 % index-stream term by roughly 8×. **The numbers here must not be
   extrapolated to a wide-warp GPU.**
2. **No writes.** `y[i] +=` (SpMV) and `dist[u] = d+1` (BFS) are outside the model; that is
   1.1–31 % of the access count. No atomics, no read-modify-write, no memory ordering.
3. **Trace-driven, not execution-driven.** The address sequence is the one an offline
   execution of the real algorithm produced; the model cannot discover a different order.
4. **Single core, single socket.** No multi-SM scaling, no cross-socket NUMA, no L2 partitioning.
5. **DRAM is a timing model**, 2 banks and FR-FCFS, with no refresh, no write turnaround, and
   row capacity scaled to the footprint. Absolute cycles are not silicon cycles.
6. **No arithmetic or occupancy model**: the baseline is 4 warps of 4 lanes irrespective of
   what a real kernel's register pressure would allow.
7. **Two graphs and four matrices**, one per structural class. No generality claim.

---

## 4. What this means for the architecture claim

M17's claim was that the Bulk engine's synthetic advantage comes from page-aware scheduling and
concurrent page cohorts. **M18 finds that this mechanism does not transfer to real sparse
workloads**, and that a different, unplanned property does: the engine performs the index
acquisition at line granularity, out of band, removing a 30–41 % instruction-stream cost from a
narrow SIMT front-end.

That is a weaker and more fragile claim than M17's:

* It is worth 5–24 %, not 2×.
* It disappears on near-dense matrices.
* It is an artefact of a 4-lane baseline more than of the engine's design.
* It is not improved by the mechanism the architecture was built around (page ordering), the
  window capacity it was sized by (2×16K), or the target locality it was motivated by.

A defensible summary for the architecture: **the engine's value on real sparse workloads is as
an index-stream offload engine, not as a locality engine.** If that is the intended value, the
design should be optimised for index acquisition and slot-pool behaviour (`bulk_slot_stall` is
the largest stall in every cell), not for page ordering and window depth.

---

## 5. Correctness, integrity and preservation

### 5.1 Gates

All 34 cells report `M9 GATES PASSED`. The gates are the M9 harness's own:
G1 (all-SIMT dispatch reproduces the baseline cycle for cycle), G2 (all-bulk dispatch
reproduces the M8 machine), G3 (mixed dispatch: exhaustive partition, exactly one descriptor,
every bulk element completed exactly once, index lines match the element count, no lost SIMT
request, residency within the shared pool, output matches the image-derived expectation).

### 5.2 Per-cell checks

| Check | Result |
|---|---|
| address mismatches (`bulk_addr_mismatch`) | 0 in all 34 cells |
| dedup accounting `lines_issued + dedup_merged == elems` | exact in all |
| lost SIMT requests `generated − completed` | 0 in all |
| slot residency `peak_total_outstanding ≤ pool` | 128 ≤ 128 in all |
| engine output value hash vs image expectation | equal, 0 mismatched addresses |
| both arms on one image and one trace hash | enforced by the harness |
| BFS all-SIMT equivalence (G1) | C == A to the cycle (142 264, 376 124, 176 395) |

### 5.3 Instrumentation neutrality

`cache_diag=1` was on in every M18 cell; it adds per-requester attribution counters and no
decisions. The M16 finding that `cache_diag` does not perturb cycles was re-confirmed on a
synthetic cell in M17 and is not re-litigated here.

### 5.4 Historical preservation

| Check | Result |
|---|---|
| regression suite `./bin/dx100_tests` | **4 470 checks, 0 failures** |
| M17 anchor, κ = 0.25, 2×8K, seed 1 | A 220 123 / B 138 924 / C 125 581 — **identical** |
| M17 anchor, κ = 0, 2×16K | unchanged path, not re-run (2×8K anchor covers the code path) |
| synthetic generator default (`dataset` empty) | byte-for-byte the previous path |

The M18 additions are two opt-in config keys (`dataset`, `skip_b`) that default to the
historical behaviour, plus the loader itself, which is unreachable unless `dataset` is set.

### 5.5 Determinism

SpMV and BFS traces are deterministic functions of a public dataset, so no seed was varied and
none was manufactured (the brief forbids manufacturing randomness to inflate the sample count).
Each configuration was run once; the A arm is reproduced exactly across cells that share a
trace (e.g. bcsstk25 A = 461 688 in all six of its cells), which is the determinism check.

---

## 6. Threats to validity, stated plainly

1. **The faithful/same-trace bracket.** The answer moves from 0.83× to 1.24× depending on how
   the index read is charged. Both are reported everywhere; the faithful pairing is argued for
   in §3.1 and in `m18-spmv-validation-report.md §1.1`, but a reader who insists on a single
   identical instruction stream for both arms will take the lower number.
2. **The trace is not the program.** Instruction grouping (one instruction per 4 nonzeros per
   stream) is a modelling choice that matches a 4-lane warp issuing three loads per group. A
   different grouping would change absolute cycles and would change the index-offload term.
3. **Row prefix for Dubcova3** (30.7 % of rows) to fit the cycle budget. It preserves the full
   `x`, which is the property that matters for cache pressure, but it is not the whole matrix.
4. **Dubcova3 ablation abandoned.** The `page_sched=0` cell degenerates into a repeated page
   scan (≈5 000 pages × 8 192 entries × 195 windows) and did not complete in ten minutes. It is
   reported as a gap, not as a result.
5. **`soc-Epinions1` excluded.** Its reference descriptor exceeded the address space of the
   calibrated DRAM mapping before the `skip_b` fix; the cell is not used for any number.
6. **One matrix per structural class**; four points cannot establish a predictive rule.

---

## 7. Reproducing this

```bash
cd DX100_MEM
# datasets (SuiteSparse canonical host; snap.stanford.edu for graphs)
curl -sSLo data/bcsstk25.tar.gz  https://www.cise.ufl.edu/research/sparse/MM/HB/bcsstk25.tar.gz
curl -sSLo data/qc2534.tar.gz    https://www.cise.ufl.edu/research/sparse/MM/Bai/qc2534.tar.gz
curl -sSLo data/lnsp3937.tar.gz  https://www.cise.ufl.edu/research/sparse/MM/HB/lnsp3937.tar.gz
curl -sSLo data/facebook_combined.txt.gz https://snap.stanford.edu/data/facebook_combined.txt.gz
curl -sSLo data/email-Enron.txt.gz       https://snap.stanford.edu/data/email-Enron.txt.gz

make -C sim -j4
python3 tools/build_trace.py spmv --mtx data/bcsstk25.mtx --out build/wm/m18_bcsstk25_full.bin
python3 tools/build_trace.py spmv --mtx data/bcsstk25.mtx --variant noidx \
        --out build/wm/m18_bcsstk25_noidx.bin
python3 tools/build_trace.py spmv --mtx ref/vortex/tests/opencl/spmv/Dubcova3.mtx \
        --max-rows 45000 --out build/wm/m18_dubcova3_full.bin
python3 tools/build_trace.py bfs --edges data/facebook_combined.txt.gz \
        --out build/wm/m18_bfs_facebook_combined.bin

bash sim/build/wm/m18_matrix.sh            # 28 cells, ~25 min
python3 tools/m18_extract.py  > build/wm/m18_tables.md
python3 tools/m18_metrics.py  "build/wm/m18_*_w8192.txt"
python3 tools/m18_plot.py                  # docs/m18-results.svg
./sim/bin/dx100_tests                      # 4 470 checks
```

Raw material preserved: `build/wm/m18_*.txt` (one log per cell, full three-arm dump),
`build/wm/m18_*.bin` + `.json` (traces and their integrity records),
`build/wm/m18_matrix.log`, `build/wm/m18_tables.md`.

---

## 8. Figures

`docs/m18-results.svg` — four panels: SpMV speedup by configuration and matrix; the gather-side
DRAM row-hit rate (the mechanism page ordering targets); the decomposition of the gain into
index-stream offload minus the engine's own cost; and the BFS ceiling.

---

## 9. Bottom line

* **Positive but small**: 4.7–23.6 % on real SpMV, robust to a 128-MSHR baseline.
* **Not the M17 mechanism**: page-aware scheduling is worth ≤ 0.5 % here, and 2×16K is never
  better than 2×8K.
* **Not general**: near-dense matrices gain ~5 %, and the workloads where it fails are
  identifiable in advance (index share < engine stall share).
* **Not evaluable for BFS**, and the ceiling for an engine of this shape (one level, no
  writes) is a fifth to a quarter of BFS's runtime.
* **Fragile to the front-end width**: the surviving gain is index consolidation, which a
  32-lane warp would largely absorb. That is the first thing to measure next.
