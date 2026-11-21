# M7 — Translation-aware page scheduling: design, implementation, validation

**Status:** implemented and measured on the standalone DX100 simulator. No Vortex build, no RTL change,
no Linux dependency. Every number below is a raw cycle count from `bin/dx100sim`; every percentage is
recomputed from those counts and the base is named wherever a percentage appears.

**Headline.** The M6 issue policy spends 66.6 % of W1's run with its one permitted page unable to
admit a translation request. Scheduling across pages — with **no** change to TLB, MSHRs, walkers,
park depth, residency budget, caches or DRAM — takes W1 from **233 475 to 156 907 cycles (−76 568,
−32.80 %; 1.726× vs SIMT)**, and the best setting in the sweep reaches **154 673 (−78 802, −33.75 %;
1.750×)**. The DRAM delivers *the same service* (281 896 → 282 654 bank-cycles, **+0.27 %**) and its
utilization rises from 60.37 % to 90.07 %. Raising `dtlb_parked` from 2 to 32, the capacity ablation,
reaches 153 378 on the same engine: **scheduling alone recovers 95.6 % of what 16× more park capacity
buys at the default setting, and 98.4 % at 4 cohorts**, without buying the capacity.

---

## 1. What the bottleneck actually is

### 1.1 The issue path, traced

`BulkEngine::win_issue` (`src/bulk.cpp`) is the only place target lines are issued from a window.
Before M7 it had exactly one cursor, `Batch::issue_pos`, and this control flow:

1. `li = b.order[issue_pos]` — take the next line of the sealed issue order.
2. If `free_` (the R-slot pool) is empty → `slot_stall++` and **break**.
3. Take a slot, build a `Req`, call `mem_.submit(r, cycle)`.
4. If `submit` returns false → push the slot back, `issue_stall++`, `win_refuse++` and **break**.
5. Otherwise record the pending entry, `issue_pos++`, and continue up to `eng_issue` lines per cycle.

So a refusal (a) costs **no slot**, (b) costs **no memory-system state**, (c) **is retried next cycle**
at the same `issue_pos`, and (d) **ends the cycle**, so no later line of that window is tried. `win_refuse`
is a *cycle* count (one break per cycle); `issue_stall` is the same event counted once.

### 1.2 Where the refusal comes from

`MemorySystem::tlb_probe` (`src/memory.cpp:107`):

| condition | result |
|---|---|
| VPN in a dTLB way | hit, `ready = cycle + 1`, no MSHR involved |
| VPN matches an in-flight dTLB MSHR and `m.parked < dtlb_parked` | join, `m.parked++`, `ready = NONE` |
| VPN matches an in-flight MSHR and `m.parked == dtlb_parked` | **refuse_parked**, probe rolled back, `stall_cycles++` |
| no matching MSHR and a free MSHR exists | allocate, `parked = 1`, start an L2-TLB probe |
| all `dtlb_mshr` entries busy with other VPNs | **refuse_full**, same rollback |

`m.parked` is released when the fill lands (`pending_fill_` → `tlb_fill`, MSHR cleared). A parked
request is *already submitted*: it holds an R slot and an `info_` entry while it waits, so the park
limit is really "how many requests of one page may occupy residency while its translation is in flight".

### 1.3 The trap

`win_seal` builds `b.order` **page-major** (`page_order = 1`: ascending pages, ascending lines within a
page). The single cursor therefore sits in *one* page until that page's 32 lines are gone. When that
page hits `dtlb_parked`, the cursor refuses, and because it cannot advance, the port issues nothing at
all until the walk completes — a 300-cycle exposure for a 32-line page. The policy that produced the
row locality (page-major order) and the policy that bounds translation state (two waiters per page) are
each defensible; together they serialize the port.

### 1.4 The limits, kept apart

| limit | config | effect of raising it | measured on W1 |
|---|---|---|---|
| Translation admission (per-page waiters) | `dtlb_parked = 2` | lets one page keep more requests in residency | 2 → 32: 233 475 → **153 378** (−34.31 %) |
| dTLB MSHRs (pages with a translation in flight) | `dtlb_mshr = 4` | more pages translating at once | avg occupancy was **0.70 of 4** — the pool was idle |
| TLB entries | `dtlb_entries = 16` | fewer misses after the fact | 16 → 128: **0 cycles** (M6.2) |
| Walkers / L2 TLB MSHRs | `walkers = 2`, `l2tlb_mshr = 8` | more concurrent walks | 2 → 8: **0 cycles** (M6.2) |
| Bulk request residency | `R = 128` | more outstanding lines | peak was 128 but **avg 48.4** — not the limit |
| Cache/DRAM outstanding | `l1_mshr = 16`, `l2_mshr`, `dram_q = 32` | more buffering | `l1_mshr` 16 → 64: −1 689 (0.8 %), measured on the oracle row |
| DRAM service | 2 banks, `row_bytes = 1024` | — | 60.37 % busy before, i.e. **not** the limit |

The numbers say the binding limit is the *interaction* between per-page admission and page-major
ordering, and that `dtlb_mshr` — the obvious resource to grow — was only 17.5 % used. **The capacity
being wasted was already paid for.** That is what scheduling can recover without adding hardware.

---

## 2. Design

### 2.1 Chosen mechanism: page-run cohorts

The sealed order is partitioned into **page runs**: the maximal span of `order` that maps to one 4 KiB
page. A run is a claim on lines, nothing more. The engine keeps up to `page_cohorts` runs open and
issues from whichever run the dTLB will accept:

- **Detection.** A run that is refused is *deferred*: `resume_at = cycle + park_retry`. The port does
  not re-probe it this cycle and does not let it block anything.
- **Selection.** Among open runs that are not deferred, the **oldest** wins (lowest position in the
  sealed order). Preference for the earliest page means a ready page drains at the full `eng_issue`
  rate, and later pages only fill translation gaps.
- **Opening.** Lazy: a new run is claimed only when *no* open run can take the current slot. Pages are
  still claimed in ascending order, so the DRAM sees a narrow window of consecutive pages.
- **Resumption.** `resume_at` is always finite (`park_retry ≥ 1`), so every run is re-probed within a
  bounded number of cycles regardless of what the memory system does. Forward progress is structural,
  not probabilistic.
- **Bounded state.** `page_cohorts` descriptors of 32 B each (vpn, span/cursor, retry deadline, state),
  per window buffer. Nothing scales with the page count, the window size or the workload.

### 2.2 Why lazy + oldest-first and not eager round-robin

Eager round-robin (open all cohorts at seal, rotate one line per cohort per cycle) was implemented and
measured first. It reached 156 779 cycles — *worse* than the final design — and, more importantly, it
regressed the DRAM-bound workload:

| policy | W1 cycles | W1 row hits | W0 cycles | W0 row hits |
|---|---|---|---|---|
| legacy M6 | 233 475 | 14 304 | 39 322 | 4 753 |
| eager round-robin, 4 cohorts | 156 779 | 13 601 | 41 196 | 4 348 |
| **lazy, oldest-first, 4 cohorts** | **154 673** | **14 070** | 39 646 | 4 646 |

Eager spreading costs 703 row hits on W1 and 405 on W0 because four pages' rows interleave in the DRAM
queue permanently. Lazy + oldest-first keeps roughly one page flooding at a time — extra pages are
opened only when the head page refuses — so the locality loss collapses to 234 hits on W1 and 107 on W0,
and the run is faster as well. **Spreading is a gap-filler, not a policy.**

### 2.3 Alternatives considered and rejected

| alternative | why not |
|---|---|
| `dtlb_parked` 2 → 32 | the capacity ablation; forbidden by the M7 brief, and it costs 16× the per-page waiter state |
| `dtlb_mshr` 4 → 8/16 | measured to give 0 cycles (M6.2) — walkers, not MSHRs, limit concurrent walks |
| more walkers | measured 0 cycles (M6.2); the walk count is fixed by the trace, not the policy |
| reorder `b.order` (interleave pages) | destroys the page-major locality that *is* the M6 win (14 304 vs 4 753 row hits on W0's analogue) |
| round-robin over cohorts | measured above: worse cycles *and* worse locality |
| static partition of `order` into P chunks | same spreading cost as round-robin, plus P far-apart streams |
| adaptive cohort count from DRAM pressure | would fix the W0 regression (§7.4) but needs a threshold that is itself a tuned parameter; left as the next milestone |

---

## 3. Implementation

Small and local: 3 config keys, 4 stats blocks, ~110 lines in `bulk.cpp`, no change to any structure
that existed before.

### 3.1 Configuration

| key | default | meaning |
|---|---|---|
| `page_sched` | `0` | `0` = the M6 policy byte for byte; `1` = page-run cohorts |
| `page_cohorts` | `2` | page runs open at once, bounded at 16 |
| `park_retry` | `1` | cycles a refused run waits before re-probing (≥ 1, so it is finite) |

**The default is derived, not fitted:** `page_cohorts = dtlb_mshr / dtlb_parked = 4 / 2 = 2` — the
number of pages whose translations can be in flight at once, since each may park `dtlb_parked` requests.
§5.3 shows 2 is also the setting that behaves on the DRAM-bound and page-scattered workloads.

### 3.2 Code

`src/sim.hpp`
- `Config::page_sched`, `page_cohorts`, `park_retry`.
- `Batch::Cohort { u32 pos, end; u64 vpn, resume_at; bool open; }`, `Batch::coh` (empty unless enabled),
  `Batch::open_pos`, `Batch::last_coh`, `Batch::issued_n`.
- `BulkStats::sched_{opens,switches,defer,idle,probes,cycles,scan,bytes}`.
- `MemorySystem::max_parked` — largest waiting set ever reached on one MSHR (reported, never used for
  control).

`src/bulk.cpp`
- `win_emit` — the pre-existing submit-and-bookkeep block, extracted so both policies share one copy.
  Returns 0 issued, 1 no residency slot, 2 refused.
- `win_open_cohort` — claims the next page run, counting one charged schedule cycle and the line steps
  spent delimiting it.
- `win_issue_sched` — the loop described in §2.1. A refusal costs one *event* (`sched_defer`) and
  continues to the next candidate **in the same cycle**; only a cycle in which no run can be opened or
  used is counted as one lost cycle (`sched_idle`).
- `win_issue` — dispatches on `page_sched`; the legacy path is the original loop, unchanged except that
  it now calls `win_emit`.
- `win_seal` — allocates `page_cohorts` descriptors and charges their bytes; nothing on the legacy path.
- `win_tick` — window release now keys on `issued_n >= |order|` (lines actually submitted) instead of
  `issue_pos`, which the cohort path does not advance. For the legacy path the two are identical.

`src/memory.cpp` — `max_parked` bookkeeping only.
`src/util.cpp` — config keys and the `experiment` list.
`src/ab.cpp`, `src/system.cpp` — the scheduler counters in `DiagRow` and a scheduler table in the
diagnostic output.

### 3.3 What does **not** change

Materialized trace, target addresses, element ids, output and element ordering, fanout, completion
semantics, window admission and sealing, dedup, descriptor traffic (1 line), index traffic (1 024
lines), `R = 128`, `dtlb_parked = 2`, `dtlb_mshr = 4`, `dtlb_entries = 16`, `walkers = 2`,
`l2tlb_mshr = 8`, L1/L2 geometry and MSHRs, DRAM geometry and timing, the SIMT baseline. Verified in
§4.3 by counters, not by assertion.

### 3.4 Metadata cost

| item | size | share of window SRAM |
|---|---|---|
| 2 cohort descriptors (default) | 64 B | 0.043 % of 147 584 B |
| 4 cohort descriptors | 128 B | 0.087 % |
| charged schedule cycles (`sched_cycles`) | 1 536 | 0.98 % of 156 907 |
| page switches actually taken | 602 (c=2), 695 (c=4) | ≤ 0.44 % if each cost a full cycle |

`bulk_win_bytes` includes the descriptors, so the published window-SRAM figure moves by 0.04 % when the
scheduler is on. The descriptors are the only new state.

---

## 4. Validation

### 4.1 Correctness

| property | how | result |
|---|---|---|
| one cohort **is** the M6 policy | `page_sched=1, page_cohorts=1` vs `page_sched=0` | **233 475 = 233 475**, identical `dram_reads`, `lines_issued`, row hits and latency |
| element-by-element output | every element of the descriptor compared against the legacy run | 0 missing, 0 value mismatches, 0 address mismatches |
| target-address multiset | sorted digests of all 16 384 addresses, both policies | identical (`be7df7811576f303`), also identical to SIMT |
| no lost or duplicated request | `lines_issued` unchanged (16 384); cohorts partition `order`, so no line can be claimed twice | holds |
| residency invariant | `unissued + issued_not_filled ≤ R` | `residency_violations = 0`; peak slot residency **128 = R** |
| per-page park limit | `max_parked` reported by the dTLB | **2 in every arm**, i.e. never above `dtlb_parked` |
| forward progress under saturation | 2 048 elements on 1 024 pages, `park_retry = 64`, 6 cohorts | completes: 236 636 cycles, 1 916 runs opened, `elems_done = count`, no park left behind |
| response fanout | `elems_done = count`, `completion_hash` order-dependent and reported only | 16 384 / 16 384 every arm |

### 4.2 Regression

Full suite: **3 943 checks, 0 failures, 54 groups** (was 3 908 / 52). Two groups added:
`m7_page_sched` (identity, element equivalence, residency, metadata, adversarial liveness) and
`m7_baselines` (pins W1 233 475 / 156 907 / 154 673 and W0 39 322 / 39 530 so the two workloads move
together).

Published baselines reproduced unchanged: default A/B **270 753 / 278 034**; W0 legacy **30 573 /
44 773**; W0 windowed **30 573 / 39 322**; W0 diagnostic **39 322 / 32 931**; W1 diagnostic **233 475 /
225 223** (and 233 138 / 225 560 for the descriptor and index variants).

### 4.3 Capacity did not move (measured, not asserted)

| counter | legacy | c=2 | c=4 |
|---|---|---|---|
| peak slot residency | 128 | 128 | 128 |
| `max_parked` per page | 2 | 2 | 2 |
| config `dtlb_mshr`, `walkers`, `l2tlb_mshr` | 4, 2, 8 | 4, 2, 8 | 4, 2, 8 |
| average dTLB MSHR occupancy | 0.70 of 4 | 1.06 of 4 | 1.93 of 4 |
| average L1 MSHR occupancy | 10.33 of 16 | 14.43 of 16 | 14.77 of 16 |
| descriptor / index / target requests | 1 / 1 024 / 16 384 | 1 / 1 024 / 16 384 | 1 / 1 024 / 16 384 |

The scheduler raises dTLB MSHR occupancy from 0.70 to 1.06–1.93 **within the existing pool**; it does
not enlarge it.

---

## 5. Results

### 5.1 W1 (canonical: `kappa = 0`, `P = 512`, 16 384 elements, 4 B indices, W = 8192, R = 128)

SIMT reference A = 270 753.

| arm | cycles | × vs SIMT | × vs M6 | Δ vs M6 | % of M6 | DRAM reads | row hits | conflicts | bank busy | Δ service | avg R | mean/p50/p95 lat | dTLB miss | walks | lost issue |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A SIMT | 270 753 | 1.000 | — | — | — | 16 417 | 277 | 16 138 | 78.21 % | — | 95.0 | 1571/1665/1920 | 15 835 | 512 | — |
| B legacy M6 | 233 475 | 1.160 | 1.000 | — | 100 % | 17 444 | 14 304 | 3 138 | 60.37 % | 281 896 | 48.4 | 643/524/1259 | 2 256 | 529 | 155 501 cy (66.6 %) |
| cohorts 1 | **233 475** | 1.160 | 1.000 | 0 | 100 % | 17 444 | 14 304 | 3 138 | 60.37 % | 281 896 | 48.4 | 643/524/1259 | 2 256 | 529 | — |
| **cohorts 2 (default)** | **156 907** | **1.726** | **1.488** | **−76 568 (−32.80 %)** | 67.2 % | 17 445 | 14 243 | 3 200 | 90.07 % | 282 654 | 74.7 | 666/854/1205 | 2 276 | 533 | 75 401 cy (48.1 %) |
| cohorts 4 | **154 673** | **1.750** | **1.510** | **−78 802 (−33.75 %)** | 66.3 % | 17 445 | 14 070 | 3 373 | 92.04 % | 284 730 | 87.2 | 765/1035/1203 | 2 274 | 534 | 61 557 cy (39.8 %) |
| cohorts 8 | 155 356 | 1.742 | 1.503 | −78 119 (−33.46 %) | 66.6 % | 17 445 | 14 056 | 3 387 | 91.69 % | — | 87.2 | — | — | — | — |
| cohorts 16 | 155 566 | 1.740 | 1.501 | −77 909 (−33.37 %) | 66.6 % | 17 444 | 14 037 | 3 405 | 91.63 % | — | 87.7 | — | — | — | — |
| `dtlb_parked = 32`, legacy policy | 153 378 | 1.765 | 1.522 | −80 097 (−34.31 %) | 65.7 % | 17 445 | 14 243 | 3 200 | 92.14 % | — | 107.6 | — | — | — | — |
| `dtlb_parked = 32` + cohorts 4 | 153 522 | 1.763 | 1.522 | −79 953 (−34.24 %) | 65.8 % | 17 445 | 14 210 | 3 233 | 92.19 % | — | 112.4 | — | — | — | — |
| cohorts 2 + index oracle | 147 796 | 1.832 | 1.580 | −85 679 (−36.70 %) | 63.3 % | 16 419 | 14 328 | 2 089 | 86.25 % | — | 67.0 | — | — | — | — |
| **E: oracle + `parked = 32`** (upper bound, M6.2) | 144 804 | 1.870 | 1.613 | −88 671 (−38.04 %) | 62.0 % | 16 417 | 14 338 | 2 079 | — | 254 786 | — | — | — | — | — |

Percentage bases, stated explicitly because they have been confused before: **−38.04 % is 88 671 /
233 475**, i.e. the capacity ablation measured against the *measured* engine; **−35.70 % is 80 419 /
225 223**, the same ablation against the *oracle* row B3 of the M6.2 diagnostic. Both are correct; they
are not interchangeable. §9 (bottom line) of `m6-w1-diagnostic-report.md` now states both bases at the point of use.

Three things to read off this table:

1. **Same work, 32 % less time.** DRAM service +758 bank-cycles (+0.27 %), runtime −76 568 cycles.
   Bank utilization 60.37 % → 90.07 %. The engine did not get faster per request; it stopped wasting
   the memory system.
2. **The remaining translation stall is smaller and cheaper.** Lost issue falls 155 501 → 75 401
   cycles; what remains is *hidden* behind DRAM service, which is why killing 51.5 % of the blocking
   produces 32.8 % fewer cycles rather than 51.5 %.
3. **The tail improves while the mean rises.** p95 1259 → 1205 (c=2) / 1203 (c=4); mean 643 → 666 / 765.
   Occupancy roughly doubles, so the average request queues longer, but the worst case stops being
   "behind a whole page walk".

### 5.2 W0 (`kappa = 1`, `P = 256`) — the DRAM-bound control

SIMT reference A = 30 573.

| arm | cycles | × vs M6 | Δ vs M6 | DRAM reads | row hits | conflicts | bank busy | avg R | mean/p50/p95 |
|---|---|---|---|---|---|---|---|---|---|
| legacy M6 | 39 322 | 1.000 | — | 5 128 | 4 753 | 373 | 97.01 % | 103.5 | 775/868/981 |
| cohorts 1 | 39 322 | 1.000 | 0 | 5 128 | 4 753 | 373 | 97.01 % | 103.5 | 775/868/981 |
| **cohorts 2 (default)** | **39 530** | **0.995** | **+208 (+0.53 %)** | 5 128 | 4 677 | 449 | 97.65 % | 110.9 | 832/949/1002 |
| cohorts 4 | 39 646 | 0.992 | +324 (+0.82 %) | 5 128 | 4 646 | 480 | 97.84 % | 112.4 | 848/955/1006 |
| `dtlb_parked = 32` | 39 242 | 1.002 | −80 (−0.20 %) | 5 128 | 4 728 | 398 | 97.59 % | 113.1 | — |
| cohorts 2 + oracle | 33 652 | 1.168 | −5 670 (−14.42 %) | 4 103 | 3 777 | 324 | 91.16 % | 108.1 | — |

**W0 regresses by 0.53 % at the default.** The mechanism is fully accounted for: identical requests and
identical DRAM reads, 76 fewer row hits and 76 more conflicts, and the DRAM service floor computed from
those counts rises from 38 120 to 38 576 cycles (+456); the measured delta is +208 because at 97 %
occupancy part of it is absorbed. There is nothing to recover on W0 — it is already at 97 % of
bandwidth — so the scheduler pays the locality cost and gets none of the benefit. This is the M6.2
contrast reproduced by a different mechanism, which is the strongest evidence that the diagnosis in §1
is right.

### 5.3 Sweep across the other workloads (`page_cohorts`)

Same budget everywhere; only the number of open page runs changes.

| cohorts | W1 (`P=512`) | W0 (`P=256`) | W1 with 8 B indices | W1 with `P=2048` (2 048 pages) |
|---|---|---|---|---|
| 0 (legacy) | 233 475 | 39 322 | 266 867 | 1 160 292 |
| 1 | 233 475 | 39 322 | 266 867 | 1 160 292 |
| **2 (default)** | **156 907** (1.488×) | 39 530 (0.995×) | **192 480** (1.386×) | **807 155** (1.437×) |
| 4 | **154 673** (1.510×) | 39 646 (0.992×) | 192 490 (1.386×) | 856 039 (1.355×) |
| 8 | 155 356 (1.503×) | 39 742 (0.989×) | 192 665 (1.385×) | 853 326 (1.359×) |
| 16 | 155 566 (1.501×) | 39 760 (0.989×) | 191 542 (1.393×) | 853 612 (1.359×) |

W1 alone would pick 4 (1.5 % better than 2). The page-scattered workload picks 2 and punishes 4 by 6 %
— with 2 048 pages the extra open pages cost row conflicts faster than they buy translation overlap.
That is exactly the "do not tune on W1 alone" failure the brief warns about, and it is why the default
is the derived value 2.

### 5.4 `park_retry` sensitivity (W1, cohorts 2)

| retry | 1 | 2 | 4 | 16 | 64 |
|---|---|---|---|---|---|
| cycles | **156 907** | 157 041 | 157 412 | 157 069 | 160 902 |
| row hits | 14 243 | 14 242 | 14 241 | 14 240 | 14 239 |

Flat to within 0.3 % from 1 to 16 and only 2.5 % at 64, so the design does not depend on the retry
interval; 1 (re-probe every cycle) is kept because it is the simplest and the fastest.

### 5.5 Scheduler activity

| workload / setting | lost issue (cycles) | share of run | deferral events | runs opened | page switches | probes | metadata |
|---|---|---|---|---|---|---|---|
| W1, c=2 | 75 401 | 48.1 % | 151 839 | 1 536 | 602 | 240 928 | 64 B |
| W1, c=4 | 61 557 | 39.8 % | 249 144 | 1 536 | 695 | 349 993 | 128 B |
| W0, c=2 | 6 424 | 16.3 % | 12 339 | 65 | 126 | 42 164 | 64 B |
| W1 legacy (for comparison) | 155 501 | 66.6 % | 159 874 | — | — | 159 874 | 0 |

**Events are not cycles.** On W1 c=2 the port refuses **151 839** times but loses only **75 401**
cycles: a refusal now costs a switch to another page, not the cycle. On the legacy path the two
numbers are close (159 874 events vs 155 501 cycles) precisely because there was nothing to switch to.
Quoting refusal counts as lost cycles would be wrong in both directions.

---

## 6. Where the time goes now

| quantity | legacy | c=2 | c=4 |
|---|---|---|---|
| DRAM bank-busy cycles | 281 896 | 282 654 | 284 730 |
| bank utilization | 60.37 % | 90.07 % | 92.04 % |
| DRAM service floor (measured bank-busy ÷ 2 banks) | 140 948 | 141 327 | 142 365 |
| run ÷ floor | 1.656× | **1.110×** | **1.086×** |
| lost issue cycles | 155 501 | 75 401 | 61 557 |
| residency (avg / peak of 128) | 48.4 / 128 | 74.7 / 128 | 87.2 / 128 |

The scheduler removes 80 100 cycles of blocking (155 501 → 75 401) and converts 76 568 of them into
runtime (95.6 % conversion). The gap between the two — plus the 379-cycle rise in the service floor
from extra row conflicts — is what is left in queueing at 1.11× the floor.

---

## 7. Answers to the interpretation questions

**1. How much of the W1 translation bottleneck was removable by scheduling alone?**
80 100 of the 155 501 blocked cycles (51.5 %) and 76 568 of the 233 475 total cycles (**32.80 %**),
taking 1.160× to 1.488× against SIMT — 1.510× at 4 cohorts. The remaining 75 401 lost-issue cycles are
not recoverable by scheduling: they are the 512 walks themselves (533 measured) at two walkers, and they
are already hidden behind a 90 %-busy DRAM.

**2. How close does it get to the 144 804-cycle capacity ablation?**
86.4 % of the way in cycles — 76 568 of the 88 671 that separate 233 475 from 144 804 — and, against
the same ablation applied to the *same* engine (153 378), **95.6 % of the gain** (76 568 of 80 097;
98.4 % at 4 cohorts). The last ~3 % is the index stream's cost, not translation:
c=2 plus the index oracle is 147 796, i.e. 2.1 % above the ablation, with the descriptor still fetched.
Note the ablation's 144 804 also removes the index array, so comparing against it overstates the
translation component by ~7 900 cycles.

**3. Does it preserve M6's DRAM locality advantage?**
Almost entirely. Row hits 14 304 → 14 243 (−61, −0.43 %) at 2 cohorts and 14 070 (−1.6 %) at 4; the
advantage over SIMT (277 hits, 51.6×) becomes 51.4×. The lazy/oldest-first policy exists precisely to
keep this: the eager variant lost 703 hits and was slower. The service floor rises only 379 cycles
(+0.27 %) at the default.

**4. Does it improve or regress W0?**
**Regress, by 0.53 %** (39 322 → 39 530), curable by leaving `page_sched = 0` for known DRAM-bound
deployments or by adaptive cohort control (§8.3). The cost is 76 lost row hits, not scheduler overhead;
the schedule count is 126 switches and 1 536 charged cycles is the *W1* figure — W0 charges 65.

**5. What is the remaining bottleneck: latency, admission capacity, DRAM, or overhead?**
**DRAM service.** At c=2 the DRAM is 90.07 % busy, the run sits 1.110× above the floor computed from its
own row-hit/miss mix, and 48.1 % of cycles still lose issue but are absorbed. Admission capacity is no
longer binding (refusals are events, not cycles); scheduler overhead is 0.98 % of cycles; translation
*latency* still sets the pace of the 16.3 %-idle W0 case not at all and the W1 case only while the DRAM
cannot be fed.

**6. What new bottleneck appears once translation serialization is reduced?**
Three, in order: (a) **row-locality-limited bandwidth** — the floor is 141 327 and the run is 11 %
above it, with 3 200 conflicts against 373 on the streaming workload; (b) **the index stream**, worth
9 111 cycles (156 907 → 147 796 with the oracle); (c) **residency headroom** — 74.7 of 128 slots used
at c=2, so R is still not binding, but at 4 cohorts it is 87.2 and rising.

**7. Measured cost of the new metadata and logic.**
64 B per window buffer (0.043 % of window SRAM) at the default. 1 536 charged schedule cycles (0.98 %)
for 1 536 page runs installed, and **602 page switches for 16 384 issued lines** — 0.38 % of the run
even if every switch cost a full cycle. Probes are 240 928 (1.54 per cycle), which is a combinational
select in hardware, not a pipeline stage. The honest caveat: the simulator charges the installation
cycles as a *counter* and does not stall the issue pipeline for the page select, exactly as the M6
single-cursor path does not. If a real implementation needed a registered (1-cycle) page select on every
page move, it would cost ≤ 0.44 % — measured switches, not an estimate.

---

## 8. Limitations

1. **W0 regresses 0.53 %.** Measured, accounted for, not hidden. The scheduler is not free on a
   workload that has no translation stall to recover; the parameter exists so a deployment can turn it
   off, and this is the first thing the next milestone should automate.
2. **Best cohort count is workload-dependent** (W1: 4, page-scattered: 2, W0: 1). The default is
   derived from translation capacity rather than fitted, and is within 1.5 % of the best on every
   workload tested, but a single static default cannot be optimal for all of them.
3. **No adaptivity.** The scheduler does not measure whether extra cohorts are earning anything; it
   opens them when the port would otherwise idle, which is the right *local* rule, not a global
   optimum.
4. **One workload family per point.** W1, W0, 8-byte indices and `P = 2048` are four configurations of
   the same generator, not four independent traces.
5. **No RTL/SimX cross-check** (no RISC-V toolchain or Docker on this host), unchanged from M6.
6. **`win_refuse` is now legacy-only.** In scheduler mode the comparable counters are `sched_idle`
   (cycles) and `sched_defer` (events); mixing them would double-count.
7. **The mixed model `kappa = 0.5`** trips two diagnostic-driver invariants ("index array is not exactly
   N requests", "DRAM reads identity") on the **legacy** path too. It is a pre-existing limitation of
   the diagnostic's accounting for partially coalescible traces, not an M7 defect; those results are
   therefore excluded here rather than reported as a pass.

## 9. Recommended next milestone

1. **Adaptive cohort control** (M8): open a second run only while the *observed* lost-issue rate is
   falling, close it when the DRAM is saturated. The signal exists already (`sched_idle`, bank
   utilization per epoch); the goal is W0's 0.53 % back without giving up W1's 32.8 %.
2. **Row-aware cohort order**: the remaining 11 % above the floor is conflict-driven. Choosing the next
   page by DRAM row rather than by address order is the natural follow-on, and it must be measured
   against the 3 200-conflict baseline established here.
3. **Leave index acquisition alone** in this line of work: the oracle shows 9 111 cycles (5.8 %) are
   still there, but that belongs to the index-redesign milestone, as the M7 brief specifies.

---

## Addendum (2026-11-18) — scheduler installation cost audit

M8 inspected the cost model behind the 1,536 scheduler installation cycles reported
above and found that they were **counted but not charged**: `sched_cycles++` did not
occupy the issue port. M8 added a bounded model (one port cycle per run installation,
at most one outstanding). Nothing in this document has been changed; the figures above
remain the originals.

The omission was immaterial: the corrected figures are 156,912 for the W1 two-cohort
configuration (+5 cycles), 154,807 for four cohorts (+134) and 233,528 for the legacy
page policy with a window (+53). M8 results are reported against these corrected
values. See `m8-pipelined-index-acquisition-report.md` §9.
