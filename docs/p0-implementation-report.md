# P0 implementation report — SimX memory-system instrumentation

**Task:** implement the instrumentation specified in
[`dx100-experiment-design-v3.md`](dx100-experiment-design-v3.md) §7.2, exposing TLB, translation,
page-walk and page-table-traffic statistics through SimX's exported performance statistics.

**Result:** implemented and validated for every counter in the §7.2 P0 set (the first four rows,
estimated at ≈55 lines). Rows 5–7 of §7.2 are **not** implemented, and correctly so — two of them
need a request-window stage that does not exist in the model, and the third is a per-cycle trace
belonging to the `[N3]` tracer rather than to exported counters. Details in §7 below.

**Scope held:** observability only. No capacity, associativity, MSHR depth, queue size, arbitration,
scheduling, cache, translation algorithm, DXA behaviour, RTL or configuration was changed. The
diff's five deleted lines are two `continue;` statements re-emitted with a counter increment ahead
of them (identical control flow) and three `PerfStats` field declarations re-emitted with comments
plus new siblings.

---

## 1. Files modified, and why

All paths are under `DX100_MEM/ref/vortex/` (the pinned clone `7300bc8`). 16 files, +321 / −5.

| File | Why |
|---|---|
| `sim/simx/mem/mmu.h` | `Mmu::PerfStats` (11 counters + `operator+=`), `perf_stats()`, seven private counters |
| `sim/simx/mem/mmu.cpp` | counter updates at the park/join/stall/replay sites; per-cycle miss-station occupancy sample; `perf_stats()` |
| `sim/simx/mem/tlb_l2.h` | five new `L2Tlb::PerfStats` fields + `operator+=` |
| `sim/simx/mem/tlb_l2.cpp` | allocation, registration-stall, walker-link-stall and occupancy updates; a comment on the existing `misses` semantics |
| `sim/simx/mem/ptw.h` | three new `Ptw::PerfStats` fields (PTE fetches, walker busy-cycles, peak walkers) |
| `sim/simx/mem/ptw.cpp` | one increment at the PTE-fetch issue site; per-cycle walker occupancy sample |
| `sim/simx/core.h` / `core.cpp` | `mmu_perf_stats(uint32_t which)` — per-side (d/i) statistics, additive to the existing combined accessor |
| `sim/simx/socket.h` / `socket.cpp` | `Socket::PerfStats::ummu`, filled from the socket's unit MMU |
| `sim/simx/cluster.h` / `cluster.cpp` | `Cluster::PerfStats` gains `dmmu`, `immu`, `ummu`; `perf_stats()` sums them over cores and sockets |
| `sim/simx/processor_impl.h` | `ProcessorImpl::PerfStats` gains `l2tlb`, `dmmu`, `immu`, `ummu` |
| `sim/simx/processor.h` / `processor.cpp` | cluster aggregation in `perf_stats()`; new `Processor::print_stats(std::ostream&)` |
| `sim/simx/main.cpp` | `-s` now prints the statistics (the flag was parsed and discarded before) |

**No existing counter, accessor or interface was removed or renamed.** `Core::MmuPerfStats`
(combined d+i) and its CSR consumer are untouched; `mmu_perf_stats(uint32_t)` is an overload beside
it. `Mmu::tlb_reads/hits/misses/evictions()` are unchanged.

## 2. Where the statistics now come from

Before this change there was **no statistics output at all**: `showStats` was set by `-s` and never
read, and `ProcessorImpl::perf_stats()` was consumed only by the MPM CSR block in
`csr_unit.cpp`. The chain is now:

```
Mmu (per L1 side, per unit) ─┐
L2Tlb (per cluster) ─────────┼→ Cluster::PerfStats → ProcessorImpl::PerfStats → Processor::print_stats()
Ptw (device-level) ──────────┘                                                  (gated by `-s`)
Core::mmu_perf_stats(0|1) ───┘
```

Generation sites, one owner each:

| Counter family | Generated in | Aggregated in |
|---|---|---|
| `Mmu` array reads/hits/misses/evictions | `Tlb` (`mmu_tlb.cpp`), unchanged | `Mmu::perf_stats()` reads them |
| `Mmu` mshr/stall/replay | `mmu.cpp` (`mshr_alloc`, park path, replay path, end of `on_tick`) | `Core::mmu_perf_stats(which)` → `Cluster` → `Processor` |
| `L2Tlb` everything | `tlb_l2.cpp` (`lookup`, MSHR claim, walker link, end of `on_tick`) | `Cluster::perf_stats()` → `Processor` |
| `Ptw` everything | `ptw.cpp` (`W_REQ` fetch, `W_FILL`, dispatch, end of `on_tick`) | `ProcessorImpl::perf_stats()` **directly** — the walker is device-level, so it is deliberately *not* re-summed in `Cluster` |

Output format is `key = value`, one per line, prefixed `vxstat.`, so the experiment scripts can read
it with `grep`/`awk` without a parser. Example keys: `vxstat.dmmu.stall_cycles`,
`vxstat.l2tlb.mshr_occupancy_avg`, `vxstat.ptw.pte_fetches`.

## 3. Every counter, and precisely when it increments

### 3.1 `Mmu::PerfStats` (L1 data MMU, L1 instruction MMU, socket unit MMU)

| Counter | Increments when | Notes |
|---|---|---|
| `reads` | a translated request performs a TLB probe (`tlb_.lookup`) | **probe events**, not unique requests: a request stalled downstream is re-probed next cycle |
| `hits` / `misses` | that probe found / did not find a translation | owned by `Tlb`, read not re-counted |
| `evictions` | `Tlb` evicts a line to install a fill | owned by `Tlb` |
| `mshr_allocs` | a **new** miss-station entry is claimed (`mshr_alloc` returns a slot) | one per distinct missing page per occupancy episode |
| `mshr_joins` | a request is parked on an **already live** entry for its VPN | the VPN-dedup count. Also counts the ordered case: a TLB *hit* whose page has a live entry and whose port already holds parks is parked behind it instead of being forwarded |
| `mshr_occupancy` | end of every cycle, `+=` the number of valid entries | integral; average = this / elapsed cycles. Point sample: an entry freed earlier in the same cycle is not counted for it, so each episode can be off by one cycle |
| `mshr_max_occupancy` | end of every cycle, if the count exceeds the previous peak | depth of the station bounds it |
| `stall_cycles` | a cycle in which **≥ 1 port** had a translated request the stage would not accept | see the exact predicate below |
| `stall_lane_cycles` | `+=` the number of such ports, same cycle | ≥ `stall_cycles` |
| `replay_requests` | a parked request is successfully forwarded from a replay queue | the fill-completion forwardings |

**The stall predicate, exactly** — a port counts as stalled when it presents a request that needs
translation and one of these holds:

1. the request's page is held in the registered park stage (`hold_vpn == vpn`);
2. a permission-fault response is still owed out through the port's fault slot;
3. the registered park stage is occupied by another page this cycle;
4. the miss station has no free entry, or this page's entry already holds
   `MAX_PARKED_PER_ENTRY` requests.

**Deliberately excluded** (documented so nobody reads them as translation stalls): downstream
back-pressure (`ReqOut` full — that is the cache, not the TLB), a flush token waiting behind its
port's parks (an ordering rule), and `ReqIn` empty (no work).

### 3.2 `L2Tlb::PerfStats`

Existing: `reads`, `hits`, `misses`, `evictions`, `mshr_dedups`. Added:

| Counter | Increments when |
|---|---|
| `mshr_allocs` | a free MSHR entry is claimed for a new miss |
| `mshr_occupancy` | end of cycle, `+=` valid entries |
| `mshr_max_occupancy` | as above, peak (bounded by `VX_CFG_L2_TLB_MSHR_SIZE`) |
| `stall_cycles` | a miss could not be registered: station full, or the entry already carries `MAX_REQUESTERS_PER_ENTRY` requesters |
| `walk_stalls` | the walk-issue loop found the walker link (`PtwReqOut`) full and broke out | counted at the **walk-issue** site, at most once per cycle, so it measures stalled cycles not waiting entries. Fault-report holds share the link and are **not** counted (they are not walks) |

### 3.3 `Ptw::PerfStats`

Existing: `walks`, `walk_latency`, `wc_hits`. Added:

| Counter | Increments when |
|---|---|
| `pte_fetches` | a PTE load is **accepted** by the memory port (`MemReqOut.try_send` succeeds) |
| `walker_busy_cycles` | end of cycle, `+=` walkers in a non-idle state (including `W_FILL`) | point sample, same caveat as occupancy |
| `walker_max_active` | as above, peak (bounded by `VX_CFG_PTW_NUM_WALKERS`) |

**PTE accesses vs PTE traffic.** `pte_fetches` counts *line requests*, one per issued fetch. It is
**not** a byte count and must not be reported as one: each fetch is a 64 B line request that may hit
in L1 or L2, so DRAM traffic is whatever the cache/DRAM counters report, and requested bytes are
`pte_fetches × VX_CFG_L1_LINE_SIZE`. This keeps v3 §6.2's (b) PTE-accesses and (d) PTE-bytes
quantities separate, as the audit required.

## 4. Aggregation and double counting

- Each counter has exactly one owner. `Mmu::perf_stats()` *reads* the `Tlb` counters instead of
  keeping parallel copies, so array activity is counted once.
- `Ptw` is device-level and is aggregated only in `ProcessorImpl::perf_stats()`, not in `Cluster`.
  Walks therefore appear once, never once per cluster.
- `Cluster::perf_stats()` reads each MMU instance once (`socket->perf_stats().ummu` per socket,
  `core->mmu_perf_stats(0|1)` per core).
- `operator+=` **adds** occupancy integrals and counts, and **takes the maximum** for
  `mshr_max_occupancy` — summing peaks across instances would describe no real queue. This is
  exercised by a unit test (§5, scenario C).
- Cycle counters (`stall_cycles`, `walker_busy_cycles`) are summed across instances, so they count
  **instance-cycles**, not wall cycles. This is stated in the header comment and in the printed
  output's comment.

## 5. Tests and build commands executed

### 5.1 Environment limits (important)

This machine cannot produce the project's normal build, so no workload could be run:

- `./configure` exits with **`Error: Unsupported OS.`** — the build system is Linux-only.
- No RISC-V toolchain is installed (`riscv64-unknown-elf-gcc` absent), so no kernel or test image
  can be compiled even if the build worked.
- `sw/common/stringutil.h:23` does not compile under libc++ (clang/macOS): `ByteStream` derives from
  `std::istream` without initialising it. This is **pre-existing and unrelated** to this task. To
  compile anything at all on this host I generated the config headers and put a patched *copy* of
  that one header in an out-of-tree include directory; **no file in the Vortex tree was modified
  for it**.

Config headers were generated the way `configure` does it:

```bash
cd DX100_MEM/ref/vortex
XLEN=32 ci/gen_config.py --config VX_config.toml --output $B/sw/VX_config.h --format cpp
XLEN=32 ci/gen_config.py --config VX_types.toml --output $B/sw/VX_types.h --format cpp --resolved
```

### 5.2 Compile checks (the real gate available here)

Every touched translation unit, with `-Wall -Wextra`, in **both** builds:

```bash
# VM build (build V) — all eight TUs
g++ -std=c++17 -fsyntax-only -Wall -Wextra \
    -DVX_CFG_XLEN=32 -DVX_CFG_XLEN_32 -DVX_CFG_VM_ENABLE -DSIMX_SOCKET_UNIT_MMU \
    $INC sim/simx/mem/{mmu,tlb_l2,ptw}.cpp sim/simx/{socket,cluster,core,processor,main}.cpp

# default build (VM off, the shipped configuration)
g++ -std=c++17 -fsyntax-only -DVX_CFG_XLEN=32 -DVX_CFG_XLEN_32 $INC \
    sim/simx/{socket,cluster,core,processor,main}.cpp
```

Results: **8/8 OK with VM enabled; 5/5 OK with VM disabled; zero warnings attributable to the new
code.** (`mmu.cpp`, `tlb_l2.cpp` and `ptw.cpp` compile to nothing when VM is off, which is why only
five TUs are listed for that build.)

### 5.3 Behavioural validation against the real modules

`DX100_MEM/validation/p0_counters_test.cpp` drives the **real** `Mmu`, `L2Tlb` and `Ptw` through the
**real** `SimPlatform`, with stubs only for the surrounding parts (a cluster TLB that answers misses,
a cache that echoes responses, a PTE memory that returns invalid PTEs, a walker that never answers).
No counter checked there is produced by test code.

```bash
g++ -std=c++17 -O0 -g -DVX_CFG_XLEN=32 -DVX_CFG_XLEN_32 -DVX_CFG_VM_ENABLE \
    -DSIMX_SOCKET_UNIT_MMU $INC validation/p0_counters_test.cpp \
    ref/vortex/sim/simx/mem/{mmu,mmu_tlb,tlb_l2,ptw}.cpp -o build-vortex/p0test
./build-vortex/p0test
```

Actual result:

```
P0 counter validation (VM=1, pages=4096 B)
[A] cold miss, dedup join, replay, hit
[B] miss station full -> stall cycles
[C] PerfStats aggregation algebra
[D] walker: walks, pte_fetches, latency, occupancy
[E] L2 TLB: dedup, allocs, occupancy, station-full stalls
ALL CHECKS PASSED
```

Coverage, mapped to the validation items the task asked for:

| Required check | Scenario |
|---|---|
| Counter initialization | A, D, E each assert all-zero on a fresh object |
| Correct hit / miss / walk increments | A (`reads`/`misses`/`hits`), D (`walks`, `pte_fetches`), E (`misses`, `mshr_dedups`) |
| Correct stall increments | B (miss station full), E (L2 MSHR full) |
| Correct aggregation | C (`Mmu::PerfStats::operator+=`, `L2Tlb::PerfStats::operator+=`, including max-not-sum) |
| PTE accesses vs traffic | D (`pte_fetches == PTE loads the memory side saw`, counted once) |
| No double counting | A (`mshr_allocs == 1` for two requests to one page; `f.tlb->served == 1`), D (`fetches == pte_fetches`), E (`mshr_allocs == 1` with `mshr_dedups == 1`) |
| Behaviour unchanged | A asserts the exact accepted-request count and that all requests drain; B asserts the stalled request is still served, not lost |

The tests also pin two invariants worth keeping: `stall_lane_cycles ≥ stall_cycles`,
`stall_cycles ≤ elapsed cycles`, and `mshr_max_occupancy ≤ station depth`.

### 5.4 What was **not** verified

- **No workload was run.** There is no before/after timing comparison of a real kernel, because
  neither the build nor a RISC-V image can be produced on this host. Behaviour preservation rests on
  the diff (no state variable, control-flow decision, channel operation or arbitration input
  changed), on the harness's request-accounting assertions, and on the reviewable diff — not on an
  end-to-end A/B measurement. **This is the main validation gap and should be closed on a Linux
  host with `make -C build simx && ./sim/simx/simx -s <kernel>` before Stage 0.**
- `evictions` is not exercised by the harness (it needs array pressure); it is a pre-existing
  counter whose update site was not touched.
- The full `cluster → processor` aggregation path is verified by compilation and by the
  `operator+=` unit test, not by running a multi-core GPU.
- `-s` output was not observed from a real run (same reason as above); the printing code is
  exercised only by compiling and by the type structure of the aggregated struct.

## 6. Metrics that remain unavailable or approximate

| Metric | Status | Reason |
|---|---|---|
| `mshr_occ_tlb` **time series** | **partial** — integral, average and peak exist; no per-cycle series | a per-cycle trace belongs to the `[N3]` tracer, not to exported counters; adding a ring buffer would be new model machinery, outside P0 |
| L2 TLB `misses` | **over-counts under back-pressure** | pre-existing: the counter increments per *attempt*, and a pipe head that cannot register an entry is re-examined every cycle. Now documented in the source and in the test. `mshr_allocs` is the per-miss-event count; report both, or use `mshr_allocs` for "distinct miss events" |
| L1 `reads` | **probe events, not requests** | a request blocked downstream is re-probed; pre-existing `Tlb` semantics |
| TLB-bus arbitration latency | **unmodelled** (audit F13/U-a) | the cluster TLB link has no modelled arbitration delay; no counter can recover it |
| `stall_cycles` across instances | **instance-cycles** | summed by design; do not divide by wall cycles to get a utilization without dividing by the instance count |
| Unit MMU (`ummu`) in most builds | **all zero by construction** | `SIMX_SOCKET_UNIT_MMU` is defined only when `VM ∧ (TEX ∨ RTU ∨ DXA)` (`sim/simx/constants.h:96-98`). In build V there is no unit MMU, so zero means "no such stage", **not** "the UTLB never missed". Only the `VM ∧ DXA` build has one |
| `-s` output in HTIF / debug mode | **not printed** | `print_stats()` is called in the normal run branch only; HTIF-driven riscv-tests and `-d` sessions produce no statistics block |
| Window occupancy, `gen_lead` | **not implemented** | require the request-window stage and tile buffer that do not exist in the model (§7.2 rows 6–7, §7.3). Deferred with the stages, not silently dropped |
| DRAM bank groups, row-hit detail | **not added** | existing `dram_bank_model.h` stats are unchanged; bank-group structure is a model limitation (audit U-d), not an instrumentation one |

## 7. Unresolved issues against v3 §7.2

| §7.2 row | Status |
|---|---|
| `dmmu/ummu` reads, hits, misses, evictions | **Resolved.** Reported for d/i separately and for the unit MMU, exported through `Cluster` → `Processor` → `-s`. Note the table names `dmmu` and `ummu`; the instruction MMU is also reported, which the table omitted |
| L2 TLB reads/hits/misses/evictions/`mshr_dedups` | **Resolved**, plus allocation, occupancy and stall counters the table did not list |
| `walks`, `walk_latency`, `wc_hits` | **Resolved**, plus `pte_fetches` and walker occupancy, which the table's estimate did not include but the experiment needs |
| `tlb_stall_cycles` | **Resolved**, implemented exactly as specified (park stage occupied / miss station full), with the predicate stated in §3.1 and two counters (cycle and lane granularity) |
| `mshr_occ_tlb` time series | **Partially resolved.** Integral + average + peak exported; the per-cycle series needs the tracer |
| window occupancy | **Not implemented** — out of P0 scope; needs the window stage (W5) |
| `gen_lead` | **Not implemented** — out of P0 scope; needs the tile buffer (W6) |

## 8. Confirmation of scope

- No capacity, associativity, MSHR count, queue depth, latency, arbitration rule or translation
  algorithm was changed. Every added statement either increments a new counter, samples existing
  state read-only, or copies existing state into a statistics struct.
- No RTL, no `.toml`, no workload, and no earlier document was modified.
- `VX_config.toml` is unchanged (`VM_ENABLE=false` still ships); the VM build used for validation is
  a compile-time define, as it is for the experiment.
- The 18-run probe and the full experiment were **not** started.

## 9. Reproducing on a Linux host

```bash
cd DX100_MEM/ref/vortex
./configure --xlen=32 && make -C build simx -j
# behaviour A/B: run any kernel with and without this patch and diff the cycle count
./build/sim/simx/simx -s <kernel.vxbin> | grep vxstat
```

`build/sim/simx/simx -s` now prints the block described in §2; without `-s` nothing is printed and no
counter is read.

## 10. Review follow-ups

A line-by-line review of the diff found one defect and three documentation-level nits.

**Defect, fixed.** `L2Tlb::walk_stalls` was first incremented in the *fault-report* path (step 2 of
`on_tick`), not the walk-issue loop (step 4). It now counts walk-issue stalls only, at most once per
cycle, and fault-report holds are excluded — a fault report is not a walk. Re-validated: the same
five scenarios still pass.

**Nits, documented rather than changed:**

- `Core::mmu_perf_stats(uint32_t which)` treats any non-zero value as the instruction MMU. Only 0
  and 1 are ever passed (`Cluster::perf_stats()`), so this is a latent readability issue; an assert
  would be a behaviour change on a misuse path and was not added.
- `mshr_joins` includes the ordered hit-behind-a-live-entry case; the definition is now stated in
  `mmu.h` and in §3.1.
- Occupancy counters are point samples at end of cycle (§3.1, §3.2), so per-episode error is ±1
  cycle on the integral.
