# M18 — Workload selection and simulator compatibility

**Milestone:** M18, real-world irregular workload validation.
**Question:** can a page-aware Bulk memory engine improve the execution of real sparse and
graph workloads compared with a properly provisioned Vortex-calibrated SIMT baseline?

This report is Phase 0 and Phase 1: what the existing simulator can represent, which public
datasets were selected and why, and exactly how each dataset became an executable trace.
No performance claim is made here; the measurements are in the workload reports.

---

## 1. What the simulator is

`DX100_MEM/sim` is a cycle-accurate model of a memory hierarchy driven by an access trace:

| Component | Model | Source |
|---|---|---|
| SIMT front-end | 1 core, 4 warps, 4 lanes, coalescer, per-core L1 | `src/system.cpp` |
| Bulk engine | descriptor fetch, pipelined index acquisition, window buffers, page directory, dedup, page-ascending issue, fan-out | `src/bulk.cpp` |
| Hierarchy | L1 (16 KiB, 4-way, 16 MSHRs), L2 (1 MiB, 8-way, 64 MSHRs) | `src/memory.cpp` |
| Translation | dTLB 16×4, L2 TLB 512×4, 2 walkers, Sv32 (2 levels) | `src/memory.cpp` |
| DRAM | 2 banks, 4096 rows × 1 KiB, t_hit 14, t_miss 26, FR-FCFS | `src/memory.cpp` |
| Residency | one shared slot pool of R = 128 line requests for both clients | `src/sim.hpp` |

The trace is `std::vector<Access>` grouped by instruction:

```cpp
struct Access { u64 addr; u64 line; u32 elem; u32 instr; u32 lane; };
struct Trace  { std::vector<Access> acc; u32 lanes, n_instr; std::vector<u8> instr_class; ... };
```

`instr_class[i]` is the operation-class metadata (SIMT or Bulk) that the dispatcher consumes.
It is decided **before execution** and never changed at run time.

### 1.1 What the model represents, precisely

* **Reads only.** `Access` carries no write flag, `Req` has no write field, and
  `MemorySystem::write_va()` exists only to load a memory image. There is no store path,
  no read-modify-write and no atomic.
* **One indirection level per descriptor.** A descriptor is
  `{index_base, data_base, count, index_bytes, data_bytes}`; the engine reads `count` index
  values from memory and forms `data_base + index[j] * data_bytes`. A target address can
  never depend on the *value* that a previous target returned.
* **One contiguous index window.** The index array is a single buffer `[index_base,
  index_base + count*index_bytes)`. Scattered per-vertex ranges cannot be described.
* **Address space is flat and unbounded**; DRAM row capacity is
  `banks × rows × row_bytes` and rows are scaled to the footprint so distinct addresses do
  not alias onto one row (see §5.4).

### 1.2 What was added in M18

Nothing in the existing paths changed. The generator is still the default:

| Change | File | Effect on historical results |
|---|---|---|
| `dataset=<path>` loads an `M18TRC1` binary trace | `src/workload.cpp`, `src/sim.hpp`, `src/util.cpp` | none: empty by default |
| `skip_b=<0/1>` omits the "bulk only over the whole trace" arm | `src/ab.cpp` | none: default 0 keeps it |
| `tools/build_trace.py`, `tools/dataset_stats.py` | new | offline only |

`build_trace()` dispatches to `build_trace_file()` only when `cfg.dataset` is non-empty, so
every M5–M17 run constructs exactly the trace it constructed before. Verified by running the
M9/M18 regression suite (§6).

---

## 2. Compatibility matrix

Required capabilities from the milestone brief, answered against the code:

| Capability | Representable? | Evidence / limitation |
|---|---|---|
| Multiple indirect streams | **No** | one descriptor per run; `bulk_wl=E0` builds exactly one index array |
| Dependent address generation | **No** (one level) | `data_base + index[j]*elem_bytes`; index values are never taken from returned data |
| Multiple data arrays | **Yes** | flat VA space; each array is a region, streams coexist in one trace |
| Reads | **Yes** | the trace is an address stream |
| Writes | **No** | no write path (`Access` has no direction; `Req` has no store field) |
| Data dependencies | **Partially** | program order is preserved in the trace; the model does not enforce a dependency, so a trace must only contain orders that are legal |
| Variable-length graph adjacency lists | **Trace only** | a CSR row is a contiguous run, so it is a legal trace; the engine cannot *select* per-vertex runs |
| Multiple memory-access phases | **Yes (trace)** / **No (engine)** | phases are instruction ranges; the engine cannot reconfigure per phase |

Consequences for the three candidate workloads:

### 2.1 SpMV — representable

CSR SpMV issues, per row `i`:

1. `rowptr[i]`, `rowptr[i+1]` — sequential scalar reads;
2. `colidx[j]` for the row — a contiguous 4 B stream;
3. `value[j]` — a contiguous 8 B stream;
4. `x[colidx[j]]` — **one** gather, whose index is read from memory (stream 2).

Stream 4 is exactly the engine's shape: an index array in memory and a data base. The
accumulation `y[i] += value[j]*x[colidx[j]]` and the `y[i]` store are arithmetic and a
write, and the model has neither; both arms therefore execute the same read stream and the
`y` store is omitted from both (quantified in §5.3). This is a memory-system comparison, not
an arithmetic comparison, and it is stated as such everywhere.

### 2.2 BFS — not representable

A level-synchronous BFS issues four streams per iteration:

1. `frontier[k]` — the frontier list;
2. `rowptr[frontier[k]]` — a **gather** keyed by a value read in stream 1;
3. `adj[b..e]` — a variable-length run whose bounds came from stream 2;
4. `dist[u]` for each neighbour — a **gather** keyed by a value read in stream 3, and
   `dist[u] = d+1` — a **scatter write**.

Streams 2 and 4 are each one indirection, but they are *chained through memory*: the index
for stream 4 lives in the data of stream 3. The engine has one level and no writes, so no
faithful mapping exists. Pre-materialising `dist` addresses into an index array would be
precisely the "pre-materialized target addresses" the brief forbids, and the frontier is
data-dependent (it is the previous level's output), so even the *selection* of work would
have to be pre-computed by something other than the engine.

Per the brief ("report the limitation rather than inventing an unsupported implementation"),
BFS is **measured as an all-SIMT trace only**, and the comparison is reported as not
evaluable rather than distorted. See `m18-bfs-validation-report.md`.

### 2.3 PageRank — not implemented

Same two obstructions as BFS (a chained gather plus a scatter-add accumulation into
`rank[]`), and the brief makes it optional and explicitly subordinate. Not attempted.

---

## 3. Datasets

All datasets are public, versioned and fetched from their canonical distributions. Structural
statistics below were computed from the files themselves (`tools/dataset_stats.py`,
`tools/build_trace.py`), not taken from a paper table.

### 3.1 Selection criteria

Three matrices were required with *different* structure, chosen on measured statistics: row
length distribution, column reuse, and page span per row.

| Role | Matrix | Why it is that role |
|---|---|---|
| regular | **HB/bcsstk25** | banded structural stiffness matrix; row length 2–59, CV 0.43; 1.3 pages per row |
| repeated references | **Bai/qc2534** | 183 nonzeros in every row over only 2 534 columns: 99.45 % of column references repeat |
| irregular / distributed | **HB/lnsp3937** | CV 0.47, row lengths 1–11, 84.5 % repeat; a CFD operator with scattered columns |
| large + x exceeds L2 | **UTEP/Dubcova3** (row prefix) | 146 689-column `x` (1.12 MiB) is larger than the 1 MiB L2, the only case where the gather target stream itself cannot be cached |

`qc2534` is deliberately the *worst* case for the accelerator's premise (a nearly dense
matrix with a 20 KiB `x`): it is included because the brief asks for a repeated-reference
matrix, not because it flatters Bulk.

### 3.2 Structural statistics

| Matrix | dims | nnz stored | nnz expanded | row len mean / sd / CV | row len p50 / p99 / max | unique cols | repeat frac | pages / row | working set |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|
| HB/bcsstk25 | 15 439² | 133 840 | 252 241 | 16.34 / 7.08 / 0.433 | 17 / 37 / 59 | 15 439 (100 %) | 0.9388 | 1.34 | 3.34 MiB |
| Bai/qc2534 | 2 534² | 232 947 | 463 360 | 182.86 / 0.35 / 0.002 | 183 / 183 / 183 | 2 534 (100 %) | 0.9945 | 1.29 | 5.61 MiB |
| HB/lnsp3937 | 3 937² | 25 407 | 25 407 | 6.45 / 3.06 / 0.473 | 6 / 11 / 11 | 3 937 (100 %) | 0.8450 | 1.10 | 0.38 MiB |
| UTEP/Dubcova3 (prefix) | 45 000 × 146 689 | — | 1 589 539 | 35.3 | — | 146 689 | — | — | ≈24 MiB |

Symmetric matrices (bcsstk25, qc2534, Dubcova3) are stored as one triangle; both triangles are
needed by `y = Ax`, so they are expanded before the trace is built. `nnz expanded` is the
number of scalar products the kernel performs.

Working set = `rowptr + colidx + values + x + y`. Note `x` alone is 123 KiB (bcsstk25),
20 KiB (qc2534), 31 KiB (lnsp3937) and 1.12 MiB (Dubcova3).

### 3.3 Citations and download

The SuiteSparse Matrix Collection is distributed at `https://sparse.tamu.edu`; that host
refuses automated fetches (HTTP 403), so the files were taken from the collection's original
canonical host, which serves the identical archives:

```
https://www.cise.ufl.edu/research/sparse/MM/HB/bcsstk25.tar.gz
https://www.cise.ufl.edu/research/sparse/MM/Bai/qc2534.tar.gz
https://www.cise.ufl.edu/research/sparse/MM/HB/lnsp3937.tar.gz
```

`UTEP/Dubcova3` is the copy already carried by this repository at
`ref/vortex/tests/opencl/spmv/Dubcova3.mtx` (Vortex's SpMV test input); its header records
`UF Sparse Matrix Collection, id 1849, date 2007`.

* T. A. Davis and Y. Hu, *The University of Florida Sparse Matrix Collection*, ACM TOMS 38(1), 2011.
* HB/bcsstk25 — Boeing/Harwell-Boeing collection, id 47, 1984, structural problem.
* Bai/qc2534 — Bai collection, id 326, 1991, electromagnetics problem.
* HB/lnsp3937 — Harwell-Boeing collection, id 191, 1982, computational fluid dynamics.
* UTEP/Dubcova3 — id 1849, 2007, 2D/3D problem.

Graphs (BFS), from the Stanford Network Analysis Project:

```
https://snap.stanford.edu/data/facebook_combined.txt.gz
https://snap.stanford.edu/data/email-Enron.txt.gz
https://snap.stanford.edu/data/wiki-Vote.txt.gz
https://snap.stanford.edu/data/soc-Epinions1.txt.gz
```

* J. Leskovec and A. Krevl, *SNAP Datasets*, Stanford, 2014.
* `facebook_combined`: 4 039 vertices, 88 234 undirected edges, mean degree 43.7, diameter 8.
* `email-Enron`: 36 692 vertices, 183 831 undirected edges, mean degree 10.0, 9 BFS levels.
* `wiki-Vote`: 8 298 vertices, 100 762 undirected edges, mean degree 24.3.
* `soc-Epinions1`: 75 888 vertices, 405 740 undirected edges, mean degree 10.7.

Graph files are treated as undirected edge lists with duplicate edges merged and self-loops
dropped; the BFS source is the highest-degree vertex (deterministic — SNAP id spaces are not
guaranteed contiguous, and vertex 0 is isolated in `wiki-Vote`).

---

## 4. Trace construction

### 4.1 Address layout (SpMV)

| Region | Base | Size |
|---|---|---|
| `x` (doubles) | 0 | `m × 8` |
| `rowptr` (int32) | aligned after `x` | `(m+1) × 4` |
| `colidx` (int32) | `max(aligned, nnz×8 + 64 KiB)` | `nnz × 4` |
| `value` (double) | aligned after `colidx` | `nnz × 8` |
| engine index array | **= `colidx` base** | `nnz × 4` |
| descriptor line | `colidx_base/64 − 1` | 64 B |

`x` is placed at **address 0** for a reason: with `data_base = 0` and `elem_bytes = 8` the
engine's index value for element `j` is `x_addr/8 = colidx[j]`. The index array the engine
reads is therefore **byte-identical to the CSR column-index array** — the engine acquires the
real index structure of the matrix, and nothing is pre-materialised on its behalf. This is
checkable and was checked: the descriptor the harness writes and the matrix's own `colidx`
array are the same bytes.

### 4.2 Instruction grouping

`lanes = 4` (the Vortex calibration). Per row, in program order:

* one instruction covering `rowptr[i]` and `rowptr[i+1]` (SIMT);
* ⌈L/4⌉ instructions covering the row's `colidx` entries (SIMT, 16 B each);
* ⌈L/4⌉ instructions covering the row's `value` entries (SIMT, 32 B each);
* ⌈L/4⌉ instructions covering `x[colidx[j]]` (**Bulk** — the only class the engine receives).

The gather instructions are emitted after the index instructions for the same chunk, so the
dependency "index first, then target" is present in program order. Total instructions ≈ 0.75
per nonzero, which is what a warp issuing three loads per 4 nonzeros does.

### 4.3 The two variants, and why there are two

In the model one trace is shared by all arms, and arm C executes every SIMT-class instruction.
For SpMV the column-index read is performed by *different clients* in the two architectures:
SIMT reads `colidx` in the baseline, and the engine reads the same bytes as its index array
in the hybrid. One trace cannot express both, so two traces are built:

| variant | streams in trace | faithful for | bias |
|---|---|---|---|
| `full` | rowptr, colidx, value, gather | arm A (SIMT reads its indices) | arm C is charged the index stream **twice** |
| `noidx` | rowptr, value, gather | arm C (only the engine reads `colidx`) | arm A is **not** charged the index stream |

Neither is a lie; each is exact for one arm. The faithful comparison is therefore
**A(`full`) vs C(`noidx`)**, reported as "S faithful", with the same-trace pairs
(A/C in `full`) reported alongside as a conservative bound. Both numbers appear in every
table so the reader can see the width of the bracket. The direction of the difference is
measurable: on Dubcova3 the duplicated index stream is 99 347 lines, and C's DRAM read excess
over A is 99 307 — i.e. essentially the whole gap is the artefact, not a mechanism.

### 4.4 What is charged

| Cost | Charged in `full` | Charged in `noidx` |
|---|---|---|
| row-pointer reads | yes (SIMT) | yes (SIMT) |
| column-index reads | yes (SIMT) + engine index stream | engine index stream only |
| value reads | yes (SIMT) | yes (SIMT) |
| address generation for the gather | engine AGU (`ag_lanes`), SIMT coalescer | same |
| descriptor fetch | yes (`desc_fetch=1`) | yes |
| translation, cache fills, DRAM | via the shared hierarchy | same |
| `y[i]` accumulation + store | **not modelled** (no write path) | same |

### 4.5 Trace integrity record

Each trace writes `<trace>.json` with the counts the brief requires: accesses by type,
unique target lines, unique pages, duplicate fraction, instruction and class counts, and a
SHA-256 over the access sequence. Both arms consume the same file, and the harness re-hashes
the trace it loaded (`trace_hash`) and the memory image; the two arms' images are compared.

Measured (SpMV):

| trace | accesses | of which gather | unique lines | unique pages | duplicate line frac |
|---|---:|---:|---:|---:|---:|
| bcsstk25 full | 787 601 | 252 241 | 50 192 | 786 | 0.936 |
| qc2534 full | 1 395 148 | 463 360 | 87 356 | 1 366 | 0.937 |
| lnsp3937 full | 84 095 | 25 407 | 5 504 | 88 | 0.935 |
| dubcova3 (prefix) full | 4 858 617 | 1 589 539 | 316 377 | 4 945 | 0.935 |

The duplicate fraction is high (0.93) for every matrix, and it is *structural*: a 1 M-line
matrix produces ~1 M index references into an `x` of only `m` elements, so line reuse is the
normal case, not a property of the synthetic generator.

---

## 5. Configuration manifest

### 5.1 Fixed environment (identical for every arm and every cell)

Transcribed from `sim/configs/ab_controlled.cfg`, which is itself transcribed from
`VX_config.toml @ 7300bc8` (see `m4-baseline-calibration-report.md §2`):

```
cores=1 warps_per_core=4 lanes=4 elem_bytes=8 R=128 lsu_pending=0 mem_path=l1
l1_lines=256 l1_ways=4 l1_latency=2 l1_mshr=16
l2_lines=16384 l2_ways=8 l2_latency=4 l2_mshr=64
dtlb_entries=16 dtlb_ways=4 dtlb_mshr=4 dtlb_parked=2
l2tlb_entries=512 l2tlb_ways=4 l2tlb_mshr=8 l2tlb_latency=4
walkers=2 walk_cache=16 levels=2 walk_latency_per_level=150
banks=2 row_bytes=1024 t_hit=14 t_miss=26 link=0 dram_q=0 frfcfs=1 bank_hash=1
ag_lanes=8 eng_issue=4 idx_window=0 desc_fetch=1
page_dir=512 org_lanes=4 fanout_lanes=4 batch_deadline=4096 org_overlap=1
dedup=1 page_order=1 page_sched=1 idx_mode=2 pool_slots=128
```

`elem_bytes=8` because the matrices and `x` are double precision; `y` would be too. The
engine's index entries stay 4 B (`bulk_wl=E0`), which is the width of `colidx` itself.

### 5.2 Runner

```
sim/build/wm/m18_run.sh <trace.bin> <window> <tag> [extra --set ...]
```

`window ∈ {8192, 16384}` = the two 8K / two 16K buffer configuration. `rows` is derived from
the trace footprint, `bulk_index_base`/`bulk_desc_line` from the trace layout, and
`bulk_max_elems` from the element count — all read from `<trace>.json`, so no cell can be
run against a mismatched layout.

### 5.3 Omitted work, quantified

The `y[i]` accumulation and store are not modelled: `m` stores of 8 B plus `m` reads of `y`
per pass. Against `nnz` gathers that is `2m/nnz`:
bcsstk25 12.2 %, qc2534 1.1 %, lnsp3937 31.0 %, Dubcova3 5.7 %. lnsp3937 is the worst case;
its result should be read with that in mind. Arithmetic (the multiply-add) is outside the
model for both arms and cancels in the comparison.

### 5.4 DRAM row space

`banks × rows × row_bytes` must cover the footprint or distinct addresses alias onto one row.
Rows are therefore the smallest power of two ≥ footprint/(banks×row_bytes), applied
identically to all arms:

| trace | max address | rows | row space |
|---|---:|---:|---:|
| lnsp3937 | 574 KiB | 4096 | 8 MiB |
| bcsstk25 | 4.9 MiB | 4096 | 8 MiB |
| qc2534 | 8.9 MiB | 8192 | 16 MiB |
| dubcova3 prefix | 39.5 MiB | 32768 | 64 MiB |

Timing parameters (t_hit, t_miss, banks, row_bytes, FR-FCFS) are never changed. This is a
capacity parameter scaled to the workload, not a tuned one.

---

## 6. Correctness and integrity checks run

| Check | Result |
|---|---|
| Existing regression suite (`./bin/dx100_tests`) | see `m18-spmv-validation-report.md §5` |
| M9 gates (G1/G2/G3) on every cell | every cell reports `M9 GATES PASSED` |
| Address mismatches (`bulk_addr_mismatch`) | 0 in every cell |
| Dedup accounting (`lines_issued + dedup_merged == elems`) | exact in every cell |
| Lost SIMT requests (`generated − completed`) | 0 in every cell |
| Slot residency (`peak_total_outstanding ≤ pool`) | held in every cell |
| Trace hash identical across arms | guaranteed by construction (one `Trace` object) |
| All-SIMT equivalence (BFS) | G1: C reproduces A cycle for cycle |

---

## 7. Limitations of this phase

1. **Reads only.** SpMV's `y` store and BFS's `dist` store are not modelled. The comparison
   is over the read stream, which is the majority of the traffic but not all of it.
2. **One indirection.** Any workload whose second stream depends on the first stream's
   *values* cannot be expressed. This excludes BFS frontier expansion and PageRank.
3. **Trace-driven, not execution-driven.** The address sequence is produced by an offline
   execution of the real algorithm over the real dataset, then replayed. The model does not
   simulate control flow, so it cannot discover a different access order than the one
   recorded. Stated in every report.
4. **Fixed program order.** The baseline issues instructions in the recorded order; it does
   not model an out-of-order or multi-issue front-end beyond `warps_per_core=4`.
5. **Single core.** `cores=1` is required by the dispatch experiment.
6. **`x` reuse is geometry, not cache behaviour**: all matrices here have `x` smaller than
   L2 except Dubcova3's prefix, which is why Dubcova3 is included.
7. **The synthetic `W1 N=16384 …` header line is printed for file traces too.** `N`,
   `Lambda`, `P`, `kappa` and `seed` are not consulted when `dataset` is set; the line is
   cosmetic and is called out wherever a raw log is quoted.
