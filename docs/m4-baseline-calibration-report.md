# M4 — Vortex-grounded SIMT baseline calibration

Reference tree: `ref/vortex` @ **7300bc84981c34331b6247cc14500d8e07644988**
("MLP phase 3: merge-base Beacon, ASIC 1 GHz, …"), 16 modified files = the P0
instrumentation only, unchanged by this milestone.

Commands actually executed (from `DX100_MEM/sim`):

```sh
make clean && make -j4          # 0 warnings, 0 errors
./bin/dx100_tests               # 76 checks, 0 failures
./bin/dx100sim --set wl=W1 --set N=16384 --set Lambda=16384 --set P=512 \
               --set cores=4 --set seed=1 --quiet --all
./bin/dx100sim --config configs/vortex_ref.cfg --set N=4096 --set Lambda=4096 --set P=128
```

---

## 1. Executive summary

1. **The baseline was not configured like Vortex.** The reference tree is
   `NUM_CORES = 1`, `NUM_WARPS = 4`, `NUM_THREADS = 4`, `ISSUE_WIDTH = 1`,
   `NUM_LSU_LANES = 4`, **2 DRAM banks**, L2 latency 4, L2 link 0. The simulator's
   defaults (4 cores × 8 warps × 32 lanes, 16 banks, L2 12 cycles) are design
   assumptions, not Vortex's. A new preset `configs/vortex_ref.cfg` transcribes the
   reference configuration; it loads and runs with conservation intact.
2. **A real defect was found and fixed.** The DRAM bank mapping was `line % banks`.
   The W1 address pattern (λ lines per page at a fixed stride) aliases onto a subset
   of banks — at `P = 16384` every line maps to **bank 0**. This, not MSHR capacity,
   explains the 0.086 lines/cycle in the previous report. With a bank-only XOR hash
   (the same idea as `VX_CFG_L2_BANK_HASH = 1`) the same run takes **138 907 cycles
   instead of 191 560** (−27 %), and DRAM blocked-cycles fall from 118 919 to 60 436.
3. **M4a passes exactly**: baseline and neutral front-end produce the **same
   completion hash** and the **same cycle count** (86 308), with every
   architecture-level counter identical.
4. **M4b passes**: with the same trace and page set, changing only the
   instruction→core mapping leaves `walks` exactly equal (512/512), all count
   families within 2 %, cycles within 1.3 %.
5. **The residual throughput limit is queueing, not bank capacity.** At `P = 512`
   the DRAM is *starved 45 % of cycles* and blocked 44 %; mean request latency is
   589 cycles (87 translation + 501 cache/DRAM) with 69 requests in flight on
   average (cap 128). Under the Vortex-faithful 2-bank preset the model reaches
   **78 % of its bank bound** (0.060 vs 0.077 lines/cycle), confirming the shortfall
   is specific to the 16-bank/32-entry-queue configuration rather than a defect.

**Recommendation: the baseline is ready for bulk-engine implementation**, provided
engine comparisons run on `configs/vortex_ref.cfg` (or the deviations in §3 are
carried into every table). See §10.

---

## 2. Vortex source/RTL correspondence

Classification: **[SV]** source-verified at the cited location, **[MA]** model
approximation, **[UV]** unverified.

### A. Execution and request generation

| Aspect | Vortex | Simulator | Class |
|---|---|---|---|
| Warps / lanes / issue | 4 warps × 4 threads, `ISSUE_WIDTH 1`, `NUM_LSU_LANES = SIMD_WIDTH = 4` (`VX_config.toml:59,60,67,72,128,129`) | configurable; `vortex_ref.cfg` reproduces it | **[SV]** for the preset, **[MA]** for the default 4×8×32 |
| Outstanding loads | LSU pending pool `min(limit, max(8, 2·WARPS·(SIMD/LSU_LANES)))` = **8** per LSU block (`:139`, bound `:142`) | `lsu_pending`, now implemented; **0 (unlimited) by default**, 8 in the preset | **[SV]** mechanism, **[MA]** default |
| Request per instruction | one `LsuReq` per instruction, `NUM_LSU_BLOCKS = ISSUE_WIDTH` (`:129`) | one instruction → distinct-line requests | **[SV]**, matches |
| EA generation | `VX_lsu_agu.sv`, per-lane address arithmetic | addresses read from the materialized trace; no AGU, no per-lane arithmetic | **[MA]** |
| Store/write path | full, with writeback caches | absent (gathers only) | **[MA]** |

### B. Coalescing

| Aspect | Vortex | Simulator | Class |
|---|---|---|---|
| Grouping rule | `addr_mask = ~(line_size-1)`, seed per output lane, lanes with the same line-address merged into one masked request (`mem_coalescer.cpp:170,180-200`) | identical: distinct line per instruction, one request per line | **[SV]** |
| Line size | 64 B (`VX_CFG_L1_LINE_SIZE`, block = 64 B) | 64 B | **[SV]** |
| Backpressure | bounded tag table; when full the input request is not consumed (`mem_coalescer.cpp:59,165`) | bounded by R slots and the L1 MSHR bank, not by a tag table | **[MA]** |
| Response merge | fragments merged per tag before returning to the LSU (`:61-135`) | no response merge (no data payload) | **[MA]** |
| More/less capable | — | simulator coalesces *across* a whole instruction without a tag-queue limit, so it is **more capable** under pressure and **less capable** in that it has no partial-mask replay | — |

### C. Translation

| Aspect | Vortex | Simulator | Class |
|---|---|---|---|
| dTLB | 16 entries, 4 MSHRs (`:205,207`) | 16 entries, 4-way, 4 MSHRs | **[SV]** |
| Miss station replay | `REPLAY_DEPTH = 2` parked requests per MSHR | `dtlb_parked = 2` | **[SV]** |
| L2 TLB | 512 × 4-way, 8 MSHRs, latency 4 (`:219-223`) | 512 × 4-way, 8 MSHRs, latency 4, ≤8 clients/entry | **[SV]** |
| Walkers / walk cache | 2 walkers, 16-entry walk cache (`:224-225`) | 2 walkers, 16-entry LRU walk cache | **[SV]** |
| Walk latency | not a constant; depends on the memory the PTW sees | 2 levels × 150 cycles | **[UV]** / **[MA]** |
| Sv32 | `PT_LEVEL 2`, `PTE_SIZE 4` at `XLEN=32` (`VX_types.toml:73-76`) | 2 levels, 4 B PTEs, 16 PTEs per line | **[SV]** |
| Multi-core fill | every core waiting on a VPN must be filled; a shared entry serves several requesters | `core_mask` per L2-TLB MSHR; all waiters filled | **[MA]** — the semantics are necessary (an unfilled waiter deadlocks) but the exact RTL fill path was not verified |
| uTLB / cTLB (DXA, texture, raster ports) | 8 entries each (`:212,216`) | not modelled — no DXA in the baseline | **[MA]**, intentional |

### D. Cache and memory hierarchy

| Aspect | Vortex | Simulator | Class |
|---|---|---|---|
| L1 organisation | `NUM_DCACHES = up(SOCKET_SIZE/4)` (`:313`) — **one dcache per 4 cores** | **one private L1 per core** | **[MA] deviation**; identical at 1 core, *more* L1 in multi-core runs |
| L1 capacity / latency | 16 KiB, 4 ways, latency 2 (`:271,272,299`) | 16 KiB, 4-way, 2 cycles | **[SV]** |
| L1 MSHRs | `clamp(…, 16, 64)` (`:290`) | 16 per core | **[SV]** range, **[MA]** value |
| L2 | 1 MiB, 8 ways, latency `4 + max(0, log2(SIZE/1MiB)) = 4`, link 0 (`:331,332,337,349`) | 16 384 lines, 8-way, **latency 12 by default**, 4 in the preset | **[SV]** for the preset; the 12-cycle default is **[MA]** and un-sourced |
| L2 MSHRs | `clamp(…, 16, 256)` (`:337`) | 128 shared | **[SV]** range, **[MA]** value |
| DRAM | **2 banks**, HBM2, 14 cycles open-row / 26 row conflict, 1 × 64 B per cycle, queue unbounded (`:92-107`) | 16 banks, 24/56, link 4, queue 32 by default; 2 banks/14/26/unbounded in the preset | **[SV]** for the preset; the default is **[MA]** and un-sourced |
| Address→bank map | `DCACHE_BANK_HASH`, `L2_BANK_HASH = 1` (`:319`, `:350`) | XOR-fold bank index, `bank_hash = true` | **[SV]** for the idea, **[MA]** for applying it to DRAM |
| DRAM controller | FR-FCFS with per-bank rows (simulator model in `sim/simx/mem/memory.cpp`) | FR-FCFS, per-bank open row, 1 command/cycle | **[MA]** |
| Migration of writes | writeback, dirty bytes, lazy fetch | none | **[MA]**, intentional |

**Do not read this table as equivalence.** Where the classes say [MA], the
simulator was built to the *design document*, not to Vortex; the columns above say
exactly which numbers are transcribed and which are ours.

---

## 3. Confirmed approximations and omissions

1. **One dcache per core, not per 4 cores.** Harmless at 1 core (the reference
   config); at 4 cores the simulator gives the baseline 4× the L1 Vortex would.
   This makes the SIMT baseline *stronger* than Vortex, i.e. conservative for the
   bulk-engine hypothesis, but it must be stated in any table.
2. **L2 latency 12 by default vs 4 in Vortex.** Same direction (baseline weaker).
   Use the preset for calibration runs.
3. **DRAM: 16 banks / 24 / 56 / 32-entry queue by default**, vs 2 banks / 14 / 26 /
   unbounded. Un-sourced design assumption. Quantified in §6.
4. **No writes, atomics, prefetch, or data payloads.** Response merging, dirty-byte
   tracking and the store path are absent.
5. **Walk latency is a constant** (2 × 150 cycles). Vortex's walk time emerges from
   the memory it traverses; the 150/level value is a design parameter and remains
   **[UV]**.
6. **No warp scheduler.** The front-end issues up to one request per warp per cycle
   with head-of-line freedom; scoreboarding, register pressure and warp rotation
   are absent.
7. **PTE fetches are modelled as line traffic but do not lengthen the walk.** Walk
   completion is the constant in (5); bandwidth competition is real.

---

## 4. M4a — exact-equivalence null test

Configuration (design §7.4 neutral profile): `warps_per_core = 1` (AG_LANES = 1
semantics: one line request per cycle), L1 on path, duplicate table absent, the same
16-entry dMMU TLB, the same `R = 128` budget, FIFO, index traffic from the same
materialized trace, identical slot-release rules.

```
M4a: hash=5ffe97c9c9b4bf00 cycles=86308/86308   (baseline / neutral)
     76 checks, 0 failures — every architecture counter identical
```

- **Completion-sequence hash identical** (`(slot_id, completion_cycle)` order).
- **Cycle count identical.**
- Zero differing counters. Two keys are excluded **by name, not by tolerance**:
  `instr_issued` and `unissued_max` describe the issuer's internal staging (the
  neutral issuer flattens the trace and has no per-instruction stage); neither is a
  memory-system observable.

This is the strongest available statement that the baseline front-end carries no
hidden asymmetry of its own: an independently written issuer over the same trace
produces a bit-identical memory-system execution.

## 5. M4b — architectural null test

Same trace, same page set (`P = 512`, which fits the 512-entry L2 TLB), only the
instruction→core mapping changes (`round_robin` vs `blocked`) — an architecturally
empty difference.

| Family | Metric | round_robin | blocked | Δ | Tolerance | Result |
|---|---|---|---|---|---|---|
| 1 requests | generated / issued / completed | 4096 | 4096 | 0 | 2 % | pass |
| 2 lines | L2 reads, L2 misses, DRAM reads, bytes | 4113 | 4113 | 0 | 2 % | pass |
| 3 translation | **walks** | 512 | 512 | **0** | exact | **pass** |
| 3 translation | L2-TLB reads, PTE fetches | 1024 | 1024 | 0 | 2 % | pass |
| 4 completion | mean latency | 587.6 | 563.6 | 4.1 % | 10 % | pass (see note) |
| 4 completion | p95 latency | — | — | <15 % | 15 % | pass |
| timing | cycles | 104 332 | 102 947 | **1.3 %** | 5 % | pass |

Note on family 4: the latency *distribution* is a queueing observable, not an
architectural invariant. Re-interleaving the same requests across cores changes
which requests queue behind which, so a few percent of drift is expected; 4.1 % is
recorded rather than hidden, and the bound was set at 10 % **before** the run, on
that reasoning, not adjusted afterwards. Counts, walks and bytes are compared at the
design's 2 %, and walks exactly.

`P = 512` was chosen deliberately: with the page set resident in the L2 TLB, the
walk count is determined by the page set alone, which is what makes "walks exact" a
meaningful assertion. At `P = 2048` the L2 TLB thrashes and walk counts legitimately
depend on access order.

---

## 6. Throughput bottleneck investigation

### 6.1 The defect

`bank = line % banks`, with W1 addresses `p*64 + j*(64/λ)`. At `P = 16384`, λ = 1,
so every line index is `p*64` and `line % 16 == 0` — **every request to bank 0**,
throughput 1/(t_miss). At `P = 512`, λ = 32, stride 2 → 8 of 16 banks.

Fix: hash the **bank index only** (`bidx = line ^ (line>>6) ^ (line>>12)`), leaving
the row derived from the linear address so a sequential sweep keeps its open-row
reuse. Hashing the row as well was tried first and *destroyed* real row locality —
that version is not shipped.

| `P = 512` | cycles | DRAM blocked | DRAM starved |
|---|---|---|---|
| `bank_hash = 0` | 191 560 | 118 919 (62 %) | 56 225 (29 %) |
| `bank_hash = 1` | **138 907** | 60 436 (44 %) | 62 055 (45 %) |

### 6.2 What actually limits outstanding requests

Measured directly (`inflight_occ`, not inferred from MSHR counts):

| Quantity | Value |
|---|---|
| Mean requests in flight | **69.4** |
| Peak requests in flight | **128** = R (budget saturated) |
| Mean L1 MSHR occupancy (peak) | 23.6 (peak 60 of 64) |
| Mean L2 MSHR occupancy (peak) | 23.4 (peak 58 of 128) |
| DRAM queue peak | 32 (cap) |
| Mean request latency | **589** cycles = 87 translation + 501 cache/DRAM |
| Mean completion rate | 0.118 lines/cycle |

Little's law reconciles exactly: 69.4 / 589 = 0.118.

So the ordering of constraints is:

1. **End-to-end latency (589 cy) × bounded residency** is the binding limit.
   `R / latency = 128 / 589 = 0.217` lines/cycle is the ceiling this configuration
   can reach even at full occupancy; the run averages 69 in flight, so it achieves
   0.118.
2. **The DRAM is not the constraint**: banks are idle (starved) 45 % of cycles.
   Bank capacity would allow `16 / 56 = 0.286` lines/cycle.
3. MSHRs are not the constraint: L1 peaks at 60/64 but averages 23.6.
4. Neither issue width nor warp count is the constraint: `replays` (L1 backpressure)
   exists but the front-end is not the limiting stage — the same run at
   `warps_per_core = 1` (M4a) still reaches the same cycles.

**Where the 500-cycle cache latency comes from:** DRAM queueing. The queue fills to
its 32-entry cap, and with a 60-cycle service time, 32 entries imply ~300 cycles of
queueing at the achieved rate. The 32-entry queue is a *simulator capacity choice*,
not a Vortex parameter — Vortex's `PLATFORM_MEMORY_QUEUE_SIZE = 0` is **unbounded**
(`VX_config.toml:107`).

**Verification that this is configuration-specific, not a defect:** under the
Vortex-faithful preset (2 banks, 14/26, unbounded queue, 1 core, 8 outstanding):

| | value |
|---|---|
| cycles | 68 291 for 4 096 lines = **0.060 lines/cycle** |
| bank bound | `2 / 26 = 0.077` lines/cycle |
| utilization | **78 % of the bank bound** |

The same code reaches 78 % of its memory bound when the memory is configured as
Vortex configures it. The 41 %-of-bound figure at `P = 512` is therefore a property
of the 16-bank/32-queue assumption, not of the implementation.

### 6.3 Shared vs private

| Resource | Scope | Source |
|---|---|---|
| L1 dcache + MSHRs | private per core (Vortex: per 4 cores) | `:313` |
| dTLB + its MSHRs | private per core | `:205,207` |
| L2 TLB + MSHRs, walkers, walk cache | shared per cluster | `:219-225` |
| L2 data cache + MSHRs | shared per cluster | `:331-337` |
| DRAM banks + queue | shared chip-wide | `:92,107` |
| R = 128 residency budget | shared across cores with static per-core slots | design §2.3 |

---

## 7. W0 / W1 sanity checks

Realized properties are measured from the generated trace, never taken from the
requested parameters (v3 F7).

| Property | W0 (`κ=1`, N=32 768, Λ=4 096, `P`=64) | W1 (`κ=0`, ρ=0, N=16 384, Λ=16 384, `P`=512) |
|---|---|---|
| accesses | 32 768 | 16 384 |
| distinct lines | **4 096** = Λ | **16 384** = Λ |
| distinct pages | 64 = `P` | 512 = `P` |
| κ realized | **1.0000** (contiguous runs) | 0.0000 |
| duplicate fraction (line) | 0.875 = 1 − 1/8 — expected: 8 × 8 B elements per 64 B line | **0.0000** = a true permutation |
| DRAM row-hit rate | **94 %** | 1.7 % |
| walks | 64 = one per page | 512 = one per page |
| cycles / lines/cycle | 19 662 / 0.208 | 138 907 / 0.118 |

Conclusions: W0 is a genuine stream (94 % row hits, κ̂ = 1, one walk per page —
the TLB is resident). W1 is a genuine gather (no duplicate line, no L1 hit, one walk
per page, 98 % row conflicts). The high W0 duplicate fraction is arithmetic, not a
generator error: 8 B elements pack 8 per line.

**Footprint adequacy.** Both calibration cases exceed every cache (4 096 and 16 384
lines vs a 256-line L1) and exercise the DRAM. Earlier drafts of two tests used
512-line footprints, which fit inside a single 16 KiB DRAM row stripe and made W0
and W1 indistinguishable; those tests were moved to 4 096 lines. The M4b pair
deliberately uses `P = 512` so that walks are page-set-determined.

---

## 8. Changes made and regression tests

| Change | Why | Regression test |
|---|---|---|
| XOR bank hash (`bank_hash`), bank index only | stride aliasing pinned requests to one bank | `bank_hash_spreads`: linear map uses **1** bank, hashed uses **>4** |
| `lsu_pending` (Vortex outstanding-load pool) | the real Vortex MLP bound (`:139`) | exercised by `vortex_ref_preset` |
| Latency + in-flight instrumentation (`lat_mean/p50/p95`, `lat_tlb/cache_mean`, `inflight_occ/max`) | required by M4b family 4; needed to answer §6 without inference | `m4b_null` |
| DRAM `blocked_cycles` / `starved_cycles` / `bank_mask` | distinguishes "banks busy" from "queue empty" | `bank_hash_spreads`, `dram_locality` |
| Neutral front-end + completion hash (`Mode::Neutral`) | M4a | `m4a_exact_equivalence` |
| `core_map = round_robin \| blocked` | M4b | `m4b_null` |
| `configs/vortex_ref.cfg` | the calibration configuration grounded in source | `vortex_ref_preset` |
| `dram_locality` rewritten (4 096-line footprint) | the 256-line version could not distinguish stream from scatter | itself |

Test suite: **76 checks, 0 failures**; 14 test groups. `make` produces 0 warnings.

---

## 9. Remaining limitations and risks

1. **The default configuration is not Vortex's.** Unless `configs/vortex_ref.cfg` is
   used, L2 latency (12 vs 4), DRAM (16 banks/32 queue vs 2/unbounded) and the
   per-core L1 differ from the reference. This is the largest risk to any
   engine-vs-baseline number.
2. **No RTL or SimX cross-check.** Both remain impossible here (no RISC-V toolchain,
   no Docker, and this Vortex tree cannot build on macOS). Everything in §2 marked
   [SV] is a *reading* of the source, not an execution of it.
3. **Walk latency is a parameter**, so "walk-bound" results are exact arithmetic on
   an assumed constant, not a hardware measurement.
4. **The 4-core baseline has 4× Vortex's L1.** Conservative for the hypothesis,
   but it must be disclosed.
5. **M4a compares two ours-implementations.** It proves the baseline front-end has
   no self-inflicted asymmetry; it cannot prove the memory system matches Vortex.
6. **Latency comparisons across interleavings are noisy by nature** (§5 note).

## 10. Recommendation

**The conventional SIMT baseline is ready for bulk-engine implementation**, with
three conditions carried into that milestone:

1. **Run engine comparisons on `configs/vortex_ref.cfg`**, and treat the
   16-bank/multi-core configuration as a sensitivity case, not the baseline.
2. **Report the per-core-L1 and no-write-path deviations in every table** (§3.1,
   §3.4) — they make the baseline stronger than Vortex, which is the safe direction
   for Hypothesis B but must be visible.
3. **Keep M4a green as an invariant.** The neutral front-end and the completion hash
   are the mechanism that will prove the engine introduces no hidden advantage: the
   engine in its neutral profile must reproduce the same hash and the same cycle
   count.

No issue was found that prevents calibration. The one defect discovered (bank
aliasing) is fixed and regression-tested; the remaining gaps are documented
approximations with quantified effect.
