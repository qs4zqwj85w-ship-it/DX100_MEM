# Standalone DX100 memory-system simulator — design

**Status:** design only. No simulator code has been written. This document is the approval gate for
implementation (see §10).

**One-line proposal:** replace the Vortex-hosted experiment with a ~5 000-line, dependency-free C++20
simulator that models the *same* memory question on macOS natively, because (a) the complete Vortex
simulator cannot be built here, and (b) more importantly, **the experiment's arms do not exist in
Vortex either** — they are modelled extensions (v3 §7.3), so the arm code has to be written from
scratch regardless of host. Writing it standalone is cheaper, more controllable, and removes a build
system that costs more than the model.

---

## 1. Research objective

The question is unchanged, and is preserved verbatim from v3:

> Can extending Vortex DXA with bulk indirect address generation and request management improve
> irregular GPU memory access beyond conventional SIMT execution, coalescing, caching, and request
> scheduling?

### 1.1 What the study is actually testing

Three separable mechanisms, kept separate exactly as v3 §1 does:

| | Mechanism | Tested by |
|---|---|---|
| **M1** | bulk address generation (addresses exist before SIMT computes them) | `A1 → A4` (both FIFO, both 128 slots) and `A3g → A5*` (primary) |
| **M2** | request organization / reordering | `A4 → A5 → A5*` |
| **M3** | duplicate elimination | `A5*` vs `A5*+D` |
| **M4** | translation optimization | `B2 → B3`, and the C-family 2×2 |

The standalone simulator's job is to make **all four measurable with native counters**, on one host,
with the fairness mechanisms of v3 §3–§4 preserved.

### 1.2 What changes, and what does not

| | Change |
|---|---|
| **Platform** | Native macOS, `clang++`, no Linux/VM/Docker, no Vortex build system, no third-party libraries |
| **Instrumentation** | Native counters from day one; the P0 workaround (§7.2 "requires new instrumentation") becomes moot |
| **Arm control** | `A3g`, `A2/A3` windows, `A5*`, `B3` become first-class configurable components instead of edits inside a 30 kLOC codebase |
| **Unchanged** | The arm set `A0…A5*+D`, `B0…B5`, `C0/C1`; the capacity budget `R = 128`; the workload generator (`Λ`, `P`, `λ`, `ρ_dup`, `κ`, `L`); the metrics of §7.4; the gates of §9; the probe of §10 |

### 1.3 What the simulator is *not*

It is not a GPU. It has no ISA, no decoder, no scoreboard, no register file, no warp scheduler policy,
and no graphics units. It models a **memory system with two possible request front-ends**, driven by a
canonical access trace. Every claim it produces is a claim about that model, which is why §7 spends
most of its length on fairness and §9 on what cannot be concluded.

---

## 2. Minimum architecture and component diagram

### 2.1 Structure

```
                     workload generator  (W0..W3; Λ, P, λ, ρ_dup, κ, L, seed)
                                 |
                        canonical Trace  (materialized, N ≤ 16 384 accesses)
                                 |
            +--------------------+---------------------+
            |                                          |
   ┌────────▼─────────┐                       ┌────────▼──────────┐
   │  SIMT front-end  │                       │  engine front-end │
   │  warp / EA gen   │                       │  descriptor fetch │
   │  coalescer       │                       │  index stream     │
   │  R slots (FIFO / │                       │  address gen      │
   │   predicted /    │                       │  dup table        │
   │   clairvoyant)   │                       │  R slots (policy) │
   └────────┬─────────┘                       │  completion/remap │
            │                                 └────────┬──────────┘
            │        ┌───────────────────────┐         │
            └───────►│ outstanding budget R  │◄────────┘
                     │  (shared invariant)   │
                     └───────────┬───────────┘
                                 |
                     L1 TLB  (dmmu 16 / ummu 8, 4 MSHRs, park+replay)
                                 |
                     cluster L2 TLB (512, 4-way, 8 MSHR, 4 cyc)
                                 |
                     PTW (2 walkers, 16-entry walk cache, L cycles)
                                 |
                     L2 cache (1 MiB, MSHR-limited)
                                 |
                     DRAM (banks, rows, FR-FCFS or FIFO, tRCD/tRP/tCAS)
```

### 2.2 The one structural rule that makes the comparison valid

**Both front-ends drive the same `MemorySystem` object.** There is exactly one translation path, one
cache, one DRAM model, one set of latencies and one bandwidth, instantiated once per run and
configured identically for both. A run differs from another run only in `(front_end_kind, arm_params,
workload, seed)`. This is not a stylistic choice: it is what makes "identical memory system"
falsifiable — if the two front-ends had separate instances, a config drift between them would be
invisible in the results and fatal to the conclusion.

### 2.3 Component inventory

| Component | Models | State | Deliberately absent |
|---|---|---|---|
| `Trace` | the workload's access stream | immutable array | any RNG at run time |
| `SimtFrontEnd` | warp, EA generation, coalescing, slots | warp pc, per-slot records | pipeline, scoreboard, ILP |
| `BulkFrontEnd` | descriptor, index stream, addrgen, dup table, slots, remap | table, slots, heads | "free" parallelism |
| `L1Tlb` | per-client TLB + miss station + replay | entries, MSHR, park stage | permission/fault detail |
| `L2Tlb` | shared cluster TLB | 512 entries, 8 MSHRs, pipe | hierarchy above |
| `Ptw` | page walkers + walk cache | 2 walkers, 16 tags | multi-level faults |
| `L2Cache` | shared L2 | tags, MSHRs | writeback protocol detail |
| `Dram` | banks, rows, scheduling | per-bank open row, queue | refresh, ECC, 3D stacking |
| `Stats` | all counters | counters | — |

---

## 3. Baseline and accelerator execution flows

### 3.1 Baseline (SIMT) — per cycle

```
for each active warp (round-robin, W warps):
  if warp has no pending memory instruction: fetch next instruction from Trace
  else if all lanes resolved: retire, warp ready
  else: continue (memory-bound: no other work to do)

issue (at most LSU_LINES_PER_CYCLE):
  1. EA gen:  addr[l] = base + w·idx[l]                      (L lanes, 1 cycle)
  2. coalesce: group the L addresses by 64 B line → 1..L line requests
               (a fully coalesced instruction yields max(1, L·w/ℓ) requests)
  3. per line request:
       a. allocate an R slot (FIFO order; if none free → stall, count it)
       b. L1 TLB probe:
            hit  → translate, forward downstream this cycle
            miss → park on miss station (VPN-deduped, ≤2 per entry);
                   if no entry or entry full → stall port, count it
       c. on fill: replay parked requests in arrival order, release slot on response
```

Modelled per-instruction cost: EA generation (1 cycle) + coalescing (1 cycle/lines) + queueing. **No
decode, no operand fetch, no register writeback.**

### 3.2 Accelerator (bulk engine) — per cycle

```
descriptor unit (one descriptor per tile):
  1. read descriptor: base, count, stride, index-array pointer     (1 line read)
  2. stream the index array: 8 indices per 64 B line                (charged as traffic + TLB)

address generator (AG_LANES = 8 addresses/cycle):
  3. for each index i: addr = base + w·B[i], line = addr >> 6
  4. duplicate table lookup (1/cycle): 
        hit  → the line is already in the slot pool; attach i to it (no new request)
        miss → allocate an R slot for the line
     if the table is full → evict (LRU/FIFO), count the eviction
  5. when a slot is formed, it becomes *unissued* and participates in the arm's selection policy

issue policy (arm-dependent, on the same R slots):
  A4   : FIFO
  A5   : rank by predicted row (address-only hash, same function as A2)
  A5*  : rank by actual open-row state (clairvoyant, same function as A3/A3g)
  A5*+D: as A5*, plus duplicate-word suppression (M3)

completion:
  6. each returned line is written into the LMEM tile at its destination offset
     (charged at LMEM_WRITE_BW)
  7. when all slots for a tile have returned, the descriptor retires
```

### 3.3 Cost symmetry — what each side pays

| Cost | Baseline pays | Engine pays |
|---|---|---|
| Index-array reads | as ordinary loads through L1 | as descriptor + stream through L1 path |
| Address generation throughput | 1 instruction/cycle/warp | `AG_LANES` addresses/cycle, then 1 slot/cycle |
| Duplicate detection | tag compare in L1 (free) | **explicit table lookup, capacity and latency charged** |
| Outstanding capacity | `R` slots (32/SM at S=4) | `R` slots (single global client) |
| Result placement | register writeback (free) | **LMEM tile write + copy-out** |
| Pipeline overlap | none modelled | none modelled |

The last row matters: because neither side models overlap with compute, neither can hide latency
behind it. That is a deliberate, symmetric simplification (§9, R1).

---

## 4. Component specifications

### 4.1 Reference parameters

Values marked **[V]** are source-verified from the Vortex tree at `7300bc8`; **[A]** are modelling assumptions that must appear in every results table and be swept in §7.8.

| Parameter | Value | Source |
|---|---|---|
| Cache line / block | 64 B | **[V]** `VX_config.toml:78` |
| Element width `w` | 8 B | **[V]** v3 §5.1 |
| L1 dcache | 16 KiB, 4-way, 2-cycle | **[V]** `:272,273,299` |
| L1 dcache MSHRs | 16 (clamped 16–64) | **[V]** `:290` |
| L1 dcache read queue | 4 | **[V]** `:308` |
| L2 | 1 MiB (16 384 lines) | **[A]** v3 §5.2 lattice |
| L2 MSHRs | 16 stock / 64 per bank × 2 banks = 128 | **[V]** clamp `:337`; budget per v3 §2.3 |
| dTLB / iTLB | 16 entries, 4-way | **[V]** `:205` |
| uTLB (engine) | 8 entries | **[V]** `:212` |
| L1 TLB MSHRs | 4 | **[V]** `:207` |
| Parked per MSHR entry | 2 | **[V]** RTL `REPLAY_DEPTH`, SimX `MAX_PARKED_PER_ENTRY` |
| L2 TLB | 512, 4-way, 8 MSHRs, 4 cycles | **[V]** `:219-223` |
| Page walkers | 2 | **[V]** `:224` |
| Walk cache | 16 entries | **[V]** `:225` |
| Page size | 4 KiB | **[V]** `:212` region |
| Translation mode | **Sv32, 2 levels** | **[V]** `VX_types.toml:73-74`; XLEN=32 is the build default — §4.1.1 |
| PTE size / table | 4 B PTE, 4 KiB table ⇒ 1024 PTEs/table | **[V]** `VX_types.toml:75-76` |
| PTEs per 64 B line | 16 | **[D]** derived (64/4) |
| Walk latency | **300 cy/walk** (2 levels × 150; swept 75–225 per level) | **[A]**; replaces v3's 305, a 3-level figure |
| DRAM latency | 60 cycles (swept 30/60/120) | **[A]** v3 §10 P1 |
| Link bandwidth | 64 B/cycle = 1 line/cycle | **[A]** = 102.4 GB/s @1.6 GHz, the denominator v3 §7.4 mandates |
| Outstanding budget `R` | 128 line requests | **[V]** derived, v3 §2.3 |
| Cores `S` | 4 or 8 | **[A]** v3 §3.2 |
| Lane group `L` | 4 or 32 | **[V]** `:59,72` |

Two things this table makes explicit, both required for reproducibility: the stock Vortex
`MEMORY_LATENCY = 0` (`:107`) is *not* used — a zero-latency memory makes the entire study vacuous,
and v3 §2.3 replaces it with a declared latency and a derived budget; and the declared
`PEAK_BW = 460000` (`:96`) is *not* used as a denominator — v3 §7.4 requires the derived 102.4 GB/s.

### 4.1.1 The reference configuration we reproduce, and the XLEN decision

**Reference:** Vortex at `7300bc8`, **`XLEN = 32`** — the default of `configure` (`:134`,
`default_xlen=32`) and of every build in this study so far. At `XLEN = 32` the tree explicitly
selects

```
VX_VM_ADDR_MODE = 'SV39' if XLEN == 64 else 'SV32'     VX_types.toml:73
VX_VM_PT_LEVEL  = 3      if XLEN == 64 else 2          VX_types.toml:74
VX_VM_PTE_SIZE  = 8      if XLEN == 64 else 4          VX_types.toml:75
VX_VM_PT_SIZE   = VX_VM_PAGE_SIZE = 4096               VX_types.toml:76
```

and the generated header confirms the resolved values: `SV32`, `PT_LEVEL 2`, `PTE_SIZE 4`,
`PT_SIZE 4096`.

**Decision: reproduce Sv32 — 2 levels, 4 KiB pages, 1024 PTEs per table, 4 B PTEs, 16 PTEs per line.**
Reason: it is what the reference configuration *is*. XLEN=64 was **not** adopted, because choosing a
wider ISA to keep v3's arithmetic intact would invert the direction of the work — the model would be
tuned to the numbers instead of the numbers derived from the model. The cost is that v3's §6.2 (b)–(d)
rows no longer describe this platform, which is accepted and handled explicitly in §4.11 and C-4.

What Sv32 changes, and what it does not:

| Quantity | Sv32 consequence |
|---|---|
| PTE accesses per walk | **2** (was 3) |
| PTEs per 64 B line | **16** (was 8) |
| Leaf lines for `W` walked pages | `ceil(W/16)` (was `ceil(W/8)`) |
| Interior lines | **1** for every tested `P` (root PTEs ≤ 16 ⇒ ≤ 64 B) |
| Walk latency | `levels × L_pt` = **300 cy** at `L_pt = 150` (was 305 as a single 3-level figure) |
| Number of walks | **unchanged** — a walk is per distinct page, independent of level count |

### 4.2 The trace (the fairness primitive)

```
struct Access { uint32_t elem;  uint64_t addr;  uint32_t line;  uint16_t instr; uint8_t lane; };
struct Trace  { std::vector<Access> accesses;  // program order, materialized
                std::vector<uint32_t> instr_len; // accesses per memory instruction
                TraceMeta meta;                 // realized Λ, P, λ, κ̂, ρ̂, bytes };
```

`N ≤ 16 384`, so the trace is **materialized once** and both front-ends consume the same array. This
eliminates an entire class of bias: there is no per-arm regeneration, no RNG at run time, and no way
for one arm to see a different address stream. It is the single most important design decision in this
document.

### 4.3 SIMT front-end

- **Warp model:** `L` lanes per memory instruction; `W` warps per SM issuing round-robin; no ILP.
- **Coalescing rule** (exact, from v3 §5.4): a fully-coalesced instruction of `L` contiguous lanes
  produces `max(1, L·w/ℓ)` line requests — 1 at `L=4`, 4 at `L=32`; a divergent instruction produces
  up to `L`.
- **Slots:** `R/S` per SM (32 at S=4, 16 at S=8). FIFO for A0/A1; predicted or clairvoyant selection
  for A2/A3; a global scheduler for A3g.
- **Backpressure:** no free slot → the warp stalls and `stall_slots` counts it.
- **Hit-under-miss:** a hit issues while older requests are parked (this is Vortex's behaviour and it
  matters — it is why the baseline is not trivially serialized).

### 4.4 Bulk engine front-end

| Sub-component | Spec | Cost charged |
|---|---|---|
| Descriptor fetch | 1 line per tile; `TILE_ELEMS` per descriptor | 1 line request + TLB |
| Index stream | 8 indices per line, sequential | 1 line request per 8 indices + TLB |
| Address generator | `AG_LANES` = 8 lines/cycle | cycles |
| Duplicate table | 256 entries, 1 lookup/cycle, LRU, keyed on line address | 1 cycle/slot, capacity, eviction |
| Slot pool | the same `R` = 128, global | shared invariant |
| Completion / remap | per-slot destination offset; LMEM write on return | LMEM_WRITE_BW |
| Tile buffer | 128 KiB (matches v3's A4/A5* row) | occupancy |

**The duplicate table is the M3 mechanism and must be charged.** A table that is free, infinite or
zero-latency would manufacture the M3 result. Its capacity (256 entries) is smaller than `Λ` (16 384
lines) by design, so it is a *window* on duplicates, not a perfect filter — this is what makes M3 a
measured result rather than an assumption.

### 4.5 L1 TLB (`L1Tlb`)

Reproduces Vortex's structure faithfully because the whole M4 result depends on it: `n` entries
(16 for dmmu, 8 for ummu), 4-way LRU, `MSHR = 4` miss station, VPN-deduped entries, ≤2 parked requests
per entry, a registered park stage when `park_reg`, replay in arrival order, hit-under-miss.

Counters: `reads/hits/misses/evictions/mshr_allocs/mshr_joins/mshr_occupancy/stall_cycles` — the same
definitions as the P0 work (§12), which is one of the few places that work carries forward directly.

### 4.6 L2 TLB, 4.7 PTW, 4.8 L2, 4.9 DRAM

- **L2 TLB:** 512 entries, 4-way, 8 MSHRs, 4-cycle lookup, 8 requesters/entry; misses go to the PTW.
- **PTW:** 2 walkers, 16-entry walk cache, **2 levels (Sv32)**, `walk_latency` per level (default 150,
  total 300 per walk, swept 75–225 per level). Walks are issued through the same L2 as data — **the PTE
  traffic competes with data traffic**, which is the point of §4.11.
- **L2 cache:** 1 MiB, 8-way **[A]**, 64 B lines, MSHR-limited at 128, LRU. Sectoring not modelled.
- **DRAM:** `BANKS = 16`, `ROWS_PER_BANK = 1024`, timings `(tRCD, tRP, tCAS, tBURST)` configurable;
  scheduler FIFO or FR-FCFS (default **FR-FCFS**, because a FIFO-only DRAM handicaps the clairvoyant
  arms and would bias the M2 result). Row hits, row conflicts, bank conflicts and per-bank occupancy
  are counters.

### 4.11 Re-derived page-walk and PTE-traffic arithmetic (Sv32)

These supersede v3 §6.2 (b)–(d) **for this platform** and are the numbers the simulator is validated
against (§7.2). Inputs: `N = 16 384`, `Λ = 16 384` lines = 1 MiB of data, `w = 8`, `ℓ = 64`,
region = `P` contiguous pages.

**Method.** Walks are per distinct page: `W = P(1−e^(−N/P))` (v3 C26a, unchanged by the level count).
Each walk issues `levels = 2` PTE fetches. Distinct PTE lines: the leaf table holds 1024 PTEs =
16 lines, and the `W` leaf PTEs of a contiguous region occupy `ceil(W/16)` of them; the root table
needs `ceil(P/1024) ≤ 16` PTEs, i.e. one 64 B line for every `P` tested.

| `P` | walks `W` | PTE accesses `2W` | **logical payload** (`2W`×4 B) | distinct lines (leaf+int) | **cache-line traffic** (lines×64 B) | traffic % of 1 MiB | walk-bound vs data |
|---|---|---|---|---|---|---|---|
| 512 | 512 | 1 024 | **4.0 KiB** | 32+1 | **2.1 KiB** | 0.20 % | 4.7× |
| 2 048 | 2 047 | 4 095 | **16.0 KiB** | 128+1 | **8.1 KiB** | 0.79 % | 18.7× |
| 8 192 | 7 083 | 14 167 | **55.3 KiB** | 443+1 | **27.8 KiB** | 2.71 % | 64.8× |
| 16 384 | 10 357 | 20 713 | **80.9 KiB** | 648+1 | **40.6 KiB** | 3.96 % | **94.8×** |

**The two byte columns are different quantities and must not be interchanged.** *Logical payload* is
what the walker asks for: one 4 B PTE per issued fetch, i.e. `accesses × PTE_SIZE`. *Cache-line
traffic* is what the hierarchy actually moves: the distinct 64 B lines those fetches touch, i.e.
`lines × ℓ`. They differ for two structural reasons, both visible in the table:

1. **16 leaf PTEs share one 64 B line**, so traffic is 4× the distinct-PTE bytes per line fetched —
   which is why payload > traffic at every level (`80.9 vs 40.6 KiB` at `P = 16 384`).
2. **The interior PTE is re-fetched on every walk.** The root table contributes one distinct line but
   is read `W` times, so `payload − distinct-PTE bytes = (W−1) × 4 B` exactly — 41 423 B of the
   80.9 KiB at `P = 16 384` is the same root PTE read 10 356 times.

Which one matters where: **`transferred_bytes` reports cache-line traffic**, because that is what
competes with data for L2/DRAM bandwidth. Logical payload is the walker's *demand*, useful for
sizing the walker's issue rate and for predicting what a walk cache can absorb; a walk cache reduces
payload (fewer re-fetches) without necessarily reducing traffic (the lines are already resident).
Reporting either one as "PTE bytes" without saying which is the error this section exists to prevent.

Walk-bound time `= W / walkers × (levels × L_pt) = W/2 × 300`; data time `= Λ·ℓ / 64 B/cycle` =
16 384 cycles.

**Two conclusions differ from v3, and both go into every results table:**

- **PTE cache-line traffic is 2–3× smaller than v3's Sv39 figures** — 40.6 KiB vs 136 KiB at
  `P = 16 384` — because a Sv32 line holds 16 PTEs and there is one interior level. The qualitative
  finding is unchanged and slightly stronger: line traffic is **4 % of data at the extreme** (logical
  payload 7.9 %), so translation is a *throughput* problem, not a bandwidth one.
- **The walk-bound ratio is 64.8× / 94.8×, not v3's 264–386×.** v3 divides by 4 096 cycles, which
  implies 256 B/cycle of data supply, while the bandwidth v3 itself mandates is 64 B/cycle
  (102.4 GB/s). With one consistent denominator the ratio is 65–95×. "Translation dominates by ~2
  orders of magnitude" survives; the constant does not. Recorded as **C-4**.

---

## 5. Simulation methodology

### 5.1 Recommendation: cycle-based discrete-event simulation

**Decision: a single integer clock, components advanced every cycle, responses scheduled as events at
`cycle + latency`.** This is SimX's own model with the pipeline removed.

| Option | Verdict |
|---|---|
| Pure event-driven queueing (no clock) | **Rejected.** Cannot produce `cycles`, `outstanding[t]`, MSHR occupancy over time, or any back-pressure — i.e. it cannot answer the question. |
| Full cycle-accurate pipeline (decode, scoreboard, warp scheduling) | **Rejected.** The workloads are memory-bound gathers; pipeline detail would add ~10× the code and change no conclusion, while creating a large surface for unintentional bias. |
| **Cycle-based DES (chosen)** | Reproduces every §7.4 metric, keeps latency/bandwidth/queueing exact, and is ~5 kLOC. |

### 5.2 Determinism

Non-negotiable, because the study's inference rests on paired-by-seed comparisons:

1. Single-threaded; no wall-clock, no threads, no unordered containers in any iteration order.
2. All randomness from `std::mt19937_64` seeded explicitly; the workload RNG is consumed **once**, when
   materializing the trace.
3. Same-cycle events processed in insertion order (stable); components ticked in fixed index order.
4. Counters are `uint64_t` integers; the only floating point is in reporting, never in state.

### 5.3 Configuration

A flat `key = value` file, one namespace (`sim.*`, `mem.*`, `arm.*`, `wl.*`), parsed by ~80 lines with
defaults compiled in. Every resolved value is echoed to the output header for provenance, so a result
file is self-describing. No TOML/JSON dependency: the config surface is ~40 keys, and a dependency-free
build is a stated constraint.

### 5.4 Workload generation and replay

`workload = W0|W1|W2|W3` × `{Λ, P, ρ_dup, κ, L, N, seed}` → `Trace` (§4.2), with the realized
`Λ̂, P̂, λ̂, κ̂, ρ̂_dup, ρ̂_word, ρ̂_line` written into the output. Generation follows v3 §5 exactly:
`λ = Λ/P` lines per page at page-local stride `64/λ`; `ρ_dup = 0` is a permutation; `ρ_dup > 0` draws
from the solved `M`; `κ` selects regular vs divergent instructions with probability `κ`.

**Replay** is not "re-running the generator": both front-ends iterate the same immutable array.

### 5.5 Output

```
# one `key = value` line per counter, prefixed `simx.`
simx.cycles = 48213
simx.mem.transferred_bytes = 1114112
simx.tlb.dmmu.misses = 2047
simx.ptw.walks = 2047
simx.ptw.pte_fetches = 6141
...
# then a provenance block: every config key actually used, and realized workload stats
```
A driver script runs the matrix of `(arm, workload, seed)` and concatenates outputs; analysis is done
outside the simulator so the simulator stays a pure instrument.

### 5.6 Size estimate

| Area | LOC |
|---|---|
| clock, config, stats, trace | 700 |
| SIMT front-end + coalescer | 500 |
| engine front-end (descriptor, addrgen, table, remap) | 800 |
| TLB + L2 TLB + PTW | 900 |
| L2 cache + DRAM | 900 |
| unit tests | 900 |
| drivers/analysis | 300 |
| **Total** | **~5 000** |

---

## 6. Required metrics

Every metric of v3 §7.4, with the component that now produces it. The last column is the substantive
change: several rows v3 marked "requires new instrumentation" or "not available" become native.

| Metric | Producer | Status vs v3 |
|---|---|---|
| `cycles` | clock | native |
| `transferred_bytes` | L2/DRAM | native |
| `bw_achieved`, `bw_util` | derived vs 102.4 GB/s | native |
| `outstanding[t]` mean/p95/integral | slot pool sampler | native (v3 had no sampler) |
| `hit_L1`, `hit_L2` | caches | native |
| `row_hits`, `row_conflicts`, `row_hit_rate` | DRAM | native |
| `stall_slots`, `stall_tlb`, `stall_down` | front-ends + TLB | native (superset of v3's single `tlb_stall_cycles`) |
| `dyn_insts` | trace | native (memory instructions only) |
| `addr_gen_cycles` | EA gen + AG | native |
| `gen_lead` | engine vs SIMT timestamp | **native** (was "no such counter") |
| `dups_eliminated` (`dup_line`) | duplicate table | **native** |
| `line_efficiency`, `elem_efficiency` | derived | native |
| `tlb_stall_cycles` | L1 TLB | native (§12: definitions from P0) |
| `mshr_occ_tlb` time series | sampler | **native** (was "needs the tracer") |
| `dep_wait`, `t_dependency_chain`, `result_delivery` | trace + completion | native but **model-defined** — see R1 |
| energy, area | — | not produced (needs synthesis) |

**Not producible, and must not be claimed:** anything about RTL equivalence, silicon feasibility, or
power. Also `hit_L1` is meaningful only for the SIMT arms (the engine has no L1), exactly as v3 §7.4
requires.

---

## 7. Fairness and validation strategy

### 7.1 Fairness invariants — machine-checked every run

| ID | Invariant | Failure mode it prevents |
|---|---|---|
| **F-1** | Both front-ends read the same `Trace` object; byte-identical access sequence | two different address streams |
| **F-2** | One `MemorySystem` instance, one config; the arm cannot vary latency, bandwidth, cache or TLB size | hidden memory advantage |
| **F-3** | `unissued + issued_not_filled ≤ R` for every arm every cycle (v3 G4) | the v2 storage bug (audit F1) |
| **F-4** | Engine pays index-stream traffic, descriptor traffic, AG throughput, table lookups, LMEM writes | free accelerator |
| **F-5** | Baseline gets realistic coalescing, `R/S` slots per SM, hit-under-miss, and the **clairvoyant A3g control** | strawman baseline |
| **F-6** | `ρ_dup = 0` is a permutation — **exactly zero** duplicates (asserted, not expected) | v2's wrong duplicate model (audit F5) |
| **F-7** | Realized workload quantities are measured and reported; analytic values are never substituted | comparing a model to a distribution it did not generate |
| **F-8** | Any arm may only use information a plausible implementation could have (address-only → A2/A5; open-row → A3/A3g/A5*), and the information class is printed with the result | unknowable clairvoyance leaking into the "realistic" arms |

### 7.2 Validation tiers

**Tier 1 — unit tests (per component).** Coalescer: 12 known lane patterns → known line sets, including
`L=4/L=32` endpoints of `max(1, L·w/ℓ)`. TLB: capacity, associativity, LRU order, MSHR dedup, park/replay
order, stall predicate. Walker: 2-walker concurrency, walk-cache hit shortens the walk. DRAM: timings
produce the expected access latency for hit/miss/conflict.

**Tier 2 — analytic cross-checks.** The strongest external check available without RTL. Targets are
computed independently (before the simulator runs) and are the §4.11 table plus v3's closed forms.

| Check | Target | Tolerance |
|---|---|---|
| walks at `P` | `P(1−e^(−N/P))` | 5 % |
| PTE accesses | `2 × walks` | exact |
| PTE bytes | §4.11: 2.1 / 8.1 / 27.8 / 40.6 KiB | 2 % |
| walk-bound ratio | 4.7× / 18.7× / 64.8× / 94.8× | 5 % |
| duplicate fraction | `1 − (M/N)(1−e^(−N/M))` | 1 % of target |
| κ realization | `L_req = κ·max(1,L·w/ℓ)+(1−κ)·L` | exact, per instruction |
| streaming ceiling | `N·w/64` cycles | result in `[0.90, 1.005]×` |

**Tier 3 — known-answer tests (KAT).** Traces small enough that every counter is computed by hand and
written into the test as a literal, so a regression is a diff rather than a judgement:

| KAT | Hand-computed answer |
|---|---|
| 4 pages, 2-entry TLB, 4 accesses (one per page) | 2 misses, 2 evictions, 2 walks, 4 PTE fetches |
| 33 sequential lines, 16-entry 4-way LRU | 17 misses, 16 hits, exact eviction order recorded |
| 8 accesses to 1 line, 4-entry dup table | 1 slot alloc, 7 joins, 0 duplicate line requests |
| 32 lanes contiguous, `L=32` / `L=4` | exactly 4 / exactly 1 line requests |
| 3 pages miss in one cycle, 2 walkers | 2 walks start; the third starts exactly `walk_latency` later |
| 1 write after 1 read to the same line, `ρ_dup=0` | `dup_word = 0` exactly (permutation property, F-6) |

**Tier 4 — conservation identities, asserted every run.** These catch classes of silent loss that no
aggregate metric exposes:

```
requests_generated == requests_issued + requests_unissued         (unissued >= 0)
requests_issued    == requests_completed + requests_outstanding   (outstanding == 0 at end)
slot_allocs        == slot_frees + slots_live                     (slots_live == 0 at end)
walks              == walk_fills + walk_faults
pte_fetches        == levels * walks_begun - walk_cache_saves
tlb_reads          == tlb_hits + tlb_misses
l2_accesses        == l2_hits + l2_misses
```

**Tier 5 — source-conformance tests.** Behaviours transcribed from the Vortex tree and asserted as
literals, never fitted to data: ≤2 parked requests per miss-station entry (`REPLAY_DEPTH`); one miss
issued per tick over a single-request link (`UMMU_NUM_REQS = 1`); VPN dedup on the miss station;
replay in arrival order; L2 TLB 8 requesters/entry; 2 walkers; 16-entry walk cache; 4-cycle L2 TLB
lookup; 16-entry 4-way L1 TLB with 4 MSHRs. Each assertion cites the file:line it was transcribed
from, so a reviewer can check the transcription rather than trust it.

**Tier 6 — differential oracle.** Four components (L1 TLB+MSHR, L2 TLB, walker, L2 cache) are
implemented a **second** time in the most obviously-correct way available — fully associative, linear
scan, no indexing tricks — and the two implementations must agree **exactly** on 10⁶ random requests.
This replaces RTL equivalence for implementation bugs: it cannot show the model matches hardware, but
it removes "the fast version was wrong" as an explanation for any result.

**Tier 7 — invariant and null tests.** The null test of §7.4, the two-sided sanity check (a
crippled engine must lose, a crippled baseline must win), and v3 §10 P3 monotonicity
(`A3g ≥ A1`, `A4 ≥ A1`, `A5* ≥ A1`; effect vanishes at `κ = 1`).

**Tier 8 — sensitivity.** Every **[A]** parameter in §4.1 swept at ±2×; a conclusion that flips inside
that band is reported as parameter-dependent, not as a result.

### 7.3 Anti-strawman protocol

The realistic risk in this study is not a broken model, it is a *flattering* one. Three mechanisms
guard it: the clairvoyant `A3g` control (v3 §3.2) which removes the engine's visibility advantage; the
null test above; and the requirement that `A1 → A4` (both FIFO, both 128 slots, no scheduler) be
reported alongside the primary `A3g → A5*` pair, so a reader can see the answer without any
clairvoyance at all.

### 7.4 The M4 null test, specified

The null test runs in two stages. Both are blocking.

**Stage M4a — exact equivalence (zero tolerance).** The engine front-end is placed in a **neutral
profile** that removes every architectural difference it is legitimate for it to have, so that the two
front-ends are the same pipeline written twice:

| Knob | Neutral value | Why |
|---|---|---|
| `engine.l1_bypass` | **false** | the engine gets the same L1 tag/MSHR path; the real bypass is a property of the A/B comparison, not of the shared machinery |
| `tlb.client` | **dmmu** (16 entries, 4-way, 4 MSHRs, 2 parked/entry) | the ummu's 8 entries would deliberately handicap the engine in a test whose purpose is equality |
| `slots` | `R` for both, same allocation order | identical capacity (F-3) |
| `policy` | **FIFO** for both | no scheduler on either side |
| `dup_table.enable` | **false** | `ρ_dup = 0` has nothing to eliminate (F-6) |
| `ag.lanes` | **1 line/cycle** | same issue granularity as the coalescer path |
| descriptor/index traffic | attributed from the **same trace entries** | both pay the same index bytes (F-4) |
| completion rule | slot released on fill, same order | identical completion semantics |

Pass criterion: **every counter in §6 is identical**, and the completion sequence — the ordered list of
`(slot_id, completion_cycle)` — hashes to the same value. Not ±5 %: *identical*. A single differing
request anywhere means the shared machinery was instrumented twice in two different ways.

**Stage M4b — architectural null test (±5 %).** With the *real* engine configuration (`l1_bypass =
true`, `ag.lanes = 8`, FIFO, dup off) against the real baseline `A1`, on W1 at `ρ_dup = 0`:

| Family | Compared quantities | Tolerance |
|---|---|---|
| **Timing** | `cycles` (paired by seed) | **±5 %** |
| **Request counts** | generated, issued, accepted by L2, completed, outstanding at end (= 0) | ±2 % |
| **Line transactions** | L2 accesses, L2 misses, DRAM reads, `transferred_bytes` | ±2 % |
| **Translation events** | L1 TLB reads/hits/misses, L2 TLB reads/misses, `walks`, `pte_fetches` | ±2 %, walks **exact** |
| **Completion behaviour** | per-slot occupancy integral, mean/p95 latency per request, count of requests that ever stalled | ±2 %; p95 ±5 % |

`walks` is exact because it is a function of the page set, which is identical by construction (F-1):
any difference is a bug, not noise. A failure at either stage is reported with the *first* diverging
counter and the cycle it diverged at; the gates are never relaxed to make a milestone pass.

### 7.5 Replacement for probe P2 — and what is lost

v3's P2 compared SimX against **rtlsim** for one DXA cell. A standalone model has no RTL counterpart,
so P2 cannot run. The replacement is a ladder of five checks, strongest first, with an explicit
statement of what each does **not** establish:

| Replacement | Establishes | Does **not** establish |
|---|---|---|
| Tier 2 analytic identities | internal consistency with closed-form queueing/counting | that reality behaves like the closed form |
| Tier 3 KATs | component behaviour on hand-checkable cases | anything about scale |
| Tier 4 conservation | no request is silently lost, duplicated or stranded | that the *timing* is right |
| Tier 5 source conformance | we transcribed Vortex's mechanisms correctly | that Vortex is right, or that our timing matches it |
| Tier 6 differential oracle | our fast implementation matches a naive one | anything about hardware |

**Unavailable, permanently, in this direction:** RTL/silicon equivalence for any arm; any claim that
the engine's timing matches a real DXA+bulk implementation (none exists); any cycle-level validation
against SimX itself (it does not build here, and its arms do not exist either — U-a). Every engine
result is therefore a **modelled** claim, and the paper must say so in the abstract, not in a
limitations paragraph.

The one external anchor that survives is **static**: the parameters in §4.1 marked **[V]** are read
from the Vortex tree, so a reader can verify that the model's *capacities and structures* match a real
GPU design, even though its *timing* cannot be verified against one.

### 7.6 G8a: a Vortex finding versus an assertion about our model

v3's probe P1 contains gate G8a — "the ummu is engaged" — with the consequence "if the DXA never
translates, Hypothesis B is void". In SimX that was an empirical question. Here it must be split into
two statements of different epistemic status:

| | Statement | Status | Evidence |
|---|---|---|---|
| **S1** | The DXA read path is translated by a *separate* 8-entry unit MMU, distinct from the per-core dMMU, and that unit exists only when `VM ∧ DXA` | **Architectural finding about Vortex** | verified from source, not from simulation: `VX_socket.sv:855-880` (ummu), `:893-899` (`UTLB_SIZE = 8`, one request lane), `constants.h:96-98` (existence condition), `VX_config.toml:212` |
| **S2** | In our model, the engine front-end engages its ummu in the same regime (misses when `P` exceeds the uTLB reach) | **Assertion about our own code** | Tier 4 conservation + Tier 3 KAT + a config sweep showing misses → 0 as `P` → 1 page |

**What is preserved.** The *useful content* of G8a is S1: the architectural fact that indirect-access
translation is a separate, smaller-TLB, single-request-lane structure — which is precisely why
Hypothesis B (bulk translation) is worth testing at all, and why the B-family's 8-entry uTLB is a
plausible bottleneck. **S1 is already settled by reading the source**, and it does not need a
simulation to be true. S2 is a self-check that our front-end exercises the path we claim; it can fail
only because we wrote the model wrong, and its failure mode is a bug report, not a finding.

**What is lost.** In SimX, a zero miss count would have been evidence about *Vortex*. In the
standalone model it is evidence about *us*. Concretely: the standalone study can no longer discover
that the DXA fails to translate; it can only assert that its own model does. Any text relying on G8a
must therefore cite S1's source lines for the architectural claim and label S2 as an implementation
check (consistency item **C-3**).

---

## 8. Proposed implementation milestones

Each milestone ends in a gate. **No later milestone starts until its predecessor's gate passes.**

| M | Content | Gate (numeric) |
|---|---|---|
| **M0** | clock, config, stats, trace generator, no memory | trace byte-identical over 100 runs; `Λ̂, P̂, λ̂, κ̂, ρ̂` within 0.5 % of targets; `N·w` bytes exact |
| **M1** | L2 + DRAM + slots, **no TLB**; W0 and W0-aligned W1 | W0 in `[0.90, 1.005]`× the streaming ceiling; DRAM row-hit rate monotone in locality |
| **M2** | L1 TLB + L2 TLB + PTW | walks within 5 % of `P(1−e^(−N/P))` at `P ∈ {2048, 8192}`; PTE bytes within 2 % of the §4.11 table (8.1 / 27.8 KiB); `walks` exact vs page set; ≤2 concurrent walkers |
| **M3** | full baseline (coalescer, `R/S` slots, hit-under-miss) on W1/W3 | W1 cycles monotone in `P`; MSHR occupancy within 15 % of the Little's-law estimate; stall accounting sums to total cycles |
| **M4a** | engine in the **neutral profile** (§7.4) | **every counter identical** and completion-sequence hash equal. **Failure blocks M4b and M5.** |
| **M4b** | same engine, real config (L1 bypass, `AG_LANES=8`), FIFO, dup off | `\|A4 − A1\| ≤ 5 %` cycles; the four counter families within ±2 %, `walks` exact (§7.4). **Failure blocks M5.** |
| **M5** | engine policies (`A5`, `A5*`), duplicate table (M3 ablation), `A3g`/`A2` SIMT windows | `dup_line` reduction within 2 % of realized; monotonicity of P3 holds |
| **M6** | W2/W3 breadth, seeds, sensitivity sweeps, analysis driver | paired-by-seed Δ with CI; every **[A]** parameter swept |

Estimates: M0 0.5, M1 1.5, M2 1.5, M3 1, M4 1, M5 1, M6 0.5 — about **7 working units**, roughly half
the effort of porting and instrumenting Vortex, and with no build-system work at all.

---

## 9. Risks and limitations

| ID | Risk | Severity | Mitigation / disclosure |
|---|---|---|---|
| **R1** | No pipeline: `cycles` is a memory-system cycle count, not an application runtime | **High** | Report as "memory-system cycles"; validate W0 against the ceiling; never claim speedup for a compute-bound workload |
| **R2** | Accelerator is **unvalidated** — there is no RTL to compare with | **High** | Every engine result is labelled *modelled*; `B3`/`A5*` claims are conditional on review; state that no silicon exists |
| **R3** | DRAM model approximates a real controller | Med | Parameterized timings; FR-FCFS default; row metrics reported so readers can judge; sensitivity sweep |
| **R4** | Walk latency (300 cy) drives the M4 and B-family results | Med | Swept 75–225 per level (150–450 total), spanning a 32–190× walk-bound ratio; conclusions must hold at both ends of the band |
| **R5** | Synthetic workloads, not applications | Med | W2/W3 add irregular structure; external validity explicitly bounded in the paper |
| **R6** | Baseline may be inadvertently weak | Med | Clairvoyant `A3g` control + blocking null test + `A1→A4` reported alongside |
| **R7** | "Same trace for both front-ends" fixes access order | Low | Address generation is charged explicitly, so the order is a *modelled* cost, not a hidden assumption |
| **R8** | ~~Translation-level change shifts PTE counts~~ | — | **Resolved:** Sv32 adopted (§4.1.1) and arithmetic re-derived (§4.11) |
| **R9** | Results diverge from v3's published numbers (136 KiB, 264–386×) | Med | Expected and intended: §4.11 supersedes them with Sv32 figures (40.6 KiB, 64.8×/94.8×). Every comparison against v3 must use the §4.11 column, never v3 §6.2 |

---

## 10. Go/no-go criteria for implementation

Implementation starts only when **all** of the following hold:

1. **This document reviewed and approved**, with the arm set frozen (`A0…A5*+D`, `B0…B5`, `C0/C1`).
2. **§4.1 parameter table approved**, each value tagged **[V]** or **[A]**, and the sweep bands for the
   **[A]** values agreed.
3. **Consistency items resolved:** C-1 by §4.1.1 (Sv32 adopted, arithmetic re-derived in §4.11), C-2 by
   §7.5 (P2 replacement, with its losses stated), C-3 by §7.6 (G8a split into S1/S2), C-4 recorded in
   §4.11 (walk-ratio denominator).
4. **The null test (M4) accepted as a blocking gate** — i.e. the project agrees in advance that a
   > 5 % gap between two identically-configured front-ends invalidates the comparison.
5. **Agreement that engine results are modelled claims**, not hardware results, and will be described
   that way in any write-up.
6. **Effort accepted**: ~5 000 LOC, 7 working units, M0–M6.
7. **Scope accepted**: no ISA, no pipeline, no energy/area, single socket, no coherence.

If (4) or (5) is unacceptable, the study should not proceed in this form: without the null test the
comparison has no credibility, and without the modelled-claim label the results would be overstated.

---

## 11. Relationship to Vortex

### 11.1 Reproduced (because the result depends on it)

| Mechanism | Why it must be faithful |
|---|---|
| L1 TLB with 4 MSHRs, VPN dedup, ≤2 parked/entry, replay in order | the entire M4 result is a TLB-stall result |
| Shared L2 TLB 512/4-way/8 MSHR/4 cyc | determines when translation saturates |
| 2 walkers + 16-entry walk cache | sets walk throughput, hence the 64.8–94.8× walk-bound ratio (§4.11) |
| 64 B lines, 16 KiB/4-way L1, MSHR-limited caches | sets coalescing and MLP |
| Virtual addressing for global accesses; engine uses its own 8-entry TLB | v3 §2.1's two-client structure |
| DRAM bank/row behaviour with a real scheduler | row-hit ordering is M2's whole premise |

### 11.2 Simplified

| Vortex | Here | Risk |
|---|---|---|
| Full RISC-V pipeline, scoreboard, warps | fixed 1-instruction/cycle issue per warp | R1 |
| Permission/fault semantics on translation | not modelled — the workloads never fault | none for read-only gathers |
| Cache sectoring, dirty-byte tracking, writeback detail | simple LRU + MSHR | low for read-dominated gathers |
| Multi-socket, cluster arbitration | one socket, one arbiter | none for the question asked |

### 11.3 Omitted (unnecessary)

ISA decode, FPU/SFU, graphics units (TEX/RTU/OM/raster), TCU/TFU, DTM/debug, DCRs, ELF/HTB loading,
Verilator/RTL co-simulation, the `configure`/`gen_config` machinery, and every third-party dependency
(softfloat, ramulator, cocogfx).

### 11.4 Assumptions that could materially change conclusions

1. **No compute/memory overlap** — both sides equally penalized, but a real GPU hides gather latency
   behind other warps; absolute cycles are therefore pessimistic for both.
2. **DRAM policy** — FR-FCFS favours the clairvoyant arms; a FIFO controller would shrink the M2
   result. Both must be run.
3. **Walk latency** — dominates M4 (§9 R4).
4. **Duplicate-table capacity (256 vs `Λ` = 16 384)** — sets how much M3 can win; must be swept.
5. **`AG_LANES = 8`** — sets the engine's issue rate; swept at M6.

### 11.5 Source-verified vs modelled — the labelling rule

Every number in a results table carries one of: **[V]** read from Vortex source at `7300bc8`;
**[A]** a modelling assumption; **[M]** measured by this simulator. Mixing these in prose without
labels is prohibited, because the study's earlier audit (v2→v3) failed precisely by blurring a derived
quantity with a measured one.

---

## 12. Migration from the current approach

Nothing is deleted or modified. The recommendation per artefact:

| Artefact | Recommendation | Rationale |
|---|---|---|
| **P0 instrumentation diff** (16 files, +331/−5 in `ref/vortex`) | **Preserve, do not revert, do not extend.** Mark dormant. | It is the only *measured* evidence for Vortex's translation counters, it is a clean reviewable patch, and it becomes usable unchanged if a Linux host ever appears. Reverting would destroy work; extending it would spend effort on a platform we are leaving. |
| **P0 report + test harness** | **Preserve.** Reuse the *definitions* and the *test style*, not the code. | The counter definitions (`stall_cycles` predicate, `mshr_joins`, occupancy point-sample, PTE-accesses-vs-bytes) transfer directly into §4.5/§6. The harness itself tests SimX internals and does not port. |
| **v3 experiment design** | **Preserve as the experimental contract.** The simulator implements it; it does not replace it. | Arms, `R = 128`, generator, metrics, gates and the probe all remain valid. Two items must be re-scoped (below), and the rest is adopted verbatim. |
| **recon / gap-analysis / audit docs** | **Preserve as the parameter source.** | §4.1's **[V]** values come from them; they are the provenance chain for every hardware number. |
| **`ref/vortex` clone** | **Preserve, read-only.** | Still the architectural reference. |

### Consistency items to resolve before implementation (not edits to be made now)

- **C-1 — translation levels. RESOLVED.** Reference configuration is `XLEN = 32`, hence **Sv32**
  (`VX_types.toml:73-76`, confirmed in the generated header). v3's Sv39 assumption is valid only for an
  XLEN=64 build, which v3 never declares. The simulator reproduces Sv32 and **v3 §6.2 (b)–(d) is
  superseded** by §4.11. XLEN=64 was explicitly rejected: adopting it to preserve v3's arithmetic would
  tune the model to the numbers rather than derive the numbers from the model.
- **C-4 — the walk-ratio denominator. NEW.** v3's `264–386×` divides walk time by 4 096 cycles, i.e. it
  assumes 256 B/cycle of data supply, while the bandwidth v3 mandates elsewhere is 64 B/cycle. On one
  consistent denominator the ratio is **64.8× / 94.8×** (§4.11). The qualitative claim (translation
  dominates by ~2 orders of magnitude) is unchanged; the constant is not. No v3 text is edited; every
  comparison must cite §4.11 instead.
- **C-2 — probe P2 loses its meaning. RESOLVED by replacement.** v3 §10's P2 compares SimX against
  **rtlsim**; a standalone model has no RTL counterpart, so P2 cannot run. The replacement ladder is
  §7.5 (analytic → KAT → conservation → source conformance → differential oracle), with the losses
  stated explicitly rather than implied. No RTL/silicon equivalence is claimed anywhere.
- **C-3 — probe P1's G8a changes character. RESOLVED by split.** G8a is separated (§7.6) into **S1**, an
  architectural finding about Vortex settled from source alone (`VX_socket.sv:855-880,893-899`,
  `constants.h:96-98`), and **S2**, an assertion about our implementation, checkable only by our own
  tests. The study can no longer *discover* that the DXA fails to translate; it cites S1's source lines
  instead, and labels S2 as a self-check.

---

## Recommendation

Adopt this design. It is the smallest model that can still answer the question: it preserves every
fairness mechanism v3 fought for (the `R = 128` shared budget, capacity-matched and visibility-matched
arms, permutation-duplicates, realized-quantity reporting), it makes the four previously-unmeasurable
metrics native, and it removes the platform dependency entirely — on a host where the full simulator
cannot be built and, more decisively, where the arms do not exist to be run.

The two things to approve explicitly before any code: the **blocking null test** (§10.4) and the
**modelled-claim label** (§10.5). Everything else in the plan is reversible; those two determine
whether the eventual result means anything.
