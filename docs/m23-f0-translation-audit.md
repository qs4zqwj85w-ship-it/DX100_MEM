# M23-F0 — Translation Model Audit and Validation

**Status.** Audit and validation only. No simulator source was modified, no M23
configuration was changed, no historical result, trace, log or report was
touched, and no architecture mechanism (PWC, large TLB, huge pages) was added.
Every micro-test below runs from a **new, separate configuration**
(`sim/configs/m23f0_micro.cfg`) and writes into a **separate output directory**
(`build/wm/m23f0/`).

**Labels used throughout.**

* **[V]** verified — read from the source and/or measured in a run whose log is
  named.
* **[I]** inferred — follows from the source, but no instrumented run isolates it.
* **[?]** unresolved — stated as an open question, not as a conclusion.

---

## 0. Answer first

1. **Two walks do run concurrently, and both walkers can be occupied in the same
   cycle** [V]. The limit is not the walker count in practice: it is the four-entry
   per-core dTLB MSHR file, which caps independent walks at 4 no matter how many
   walkers are configured [V, micro-test §4].
2. **A walk costs 300 cycles, not 150** [V]. `Config::walk_latency_per_level = 150`
   is charged **per level** and `Config::levels = 2`, so
   `lat = levels × walk_latency_per_level = 300` for every walk.
3. Therefore **`cycles ≈ walks × 150` is not evidence of a 150-cycle walk.** It is
   the *throughput* identity of two walkers each holding a walk for 300 cycles:
   `walks × 300 / 2 = walks × 150`. In M23-B/M23-D the identity holds to 0.004 %
   because `walker_busy = walks × 300` exactly and both walkers are busy for the
   whole run [V].
4. **Walk latency is a pure fixed delay and is not coupled to memory service**
   [V]. The page-table fetches are real L2/DRAM traffic, but `ptw_tick` discards
   every `l2_probe` return code and sets the completion time to `cycle + 300`
   regardless of when (or whether) those fetches complete.
5. **The reported `cycles ≈ walks × 150` is a genuine walk-throughput bound on a
   model whose walk latency is a constant** — both things at once. It is not an
   accounting identity and not a coincidence, but it is also not a validated
   translation timing: a walk never waits on its own PTEs.
6. **A real defect exists and reproduces today**: on the historical 32-lane
   Dubcova3 trace the SIMT arm strands exactly one request forever
   (`completed 1 105 024 != 1 105 025`), the machine then does *nothing* for the
   remaining guard cycles, and the run is reported as a deadlock [V]. The stranding
   rate scales with dTLB pressure: 1 request at 16 entries / 4 ways, **102** at
   8 entries / 1 way, **0** at 64 entries / 16 ways [V]. Cause: §6.

---

## 1. Source-code map

All translation code lives in `sim/src/memory.cpp` (implementation) and
`sim/src/sim.hpp` (types). Nothing translation-related is in `bulk.cpp` or
`ab.cpp`; the bulk engine reaches translation only through
`MemorySystem::submit`.

| # | Concern | File : function (line) | Data structures |
|---|---|---|---|
| 1 | dTLB lookup + allocation of a translation request | `memory.cpp : MemorySystem::tlb_probe` (118) | `tlb_e_` (`TlbEntry`, sim.hpp:673), `tlb_m_` (`TlbMshr`, 674) |
| 2 | L2 TLB lookup (shared, pipelined) | `memory.cpp : MemorySystem::l2tlb_tick` (190) | `l2tlb_e_` (`L2Entry`, 675), `l2tlb_m_` (`L2Mshr`, 678), `l2tlb_pipe_` (`PipeEnt`, 687) |
| 3 | TLB miss handling | `tlb_probe` → `l2tlb_pipe_`; `l2tlb_tick` → `L2Mshr` alloc or join | `TlbMshr.parked`, `L2Mshr.reqs/core_mask` |
| 4 | Page-walk request allocation and dispatch | `memory.cpp : MemorySystem::ptw_tick` (249), first loop | `walk_busy_` (713), `walk_cache_` (712) |
| 5 | Walker concurrency and availability | `ptw_tick` (249), `for (i < cfg_.walkers) if (walk_busy_[i] <= cycle)` | `walk_busy_` (vector of absolute free-at cycles) |
| 6 | Walk latency and completion | `ptw_tick` (249): `lat = levels × walk_latency_per_level`; `m.ready = cycle + lat`; retire loop at the end of the same function | `L2Mshr.ready` |
| 7 | Page-walk queueing | `l2tlb_tick` (190), `l2tlb_req_per_entry` / MSHR-full retry; walk admission gated by `dtlb_mshr` × `dtlb_parked` in `tlb_probe` | `l2tlb_m_` (8 entries), `TlbMshr.parked` (≤2) |
| 8 | Translation response delivery | `tlb_fill` (168) + `pending_fill_` drained in `MemorySystem::tick` (671); request picks the PPN up in `tick` stage 1 | `pending_fill_` (714), `Req::tlb_at` |
| 9 | Interaction with L1/L2/DRAM/outstanding | `l2_probe` (309) called from `ptw_tick` with `client = 2`; `Req` budget `R` in `system.cpp : Sim::run` | `l2_m_`, `dram_wait_`, `inflight_` |

Key constants: `VPN_SHIFT = 6`, `PAGE_LINES = 64` (4 KiB page = 64 lines),
`ROOT_BASE`, `LEAF_BASE`, `PPN_OFFSET` (`memory.cpp:12-18`). Page size is
hard-coded; see §7.

Defaults that matter (`sim.hpp:57`): `walkers = 2`, `walk_cache = 16`,
`walk_latency_per_level = 150`, `levels = 2`; `dtlb_entries = 16`,
`dtlb_ways = 4`, `dtlb_mshr = 4`, `dtlb_parked = 2`; `l2tlb_entries = 512`,
`l2tlb_ways = 4`, `l2tlb_mshr = 8`, `l2tlb_req_per_entry = 8`,
`l2tlb_latency = 4`.

---

## 2. Walk timing and concurrency, from the code

### 2.1 The path, in execution order

```
submit()                         tlb_probe(): 1-cycle hit  ──► ready = cycle + 1
  miss ──► dTLB MSHR (4/core, ≤2 parked per VPN)  ──► l2tlb_pipe_ (ready = cycle + l2tlb_latency = 4)
              │  (refused if all 4 MSHRs hold other VPNs, or 2 already parked)
              ▼
l2tlb_tick():  probe 512-entry / 4-way shared L2 TLB
  hit  ──► pending_fill_ (dTLB fill next tick)
  miss ──► join or allocate one of 8 L2Mshr (ready = NONE  ⇒ needs a walk)
              ▼
ptw_tick():  for each L2Mshr with ready == NONE, in slot order:
  walk_cache_ hit ──► ready = cycle + 1          (no walk, no PTE traffic)
  else             ──► lat = levels × 150 = 300
                       for lvl in 0..levels-1: l2_probe(pte_line_of(lvl, vpn), cycle, ready)
                       walks++;  walk_latency += 300;  m.ready = cycle + 300
                       walk_cache_.push_back(vpn)        (FIFO, 16 entries)
  walk_busy_[w] = m.ready        (w = first index with walk_busy_[w] <= cycle)
              ▼
retire:  m.ready <= cycle ──► install in L2 TLB, push pending_fill_ per core in core_mask, free the MSHR
```

### 2.2 The specific questions

**Does the simulator permit two independent walks to run concurrently?** Yes [V].
`walk_busy_` is a vector of `cfg.walkers` independent absolute timestamps; two
L2 MSHRs can be issued in the same `ptw_tick` call to different walker slots, and
each keeps its own completion cycle. Measured: `walker_max = 2` in every M23-B and
M23-D cell, and `walker_busy = 2 × walks × 300` exactly (B/M0: 57 022 800 =
2 × 190 076 × 300). A serialized model could not produce `walker_busy / walks =
600`.

**Can both walkers be occupied during the same cycle?** Yes, and they are, for the
whole run: `walker_busy / cycles = 2.000` on B/M0 and D/M0, i.e. 100.00 % of both
walkers busy in every cycle of the run [V]. This is not an assumption about the
configuration; it is the measured busy integral.

**Is walk latency counted per walker, per request, or globally?** Per request, on
a per-walker timer [V]. `ptw.walk_latency` is a plain sum, `walks × 300`; it is a
reporting counter, never consulted by the model.

**Is the 150-cycle latency a fixed delay, a memory transaction, or a combination?**
A **fixed delay**, charged `levels` times [V]. `lat = 300` is computed before the
PTE fetches and is not adjusted by them:

```cpp
u64 lat = static_cast<u64>(cfg_.levels) * cfg_.walk_latency_per_level;
for (u32 lvl = 0; lvl < cfg_.levels; ++lvl) {
  u64 ready = NONE;
  l2_probe(pte_line_of(lvl, m.vpn), cycle, ready);   // return code ignored
  ptw.pte_fetches++;
}
m.ready = cycle + lat;
```

So the PTE fetch is *real traffic* but *not* part of walk timing. A walk whose PTE
line is missing in L2 and queued behind 30 data requests still "completes" 300
cycles after issue.

**Can page walks overlap with one another?** Yes [V]. In the micro-test with 4
walkers, `walker_busy = 300 000` over a 76 112-cycle run means 3.94 walks
in flight on average.

**Can page walks overlap with ordinary data requests and execution?** Yes [V].
`ptw_tick`, `dram_tick` and `cache_tick` are independent calls in
`MemorySystem::tick`, and a walk holds only a walker slot — it does not hold an
`Req` slot, an L1/L2 data MSHR, or an `inflight_` entry. The PTE fetches do compete
for **L2 MSHR entries and DRAM queue slots**, which is the only coupling.

**Does a requesting access wait independently for its translation, or does a
context/batch wait for all translations?** Independently, per request [V]. Each
`Req` carries its own `stage`, `ready` and `tlb_at`; stage 1 advances when *that*
request's PPN is present. There is no barrier over a batch or a warp. (The
front-end has a separate head-of-line rule — one instruction's lines all hold
slots before the next instruction is formed — but that is not translation.)

**Are page-walk requests subject to queueing or backpressure?** Yes, in two
places [V]:
* dTLB MSHR file: 4 entries per core, at most `dtlb_parked = 2` requests may wait
  on one VPN. A miss for a fifth distinct VPN is **refused at `submit`**
  (`refuse_full`), and the front-end keeps the request outside the memory system.
* L2 TLB MSHR file: 8 entries; a miss for a ninth VPN is retried after
  `l2tlb_latency` (`l2tlb.walk_stalls`).

In every M23-B/M23-D cell `l2tlb_walk_stalls = 0` — the 8-entry walk queue is
never full. All backpressure is at the 4-entry dTLB MSHR: `tlb0_mshr_occ /
cycles = 3.99 / 4` on both B/M0 and D/M0 [V], and `tlb_refuse_full` is 110 739 514
(B/M0) and 703 491 625 (D/M0).

**Are page-table accesses represented as actual memory traffic?** Yes [V]. They go
through `l2_probe`, allocating a real `l2_m_` MSHR and a real `dram_wait_` entry,
tagged `client = 2`. `pte_fetches = 2 × walks` in every cell (M23-B/M0: 380 152 =
2 × 190 076).

They **bypass the L1 entirely** (`l2_probe`, not `l1_probe`), even with
`mem_path = l1`.

**Is there a page-walk cache?** Yes, and it is minimal [V]. `walk_cache_` is a
`std::deque<u64>` of **VPNs** — not PTE addresses — of capacity
`Config::walk_cache = 16`, searched linearly, evicted **FIFO** (not LRU). A hit
sets `ready = cycle + 1`, does not increment `walks`, and issues no PTE traffic.
It is keyed on the VPN, so it is effectively a 16-entry "recently walked pages"
list, and it is only consulted when a walk would start. Measured effect: zero on
M23-D (all cells `walk_wc_hits = 0`) and 10 473–10 883 hits on the DX-100 Criteo
cells, where `walks = l2tlb_misses − walk_wc_hits` holds exactly [V].

**Are page-walk accesses included in L2/DRAM counters?** Yes [V]. `l2.reads`,
`l2.hits`, `l2.misses`, `dram.reads`, `dram.row_hits/conflicts` and the per-client
attribution (`cc[2]`) all include PTE traffic.

**Is there any hidden serialization point that limits effective walker
concurrency?** Yes: **`dtlb_mshr`** [V]. A core can have at most
`dtlb_mshr × dtlb_parked = 4 × 2 = 8` requests inside translation, spread over at
most **4 distinct VPNs** — and a VPN becomes a walk. With `levels = 2` and
`walkers = 2`, walker throughput is one walk per 150 cycles, which is under the
4-VPN supply rate, so it only binds at ≥ 4 walkers. Measured in §4:

| walkers | walker_max | cycles |
|---:|---:|---:|
| 4 | 4 | 76 112 |
| 8 | **4** | **76 112** |
| 8, `dtlb_mshr = 8` | **8** | **38 164** |

Going from 4 to 8 walkers changes nothing; raising `dtlb_mshr` to 8 lets all eight
walkers run. That is the hidden serialization, isolated.

---

## 3. Counter reconciliation — M23-B Criteo and M23-D ogbn-products

Source logs: `build/wm/m23b_*.txt`, `build/wm/m23d_train_*.txt` (unchanged).

| workload | cfg | cycles | walks | walks × 150 | Δ | Δ% | walker_busy | walks×300 | avg active walkers |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| B Criteo | M0 | 28 511 774 | 190 076 | 28 511 400 | +374 | 0.0013 | 57 022 800 | 57 022 800 | 2.000 |
| B Criteo | 2×8K | 15 778 298 | 104 877 | 15 731 550 | +46 748 | 0.30 | 31 473 573 | 31 463 100 | 1.994 |
| B Criteo | 2×16K | 12 273 324 | 81 493 | 12 223 950 | +49 374 | 0.40 | 24 458 558 | 24 447 900 | 1.993 |
| B Criteo | 2×32K | 8 942 139 | 59 300 | 8 895 000 | +47 139 | 0.53 | 17 800 883 | 17 790 000 | 1.991 |
| D ogbn | M0 | 176 184 939 | 1 174 566 | 176 184 900 | +39 | 0.00002 | 352 369 800 | 352 369 800 | 2.000 |
| D ogbn | 2×8K | 156 713 716 | 1 044 628 | 156 694 200 | +19 516 | 0.012 | 313 388 400 | 313 388 400 | 2.000 |
| D ogbn | 2×16K | 139 088 324 | 927 157 | 139 073 550 | +14 774 | 0.011 | 278 147 100 | 278 147 100 | 2.000 |
| D ogbn | 2×32K | 113 518 967 | 756 473 | 113 470 950 | +48 017 | 0.042 | 226 941 900 | 226 941 900 | 2.000 |

**Reproduction of the identity.**
`cycles − walks × 150` is 374 cycles on B/M0 (1.3 × 10⁻⁵ of the run) and 39 cycles
on D/M0 (2.2 × 10⁻⁷). The reason is exact and not statistical:
`walker_busy = walks × 300` to the cycle, and `walker_busy / cycles = 2.000`, so
`cycles = walker_busy / 2 = walks × 150` follows from the two-walker configuration,
not from the workload.

**What the residual is.** The small positive Δ appears once the DX-100 cells
introduce data-path replays: B/8K has 14 207 replays and Δ = 46 748; B/32K has
344 779 replays and Δ = 47 139. Δ is the non-translation tail (data service after
the last walk retires), not an error in the walk count.

**Other sources of cycles — measured, not assumed.**

| counter | B Criteo M0 | D ogbn M0 | reading |
|---|---:|---:|---|
| `dram_starved_cycles` | 28 109 630 (98.6 %) | 167 190 562 (94.9 %) | DRAM has nothing queued |
| `tlb0_mshr_occ` / cycles | 3.99 / 4 | 3.99 / 4 | dTLB MSHR file saturated |
| `simt_issue_stalls` | 113 685 562 | 703 536 617 | front-end refused at `submit` |
| `tlb_refuse_full` | 110 739 514 | 703 491 625 | refusal reason: 4 VPNs resident |
| `l1_stall_cycles` | 0 | 0 | caches never backpressure |
| `l1_mshr_max` / `l2_mshr_max` | 8 / 7 | 7 / 7 | 128 / 64 available |
| `lat_tlb_mean` (cycles) | 337 | 589 | translation share of request latency |
| `lat_mean_x100` (cycles) | 349 | 630 | total request latency |

Both workloads are **translation-throughput bound with an idle memory system**:
the DRAM is starved for ~95–99 % of cycles while the two walkers are busy for
100 % of them. Nothing else is close to a limit.

**Walker utilization and queue occupancy.**

| | B Criteo M0 | D ogbn M0 |
|---|---:|---:|
| configured walkers | 2 | 2 |
| max simultaneous walks | 2 | 2 |
| average active walkers | 2.000 | 2.000 |
| walker utilisation | 100.00 % | 100.00 % |
| L2 walk-queue stalls (`l2tlb_walk_stalls`) | 0 | 0 |
| dTLB refusals (`refuse_full`) | 110 739 514 | 703 491 625 |
| max waiters on one VPN (`tlb_max_parked`) | 2 | 2 |

Queue occupancy is not reported as a time series anywhere in the model; the
available proxies are the refusal counts above and `tlb0_mshr_occ`. Both say the
same thing: the submission queue in front of translation is full essentially
always, and the walk queue behind it is never full.

**Verdict on the nature of the agreement.** All three proposed explanations are
partly right, and the distinction matters:

* *Genuine walk-bound execution* — **yes**, in the sense that translation
  throughput is the binding resource: DRAM idle 95–99 %, caches at 8/128 MSHRs.
* *Serial latency accounting* — **not serialization** (walks overlap, 2.000 busy),
  but the latency is a **constant**, so the model cannot express "this walk is slow
  because its PTE is behind 30 data requests".
* *Accounting identity* — **no**. `walks × 150` is not computed anywhere; it is an
  emergent relation `walkers × (levels × walk_latency_per_level) / walkers` that
  happens to coincide with the total because the machine has nothing else to do.
* *Implementation artifact* — **partly**. The factor 150 is `300 / 2`, and 300 is a
  configured per-level number applied twice. Anyone reading `walks × 150` as "150
  cycles per walk" is reading the model backwards.

---

## 4. Independent walker micro-test

**Configuration:** `sim/configs/m23f0_micro.cfg` (new, separate). Workload
`P = 1000` pages, `Lambda = 1000` lines (one line per page), `N = 1000` accesses,
`lanes = 1`, `kappa = 0`, `rho_dup = 0`, so the generator draws a permutation:
**1000 accesses → 1000 distinct lines → 1000 distinct 4 KiB pages, each touched
once**, one access per instruction. Everything below the workload is identical to
the shared section of `configs/m23_m0.cfg`. Output directory
`build/wm/m23f0/` (new).

Realized trace, from the run header: `accesses=1000 instrs=1000 lines=1000
pages=1000 kappa=0.0000 rho_line=0.0000` — no line, page or word reuse [V].

**Command (one cell per walker count):**

```bash
cd sim && ./bin/dx100sim --config configs/m23f0_micro.cfg --set walkers=<W> --all \
  > ../build/wm/m23f0/micro_walkers<W>.txt
```

### 4.1 Results

| walkers | cycles | ideal `ceil(1000/W) × 300` | Δ | `walks` | `walker_max` | avg active | util | `inflight_max` | `lat_tlb_mean` | `tlb_refuse_full` |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 300 038 | 300 000 | +38 | 1000 | 1 | 1.000 | 99.99 % | 5 | 1 198 | 297 805 |
| 2 | 150 064 | 150 000 | +64 | 1000 | 2 | 1.999 | 99.96 % | 6 | 599 | 148 406 |
| 4 | 76 112 | 75 000 | +1 112 | 1000 | 4 | 3.942 | 98.54 % | 8 | 304 | 74 700 |
| 8 | 76 112 | 37 500 | +38 612 | 1000 | **4** | 3.942 | 49.27 % of 8 | 8 | 304 | 74 700 |

`completed = 1000`, `requests_generated = 1000`, `conservation = OK`,
`pte_fetches = 2000 = 2 × walks`, `walk_wc_hits = 0` in all four cells [V].

### 4.2 Comparison with the brief's expectation, and the correction

The brief's bound `ceil(1000/2) × 150 = 75 000` assumes a **150-cycle** walk. The
implementation charges `levels × walk_latency_per_level = 2 × 150 = 300`. The
correct bound for two walkers is therefore

```
ceil(1000 / 2) × 300 = 150 000 cycles
```

and the measured value is **150 064 cycles (+64, 0.043 %)**. The 2× discrepancy is
the whole point of the audit: the number 150 in `walks × 150` is not a walk
latency, it is a *per-walker service interval* at two walkers.

**Where the +64 comes from** [I]: pipeline fill and drain. The first walk cannot
start until a request has been admitted through the 4-cycle L2 TLB probe, and the
run ends only after the last translated request's data stage returns (measured
mean data-stage latency `lat_cache_mean = 40` cycles). A few tens of cycles of
skew at each end is the expected size, and 64 is that order.

**Where the +1 112 at four walkers comes from:** 75 000 of the 76 112 cycles are
pure walker occupancy (`walker_busy / 4`), so the extra 1 112 is drain skew, not
lost concurrency. It is larger than the two-walker skew because admission is
serialized (one submit attempt per cycle at `warps_per_core = 1`) while at most
four distinct VPNs may be resident in the dTLB MSHR file, so the final four walks
cannot all be started the instant the previous four retire.

### 4.3 Interpretation

* The walker pool is real: 1 → 2 → 4 walkers scales 300 038 → 150 064 → 76 112,
  i.e. almost exactly 2× per doubling [V].
* **8 walkers gives zero improvement** and `walker_max = 4`: the four-entry dTLB
  MSHR file, not the walker count, is the concurrency cap [V].
* **Confirmatory ablation** (`build/wm/m23f0/micro_mshr8_walkers8.txt`):
  `--set dtlb_mshr=8 --set walkers=8` → **38 164 cycles, `walker_max = 8`,
  `inflight_max = 16`**. Doubling the MSHR file halves the time again and lets all
  eight walkers run. The cap is therefore exactly `dtlb_mshr` [V].
* Queueing shows up only at the dTLB: `l2tlb_walk_stalls = 0` in every cell, while
  `tlb_refuse_full` falls from 297 805 to 74 700 as walkers are added and then
  stops falling at 4 [V].

---

## 5. SIMT baseline concurrency audit

From `configs/m23_m0.cfg` / `m23_dx*.cfg` §1 (identical in all four) and
`sim.hpp`:

| quantity | value | source |
|---|---:|---|
| lane width | **4** | `lanes = 4` |
| warps / execution contexts per core | **4** | `warps_per_core = 4` |
| cores | 1 | `cores = 1` |
| max resident threads | 16 | 4 warps × 4 lanes |
| L1 MSHRs | **128** | `l1_mshr = 128` (M16 strengthening) |
| L2 MSHRs | 64 | `l2_mshr = 64` |
| global outstanding budget | **R = 128** slots | `R = 128` |
| translation outstanding, per core | **4 MSHRs × 2 parked = 8 requests, ≤4 distinct VPNs** | `dtlb_mshr = 4`, `dtlb_parked = 2` |
| per-core LSU cap | unlimited | `lsu_pending = 0` |
| max observed in flight (memory system) | **13** (B Criteo), **12** (D ogbn) | `inflight_max` |
| average in flight | 4.39 (B), 4.28 (D) | `inflight_occ / cycles` |
| peak slot residency | 128 / 128 | `simt_peak_slot = 128` |
| peak requests formed but **not admitted** | 124–126 | `unissued_max` |

### 5.1 Why ogbn-products reaches only 12 in flight with a budget of 128

The budget and the memory system are not the same thing [V]:

1. `R = 128` bounds **slot residency**: a request claims a slot when it is formed
   and releases it when its fill returns. Of the 128 slots at peak, 124 are held by
   requests that `tlb_probe` **refused** (`tlb_refuse_full = 703 491 625`). They
   sit in `SimtCore::held`, outside the memory system entirely.
2. Inside the memory system, a request must own one of 4 dTLB MSHR entries
   (`dtlb_parked ≤ 2` each). With one core that is **at most 8 requests in
   translation and at most 4 distinct pages walking at a time**.
3. The remaining ~4 of the 12 are requests that have finished translation and are
   in the cache stage (`l1_mshr_max = 7` of 128, so the caches are nowhere near a
   limit).

So `inflight_max = 12` is **4 × 2 + ~4**, and the binding constraint is the
translation MSHR file, 32× smaller than the global budget. This is the same 4-VPN
cap the micro-test isolated in §4.3.

For contrast, the same machine with the Bulk engine reaches `inflight_max = 119`
(2×16K) and `128` (2×32K) on the identical trace, because engine requests carry
their own index/target streams and are not parked on the 4-entry dTLB MSHR at the
same rate [V]. The baseline's low occupancy is a property of its translation
admission policy, not of R.

### 5.2 Do memory dependencies prevent the baseline from filling its budget?

No [V]. The front-end forms requests freely (peak 128 slots, 124 waiting), the L1
and L2 MSHR files are at 8/128 and 7/64, and `l1_stall_cycles = 0`. Nothing in the
data path is full. The only thing that stops a formed request from entering the
memory system is `tlb_probe` returning false.

---

## 6. The 32-lane Dubcova3 stall

### 6.1 Where it is actually recorded

The stall is documented in **`docs/m18a-lane-width-sensitivity.md` §5**, not in
M20. M20's own 32-lane Dubcova3 cell **completes** (`build/wm/m20_dubcova3_full_w8192_l32.txt`,
arm A cycles 3 366 104, `A_dram_starved_cycles = 22 987`, gates passed). The two
runs used **different traces**: `m18a_dubcova3_full_l32.bin`
(sha256 `d58eda11…`, 4 945 unique pages) versus `m20_dubcova3_full_l32.bin`
(sha256 `5b535e9a…`, 4 989 unique pages, different `rowptr_base`). The M18a report's
"32-lane point is excluded" statement is therefore about the M18a trace, and M20's
32-lane Dubcova3 number (1.124) is a *different input*, not a fix.

### 6.2 It reproduces with today's binary

```
./sim/build/wm/m18_run.sh build/wm/m18a_dubcova3_full_l32.bin 8192 <tag> \
    --set lanes=32 --set max_cycles=20000000
→ VIOLATION: A: completed == requests_generated: 1105024 != 1105025
  → build/wm/m23f0/dubcova3_l32_repro.txt
```

Identical to the historical record: **exactly one** request short, out of
1 105 025.

### 6.3 What the machine is doing while stalled

Two runs of the same cell at guard 5 000 000 and 20 000 000 cycles differ in
**exactly two** counters (`build/wm/m23f0/dubcova3_l32_repro_5M.txt` vs
`…repro.txt`):

| counter | 5 M guard | 20 M guard | delta |
|---|---:|---:|---:|
| `A_dram_starved_cycles` | 1 665 814 | 16 665 814 | **+15 000 000** (1.00/cycle) |
| `A_inflight_occ` | 380 315 342 | 395 315 342 | **+15 000 000** (1.00/cycle) |
| every other `m16 A_*` counter | — | — | **0** |

So for the last 15 M cycles there is exactly **one** resident request, it
generates **no** L1/L2/DRAM traffic, no walk is issued, and no completion occurs.
Progress stops at ≈ 3.33 M cycles — the same order as the 3.37 M cycles the M20
variant needs to finish the same trace [V].

### 6.4 Cause [I, with the mechanism verified by ablation]

The stranded request is in `Req::stage == 1`, and the code path that can strand it
is in `MemorySystem::tick`:

```cpp
if (r.stage == 1) {
  if (r.ready != NONE && cycle < r.ready) { keep.push_back(r); continue; }
  ...search tlb_e_ for r.vpn...
  if (!found) { keep.push_back(r); continue; }     // <-- retries forever
  ...
}
```

`r.ready` is `NONE` for a request that allocated a dTLB MSHR, so this loop re-reads
the **array** every cycle but never calls `tlb_probe` again. The only thing that can
make `found` true is `tlb_fill`.

`tlb_fill` is applied from `pending_fill_` at the end of the same `tick`, and the
fill loop then **clears the MSHR** for that VPN. If more than
`dtlb_entries / dtlb_ways` fills target one set within that single tick, `tlb_fill`'s
LRU replacement evicts a VPN that was installed earlier in the same tick — and any
request that was parked on that evicted VPN has already lost its MSHR, so nothing
re-issues its translation. It waits for a TLB entry that never comes, `inflight_`
never empties, `MemorySystem::idle()` stays false, and the run hits the guard.

The M18a 32-lane trace is where this first fires because it has 72 688 dTLB misses
and 42 117 L2 TLB hits with only 4 sets × 4 ways, so a tick can carry five or more
fills into one set. [I] — the eviction itself is not directly instrumented; what is
verified is the stranding, its arithmetic, and its scaling.

**Ablation that isolates dTLB pressure as the trigger** (same trace, lanes = 32,
guard 8 M):

| dTLB | stranding | A cycles | gates |
|---|---|---:|---|
| 8 entries / 1 way | **102 requests lost** (1 104 923 of 1 105 025) | 8 000 000 (guard) | FAILED |
| 16 entries / 4 ways (historical) | **1 request lost** | 20 000 000 (guard) | FAILED |
| 64 entries / 16 ways | **0** | 3 356 678 | PASSED |

The number of stranded requests scales monotonically with dTLB pressure, which is
what a fill/eviction race predicts and what a resource deadlock does not.

### 6.5 Does the bug remain present?

**Yes** [V]. It reproduces with the current binary on the historical trace, at the
historical settings. It is **not fixed**, and this audit deliberately does not fix
it: the fix touches the translation retry policy, which the brief excludes. It is
also **latent rather than absent** elsewhere — the micro-test (1000 pages, 4 sets ×
4 ways, at most 4 concurrent fills) never trips it because 4 fills cannot exceed
four ways in a set.

---

## 7. Translation-model capability inventory

Legend: **[V]** present and verified in code, **[A]** absent.

| capability | status | evidence / what it would take |
|---|---|---|
| Configurable walker count | **[V]** | `Config::walkers`; `walk_busy_` sized from it. Swept 1/2/4/8 here. |
| Independent walker concurrency | **[V]** | per-walker `walk_busy_[i]`; measured `walker_max` up to 8. |
| Configurable walk latency | **[V]** | `walk_latency_per_level` × `levels`; a constant, not derived from PTE service. |
| Page-table hierarchy depth | **[A]** in substance | `levels` exists, but `pte_line_of(level, vpn)` maps **every level ≥ 1 to the same leaf line** (`LEAF_BASE + vpn/16`). `levels = 3` reads the leaf twice. A real depth needs per-level table geometry (base, span, index bits) and a per-level latency. |
| Actual PTE memory accesses | **[V]** | `l2_probe(pte_line_of(...), client = 2)` — real L2 MSHR + DRAM entry, 2 per walk. |
| PTE caching | **[A]** | no PTE-address cache; `walk_cache_` keys on **VPN**, not PTE line, so a page with a resident PTE still walks. A PTE cache needs a tag = PTE line address, a fill path from `l2_probe`, and a latency charged to the walk. |
| Page-walk cache | **[V]** (minimal) | `walk_cache_`, 16 entries, **FIFO**, linear search, hit = `cycle + 1`, no walk counted, no traffic. Upgrade: LRU + VPN→PPN payload + configurable capacity. |
| PTE/data traffic contention | **[V]** (partial) | shared L2 MSHRs (64) and DRAM queue; no bandwidth or port arbitration, and PTE fetches bypass the L1. |
| Configurable L2 TLB capacity / associativity | **[V]** | `l2tlb_entries = 512`, `l2tlb_ways = 4`, set-indexed by `vpn % sets`, LRU. |
| TLB lookup latency | **[A]** | dTLB hit is hard-coded `ready = cycle + 1` (`tlb_probe`); L2 TLB has `l2tlb_latency = 4` but the dTLB's is not a knob. Would need `dtlb_latency` threaded through `tlb_probe`/`tick`. |
| Configurable page size | **[A]** | `VPN_SHIFT = 6`, `PAGE_LINES = 64`, `va >> 12` and `(vpn / 16)` are compile-time constants. Would need page-shift and PTE-span parameters through `pa_line_of_va`, `tlb_probe`, `pte_line_of`, `build_trace`'s page count. |
| Huge-page translation | **[A]** | no page-size field in `TlbEntry`/`L2Entry`, no PTE-level size bit. Would need a size tag per entry and a walker that stops at the level whose entry is a huge page. |
| TLB replacement policy | **[A]** as a choice | LRU everywhere via `lru_clock_`; no policy enum. Random/FIFO would need a policy field and a second victim-selection branch in `tlb_fill`/the L2 retire loop. |
| Translation request queueing and backpressure | **[V]** | 4 dTLB MSHRs × 2 parked (refusal at `submit`), 8 L2 MSHRs (`walk_stalls`), `l2tlb_req_per_entry = 8`. |
| Walk throughput coupling to PTE latency | **[A]** | completion is `cycle + levels × 150` regardless of `l2_probe`'s result. Would need the walk to complete on PTE arrival (or `max(fixed, pte_ready)`). |

---

## 8. Recommended next implementation sequence

Ordered by "what blocks a trustworthy sweep", not by ambition. None of this is
implemented here.

1. **Fix the stage-1 stranding (§6.4).** On a stage-1 miss, re-call `tlb_probe`
   (or re-install the VPN into a MSHR) instead of only re-reading the array; and
   make `tlb_fill` in a single tick not evict an entry installed earlier in the
   same tick. Add a regression test on the M18a 32-lane trace asserting
   `completed == requests_generated`. **This must precede any translation sweep**,
   because a sweep that changes dTLB geometry changes how often the model
   deadlocks.
2. **Make walk latency a memory transaction, not a constant.** Complete the walk
   when its last-level PTE fetch returns (or at `max(fixed, pte_ready)`), and
   charge the PTE's own L2/DRAM service. Until this exists, no translation-timing
   conclusion can distinguish "fast walker" from "slow memory".
3. **Separate walk throughput from translation admission.** Raise `dtlb_mshr`
   (4 → 16) or decouple "in translation" from "in the dTLB MSHR file". Today
   ≥ 4 walkers are unusable on a single core (§4.3), so any walker-count study is
   measuring the MSHR file.
4. **Add the two missing knobs that the sweep would otherwise confound:** dTLB
   lookup latency and a page-walk-cache policy (LRU vs FIFO, capacity).
5. **Only then** the PWC / large-TLB / huge-page experiments, each as its own
   milestone with the M23-A frozen configs as the baseline.
6. **Report convention:** quote walk latency as
   `levels × walk_latency_per_level` (300 today), and quote `cycles / walks` as a
   *service interval*, never as a walk latency.

---

## Appendix — artifacts produced by this audit

| path | contents |
|---|---|
| `sim/configs/m23f0_micro.cfg` | new, isolated 1000-page walker micro-test configuration; not used by any M23 cell |
| `build/wm/m23f0/micro_walkers{1,2,4,8}.txt` | walker sweep, full `--all` output |
| `build/wm/m23f0/micro_mshr8_walkers8.txt` | confirmatory ablation (`dtlb_mshr = 8`) |
| `build/wm/m23f0/micro_tlbthrash_e4w{1,2}.txt` | dTLB-thrash negative control (no stranding) |
| `build/wm/m23f0/dubcova3_l32_repro.txt` | reproduction of the historical 32-lane stall, 20 M guard |
| `build/wm/m23f0/dubcova3_l32_repro_5M.txt` | same cell, 5 M guard, for the counter delta |
| `build/wm/m23f0/dtlbtest_{small,big}.txt` | dTLB 8/1-way and 64/16-way ablation |
| `build/wm/m23f0/*.time` | wall-clock per micro-test cell |

Nothing else in the repository was modified. No historical log, trace, config or
report was rewritten.
