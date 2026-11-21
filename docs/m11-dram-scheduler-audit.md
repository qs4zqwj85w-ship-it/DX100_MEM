# M11 — Existing DRAM scheduler audit and observability design

**Status:** investigation only. No source file, configuration file, test, baseline or
published report was modified. No simulator run was executed for this task; every
number below is read from archived logs already in the working tree
(`sim/build/wm/`, `sim/build/m9cap/`) or from archived reports.

Evidence tags used throughout:

| Tag | Meaning |
| --- | --- |
| **[S]** | Verified in source. The quoted code is the authority, not the comment around it. |
| **[M]** | Measured: an archived log in this working tree, reproducible from the command in §5.4. |
| **[M\*]** | Measured earlier and recorded in a published report; the raw log is not in this tree. |
| **[D]** | Derived by arithmetic from [S]/[M] values. The derivation is shown. |
| **[H]** | Hypothesis. Requires a new experiment (not authorised here). |

---

## 1. Architecture and source map (Part 1)

### 1.1 One hierarchy, two clients

There is exactly one memory model: `class MemorySystem` (`src/sim.hpp:466`), implemented
entirely in `src/memory.cpp` (581 lines). It contains the per-core dTLB + MSHRs, the
shared L2 TLB, the page-table walker and walk cache, the per-core L1, the shared L2, and
the banked DRAM with its FR-FCFS arbiter. There is no separate DRAM model file and no
second queue.

`Sim` owns one `MemorySystem` (`src/sim.hpp:1150–1151`). Both the SIMT front-end and the
engine reach it through the same object:

* SIMT, plain run: `Sim::run` → `mem_->submit` (`src/system.cpp:173`, retry loop at
  `:220` for the M9 number-of-cores path).
* SIMT, M9 hybrid: `Sim::run_hybrid`'s `simt_step` → `mem_->submit(r, now)`
  (`src/bulk.cpp:1783`).
* Engine: `BulkEngine` holds `MemorySystem& mem_` (`src/sim.hpp:1073`, bound in
  `bulk.cpp:249`) and submits at `bulk.cpp:628` (M5 FIFO issue), `bulk.cpp:894`
  (windowed index issue, `idx_mode >= 3`) and `bulk.cpp:1178` (`win_emit`, target lines).

**Same queue, same scheduler instance: yes, verified.** In an M9 dispatched run the
engine and SIMT requests are pushed into the *same* `dram_wait_` vector and arbitrated by
the *same* `dram_tick` call, in the same cycle order. There is no per-client queue,
no per-client arbiter and no priority between clients anywhere below `submit`.

Note on the arms of the M9 diagnostic: arms A, B and C are three *separate simulations*
(`src/ab.cpp:1187`, `:1203`, `:1222` each construct `Sim sim(c, trace)`), so each has its
own `MemorySystem`. Only arm C (dispatch = 1) has both clients inside one hierarchy. B
(`bulk only`) is therefore a single-client measurement of the same model, not a
different controller.

### 1.2 The complete request path

| # | Hop | Location | What happens |
| --- | --- | --- | --- |
| 1 | Client issue | `system.cpp:173`/`:220`, `bulk.cpp:628/894/1178` | A `Req` (VA line, vpn, core, slot, `bypass_l1`) is offered |
| 2 | Submission / admission | `MemorySystem::submit`, `memory.cpp:478–488` | `tlb_probe`; on success the request enters `inflight_` at stage 1. `false` = "retry next cycle", the caller keeps the request |
| 3 | Translation | `memory.cpp:511–540` (stage 1) | Waits for the dTLB fill, then computes `pa_line = (ppn << 6) | line_in_page` |
| 4 | Cache probe | `memory.cpp:542–567` (stage 2) | `l1_probe` (default path) or `l2_probe` if the path bypasses the L1 |
| 5 | L1 | `memory.cpp:342–374` | Hit → `cycle + 2`; MSHR join; else allocates an L1 MSHR (16 per core) and calls `l2_probe` |
| 6 | L2 | `memory.cpp:298–340` | Hit → `cycle + 4`; MSHR join (64 shared); else allocates an L2 MSHR |
| 7 | **DRAM queue push** | `memory.cpp:330` | `dram_wait_.push_back(DQ{line, ++seq_, cycle, bank, row})` — this is the *only* producer of DRAM requests |
| 8 | Arbitration | `MemorySystem::dram_tick`, `memory.cpp:424–473` | One command per cycle, FR-FCFS (§2) |
| 9 | Bank timing | `memory.cpp:449–458` | `lat = (open ? t_hit : t_miss) + link`; `bank_ready_[bank] = cycle + lat`; row state updated |
| 10 | Completion | `memory.cpp:464–472` | The matching L2 MSHR is released at `cycle + lat + l2_latency`; any L1 MSHR already waiting on the same line is stamped `+ l1_latency` |
| 11 | Client completion | `cache_tick` `memory.cpp:375–423`, then `tick` `memory.cpp:568–575` | MSHRs install into the cache arrays, in-flight requests see `ready` and are returned to the client |

Order inside a cycle is fixed: `l2tlb_tick → ptw_tick → dram_tick → cache_tick`
(`memory.cpp:493–499`), then in-flight retirement. DRAM therefore sees this cycle's PTW
PTE fetches but not this cycle's cache installs.

### 1.3 What never reaches DRAM

A request stops before the DRAM queue in three places:

1. **L1 hit** — `cycle + 2`, no MSHR, no L2 traffic (`memory.cpp:352`).
2. **L1 MSHR join** — another request for the same line is already in flight
   (`memory.cpp:358`).
3. **L2 hit** — `cycle + 4` (`memory.cpp:304`); or **L2 MSHR join** (`memory.cpp:308`).

DRAM requests are therefore exactly the *L2 MSHR allocations* (`l2.mshr_allocs`).
Measured for W1 κ=0.25, arm C: L1 requests 14 153, L2 reads 15 205, L2 hits 1 581,
**DRAM reads 13 589** — i.e. 4.0 % of the line requests were absorbed by the caches and
the rest reached the controller **[M]**.

**The engine does not bypass the L1 in the published configuration.** `Req::bypass_l1`
is set true by the engine at `bulk.cpp:627/893/1177`, but `path_bypasses_l1`
(`sim.hpp:494–496`) is

```cpp
return path_mode_ == 2 || (path_mode_ == 0 && req_wants_bypass);
```

and `configs/ab_controlled.cfg` sets `mem_path = l1` → `path_mode_ = 1`. The expression is
false regardless of `bypass_l1`: **[S]** every bulk request takes the same private L1 (and
its 16 MSHRs) as the SIMT client. The `bypass_l1` flag only has an effect under
`mem_path = native` (`path_mode_ = 0`). This matters for §6: the L1 MSHR pool is a shared
funnel in front of the DRAM queue.

---

## 2. Exact FR-FCFS behaviour (Part 2)

### 2.1 The selection code

`src/memory.cpp:424–473`, verbatim semantics:

```cpp
dram.q_occ_sum += dram_wait_.size();      // diagnostic only
dram.elig_last = 0;
if (dram_wait_.empty()) { dram.starved_cycles++; return; }

size_t best = dram_wait_.size();          // "no candidate"
bool   best_open = false;
u64    eligible  = 0;
for (i = 0 .. dram_wait_.size()-1) {
    q = dram_wait_[i];
    if (bank_ready_[q.bank] > cycle) continue;              // bank busy: not eligible
    ++eligible;
    is_open = bank_open_[q.bank] && bank_row_[q.bank] == q.row;
    if (best == size)            { best = i; best_open = is_open; continue; }
    if (frfcfs && is_open && !best_open)      { best = i; best_open = is_open; continue; }
    if (is_open == best_open && q.arrive < dram_wait_[best].arrive) { best = i; best_open = is_open; }
}
if (best == size) { dram.blocked_cycles++; return; }        // queue non-empty, both banks busy

q = dram_wait_[best];  dram_wait_.erase(begin() + best);    // arbitrary-position removal
open = bank_open_[q.bank] && bank_row_[q.bank] == q.row;
dram.bank_mask |= 1ull << q.bank;
if (open) dram.row_hits++;
else if (bank_open_[q.bank]) dram.row_conflicts++;          // closed bank is NOT counted
if (!open) { bank_open_[q.bank] = true; bank_row_[q.bank] = q.row; }
lat = (open ? t_hit : t_miss) + link;
bank_ready_[q.bank] = cycle + lat;
dram.reads++; dram.busy_cycles += lat;
```

Pseudocode as a policy:

```
every cycle:
    if queue empty:            starved++, return
    candidates = [ q in queue : bank_ready[q.bank] <= now ]
    if candidates empty:       blocked++, return
    pick = argmin over candidates of (not row_hit(q), arrive(q))
           # row_hit = bank_open[q.bank] and bank_row[q.bank] == q.row
           # ties inside a class are broken by arrival cycle, FIFO
    issue pick to its bank; that bank is busy until now + (row_hit ? t_hit : t_miss) + link
```

### 2.2 Answers to the specific questions

| Question | Answer |
| --- | --- |
| Row hits prioritised over conflicts? | **Yes**, strictly, among *eligible* requests (`memory.cpp:442`). A row hit always beats a non-hit, regardless of age. |
| How is age/FIFO used? | Only as the tie-break *inside* a class: `q.arrive < best.arrive` (`:443`). `DQ::seq` exists (`sim.hpp:538`) and is filled (`++seq_`, `:330`) but **is never read by the arbiter**. |
| Tie-breaking | Arrival cycle. Two requests pushed in the same cycle compare equal, so the first in queue order wins (`<` is strict). |
| Bank readiness considered? | **Yes** — `if (bank_ready_[q.bank] > cycle) continue;` (`:438`). An ineligible row hit is invisible to the decision. |
| Channel availability considered? | **No channels exist.** `banks = 2`, and there is no channel dimension in the config, the address map or the arbiter. |
| Multiple banks concurrently? | Yes for *state*: each bank has its own `bank_ready_`, `bank_row_`, `bank_open_`. No for *issue*: exactly **one command per cycle** total (`dram_tick` returns after one service). Two banks mean two independent busy timers, not two issue ports. |
| Arbitrary reordering within the queue? | **Yes.** Any entry may be chosen; it is removed with `dram_wait_.erase(begin() + best)` (`:448`). No per-bank queues, no window/age limit. |
| Fairness / starvation protection? | **None.** No aging, no round-robin, no starvation bound. A stream of row hits can defer a non-hit indefinitely. With `dram_q = 0` (unbounded) capacity does not bound it either. `starved_cycles`/`blocked_cycles` are counters, not protections. |
| Queue capacity: bounded or unbounded? | **Unbounded in the published configuration.** `dram_q = 0` disables the only gate, which is at **L2 MSHR allocation**, not in front of the controller: `memory.cpp:312–314` (`if (cfg_.dram_q && dram_wait_.size() >= cfg_.dram_q) { … dram.q_full++; return 2; }`). The `Config` default is 32 (`sim.hpp:60`); the config file sets 0. |
| What happens when nothing is eligible? | Two distinct cases, both `return` with no state change: queue empty → `starved_cycles++` (`:430`); queue non-empty but every bank busy → `blocked_cycles++` (`:445`). Measured, these two plus the service cycles partition the run exactly (W1 κ=0.25 C: 11 665 + 100 328 + 13 589 = 125 582 = cycles + 1) **[M]**. |

### 2.3 Simplifications that a textbook FR-FCFS description would get wrong

1. **Three row outcomes, not two.** `row_hits`, `row_conflicts` (bank open, different
   row) and *cold activations* (bank closed) — the last is counted in neither, so
   `reads - row_hits - row_conflicts` is the number of first-touches per bank. W1 κ=0.25 C
   has 13 589 − 11 383 − 2 204 = **2** such events **[M]**, i.e. after warm-up both banks
   always have a row open. Any "conflict rate" computed as `1 − hit_rate` is therefore
   *not* the activation rate; it is the activation rate plus the cold starts.
2. **One command per cycle, no bank-level parallelism in issue.** With `banks = 2` the
   model can sustain at most one command per cycle, so peak bandwidth is 64 B/cycle, not
   2 × 64 B/cycle.
3. **No refresh, no writeback, no read/write turnaround, no bank groups, no channels,
   no column dimension.** A "row" is consumed only at line granularity; there is no
   concept of a column access or of consecutive columns within an open row.
4. **Fixed latency per line.** `t_hit = 14`, `t_miss = 26`, `link = 0`
   (`configs/ab_controlled.cfg`). There is no tRCD/tRP/tCAS decomposition: an activation
   costs a flat 26 cycles and a hit a flat 14.
5. **The queue is a `std::vector` scanned linearly and erased in the middle.** Cost is
   O(n) per command with n ≤ 16 in practice (§6), which is why the model is fast; it is
   not a hardware queue model.
6. **A request's arrival time is the L2 MSHR allocation cycle** (`DQ::arrive = cycle` at
   `memory.cpp:330`), not the cycle it was formed at the client. Queue-wait measured from
   `arrive` excludes translation and L1/L2 MSHR time.

---

## 3. Physical address mapping (Part 3)

### 3.1 The formulas

Two steps. First VA → PA line (`memory.cpp:82–86`, `:157–177`):

```
pa_line = ((vpn + 0x4000) << 6) | (va_line & 63)      // 64 lines per 4 KiB page
        = va_line + 1 048 576                          // i.e. PA = VA + 64 MiB
```

Then PA line → bank/row (`memory.cpp:322–329`):

```
lines_per_row = max(1, row_bytes / 64)     = 1024 / 64 = 16
bidx = pa_line
if (bank_hash) bidx ^= (pa_line >> 6) ^ (pa_line >> 12)
bank = bidx % banks                        = bidx % 2
row  = ((pa_line / banks) / lines_per_row) % rows
     = ((pa_line / 2) / 16) % 4096
     = (pa_line / 32) % 4096
```

Units: `pa_line` is a **64-byte line number**, not a byte address (`LINE_BYTES = 64`,
`sim.hpp:27`). `row_bytes = 1024` → 16 lines per bank-row. There is no column field:
within a row the model only distinguishes "same row" from "different row".

### 3.2 What is and is not modelled

| Feature | Present? | Note |
| --- | --- | --- |
| Channels | **No** | `banks = 2`; no channel term anywhere in the map or the arbiter |
| Bank groups | **No** | |
| Columns | **No** | Line granularity only; no consecutive-column reuse to exploit |
| Row | Yes | 4 096 rows per bank index, modulo-wrapped (§3.4) |
| Bank hashing / XOR | Yes | `bidx ^= (line >> 6) ^ (line >> 12)` over the **line-number**, i.e. bits 6 and 12 of a byte address, not the classic `A[8:10]` style |
| Realistic interleaving | Partly | Bank = parity of a 3-term XOR, row = `line/32`; a linear stream alternates banks every line and changes row every 32 lines |

The hash folds *page* bits into the bank, which is the model's answer to
"`VX_CFG_L2_BANK_HASH = 1`". A stride of exactly 2 lines (128 B) still maps alternately to
bank 0 and bank 1, and one of 1 line alternates too, so the anti-aliasing claim holds only
for strides that the XOR actually breaks.

### 3.3 Concrete addresses (configured map)

`banks = 2`, `rows = 4096`, `row_bytes = 1024`, `bank_hash = 1`, `LINE_BYTES = 64`,
`PPN_OFFSET = 0x4000`, published bases `bulk_data_base = 0`, `bulk_desc_line = 98 304`,
`bulk_index_base = 0x00800000`.

| VA | pa_line | bank | row | Row-block covers |
| --- | --- | --- | --- | --- |
| `0x0` (data line 0) | 1 048 576 | 0 | 0 | lines 1 048 576–1 048 607 |
| `0x40` (data line 1) | 1 048 577 | 1 | 0 | same block, other bank |
| `0x800` (data line 32) | 1 048 608 | 0 | 1 | next block |
| `0x1000` (page 1) | 1 048 640 | 1 | 2 | pages advance row by 2 |
| `0x600000` (descriptor) | 1 146 880 | 0 | 3072 | |
| `0x800000` (index line 0) | 1 179 648 | **0** | **0** | **same (bank,row) as data line 0** |

Worked example, VA `0x1000`: `vpn = 1`, `ppn = 1 + 0x4000`, `pa_line = (0x4001 << 6) | 0 =
1 048 640`. `1 048 640>>6 = 16 385`, `>>12 = 256`; `1 048 640 ^ 16 385 ^ 256 = 1 065 473`,
odd → bank 1. `row = 1 048 640 / 32 = 32 770`, `32 770 − 8×4096 = 2` → row 2.

Consequences of the map, all [D]:

* A 4 KiB page spans 64 lines = **2 row numbers × 2 banks = 4 bank-rows**, each holding 16
  lines. "Rows per page = 2" (as stated in `dx100-architecture-gap-analysis.md`) is the row
  *index* count; per bank it is 2 rows of 16 lines.
* A linear sweep therefore alternates banks every line and keeps two rows open, scoring
  15/16 hits = **93.75 %** in steady state. Measured W0 A: **93.6 %** **[M]** — the map's
  own signature, and a useful sanity check on this section.
* **The map is periodic with period 8 MiB** in line space: 4 096 rows × 32 lines = 131 072
  lines = 8 MiB. The row index is `% 4096`, so PA `x` and PA `x + 8 MiB` share a
  (bank,row) coordinate.

### 3.4 Consequence: index and data share row coordinates

`bulk_index_base = 0x00800000` is **exactly one period** above `bulk_data_base = 0`, and
`PPN_OFFSET` shifts both by the same 64 MiB, so index line *i* and target line *i*
evaluate to the identical `(bank, row)` — confirmed arithmetically for line 0 in §3.3 and
true for all *i* because 131 072 ≡ 0 (mod 32) and the bank XOR sees identical low bits.

Effect **[D]**: when the controller has a row open for a data line and the model receives
the *aliased* index line, it counts a **row hit** where a real DRAM (different physical row)
would have paid an activation. The fan-in is bounded: index traffic is 774 of 13 589 DRAM
reads in W1 κ=0.25 C (5.7 %), so the row-hit rate is inflated by **at most 5.7 points**, and
only for index lines that arrive while the aliased data row happens to be open.

This is a fidelity limitation, not a functional bug: conservation, ordering and value
checks are unaffected, and it is invisible to the M5–M9 conclusions because those are
relative comparisons between arms that share the same map. It does, however, mean the
row-hit figures in §5 should be read as "in this map", not "in a real HBM device" — which
is also the reason the M11 candidate must not be justified by the absolute row-hit number.

---

## 4. Existing observability (Part 4)

Three distinct states:
**(a)** exported by `collect_mem_stats` (`src/system.cpp:407–425`) and/or printed;
**(b)** internal state that exists but is never exported;
**(c)** not reconstructable from anything the simulator records.

| Metric | Exists? | Source location | What it tells us | Limitation |
| --- | --- | --- | --- | --- |
| DRAM requests | **Yes, exported** | `dram.reads`, `memory.cpp:457`; `system.cpp:407` | Commands issued = L2 MSHR allocations that reached the controller | Cannot be split by requester (a, §4.1) |
| DRAM row hits | **Yes, exported** | `memory.cpp:451`; `system.cpp:408` | Commands whose bank already had that row open | No per-bank / per-row breakdown |
| DRAM row conflicts | **Yes, exported** | `memory.cpp:452`; `system.cpp:409` | Bank open, different row → activation | Excludes cold activations (bank closed), so it is not the activation count |
| Row misses (cold activations) | **No counter, but derivable** | `reads − hits − conflicts` | First touch per bank | Not printed anywhere; a derived quantity only |
| Bank utilization | **Yes, derivable + printed** | `dram.busy_cycles` (`:458`, `system.cpp:425`); printed as `DRAM bank busy (cycles)` `ab.cpp:1646` | Σ latency of issued commands | Divided by `banks × cycles` by the reader; no per-bank split, so it cannot show imbalance |
| Bank idle cycles | **Derivable** | `banks × cycles − busy` | Headroom | Same caveat; also conflates "idle" with "one bank busy, other idle" |
| Queue occupancy | **Yes** | peak `dram.max_q` (`memory.cpp:331`), integral `dram.q_occ_sum` (`:428`), both exported (`system.cpp:412`, `:422`); per-cycle `CycleSample::dram_q` (`bulk.cpp:1974`) | Mean and peak depth | **No distribution/histogram**; `dram-queue-sensitivity.md` §5.1 documents this gap |
| Queue-full refusals | **Yes** | `dram.q_full`, `memory.cpp:313` | Admission refusals **only when `dram_q > 0`** | Zero by construction in the published config; counts retry attempts, not distinct requests |
| Request wait time | **No** | `DQ::arrive` exists (`sim.hpp:538`, set at `memory.cpp:330`) but is only used as a tie-break | — | Would need `cycle − arrive` at service; nothing records it |
| Request service time | **Partly** | per-command latency is implicit in `t_hit`/`t_miss`; end-to-end `lat_mean/p50/p95` exist (`collect_lat_stats`) | Total request latency incl. translation + caches | No DRAM-only component, no per-request record |
| Per-bank distribution | **No** | `dram.bank_mask` (`memory.cpp:450`, `system.cpp:413`) records only *which banks ever served* | — | With 2 banks the mask saturates immediately; no counts |
| Per-row distribution | **No** | — | — | Would need a row histogram (4 096 × 2 rows) |
| Requester identity | **No** | `DQ` has `{line, seq, arrive, bank, row}` — no client tag; `Req` has `core`/`bypass_l1` but they stop at `submit` | — | Cannot attribute any DRAM metric to SIMT vs Bulk |
| Row-hit selection opportunities | **Partly** | `dram.elig_sum`, `elig_max`, `elig_last` (`memory.cpp:435–462`), `system.cpp:419–421`; printed as "eligible candidates" (`ab.cpp:2250`) | How many candidates a decision chose from | Counts candidates, **not how many of them were row hits** |
| FR-FCFS scheduling decisions | **Yes, implicitly** | `dram.svc_events` (`:459`) = decisions that issued | Pairs with `elig_sum` for mean candidates/decision | No record of *which* candidate was chosen or what was rejected |

### 4.1 The one structural gap that matters most

Every DRAM counter is **client-blind**. Since both clients share the queue (§1.1), the
published logs can say "13 589 DRAM reads" but not "…of which 774 were index lines and
12 376 were target lines". The split is only available *indirectly*, from the engine's own
`bulk_idx_*`/`bulk_tgt_*` counters, which count submitted/completed requests rather than
controller commands. Any M11 evaluation must not confuse the two.

### 4.2 Aggregate row-hit rate is not evidence of good scheduling

`row_hit_rate = row_hits / reads` (`ab.cpp:2272–2273`). Three reasons it cannot be read as
"the scheduler already did as well as it could":

1. It is a **property of the arrive order** more than of the arbiter: with ≤16 candidates
   (§6), the arbiter can only choose among what the issue order happens to have made
   visible. A high rate says the *upstream* order is row-friendly (M7's page-major issue),
   not that FR-FCFS extracted it.
2. It **counts alias hits** (§3.4) and **omits cold activations**, so it is neither the
   activation rate nor a real-device hit rate.
3. It has **no denominator for opportunity**. A run that scores 84 % while every eligible
   decision had at most one row-hit candidate is extracting 100 % of what was visible;
   a run that scores 84 % while averaging six visible candidates may be leaving hits
   behind. The current counters cannot tell these apart — that is what §7 adds.

---

## 5. What FR-FCFS already achieves (Part 5)

### 5.1 Source of the numbers

Archived M9 logs from the E3 implementation work
(`sim/build/wm/e3d_*.txt`, default policy = E3; `e0c_*.txt`, E0 control). Every cell:
`N = 16384`, `P = 512`, `rho_dup = 0`, `4 B/element`, `R = 128` slots, `seed = 1`,
`batch_window = 8192`, `batch_deadline = 4096`, unbounded DRAM queue, 2 banks, 14/26/0,
`bank_hash = 1`, `mem_path = l1`.
**W1** = `bulk_stream = 0`; **W0** here = `bulk_stream = 1` (the M9 constructed stream).
This W0 is *not* the `kappa = 1, P = 256` W0 used by the M6–M8 reports — the two must not
be mixed.

### 5.2 Per-workload, per-arm DRAM behaviour

Bank utilisation = `busy / (2 × cycles)`; bank-idle = `2 × cycles − busy`.

**W1, E3 default (M9 published default)**

| κ | arm | cycles | DRAM reads | hits | conflicts | rate | bank busy | util | bank-idle | starved | blocked |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.25 | A (SIMT) | 220 123 | 12 783 | 508 | 12 273 | 4.0 % | 326 262 | 74.1 % | 113 984 | 53 053 | 154 288 |
| 0.25 | B (bulk) | 138 924 | 13 771 | 11 611 | 2 158 | 84.3 % | 218 714 | 78.7 % | 59 134 | 20 634 | 104 520 |
| 0.25 | C (M10) | **125 581** | 13 589 | 11 383 | 2 204 | **83.8 %** | 216 718 | **86.3 %** | 34 444 | 11 665 | 100 328 |
| 0.50 | A | 169 826 | 9 397 | 1 204 | 8 191 | 12.8 % | 229 874 | 67.7 % | 109 778 | 50 686 | 109 744 |
| 0.50 | B | 127 138 | 10 386 | 8 298 | 2 086 | 79.9 % | 170 460 | 67.0 % | 83 816 | 16 304 | 100 449 |
| 0.50 | C | **94 035** | 9 876 | 8 667 | 1 207 | **87.8 %** | 152 772 | 81.2 % | 35 298 | 10 763 | 73 397 |
| 0.75 | A | 120 110 | 6 413 | 2 373 | 4 038 | 37.0 % | 138 262 | 57.6 % | 101 958 | 46 261 | 67 437 |
| 0.75 | B | 116 804 | 7 436 | 5 579 | 1 855 | 75.0 % | 126 388 | 54.1 % | 107 220 | 40 194 | 69 175 |
| 0.75 | C | **95 194** | 6 666 | 5 591 | 1 073 | **83.9 %** | 106 224 | 55.8 % | 84 164 | 32 566 | 55 963 |

**W0 (bulk_stream = 1), E3 default**

| κ | arm | cycles | reads | hits | conflicts | rate | busy | util | bank-idle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.25 | A | 11 052 | 1 019 | 954 | 63 | 93.6 % | 15 046 | 68.1 % | 7 058 |
| 0.25 | B | 17 267 | 2 046 | 1 896 | 148 | 92.7 % | 30 444 | 88.2 % | 4 090 |
| 0.25 | C | **18 142** | 1 792 | 1 600 | 190 | **89.3 %** | 27 392 | 75.5 % | 8 892 |
| 0.50 | C | **24 832** | 2 595 | 2 385 | 208 | 91.9 % | 38 850 | 78.2 % | 10 814 |
| 0.75 | C | **31 594** | 3 344 | 3 112 | 230 | 93.1 % | 49 600 | 78.5 % | 13 588 |

**Queue behaviour, [15] block, arm B / arm C** **[M]**: mean queue depth 9.8 / 10.7
(κ=0.25), 7.9 / 10.0 (κ=0.50), 6.3 / 6.8 (κ=0.75); **peak depth 16 in every cell**; mean
candidates per decision 6.6 / 6.6, 6.3 / 6.5, 6.5 / 6.5; peak candidates 16 / 16.
`dram_q_full` is **0 by construction** (`dram_q = 0`), so there is no refusal data to
report; refused-admission behaviour is only observable with a finite `dram_q`, and
`dram-queue-sensitivity.md` measured it separately (depth 8 binds; 16 = unbounded).

For completeness, the E0 control on the same cells (W1 κ=0.25, arm C): 153 475 cycles,
13 609 reads, 10 470 hits, 3 137 conflicts, **76.9 %**, starved 33 353, blocked 106 514
**[M]**. The E3 policy raises the row-hit rate 7 points and shortens the run 18 %, which is
direct evidence that row locality *at the controller* is a first-order lever in this model
— a fact that will be used to bound the remaining opportunity in §6.

### 5.3 The 81.6 % figure: source and exact scope

**Where it comes from.** It is `100 × dram_row_hits / dram_reads`, the quantity printed by
`ab.cpp:2272–2273` and recorded in `dram-queue-sensitivity.md` §3.4 and
`dx100-architecture-gap-analysis.md:61`.

**Reproduced in this tree [M]** — `sim/build/wm/m6_def.txt`, `experiment=m6`,
`batch_window = 8192`, W1 (`Λ = 16384`, `P = 512`, `κ = 0`, `N = 16384`, seed 1),
unbounded queue, arm B (bulk only):

| | reads | row hits | conflicts | rate |
| --- | --- | --- | --- | --- |
| A (SIMT only) | 16 417 | 277 | 16 138 | **1.69 %** (the "1.7 %" of the reports) |
| B (M6 bulk, E0) | 17 446 | 14 265 | 3 179 | **81.77 %** (the "81.6 %") |
| B (M6 bulk, forced E3) | 17 445 | 15 283 | 2 160 | 87.61 % |

**Exact scope of the 81.6 %.** Bulk-only arm; the *whole* W1 trace (not the M10-mixed
12 376-element subset); `batch_window = 8192`; unbounded queue; both banks; the E0 anchor
(81.6) — with E3 the same configuration reaches 87.6 %. It is **not** an M10 number (M10
arm C is 83.8 % on a different element subset), **not** per-bank, and **not** a
real-device figure (§3.4). Quoting it as "the scheduler is already at 81.6 % of optimal"
conflates a measurement with a ceiling.

### 5.4 Reproduction

```
cd DX100_MEM/sim
# arm C, W1, kappa 0.25, default (E3) policy
./bin/dx100sim --config configs/ab_controlled.cfg --set N=16384 --set Lambda=16384 \
  --set P=512 --set rho_dup=0 --set elem_bytes=4 --set bulk_wl=E0 --set seed=1 \
  --set batch_window=8192 --set bulk_max_elems=262144 --set max_cycles=200000000 \
  --set bulk_index_base=8388608 --set bulk_desc_line=98304 --set page_sched=1 \
  --set idx_mode=2 --set experiment=m9diag --set pool_slots=128 --set kappa=0.25
```

`[7]` prints the A/B/C DRAM block, `[15]` the B/C scheduling block. Both are in the
archived logs above; nothing was re-run for this audit.

---

## 6. Where the remaining opportunity is — and is not (Part 6)

### 6.1 The three categories, quantified

| | Category | Size in W1 κ=0.25 C | Evidence |
| --- | --- | --- | --- |
| **A** | Requests in the DRAM queue, visible to FR-FCFS | **mean 10.7, peak 16** entries (`[15]`) | `dram_q_occ_sum`, `max_q` |
| **B** | In the sealed target window but not yet formed/queued | window holds ≤ 8 192 elements / 12 376 lines; queue holds ≤ 16 lines → **>99.8 % of the window is invisible** at any instant | `[16]` window occupancy, `bulk_buf_occ`, `tgt_unissued` |
| **C** | Cannot be submitted: slot pool, translation, L1/L2 MSHR | R = 128 slots; mean 54.7 targets in flight, peak 120; `engine slot stall (free <= 8)` **40 836 cycles**; SIMT issue refused **8 094**; dTLB parked refusals **183 436**; page walks 539 | `[9]`, `[4]`, `[15]`, `[6]` |

**The queue never exceeds 16 entries, and the reason is structural, not a policy choice.**
With `mem_path = l1` every client request must hold one of the **16 per-core L1 MSHRs**
(`l1_mshr = 16`, `memory.cpp:365–374`) before it can allocate an L2 MSHR and be pushed into
`dram_wait_`. So `|dram_wait_| ≤ 16 + (PTE fetches in flight)`, and the measured peak is
exactly 16 in every cell of every archived run **[M]**. This is the same finding
`dram-queue-sensitivity.md` reached from the other direction (depths 16/32/64/unbounded are
bit-identical; depth 8 binds), and it is why the "broad visibility" premise of that
experiment could not be tested.

### 6.2 What that means for FR-FCFS's own capability

A bank-row is 1 KiB = **16 lines**. The queue's peak visibility is 16 lines *across both
banks*, i.e. **at most half a bank-row per bank**, and the mean (6–13) is less. FR-FCFS can
therefore never assemble a full row's worth of hits at once; it can only pick, among ~6
candidates, the one whose row is already open. Measured consequence: the arbiter's
candidate set is 6.3–8.9 entries, and the row-hit rate it achieves (83.8–87.8 %) is close
to what the *arrive order* already delivers — the arrive order is page-major, lines
ascending (`page_order = 1`), which walks a page's 2 row-blocks in order.

### 6.3 Is the page-major issue order hiding physically related requests?

Partly yes, by construction, and this is measurable rather than assumed:

* Page-major order groups requests by 4 KiB page. Within a page the 64 lines map to
  **2 rows × 2 banks** (§3.3), so the several hundred lines of a page visit 4 bank-rows.
* Across pages, consecutive pages are *disjoint* rows (row = `line/32`, pages advance the
  row by 2): page *p* → rows 2p/2p+1, page *p+1* → rows 2p+2/2p+3. Sorting by `(bank,row)`
  globally would therefore gain nothing *between* pages — every page is its own set of
  rows. The only grouping loss is **within** a page (returning to row 2p after row 2p+1
  was opened) and **across the two banks** (each page uses both).
* The floor is computable. W1's traces touch 512 distinct pages — arm C reports **526 L2 TLB
  misses / 526 walks** (`[6]`), i.e. 512 compulsory plus 14 conflict re-walks **[M]**. Each
  page occupies 2 row indices on each of 2 banks, so an ideal schedule pays **4 activations
  per page = 2 048** for the whole run. Measured arm C pays `reads − hits = 13 589 − 11 383
  = 2 206` activations = **4.31 per page**, against that floor of 4.0. The issue order is
  therefore within **7.7 %** of the activation minimum **[D]**.

**So the honest answer to "does page-major order prevent related requests from reaching
the queue together?" is: mostly no.** The order is already row-grouped at page granularity,
and the residual is the page-internal row alternation plus bank alternation, which an
upstream organizer cannot remove without a different page→row mapping (§3.4), not a
different issue order. **[D]**

### 6.4 How much is left? — a bounded estimate

Two conversions are possible, and both are capped:

1. **Conflict → hit.** Each conversion saves `t_miss − t_hit = 12` cycles of bank
   occupancy. W1 κ=0.25 C has 2 206 non-hit events (2 204 conflicts + 2 cold) against a
   floor of 2 048 (512 pages × 4 activations, §6.3) — a **surplus of 158**. Removing all
   158 saves 1 896 bank-cycles ≈ **948 cycles = 0.75 %** of 125 581. The floor itself is
   not reachable by reordering: it requires visiting each page's 4 bank-rows exactly once,
   and the surplus is what page-internal alternation costs.
   Caveat: the 2 048 floor assumes every touched page exercises both row indices on both
   banks, which is near-certain at ~24 accesses per page but is an assumption, not a
   counter. A per-`(bank,row)` counter (§7, I6) would replace it with a measurement.
2. **The transfer function is calibrated, not assumed [M].** Forcing E3 on the M6
   configuration moved row hits 14 265 → 15 283 (+5.9 pp), bank busy 282 416 → 270 174
   (−12 242) and cycles 233 528 → 227 293 (−6 235). Predicted from `Δbusy/2 = 6 121`;
   observed 6 235 — within 2 %. So on this model `Δcycles ≈ Δbank_busy / 2` while the
   run is DRAM-bound, and 1 pp of row-hit rate is worth ≈ **0.45 %** of cycles on W1.
3. **Utilisation ceiling [D].** W1 κ=0.25 C is already at **86.3 %** bank utilisation with
   100 328 blocked cycles (80 % of the run has work but both banks busy). Removing every
   conflict would save at most `2 206 × 12 / 2 = 13 236` cycles (10.5 %) and that bound is
   unreachable: it assumes zero activations. The realistic bound is **0.75 %** from (1).

So the remaining scheduler opportunity on the M10 headline cell is **under 1 %**, i.e.
1.008×, not the ~1.05× previously derived in
`dx100-architecture-gap-analysis.md:145` (that estimate assumed a 12-point row-hit gain
was reachable; the activation floor says only a 0.75-point gain is).

### 6.5 Which category is the limiting factor in M10?

**C, then A — not B.** In order of evidence:

* The engine is not short of *generated* work: `mean targets in flight 54.7`, peak 120,
  window occupancy peak 8 192. Category B is enormous but irrelevant, because what limits
  admission is upstream of the queue.
* The funnel is the shared L1: 16 MSHRs cap the whole machine's DRAM visibility at 16
  entries, which is *also* why the queue is never deeper than 16 and why FR-FCFS has
  ~6 candidates. `engine slot stall (free <= 8)` 40 836 cycles and 183 436 parked dTLB
  refusals show the pressure is at admission/translation, not at ordering.
* Therefore a bulk-side DRAM organizer — which by definition operates on category B and
  hopes to improve category A — would be feeding a controller that can see 16 lines, and
  whose arrive order is already page-grouped.

**[H]** The experiment that would change this conclusion is a deeper visibility window
(raise `l1_mshr` or `l2_mshr`, or place the queue downstream of the MSHRs), which
`dram-queue-sensitivity.md` §5.2 explicitly could not run. Only then does "broad
visibility" exist for an organizer to exploit. This audit does not authorise it.

---

## 7. Proposed instrumentation (design only — not implemented) (Part 7)

All proposals are **additive diagnostic fields**. Each is written in the same cycle as the
event it counts and is never read by `dram_tick`'s arbitration, by any front-end decision,
or by a gate. None changes the number of cycles, the order of commands, or any request's
fate. Storage is O(1) per counter (two banks → fixed-size arrays).

### 7.1 Counters

| # | Field | Location | Event that increments | Identity | Granularity | Storage | Timing risk |
| --- | --- | --- | --- | --- | --- | --- | --- |
| I1 | `hit_cand_sum` | `dram_tick` loop, `memory.cpp:436–444` | For each *eligible* candidate that is a row hit, `+1` | none | per-decision → aggregate | 1 × `u64` | none |
| I2 | `hit_busy_sum` | same loop, before the `continue` at `:438` | For each *ineligible* queue entry that would have been a row hit (`bank_ready_ > cycle && bank_open_ && bank_row_ == row`), `+1` | none | per-decision → aggregate | 1 × `u64` | none |
| I3 | `missed_hit_events` | `:449–453`, after the winner is chosen | `if (hit_cand > 0 && !open) ++` — under FR-FCFS this is **0 by construction**; a non-zero value would indicate a bug, and it is the control that proves I1 is not tautological | none | per-decision | 1 × `u64` | none |
| I4 | `wait_sum`, `wait_max`, `wait_hist[12]` | `:456`, beside `bank_ready_` assignment | `w = cycle − q.arrive`; accumulate, track max, bucket (reuse the `SlotAges` 2^b scheme, `sim.hpp:657+`) | per request | per-request → aggregate | 1 + 1 + 12 `u64` | none |
| I5 | `per_bank_reads[banks]`, `per_bank_hits[banks]`, `per_bank_busy[banks]` | `:450–458` | `++per_bank_reads[q.bank]`; `++per_bank_hits[q.bank]` when open; `per_bank_busy[q.bank] += lat` | bank | aggregate | 3 × `banks` `u64` (= 6) | none |
| I6 | `rows_touched` (bitset or sorted set) | `:453` | Insert `(bank,row)` on the first activation of that pair | (bank,row) | per-run set | 8 192 bits = 1 KiB (4096 × 2), or a `std::set` for exact counting | none (allocation only) |
| I7 | `client_reads[2]`, `client_hits[2]` | `:450–457`, fed by a new `u8 client` in `DQ` | `++client_reads[q.client]` | client | aggregate | 4 × `u64` | none |
| I8 | `q_occ_hist[65]` | `:428` (where `q_occ_sum` is already accumulated) | `++q_occ_hist[min(64, dram_wait_.size())]` | none | per-cycle → histogram | 65 × `u64` = 520 B | none |

### 7.2 Requester identity (the one change that touches a struct)

`DQ` (`sim.hpp:538`) gains `u8 client = 0`. It is filled at the single push site
(`memory.cpp:330`), which requires the client tag to be threaded through
`l1_probe`/`l2_probe`. Minimal form:

* add `u8 client` as the last parameter of `l2_probe` (`memory.cpp:298`) and `l1_probe`
  (`:342`), defaulted to 0 so `ptw_tick`'s PTE fetches (`:262`) and every other caller keep
  their current behaviour;
* at `memory.cpp:542–552` (stage 2) pass `r.bypass_l1 || r.core != 0 ? … ` — concretely
  `client = r.bypass_l1 ? 1 : 0` is *not* sufficient under `mem_path = l1`, because the
  flag is overridden (§1.3). The tag must be set at `submit` time into `Req` (e.g.
  `Req::client`, set by `BulkEngine` at `bulk.cpp:627/893/1177`), so the front-end that
  formed the request decides the tag and the path decision stays where it is.

This is the only proposal that changes a function signature. If a signature change is
unacceptable, the cheaper alternative is to derive the tag from the *line address*
(`line` inside the index array ⇒ index line; inside the descriptor line ⇒ descriptor;
otherwise target), which is available at the push site with no plumbing but misclassifies
aliased addresses (§3.4).

### 7.3 Bounded decision trace (optional, for the oracle in §8)

A ring buffer of the last `N = 4 096` decisions, one record each:

```
struct DramDec { u32 cycle; u64 line; u16 bank, row, cand, hit_cand, hit_busy; u8 open, client, pad; };
```

24 bytes × 4 096 = **96 KiB**, behind an existing diagnostic flag (the `CycleTrace*`/`observe`
hook at `sim.hpp:1153–1156` is the natural place; it is already null unless
`experiment=m9diag`). A full trace of every decision is also affordable for these runs —
13 589 decisions × 24 B ≈ 326 KB for W1 κ=0.25 — so the ring can be sized from config.

### 7.4 What each proposed counter answers

| Question from the brief | Counter |
| --- | --- |
| 1. Row-hit requests eligible at each decision | I1 (`hit_cand_sum / svc_events`), distribution via the trace |
| 2. How often FR-FCFS chose a hit when one was available | I3 (expected exactly `svc_events − (decisions with hit_cand = 0)`; any shortfall is a bug) |
| 3. Row hits missed because requests were not yet in the queue | **Not answerable with these counters alone** — it needs the *window* contents vs the queue; pair I1 with `tgt_unissued`/`bulk_buf_occ` (already exported) and the window's line list (§8) |
| 4. Requests delayed by bank timing rather than queue order | I2 + `blocked_cycles` (already exported) |
| 5. How evenly requests spread across banks | I5 |
| 6. How long requests waited in the queue | I4 |
| 7. How much of the window was invisible to the controller | `dram_q` (exists, per-cycle) divided by the window's line count (`bulk_lines_issued` per window, exists) |

---

## 8. Offline scheduling oracle (design only) (Part 8)

### 8.1 Feasibility: the honest answer is "not validly, from existing state"

An offline oracle needs a fixed list of `(arrival_cycle, line, requester)` and must replay
a different policy over it. **That list does not determine the run**, because arrival times
are endogenous to the schedule in this model:

* A request enters `dram_wait_` only when an L2 MSHR is allocated, which requires an L1
  MSHR (`l1_mshr = 16`) and is refused when the path is full (`memory.cpp:312–314`,
  `:342–374`).
* A DRAM completion frees the MSHR and admits a *new* request, possibly in the same cycle
  (`cache_tick` runs after `dram_tick`).
* The engine's issue rate, window sealing and page cursor all depend on completions
  (`bulk.cpp` `win_emit`/`win_tick`).

So a better policy changes *which* requests are in the queue and *when*: replaying over the
original arrival trace yields a **bound that is optimistic by construction** and cannot be
compared point-for-point with the measured run. Any oracle report must say so.

### 8.2 The oracle that can be built correctly

Given I1–I8 and the decision trace (§7.3), two comparisons are sound:

**(a) Same-arrival replay (upper bound, labelled optimistic).** Take the recorded trace,
replay it through the real bank-state machine (`bank_open_`, `bank_row_`, `bank_ready_`
exactly as `memory.cpp:449–458`), and compare three selectors over the *same* candidate set
at each decision:

1. `FR-FCFS` — must reproduce the recorded decision exactly (this is the trace's own
   consistency check; any divergence is a replay bug, not an opportunity).
2. `bank-aware` — among eligible, fewest pending requests for that bank (load balance).
3. `row-aware` — among eligible, prefer the row with the most queued requests (grouping),
   then row hit, then age.

Report `Δ bank_busy` and `Δ activation count` per selector. Because arrivals are frozen,
the result is an upper bound on what the policy could do to *this* arrival stream — not on
the run.

**(b) Closed-loop shadow oracle (sound, no trace needed, no scheduling change).** Inside
`dram_tick`, before committing to `best`, evaluate the three selectors on the *live*
candidate set and accumulate the *latency the alternative would have paid this cycle*
(`t_hit` vs `t_miss`) into counters. This never changes the chosen command, so timing is
untouched, and it answers "how many activations did this cycle's choice avoid or incur
compared with selector X" against the real, endogenous arrival stream. It is the only
oracle in this model that is both cheap and not optimistic — but it cannot accumulate a
*run-level* cycle delta, only per-decision differences.

### 8.3 What a valid oracle would additionally need

1. **Distinct `(bank,row)` count** (I6) to know the activation floor exactly — §6.4 infers
   it from the 512-page footprint; a counter would make it a measurement instead of an
   inference.
2. **Requester tag** (I7/§7.2) so the oracle can be restricted to the bulk stream; a
   policy that helps SIMT while hurting bulk is not what M11 proposes.
3. **The window's line list at the decision** (not just a count), to answer brief-question
   3 ("hits missed because requests were not yet queued"). This is the one quantity that
   is genuinely absent: the engine knows the sealed window, the controller knows the
   queue, and nothing joins them.
4. **A re-run, not a replay**, to turn an upper bound into a measurement — i.e. the policy
   has to be implemented and simulated, which is out of scope here.

### 8.4 What the oracle may not do (restated as constraints on the design)

* No future requests: candidate sets come only from `dram_wait_` at the decision.
* No ignoring bank timing: `bank_ready_` is honoured in every selector.
* No ignoring activation/precharge: the selector's cost model is exactly `t_hit`/`t_miss`.
* No ignoring dependencies: an L2 MSHR still owns its line; selectors reorder, they do not
  duplicate or drop.
* No unlimited capacity: the queue is whatever the model admitted; the oracle never adds an
  entry.
* No correctness change: the oracle never alters the issued command, so responses and
  hashes are unchanged.

---

## 9. Limitations and uncertainties (Part 9)

**Verified source facts.** Everything in §1–§4 marked **[S]** was read in the current tree:
the arbiter, the address map, the MSHR/QUEUE admission gate, the export list.

**Measured.** §5 is from archived logs in this tree. The 81.6 % figure is reproducible
(81.77 % in `m6_def.txt`) but the original log for the 17 445-read cell is not in the tree
(the archived reports are the only record).

**Derived, with the derivation shown.** The 8 MiB aliasing (§3.4), the 93.75 % linear-stream
hit rate (§3.3), the ≤16-entry visibility bound (§6.1), the 2 048-activation floor and the
0.75 %/10.5 % opportunity bracket, and the `Δcycles ≈ Δbank_busy/2` transfer function
(§6.4).

**Hypotheses that need new experiments.**

* **[H1]** The visibility bound is the *only* reason the queue is shallow; raising
  `l1_mshr` (or bypassing the L1 for the engine via `mem_path = native`, which the
  published config deliberately does not do) would deepen it and make an organizer
  meaningful.
* **[H2]** The index/data aliasing (§3.4) inflates the measured row-hit rate by a
  measurable amount. Testable by moving `bulk_index_base` off the 8 MiB boundary; not run.
* **[H3]** A bank-aware issue order could convert the ~158 surplus activations. The oracle
  of §8.2(b) can bound it without changing timing.

**Model scope that bounds every conclusion above.** One footprint (N = 16 384), one seed,
two banks, no channels, no bank groups, no columns, no refresh, one command per cycle,
`t_hit`/`t_miss` fixed at 14/26. A real HBM controller differs in all of these, so
"FR-FCFS here already captures the locality" does not transfer to a device with 16–32
banks and 64–256 queue entries — but neither does any presumed optimization headroom.

**A methodological caution.** W1's 83.8 % (arm C) and the M6/M7 81.6 % (arm B) are
different element subsets, different policies (E3 vs E0) and different read counts. They
should not be tabulated as a trend.

---

## 10. Recommendation

**Verdict: B — limited opportunity.**

FR-FCFS in this model already captures essentially all row locality that the issue order
makes available, and the remaining headroom is bounded by a structural property of the
memory path rather than by the arbiter's policy:

1. The controller's visible set is **6–13 candidates, peak 16**, because the shared L1
   (16 MSHRs) caps outstanding misses — not because of anything the scheduler does.
2. A bank-row is 16 lines, so the queue can hold **at most half a row per bank**; an
   organizer that reorders the window cannot put more rows in front of the arbiter than
   the queue can hold.
3. The arrive order is already page-major, and pages are **disjoint row sets** in this map
   (§3.3), so physically related requests are already adjacent: arm C pays 4.31 activations
   per page against a floor of 4.0, i.e. it is within **7.7 %** of the activation minimum.
   The residual is worth a **derived 0.75 %** on W1 κ=0.25 (10.5 % only if every activation
   vanished, which is not physically meaningful), with utilisation already at 86.3 %.
4. The measured transfer function (§6.4, E0 → E3 on the M6 configuration: +5.9 pp row hits
   → −2.7 % cycles) shows that row locality *does* pay in this model — which is why the
   opportunity is worth bounding rather than dismissing — but the M7 page-order work has
   already spent most of it.

**What would change the verdict** (in the order I would run it, each requiring the
instrumentation of §7 first and an authorisation that this brief does not give):

1. Instrument I1–I8, then read `hit_busy_sum` and the wait histogram. If most non-hits
   happen while a row hit was *ineligible*, the problem is bank timing, and no organizer
   fixes it.
2. Run the closed-loop shadow oracle (§8.2b). If a row-aware selector's per-decision
   advantage is under ~1 %, the ceiling is real and B stands.
3. Only if (1) and (2) show a gap: relax the visibility bound (H1) and re-measure. If the
   queue can hold a full row per bank, an organizer becomes meaningful — and only then is
   implementing one worth its complexity.

No code was changed. I will wait for review before proposing an implementation.
