# DX100 → Vortex Capability Gap Analysis

**Scope.** Task 2 of the DX100/Vortex research case. Using the Task 1 Vortex memory
reconnaissance (`vortex-memory-recon.md`), classify each of DX100's fourteen claimed
capabilities as implemented / partially implemented / not implemented / cannot determine
in Vortex, then decide whether a defensible research gap survives.

**Vortex snapshot.** `7300bc84981c34331b6247cc14500d8e07644988` (VORTEX_VERSION 3.0),
read-only clone at `DX100_MEM/ref/vortex`. Nothing in `DWF/` or the Vortex tree was
modified for this task; no simulation, elaboration or synthesis was run.

**DX100 reference.** Khadem, Kamalakkannan, Zhu, Poptani, Gu, Dominguez-Trujillo, Talati,
Fujiki, Mahlke, Shipman, Das. *DX100: A Programmable Data Access Accelerator for
Indirection.* ISCA 2025 (52nd), arXiv:2505.23073v2, DOI 10.1145/3695053.3731015.
Section numbers below refer to that version.

**Evidence rule.** A capability is "implemented" only if I read code or configuration in
the snapshot that performs it, or a design document in the tree that states it as built.
A signal name, a CSR, a parameter, or a paper's related-work section is never sufficient:
two of the findings below are features that *look* present in one model and are absent in
the other (§2.6, §2.13).

---

## 0. Verdict

| | Count |
|---|---|
| Already implemented in Vortex | 1 of 14 |
| Partially implemented | 8 of 14 |
| Not implemented | 5 of 14 |
| Cannot determine | 0 (4 capabilities carry sub-claims I could not settle — listed in §7) |

The single "already implemented" is DX100's *least* interesting capability (bulk
descriptors over affine iteration spaces), and it is implemented because Vortex has copied
the NVIDIA Hopper TMA model, which the tree states explicitly
(`docs/designs/dxa_async_copy_multicast.md`: "an NVIDIA-Hopper-TMA-style asynchronous
bulk-copy and multicast unit"). What Vortex lacks is precisely what makes DX100 a DX100:
the engine cannot consume data as an index.

**Bottom line.** Porting DX100 to Vortex is feature work, not a research contribution: it
needs index-based addressing, a scatter/RMW path, engine-side predicate evaluation, and a
reorder buffer with result remapping — four missing mechanisms, none of them novel after
ISCA 2025. One narrow hypothesis survives (§6), and it is *not* any of DX100's headline
techniques: it concerns the interaction between a bulk engine's known future address
stream and GPU address translation, which DX100 assumes away with huge pages.

---

## 1. Classification rubric

| Label | Meaning |
|---|---|
| **Implemented** | Vortex RTL performs this function as part of the named mechanism, under the default configuration. |
| **Partially implemented** | The function exists at a different granularity, in a different structure, or only in one of the two models (RTL / SimX). |
| **Not implemented** | No structure in the snapshot performs it; any counter or field that suggests otherwise is shown to be unwired or dead. |
| **Cannot determine** | The snapshot cannot settle it and the paper does not either. |

I also record, per capability, whether the *literature* already covers it, because
"absent from Vortex" and "absent from the field" are different claims. A capability that
is absent from Vortex and present in four prior papers is a porting exercise.

---

## 2. Capability comparison matrix

### 2.1 Summary table

| # | Capability | Vortex | Literary status | Residual gap |
|---|---|---|---|---|
| 1 | Bulk descriptors + iteration ranges | **Implemented** (affine only) | Commodity (TMA) | Data-dependent ranges; per-iteration predicate; store/RMW |
| 2 | Bulk index fetching | Partial | Covered (DVR, IRU) | Engine cannot *use* the fetched index without the core |
| 3 | Independent address generation | **Not implemented** | Covered (IRU, DVR) | Index-sourced address arithmetic in a DMA-class engine |
| 4 | Multi-level indirection | **Not implemented** | Covered (DVR) | Chained indirection without a core round trip per level |
| 5 | Cross-warp / cross-SM visibility | Partial | Covered (ICC 2018, 2019) | Cross-cluster; visibility of *future* requests, not just issued ones |
| 6 | Duplicate elimination | **Not implemented** | Covered (ICC, IRU) | Tile-granularity dedup before issue |
| 7 | Cache-line grouping | Partial | Covered (warp coalescer, IRU) | Grouping of reordered requests |
| 8 | DRAM row/bank/channel scheduling | **Not implemented** | Covered (PHI, COBRA, Milk, MC FR-FCFS) | Honest baseline does not exist in Vortex (§2.8) |
| 9 | Large outstanding window | Partial | Covered (DVR, PHI) | Single-client 16K-address future view |
| 10 | Result tracking / remapping to threads | Partial | Covered (IRU relaxes mapping) | Element→iteration remap (vacuous without reorder) |
| 11 | Dependency handling / iteration overlap | Partial | Covered (DVR, runahead) | Breaking the index→data chain in hardware |
| 12 | Conditional filtering + compaction | **Not implemented** | Covered (IRU filter, stream compaction) | Engine-side predicate for stores/RMW |
| 13 | Address translation / TLB | Partial | **Crowded through MICRO 2025** | Engine-driven walk scheduling (§6) |
| 14 | Software / compiler interface | Partial | Covered (Milk, Polygeist) | Automatic hoisting of `A[B[i]]` |

### 2.2 Row 1 — Bulk operation descriptors and iteration ranges

**Vortex: implemented, for affine iteration spaces.**

- A descriptor is `dxa_desc_t`: `base_addr`, `meta`, packed tile sizes `tile01/tile23/tile4`,
  `cfill`, bounds `size0..4`, byte row strides `stride0..3`, per-CTA `smem_stride`
  (`docs/designs/dxa_async_copy_multicast.md` §2; meta subfield layout at
  `hw/rtl/dxa/VX_dxa_pkg.sv:36-47`). Sixteen descriptors live in a DCR-written table
  (`VX_DCR_DXA_DESC_COUNT = 16`), selected by `meta[3:0]` (`sw/kernel/include/vx_dxa.h:26-28`).
- Ranks 1–5 are issued from software by `vx_dxa_issue_{1,2,3,4,5}d_wg`
  (`sw/kernel/include/vx_dxa.h:72,85,99,117,136`); a whole warp's operands are packed into
  lanes first (see §2.14).
- Execution is a single rolling cursor plus a 4-level ripple odometer
  (`hw/rtl/dxa/VX_dxa_addr_gen.sv:16-30`), one beat per cycle, with OOB clamp and constant
  fill.

**DX100: §3.1, Table 2.** `ILD/IST/IRMW/SLD/SST/ALUV/ALUS/RNG`; loops are `single`
(`i = F to G`) or `range` (`j = H[i] to H[i+1]`, `j = H[K[i]] to H[K[i]+1]`); `DTYPE`,
`OP`, `TC` (condition tile) are descriptor-level fields; `RNG` fuses many short range loops
into one tile (§3.4).

**Residual gap.** Everything DX100 puts in a descriptor that Vortex does not: (a) an
iteration count that comes from memory rather than from a register, (b) an opcode field
selecting load / store / RMW, (c) a per-iteration predicate tile. Vortex's descriptor
encodes *where*, never *whether* or *what to do there*, and the DXA has no software→global
path at all (`docs/designs/dxa_async_copy_multicast.md` §1; `hw/rtl/dxa/VX_dxa_gmem_req.sv:200`
fixes `req_data.rw = 1'b0`).

### 2.3 Row 2 — Bulk index fetching

**Vortex: partial.** A contiguous index array can be streamed into LMEM by a rank-1 DXA
transfer — functionally DX100's `SLD` (§3.3). But DX100's point is that the index tile is
*consumed in place* by the next unit: "as soon as an element of B[i] is loaded into the
Scratchpad by the Stream Access unit, the Indirect Access unit can immediately access it"
(§3.5). In Vortex the tile lands in LMEM and only threads can read it; any use of the
index requires the warp to issue loads through the LSU.

**Residual gap.** An index tile that is an *operand of the engine*, not a destination.

### 2.4 Row 3 — Independent address generation

**Vortex: not implemented.** The only address generator in the tree that is decoupled from
the core is the DXA's, and it is a constant-stride odometer: `VX_dxa_addr_gen.sv:16-30`
advances `gmem_cursor_r` by precomputed deltas from `VX_dxa_setup`'s three DSP multipliers.
No path carries data from a returned cache line back into address generation — verified by
reading the module list: `gmem_req → smem_wr → completion`, with no feedback into
`addr_gen` (`docs/designs/dxa_async_copy_multicast.md` §3, §4 steps 5–9).

**DX100: §3.2, Operation Stage 1.** "Address Generator fetches the index value
(`idx = SPD[TS1][i]`) from the Scratchpad and calculates the virtual word address
(`BASE[idx]`)." The index source may be a stream (`A[B[i]]`), a previous indirect access
(`A[B[C[i]]]`), or the ALU (`A[f(C[i])]`).

**Residual gap.** The defining DX100 mechanism. Absent from Vortex; present in the
literature (IRU reorders fetched indices; DVR vectorizes the chain in-core).

### 2.5 Row 4 — Multi-level indirection

**Vortex: not implemented.** Each DXA transfer is independent; a second level requires the
elements of the first to be read by threads (or a new kernel/barrier phase) and a new
descriptor written. There is no scratchpad-to-descriptor path: descriptors live in a
DCR-written RAM (`VX_dxa_desc_table.sv`, single read port) and are never written by the
engine.

**DX100: §3.1** ("multiple levels of indirection `A[B[C[i]]]` are common"), enabled by
tiles as both index source and destination (`TS1` from "indirect access for multi-level
indirection", §3.2).

**Residual gap.** Chained indirection with no core round trip per level.

### 2.6 Row 5 — Cross-warp and cross-SM request visibility

**Vortex: partial, and structurally different from DX100's.**

- The L1 dcache is shared by a whole socket: `VX_cache_cluster` instantiated once with
  `.NUM_REQS (DCACHE_NUM_REQS)` over all `VX_CFG_SOCKET_SIZE` cores (`hw/rtl/VX_socket.sv:247-258`),
  so requests from every warp of every SM in a socket meet in one cache.
- The L2 is shared per cluster, with MSHRs per bank fed by all core ports; a request that
  matches a line already in flight links into the existing chain
  (`hw/rtl/cache/VX_cache_mshr.sv:207`, `addr_matches`).
- Cross-cluster: no. Cluster ports meet only at the memory bus / LLC.

**DX100: §3 (design rationale).** "a shared accelerator can reorder memory accesses across
threads from different cores … further improving row buffer hit rates and enabling higher
coalescing factors."

**Residual gap.** Vortex's visibility is over requests *already issued into a cache*, and
is used only for merging and hazard ordering. DX100's is over a 16K-element batch not yet
issued, used for reordering. Those are not the same capability, and Vortex has no structure
that holds a batch.

**Literature.** Intra-cluster coalescing (IPDPS 2018) already gives cross-SM visibility
inside a cluster for exactly the merge purpose.

### 2.7 Row 6 — Duplicate access elimination

**Vortex: not implemented (beyond same-word merge).** This row required the most care
because two artefacts suggest otherwise:

1. `VX_CFG_DCACHE_READ_MERGE` merges *same-word reads from the ports of one LSU block in
   the same cycle* (`hw/rtl/VX_socket.sv:279`, `hw/rtl/cache/VX_cache.sv:65,102-106`), and
   the icache merges same-line fetches (`VX_socket.sv:183`). Both are single-cycle,
   single-block, same-address merges — a uniformity optimisation, not access elimination
   across a working set. The L2 instantiation passes no `READ_MERGE` at all, so nothing
   merges at the LLC.
2. MSHR chaining merges same-line requests (`VX_cache_mshr.sv:207`).

3. The decisive artefact: `perf_gmem_dedup`. In the RTL it is declared, reset, and exported
   — and **never incremented** (`hw/rtl/dxa/VX_dxa_worker.sv:315-341`; CSR read at
   `hw/rtl/core/VX_csr_mpm.sv:176`; cluster sum at `hw/rtl/dxa/VX_dxa_core.sv:253`). In
   SimX it *is* incremented, but the increment only counts *consecutive identical
   cache-line addresses* while still enqueueing work for every element
   (`sim/simx/dxa/dxa_core.cpp:545-549`: `if (!elem_oob && cl_addr == global_prev_cl) {
   ++perf_stats_.gmem_dedup; }` followed unconditionally by `w.work_list.push_back(lw)` at `:550`).
   So the CSR measures adjacency, not elimination, and the RTL does not measure it at all.

**DX100: §3.2, Word Table.** "A Word Table organizes target words within each column into a
linked list … This component enables the unit to access unique columns, effectively
coalescing redundant accesses within a tile." Measured: 32K tiles reduce memory accesses by
1.4× geometric mean over 1K tiles (§6.4).

**Residual gap.** Dedup across a tile before issue. Real, but not novel: IRU filters
duplicates in the memory partition, ICC merges redundant L1 misses across SMs (19.4%
redundant on average, up to 48%).

**Model divergence note.** This is the one metric where RTL and SimX disagree by
construction; any experiment quoting `dxa.gmem_dedup` from RTL would read zero.

### 2.8 Row 7 — Cache-line grouping

**Vortex: partial.** The LSU coalescer groups adjacent lanes into aligned requests per
fixed lane groups (`hw/rtl/libs/VX_mem_coalescer.sv:126-170`), and the DXA's SMEM writer
groups by construction (a row-major span is drained as one aligned write,
`lw.valid_length = gspan`, `sim/simx/dxa/dxa_core.cpp:534`).

**Absent:** grouping of a *reordered* address set — choosing which of 16K addresses share a
line. Vortex groups in lane order, so grouping quality is whatever the programmer's index
layout gives.

**DX100: §3.2.** Row Table slices (per DRAM bank, BCAM of 64 rows) + Word Table column
lists = grouping by row and by column after reorder.

### 2.9 Row 8 — DRAM row/bank/channel-aware scheduling

**Vortex: not implemented in RTL; partially modelled in SimX.** This row carries the most
important methodological finding of Task 2.

- RTL: there is no memory controller. Address interleaving is a static function of the
  block address, exposed as a `INTERLEAVE` parameter on the AXI bridge
  (`hw/rtl/mem/VX_membus_to_axi.sv:24`); cache-level bank spreading is a hash
  (`VX_CFG_DCACHE_BANK_HASH` / `VX_CFG_L2_BANK_HASH`, `docs/designs/mlp_optimizations.md`,
  "Hashed bank select"). Neither is row-aware; both are fixed maps.
- SimX: `sim/common/dram_bank_model.h:65-84` is a per-bank reservation model with separate
  read/write queues, watermarks, a horizon and optional refresh. It decides *when* a bank's
  queued command issues; it does not select among a bank's queued rows by row-hit.
- The DXA descriptor has `L2PROMO` (and `SWIZZLE`, `INTERLEAVE`) fields allocated in
  `VX_types.toml:278-291` but **unwired** per the design doc §2 — a hook, not a capability.

**DX100: §3.2 stages 1–2, §3.6.** Row Table BCAM finds an unsent entry for the target row,
the Word Table links same-column words, and the Request Generator "arbitrates among Row
Table slices in a predetermined order, interleaving slices corresponding to different DRAM
channels and bank groups." Motivation: MC windows are 32–128 accesses (§2.1, §7), DX100's
is 16K.

**Residual gap and the trap.** The mechanism is absent. But Vortex's DRAM baseline has no
FR-FCFS, so an engine that reorders for row hits would be measured against a scheduler that
does not reorder at all. DX100's own 2.7× row-buffer-hit gain is relative to a real FR-FCFS
controller with a 32-entry request buffer per channel (§5 config). **Any Vortex experiment
that reports DX100-style row-locality speedups without first implementing a credible
MC scheduler is measuring the absence of a baseline, not the value of the idea.** This
must be fixed before capability 8 can be researched here, and fixing it is itself a
substantial task.

### 2.10 Row 9 — Large outstanding request windows

**Vortex: partial, and the framing is inverted.** Vortex has an explicit programme for
this, documented in `docs/designs/mlp_optimizations.md`:

- MSHR depth is sized by Little's law from platform bandwidth × latency:
  `VX_CFG_DCACHE_MSHR_SIZE` clamped 16–64 (`VX_config.toml:290`), `VX_CFG_L2_MSHR_SIZE`
  16–256 (`:337`), L3 likewise (`:362`).
- The LSU pending pool: `VX_CFG_LSU_PENDING_SIZE = min($VX_CFG_LSU_PENDING_LIMIT,
  max(8, 2 * $VX_CFG_NUM_WARPS * ($VX_CFG_SIMD_WIDTH // $VX_CFG_NUM_LSU_LANES)))`
  (`VX_config.toml:139`) with the limit clamped 32–256 (`:142`). For the default 4-warp /
  4-thread / 4-lane shape this evaluates to **8** — the number quoted in the Task 1 report,
  now with its derivation.
- Fill forwarding and back-to-back fills keep the chain from blocking the arbiter.

So the *aggregate* outstanding capacity of the Vortex memory system is comparable to or
larger than DX100's (128-entry Request Table, 32-entry MC buffer per channel, §5). What
Vortex lacks is a *single client with a large future view*: the DXA's own window is small
(`VX_CFG_DXA_MAX_INFLIGHT = 8`, `VX_CFG_DXA_QUEUE_SIZE = 16`, `VX_config.toml:429-430`;
`slot_table_r[MAX_OUTSTANDING]` credit model in `VX_dxa_gmem_req.sv`).

**Residual gap.** Not "more MSHRs" — a batch-shaped requester. Claiming "large windows" as
the contribution would be refuted by Vortex's existing Little's-law sizing.

### 2.11 Row 10 — Result tracking and mapping to original GPU threads

**Vortex: partial.** DXA tracks per-tag state (`slot_table_r`, ≤8 in flight), synthesises
immediate arrivals for OOB lines, defers the last line so it drains carrying the completion
flag, and releases a barrier through `VX_dxa_completion.sv` → `txbar`. Multicast replays
writes per receiving CTA at `issuer_smem + r·smem_stride`
(`hw/rtl/dxa/VX_dxa_smem_wr.sv:77-80`; `VX_dxa_core.sv` fan-out). For ordinary loads,
word-granular MSHR hazards keep per-lane results ordered against stores
(`VX_cache_mshr.sv:526-535`).

**Absent:** any element→iteration mapping, because there is no reorder to undo. DX100 needs
`Previous_i` and `Tail_i` in the Word Table (§3.2 stage 3: "The Word Table traverses this
linked-list to retrieve the word offsets (WO) and iteration numbers (i)") precisely to put
reordered results back in iteration order.

**Residual gap.** Vacuous without capability 3; becomes necessary the moment indices are
reordered.

### 2.12 Row 11 — Dependency handling and overlapping independent iterations

**Vortex: partial.** The scoreboard releases destinations on completion
(`hw/rtl/core/VX_scoreboard.sv:170-186`); the DXA frees the issuing warp immediately and
completes via a barrier transaction, and its setup engine overlaps the next transfer's
setup with the previous transfer's drain (`VX_dxa_setup.sv`, "staged → active",
docs §4 step 4). Within a transfer, beats overlap freely.

**Absent:** hardware that breaks the *index → data* chain. In Vortex a level of indirection
costs cores: load index, compute address, load data. DX100 removes the core from that loop
and overlaps stream fill with indirect issue (§3.5).

**Literature.** This is exactly the decoupled/runahead line: Vector Runahead (ISCA 2021),
Decoupled Vector Runahead (MICRO 2023, 1139 bytes of state, 2.4× over a 5-wide OoO core),
Scalar Vector Runahead (MICRO 2024).

### 2.13 Row 12 — Conditional access filtering and request compaction

**Vortex: not implemented at engine level.**

- Data-dependent filtering exists only as SIMT lane masking: the LSU/coalescer honours the
  thread mask, so a predicated-off lane contributes no request. That is per-warp
  predication at issue time, not filtering of a batch.
- The DXA has *bounds* filtering, not predicate filtering: per-element OOB detection
  (`elem_oob`, `sim/simx/dxa/dxa_core.cpp:523-526`) with clamp and `cfill`, which is a
  correctness mechanism for tiled copies.
- No compaction exists because there is nothing to compact into: no store/RMW path.

**DX100: §3.1, §3.2 stage 1.** Condition tile `TC` is read per element; "If condition
holds, it triggers Address Generator". Conditions are mandatory for stores and RMWs:
"neglecting conditions for store and RMW compromises the correctness." Filtering is exact
("DX100 ensures 100% accurate fetching", §6.3) — the contrast DX100 draws against
prefetchers.

**Residual gap.** Engine-side predicate evaluation. Real; covered in the literature only
partially (IRU filters duplicates and reorders a frontier; stream compaction is a
well-known software primitive).

### 2.14 Row 13 — Address translation and TLB interaction

**Vortex: partial, and this is where the interesting gap lives.**

Verified translation facts (default configuration):

| Item | Value | Source |
|---|---|---|
| Base page size | 4 KB (`VX_VM_PAGE_LOG2_SIZE = 12`) | `VX_types.toml:71` |
| Page-table format | SV39, 3 levels, 8 B PTEs | `VX_types.toml:73-76` |
| L1 DTLB | 16 entries | `VX_config.toml:205` |
| L1 TLB MSHR | 4 | `VX_config.toml:207` |
| L2 TLB (shared) | 512 entries, 4-way, 4-cycle | `VX_config.toml:219-223` |
| Mega-page entries | 8 (`VX_CFG_L2_TLB_MEGA_SIZE`) | `VX_config.toml:221` |
| L2 TLB MSHR | 8 | `VX_config.toml:222` |
| MMU width | one lane per dcache port of the socket (`SOCKET_SIZE * DCACHE_NUM_REQS`) | `hw/rtl/VX_socket.sv:344` |

Consequences: 16 L1 entries cover 64 KB; 512 shared entries cover 2 MB at 4 KB pages. A
DX100-sized tile of 16K elements scattered over a large array touches up to 16K distinct
pages; the 8 mega entries cover 16 MB even if 2 MB pages are in use.

**DX100: §3.6.** "We assume the stream and indirect memory regions are mapped through huge
pages … Using huge pages, we provision a small 256-entry TLB … We provide certain DX100 APIs
to transfer the required PTEs to DX100's TLB once for the whole application lifetime."
Translation itself is a hard step in the pipeline: "Address Decoder translates the virtual
address to the physical address using Interface" (§3.2 stage 1), and the TLB also holds the
coherence region for scalability (§6.6).

**Residual gap.** (a) No engine-private, software-preloaded translation structure in
Vortex — DXA requests ride whatever translation the socket provides. (b) DX100 *buys its
way out* with huge pages; Vortex's default is 4 KB, so the cost DX100 assumes away is
present here. **Whether DXA gmem requests are translated by the socket dMMU or issued
physical is not settled in this pass** — see §7.

**Literature.** Crowded and current: scheduling page-table walks by work-to-service and
batching per SIMD instruction (Shin et al., ISCA 2018, pp. 180-192); neighborhood/P TE
cache-line coalescing for walks (MICRO 2018, 37% fewer page-table accesses, 1.7×); inter-L1
TLB locality prefetch/probe (Valkyrie, PACT 2020, 1.95×); intra-group shared L1 TLBs
(ICPP 2023, +20.5%); DUCATI's unified cache/TLB and DRAM-TLB (TACO 2019, +81%); software
page-table walking (SoftWalker, MICRO 2025, −72.8% walk latency, 2.24× overall / 3.94× on
irregular); and LATPC (MICRO 2025), which coalesces warp-level TLB misses *and batches walks
to raise DRAM row-buffer locality* (1.47×).

That last one matters for §6: "batch the walks for row locality" is already published, in
2025. A TLB story must therefore be narrower than "walks with better locality".

### 2.15 Row 14 — Software and compiler interface

**Vortex: partial.**

- Software surface exists and is complete for what the hardware does: `sw/kernel/include/vx_dxa.h`
  (issue intrinsics, operand packing, `vx_dxa_pack_meta`), `sw/runtime/include/dxa.h`,
  `sw/common/dxa_meta.h`, host-side DCR descriptor programming, and a documented
  pack-a-4-lane-warp procedure (`vx_dxa.h:37-57`, `__VX_DXA_ISSUE1/2`).
- The instruction is a RISC-V `custom0`/`funct7=0x3` extension (per the design doc; `MISA`
  bit 10 — I did not verify the MISA field here). Opcode `INST_SFU_DXA = 4'h9` at
  `hw/rtl/VX_gpu_pkg.sv:615`, decoded to `EX_SFU` at `hw/rtl/core/VX_decode.sv:879-880`.
  Note: `docs/designs/dxa_async_copy_multicast.md` cites `VX_gpu_pkg.sv:514` and
  `VX_decode.sv:694-698` for these, which are **stale in this snapshot** — the design
  docs' line numbers must be re-checked before they are trusted as evidence.
- Operand transport reuses `vx_wgather` — which is a *register cross-lane permute*
  (`hw/rtl/VX_gpu_pkg.sv:503-504`; ALU implementation `hw/rtl/core/VX_alu_int.sv:162-190`),
  not a memory gather. The DXA borrows it to move six operands into four lanes. (This is
  the naming trap flagged in the Task 1 report.)
- **Absent:** any pass that finds `A[B[i]]`, proves it legal, hoists it, and emits these
  intrinsics. Vortex has the LLVM-based toolchain and intrinsics, not the transformation.

**DX100: §4.2.** Polygeist → MLIR `affine`/`scf`, loop tiling, DFS over use-def chains from
induction variables to detect indirection, legality via alias analysis (no core stores to
the region, no cross-iteration dependencies), hoist to `packed_load`, sink to
`packed_store`/`packed_RMW`, pattern-match to DX100 APIs, insert `wait` calls. §4.1 adds a
manual API fallback and states the ISA is 192 bits, sent as three 64-bit MMIO stores.

**Residual gap.** The compiler half. Note this is *not* a hardware gap, and Vortex's
register-packing issue mechanism (three-operand-per-lane) is far clumsier than a 192-bit
MMIO write.

---

## 3. Prior-art comparison table

Scope: what each work actually provides, so that a Vortex proposal can be positioned
without overclaiming. Rows marked † were read only in abstract/secondary form.

| Work | Venue | Mechanism | Covers DX100 capability | Does not cover |
|---|---|---|---|---|
| **DX100** †(full text read) | ISCA 2025 | Shared mmio accelerator, 8-instruction ISA, scratchpad tiles (16K × 4 B), Row Table (BCAM/slice, 64 rows) + Word Table linked lists, 256-entry TLB + huge pages, MLIR passes | 1–14 as claimed | Row-locality vs a real FR-FCFS MC (its own baseline); 4 KB translation; scatter correctness without exclusive region ownership |
| **TMA** (NVIDIA Hopper) | product, 2022 | Per-SM engine, `CUtensorMap` descriptors 1–5D, bulk async copy GMEM↔SMEM, `cta_mask` multicast, swizzle, L2 promotion, mbarrier completion | 1, 2 (as copies), 7, 10, 14 | Indirection, reordering, dedup, predicates, RMW. Vortex's DXA is explicitly modelled on it |
| **IRU** (Segura, Arnau, González) † | arXiv 2007.07131; J. Supercomputing 79:762–787 (2023) | Reorder unit in memory partitions beside L2; partitioned hash table groups indices that map to the same block; duplicates filtered in L2; relaxes thread↔index mapping so reordered indices may be returned | 3, 5 (in-partition), 6, 7, 10 (by relaxing the mapping) | No ISA, no multi-level indirection, no RMW/store, no conditions, no DRAM row table. Reported 1.32× coalescing gain, 13% energy saving (secondary) |
| **ICC** (Wang, Zhao, Kaeli, Wang, Eeckhout) | IPDPS 2018, pp. 990–999; TC 68(7) 2019 | Merges L1 misses across SMs of a cluster before the NoC; distributed-block scheduling; coalesced cache | 5, 6 | Bulk descriptors, indirection, reorder, DRAM scheduling. 19.4% (to 48%) of L1 misses redundant; +9.7% (to 33%) |
| **DVR** (Naithani, Roelandts, Ainsworth, Jones, Eeckhout) | MICRO 2023, pp. 17–31 | In-core decoupled vector runahead subthread: infers loop bounds, vectorizes the indirect chain, issues far ahead into L1, handles divergence, nested runahead to cross outer iterations | 2, 3, 4, 11 | No offload of address computation, no DRAM row awareness, no cross-SM dedup, prefetch not service. 1139 B state; 2.4× over 5-wide OoO, 2.0× over VR |
| **DMP** (Fu et al. 2024) † | as characterized by DX100 §6.3 | State-of-the-art indirect prefetcher; raises access rate but does not reorder | 2/11 as prefetch | Reordering, coalescing, predicates. DX100's own SOTA baseline; details not read here — evidence gap |
| **CaLRS** (Ma, Meng, Chen, Wu) | ScientificWorldJournal 2015:848416 | Criticality-aware service order at shared LLC banks | 8 (at LLC, not DRAM row) | Indirection, row targeting, batch visibility |
| **PTW scheduling** (Shin et al.) | ISCA 2018, pp. 180–192 | Reorder page-table walks by work-to-service, batch per SIMD instruction | 13 | Bulk-engine-driven walks |
| **Neighborhood translation** | MICRO 2018 | Coalesce PTEs already read by a walk for neighboring pages; 37% fewer PT accesses, 1.7× | 13 | — |
| **Valkyrie** | PACT 2020 | Inter-L1-TLB locality prefetch + probe; 1.95× combined | 13 | — |
| **IGS-TLB** | ICPP 2023 | L1 TLBs decoupled into groups with disjoint ranges; +20.5% | 13 | — |
| **DUCATI** | TACO 2019 | Unified cache/TLB (UCAT) + DRAM-resident TLB; +81% | 13 | — |
| **SoftWalker** | MICRO 2025 | Software threads perform walks; In-TLB MSHRs; −72.8% walk latency, 2.24× (3.94× irregular) | 13 | — |
| **LATPC** | MICRO 2025 | Warp-level VPN coalescing + MSHR compression + **walk batching for DRAM row locality**; 1.47× | 13, and 8 applied to walks | Bulk-engine future knowledge (works from a warp's current instruction) |
| **Milk / Propagation Blocking** | 2016 / 2017 | Software batching of random accesses into cache-fitting, row-sorted batches | 3, 8 (by software) | Hardware; adds intermediate structures and cache pollution (DX100 §7) |
| **PHI / COBRA** | 2019 / 2022 | Defer writes / RMW with poor locality, batch to sequential DRAM | 8 | Indirection generality |

**Reading of the table.** Every one of DX100's fourteen capabilities has at least one prior
work, and six of them (5, 6, 7, 8, 11, 13) have multiple. The claim that survives is not a
capability claim; it is a coupling claim (§6).

---

## 4. Ranked candidate gaps

Ranked by (a) genuinely absent from Vortex, (b) not already published in this exact form,
(c) measurable inside Vortex without first building a new research baseline.

### Rank 1 — Translation for a bulk engine whose entire future address stream is known

Vortex: 4 KB base pages, 16-entry L1 DTLB, 512-entry 4-way L2 TLB with 8 MSHRs, a walker
shared by every dcache port. DXA: an engine that can enumerate a tile's addresses ahead of
issuing them, currently with no relation to the walker at all.

The published GPU-translation work (ISCA'18 through MICRO'25) schedules or coalesces walks
using *current* demand: the warp's in-flight instruction, the in-flight miss queue, samples
of reuse. None of it has a producer that knows 16K addresses before the first request is
issued, and DX100 does not use its lookahead for translation at all — it assumes huge pages
and preloads 256 PTEs.

Survives because: it is absent from both Vortex and DX100; it is narrow; it can be measured
with translation counters (walk count, walk latency, TLB MPKI) without touching the DRAM
model; and it is orthogonal to LATPC (which batches a warp's own misses).

### Rank 2 — Engine-side predicate + variable-length range as a single descriptor

Vortex cannot express `j = H[K[i]] to H[K[i]+1]` with `if (D[E[j]] < F)` in one mechanism:
ranges are register-set strides, and predicates live in the warp's thread mask, so a
per-element condition forces the core back into the loop. DX100's `RNG` + `TC` + `ALUV`
handles both engine-side (§3.1, §3.4).

Weaker than Rank 1 because the capability itself is DX100's, and IRU already ships
filtering hardware in a GPU memory partition. The defensible increment would be "condition
evaluation on *store and RMW* batches with reordering", which is a correctness-and-ordering
problem, not a bandwidth problem — and which is exactly why DX100 requires exclusive region
ownership (§3.6, §6.6). On a GPU, where a bulk engine shares data with texture/raster/
tensor clients, that exclusivity assumption is not free.

### Rank 3 — Tile-granularity duplicate elimination before issue

Absent in Vortex (§2.7 — the counter is dead in RTL and mislabelled in SimX), measured by
DX100 at 1.4× fewer accesses for 32K vs 1K tiles. But ICC and IRU do the same thing at line
and partition granularity, so the contribution would be a granularity study, which is thin.

### Rank 4 — Row-aware scheduling inside a GPU-side engine

The mechanism is absent, and cannot be researched honestly here yet: Vortex's DRAM model
has no FR-FCFS (§2.9). Building that baseline is a prerequisite task, not a contribution.

### Rank 5 — Batch-shaped requester / large window

Dismissed as a claim: Vortex sizes MSHRs and the LSU pending pool by Little's law
(`docs/designs/mlp_optimizations.md`) and the DXA already has a 16-slot queue with an
8-transfer credit limit. "We will have a bigger window" invites a refutation by
configuration.

### Rank 6 — Result remapping to original threads

Vacuous while there is no reorder (§2.11). Only becomes interesting as a corollary of
Rank 3 or 4.

---

## 5. Why each candidate might not be novel

| Candidate | Strongest non-novelty objection |
|---|---|
| 1 (translation lookahead) | LATPC (MICRO 2025) already batches walks to improve row locality; ISCA'18 already reorders walks by benefit. The increment is "the requester knows the future", which reviewers may call an engineering coupling rather than a mechanism. Also: a 4 KB-page story may be dismissed as "use huge pages", which is DX100's own answer. |
| 2 (predicate + range) | DX100 2025 owns it on CPU; IRU owns filtering in a GPU partition; predication is native to SIMT. The GPU-specific version (predicated *stores* under reordering) is a coherence/ordering argument, and requires exclusive regions — a restriction a GPU cannot impose. |
| 3 (tile dedup) | ICC 2018/2019 reports 19.4% redundancy across SMs and merges it; IRU filters in L2 partitions. A tile-granularity variant risks reading as a parameter sweep. |
| 4 (row-aware scheduling) | FR-FCFS (Rixner 2000), parallelism-aware batch scheduling (Mutlu & Moscibroda 2008), PHI 2019, COBRA 2022, Milk 2016, Propagation Blocking 2017 all target row locality. DX100's contribution was the *window*, and that window argument is DX100's. |
| 5 (large window) | Little's-law MSHR sizing is already in Vortex; DVR reaches far ahead in-core with 1139 bytes. |
| 6 (result remap) | IRU already changes the thread↔index contract; the linked-list remap is a standard scatter-gather structure. |

Additional structural objections that apply to all six:

- **DX100 already published the thesis.** "Offload bulk indirection to a shared engine and
  reorder for DRAM bandwidth" is 2025 work with 12 benchmarks. A Vortex port is a
  reproduction.
- **The Vortex baseline is weak where DX100's gains live.** No FR-FCFS at the MC, one
  memory model shared with SimX, 2 HBM2 channels with latency 0 in the default config.
  Gains in this area are easy to produce and hard to defend.
- **GPU asymmetry.** DX100's evaluation has 4 cores, 2 DDR4 channels, 51.2 GB/s — a
  bandwidth-starved CPU machine where reordering pays. A GPU with 30+ SMs already sustains
  far more outstanding requests per byte/s of bandwidth; the same reordering window buys
  less. Nothing in the snapshot suggests otherwise, but nothing measures it either.

---

## 6. The recommended narrow hypothesis

**One survives.** It is a translation hypothesis, not a bandwidth hypothesis.

> **Hypothesis.** For a bulk indirect-access engine on a GPU with 4 KB base pages, exposing
> the engine's pre-issue address stream to the L2 TLB / page-table walker converts
> translation from a demand-serialised cost into a scheduled one: the engine can present
> the walker with a *complete, deduplicated set of the pages its tile will touch, ordered by
> the row/bank coordinates the walks will hit*, before the first data request is issued.
> The expected effect is fewer walks, lower walk queueing, and a higher fraction of walk
> requests that find their PTE row open — measurable at translation-only counters even
> where end-to-end cycles barely move.

**Why this and not DX100.** DX100 never puts its lookahead and its TLB in the same
sentence: it assumes huge pages and preloads 256 PTEs by API (§3.6), which removes the
problem it would otherwise have. Vortex's default 4 KB pages put it back. The GPU
translation literature (ISCA'18, MICRO'18, PACT'20, ICPP'23, MICRO'25 ×2) has no producer
with a 16K-address future view — LATPC works from one warp instruction's VPNs, Shin et al.
from the in-flight walk queue.

**Minimum evidence needed before any implementation is justified** (all of it is
measurement, none of it is design):

1. Translate-then-measure: does a 16K-element random gather in Vortex already bottleneck on
   translation? Report TLB MPKI at L1 and L2, walk count, and walk-queue occupancy for a
   tile-sized access pattern, against a sequential control of the same footprint.
2. Ablate page size: 4 KB vs mega-page entries, at fixed TLB capacity. If mega pages alone
   remove the effect, the hypothesis is dead — that is DX100's answer and it is free.
3. Ablate TLB capacity alone. If 4× the L2 TLB entries reproduces the gain, the mechanism
   is capacity, not scheduling.
4. Only then: does perfect future knowledge (an oracle fed the tile's page set) beat
   LATPC-style warp coalescing by a margin that survives the ablation in (3)?
5. Independent of the above: confirm whether DXA gmem requests are translated at all
   (§7) — a hypothesis about engine-driven walks presupposes the engine's requests take
   walks.

**If (1) shows translation is not a bottleneck for tile-shaped access, the honest
conclusion is that there is no defensible gap**, because Ranks 2–6 are either DX100's own
published capability or blocked by a baseline (Rank 4) or a parameter sweep (Ranks 3, 5).

---

## 7. Evidence gaps

Explicitly unresolved in this pass. None of these change a classification above; several
would change what an experiment could claim.

1. **No measurements at all.** Every Vortex number here is a configuration expression
   evaluated by hand for the default shape (4 warps, 4 threads, 4 LSU lanes, 1 LSU block).
   No simulation, elaboration, or synthesis was run. The parameter values are exact; their
   *effect* is unmeasured.
2. **Whether DXA gmem requests are translated.** The DXA issues on the cluster's memory bus
   (`VX_dxa_gmem_req.sv`), and the socket dMMU has one lane per dcache port
   (`VX_socket.sv:344`), but I did not trace whether DXA traffic enters those lanes or
   whether the runtime pins DXA regions physically. Capability 13's classification
   ("partial") is safe either way; hypothesis §6 step 5 depends on the answer.
3. **SimX DRAM arbitration policy.** `sim/common/dram_bank_model.h:65-84` shows per-bank
   queues, watermarks and a horizon; I did not trace the selection among ready commands, so
   "no FR-FCFS" is established for the RTL (which has no MC) but only inferred for SimX.
4. **RTL/SimX equivalence.** Carried over from Task 1 and now concrete: `gmem_dedup` counts
   in SimX and is dead in RTL, and SimX's DXA is described in the design doc as "NoC-only"
   while the RTL has a full setup/addr-gen/smem path. Any metric quoted from one model is
   suspect in the other.
5. **DX100's own translation cost is unquantified.** The paper reports area, power, request
   table size and TLB entries, but no TLB-miss or walk statistic under its huge-page
   assumption. I cannot claim DX100 "has a translation problem" — only that it assumes the
   problem away.
6. **DMP (Fu et al. 2024)** — DX100's SOTA baseline — was not read; its characterization
   here is DX100's.
7. **IRU and ICC figures** are from abstracts and secondary descriptions (IRU: 1.32×
   coalescing, 13% energy; ICC: 19.4%/9.7%), not from full-text reading of the mechanism's
   constraints.
8. **Vortex's "already implemented" row 1 is scoped to DXA.** I did not survey the command
   processor's DMA or the texture path for a second descriptor mechanism; the CP DMA is
   described in `docs/designs/command_processor.md` and was out of scope here.

---

## 8. Decision

- **Is there a missing capability?** Yes, several, and they are enumerated and evidenced.
- **Is any of them a research contribution by itself?** No. Rows 3, 4, 6, 8 and 12 are
  feature work relative to DX100 2025, and the literature covers each.
- **Is one narrow hypothesis worth simulating?** One, and only conditionally: engine-driven
  page-walk scheduling from a pre-issue address stream (§6), gated on measurement steps
  1–3, which are experiments on *existing* Vortex code and require no new hardware.
- **What must not be claimed.** That DX100's reordering/coalescing/interleaving is novel in
  Vortex because Vortex lacks it, and that row-locality gains measured against Vortex's
  current memory model mean anything.

Per the task's completion criterion: the missing capability that would justify simulation
is translation, not bandwidth. Simulating a DX100 port would mostly re-derive ISCA 2025 on
a weaker memory model.

*No code was modified. No module was implemented. No architecture is proposed beyond the
narrow hypothesis above, which is stated as a hypothesis and gated on measurement.*
