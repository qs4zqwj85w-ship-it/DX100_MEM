# M21 — Workload Matrix Report

**Question.** Which workload characteristics make the Bulk engine useful, and
which make it ineffective?

**Method.** Eight controlled single-level indirect-access workloads plus two
boundary probes, all built from one generator, all with the same logical
operation count and the same instruction stream shape, differing only in the
values of the index vector. Three arms per cell from one M9 run: A =
Vortex-calibrated SIMT (4 lanes), C with Bulk 2×8K, C with Bulk 2×16K. Bulk is
unchanged: no new mechanism, no capacity, no per-workload tuning.

Width: M20 established that only the 4-lane configuration is calibrated; 8/16/32
lanes model instruction formation without a corresponding memory pipe. Per the
M21 brief, the whole study therefore stays at **4 lanes** and every result here is
labelled a four-lane result.

---

## 1. What a workload is

Every workload is `y[i] = x[c[i]]` for i = 0 … 131 071, organised as 8 192 rows of
16 elements. The trace issues, per row: `rowptr` (2 SIMT reads), `colidx` (the
index array, 4 B/elem, SIMT in the `full` variant), `value` (a dense 8 B stream
present in every variant so the value path is charged identically), and
`x_gather` (the indirect access, 8 B/elem, the engine's target stream).

Only `c[]` changes between workloads. The index array is therefore byte-identical
in size, layout and transaction count across the whole matrix — the index stream
is a *constant* here, and every difference in the results is a property of the
target stream. That is deliberate: M20 showed the index stream dominates the
Bulk result when the target set is cache-resident, so the way to find the
boundary is to hold the index stream fixed and move the target distribution.

| parameter | value in every workload |
|---|---|
| logical operations (`ops`) | 131 072 |
| rows / elements per row | 8 192 / 16 |
| index array | 131 072 × 4 B = 512 KiB, 4 B apart, dense |
| value array | 131 072 × 8 B = 1 MiB, dense |
| row pointer array | 8 193 × 4 B |
| element size | 8 B |
| lanes | 4 |
| instruction stream | 106 496 instructions, 32 768 of them `x_gather` |

## 2. The matrix

| id | family | pattern | column space | parameters | what it isolates |
|---|---|---|---:|---|---|
| w1 | uniform random | `uniform` | 16 384 | seed | 128 KiB target set (L2-resident), no structure |
| w2 | clustered | `clustered` | 16 384 | 90 % of accesses in 5 % of the column space (816 of 16 384 columns = 6.4 KiB) | page concentration |
| w3 | high duplicate | `dup_high` | 512 | 512 distinct indices | 64 target lines, extreme reuse |
| w4 | low duplicate | `dup_low` | 1 048 576 | 131 072 distinct lines, one index per line | zero line sharing, 8 MiB footprint |
| w5 | sequential | `sequential` | 131 072 | stride 1 | perfect spatial locality |
| w6 | power-law | `zipf` | 16 384 | α = 1.0 | hot head + long tail |
| w7 | working set ≪ L2 | `uniform` | 2 048 | seed | 16 KiB target set (L1-sized) |
| w8 | working set ≫ L2 | `uniform` | 262 144 | seed | 2 MiB target set |
| w9 | boundary probe | `sequential` | 2 048 | stride 1 | ordered *and* resident: no ordering or merge left to win |
| w10 | boundary probe | `uniform` | 1 024 | seed | 8 KiB, maximally resident |

w1 is also the middle point of the working-set transition (16 KiB → 128 KiB →
2 MiB is w7 → w1 → w8). w9 and w10 were added after the primary matrix, to probe
the parity corner rather than to look for a favourable case.

**BFS is excluded.** The current engine supports one level of indirection and no
write path; a BFS frontier (`adj → dist[u]`) is a chained gather with a write to
`dist`, so a Bulk number on it would not be a Bulk comparison. M18's BFS traces
remain in the tree as a characterisation exercise only and are not used here.

## 3. Generation, determinism and hashes

Generator: `tools/build_trace.py synth` — Python `random.Random(seed)`, no other
source of randomness. The exact generation parameters are written into every
trace's `.json` under `generation` (ops, rows, nnz_per_row, cols, pattern, seed,
cluster_frac, cluster_pages, distinct, stride, zipf_a, elem_bytes, lanes, rng,
sorted_per_row), together with `index_vector_sha256` (the index values alone) and
`trace_sha256` (the full instruction stream).

Three paired seeds per workload (1, 2, 3). Seed dependence of the result:

| workload | S_f seed 1 | seed 2 | seed 3 | spread |
|---|---:|---:|---:|---:|
| w1_uniform | 1.159 | 1.160 | 1.159 | 0.001 |
| w2_clustered | 1.038 | 1.039 | 1.038 | 0.001 |
| w3_duphigh | 1.039 | 1.039 | 1.038 | 0.001 |
| w4_duplow | 2.994 | 2.997 | 2.995 | 0.003 |
| w5_sequential | 1.132 | 1.132 | 1.132 | 0.000 |
| w6_zipf | 1.074 | 1.075 | 1.077 | 0.003 |
| w7_wssmall | 1.087 | 1.079 | 1.079 | 0.008 |
| w8_wslarge | 1.532 | 1.527 | 1.525 | 0.007 |

w5 is seed-invariant by construction (a deterministic stride sequence); its three
runs are therefore a direct determinism check and they agree to the cycle
(361 952 A cycles, 319 738 C cycles in all three).

Trace hashes (first 16 hex digits of the full record in the `.json`):

| workload | seed 1 trace | seed 2 trace | seed 3 trace |
|---|---|---|---|
| w1_uniform | `580af2b6785fccc0` | `3ec2d8cd2713fbcb` | `0744f4d2c33f7e83` |
| w2_clustered | `ddc5c327a3c5cf9a` | `ceb019d1184691af` | `0fab4fbc4458a0f3` |
| w3_duphigh | `ea2d24efc87d453c` | `68bf193702b7b280` | `275d81e6d4763e22` |
| w4_duplow | `fe835fff021c039a` | `710a0955822d49e4` | `6ff42eb1210bb577` |

## 4. Controls

| control | how it is held |
|---|---|
| logical operation count | 131 072 gathers in every workload and every arm; identical instruction stream length (106 496) |
| index acquisition | charged to whichever arm reads it: `full` has SIMT read the index array, `noidx` gives it to the engine alone. Both pairings reported |
| target acquisition | every gather is a real instruction; the engine's target addresses are the trace's own addresses (`bulk_data_base = 0`) |
| translation | the same DTLB/L2 TLB/walker configuration for both arms; walks are counted per arm |
| descriptors | one descriptor read per window/cohort open, from memory, in every C arm |
| address generation | `ag_lanes = 8`, `org_lanes = 4`, `fanout_lanes = 4` in every cell |
| write traffic | zero: every trace is read-only (`write_accesses: 0`) |
| memory mapping | identical VA layout, identical page placement, identical DRAM row/bank hashing within an experiment |
| Bulk parameters | one policy, both windows run on every workload, nothing tuned per workload |
| seeds | three paired seeds, §3 |
| history | no `m17_*`/`m18_*`/`m19_*`/`m20_*` file is modified; M21 writes only `m21*` |

## 5. What the matrix measured, in one table

`S_faithful` = A(full)/C(noidx) — the pairing in which each arm pays for its own
index acquisition (M19 §1). Full analysis in `m21-performance-boundaries.md`.

| workload | structure (target set) | S_f 8K | S_f 16K | verdict |
|---|---|---:|---:|---|
| w4_duplow | 131 072 lines, 2 048 pages, 8 MiB, no reuse | **2.994** | **5.655** | large win |
| w8_wslarge | 32 206 lines, 512 pages, 2 MiB | **1.532** | **1.690** | large win |
| w1_uniform | 2 048 lines, 32 pages, 128 KiB | 1.159 | 1.164 | win |
| w5_sequential | 16 384 lines, 256 pages, 1 MiB, ordered | 1.132 | 1.138 | win |
| w7_wssmall | 256 lines, 4 pages, 16 KiB | 1.087 | 1.097 | win |
| w6_zipf | 2 048 lines, 32 pages, 128 KiB | 1.074 | 1.081 | win |
| w9_seqsmall | 256 lines, 4 pages, 16 KiB, ordered | 1.064 | — | win (marginal) |
| w10_runresid | 128 lines, 2 pages, 8 KiB | 1.041 | — | parity |
| w3_duphigh | 64 lines, 1 page, 4 KiB | 1.039 | 1.039 | parity |
| w2_clustered | 2 044 lines, 32 pages (90 % in 6.4 KiB) | 1.038 | 1.048 | parity |

The ordering is the finding, and it is not the ordering the experiment expected:
**the win is largest where the target set is largest and least ordered, and
smallest where the target set is smallest** — i.e. it is driven by the *target*
side, not by duplicate rate (w3 has a 99.95 % duplicate rate and wins 3.9 %;
w4 has a 0 % duplicate rate and wins 199 %).

## 6. Reproduce

```
cd DX100_MEM
bash build/wm/m21_matrix.sh              # 64 primary cells + 40 ablation cells
python3 tools/m21_extract.py structure    # unique lines/pages, dup rate, reuse
python3 tools/m21_extract.py boundary     # S_f, S_same and the four-arm decomposition
python3 tools/m21_extract.py memory       # DRAM, row hits, cache hit rates, TLB
python3 tools/m21_extract.py mechanism    # phases, occupancy, stalls, AG/fanout cost
python3 tools/m21_extract.py ablation      # control vs page-order/cohort ablations
```
