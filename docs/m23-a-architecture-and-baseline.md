# M23-A — Preferred DX-100 Architecture and M0 Baseline

M23 is one milestone with five subsections. **This report covers M23-A only**:
architecture and baseline establishment. M23-B (recommendation embedding lookup),
M23-C (graph adjacency), M23-D (GNN feature aggregation) and M23-E (κ sweep and
reference mapping) have not been started, and no real-workload performance
experiment has been run.

---

## 1. What is frozen

| configuration | file | one-line description |
|---|---|---|
| **M0** | `sim/configs/m23_m0.cfg` | conventional Vortex SIMT, 128 L1 MSHRs, no DX-100 block |
| **DX-100 2×8K** | `sim/configs/m23_dx8k.cfg` | fused, two 8 192-entry context buffers |
| **DX-100 2×16K** | `sim/configs/m23_dx16k.cfg` | fused, two 16 384-entry context buffers |
| **DX-100 2×32K** | `sim/configs/m23_dx32k.cfg` | fused, two 32 768-entry context buffers |

The four files are derived from one template. `m23_dx8k.cfg` → `m23_dx16k.cfg` →
`m23_dx32k.cfg` differ in **exactly one assignment** (`batch_window`) plus the
title and run lines; the shared block is byte-identical. This is asserted by
`test_m23_architecture_matrix`, which nulls the window and requires
`to_string()` equality across the three DX-100 files, and compares every shared
field of all four.

Everything in the simulator is unchanged except an additive diagnostic
(§6). No historical configuration file was edited.

## 2. The shared memory system (identical in all four)

Verified against `src/sim.hpp`, `src/memory.cpp` and `Config::to_string`, not
against the older reports.

| resource | value | where it comes from |
|---|---|---|
| cores / warps / lanes | 1 / 4 / **4** | Vortex topology; lane width fixed for every architecture |
| element width | 4 B | XLEN = 32 |
| shared global outstanding budget | **R = 128** | `R = L2_MSHR × banks = 64 × 2` (design §2.3) |
| SIMT per-core outstanding cap | none (`lsu_pending = 0`) | M5.1 §3: the SIMT arm is bounded by R alone |
| L1 D-cache | 16 KiB, 4-way, 2 cycles, **128 MSHRs** | `l1_lines = 256` × 64 B; M16 strengthening |
| L2 | 1 MiB, 8-way, 4 cycles, 64 MSHRs | `l2_lines = 16384` × 64 B |
| dTLB | 16 entries, 4-way, 4 MSHRs, `parked = 2` | Vortex `VX_CFG_DCACHE` translation |
| L2 TLB | 512 entries, 4-way, 8 MSHRs, 4 cycles | reach = 512 × 4 KiB = **2 MiB** |
| page walkers / walk cache / levels | 2 / 16 / 2 (Sv32) | 4 KiB pages, two-level walk |
| walk latency | 150 cycles per level | calibration (M4) |
| page size | **4 KiB** | `LINE_BYTES = 64`, `vpn = line >> 6` |
| DRAM | 2 banks, 4 096 rows/bank, 1 024 B/row | `banks`, `rows`, `row_bytes` |
| DRAM timing | `t_hit = 14`, `t_miss = 26`, 64 B/cycle | HBM2 calibration (M4) |
| DRAM scheduling | FR-FCFS, unbounded queue (`dram_q = 0`), hashed bank index | Vortex defaults |
| address map | row = `((line / banks) / lines_per_row) % rows` | `src/memory.cpp:365` |
| instrumentation | `cache_diag = 1` in all four | per-requester attribution (M15); decision-neutral |

### 2.1 DRAM capacity — what the simulator does and does not model

**There is no modelled DRAM capacity.** The backing store is a sparse map,
`std::unordered_map<u64, std::array<u8, 64>> img_` (`src/sim.hpp:731`), so any
address inside `VA_LIMIT` is legal and unbacked lines read as zeros. The only
place a capacity-like number appears is the row index above, which wraps at
`banks × rows × row_bytes` = **2 × 4 096 × 1 024 B = 8 MiB**. A footprint larger
than 8 MiB therefore reuses rows rather than growing memory.

Consequences, applied identically to all four configurations:

* Rows are reused every 8 MiB of span; a target set larger than 8 MiB has
  artificially high row reuse for its size.
* No allocation, no replacement, no capacity pressure, no "out of memory".
* Reported instead of a capacity: each workload's **address span** and
  **working-set footprint** (§7).

The historical record never established a capacity, and none is invented here.

## 3. M0 — the strengthened conventional baseline

M0 is the Vortex SIMT path with no engine: `dispatch = 0`, `batch_window = 0`,
`win_fused = 0`, `experiment = none`. `Sim::run()` takes the legacy path
(`src/system.cpp:72`: the hybrid driver is entered only when `dispatch != 0`), no
`BulkEngine` is constructed, and no Bulk counter exists.

**The one deliberate change from the historical baseline is `l1_mshr = 16 → 128`**
(M16: 128 L1 MSHRs saturate the shared 128-entry budget). Measured, same shared
workload (`W1`, N = 16 384, 512 pages):

| M0 variant | cycles | DRAM reads | L1 MSHR allocs |
|---|---:|---:|---:|
| historical, `l1_mshr = 16` | 270 753 | 16 417 | 16 384 |
| **M0, `l1_mshr = 128`** | **261 316** | 16 417 | 16 384 |

The 16-MSHR run reproduces the published controlled baseline **exactly**
(270 753), which is the proof that the MSHR capacity is the only difference and
that M0's execution semantics are otherwise untouched. The strengthening makes
SIMT **3.5 % faster**, i.e. it makes M0 a *harder* baseline, which is the point:
DX-100 must not be flattered by a weakened SIMT path.

## 4. The three DX-100 configurations

| knob | value | why |
|---|---|---|
| `win_fused` | **1** | fused context-buffer storage; the default for all M23 work |
| `batch_window` | 8 192 / 16 384 / 32 768 | the two physical context buffers' capacity |
| `dispatch` | **1** | compiler-directed SIMT/Bulk selection |
| `op_class` | `gen` | the class comes from compiler/workload metadata |
| `page_sched` | 1 | page-aware organisation and scheduling |
| `page_cohorts` | 2 | derived from `dtlb_mshr / dtlb_parked` |
| `dedup` | 1 | retained as a secondary optimisation (fused layout requires it) |
| `page_order` | 1 | required by the fused layout |
| `ag_lanes` / `eng_issue` | 8 / 4 | address generation and request formation |
| `idx_mode` | 2 | tiled, per-line-ready index acquisition |
| `page_dir`, `org_lanes`, `fanout_lanes`, `batch_deadline`, `org_overlap` | 512, 4, 4, 4096, 1 | unchanged from the M6/M7 configuration |
| `reuse_dir` | 0 | the cross-window reuse buffer stays off: dedup is the secondary optimisation, not reuse |
| `ab_charge_latch` | 0 | no latch charge; peak outstanding is R in every arm |

**Unfused storage is historical.** Every M18–M21 performance number in this tree
was measured with `win_fused = 0`. Those results are **not** relabelled as fused
performance and no report was edited to imply otherwise. M23 measures the fused
implementation directly, from here on.

The fused layout is one 64-bit word per element (`line 26 | position 32 |
offset 6`) plus a 6 B-per-line run table and a 16 B-per-page directory entry;
`BulkEngine` refuses a configuration that cannot express a line as a run
(`dedup = 0`, `page_order = 0`, `W > 65 536`, `R > 255`).

## 5. Compiler-directed selection

Selection is the M9 mechanism, and it satisfies every invariant the brief states
because of how it is structured, not by convention:

| requirement | how it is met |
|---|---|
| before execution | `build_op_classes()` runs once, before the first cycle; nothing classifies during the run (`src/bulk.cpp:2178`) |
| metadata, not measurement | `op_class = gen` consumes the class recorded with the trace — the compiler annotation stand-in. `pattern` (static stride analysis) exists only as a cross-check |
| no runtime sampling | no counter feeds the decision; the class map is an input |
| no waiting for Bulk to fail | there is one descriptor per bulk-class instruction, submitted once |
| no mid-operation switch, no fallback | the SIMT front end only ever sees SIMT-class instructions; there is no migration path |
| no oracle on physical addresses | the class never depends on a PA; `pattern` uses only per-instruction lane addresses, the information a compiler has when it generates the code |
| charged for metadata | one descriptor fetch per bulk operation, and the partition's elements are exactly the annotated elements — verified by an identity assertion every hybrid run |
| every decision reported | `m23_select = <path>` writes one row per instruction: class, element count, metadata source, and the rule (§6) |
| κ is not a compiler variable | κ is a *workload parameter*; the class comes from the annotation, and κ only changes the trace the annotation was made on |

Two uniform maps exist for equivalence gates (`op_class = simt` / `bulk`) and
cannot be used as a policy: they are decided before execution like any other
annotation, and they are how M9 proved the partition is the only difference
between the arms.

The audit trail for the shared workload (DX-100 2×8K, `W1` permutation):

```
# instr  class  elems  source               reason
0        bulk   4      compiler-annotation  independent-irregular-gather
1        bulk   4      compiler-annotation  independent-irregular-gather
...
4095     bulk   4      compiler-annotation  independent-irregular-gather
# totals  simt_instr=0  bulk_instr=4096  simt_elems=0  bulk_elems=16384
```

All 4 096 instructions are bulk-class because the synthetic `W1` generator
classifies by its contiguity rule and a permutation has no contiguous lane run.
That is a property of the calibration workload, not of the mechanism: M23-B..E
use file traces whose class bytes come from the application's own structure, and
those runs will show a real SIMT/Bulk mix.

## 6. The one implementation addition

`Config::m23_select` (default empty) makes `run_hybrid` write the decision table
above, once, immediately after the class map is final and before cycle 0. It is:

* **additive** — no decision, counter or ordering reads it;
* **invisible when empty** — the default, so every historical run is unchanged;
* **not in `to_string()`** — so configuration signatures and the paired-driver
  environment identity are untouched.

Paths are resolved against the simulator's working directory (`DX100_MEM/sim`),
so the trail from the CLI lands in `sim/build/wm/`, and `../build/wm/m23_run.sh`
writes to the same place.

Nothing else in the simulator was modified for M23-A.

## 7. Validation results

Run with `./bin/dx100_tests m23_architecture_matrix` (194 checks) and one CLI smoke
per configuration.

| # | requirement | result |
|---|---|---|
| 1 | fused storage active in all three DX-100 configurations | **pass** — `win_fused_active = 1`, `bulk_win_fused = 1`, `bulk_win_fused_vec_bytes > 0` |
| 2 | configured context capacities correct | **pass** — `batch_window` = 8 192 / 16 384 / 32 768, and the three files differ in that assignment only |
| 3 | M0 has no Bulk functionality | **pass** — no engine constructed, `dispatch_mode = 0`, all `bulk_*` counters zero/absent |
| 4 | identical SIMT lane width | **pass** — `lanes = 4` in all four, asserted field by field |
| 5 | identical cache / translation / DRAM / budget settings | **pass** — 35 shared fields compared pairwise |
| 6 | MSHR and outstanding settings as intended | **pass** — `l1=256x4/2cy mshr=128`, `slot_pool_total = 128`, R = 128 in all four |
| 7 | compiler-directed selection active in all DX-100 runs | **pass** — `dispatch_mode = 1`, partition identity `bulk_elems + simt_elems = trace_accesses`, `bulk_dispatch_unaccounted = 0` |
| 8 | M0 conventional semantics unchanged | **pass** — reproduces 270 753 at `l1_mshr = 16`, the published controlled SIMT baseline |
| 9 | regression tests and historical anchors preserved | **pass** — 4 735 checks / 0 failures; M17 anchor 220 123 / 138 924 / 125 581 unchanged |
| 10 | no historical result altered | **pass** — no `m1x`–`m22` log or report modified; new files only |

### 7.1 Smoke numbers (shared `W1` workload, N = 16 384) — *sanity, not results*

| configuration | cycles | DRAM reads | fused array bytes | elements admitted |
|---|---:|---:|---:|---:|
| M0 | 261 316 | 16 417 | — | — |
| DX 2×8K | 148 991 | 17 444 | 245 760 | 16 384 |
| DX 2×16K | 129 036 | 17 413 | 237 568 | 16 384 |
| DX 2×32K | 129 036 | 17 413 | 237 568 | 16 384 |

These confirm wiring only. The fused footprint matches the M15 formula exactly
(`8W + 6L + 16·page_dir + 64` per buffer: 2 × (65 536 + 49 152 + 8 192) at 8K).
Two structural facts to carry into M23-B..E rather than read as performance:

* 2×16K and 2×32K are **identical at N = 16 384** because one 16K buffer holds
  the whole run; the 32K configuration only differentiates on a workload with
  more than 16 384 admitted elements per buffer.
* DX-100 completes with *more* DRAM reads than M0 (+6.3 % at 8K): its target
  requests bypass L1 and fetch whole lines. Traffic is not the mechanism; request
  count and row locality are.

## 8. Unresolved simulator limitations

1. **No DRAM capacity model** (2.1). Spans beyond 8 MiB alias rows.
2. **L2 TLB reach is 2 MiB** (512 × 4 KiB). Any workload footprint above that
   walks continuously; this bounds the translation behaviour available to M23.
3. **2×32K is not exercised to capacity** by the shared workload. A workload with
   ≥ 32 768 admitted elements per window is required before any 32K claim is made.
4. **The synthetic generator is single-class.** Its κ rule annotates every
   instruction bulk on a permutation, so the synthetic matrix cannot exercise a
   mixed SIMT/Bulk partition; only file traces can.
5. **`op_class = pattern` must not be used as a selection source in M23**, because
   it re-derives the class from the addresses. It stays available as a
   cross-check only.
6. **`win_bytes` is a model, not the realized footprint** (M22: 6.33× at 8K). M23
   quotes `bulk_win_fused_vec_bytes`, which is measured.
7. **`batch_deadline = 4096`** seals partial windows on long runs. It is inherited
   unchanged from the M6/M7 configuration and may need revisiting for the larger
   M23 workloads; changing it would be a configuration change that must be
   applied to all three DX-100 files and documented.
8. **No area, timing or power claim.** No synthesis exists; M22's density
   assumption remains the only conversion in the tree.

## 9. Commands

```bash
cd DX100_MEM/sim

# rebuild
make -j

# the four configurations (identical except the DX-100 block)
./bin/dx100sim --config configs/m23_m0.cfg
./bin/dx100sim --config configs/m23_dx8k.cfg  --set m23_select=build/wm/m23_sel_8k.tsv
./bin/dx100sim --config configs/m23_dx16k.cfg --set m23_select=build/wm/m23_sel_16k.tsv
./bin/dx100sim --config configs/m23_dx32k.cfg --set m23_select=build/wm/m23_sel_32k.tsv

# via the wrapper (selects the file, enables the audit trail, passes --set through)
../build/wm/m23_run.sh m0
../build/wm/m23_run.sh dx8k  --set dataset=../build/wm/m18_bcsstk25_full.bin
../build/wm/m23_run.sh dx16k --set dataset=... --set bulk_index_base=0x20000000

# validation
./bin/dx100_tests m23_architecture_matrix   # 194 checks
./bin/dx100_tests                           # 4 735 checks, 0 failures
```

## 10. M23 subsection plan (not started)

| subsection | workload | status |
|---|---|---|
| **M23-A** | architecture and M0 baseline | **this report — complete** |
| M23-B | recommendation embedding lookup | not started |
| M23-C | graph adjacency | not started |
| M23-D | GNN feature aggregation | not started |
| M23-E | synthetic κ sweep and real-workload reference mapping | not started |

All four experiments must use these files unchanged, the frozen compiler-directed
selection, fused storage, and the same M0. Any deviation is a configuration
change and belongs in a report, not in an ad-hoc `--set`.
