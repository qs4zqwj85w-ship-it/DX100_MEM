# M18 — BFS validation report

**Workload:** level-synchronous BFS over SNAP graphs, CSR adjacency.
**Outcome in one sentence:** the current Bulk engine **cannot represent** BFS's access
pattern, so no Bulk comparison is reported; instead this report measures the real BFS
baseline, quantifies the ceiling any single-level accelerator could reach on it (19.9–26.3 %),
and states exactly which mechanism is missing.

This is a limitation result, deliberately produced rather than a distorted measurement. The
milestone brief requires it: *"If the existing simulator cannot faithfully represent an
application, document the limitation and choose a more suitable workload rather than
distorting the application."*

---

## 1. The access pattern BFS issues

Per BFS level, for each frontier vertex `v`:

| # | Stream | Address | Indirection |
|---|---|---|---|
| 1 | `frontier[k]` | sequential over the frontier list | 0 |
| 2 | `rowptr[v]`, `rowptr[v+1]` | `rowptr_base + v*4` | **1** (index = a value read in stream 1) |
| 3 | `adj[b..e]` | `adj_base + j*4` | 2 (bounds came from stream 2) |
| 4 | `dist[u]` for each neighbour `u` | `dist_base + u*8` | **2** (index = a value read in stream 3) |
| 5 | `dist[u] = d+1` | same address as 4 | **write** |

A real BFS also maintains the next frontier (a compacted list), which is another write.

### 1.1 Why the engine cannot do it

The engine forms a target address as `data_base + index[j] * data_bytes`, where `index[j]`
is read from a **contiguous** index array. Two properties of BFS defeat that:

1. **Chained gathers.** Stream 4's index lives in stream 3's data. The engine has one level:
   it cannot feed a filled line back into its index reader. The `dist` probe is the dominant
   irregular access in BFS and it is exactly the one the engine cannot key on.
2. **Data-dependent work selection.** Which `adj` entries to read at all is decided by the
   previous level's `dist` values. The work set is not a contiguous window of the index
   array; it is a scattered set of per-vertex ranges. One descriptor describes one contiguous
   range, so even the adjacency gather would need one descriptor per frontier vertex.
3. **Writes.** `dist[u] = d+1` and the next-frontier append are stores. The model has no
   store path at all (`Access` carries no direction; `Req` has no write field).

Making any of these work would be an architecture change (a chained-gather mechanism, a
scatter/atomic path, or per-vertex descriptors), which the milestone explicitly forbids.
Pre-materialising `dist` addresses into an index array would be the "pre-materialized target
addresses" the brief forbids, and would also hide the very cost that dominates BFS.

### 1.2 What was therefore measured

The full BFS trace **as SIMT work**, on the real graphs, plus one decomposition run in which
the `dist` probe stream is removed. That run is not a configuration — no implementation can
compute a BFS without probing `dist` — it is the ceiling: it says how much of the runtime the
only stream a single-level engine could even have addressed accounts for.

---

## 2. Datasets and structure

| Graph | V | E (undirected) | mean deg | max deg | levels | reached | probes | duplicate probes | trace accesses |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `facebook_combined` | 4 039 | 88 234 | 43.7 | 1 045 | 6 | 4 039 (100 %) | 176 468 | 97.7 % | 365 053 |
| `email-Enron` | 36 692 | 183 831 | 10.0 | 1 383 | 9 | 33 696 (91.8 %) | 361 622 | 90.7 % | 824 332 |
| `wiki-Vote` (extra) | 8 298 | 100 762 | 24.3 | 1 065 | 5 | 7 066 (85.2 %) | 201 472 | 96.5 % | 424 142 |

Source (SNAP, https://snap.stanford.edu/data/):
`facebook_combined.txt.gz`, `email-Enron.txt.gz`, `wiki-Vote.txt.gz`.
Edges are treated as undirected, duplicate edges merged, self-loops dropped. The BFS source
is the highest-degree vertex in each graph (deterministic; SNAP id spaces are not contiguous
and vertex 0 is isolated in `wiki-Vote`).

The two chosen graphs differ in the way the brief asks: `facebook_combined` is dense and
highly clustered (mean degree 43.7, diameter 8, 97.7 % of probes are duplicates — most
neighbours are found immediately), while `email-Enron` is sparse and power-law (mean degree
10.0, 9 levels, 90.7 % duplicates, and the deepest level structure).

---

## 3. Baseline measurement (SIMT, Vortex calibration)

| Graph | Cycles | DRAM reads | Row-hit | L1 hit | L2 hit | dTLB misses | page walks | replays |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `facebook_combined` | 142 264 | 11 652 | 89.2 % | 69.9 % | 18.2 % | 581 | 182 | 192 890 |
| `email-Enron` | 376 124 | 30 050 | 76.8 % | 38.4 % | 81.8 % | 72 614 | 450 | 3 731 147 |
| `wiki-Vote` | 176 395 | 13 942 | 87.0 % | 40.0 % | 85.8 % | 8 721 | 218 | 1 026 194 |

Two things stand out and they matter for the architecture question:

* **The graphs' working sets are small.** 88 000–405 000 edges at 4 B is 0.4–1.6 MiB; the
  whole adjacency of `facebook_combined` fits in the 1 MiB L2. DRAM reads are only
  11 652–30 050, i.e. one order of magnitude below the probe count. BFS on these graphs is
  not a DRAM-bound workload at all — it is a latency/occupancy-bound one.
* **Translation and replay dominate `email-Enron`**: 72 614 dTLB misses, 3.7 M replays. The
  graph's vertex ids are spread over a 36 692 × 4 B = 143 KiB `dist` array, so even a fully
  cached probe still pays address translation and LSU replay.

---

## 4. The ceiling for a single-level accelerator

| Graph | Full BFS | Without the `dist` probe stream | Probe share |
|---|---:|---:|---:|
| `facebook_combined` | 142 264 | 113 931 | 19.9 % |
| `email-Enron` | 376 124 | 277 115 | 26.3 % |

Even a hypothetical engine that could consume the dependent probe stream **for free** could
remove at most 20 % (`facebook_combined`) to 26 % (`email-Enron`) of BFS cycles, because the
other 74–80 % is adjacency traversal, frontier maintenance and loop overhead that a
one-level gather engine does not touch. And that hypothetical engine cannot exist: the probe
index comes from the adjacency values, and the probe's result is written back.

So the answer to "does Bulk outperform SIMT on real BFS?" is not "no, it is slower" — it is
**"not evaluable with this engine, and the ceiling for a mechanism of this shape is a fifth to
a quarter of the runtime, before counting the mechanism it does not have."**

---

## 5. Correctness and integrity

Every BFS run was executed with `op_class=simt`, which activates gate **G1** of the dispatch
harness: the dispatched arm must reproduce the baseline **cycle for cycle** across 30
counters (cycles, requests, slot allocations, completions, completion hash, L1/L2/DRAM
counters, TLB, walks, replays, residency). All runs report `M9 GATES PASSED`, and C's cycle
count equals A's exactly (142 264, 376 124, 176 395).

That is a strong check on the trace path itself: the file-driven trace produces a
bit-identical execution under two different driver paths.

The BFS distance result is computed by the trace builder and recorded per graph
(`app_output_checksum` = sum of distances, 11 428 for `facebook_combined`, 146 222 for
`email-Enron`; unreachable vertices stay at −1 and contribute 0).

Note: `soc-Epinions1` (75 888 V, 405 740 E) was also built and run; it reproduced A = C, but
its reference descriptor exceeded the address space available in the calibrated DRAM mapping
before the `skip_b` fix, so its cell is not used for any number in this report.

---

## 6. What would have to change

Not done here, recorded so the limitation is actionable:

1. **A chained gather** (index ← a filled line of the previous pass). This is the whole BFS
   probe stream.
2. **A store path** in the trace and the hierarchy (`dist[u] = d+1`, frontier append).
3. **Per-vertex descriptors**, or a list-of-ranges descriptor, to address the scattered
   adjacency ranges selected by the frontier.

Any one of them is a new mechanism. Item 1 alone is what turns SpMV's single-level gather
into BFS's two-level traversal, and it is the difference between the 1.05–1.24× measured for
SpMV and the ≤ 26 % ceiling measured here.

---

## 7. Limitations

1. **Not a Bulk measurement.** No speedup number for BFS exists in this milestone, by
   construction.
2. **Trace-driven**: the BFS execution is real (real levels, real frontier evolution, real
   duplicate rate), but the address sequence is recorded offline and replayed; the model does
   not re-run the algorithm.
3. **Writes are absent from every trace**, so both the measured baseline and the ceiling
   understate BFS's true cost by the `dist` store stream.
4. **Single source vertex.** BFS from one source; multi-source and direction-optimised
   (top-down/bottom-up) variants are not modelled.
5. The graphs are small enough to be L2-resident, so these numbers do not represent a
   scale-free web graph (billions of edges) where BFS becomes DRAM-bound. On such a graph the
   ceiling for the probe stream would be larger — but the engine still could not reach it.
