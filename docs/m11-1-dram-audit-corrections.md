# M11.1 — DRAM audit corrections and Vortex comparison

**Scope.** Read-only. No simulator source, configuration, test, baseline, archived result or
the original `m11-dram-scheduler-audit.md` was modified. No simulator run was executed for
this task; every number is read from archived logs already in `sim/build/`, or derived by
arithmetic from source. One file was created: this report.

**Labels** (used on every substantive statement):

| Label | Meaning |
| --- | --- |
| **[S]** | Verified source fact — read in the source quoted |
| **[M]** | Measured simulator result — from an archived log or archived report |
| **[D]** | Derived calculation — arithmetic shown from [S]/[M] inputs |
| **[H]** | Hypothesis — needs a new experiment |
| **[?]** | Unknown — not reconstructable from existing state |

---

## 1. Per-bank visibility correction (Issue 1)

### 1.1 The queue is global, and occupancy is global

**[S]** `MemorySystem` holds **one** queue: `std::vector<DQ> dram_wait_` (`sim.hpp:564`), one
`DQ` per pending line request (`sim.hpp:538`: `{u64 line, seq, arrive; u32 bank, row}`). Each
entry carries its own `bank`, computed at push time (`memory.cpp:328–330`). There is no
per-bank queue, no per-bank head, no per-bank arbiter.

**[S]** Bank state is per bank but hold-only: `bank_ready_`, `bank_row_`, `bank_open_` are
`std::vector<u64>/<bool>` of length `cfg.banks` (`sim.hpp:565–566`, allocated at
`memory.cpp:46–48`). They record *when a bank is free* and *which row it holds*; nothing
records *how many queued requests target it*.

**[S]** The queue is drained one command per cycle, chosen by a single scan over the whole
vector (`memory.cpp:436–448`).

**Therefore: occupancy is global, not per-bank.** A request for bank 0 and a request for
bank 1 compete in the same scan, and either bank may hold any number of the queued entries.

### 1.2 Maximum possible per-bank visibility

**[S]** The only hard bound on total occupancy in the published configuration is the number of
line requests that can be outstanding past the L1: `l1_mshr = 16` per core
(`configs/ab_controlled.cfg`), enforced at `memory.cpp:365–374` (no free L1 MSHR → return 2 →
the request bounces back to the front-end). L2 MSHRs (64) are downstream of that gate, so the
DRAM queue can receive at most one line per L1 MSHR slot plus PTE fetches issued directly by
the walker (`memory.cpp:262`).

**[D]** Consequently:

| Quantity | Bound | Basis |
| --- | --- | --- |
| Maximum total queue occupancy | **16** lines (+ in-flight PTE fetches) | 16 L1 MSHRs [S] |
| Maximum requests targeting **one** bank | **16** (all of them) | no per-bank split exists [S] |
| Maximum requests targeting both banks | 16 total, split however the arrive order dictates | [S] |

**The original report's "at most half a row per bank" is not supported.** A total of 16 does
not imply 8 per bank; a single bank could hold all 16. The defensible aggregate statement is
weaker but still decisive: **the whole queue can hold at most 16 lines, which is exactly one
bank-row's worth of lines (1 KiB / 64 B = 16) — in total, across both banks.**

### 1.3 What the arrive order actually does — derived, not assumed

**[S]** The bank is `(line ^ (line>>6) ^ (line>>12)) & 1` (`memory.cpp:326–328`). For
consecutive line addresses, the XOR's `>>6` and `>>12` terms are constant over a 64-line span,
so `bank = bit0(line) ^ constant` — **consecutive lines always alternate banks**, regardless
of page [D, checked for pages 0, 1 and the 2 MiB footprint].

**[D]** Any run of consecutive lines therefore splits evenly across the two banks: a queue of
depth *d* fed by a sequential arrive stream holds ⌈d/2⌉ and ⌊d/2⌋. At the measured peak of 16
that is exactly **8 / 8**. So the original "half a row per bank" conclusion is correct *for
the dominant sequential component of the arrive order*, but for a reason it did not state (the
address map's parity interleave), not because queue depth implies an even split.

**[H]** At page and window boundaries the arrive stream is not perfectly sequential (page-major
issue jumps between pages, and SIMT interleaves), so the true per-bank split can skew and can
only be established by instrumentation (§1.5).

### 1.4 Observed and average visibility

**[M]** Archived M9 logs, `[15]` block (arm B / arm C), W1 and W0, E3 default:

| cell | mean depth B | mean depth C | peak B | peak C | mean candidates/decision C |
| --- | --- | --- | --- | --- | --- |
| W1 κ=0.25 | 9.8 | 10.7 | 16 | 16 | 6.6 |
| W1 κ=0.50 | 7.9 | 10.0 | 16 | 16 | 6.5 |
| W1 κ=0.75 | 6.3 | 6.8 | 16 | 16 | 6.5 |
| W0 κ=0.25 | 12.8 | 11.1 | 15 | 15 | 7.9 |
| W0 κ=0.50 | 13.0 | 12.8 | 16 | 16 | 8.6 |
| W0 κ=0.75 | 13.2 | 13.4 | 16 | 16 | 8.9 |

`dram_q_occ_sum / (cycles+1)` for the means (`ab.cpp:2274–2276`), `CycleSample::dram_q` peak
(`traditional` trace, `bulk.cpp:1974`), `dram_elig_sum / dram_svc_events` for candidates
(`ab.cpp:2278–2281`).

**[M]** Peak occupancy is 15 or 16 in every one of the six cells, i.e. the queue saturates the
16-entry bound; the mean is 6.3–13.4.

### 1.5 Per-bank request counts are not observable

**[S]** The only per-bank datum ever recorded is `dram.bank_mask |= 1ull << q.bank`
(`memory.cpp:450`), exported as `dram_bank_mask` (`system.cpp:413`) and consumed **solely** as
a validity check that no bank above `cfg.banks` was used (`ab.cpp:139–143`). It is a bitmask,
not a count, and with `banks = 2` it saturates on the first two commands.

**[S]** A grep of `sim/src/` finds no `per_bank`, `bank0`, `bank1` or `banks[]` counter of any
kind.

| Requested quantity | Available? | Note |
| --- | --- | --- |
| Mean requests per bank | **[?]** | No counter; not in any archived log |
| Peak requests per bank | **[?]** | Only the *aggregate* peak (16) is recorded |
| Distribution across banks | **[?]** | `bank_mask` only proves both banks were used |
| Distribution across rows within a bank | **[?]** | No row counter and no row histogram |
| Mean/peak aggregate depth | **[M]** | First table above |

**[D]** Partial reconstruction is possible *only* for fully sequential arrive runs: there the
split is exactly ⌈d/2⌉/⌊d/2⌋ by §1.3. For the real interleaved stream it is not
reconstructable — the required fields are the per-entry bank at each cycle (the existing
`CycleSample` records `dram_q` but not its composition) and a per-bank outstanding counter.

**Corrected statement (replaces the original).** *The controller's visible set is at most 16
lines in total — one bank-row's worth — and is measured to reach 15–16 at peak and 6–13 on
average. Consecutive-line runs split that set evenly across the two banks by construction of
the address map (8/8 at peak); any other split is unmeasured. The scheduler can therefore keep
at most **one row open and fully fed, or two rows half fed**; it cannot hold a full row per
bank (which would need 32 entries).*

---

## 2. Validation of the activation lower bound (Issue 2)

### 2.1 Reconstructing the addresses

**[S]** VA → PA (per page, `memory.cpp:82–86`): `pa_line = va_line + 1 048 576`, i.e. PA = VA +
64 MiB (`PPN_OFFSET = 0x4000` pages × 4 KiB).

**[S]** PA → (bank, row) (`memory.cpp:322–329`):

```
bank = (pa_line ^ (pa_line>>6) ^ (pa_line>>12)) & 1
row  = (pa_line / 32) % 4096
```

Worked for page *p* of W1 (whose footprint is VA `[0, 2 MiB)` — 512 pages of 4 KiB):

| line in page | pa_line | row | bank |
| --- | --- | --- | --- |
| *i* ∈ [0,16) | 1 048 576 + 64p + *i* | 2p mod 4096 | 0 |
| *i* ∈ [16,32) | … | 2p mod 4096 | 1 |
| *i* ∈ [32,48) | … | (2p+1) mod 4096 | 0 |
| *i* ∈ [48,64) | … | (2p+1) mod 4096 | 1 |

**[D]** A 4 KiB page therefore occupies exactly **4 (bank,row) pairs**: two row indices × two
banks, 16 lines each. Pages *p* and *p'* occupy disjoint pairs because their row indices are
`{2p, 2p+1}` and `{2p', 2p'+1}`, which are distinct for `p ≠ p'` while `2p ≤ 1022 < 4096` — the
footprint is smaller than the 8 MiB period (§3), so **no two pages in W1 share a row**.

### 2.2 Why four activations per page is the floor

**[S]** The model activates a row only when `!(bank_open_[b] && bank_row_[b] == row)`
(`memory.cpp:449–453`); a bank holds exactly one row at a time, and there is no precharge
hiding.

**[D]** Given (a) each page's lines span both banks and both of its row indices, (b) pages do
not share pairs, and (c) a bank can hold one row: **each of the 4 pairs must be activated at
least once**, so ≥ 4 activations per page and ≥ 2 048 for 512 pages.

Three qualifiers, each of which the original report owed the reader:

1. **(a) is a coverage assumption, not a theorem.** If every access to a page happened to land
   in one 32-line half *and* one parity, a page might need only 1–2 activations. With 24–32
   accesses per page spread over 64 lines, P(one half untouched) ≈ 2⁻²⁴ per half ⇒ over
   1 024 halves ≈ 6 × 10⁻⁵. So the floor is 2 048 **unless** coverage fails, and it can only be
   *lower*, never higher. **[D]**
2. **(b) fails outside the footprint.** Two physically distinct addresses 8 MiB apart share a
   (bank,row) (§3), so a page at VA *x* and a page at VA *x* + 8 MiB do **not** get distinct
   pairs. W1's footprint is 2 MiB, so this does not apply to its data pages — but it does apply
   to the index array, whose base is exactly 8 MiB above the data base. **[S]/[D]**
3. **The floor must include non-target traffic.** Index lines, the 64-line descriptor and PTE
   fetches also activate rows: descriptor at PA 70 MiB → row 3 072; root PTE table at PA 66 MiB
   → row 1 024; leaf tables at PA 68 MiB → row 2 048. These add **≈ 3–5 pairs**, and the index
   array's rows (0–773 after aliasing) fall *inside* the data set. So the realistic floor is
   **≈ 2 051–2 053**, not exactly 2 048. **[D]**

**Classification.** The 2 048 figure is an **idealized lower bound under a stated coverage
assumption**, not a strict mathematical lower bound (which would be only 512, one activation
per page, and is not physically achievable given the parity interleave). It is a *tight*
idealization: the probability that coverage fails is ~10⁻⁴.

### 2.3 Initial bank state

**[S]** Both banks start closed: `bank_open_.assign(cfg.banks, false)` (`memory.cpp:48`).

**[S]** A first access to a closed bank increments neither `row_hits` nor `row_conflicts`
(`memory.cpp:451–452` is an `if/else if`). Measured W1 κ=0.25 arm C: `13 589 − 11 383 − 2 204 =
2` — exactly one cold activation per bank **[M]**, which confirms both the initial state and
the counter semantics.

### 2.4 Recalculated headroom

**[M]** W1 κ=0.25, arm C: 13 589 DRAM reads, 11 383 row hits, 2 204 row conflicts,
125 581 cycles, bank busy 216 718.

| Quantity | Value | Basis |
| --- | --- | --- |
| Activation-causing events | 2 206 (= 2 204 conflicts + 2 cold) | [M]/[S] |
| Idealized floor (data pages) | 2 048 | [D] §2.2 |
| + descriptor / PTE pairs | ≈ +3…5 | [D] §2.2 |
| **Surplus over floor** | **≈ 153–155** | [D] |
| Cost per avoided activation | 12 cycles of bank time (`t_miss − t_hit`) | [S] |
| Cycles saved (two banks) | 154 × 12 / 2 = **924** | [D] |
| **Headroom** | **≈ 0.74 %** of 125 581 | [D] |
| Absolute ceiling (all activations gone) | 2 206 × 12 / 2 = 13 236 = **10.5 %** | [D], unreachable |

The original 0.75 % survives, **provided it is read as "the residual activation cost", not as
"what a bulk organizer could recover"**. Two reasons the addressable share is smaller:

* Part of the surplus is PTE and descriptor traffic interleaved into the target stream (a walk
  activating row 1 024 between two target rows on the same bank). **[D]** No counter separates
  that from ordering loss.
* The floor assumes perfect grouping of a page's four pairs, which needs 32 lines of
  scheduling freedom — twice the queue's total capacity (§1.2). So the floor is not reachable
  by reordering at all under 16-entry visibility; it is a lower bound on activations, not a
  target. **[D]/[H]**

**Terminology correction for the original report:** "4.31 activations per page against a floor
of 4, therefore 7.7 % overhead" is a valid *derived ratio*, but "2 048 is the floor" must be
quoted as an idealized bound under stated assumptions (§2.2), and the 0.74 % as an upper bound
on the residual, not an achievable gain.

---

## 3. Address-mapping periodicity artifact (Issue 3)

### 3.1 The address-space correspondence is exact

**[S]/[D]** The map's period in line space is 4 096 rows × 32 lines = 131 072 lines = **8 MiB**
(row is `% 4096`, bank depends only on bits 0/6/12 of the line number). `bulk_data_base = 0`
and `bulk_index_base = 0x00800000` are exactly one period apart, and `PPN_OFFSET` shifts both
by the same 64 MiB, so index line *i* and target line *i* evaluate to identical `(bank,row)`.
Checked for line 0: both give bank 0, row 0.

### 3.2 Does the actual access behavior exploit it? Two different answers per arm

The correspondence only matters if index-fetch requests and target requests **coexist in the
queue** while the aliased row is open. The archived `[1]` timelines answer that directly:

| event | arm B (bulk only) | arm C (M10 dispatched) |
| --- | --- | --- |
| descriptor fetch done | 337 | 586 |
| index acquisition done | **12 866** | **13 645** |
| first target issue | **4 115** | **14 282** |
| overlap of index and target phases | **yes: 4 115 → 12 866 (8 751 cycles)** | **none: index ends 13 645, targets start 14 282 (637-cycle gap)** |

**[M]**, W1 κ=0.25, `sim/build/wm/e3d_k0.25.txt`.

**[D]** For **arm C — the arm the verdict rests on — the index phase is strictly over 637
cycles before the first target request is issued.** No index line can be in `dram_wait_` at the
same time as a target line, so the periodicity **cannot produce a single spurious row hit in
arm C**. The correspondence is an address-space fact with no behavioral consequence there.

**[D]** For **arm B** the phases overlap for 8 751 cycles, so spurious hits are possible. Bound:
at most 774 index lines (arm C) / 1 024 (arm B) can be affected, i.e. **≤ 5.7 %** of that arm's
DRAM reads — and only those that arrive while the aliased data row happens to be open. B's
measured rate is 84.3 % against C's 83.8 %; the entire difference is inside this bound, so the
effect cannot be separated from the different element subsets by inspection. **[?]**

### 3.3 The three locality components

**[D]** Using the `[8]` traffic rows (`bulk_*_line_bytes`, which count the engine's line
traffic, not DRAM reads — so these are upper bounds on DRAM share):

| component | W1 κ=0.25 C | note |
| --- | --- | --- |
| **Index–index** | 774 index lines, read sequentially from 8 MiB: rows change every 32 lines ⇒ ~24 row-blocks ⇒ ~48 activations ⇒ **≈ 94 % internal hit rate** | [D]; the index sweep is the most row-local stream in the run |
| **Target–target** | 12 376 target lines, page-major, lines ascending: this is the 83.8 % aggregate | [M] |
| **Index–target** | possible only in arm B (§3.2); zero in arm C | [D] |

### 3.4 Row-hit rate excluding index traffic

**[S]** The simulator has **no per-requester DRAM counters**: `DQ` carries no client tag
(`sim.hpp:538`), so no archived log can subtract index hits. **[?]** The exact figure is not
recoverable.

**[D]** An analytic estimate, stated as an estimate: if the index sweep behaves as §3.3 says
(~48 activations over 774 lines ⇒ ≈ 726 hits), then target+descriptor only would be
`(11 383 − 726) / (13 589 − 774) = 10 657 / 12 815 = **83.2 %**` — i.e. **lower** than the
83.8 % aggregate, because index traffic is *more* row-local than the target stream. The
headline number is therefore not propped up by index traffic; if anything the index phase
flatters it.

### 3.5 Scope

**[S]/[D]** The artifact is a property of the **address layout**, not of W1 or M10: any index
base that is a multiple of 8 MiB above the data base will alias. It is a general consequence of
`row = (pa_line/32) % 4096` truncating the row address; W1 merely happens to sit at offset 0
where the published bases align exactly. It matters behaviorally only when the two streams
overlap (arm B, or a future design that interleaves index and target fetches) **[H]**.

**Upstream context [S]:** Vortex's own Ramulator integration has the same class of artifact and
documents it — `sim/common/dram_sim.cpp:134`: *"Addresses above a channel's 8 Gb alias, which
only affects row hits."* (the constant it guards is `channel_size_`, `:135`) Address truncation aliasing is therefore not a defect unique to
this model, but it does mean neither model's absolute row-hit rate should be read as a device
measurement.

---

## 4. Vortex's actual DRAM scheduling architecture (Issue 4)

### 4.1 The RTL has no DRAM scheduler

**[S]** `hw/rtl/mem/` contains only interconnect and local-memory blocks:
`VX_mem_axi_arb.sv`, `VX_mem_axi_if.sv`, `VX_mem_axi_slice.sv`, `VX_mem_axi_xbar.sv`,
`VX_mem_bus_arb.sv`, `VX_mem_bus_delay.sv`, `VX_mem_bus_if.sv`, `VX_mem_bus_slice.sv`,
`VX_mem_bus_switch.sv`, `VX_mem_req_pending.sv`, `VX_mem_xbar.sv`,
`VX_membus_from_axi.sv`, `VX_membus_to_axi.sv`, `VX_lsu_adapter.sv`, `VX_lsu_mem_arb.sv`,
`VX_lsu_mem_if.sv`, `VX_lmem_*`. The socket exposes AXI (`hw/rtl/Vortex_axi.sv`).

**[S]** A grep of all of `hw/rtl` for `fr.?fcfs|first.?ready|row.?hit|row.?buffer|bank.?conflict`
returns only unrelated uses: a "bank-conflict-free register offset" comment in
`hw/rtl/tcu/VX_tcu_uops.sv:279`, "first ready output" in a stream dispatcher
(`hw/rtl/libs/VX_stream_dispatch.sv:84,110`), and a local-memory comment
(`hw/rtl/mem/VX_lmem_unit.sv:17`). **No DRAM command scheduling, no row/bank state, no
controller.**

**[S]** `VX_config.toml:102–105` states it plainly: the DRAM switches are *"Simulation-only:
the RTL never reads them."*

### 4.2 Both Vortex simulators delegate to `DramSim`, which wraps an external model

**[S]** `sim/simx/mem/memory.cpp` — the L2/memory endpoint holds `DramSim dram_sim_` (`:43`),
ticks it (`:86`), gates on `dram_sim_.can_accept(i)` (`:91`) and issues
`dram_sim_.send_request(...)` per channel (`:132`). It attaches the functional RAM separately
(`:160`, `attach_ram`).

**[S]** `sim/rtlsim/processor.cpp:148` constructs `DramSim::platform_config()` and drives it
from `dram_queue_[b]` per bank (`:441–446`), so the RTL simulation uses the same backend.

**[S]** `sim/common/dram_sim.cpp:58`: *"A DRAM backend: Ramulator, or the bank model
(`sim/common/dram_bank_model.h`) with `VORTEX_DRAM_MODEL=bank` — a choice of simulator, not of
machine."* Selector at `:373–380`; the same switch appears as `AXI_MEM_MODEL` for rtlsim
(`sim/rtlsim/Makefile:138–145`).

### 4.3 Backend 1 (default): Ramulator, FR-FCFS

**[S]** `sim/common/dram_sim.cpp`, `channel_config()`:

```cpp
controller["impl"] = "Generic";
controller["Scheduler"]["impl"] = "FRFCFS";
controller["RefreshManager"]["impl"] = "AllBank";   // with a comment that refresh is not modelled
controller["RowPolicy"]["impl"] = "OpenRowPolicy";
memory_system["AddrMapper"]["impl"] = "RoBaRaCoCh";
```

with `Frontend/impl = "GEM5"`, `MemorySystem/impl = "GenericDRAM"`, and an HBM2 8 Gb device
(2 pseudo-channels × 4 bank groups × 4 banks, 1 KiB rows) or GDDR6 (`set_gddr6_device`, 4 bank
groups × 4 banks, 2 KiB pages).

**[S]** Number of channels: `config.num_channels = VX_CFG_PLATFORM_MEMORY_NUM_BANKS;`
(`dram_sim.cpp:363`) — i.e. **Vortex models one Ramulator channel per "memory bank" of the
platform configuration**, which is 2 (`VX_config.toml:92`), matching the comment *"one channel
per bank"* (`VX_config.toml:98`).

**[S]** Address mapping into Ramulator is not the platform address: `dram_addr()`
(`dram_sim.cpp:217–231`) interleaves a row visit (4 bursts), then pseudo-channel / bank group /
bank, with the bank XOR-folded by the row, over a `kChannelSize` window (8 Gb) — addresses above
that alias (comment at `:134`).

### 4.4 Backend 2 (`VORTEX_DRAM_MODEL=bank`): a reservation model, per-bank FCFS

**[S]** `sim/common/dram_bank_model.h:30–56`: *"each request is scheduled once, by placing its
precharge, activate and column commands at the first DRAM clocks every JEDEC constraint allows
… Banks keep their rows open; a request to a bank waits for the bank's earlier requests
(**first come, first served per bank**). The controller issues one command per clock."*
Parameters: `read_queue = 32`, `write_queue = 32`, `horizon = 448`, write turnaround at
26/7 bursts, optional per-bank or all-bank refresh (`Refresh::Off` by default for `dram_sim`).

**[S]** This backend has **no row-hit priority and no reordering across a bank's requests** —
it is FCFS-per-bank with exact JEDEC timing, and the comment notes it is cycle-exact against
Foldsim's memory endpoint. It is *not* FR-FCFS.

### 4.5 Which description is accurate

| Candidate | Verdict |
| --- | --- |
| **A. Vortex RTL implements FR-FCFS** | **False [S]** — no scheduler, row state or bank arbitration exists in `hw/rtl`; the socket is AXI and the DRAM switches are explicitly simulation-only |
| **B. Vortex delegates DRAM scheduling to an external memory simulator/controller** | **True for both simulators [S]** — SimX and rtlsim both drive `DramSim`; the default backend is Ramulator with `Scheduler = FRFCFS`, the alternative is the built-in bank model |
| **C. Vortex provides an abstract memory interface and the real policy depends on the integration environment** | **True of the RTL [S]**, and true of the *choice* of backend, but not of "anything goes": the two shipped backends are FR-FCFS (Ramulator) and FCFS-per-bank (bank model) |
| **D. Insufficient source** | **False** — the source states the policy explicitly |

**Answer: B, with C applying to the RTL layer.** Vortex RTL has no DRAM policy; the RTL's
memory behavior is whatever the platform attaches. Both of Vortex's own simulators attach
`DramSim`, whose default is Ramulator's FR-FCFS controller.

### 4.6 Which backend corresponds to our reference configuration

**[S]** `configs/ab_controlled.cfg` cites `VX_config.toml:92,96–107` for "2 channel banks, HBM2
14/26 cycles, 64 B/cycle, unbounded queue". Those are exactly
`VX_CFG_PLATFORM_MEMORY_NUM_BANKS = 2`, `TYPE = "HBM2"`, `DATA_RATE = 2 × clock`,
`LATENCY = 0`, `QUEUE_SIZE = 0`, `DATA_SIZE = 64` (`VX_config.toml:92–107`), whose comment
describes *"14 cycles for an idle open-row read, 26 for a row conflict"* — the numbers our
model uses as `t_hit`/`t_miss`.

**[D]** Both Vortex backends read those switches, so the *parameters* are backend-independent.
The *policy* our model implements — row-hit priority, oldest-first tie-break, unbounded queue —
corresponds to the **default Ramulator FRFCFS backend**, not to the bank model (FCFS per bank)
and not to any RTL logic.

### 4.7 Our standalone model is our own implementation

**[S]** `sim/src/memory.cpp` is 583 lines and covers translation, caches and DRAM in one file;
Vortex's equivalents are `sim/simx/mem/memory.cpp` (199 lines: interconnect + `DramSim` handle),
`sim/common/dram_sim.cpp` (405 lines: backend wrapper) and `sim/common/dram_bank_model.h` (876
lines: JEDEC-level bank model). Our DRAM is **2 banks with 1 KiB rows, one command per cycle,
flat 14/26-cycle latency, no bank groups, no channels, no columns, no refresh** — a coarse
abstraction, parameter-matched to the platform switches and policy-matched to Ramulator's
FRFCFS.

**[D]** Therefore the original audit's framing ("our MC is FR-FCFS") is **correct about the
policy family and about the Vortex default**, but it is not a port: absolute numbers from our
model (2 banks, 14/26) are not comparable with Ramulator's HBM2 device (32 banks per channel,
bank-group timings), and *nothing* in our model corresponds to RTL behavior, because the RTL
has no DRAM policy at all.

---

## 5. Revised headroom assessment

### 5.1 Effect of each correction on the original <1 % estimate

| Issue | Original claim | Correction | Effect on the estimate |
| --- | --- | --- | --- |
| 1 | "≤ 16 lines ⇒ at most half a row per bank" | queue is global; per-bank max is 16; even 8/8 split holds only because consecutive lines alternate banks by map construction; per-bank split unmeasured | **Statement corrected, conclusion unchanged.** The cap that matters — at most one row's worth of lines in total — is intact and is now properly sourced |
| 2 | "floor 2 048, surplus 158, 0.75 %" | floor is *idealized* (coverage assumption), must add ~3–5 pairs for descriptor/PTE, surplus ≈ 154, headroom ≈ **0.74 %**, addressable share smaller than the residual | **Estimate survives (−0.01 pt)** but must be labelled an upper bound on residual cost, not an achievable gain |
| 3 | "aliasing bounds inflation at ≤ 5.7 pts" | in arm C the index phase ends **637 cycles before** the first target issue ⇒ **zero** spurious hits; the artifact is behaviorally relevant only to arm B | **Estimate unaffected for the verdict**, which rests on arm C |
| 4 | "our MC is FR-FCFS" | true as a policy family and as Vortex's default SimX backend (Ramulator); the RTL has no scheduler; our model is an independent 2-bank abstraction | **No effect on the numbers**; changes how the claim may be cited |

### 5.2 Why the verdict does not depend on the corrected Issue 1

The activation-floor argument (§2) is a *direct* measurement of what the arrive order plus the
scheduler achieved — 2 206 activations against ≈ 2 051 ideal — and it is independent of how the
queue splits across banks. Correcting the visibility claim therefore removes a supporting
argument without weakening the conclusion: even if the scheduler saw a perfectly balanced,
maximally deep queue, it would have at most ≈ 154 activations of slack to recover on this cell.

The complementary bound points the same way: bank utilisation is already 86.3 % and 80 % of the
run has work with both banks busy **[M]**.

### 5.3 Where the estimate remains soft

1. **[?]** The split of the ≈ 154 surplus between "ordering loss a bulk organizer could fix"
   and "PTE/descriptor interleaving no organizer can fix" is unmeasured. If the split were
   50/50, the addressable headroom would be ≈ 0.37 %.
2. **[H]** The causal claim that 16-entry visibility is the *reason* the scheduler cannot do
   better is a hypothesis: the floor argues it cannot help much at this depth, but does not
   prove that a deeper queue would help either (a deeper queue would raise utilisation pressure,
   not row locality, since rows are already grouped per page).
3. **[?]** Per-bank and per-row distributions remain unmeasurable, so the "8/8 split" remains a
   map-derived inference rather than an observation (§1.5).
4. **[D, not measured]** The floor's coverage assumption (§2.2) is probabilistic; if a future
   workload scattered accesses over one half of each page, the floor would drop and the surplus
   would grow (bounded above by the 10.5 % absolute ceiling).

### 5.4 Verdict

**B — Limited opportunity.**

The <1 % estimate remains credible after all three corrections, and it is now supported by the
stronger of the two original arguments (the activation floor, which is a direct measurement)
rather than the weaker one (the per-bank visibility argument, which was overstated and is now
stated correctly). The corrections move the number from 0.75 % to ≈ 0.74 % and, more
importantly, reclassify it as an **upper bound on residual activation cost** rather than an
achievable gain from a bulk-side organizer.

The Issue 4 investigation does not change the verdict but changes how the baseline must be
described: the FR-FCFS our model implements is not Vortex RTL behavior (there is none) — it is
the policy of Vortex's *default simulation backend* (Ramulator, `Scheduler = FRFCFS`). Any claim
that "DX100 beats / does not beat Vortex's memory controller" must therefore be stated against
Ramulator's FRFCFS configuration, not against the RTL.

**C was considered and rejected.** C would be correct if the headroom estimate rested only on
the visibility argument (which needs unmeasurable per-bank data). It does not: the floor
comparison uses only counters that exist (`dram_reads`, `dram_row_hits`, `dram_row_conflicts`)
and one workload property that is verifiable from source (the map). The residual uncertainty
(§5.3) is quantified, not open-ended.

---

## 6. Remaining uncertainties

| # | Uncertainty | Label | What would settle it |
| --- | --- | --- | --- |
| U1 | Per-bank / per-row queue distribution | **?** | Per-bank outstanding counters, per-row histogram (§7, I5/I6 of M11) |
| U2 | Share of the 154-activation surplus attributable to PTE/descriptor interleaving | **?** | Requester tag on `DQ` (M11 §7.2) |
| U3 | Index-target row-hit inflation in arm B | **H** | Move `bulk_index_base` off the 8 MiB boundary and re-measure arm B |
| U4 | Whether a deeper queue would actually help | **H** | Raise `l1_mshr`/`l2_mshr` and re-run (the experiment `dram-queue-sensitivity.md` §5.2 could not run) |
| U5 | Whether the bank model (FCFS-per-bank) would change any conclusion | **H** | Not runnable here: our model is not the bank model; only a backend swap in Vortex could compare |
| U6 | Page coverage of the 512 pages by the bulk element subset | **D**, ~10⁻⁴ failure probability | A distinct-`(bank,row)` counter |

---

## 7. Recommended next experiment (single, bounded)

If exactly one experiment is authorised, it should be the one that discriminates the two live
explanations, because everything else is already bounded:

**Instrument `DQ` with a requester tag and two counters — `hit_busy_sum` (row hits that were
ineligible because the bank was busy) and a per-`(bank,row)` activation count — then re-run
W1 κ=0.25 arm C unchanged.** Decision rule:

* If `hit_busy_sum` is a large fraction of the 2 204 conflicts, the residual is **bank timing**
  and no organizer can remove it ⇒ verdict B is closed, permanently.
* If `hit_busy_sum` is near zero and the per-row activation count is well above the distinct
  pair count, the residual is **ordering** ⇒ the ceiling is real but a policy could approach it;
  the next step would then be the closed-loop shadow oracle of M11 §8.2(b), still without
  changing the schedule.
* If the per-row count comes out *below* 2 051, the coverage assumption of §2.2 is false and the
  headroom must be recomputed upward (bounded by 10.5 %).

This is the smallest instrumentation that converts U1, U2 and U6 from unknowns into
measurements, and it changes no scheduling behavior. A visibility experiment (U4) should follow
only if the ordering branch fires.

---

## 8. Summary

1. **Per-bank visibility — corrected.** The queue is global; a single bank could hold all 16
   entries. The defensible aggregate bound is one bank-row's worth of lines (16), not half a row
   per bank; the even 8/8 split happens to hold for sequential runs because consecutive lines
   alternate banks by construction of the map. Per-bank counts are not observable anywhere.
2. **Activation floor — validated with corrected terminology.** 2 048 is an *idealized* lower
   bound under a stated (and near-certain) coverage assumption; adding descriptor/PTE pairs the
   realistic floor is ≈ 2 051. Measured 2 206 ⇒ surplus ≈ 154 ⇒ ≈ 0.74 % of cycles, unchanged
   from the original estimate but now an upper bound on residual cost.
3. **Periodicity artifact — real in address space, inert in the verdict's arm.** For arm C the
   index phase ends 637 cycles before the first target issue, so index and target lines never
   share the queue and **no spurious row hits are possible**; the artifact can only affect arm
   B, bounded by ≤ 5.7 % of its reads. Excluding index traffic would *lower* the rate
   (≈ 83.2 % vs 83.8 %) because the index sweep is the more row-local stream.
4. **Vortex — delegates, and the RTL has nothing to delegate.** `hw/rtl/mem` is AXI/bus
   arbitration only; SimX and rtlsim both drive `DramSim`, whose default is Ramulator with
   `Scheduler = FRFCFS`, `AddrMapper = RoBaRaCoCh`, one channel per platform memory bank (= 2),
   plus an alternative JEDEC-level bank model that is **FCFS per bank, not FR-FCFS**. Our
   standalone FR-FCFS matches the default backend's policy family, is parameter-matched to
   `VX_CFG_PLATFORM_MEMORY_*`, and is not a port of anything in Vortex.
5. **Verdict: B, limited opportunity**, with the <1 % figure reclassified as an upper bound on
   residual activation cost and the estimate now resting on the activation floor rather than on
   the corrected visibility argument.
