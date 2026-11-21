# Task 3 — Design of a controlled experiment: does a GPU-native bulk indirect-access engine expose MLP beyond SIMT?

**Status: design only. No simulator code was written. This document is submitted for review.**

Snapshot under study: `DX100_MEM/ref/vortex` at `7300bc84981c34331b6247cc14500d8e07644988`
(VORTEX_VERSION 3.0). All baseline values marked *(verified)* were read from that tree in
this session; all values marked *(recon)* come from `vortex-memory-recon.md` (Task 1).
DX100 section references are to arXiv:2505.23073v2.

---

## 0. Purpose and scope

### 0.1 The question

> Can separating bulk indirect-address generation from SIMT execution expose substantially
> more independent future memory operations than conventional GPU execution — **even after
> the baseline is given larger outstanding-request queues and an idealized larger scheduling
> window**?

The second clause is the whole experiment. Everything in §6 exists to make the comparison
admissible; §11 pre-registers what each outcome means.

### 0.2 What this design deliberately is not

- **Not a DX100 port.** Task 2 established that four of DX100's mechanisms (index-sourced
  addressing, multi-level indirection without a core round trip, engine-side predicates,
  tile-granularity dedup) are absent from Vortex, and that its row-aware scheduler would be
  measured against a memory controller with no row arbitration at all. A port would
  reproduce ISCA 2025 on a weaker baseline. This design instead asks what part of the
  *effect* is attributable to what *mechanism*.
- **Not a new architecture proposal.** The "full engine" arm (§6, C6) is a parameterized
  model of the mechanisms Task 2 identified, sized from DX100's published configuration so
  that its costs are not fictional. No claim is made that this engine should be built.
- **Not dependent on new RTL.** The experiment runs in SimX only (§2.2), for reasons in §2.3.

### 0.3 Why SimX, and the one thing it cannot do

`sim/common/dram_bank_model.h` already contains a reservation-based DRAM channel model:
channels, banks, rows/columns, JEDEC timing constraints, per-bank FCFS, read/write buffers
with watermarks, an issue horizon and optional refresh *(verified: header comment
`:26-58`, `Config` at `:65-100`, `DramBankSim` at `:546-585`)*. That is most of required
component D, and it is validated against an RTL endpoint (`foldsim_mem_bank.sv`), which is
why building it again would be waste.

What it cannot do: it models **no bank groups** (the header describes channel → rank/BG?
No — it models channel → bank → row/column only). DX100's third technique, *interleaving
across channels and bank groups* (§3.2 stage 2, §6.2), therefore has no target in this
model. §12 R3 states the options.

---

## 1. Claims under test, and the isolation principle

The engine's advantage is not one thing. It is four, and they must be measured separately
or the experiment proves nothing:

| Claim | Mechanism | Isolated by | If this alone explains the result |
|---|---|---|---|
| **C1 Occupancy** | A separate generator issues requests without occupying SIMT registers, LSU slots or MSHR chains, so more lines are in flight | C2 vs C1 (queues enlarged, same generator) | The contribution is queue depth. Report as a capacity result, not an engine result |
| **C2 Visibility** | A batch of 16K known addresses is reordered before issue, so the *set* presented to the MC is better than arrival order | C3 (oracle window, SIMT generation) | The contribution is scheduling visibility; the engine is one way to get it |
| **C3 Work reduction** | Duplicate addresses are dropped and conditional elements filtered before issue | C5 vs C6 with dedup on/off | The contribution is traffic elimination; a compress/coalesce unit suffices |
| **C4 Issue-cost removal** | Per-element address computation leaves the core, freeing issue slots and removing load-use stalls | C4 vs C2 (same request stream, generation moved off-core) | The contribution is dynamic instruction count / energy, not bandwidth |

**Isolation principle.** Each claim gets a pair of arms that differ *only* in that claim's
mechanism. The pair-wise structure is what makes the conclusion defensible; a single
"engine vs baseline" number cannot distinguish C1 from C2 from C4, and is exactly the error
the task constraint warns about.

**Anti-assumption rule (from the task).** Nowhere does this design assume a larger window
is inferior. Instead it *measures the ceiling of a larger window* (C3) and requires the
engine to beat that ceiling to claim anything beyond queue capacity.

---

## 2. Simulator block diagram

### 2.1 Blocks

```
                        ┌─────────────────────────── one Vortex "socket" ───────────────────────────┐
                        │                                                                           │
  ┌──────────┐  ┌───────┴────────┐  ┌─────────────────┐  ┌──────────────┐  ┌──────────────────┐    │
  │ SM 0     │  │ SM 1 … SM n-1  │  │ [N1] BULK ENGINE│  │ [N2] ORACLE  │  │ [N3] WARP-STATE  │    │
  │ ┌──────┐ │  │  (identical)   │  │  descriptor RAM │  │  WINDOW      │  │      TRACER      │    │
  │ │warp  │ │  │                │  │  ┌───────────┐  │  │  (config C3  │  │  (C4 dependency  │    │
  │ │sched │ │  │                │  │  │ ctrl FSM  │  │  │   only)      │  │   wait, stall    │    │
  │ ├──────┤ │  │                │  │  ├───────────┤  │  │ ┌──────────┐ │  │   reason, OC)    │    │
  │ │ LSU  │ │  │                │  │  │ index fill│  │  │ │ 16K-entry│ │  └──────────────────┘    │
  │ │ coal.│ │  │                │  │  │ addr-gen  │  │  │ │ reorder  │ │                          │
  │ │ MSHR │ │  │                │  │  │ row/word  │  │  │ │ buffer + │ │                          │
  │ └──┬───┘ │  │                │  │  │  tables   │  │  │ │ perfect  │ │                          │
  │    │     │  │                │  │  │ req table │  │  │ │ MC view  │ │                          │
  └────┼─────┘  └───────┬────────┘  │  └────┬──────┘  │  │ └────┬─────┘ │                          │
       │                │           └───────┼─────────┘  └──────┼───────┘                          │
       ▼                ▼                   ▼                   ▼                                  │
  ┌──────────────────────────────────────────────────────────────────┐                             │
  │ [E] L1 dcache cluster (per-socket)  · banks · MSHR chains · merge│  ← existing: sim/simx/mem/cache
  └──────────────────────────────┬───────────────────────────────────┘                             │
                                 ▼                                                                  │
  ┌──────────────────────────────────────────────────────────────────┐                             │
  │ [F] dMMU lanes → L1 DTLB (16) → L2 TLB (512, 4-way) → PTW        │  ← existing: mem/mmu*.cpp, ptw.cpp
  └──────────────────────────────┬───────────────────────────────────┘                             │
                                 ▼                                                                  │
  ┌──────────────────────────────────────────────────────────────────┐                             │
  │ [G] L2 (1 MB, 8-way, sectored) · per-bank MSHR · no read merge    │  ← existing: cache_cluster.cpp
  └──────────────────────────────┬───────────────────────────────────┘                             │
                                 ▼                                                                  │
  ┌──────────────────────────────────────────────────────────────────┐                             │
  │ [H] memory interface / address→channel mapping (static interleave)│  ← existing: mem/memory.cpp
  └──────────────────────────────┬───────────────────────────────────┘                             │
        ┌────────────────────────┴────────────────────────┐
        ▼                                                 ▼
  ┌───────────────────────┐                       ┌───────────────────────┐
  │ [I] channel 0         │                       │ [I] channel 1         │   ← existing:
  │  read q (32) write q  │                       │  read q (32) write q  │     sim/common/dram_bank_model.h
  │  banks → rows/cols    │                       │  banks → rows/cols    │     (+ dram_sim.h)
  │  reservations, tCL…   │                       │  reservations, tCL…   │
  └───────────────────────┘                       └───────────────────────┘
```

**New blocks: [N1] bulk engine, [N2] oracle window, [N3] tracer. Everything else is
existing SimX code that the experiment parameterizes rather than replaces.**

### 2.2 Where each new block attaches

| Block | Attaches as | Rationale |
|---|---|---|
| [N1] engine | A new `SimObject` beside `DxaCore` (`sim/simx/dxa/dxa_core.{h,cpp}`), issuing `MemReq` into the same `Memory` object as the dcache, with a response callback | Mirrors the real DXA's position (cluster-shared client of the memory system) and reuses the existing request/response plumbing |
| [N1] result write | Through `LsuMemAdapter`'s LMEM path (`sim/simx/mem/lsu_mem_adapter.h`) or the `LocalMem` model, one word per beat | Matches how the RTL DXA drains to LMEM; makes "result delivery overhead" a real, chargeable cost |
| [N2] oracle window | A pass-through stage between `LsuMemAdapter` and `Cache` on the dcache request path, config C3 only | Must sit *before* MSHR allocation to be a window at all (§6 C3) |
| [N3] tracer | Passive observers on cache/MSHR/MC/warp-scheduler events; samples every 64 cycles | Metrics in §7 require time series, which no existing counter provides |

### 2.3 Why not RTL / rtlsim

The instrumented quantities (outstanding-over-time, per-warp dependency wait, oracle
windows) are intrusive; rtlsim at 4–8 SMs would take hours per point for a design-space
sweep of ~300 points. SimX is cycle-level and already models MSHRs, banks, DRAM timing.
Task 2 flagged that RTL and SimX diverge on exactly one counter relevant here (DX100-style
`gmem_dedup` is dead in RTL, adjacent-only in SimX) — the design therefore counts dups in
the new model and validates against an offline oracle (§9 V5), never against that CSR.

---

## 3. Timing and event model

### 3.1 Cycle base

Single clock domain at `VX_CFG_PLATFORM_CLOCK_RATE = 400` MHz *(verified
`VX_config.toml:97`)*. All latencies in core cycles. DRAM clocks are derived from
`VX_CFG_PLATFORM_MEMORY_DATA_RATE = 2 × clock` *(verified `:106`)* inside the existing bank
model; no new time base is introduced.

### 3.2 Per-request lifecycle and charging rule

A request exists in exactly one state at a time; each transition costs cycles from a named
parameter. **No transition may be free unless it is listed in §3.5.**

| # | Event | Cost / rule | Source |
|---|---|---|---|
| 1 | Lane coalescing | Adjacent-lane groups → one request per aligned block | `VX_mem_coalescer.sv:126-170` *(recon)* |
| 2 | Address translation | dMMU lane → L1 DTLB (16) → L2 TLB (512, 4-way, 4 cyc) → walk (3 levels, SV39, 4 KB) | `VX_config.toml:205,219-223`; `VX_types.toml:71-76` |
| 3 | dcache lookup | 2 cycles (`2 + max(0, log2(size/16K))` = 2) | `VX_config.toml:299` |
| 4 | MSHR allocate / chain | One entry per line per bank; a same-address request links into the chain (`addr_matches`) instead of allocating | `VX_cache_mshr.sv:207` *(verified)* |
| 5 | L2 lookup | 4 cycles, `L2_LINK_LATENCY = 0` each way | `VX_config.toml:339,344` |
| 6 | MC enqueue | One request per `bursts` clocks while the channel buffer has room; buffer = 32 read bursts | `dram_bank_model.h:71-72` *(verified)* |
| 7 | DRAM service | Reservation of PRE/ACT/COL at the earliest JEDEC-legal clock; data departs `tCL + tBL` after the last column; one command per clock; **per-bank FCFS** | `dram_bank_model.h:26-58` |
| 8 | Fixed platform latency | `+ VX_CFG_PLATFORM_MEMORY_LATENCY` after data departs (default **0** — see §9 V0) | `VX_config.toml:107` |
| 9 | Fill / response | Back through L2 → L1; fill forwarding lets head-of-chain reads take the fill directly | `docs/designs/mlp_optimizations.md` |
| 10 | Result delivery | Engine: one LMEM word per beat via the DMA port, then barrier release | §7 `result_delivery_cycles` |

### 3.3 Engine pipeline (N1) — stages and honest costs

The engine is a *costed* model, not a magic requester. Five stages, each with a rate:

| Stage | Model | Cost parameter | DX100 basis |
|---|---|---|---|
| S1 descriptor fetch | Read 1 descriptor from a 16-entry RAM | 1 cycle + contention for one read port | §3.5 (192-bit instruction = 3×64-bit MMIO stores) |
| S2 index fill | Stream `B[i]` into a tile buffer at the memory system's rate | Occupies the same channel bandwidth as any read; **not** free | §3.3 `SLD` |
| S3 address generation | `A + idx·elem_size` per element, `LANES_ADDR` elements/cycle, gated by tile fill level (DX100's overlap, §3.5) | `ceil(N / LANES_ADDR)` cycles, and it *competes* with S2 for tile-buffer ports | §3.2 stage 1 |
| S4 row/word table insert | Per element: 1 BCAM probe (row hit) + 1 SRAM probe (column) + linked-list append | 1 element/cycle/slice; slice count = channels × banks; miss allocates an entry | §3.2 stage 1(a)-(c), Fig. 4 |
| S5 issue + response | Request generator arbitrates slices; `REQ_TABLE` entries track outstanding lines; responses walk the word list to recover `(i, WO)`. | Issue 1 request/cycle; response reverse-map 1 word/cycle | §3.2 stages 2–3 |

**Modeled engine capacity (defaults, from DX100 §5 config):** tile = 16K elements; 32 tiles;
2 MB scratchpad; 16 ALU lanes; request table 128; row-table 64 entries × (channels × banks)
slices; 256-entry TLB. Dedup: word-table linked list per column, table size 128 (the same
`REQ_TABLE`).

**Deliberate conservatism.** DX100 does not report the per-element cost of its BCAM probe
and linked-list update; the design charges 1 element/cycle/slice, which is optimistic for
the engine. §12 R4 records this and the sensitivity run that bounds it.

### 3.4 Concurrency and backpressure rules

1. A component may accept at most one new request per cycle per port. Ports are those the
   configuration defines; the engine does not get extra ports in C4–C6 (the RTL DXA's port
   count is `min(NUM_DXA_CORES, … × L1_MEM_PORTS)`, `VX_config.toml:428`).
2. When MSHRs, pending pool, or the MC buffer is full, the offered request stalls its
   source and the stall is counted against that source (§7 `stall_*`).
3. The engine and the SMs contend for the same L2 banks, TLB lanes and channels by the same
   rules; no priority is granted in C4–C6. (C6 adds *no* arbitration advantage, by choice.)
4. Responses return in completion order per channel; the engine and MSHRs may complete out
   of order relative to issue. Determinism is preserved by a fixed tie-break (lowest tag).
5. Time advances one cycle per tick; all events at tick *t* read state as of *t-1*.

### 3.5 What is not modeled (each with its bias direction)

| Not modeled | Bias |
|---|---|
| Instruction fetch/decode bandwidth for the engine's control | Favours the engine slightly (its control is 3 MMIO stores per launch, `§4.1`) |
| Coherence traffic between engine scratchpad and cores | Favours the engine (DX100 needs a coherency agent and exclusive regions, §3.6) |
| Bank groups | Favours the baseline (removes one engine technique) — §12 R3 |
| Realistic DRAM address scrambling | Neutral if seeds are randomized; §9 V6 |
| Core energy / instruction-count effects | Out of scope: C4 is measured in cycles only, so an instruction-count win is invisible unless it changes stalls |

---

## 4. Workload definitions

All workloads are **hand-written kernels over pre-materialized index arrays**, so every arm
executes byte-identical work and the compiler cannot be a confound (matching Task 2's
finding that Vortex has no hoisting pass). Each has a scalar tail path so arms that cannot
express a pattern still produce correct results.

Common shape: `N` elements per kernel invocation, `A` a data array of `|A|` words, `B` an
index array of `N` words, results reduced into `out` (so nothing is dead code).

### W0 — Contiguous control (the ceiling)

`out[k] += A[i·S + k]` for k in a fixed unroll; `i` sequential. Purpose: measures the
platform's achievable streaming bandwidth under each arm. **No arm may be faster than W0**;
if one is, the model is wrong (§9 V2). Also the only workload where coalescing is perfect,
so its gap to W1 quantifies the cost of divergence.

### W1 — Random indirect gather

`out[j] += A[B[i]]`, `i` over `N`; `B` is a permutation of a chosen footprint, so
`|A| = N·f` controls the working set and hence the cache hit rate.

Three knobs, all *constructed*, not emergent:

- **Duplicate rate `d`**: sample `N` indices with replacement from `N·f/(1+d)`..; formally,
  draw from a pool of `M = N/(1+d)` distinct indices, so expected distinct fraction is `1/(1+d)`.
- **Row locality `ρ`**: order `B` by sorting a fraction `ρ` of consecutive pairs into the same
  DRAM row (using the same address→{channel,bank,row} map as the MC model). `ρ = 0` is a
  random permutation; `ρ = 1` is fully row-sorted. **This is the direct analogue of DX100's
  Fig. 8 index reordering and the only honest way to test C2.**
- **Footprint `f`**: `|A| / N ∈ {8, 64, 512}`, i.e. the gather streams through a working set
  that is L1-resident, L2-resident, or DRAM-only.

### W2 — Graph-style traversal (variable-length ranges)

CSR-shaped: for each frontier node `i`, iterate `j = H[K[i]] .. H[K[i]+1]`, load `A[B[j]]`.
This is DX100's `indirect range` (§3.1) and the pattern Vortex's DXA **cannot** express
(Task 2 §2.2), which is precisely why it belongs in the experiment: it is where a bulk
engine's value would be largest and where the baseline's per-row overhead is highest.

Degrees drawn from a power law with mean `D`; the frontier is a contiguous node range, so
the number of independent iterations varies as `frontier × D` — this is the "parallelism
limitation" DX100 itself concedes (§4.1).

### W3 — Sparse irregular (CSR SpMV)

`y[row] += A_val[p] · x[A_idx[p]]` over a synthetic CSR with power-law row lengths, plus an
RMW variant `A[B[i]] += c` to exercise the scatter path that only the engine arm can serve.
Purpose: the only workload that (a) has real reuse in the index structure, (b) can produce
writes, and (c) stresses the duplicate-removal claim (repeated indices in short rows).

### Workload parameter table

| Parameter | Symbol | Levels | Rationale |
|---|---|---|---|
| Elements per launch | `N` | 1K, 16K, 256K | 16K = one DX100 tile; 1K = under-filled tile |
| Data footprint | `f = |A|/N` | 8, 64, 512 | L1-resident → DRAM-only |
| Duplicate rate | `d` | 0, 1, 4 | 0 / 50% / 80% of requests redundant |
| Row-locality | `ρ` | 0, 0.5, 0.9 | permuted → row-sorted |
| Active warps | `W` | 4, 16, 64 per SM | under- to over-subscribed |
| SMs (cores) | `S` | 4, 8 | "multiple SMs" requirement; stock default is 1 *(verified `:4-6`)* |

---

## 5. Parameter table

### 5.1 Baseline (source-backed), and what the experiment must override

| Parameter | Stock value | Source | Experiment value | Why |
|---|---|---|---|---|
| Cores / clusters / socket | 1 / 1 / 1 | `VX_config.toml:4-6` *(verified)* | 4 and 8 cores, 1 cluster | Requirement: multiple SMs. Stock is single-SM |
| Warps × threads per SM | 4 × 4 | `:59-60` *(verified)* | 4/16/64 warps × 4 threads | Requirement: warp count sweep |
| `SIMD_WIDTH` | `= NUM_THREADS` = 4 | `:72` *(verified)* | unchanged | Keeps coalescing behaviour constant |
| Issue width | `pow2(clog2(up(warps/16)))` → 1 at 4 warps | `:67` *(verified)* | unchanged | Avoids a front-end confound |
| LSU lanes | 4 | `[lsu]` *(recon)* | unchanged | — |
| Mem block / line | 64 B | `:78` *(verified)* | unchanged | — |
| dcache | 16 KB, 4-way, 2 banks @4 threads, MSHR 16 | `:272-290,315` *(verified)* | unchanged | Baseline L1 |
| dcache read merge | on | `:326` *(verified)* | on in all arms | Must not be removed from baseline |
| L2 | 1 MB, 8-way, sectored, latency 4, link 0 | `:331-344` *(verified)* | 1 MB; link 10 cyc | A real GPU link is not 0; see §9 V0 |
| L2 MSHR | 16 (floor) | `:337` *(verified)* | 16 (C1), 64 (C2) | C1 vs C2 comparison |
| L1 DTLB / L2 TLB | 16 / 512 (4-way, 8 MSHR, 8 mega) | `:205,219-223` *(verified)* | unchanged | Translation cost stays real |
| Page size | 4 KB (SV39, 3 levels) | `VX_types.toml:71-76` *(verified)* | unchanged | Do **not** enable huge pages: that is DX100's escape hatch |
| Channels | 2 (`PLATFORM_MEMORY_NUM_BANKS`) | `:92` *(verified)* | 2, 4 | Channel-parallelism sweep |
| Block / data width | 64 B | `:94` *(verified)* | unchanged | — |
| Peak BW (declared) | 460 GB/s | `:96` *(verified)* | **derived, not declared**: `channels × 64 B × data_rate` = 102 GB/s at 2 ch, 400 MHz | See §9 V0: 460 GB/s with latency 0 is non-binding |
| Clock | 400 MHz | `:97` *(verified)* | unchanged | — |
| DRAM type / rate | HBM2 / `2 × clock` | `:105-106` *(verified)* | unchanged | — |
| DRAM fixed latency | 0 | `:107` *(verified)* | 20, 60, 150 cycles | Requirement: configurable latency; 0 makes MLP irrelevant |
| MC queue per channel | 0 = unbounded | `:108` *(verified)* | 32 (C1) / 64 (C2) | Requirement: queue capacity |
| MC read/write queue, watermarks | 32 / 32, hi 26, lo 7 | `dram_bank_model.h:71-77` *(verified)* | unchanged | Same MC in every arm |
| MC horizon | 448 clocks | `:80` *(verified)* | unchanged | — |
| DRAM geometry (banks, rows, cols) | from `dram_timing::geometry_t` | `#include <dram_timing.h>` `:16` — **not in this clone** | explicit experiment inputs | Evidence gap §12 R1 |
| DXA queue / max in-flight | 16 / 8 | `:429-430` *(verified)* | unchanged (engine arms inherit the port and credit model) | Engine must not get more ports |
| L2 sector size | 64 B | `:87` *(verified)* | unchanged | — |
| Refresh | off | `dram_bank_model.h:68` *(verified)* | off, then on in sensitivity | Isolates refresh interference |

### 5.2 Proposed engine parameters (choose from DX100 §5 unless noted)

| Parameter | Value | Justification |
|---|---|---|
| Tile size | 16K elements (sweep 1K, 4K, 16K, 32K) | DX100 §5, §6.4 sensitivity |
| Scratchpad | 2 MB, 32 tiles | §5 |
| Address ALU lanes | 16 (sweep 4, 16, 64) | §5 |
| Request table | **matched to baseline (§6.1)**, nominal 128 | §5 says 128 |
| Row-table entries / slice | 64 | §5 ("64 × 8 Row Table slices") |
| Row-table slices | channels × banks | §3.2 |
| Word-table | one entry per request-table entry | §3.2 |
| Engine TLB | 256 entries, software-preloaded | §3.6 — but **disabled** in C4–C6 baseline comparison; see §12 R5 |
| Descriptor slots | 16 | Vortex analogue `VX_DCR_DXA_DESC_COUNT = 16` |
| Result drain | 1 LMEM word/cycle | DXA's real drain rate (`VX_dxa_smem_wr.sv`) |
| Dedup | on in C6, off in C4/C5 | Claim C3 |

---

## 6. The six mandatory configurations

### 6.1 Resource equivalence rules (binding for all arms)

**R1 — Bandwidth.** Same channels, same data rate, same block size, same declared peak BW
(recomputed as in §5.1). No arm may have a faster bus.

**R2 — Cache and TLB.** Same dcache, L2, sectoring, TLB sizes, page size, and link
latencies. The engine gets no private cache (its scratchpad is modeled as an LMEM client,
not as a cache: it never serves a core request, so it cannot raise the hit rate).

**R3 — Outstanding capacity, matched at the bottleneck.** Let
`OUT_base = L2_MSHR_SIZE × L2_NUM_BANKS` = the number of line requests the memory system can
hold in flight below the LLC. Each arm's total in-flight capability to memory is set to
`OUT_base`:
- C1: `OUT_base = 16 × 2 = 32`.
- C2: queues enlarged to `64 × 2 = 128` at the L2, `32/bank` at L1, MC queue 64/channel.
- C3: same as C2, plus the oracle window (visibility, not capacity — see below).
- C4–C6: engine request table `= 128 = OUT_base(C2)`; the SMs' caches keep C2's sizes, but
  the *engine's* outstanding requests count against the same `128` budget through the shared
  L2 MSHRs. The engine does not add capacity; it changes who generates and when.

**R4 — Ports.** The engine issues on the same number of L1/L2 ports as one DXA unit would
(`VX_config.toml:428`); the SM side is unchanged.

**R5 — No privileged scheduler.** In C4 and C6, the MC's arbitration is identical; only the
*order in which requests arrive* differs (engine-generated vs demand). Row-aware behaviour in
C5/C6 is expressed as **hints/ordering at issue**, never as MC priority.

**R6 — Cost accounting.** Every engine mechanism charges cycles per §3.3. The arm must show
`address_gen_cycles + table_cycles + fill_cycles` as a visible cost in the cycle total.

### 6.2 Configuration definitions

| ID | Name | Generator | In-flight capacity | Visibility / order | MC | Purpose |
|---|---|---|---|---|---|---|
| **C1** | Baseline Vortex-like | SIMT: LSU + MSHR | `OUT_base` = 32 lines; MC 32/ch; DRAM latency param | Per-warp demand; arrival order | Per-bank FCFS (as built) | Reference point |
| **C2** | Baseline, larger queues | SIMT | 128 lines (4× C1); MC 64/ch | Per-warp demand; arrival order | Per-bank FCFS | Absorbs claim **C1** |
| **C3** | Baseline, idealized window | SIMT | 128 lines (as C2) | **16K-entry reorder buffer before MSHR allocation; perfect knowledge of all pending line addresses; selection by row-hit, zero bookkeeping cost** | Per-bank FCFS over the *reordered* stream | Absorbs claim **C2**. This is the true upper bound of "just give the baseline a bigger window" |
| **C4** | Engine address gen, no DRAM-aware order | Engine S1–S3 | Table = 128 (= C2) | Tile order as generated (no row/word table) | Per-bank FCFS | Isolates claim **C4** (off-core generation) against C2 |
| **C5** | Engine + row-aware order | Engine S1–S4 | Table = 128 | Row table reorders within a tile; tracks C2's capacity | Per-bank FCFS over engine order | Isolates claim **C2** *with* engine structure, and dedup **off** |
| **C6** | Full engine | Engine S1–S5 | Table = 128 | Row+word tables, dedup on, predicates on, result remap, S2/S3 overlap | Per-bank FCFS over engine order | Adds claims **C3** (dedup/predicates) and result-delivery costs |

**Why C3 must exist before C4.** Without it, C4-vs-C2 conflates "off-core generation" with
"the engine happens to issue in a nicer order than warp arrival". C3 gives the baseline the
best possible ordering for free; any C4/C5/C6 advantage over C3 then has to come from
capacity, work elimination, or reduced core-side stalls — and if none of those survive,
the honest conclusion is that the value was the window, which C3 already bounds.

**Why C3 is a bound and not an implementation.** A 16K-entry reorder buffer that is free
and clairvoyant cannot be built; it exists to make the hypothesis falsifiable. §11 states
what each relation between C3 and C5 means.

---

## 7. Metrics

Deterministic simulator: one cycle count per (config, workload, seed). Only address maps
and index construction are randomized, and seeds are fixed and paired across arms.

| Metric | Definition | Collector |
|---|---|---|
| `cycles` | launch → last barrier release | SimX stats (already present) |
| `useful_bytes` | 64 × (distinct lines the kernel must consume), from the index array offline | offline oracle |
| `transferred_bytes` | Σ 64 B responses at the MC, per channel | MC stats |
| `efficiency` | `useful_bytes / transferred_bytes` | derived → measures claim C3 directly |
| `bw_util` | `transferred_bytes / (cycles × bytes_per_cycle_peak)` | derived |
| `bw_achieved` | `transferred_bytes / (cycles / clock)` in GB/s | derived |
| `outstanding[t]` | lines in flight to memory, sampled every 64 cycles; report mean, p50/p95, and the integral `Σ outstanding·Δt` | [N3] tracer |
| `occ_L1`, `occ_L2`, `occ_MC` | same, per level | tracer + existing `mshr_size` accounting (`cache.h:42,68`) |
| `stall_mshr`, `stall_pending`, `stall_mc`, `stall_tlb` | cycles a source is refused, split by cause | tracer on §3.4 rule 2 |
| `row_hits`, `row_conflicts` | per channel, from the bank model's reservation events | MC stats (`dram_bank_model.h:145`) |
| `row_hit_rate` | `hits / (hits + conflicts)` | derived |
| `hit_L1`, `hit_L2` | per-level hit rate on demand *and* engine requests separately | cache stats |
| `dups_eliminated` | requests suppressed by the word table | engine model, **validated against `N − |unique(B[i])|`** |
| `addr_gen_rate` | elements/cycle through S3; and total S3 cycles | engine model |
| `dep_wait` | cycles a warp is stalled consuming a register produced by a pending load (load-use), summed over warps | [N3] on the warp scheduler |
| `t_dependency_chain` | for W1/W2: cycles from index-array fill start to last data request issued — the quantity the hypothesis is about | engine model / kernel markers |
| `result_delivery` | cycles from last data response to barrier release, plus LMEM beats | engine model + LMEM |
| `dyn_insts` | dynamic instruction count per SM (claim C4's other face) | existing performance model |

**Primary outcome:** `cycles` (and `bw_util`).
**Secondary:** `efficiency`, `row_hit_rate`, `outstanding` mean/p95, `dep_wait`.
Everything else is diagnostic.

---

## 8. Assumptions and simplifications

| # | Assumption | Direction of bias | Mitigation |
|---|---|---|---|
| A1 | The engine's table insert costs 1 element/cycle/slice (DX100 does not publish it) | Favours engine | §10 Stage 4 sweeps it from 0.25 to 4 elem/cycle/slice |
| A2 | Index fill competes for bandwidth but the engine's descriptor/control fetch is nearly free | Favours engine | Charge S1 per launch; count MMIO cycles |
| A3 | Engine scratchpad is an LMEM client, never a cache | Neutral | Enforced by R2 |
| A4 | No coherence cost for engine-owned regions | Favours engine | §12 R5; report as an upper bound |
| A5 | DXA-class engine has one LMEM drain port at 1 word/cycle | Favours baseline (delivery is serialized) | Report `result_delivery` separately |
| A6 | Bank groups absent from the MC model | Favours baseline | §12 R3; drop the interleave claim if not added |
| A7 | Oracle window in C3 is free and clairvoyant | Favours baseline | Intentional; it makes the hypothesis falsifiable |
| A8 | Workloads are hand-written, no compiler pass | Neutral (both arms) | Same kernel skeleton per arm; §4 |
| A9 | 4 KB pages, no huge pages | Favours baseline in cycles, but matches reality | §5.1; sensitivity with mega entries reported separately |
| A10 | Warm caches at kernel start, cold DRAM | Small favour to whichever arm reuses lines first | Report cold-cache runs for W1/W2 in sensitivity |

---

## 9. Validation strategy

Gates are ordered; a failure stops the campaign (no results reported from a model that
fails a gate).

- **V0 — Make the platform binding.** Stock config declares 460 GB/s at 400 MHz with
  `PLATFORM_MEMORY_LATENCY = 0` and `QUEUE_SIZE = 0` *(verified `:96-97,107-108`)*. That is
  infinite bandwidth: no arm can differ. Gate: with 2 channels, `bw_achieved ≤ 102.4 GB/s`,
  and W0 must be measurably latency-sensitive when `LATENCY` goes 20 → 150.
- **V1 — W0 reaches the derived peak.** Streaming with unbounded queues must land within
  5% of `channels × 64 B / DRAM clock`; else the requester or the bus is the bottleneck and
  the experiment measures the wrong thing.
- **V2 — Little's law.** `mean_outstanding × latency ≈ achieved_bytes_per_cycle` within 15%
  in the streaming arm. This is the check that the occupancy metric is real.
- **V3 — Ordering sanity.** For W1 with `ρ=0`, `row_hit_rate` in C1 must be near the
  random-access expectation for the configured geometry; with `ρ=0.9` it must be high even
  in C1. If a sorted index stream does not raise row hits, the address map is wrong.
- **V4 — Invariance.** In C6, raising the MC queue beyond `REQ_TABLE` must not change cycles
  by more than 1%; if it does, the engine is leaking capacity and R3 is broken.
- **V5 — Dedup oracle.** `dups_eliminated` must equal `N − |unique(B[i])|` exactly for every
  W1/W3 run, and the transferred-bytes reduction must equal the suppressed lines × 64. (This
  is the metric whose RTL counterpart is dead — Task 2 §2.7 — so it is validated
  arithmetically, not against a counter.)
- **V6 — Address-map randomization.** Run each cell with 5 seeds that permute the
  channel/bank/row map; report the spread. If the spread exceeds the effect, the result is a
  map artefact.
- **V7 — Positive control.** C5 must show a row-hit-rate gain over C2 on `ρ=0` data, of the
  same *sign* and rough magnitude as DX100 reports for reordering (2.7× average, §6.2). If
  the model cannot reproduce the effect qualitatively, it cannot be used to argue it is
  absent in some other arm. Note: 2.7× is DX100's number on a 32-entry FR-FCFS controller,
  not a prediction for this model.
- **V8 — Cross-model spot check.** One config (C2, W0) run in rtlsim as well; if cycles
  differ by more than the configured link/latency constants explain, record it and treat
  all absolute numbers as model-relative.

---

## 10. Experiment matrix

Four stages, ordered by cost and by what they can falsify.

### Stage 1 — Headline (all six configs × four workloads), nominal parameters

`N=16K`, `f=512`, `d=1`, `ρ=0`, `W=16`, `S=8`, DRAM latency 60, MC 32/ch.
**24 runs × 5 seeds = 120.** Answers: does any engine claim survive C3?

### Stage 2 — Dose-response for claim C2 (visibility)

Configs **C2, C3, C5, C6** × workloads **W1, W2** ×
`ρ ∈ {0, 0.5, 0.9}` × `d ∈ {0, 1, 4}` = 4 × 2 × 3 × 3 = 72 cells × 5 seeds = **360 runs**.
This is the core of the study: it asks whether the engine's reordering degrades gracefully
and whether it still beats the oracle at *low* row locality (the only regime where
visibility can matter).

### Stage 3 — Capacity and latency (claim C1)

Configs **C1, C2, C3, C6** × **W1, W3** × latency `{20, 60, 150}` × MC queue `{16, 32, 64}`
= 4 × 2 × 3 × 3 = 72 cells × 3 seeds = **216 runs**. Establishes where occupancy stops
mattering — the regime in which an engine story must live.

### Stage 4 — Engine cost sensitivity (claims C4/C3-cost)

Configs **C4, C5, C6** × **W1** × address lanes `{4, 16, 64}` × table rate `{0.25, 1, 4}`
elem/cycle/slice × tile `{1K, 16K, 32K}` = 3 × 1 × 3 × 3 × 3 = 81 cells × 3 seeds =
**243 runs**. Bounds assumptions A1/A2, and tests DX100's own tile-size sensitivity (§6.4).

### Stage 5 — Warp-scaling (only if Stage 1 survives)

Configs **C2, C3, C5, C6** × `W ∈ {4, 16, 64}` × `S ∈ {4, 8}` on W1. **48 cells × 3 seeds.**

**Total ≈ 1,000 runs.** At an estimated 1–5 min per SimX run for 8 SMs / 16K elements,
that is 20–80 CPU-hours, parallelizable; Stage 1 alone (2–10 hours) is the go/no-go.

### Analysis plan

- Paired by seed across arms within a cell (same index array, same address map).
- Report per-workload Δ% and a geometric mean over the four workloads, never a bare mean.
- Effect size with a batch-means CI over seeds (n=5 for Stage 1, 3 for sweeps).
- The only statistical test that matters is a paired comparison of C3 vs C5/C6 per workload
  and per `ρ` stratum; everything else is descriptive.
- Decomposition reported alongside: for the best engine arm,
  `Δcycles = Δoccupancy_term + Δwork_term + Δorder_term + Δcost_term`, using the metrics in
  §7 as the operands (bands, not exact algebra — §12 R6).

---

## 11. Pre-registered interpretation

Written before running so that the result cannot be reinterpreted after the fact.

| Observation | Conclusion | Consequence |
|---|---|---|
| **C6 ≈ C3** (within CI) | Nothing beyond window + capacity. The engine buys no MLP that a large visible window does not | Hypothesis **refuted** for this platform. Report as a negative result: the value is visibility, and a cheap reorder buffer would capture it |
| **C6 > C3 only when dedup on** (`d>0`) | The gain is work elimination (C3), not MLP | Reframe: a compress/dedupe unit in front of the MC. Weak novelty (ICC, IRU) — Task 2 §5 |
| **C6 ≈ C5** but both > C3 | Row-aware *engine* ordering beats an oracle that lacks the engine's structure — would be surprising and needs explanation (likely: the oracle's zero-cost selection is not actually zero-cost in this model, i.e. a bug) | Treat as a **bug signal** first; investigate before claiming |
| **C5 ≈ C2 < C3** | Ordering without the engine's structure does nothing; the engine's structure is the whole effect | Strengthens C2 claim; requires Stage 4 to show the cost is worth it |
| **C4 > C2** (same order, generation moved off-core) | The win is claim C4: core-side address computation and stalls, not bandwidth | Pivot to an instruction-count/energy story — different paper, and SimX cannot measure energy (§3.5) |
| **C2 > C1** by a lot, and all engine arms ≈ C2 | Occupancy is the whole story | Report queue depth as the lever; no engine needed. This is the most likely outcome given Task 2's finding that Vortex already sizes MSHRs by Little's law |
| **Nothing beats C1** | The model is not sensitive enough (V0/V1 failed) or the workload is not irregular enough | Do not report; fix the platform first |

**The claim survives only if `C6 > C3` with dedup off, at nominal `ρ`, beyond the seed
spread.** That is a deliberately hard bar, and it is the bar the task's constraint demands.

---

## 12. Risks and open questions

- **R1 — DRAM geometry is not in the snapshot.** `dram_bank_model.h` includes
  `<dram_timing.h>` *(verified `:16`)*, which is not in the clone; `dram_timing::geometry_t`
  supplies banks/rows/columns per JEDEC standard. The experiment must pin a geometry
  explicitly (and state it), or import the header before any row-locality result is
  meaningful.
- **R2 — No bank groups** in the model. DX100's interleaving claim (channels *and* bank
  groups) is only half testable. Either extend the bank model with bank groups (a real
  change to a validated model) or drop the interleave claim from the write-up. Recommend
  dropping it for this experiment.
- **R3 — Row-aware arms depend on the MC's reordering policy.** C5/C6 express ordering at
  *issue*; if the MC's per-bank FCFS plus horizon-448 reservation already extracts the same
  row hits, C5 ≈ C2 regardless of the engine. That is a legitimate finding, not a bug, and
  §11 covers it.
- **R4 — The engine's per-element table cost is unpublished in DX100.** Stage 4 bounds it;
  until then, every engine result is conditional on A1.
- **R5 — Engine TLB.** DX100 sidesteps translation with huge pages and a preloaded 256-entry
  TLB. Enabling either would hand the engine an advantage the baseline does not get (R2).
  Recommendation: **keep 4 KB pages and the shared dMMU for all arms**, and treat DX100's
  translation assumption as a *separate* result, not a configuration knob. (This is the same
  issue Task 2 §6 identified as the one surviving hypothesis — it should not be smuggled into
  this experiment.)
- **R6 — The decomposition identity is approximate.** `Δcycles` cannot be split exactly
  because the terms interact (dedup changes occupancy, occupancy changes ordering). Report
  bands and the ablation arms, not a single attribution.
- **R7 — Go/no-go is Stage 1 only.** If Stage 1 shows `C6 ≈ C3`, stop; do not run 800 more
  cells to rescue the hypothesis.

---

## 13. What this design excludes

- No implementation, no patch to Vortex, no new RTL.
- No energy or area modelling (would need the synthesis flow; out of scope for an MLP
  question).
- No compiler pass (workloads are hand-written; Task 2 §2.15 established Vortex has none).
- No multi-cluster / multi-socket scaling (single cluster, 4–8 SMs).
- No coherence protocol changes; engine regions are assumed exclusive, flagged as A4/R5.
- No claim about DX100's own evaluation being wrong; the design compares mechanisms, not
  papers.

---

## 14. Review checkpoints

1. **Platform admissibility (§9 V0–V2).** Are the derived bandwidth, latency and queue
   values accepted as the baseline? Without this the whole matrix is void.
2. **Is C3 the right ceiling?** It is free and clairvoyant by construction. A reviewer may
   argue for a cheaper, more realistic "large window" arm (e.g. a 256-entry real reorder
   buffer) alongside it. Recommend running both if Stage 1 is close.
3. **Is `dups_eliminated` the right proxy for claim C3?** It is exact (V5) but only counts
   suppressed *requests*, not suppressed *bytes*; §7 reports both.
4. **Is the engine's conservatism sufficient** (A1/A5) for a negative result to be credible?
   If the engine loses, the design must be able to say it lost despite generous assumptions.

*End of design. No code was modified; `ref/vortex` remains unmodified. Awaiting review
before any implementation.*
