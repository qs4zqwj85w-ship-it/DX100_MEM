# Vortex GPU memory-access pipeline — reconnaissance report

**Subject:** `vortexgpgpu/vortex`, VORTEX_VERSION 3.0 (`VERSION:1`)
**Snapshot read:** commit `7300bc84981c34331b6247cc14500d8e07644988`, 2026‑11‑17
**Local read-only copy:** `DX100_MEM/ref/vortex/` (shallow clone, nothing built, nothing modified)

Everything below is from reading source. **No simulation, synthesis or elaboration was run
for this report**, so every number is a *parameter value or a formula evaluated by hand*,
not a measurement. Statements that I could not settle from source are collected in §6.

Two implementations exist and they are not the same code:

| Implementation | Path | Role |
|---|---|---|
| **RTL (authoritative for structure)** | `hw/rtl/` | The hardware. SystemVerilog, 457 files. |
| **SimX (C++ cycle-level)** | `sim/simx/` | The functional/timing model used for fast runs. Mirrors the RTL but is a separate implementation (`sim/simx/mem/cache.cpp` is 3,595 lines of hand-written model). |

Where the two differ I say so. `sim/rtlsim/` runs the actual RTL under Verilator;
`sim/common/dram_sim.*` + `sim/common/dram_bank_model.h` are shared by both.

---

## 1. Architecture diagram

Default configuration is `NUM_WARPS=4`, `NUM_THREADS=4` (`VX_config.toml:59-60`), so
`SIMD_WIDTH=4`, `ISSUE_WIDTH=1`, `NUM_LSU_LANES=4`, `NUM_LSU_BLOCKS=1`
(`VX_config.toml:70,129-130`). The bracketed numbers are that default's value.

```
 ── per warp ────────────────────────────────────────────────────────────────────
   GPR banks (NUM_GPR_BANKS=4)          VX_operands / VX_lane_dispatch
        │  rs1, rs2, rs3 collected
   VX_scoreboard      inuse_regs[] hazard + FU dispatch credit
        │             (VX_scoreboard.sv:130-215, 345-360)
 ── per core, per issue slice ───────────────────────────────────────────────────
   VX_lsu_block  [1 block]                                         VX_lsu_block.sv
     ├ VX_lsu_agu × NUM_LSU_LANES  [4]  base+offset | base+uop*stride, stack interleave
     ├ per-lane attr classify: io / local(LMEM) / om / flush / amo
     ├ tag = {lsu_header_t, op_type, align[2×2], pkt_slot, fence}   (LSU_CLIENT_TAG_WIDTH)
     ├ VX_allocator pkt_sop/pkt_eop/pkt_ctr  → multi-packet response reassembly
     ├ fence_lock : a fence blocks the block until its token returns
     └ VX_lsu_mem_path                                    VX_lsu_mem_path.sv
         ├ VX_lsu_scheduler → VX_mem_scheduler            VX_lsu_scheduler.sv
         │     req queue  (elastic, CORE_QUEUE_SIZE=4)
         │     pending table (PENDING_SIZE=8 read slots)   ← MLP depth of the core
         │     channel batching  (MERGED_REQS / MEM_BATCHES)
         ├ VX_lmem_switch ─ local ─→ VX_lmem_unit  [per core: 4 banks, 16 KB]
         │      │                            VX_lmem_engine: conflict-free passes + replay
         │      └ global
         ├ VX_mem_coalescer  [4 lanes × 4 B → 2 × 8 B]   libs/VX_mem_coalescer.sv
         │     fixed lane groups of DATA_RATIO; byte-level merge; 1 req outstanding
         └ VX_lsu_adapter  stream_unpack → DCACHE_CHANNELS [2] ports
        │
 ═══════ socket boundary ═══════════════════════════════════════════════════════
        │
   VX_mmu (d-side) + VX_tlb_l1 + VX_tlb_mshr           VX_mmu.sv, vm/VX_tlb_l1.sv
        │  VA→PA splice, perms, park on miss (hit-under-miss), BARE/IO/flush bypass
   VX_mem_bus_delay  L1_LINK_LATENCY [0]
        │
   VX_cache_cluster(NUM_UNITS=1 dcache, NUM_INPUTS=4 cores)   cache/VX_cache_cluster.sv
     └ VX_cache_wrap → VX_cache → VX_cache_bank[2]     cache/VX_cache.sv:577
           sel → S0 → S1 → stC        (+ S0 → stD data drive)
           VX_cache_mshr[16 per bank]  chains keyed {line, sector}, word-granular hazard
           VX_cache_bypass             non-cacheable io/om/flush
        │  L1_MEM_PORTS [2]
   socket memory arbiter (VX_mem_bus_arb) + delay   mem/VX_mem_bus_arb.sv, mem/VX_mem_bus_delay.sv
        │
 ═══════ cluster boundary ══════════════════════════════════════════════════════
        │
   VX_cache_wrap → L2   [1 MB, 8-way, MSHR 16, write-back iff LLC]
        │  L2_MEM_PORTS
   VX_membus_to_axi → AXI4 (FPGA/ASIC)          ── or ──  simx: MemCrossBar → DramSim
        │
 ═══════ platform ══════════════════════════════════════════════════════════════
   PLATFORM_MEMORY_NUM_BANKS [2] independent channels, HBM2, 64 B requests
        sim/common/dram_sim.h (Ramulator)  |  sim/common/dram_bank_model.h (bank model)
```

Side ports into the same hierarchy (not part of the load path but sharing its queues):

```
 DXA (per socket) ── gmem reads ─→ unit MMU ─→ socket mem arb ─→ L2    hw/rtl/dxa/
                 └─ smem writes → per-core LMEM DMA port
 TCU / TEX / RTU ── their own caches ─→ socket/cluster unit MMU ─→ L2
 CP DMA (host)  ──→ LLC as a client (VX_CFG_CP_ON_LLC, VX_config.toml:246-247)
```

---

## 2. Component-by-component source map

### 2.1 LSU — request generation (per core, per issue slice)

| Symbol | File:line | What it does |
|---|---|---|
| `VX_lsu_unit` | `hw/rtl/core/VX_lsu_unit.sv:16` | One LSU per core; `VX_lane_dispatch` fans warps to `NUM_LSU_BLOCKS` blocks; `VX_lane_gather` merges results back to `commit_if` |
| `VX_lsu_block` | `hw/rtl/core/VX_lsu_block.sv:16` | The per-block datapath: addresses, byte enables, store-data alignment, tag packing, fence lock, packet reassembly |
| `VX_lsu_agu` | `hw/rtl/core/VX_lsu_agu.sv:29` | Per-lane AGU. Two forms only: `base + sext(offset)`, and pack-load `base + uop_idx*stride` (`:38-45`). Also applies the per-thread stack word interleave |
| Address→`mem_req_addr` | `VX_lsu_block.sv:296-303` | Word address = `full_addr[MEM_ADDR_WIDTH-1:REQ_ASHIFT]`; `req_align` keeps the in-word offset |
| Byte enables | `VX_lsu_block.sv:305-327` | Built per lane from `inst_lsu_wsize` + `req_align` |
| Store data alignment | `VX_lsu_block.sv:334-357` | `rs2_data` shifted by `req_align` |
| Misalignment check | `VX_lsu_block.sv:329-332` | Runtime assertion: **misaligned accesses are illegal**, not split |
| Attribute classification | `VX_lsu_block.sv:141-232` | Per lane: `is_flush`, `is_noalloc`, `is_addr_io`, `is_addr_local` (LMEM window), `is_addr_om`, `amo` |
| Request tag | `VX_lsu_block.sv:72`, `:430-437`, `VX_gpu_pkg.sv:1965` | `{lsu_header_t, op_type, align, pkt_addr, is_fence}` → `LSU_CLIENT_TAG_WIDTH`; carries `wid`, `uuid`, `rd`, `tmask`, `sop/eop`, `pid` |
| Packet reassembly | `VX_lsu_block.sv:358-424` | `VX_allocator` over `VX_CFG_LSU_PENDING_SIZE` slots; `pkt_sop/pkt_eop/pkt_ctr` mark the first/last response of one instruction's multi-packet transfer |
| Fence | `VX_lsu_block.sv:250-262` | `fence_lock` set on fence `eop` issue, cleared when the fence token returns; blocks the whole block |
| Response formatting | `VX_lsu_block.sv:505-548` | Sign/zero extend, nan-box `flw`, per-lane `rsp_data[i]` from the returned word |

**Key property:** one warp instruction is one request, with a per-lane `mask`. There is **no
per-lane request splitting at issue** — the divergence handling happens downstream, in the
coalescer (by lane group) and the dcache banks.

### 2.2 Warp-level coalescing

| Symbol | File:line | Behaviour |
|---|---|---|
| `VX_mem_coalescer` | `hw/rtl/libs/VX_mem_coalescer.sv:17` | The only memory-coalescing block on the load path |
| Grouping | `:126-170` | Lanes are partitioned into **fixed groups of `DATA_RATIO` adjacent lanes** (`i*DATA_RATIO + j`). A group's seed is the lowest set lane in the group; another lane merges only if its **block address equals the seed's** (`:160-163`) |
| Disable per request | `:158-164` | `in_req_no_merge` (wired from the AMO attribute bit, `VX_lsu_mem_path.sv:181`) forces one-lane-per-request |
| Byte merge | `:186-205` | Per-byte `byteen`/`data` merge into the output word, at the lane's `in_addr_offset` |
| FSM | `:94`, `:207-236` | Two states, `STATE_WAIT`/`STATE_SEND`; a request that needs several output words is swept **one output word per cycle**, serially (`req_rem_mask_r`, `is_last_batch` `:192`) |
| Slot table | `:257-273` | `VX_index_buffer` of `QUEUE_SIZE` entries holding `{tag, pmask, offsets}` so responses can be unmerged |
| Miss counter | `:346-354` | Counts any send where `req_rem_mask_r != '1` (a partial transfer) |

Instantiated at `VX_lsu_mem_path.sv:149-211`, only when
`NUM_LSU_LANES > 1 && DCACHE_WORD_SIZE > LSU_WORD_SIZE`; otherwise the port is wired straight
through (`:212-227`).

**Consequence for irregular access:** the coalescer is *per LSU block*, **single outstanding**
(one instruction in flight through it), and merge eligibility is a function of *fixed
adjacent-lane grouping*, not of the address set. A warp whose active lanes are scattered
across words costs one output cycle per distinct output word, and blocks the block's next
memory instruction meanwhile.

### 2.3 Outstanding-request tracking, queues, batching

| Symbol | File:line | Scope | Depth |
|---|---|---|---|
| `VX_lsu_scheduler` | `hw/rtl/core/VX_lsu_scheduler.sv:28` | per LSU block | — |
| client arbiter (LSU \| TCU meta) | `:93-215` | per block | round-robin, 2 clients max today |
| `VX_mem_scheduler` | `hw/rtl/libs/VX_mem_scheduler.sv:17` | per LSU block | — |
| request queue | `:191-226` (`VX_elastic_buffer`) | per block | `CORE_QUEUE_SIZE` = `VX_CFG_LSU_QUEUE_IN_SIZE` [4] |
| pending table (read slots) | `:249-262` (`VX_index_buffer`) | per block | `PENDING_SIZE` = `VX_CFG_LSU_PENDING_SIZE` [8] |
| ready gating | `:229-231` | reads need a free slot, writes only need queue room | — |
| channel batching | `:44-52`, `:352-436` | splits `MERGED_REQS` over `MEM_CHANNELS`, one batch per cycle | `MEM_BATCHES` |
| memory-request buffer | `:443-458` | per block | `MEM_OUT_BUF` [0] |
| response remask | `:485-560` (`rsp_rem_mask`, `rsp_sop_r`) | per block | `PENDING_SIZE` |
| `VX_lsu_adapter` | `hw/rtl/mem/VX_lsu_adapter.sv:19` | per block | unpacks one wide request into `NUM_LANES` dcache ports, `REQ_OUT_BUF` [3] |
| `VX_allocator` packet slots | `VX_lsu_block.sv:358-380` | per block | `VX_CFG_LSU_PENDING_SIZE` |

Note: the scheduler's own coalescer is **disabled** in the LSU instantiation because
`LINE_SIZE == WORD_SIZE` is passed (`VX_lsu_scheduler.sv:227` → `COALESCE_ENABLE` false at
`VX_mem_scheduler.sv:49`). Memory-side line coalescing exists in the module but is off on this
path; line coalescing happens only in the dcache (`READ_MERGE`) and in the MSHR.

The pending table is the core's **memory-level parallelism limit**: `8` outstanding reads per
LSU block at the default config
(`VX_config.toml:139`: `min(LSU_PENDING_LIMIT, max(8, 2*NUM_WARPS*(SIMD/LSU_LANES)))`);
`LSU_PENDING_LIMIT` is derived from Little's law on the configured peak bandwidth and memory
latency and floors at 32 when the modelled latency is 0 (`VX_config.toml:141-143`).

### 2.4 L1 data cache

| Symbol | File:line | Detail |
|---|---|---|
| `VX_cache_cluster` | `hw/rtl/cache/VX_cache_cluster.sv:16` | **`NUM_UNITS` cache instances behind `NUM_INPUTS` core ports**; one round-robin arbiter per request index (`:93-145`) merges inputs onto instances and records the grant in the tag for the return path |
| `VX_cache_wrap` | `hw/rtl/cache/VX_cache_wrap.sv` | adds the NC-bypass layer in front of the cache |
| `VX_cache` | `hw/rtl/cache/VX_cache.sv:16` | parameters `:19-76`; `VX_cache_init` flush sequencer `:169`; banks `:577-578` |
| `VX_cache_bank` | `hw/rtl/cache/VX_cache_bank.sv` | pipeline `sel → S0 → S1 → stC`, plus `S0 → stD`; `PIPE_EX = LATENCY-2` extra deferral stages (`:108`); arbitration priority `init > replay > fill > flush > core-req` (`:352-358`) |
| `VX_cache_mshr` | `hw/rtl/cache/VX_cache_mshr.sv:17-42` | MSHR design note: allocate before the bank pipeline, release on hit, chain pending requests per line, replay the whole chain on fill |
| MSHR match | `:207` | `valid && entry_match && ~amo_mask && ~(being consumed this cycle)`; `entry_match` is `{line, sector}` |
| order hazards | `:526-535` | word-granular probes: a same-word hit is demoted to a chained miss if an older chained **write** is still pending, or (for a write) an older chained access exists |
| chain walk | `:547-560` | `dequeue_valid`, `dequeue_addr`, one entry per cycle |
| empty | `:622` | `~|valid_table` — the authoritative occupancy used by flush |
| `VX_cache_bypass` | `hw/rtl/cache/VX_cache_bypass.sv` | non-cacheable path for io/om/flush attributes |
| `VX_cache_tags/data/repl/flush/amo` | `hw/rtl/cache/` | tag array, data array, FIFO/PLRU/random replacement, flush walk, AMO + LR/SC reservations |

Defaults for the d-cache, evaluated from `VX_config.toml:272-326`:

* 16 KB, 4-way, line = sector = 64 B (`:272-273`, `:296-297`)
* `WRITEBACK = dcache_is_llc && single_core` → **write-through** whenever L2 is enabled (`:278`)
* `MSHR_SIZE` = clamp(bytes-in-flight per cache, 16, 64) → **16 per bank** (`:290`)
* `NUM_BANKS = pow2(clog2(min(DCACHE_NUM_REQS,16)))` = **2**; `BANK_HASH`, `SET_HASH` on (`:313-325`)
* `READ_MERGE = 1` — a same-word read arriving on several ports in the same cycle takes one
  bank access and answers all ports (`:321`)
* `NC_ENABLE = 1` for the d-cache (`VX_socket.sv:271`)
* `NUM_DCACHES = up(SOCKET_SIZE/4)` (`:310`) — with a 4-core socket that is **one shared d-cache
  per socket**, arbitrated between the cores. There is **no address hash** choosing an instance;
  inputs are statically partitioned across instances by the arbiter tree.

### 2.5 L2 and L3

| Symbol | File:line | Detail |
|---|---|---|
| L2 instance | `hw/rtl/VX_cluster.sv:225-267` | `VX_cache_wrap` with `CACHE_SIZE=L2_SIZE`, `SECTOR_SIZE=L2_SECTOR_SIZE`, `NUM_REQS=L2_CACHE_REQS` |
| L2 inputs | `VX_cluster.sv:188-191` | one bus per socket port, `per_socket_mem_bus_if[L2_CACHE_REQS]` |
| L2 link delay | `VX_cluster.sv:310` | `L2_LINK_LATENCY` [0] |
| defaults | `VX_config.toml:330-350` | 1 MB, 8-way, line 128 B / sector 64 B, MSHR 16, write-back iff `l2_is_llc && single_cluster` |
| L3 | `VX_config.toml:352-368` | optional, 2 MB, always global, write-back iff L3 enabled |
| coherence point | `VX_config.toml:236-240` | `IS_LLC`/`SHARED_LLC`/`NEXT_IS_POC` flags decide where acquire/release stops; write-back is only legal at the single coherence point |

The L2 is a **banked sectored cache with its own MSHRs**; it is not a memory-side coalescer and
has no address-based "distribution" to L2 instances — there is one L2 per cluster.

### 2.6 Memory controller and DRAM

| Symbol | File:line | Detail |
|---|---|---|
| `DramSim` | `sim/common/dram_sim.h:23-63` | One **independent controller per platform memory bank**; `can_accept` / `send_request` / `tick` |
| bank model | `sim/common/dram_bank_model.h:65-84` | `Config{geom, t, refresh, read_queue=32, write_queue=32, wr_high=26, wr_low=7, horizon=448, bursts}` |
| bank model semantics | `dram_bank_model.h:29-62` | Reservation scheduling: PRE/ACT/COL placed at the first legal DRAM clock, rows kept open, per-bank FCFS, write drain at high/low watermarks, refresh optional, `horizon` bound |
| address interleave | `sim/simx/mem/memory.cpp:60-72` | `(addr >> log2(block_size)) & (num_banks-1)` → channel; then `/num_banks` inside the channel |
| platform config | `VX_config.toml:91-107` | 2 banks, 64 B, `PEAK_BW=460000`, `CLOCK_RATE=400`, `TYPE=HBM2`, `DATA_RATE=2*clock`, `LATENCY=0`, `QUEUE_SIZE=0` (unbounded) |
| RTL AXI bridge | `hw/rtl/mem/VX_membus_to_axi.sv`, `Vortex_axi.sv` | the RTL has no DRAM model; it drives AXI4 out of the chip |

**There is no reorder buffer, no read/write scheduler and no MC arbitration in RTL** — those
exist only in the simulator's `DramSim`/`DramBankModel`. In RTL the queuing is whatever the AXI
interconnect provides. With the default `LATENCY=0` and unbounded queue the simulator models
**no fixed DRAM read latency and no channel backpressure**; only the JEDEC bank timing applies.

### 2.7 TLB, MMU, address translation

| Symbol | File:line | Detail |
|---|---|---|
| `VX_mmu` | `hw/rtl/vm/VX_mmu.sv:24` | Per-client MMU: one instance per L1 cache (socket), one per unit cache. Pipeline: per-lane VPN probe → hit splice → park on miss |
| bypass | `VX_mmu.sv:187` | `req_bypass = ~vm_active | BARE | flush | io | om` — skips translation entirely |
| park / replay | `VX_mmu.sv:200`, `:248` | a missing lane parks with its payload; the lane keeps going (**hit-under-miss**), the fill replays it |
| `VX_tlb_l1` | `hw/rtl/vm/VX_tlb_l1.sv:24` | fully-associative CAM + miss station (`VX_tlb_mshr`), replay depth 2 |
| `VX_tlb_cam` | `hw/rtl/vm/VX_tlb_cam.sv` | parallel per-lane lookup, permission check, install/evict |
| `VX_tlb_mshr` | `hw/rtl/vm/VX_tlb_mshr.sv` | non-blocking miss stations, dedup of live misses, kill on fault |
| `VX_tlb_l2` + `VX_ptw*` | `hw/rtl/vm/VX_tlb_l2.sv`, `VX_ptw*.sv` | shared cluster TLB + page-table walker complex, walk cache, 2 walkers |
| `VX_mmu_snoop` | `hw/rtl/vm/VX_mmu_snoop.sv` | SFENCE.VMA / satp visibility to the L1 TLBs |
| depths | `VX_config.toml:199-227` | DTLB 16, ITLB 8, L1 TLB MSHR 4, unit TLB 8, cluster unit TLB 8, **L2 TLB 512/4-way**, L2 TLB MSHR 8, 2 walkers, walk cache 16 |

**Every global access is translated at the socket**, before the d-cache: the cache is physically
indexed and tagged (`VX_socket.sv:139-146` core-side vs `:146-152` cache-side buses). Address
translation is therefore **not** on the LSU side and cannot be bypassed per-request except by the
attribute bits above.

### 2.8 Warp scheduler, scoreboard, memory-dependency handling

| Symbol | File:line | Detail |
|---|---|---|
| `VX_warp_sched` | `hw/rtl/core/VX_warp_sched.sv:18` | per frontend slice; warp state (`active`, `stalled`, `tmask`, `pc`), split/join stacks, warp selection |
| `VX_scoreboard` | `hw/rtl/core/VX_scoreboard.sv:16` | per warp: `inuse_regs`/`inuse_xregs` (write-hazard bitmap), operand busy check `:170-186`, per-FU dispatch credit `:47-61`, greedy arbiter `:345-360` |
| hazard resolution | `VX_scoreboard.sv:170-186` | issue only when no operand reads an in-use register; `rd` is reserved at issue (`:145-152`) and released on the writeback whose `eop_wis[w]` matches (`:131-140`) |
| retirement | `hw/rtl/core/VX_commit_slice.sv:22-60` | the slice arbitrates its FUs' results onto one writeback port (fixed priority FPU first) and reports `committed_wis` |
| `VX_commit` | `hw/rtl/core/VX_commit.sv:47` | combines the issue slices' writeback |

There is **no reorder buffer**. A warp's instructions issue in program order, `rd` is reserved at
issue, and results write back in FU-completion order; correctness rests on the scoreboard
reservation, not on ordered retirement. A load therefore blocks only its dependents — but a
**second load to the same `rd`** cannot issue until the first writes back, and the warp's next
`ld` cannot pass a `st` to the same address because both go through the same in-order block.

### 2.9 Interconnect

| Symbol | File:line | Scope |
|---|---|---|
| `VX_mem_bus_if` | `hw/rtl/mem/VX_mem_bus_if.sv:17` | the packet: `{rw, addr, data, byteen, attr, tag}` + valid/ready both ways |
| `VX_lsu_mem_if` | `hw/rtl/mem/VX_lsu_mem_if.sv:17` | wide variant: per-lane `mask/addr/data/byteen/user` — the LSU-internal bus |
| `VX_mem_bus_arb` | `hw/rtl/mem/VX_mem_bus_arb.sv:16` | round-robin N:M, grant recorded in the tag via `VX_bits_insert` |
| `VX_stream_arb` | `hw/rtl/libs/VX_stream_arb.sv:17` | the primitive; with `NUM_INPUTS > NUM_OUTPUTS` inputs are **partitioned** across outputs, each output arbitrating its own slice |
| `VX_mem_bus_delay` | `hw/rtl/mem/VX_mem_bus_delay.sv` | configurable link latency |
| `VX_mem_bus_switch` | `hw/rtl/mem/VX_mem_bus_switch.sv` | tag-directed 1:N (used for DXA→core LMEM routing, `VX_socket.sv:835`) |
| `VX_mem_xbar` / `VX_mem_axi_xbar` | `hw/rtl/mem/` | full crossbar and AXI crossbar |
| simx side | `sim/simx/mem/memory.cpp:60-72` | `MemCrossBar` with round-robin and the address hash above |

Link latencies are knobs, all **0 by default**: `L1_LINK_LATENCY` (`VX_config.toml:306`),
`L2_LINK_LATENCY` (`:341`).

### 2.10 Shared (local) memory

| Symbol | File:line | Detail |
|---|---|---|
| `VX_lmem_unit` | `hw/rtl/mem/VX_lmem_unit.sv:35` | per core: `SIZE` bytes, `NUM_BANKS` banks, `NUM_BLOCKS` LSU ports, optional DMA port |
| engines | `VX_lmem_unit.sv:304`, `:364` | one pass engine, plus a **second read-only engine** when `NUM_BLOCKS > 1` so two warps' reads go through per cycle (`:20-31`) |
| `VX_lmem_engine` | `hw/rtl/mem/VX_lmem_engine.sv:16-36` | one warp request → **conflict-free passes**: pairwise `same_bank`/`same_word` matrices computed once per request; each pass takes one lane per bank; conflicting lanes replay; same-word reads broadcast; responses 4 cycles after a pass |
| defaults | `VX_config.toml:372-383` | `NUM_BANKS = NUM_LSU_LANES`, size `max(16 KB, banks*2 KB)` = **16 KB** at 4 banks |
| DMA port | `VX_lmem_unit.sv:35-62` | whole-bank-row port for DXA writes and TCU reads; DMA gets the banks the cycle after it is seen |

LMEM is the **only** memory in the design with explicit bank-conflict detection and replay. The
global path has none: it relies on the coalescer, `READ_MERGE`, and the MSHRs.

### 2.11 The bulk engine that already exists: DXA

This is important for the DX-100 question: Vortex **already contains** a descriptor-driven bulk
transfer engine, `hw/rtl/dxa/`, per socket (`VX_socket.sv:800-819`).

| Symbol | File:line | Detail |
|---|---|---|
| `VX_dxa_core` | `hw/rtl/dxa/VX_dxa_core.sv:16` | descriptor table → request queue → dispatch → `NUM_DXA_CORES` workers → gmem arbiter + smem arbiter |
| descriptor | `hw/rtl/dxa/VX_dxa_pkg.sv:82-105` | `{base_addr, meta, tile01, tile23, tile4, cfill, size0..4, stride0..3, smem_stride, estride2}` |
| meta encoding | `VX_dxa_pkg.sv:41-62` | `dim(1-5), elemsz, layout, swizzle, interleave, l2promo` |
| address generator | `hw/rtl/dxa/VX_dxa_addr_gen.sv:16-30` | 1D–5D tile iteration by nested odometer counters; inner loop is `CL`-granular adds only; outer dims use precomputed deltas |
| destination layouts | `VX_dxa_pkg.sv:107-182` | row-major, K-major, and two WGMMA-native scatter layouts (Flat/BlockMajor) |
| multicast | `hw/rtl/dxa/VX_dxa_smem_wr.sv:78-80`, `:618-641` | one transfer broadcast to every CTA in `cta_mask`, with `smem_stride` |
| dedup counter | `hw/rtl/dxa/VX_dxa_worker.sv:315-341` | `perf.gmem_dedup` |
| parameters | `VX_config.toml:425-430` | `NUM_DXA_CORES = max(1, up(NUM_CORES/8))`, queue 16, `MAX_INFLIGHT 8` |

So the existing accelerator covers **regular, strided, multi-dimensional** gmem→lmem transfers
with multicast and tensor-friendly destination layouts. What it does **not** consume is a
per-element index list: its addressing is `base + Σ tile_dim·stride` (`VX_dxa_addr_gen.sv:16-30`),
with no indirection table and no scatter to global memory (it writes LMEM only,
`VX_dxa_core.sv:190-230`).

Also relevant: the only "gather" in the ISA is **`WGATHER`**, and it is a *register* cross-lane
permute executed in the ALU (`VX_decode.sv:918-930`, `VX_alu_int.sv:162-190`,
`tests/regression/wgather/kernel.cpp:1-20`) — groups of 4 lanes, one lane's `rs1/rs2/rs3`
broadcast to the group. It touches no memory.

---

## 3. Detailed load request lifecycle

A single `ld.global` from issue to register writeback, default configuration.

**1. Operand availability.** `VX_scoreboard` holds `inuse_regs` per warp; the instruction stages
until no source reads an in-use register and the LSU's dispatch credit is available
(`VX_scoreboard.sv:170-186`, `:47-61`). On issue, `rd` is set in `inuse_regs`
(`:145-152`).

**2. Dispatch to the LSU block.** `VX_lsu_unit`'s `VX_lane_dispatch` routes the warp to its block
(`VX_lsu_unit.sv:88-101`); the operands (`rs1`, `rs2`, `op_args`) arrive on
`lsu_execute_t` (`VX_lsu_block.sv:65-70`).

**3. Address generation.** One `VX_lsu_agu` per lane computes `full_addr[i] = rs1 + sext(offset)`
(`VX_lsu_agu.sv:38-53`) and `lin_addr[i]` (pre-interleave) for window classification.

**4. Attribute classification.** Per lane, `mem_bus_attr_t` gets `is_flush`, `is_noalloc`,
`is_addr_io`, `is_addr_local` (LMEM window compare, `VX_lsu_block.sv:184-187`), `is_addr_om`, and
the AMO sideband (`:139-232`). Misalignment is a runtime assertion, not a split
(`:329-332`).

**5. Request formation.** `mem_req_mask = tmask`, `mem_req_rw = is_store`, addresses word-aligned,
byte enables and store data aligned (`VX_lsu_block.sv:266-357`).

**6. Token allocation.** For a read, `pkt_waddr` is allocated from the packet allocator
(`:358-380`); the tag is packed with `pkt_waddr` (`:426-433`). A store with `wb=0` and a fence
skipping lane bypass the response path entirely (`:262-282`).

**7. Block-level admission.** `client_if.req_valid` is offered to `VX_lsu_scheduler`
(`:466-473`) and accepted only when the block's path is ready; a fence holds the block
(`:250-262`).

**8. Scheduler queueing.** `VX_mem_scheduler` pushes into the elastic request queue
(`VX_mem_scheduler.sv:191-226`) and, for a read, allocates a **pending slot**
(`:249-262`), returning `core_req_ready_rd` only when a slot is free (`:230`). The slot id is
written into the tag (`:196-206`) and is what makes the response routable.

**9. Local vs global.** `VX_lmem_switch` splits on `is_addr_local`
(`VX_lsu_mem_path.sv:124-141`). LMEM requests leave the global path here entirely.

**10. Warp-level coalescing.** `VX_mem_coalescer` merges the active lanes into
`DCACHE_WORD_SIZE` words, one output word per cycle, recording `{tag, pmask, offsets}` in its
index buffer (`VX_mem_coalescer.sv:207-273`).

**11. Port unpacking.** `VX_lsu_adapter`'s `VX_stream_unpack` emits one dcache-word request per
granted port (`VX_lsu_adapter.sv:45-80`).

**12. Address translation.** In the socket, `VX_mmu` probes the L1 TLB for `DCACHE_NUM_REQS`
lanes in parallel (`VX_mmu.sv:160-200`). Hit → PPN spliced, permissions checked, forwarded.
Miss → lane parks in the miss station (`:200`, `VX_tlb_l1.sv:150-190`), the miss goes over
`tlb_bus` to the L2 TLB/walker, and the lane **continues** (hit-under-miss); the fill replays it.
BYPASS attributes (`BARE`, io, om, flush) skip the TLB (`VX_mmu.sv:187`).

**13. Link to the d-cache.** `VX_mem_bus_delay` of `L1_LINK_LATENCY` (`VX_socket.sv:222-236`),
then `VX_cache_cluster`'s per-index round-robin arbiter (`VX_cache_cluster.sv:93-145`).

**14. Cache lookup and miss handling.** In the bank: `sel → S0` (tag/data lookup, MSHR allocate)
`→ S1` (way select, commit decision) `→ stC` (`VX_cache_bank.sv:124-135`, `:1044`,
`:1135`).
 * MSHR match on `{line, sector}` with no word-order hazard → the request **chains behind** the
   existing miss and releases at the pipeline stage (`VX_cache_mshr.sv:207`, `:547`).
 * Hit → MSHR slot released, data returned (`VX_cache_mshr.sv:262-280`).
 * Miss → the MSHR entry persists; the fill requests the sector from the next level
   (`VX_cache_bank.sv` fill path), and on fill the **whole chain replays** in arrival order
   (`VX_cache_mshr.sv:17-42`, `:547-560`).
 * A same-word hit behind a pending chained write is **demoted to a chained miss** to preserve
   arrival order (`VX_cache_mshr.sv:526-535`).
 * Non-cacheable / io: taken by `VX_cache_bypass` instead.

**15. L1 miss → L2 → memory.** Bank memory requests are formed sector-granular and arbitrated to
`L1_MEM_PORTS` (`VX_cache.sv:577-900`), cross the link, enter the cluster's per-socket bus
(`VX_cluster.sv:188-191`) and the L2 (`:225-267`). An L2 miss allocates its own MSHR and issues
to the platform through `VX_membus_to_axi` (RTL) or `MemCrossBar`→`DramSim` (simx,
`sim/mem/memory.cpp:60-72`). The DRAM model schedules PRE/ACT/COL per channel and calls back
(`dram_bank_model.h:29-62`, `:246-330`).

**16. Response, in reverse.** Memory → L2 → cluster bus → d-cache fill (broadcast to the MSHR
chain) → d-cache response → socket (MMU replay splice if the lane was parked) → adapter's
`VX_stream_pack` (`VX_lsu_adapter.sv:84-104`) → coalescer unmerge using the index buffer
(`VX_mem_coalescer.sv:300-340`) → scheduler's `rsp_rem_mask`/`rsp_sop_r` accounting
(`VX_mem_scheduler.sv:485-560`) → block: `pkt_ctr/sop/eop` update
(`VX_lsu_block.sv:380-424`) → sign/zero extend (`:505-548`) → `VX_elastic_buffer` → result
(`:555-600`) → `VX_lane_gather` → `commit_slice` → writeback.

**17. Release.** `VX_scoreboard` clears `inuse_regs[rd]` on the writeback beat with
`eop_wis[w]` set (`VX_scoreboard.sv:131-140`), and the LSU's pending slot is released on the
response's `eop` (`VX_mem_scheduler.sv:245`).

**Reordering, precisely.** Responses may return in any order across distinct pending slots — the
tag carries the slot id and the packet id. Within one warp instruction, multi-packet responses
are reassembled by `pkt_sop/pkt_eop/pkt_ctr` at the block, so the instruction completes as a
unit. Per-warp software order is preserved by issue order + `rd` reservation, not by an ROB.

---

## 4. Existing mechanisms and their scope

| Mechanism | Where | Scope | Notes |
|---|---|---|---|
| Per-lane AGU (`base+offset`, pack `base+uop*stride`) | `VX_lsu_agu.sv:29` | per lane | only two address forms; no index load |
| Stack word interleave | `VX_lsu_agu.sv:53`, `VX_config.toml:133-134` | per thread | spreads per-thread stack frames over one line |
| Warp coalescing (byte-level) | `VX_mem_coalescer.sv:17` | **per LSU block**, single outstanding | fixed adjacent-lane groups of `DATA_RATIO` |
| Fixed-lane-group merge | `VX_mem_coalescer.sv:126-170` | per block | merge is a function of lane index and block address, not of the address set |
| Pending/outstanding pool | `VX_mem_scheduler.sv:249` | per LSU block | `LSU_PENDING_SIZE` [8]; the MLP limit |
| In-order request queue | `VX_mem_scheduler.sv:191` | per block | `CORE_QUEUE_SIZE` [4] |
| Packet (multi-beat) reassembly | `VX_lsu_block.sv:358` | per block | `LSU_PENDING_SIZE` slots |
| Fence lock | `VX_lsu_block.sv:250` | per block | blocks all warps of the block until the token returns |
| D-cache read merge | `VX_cache.sv:65`, `VX_config.toml:321` | per cache | same word, same cycle, one bank access |
| MSHR chaining | `VX_cache_mshr.sv:207` | **per bank** | `{line,sector}`; `MSHR_SIZE` [16]; shared by every core on that cache |
| MSHR order-hazard probes | `VX_cache_mshr.sv:526-535` | per bank | word-granular demotion of hits |
| Non-cacheable bypass | `VX_cache_bypass.sv` | per cache | io/om/flush |
| Lazy fetch (write-validate) | `VX_cache.sv:50-65` | LLC only | on only where write-back is legal; off by default |
| Per-lane TLB + hit-under-miss | `VX_mmu.sv:187-200` | per L1 cache (socket) | 16-entry DTLB |
| Shared L2 TLB + 2 walkers | `VX_tlb_l2.sv`, `VX_ptw.sv` | **per cluster** | 512 entries |
| LMEM conflict-free passes + replay | `VX_lmem_engine.sv:16-36` | **per core** | explicit bank-conflict resolution |
| LMEM same-word broadcast | `VX_lmem_engine.sv:66` | per core | one bank read answers all lanes of the word |
| LMEM second (read-only) engine | `VX_lmem_unit.sv:364` | per core | two warps' reads per cycle |
| LMEM DMA port | `VX_lmem_unit.sv:35` | per core | DXA/TCU whole-bank-row access |
| DXA bulk 1–5D transfer | `VX_dxa_core.sv:16` | **per socket** | descriptor-driven, gmem→lmem only |
| DXA multicast to CTAs | `VX_dxa_smem_wr.sv:78` | per socket | `cta_mask` |
| Scoreboard hazard + FU credit | `VX_scoreboard.sv:16` | per warp / per slice | no ROB |
| Address-hashed bank/set select | `VX_config.toml:317-320` | per cache | `BANK_HASH`, `SET_HASH` |

Absent from the load path (verified by the parameter set and the module list): any prefetcher,
any memory-side gather/scatter unit, any per-warp MSHR partition, any address-based steering
between cache instances, any out-of-order load issue.

---

## 5. Configurable architectural parameters

All values below are from `VX_config.toml` at this snapshot; `expr:` entries are evaluated by the
config generator, and I have written the value the default 4-warp/4-thread build produces.

| Parameter | Line | Default / formula | Meaning |
|---|---|---|---|
| `VX_CFG_NUM_WARPS` | 59 | 4 | warps per core |
| `VX_CFG_NUM_THREADS` | 60 | 4 | lanes per warp (= `SIMD_WIDTH`) |
| `VX_CFG_ISSUE_WIDTH` | 67 | `pow2(clog2(up(WARPS/16)))` = 1 | issue slices per core |
| `VX_CFG_NUM_LSU_LANES` | 129 | `SIMD_WIDTH` = 4 | lanes per LSU block |
| `VX_CFG_NUM_LSU_BLOCKS` | 130 | `ISSUE_WIDTH` = 1 | LSU blocks per core |
| `VX_CFG_LSU_LINE_SIZE` | 136 | `min(lanes*XLEN/8, L1_LINE)` = 16 B | LSU-side granule |
| `VX_CFG_LSU_QUEUE_IN_SIZE` | 137 | 4 | per-block request queue |
| `VX_CFG_LSU_PENDING_SIZE` | 139 | 8 | **outstanding read slots per block** |
| `VX_CFG_LSU_PENDING_LIMIT` | 143 | 32–256, from Little's law | bound on the pool |
| `VX_CFG_LSU_STACK_INTERLEAVE_ENABLE` | 134 | true | word-interleave thread stacks |
| `VX_CFG_MEM_BLOCK_SIZE` | 78 | 64 B | memory bus / fill granule |
| `VX_CFG_L1_LINE_SIZE` | 85 | = `MEM_BLOCK_SIZE` = 64 B | L1 line = sector |
| `VX_CFG_L2_LINE_SIZE` / `_SECTOR_SIZE` | 86-87 | 128 B / 64 B | L2 sectored |
| `VX_CFG_DCACHE_SIZE` | 272 | 16 KB | |
| `VX_CFG_DCACHE_NUM_WAYS` | 273 | 4 | |
| `VX_CFG_DCACHE_WORD_SIZE` | 277 | 8 B | coalescer/bank granule |
| `VX_CFG_DCACHE_WRITEBACK` | 278 | 0 (write-through, since L2 on) | |
| `VX_CFG_DCACHE_MSHR_SIZE` | 290 | 16 | **per bank** |
| `VX_CFG_DCACHE_LATENCY` | 299 | `2 + max(0, clog2(size/16K))` = 2 | bank pipeline depth |
| `VX_CFG_DCACHE_MRSQ_SIZE` / `_MREQ_SIZE` / `_CRSQ_SIZE` | 307-309 | 4 / 0 / 0 | memory-response queue, mem-req, core-response queue |
| `VX_CFG_NUM_DCACHES` | 310 | `up(SOCKET_SIZE/4)` = 1 | d-cache instances per socket |
| `VX_CFG_DCACHE_NUM_BANKS` | 313 | `pow2(clog2(min(NUM_REQS,16)))` = 2 | |
| `VX_CFG_DCACHE_BANK_HASH` / `_SET_HASH` | 317-320 | 1 / 1 | |
| `VX_CFG_DCACHE_READ_MERGE` | 321 | 1 | same-word merge |
| `VX_CFG_L1_MEM_PORTS` | 326 | `min(banks, PLATFORM_MEMORY_NUM_BANKS)` = 2 | |
| `VX_CFG_L1_LINK_LATENCY` | 306 | 0 | core↔dcache link, each way |
| `VX_CFG_L2_SIZE` | 331 | 1 MB | |
| `VX_CFG_L2_NUM_WAYS` | 332 | 8 | |
| `VX_CFG_L2_MSHR_SIZE` | 337 | 16 | per bank |
| `VX_CFG_L2_LATENCY` | 340 | 4 | bank pipeline |
| `VX_CFG_L2_LINK_LATENCY` | 341 | 0 | socket↔L2, each way |
| `VX_CFG_L2_NUM_BANKS` | 349 | `pow2(clog2(min(L2_NUM_REQS,16)))` | |
| `VX_CFG_L3_*` | 352-368 | 2 MB, 8-way, optional | third level |
| `VX_CFG_DTLB_SIZE` / `_ITLB_SIZE` | 203-204 | 16 / 8 | per L1 |
| `VX_CFG_L1_TLB_MSHR_SIZE` | 205 | 4 | per L1 |
| `VX_CFG_L2_TLB_SIZE` / `_MSHR_SIZE` | 216-219 | 512 / 8 | per cluster |
| `VX_CFG_PTW_NUM_WALKERS` / `_WALK_CACHE_SIZE` | 221-222 | 2 / 16 | |
| `VX_CFG_LMEM_NUM_BANKS` | 379 | = `NUM_LSU_LANES` = 4 | |
| `VX_CFG_LMEM_SIZE` | 383 | `max(16 KB, banks*2 KB)` = 16 KB | |
| `VX_CFG_PLATFORM_MEMORY_NUM_BANKS` | 92 | 2 | channels |
| `VX_CFG_PLATFORM_MEMORY_TYPE` | 105 | HBM2 | JEDEC timing set |
| `VX_CFG_PLATFORM_MEMORY_DATA_RATE` | 106 | `2*400` = 800 MT/s | |
| `VX_CFG_PLATFORM_MEMORY_LATENCY` | 107 | 0 | fixed read latency added |
| `VX_CFG_PLATFORM_MEMORY_QUEUE_SIZE` | 108 | 0 | in-flight requests per channel, 0 = unbounded |
| `VX_CFG_NUM_DXA_CORES` | 427 | `max(1, up(CORES/8))` | DXA workers |
| `VX_CFG_DXA_QUEUE_SIZE` / `_MAX_INFLIGHT` | 429-430 | 16 / 8 | |

---

## 6. Uncertainties — what source inspection alone does not settle

1. **RTL vs SimX equivalence.** The two are separate implementations (the C++ cache is 3,595
   lines). `ci/check_simx_mt_boundary.sh` and `ci/check_config_boundary.sh` exist, and
   `docs/simulation.md:82-93` warns that driver-reported cycle counts are quantised. I did not
   verify, and could not verify by reading, that SimX timing matches Verilated RTL for the memory
   path. Any cycle number taken from SimX should be treated as model output.
2. **Real DRAM latency.** The default `PLATFORM_MEMORY_LATENCY = 0` and `QUEUE_SIZE = 0` mean the
   default simulation has no PHY/controller latency and no channel backpressure beyond JEDEC bank
   timing. What a real board's L2-miss latency is cannot be derived from this source.
3. **Which AXI memory model the RTL run uses.** `hw/syn`/`sim/rtlsim` select a memory model
   (`AXI_MEM_MODEL=bank` mentioned in `dram_bank_model.h:44-46`); the interconnect and memory
   model in an FPGA/ASIC build is outside this repo's RTL, so the *system* latency above the AXI
   port is not determinable here.
4. **Cross-warp ordering guarantees.** I can see the mechanisms (fence lock per block, cache
   flush walk, `NEXT_IS_POC`/`IS_LLC` flags) but I cannot derive from source what the
   *architecturally guaranteed* ordering between warps of different blocks/cores is — that is an
   ISA/memory-model question, and `docs/` did not contain a memory-consistency specification that
   I found.
5. **`perf.gmem_dedup` semantics.** The counter exists (`VX_dxa_worker.sv:341`) but I did not read
   the worker's dedup logic closely enough to state what it deduplicates (multicast
   fan-out vs. repeated addresses). Treat it as "a dedup counter exists".
6. **DCACHE_COUNT vs. cores when `SOCKET_SIZE % 4 != 0`.** `VX_CFG_NUM_DCACHES =
   up(SOCKET_SIZE/4)` (`VX_config.toml:310`) with `NUM_INPUTS = SOCKET_SIZE` means a socket of 5–8
   cores gets 2 instances and a static partition of input ports; I did not verify the resulting
   bandwidth split for non-multiples of 4 beyond reading the arbiter partitioning.
7. **Where the `NUM_BATCHES` channel split lands physically.** The scheduler emits `MEM_BATCHES`
   batches over `NUM_CHANNELS = NUM_LSU_LANES` channels (`VX_lsu_scheduler.sv:227`), but the
   adapter then repacks per `DCACHE_CHANNELS`; the exact port-to-bank mapping across that boundary
   is spread over three modules and I did not trace every tag bit.
8. **The cache unit test has no stimulus.** `hw/unittest/cache/main.cpp` is 37 lines: it calls
   `Verilated::commandArgs` and returns 0. `mem_unit/main.cpp` and `lmem_unit/main.cpp` are 19
   lines each. Whatever `hw/unittest/*/Makefile` builds, these are **not** self-checking tests of
   the memory path. The real RTL coverage is in `tests/regression/` run through `ci/blackbox.sh`
   (see §7).

---

## 7. Observations relevant to the DX-100 question

Stated as facts about this codebase, not as a design recommendation.

1. **A bulk descriptor engine already exists** — DXA (§2.11). It is per socket, decoupled from the
   LSU, writes only LMEM, supports 1–5D strided tiles, multicast to a CTA mask, and tensor-native
   destination layouts. Anything proposed for "bulk indirect access" has to justify itself
   *relative to DXA*, not relative to an empty die.
2. **The only missing piece is indirection.** DXA's address generator is a rolling cursor over
   strides (`VX_dxa_addr_gen.sv:16-30`); there is no per-element index vector anywhere in the
   memory subsystem, and no scatter direction (gmem write) from any engine except the LSU's own
   stores.
3. **The LSU's irregular-access cost is structural, in two places:**
   * the coalescer is per block, single-outstanding, and merges only within fixed groups of
     `DATA_RATIO` adjacent lanes (`VX_mem_coalescer.sv:126-170`, `:207-236`);
   * outstanding capacity is `LSU_PENDING_SIZE` = 8 read slots per block
     (`VX_config.toml:139`), shared by all warps of the block.
   Neither is a cache-capacity or bandwidth limit; both are fixed per-block state.
4. **Miss handling is line/sector-granular and per bank**, so an irregular pattern that touches
   many distinct sectors per warp instruction converts directly into MSHR entries and replay
   chains (`VX_cache_mshr.sv:207`, `:547`). `MSHR_SIZE` is 16 per bank, 2 banks, i.e. 32 in-flight
   misses for the whole socket's d-cache at this configuration.
5. **LMEM already has the "many lanes, conflicting banks, replay" machinery** that a scatter
   engine needs on the destination side (`VX_lmem_engine.sv:16-36`), including a DMA port sized to
   all banks in one request (`VX_dxa_pkg.sv:20-22`).
6. **Address translation sits between the core and the L1** (`VX_socket.sv:139-152`), so any new
   engine that reads global memory must either go through an MMU port or be handed physical
   addresses; DXA takes the former (`VX_socket.sv:862-880`, unit MMU).

---

## 8. Test and example inventory

| Path | What it is |
|---|---|
| `hw/unittest/mem_scheduler/` | `memsim.cpp` (160 lines) + `ram.cpp`: a C++ harness driving the memory scheduler against a simple RAM. The most substantive unit harness in the memory area. |
| `hw/unittest/cache/`, `mem_unit/`, `lmem_unit/` | Verilator tops whose `main.cpp` only initialises Verilator (§6.8) — **no stimulus** |
| `hw/unittest/dxa_core/` | DXA core unit top |
| `tests/regression/` | ~90 kernels run through `ci/blackbox.sh`; memory-relevant ones: `lmem_access`, `mstress`, `ldst_order`, `ldst_wide`, `uniform_ld`, `wgather`, `fence`, `fence_mp`, `flush_coherence`, `lazy_fetch`, `sector_merge`, `cache_ways`, `io_addr`, `vm_test`, `vm_stress`, `vm_fault`, `dxa_copy`, `dxa_copy_mcast`, `dxa_kmajor_check`, `sgemm2_dxa`, `sgemm_tcu_wg_dxa_mcast` |
| `tests/unittest/dram_sim/`, `tests/unittest/dram_bank_model/` | standalone DRAM-model tests |
| `ci/blackbox.sh` | the driver entry point (`docs/simulation.md:130-165`): `--driver={simx,rtlsim,fpga,...}`, `--clusters/--cores/--warps/--threads`, `--app` |
| `docs/simulation.md`, `docs/codebase.md`, `docs/designs/` | prose documentation; `docs/simulation.md` is the one that describes drivers and their caveats |

Example invocation (from `docs/simulation.md:162`), **not run**:

```
./ci/blackbox.sh --driver=simx --clusters=1 --cores=4 --warps=4 --threads=4 --args="path-to-test-file"
```

---

## 9. Summary of what was and was not established

**Established by reading source, with citations above:** the module hierarchy and every queue and
table on the load path; where requests are formed, merged, queued and replayed; which structures
are per-lane, per-warp, per-block, per-cache, per-socket, per-cluster or per-channel; the exact
parameter set and its default values; the presence of a real descriptor-driven bulk engine (DXA)
and its exact scope; the absence of a prefetcher, a memory-side gather, or per-warp MSHR
partitioning.

**Not established:** any performance number (nothing was simulated); RTL/SimX cycle equivalence;
the system-level memory latency above the AXI port; the formal cross-warp ordering contract; the
semantics of the DXA dedup counter; the exact port→bank mapping across the scheduler/adapter
boundary.
