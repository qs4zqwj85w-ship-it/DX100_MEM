# M6 diagnostic investigation — why the bulk engine loses on W0

**Scope.** This milestone answers one question and changes nothing architectural:

> Why does the M6 accelerator take **39 322** cycles on W0 when SIMT takes **30 573**, and how
> much of that 8 749-cycle gap is avoidable overhead rather than the inherent cost of using a
> bulk indirect-access engine?

Short answer, both halves measured end to end:

| component | cycles | share of the gap | how it was isolated |
|---|---|---|---|
| descriptor fetch | **337** | 3.9 % | B0 − B1 (`desc_fetch = 0`) |
| index-array acquisition | **6 054** | 69.2 % | B0 − B2 (`idx_oracle`) |
| engine organization with identical traffic | **2 358** | 27.0 % | B3 − A |
| **total** | **8 749** | 100 % | B0 − A |

and, independently, as DRAM service time versus DRAM utilization:

| component | bank-cycles | cycle-equivalents | share |
|---|---|---|---|
| extra DRAM service for 1 027 more lines | 15 770 | 7 885 | 90.1 % |
| extra DRAM bank-idle time | 1 728 | 864 | 9.9 % |
| **total** | 17 498 | **8 749** | 100 % |

Both decompositions close to the cycle, which is the main reason to trust them. The substantive
findings:

1. **W0 is a DRAM-bandwidth problem, not a latency or MSHR problem.** SIMT runs at **98.98 %** bank
   utilization (60 522 busy bank-cycles over 2 × 30 573 offered); the engine runs at **97.01 %**
   (76 292 over 2 × 39 322). Neither arm is short of concurrency, MSHRs, slots or row locality.
   The engine loses because it reads **25.0 % more lines** (4 101 → 5 128) on a machine that has no
   bandwidth left to sell.
2. **With identical traffic the engine costs only 2 358 cycles (7.7 %)** over SIMT, and **2 046 of
   those are one empty startup phase** — the first window cannot be issued until the organizer has
   admitted W elements. In steady state (issue span + drain) the same engine leaves the DRAM idle for
   1 248 bank-cycles = 624 cycle-equivalents, against 624 bank-cycles = 312 cycle-equivalents for
   SIMT's entire run: steady-state organization costs ≈310 cycles, the ramp costs the other 2 046.
3. **The "12 288 L1 MSHR joins in SIMT" premise is inverted.** SIMT performs **zero** L1 MSHR joins
   on W0. The 12 288 joins belong to the *un-deduplicated M5 FIFO engine*, which submits one request
   per element and lets the shared L1 merge them. The windowed engine shows 0 because it merges
   *before* submitting. Both paths reach the same L2/DRAM traffic; they differ only in who merges and
   what that costs upstream (§2).
4. **Every refused submission is a dTLB "parked" refusal, not an MSHR shortage.** 71 222 for SIMT and
   20 577 for the engine on W0, with **0** refusals from MSHR exhaustion. The 36 090 figure that the
   M6 report called "issue stalls" for the windowed arm is two different events: 20 577 dTLB refusals
   plus **15 513 slot-exhaustion stalls** (§3.4).
5. **Index acquisition and target execution do overlap**, but only after the first seal: the first
   target issues at cycle 4 431, the *last* index line is not even requested until 13 202, and the
   two streams then interleave for the remaining 25 000 cycles. What does not overlap is the ramp —
   the first window is gated on half the index array (§5).

Everything here is an investigation. No ISA, memory-hierarchy, budget or architecture change was
made, and no optimization was implemented.

---

## 1. What was added to measure this (and what was not touched)

The diagnostic machinery is additive and inert unless asked for. Defaults, published numbers and
the controlled A/B environment are unchanged — verified, not assumed (§8).

| addition | file:line | effect on a normal run |
|---|---|---|
| `Config::idx_oracle` (default `false`) | `src/sim.hpp:103`, `src/util.cpp` (`idx_oracle`) | none; only the `diag` driver sets it |
| `BulkStats` timeline, `slot_stall`, `oracle_elems` | `src/sim.hpp:412-450` | none; recorded unconditionally, no branch depends on it |
| `MemorySystem::refuse_parked` / `refuse_full` | `src/memory.cpp:129,148` | none; counted at the two `tlb_probe` refusal points |
| `AbArm` bank-busy, L2-join, refusal and timeline columns | `src/sim.hpp:650-700`, `src/ab.cpp:collect` | adds rows to printed tables only |
| `experiment = diag` driver | `src/ab.cpp:run_index_oracle_diag`, `print_index_oracle_diag` | new driver; `ab`, `m6`, `m6_sweep` untouched |
| `BulkWorkload::index_values` | `src/sim.hpp`, `src/bulk.cpp:build_bulk_workload` | one copy of the index values, used as the independent expectation |

Interfaces used, unchanged from M5/M6:

* `MemorySystem::submit(Req&, cycle)` — the only way a request enters the system; returns `false`
  only when `tlb_probe` refuses (`src/memory.cpp:465`).
* `MemorySystem::tick(cycle, done)` — advances the hierarchy and returns completed `(core,slot)`
  pairs; every front-end sees fills through this one door.
* `BulkEngine::win_admit / win_seal / win_issue / on_line / win_fanout / win_tick`
  (`src/bulk.cpp:546,600,685,736,755,794`) — the window mechanisms, unmodified.
* `run_index_oracle_diag` (`src/ab.cpp`) — one SIMT reference plus four engine variants of the same
  workload, same output, same hierarchy, same R.

### 1.1 The W0 workload under test

`--set wl=W0 --set kappa=1 --set P=256` on `configs/ab_controlled.cfg`: 16 384 elements, 4 B each,
4 lanes per instruction, **every instruction a contiguous 16 B run inside one 64 B line**, 4 096
instructions over 4 096 distinct lines in 64 pages. `batch_window = 8 192`, `R = 128`,
`mem_path = l1`, one core, one dTLB, one L1, one L2, **2 DRAM banks**, `t_hit = 14`.

That last detail is the whole story: two banks at 14 cycles per row hit serve at most one line per
7 cycles, so the 4 096 target lines alone have a floor of **28 672** cycles; including row conflicts
and PTE traffic the measured bank-busy floor is **30 261** (SIMT runs at 30 573, i.e. 1.0 % above
it). The engine's 5 128 lines floor at 35 896, measured 38 146.

---

## 2. Request merging: four mechanisms, not one

The milestone asks to keep four things apart. They are implemented in four different places and on
W0 they produce exactly these numbers:

| | **accelerator bulk dedup** | **L1 MSHR join** | **L2 MSHR join** | **DRAM transaction** |
|---|---|---|---|---|
| where | `win_seal` step 2 (`bulk.cpp:600-650`) | `l1_probe` (`memory.cpp:353`) | `l2_probe` (`memory.cpp:306`) | `dram_tick` (`memory.cpp:422`) |
| key | virtual line, within one window | `(core, physical line)` | `physical line` | one command per fill |
| SIMT arm (A) | — | **0** | 0 | 4 101 reads |
| M5 FIFO engine (W = 0) | none (1 request/element) | **12 288** | 0 | 5 128 reads |
| M6 windowed engine (W = 8 192) | **12 288 merged** | **0** | 0 | 5 128 reads |

### 2.1 How duplicates are represented and retired

`win_seal` sorts the window's entries by `va_line` (`b.sorted`), walks that order once, and emits one
`LineReq{first, count, seq, slot, issued}` per run of equal lines — that is the deduplication.
The elements themselves stay in `b.ent` as `WinEnt{va_line, elem, off, width}`. `win_issue` submits
**one** `Req` per `LineReq`; on completion `on_line` copies the 64 B into a `FanJob` and
`win_fanout` reconstructs each of the `count` dependent elements into `val_`/`addr_`. One fill,
N retired elements, and the accounting closes exactly:

```
lines_issued + dedup_merged == win_entries == elems_total == elems_done
            4 096 +       12 288 ==     16 384 ==     16 384 ==     16 384
```

Every element's address is independently re-derived from the index image at retirement
(`bulk.cpp:755-790`) and compared: `addr_mismatch = 0` on all runs. The diagnostic additionally
hashes `(elem, addr, value)` for all 16 384 elements and compares it against a ground truth computed
from the index values and the memory image alone: identical for A, B0, B1, B2 and B3 (§4).

### 2.2 Why SIMT shows zero joins and the un-deduplicated engine shows 12 288

The SIMT front-end coalesces **within an instruction** (`system.cpp:178-188`): the L lanes of one
instruction are reduced to their distinct lines before any request is formed. On W0 four contiguous
4 B lanes occupy 16 B of a single line, so each instruction forms exactly **one** request, and the
4 096 requests are 4 096 different lines. Two requests for one line are never in flight together, so
the L1 has nothing to join.

The M5 FIFO engine forms **one request per element** (`generate`, `bulk.cpp:348-413`) and relies on
the L1 to merge them. On W0 that is 16 384 requests for 4 096 lines and the L1 joins **12 288** of
them. The join is real and correct: `l1_mshr_allocs` stays at 5 121 and the L2/DRAM traffic is
*identical* to the windowed arm (5 283 L2 reads, 5 128 DRAM reads). It is also more expensive:
16 384 L1 probes and 12 288 slots held by requests that will never need their own fill, for
**44 773 cycles versus 39 322** — a 5 451-cycle penalty for merging one level too late.

So the 12 288 joins are not a SIMT behaviour and not a missing accelerator capability. They are the
signature of an *un-deduplicated* front-end. The M6 report's W0 section quoted "L1 MSHR joins
12 288 → 0" without naming which engine the 12 288 came from; this report corrects that.

### 2.3 Is the ordinary join path still available to the engine?

Yes, in both directions, and measured:

* Under `mem_path = l1` (the controlled environment) engine requests *do* probe the shared L1 —
  `path_bypasses_l1` returns false for mode 1 — and the legacy engine path joins **12 288** times.
  Two independent accelerator requests to the same outstanding line join normally.
* Under `mem_path = l2` the engine has no L1 at all (by design, `bypass_l1`); measured: L1 reads
  0 for both arms, L2 MSHR joins 0, DRAM 4 098 / 5 125, cycles 30 519 / 39 029. The join path exists
  (`memory.cpp:306`) but nothing duplicates at that level either.
* Deduplication removes *work*, not *capability*: no counter or path is disabled by `dedup = 1`.
  What the windowed engine gives up is the ability of a cache to merge a line that is re-requested in a
  **later** window. On W0 no line is ever revisited (each line is touched by exactly one instruction),
  so the question cannot even arise; the M6 sweep's W = 128 row on W1 shows the same (0 merges,
  0 joins, because W1 also has no line repeats). This is a structural property, not a measured cost,
  and it is worth measuring explicitly on a workload with a reuse distance near W — **not done here**
  (see §9, hypothesis H4).

### 2.4 DRAM transaction reduction

The engine's dedup does not reduce DRAM traffic relative to SIMT on W0; it only *recovers* what
SIMT's coalescer already had. 4 096 target lines either way. What the engine adds is its own
footprint: 1 024 index lines + 1 descriptor line + 34 extra PTE fetches (17 new pages × 2 levels),
of which 32 hit in the L2, for a net **+1 027 DRAM reads (+25.0 %)**.

---

## 3. Cycle attribution on W0

### 3.1 Where the cycles go (measured timeline)

Every phase boundary below is an observed event, and the bank-idle column is
`2 × phase_cycles − Δ(DRAM bank-busy cycles)` sampled at those events, so the phases sum exactly to
the run.

| row | cycles | startup (→ first tgt) | startup idle | issue span | span idle | drain | drain idle | bank busy % |
|---|---|---|---|---|---|---|---|---|
| A: SIMT | 30 573 | — | — | — | — | — | — | 98.98 |
| B0 windowed (measured) | 39 322 | 4 432 | 1 156 | 33 946 | 1 130 | 944 | 66 | 97.01 |
| B1 no descriptor | 38 985 | 4 095 | 534 | 33 946 | 1 130 | 944 | 66 | 97.78 |
| B2 index oracle | 33 268 | 2 383 | 4 688 | 29 940 | 1 194 | 945 | 68 | 91.06 |
| B3 oracle + no descriptor | 32 931 | 2 046 | 4 092 | 29 940 | 1 180 | 945 | 68 | 91.89 |

B0's 4 432-cycle startup is **not** wasted time: it keeps the DRAM 87.0 % busy (7 708 of 8 864
offered bank-cycles) prefetching the index array — traffic the SIMT arm does not have. B3's startup,
by contrast, is **2 046 cycles of a completely idle DRAM** (4 092 of 4 092 bank-cycles idle): with
nothing left to fetch, the only thing happening is the organizer admitting the first window.

### 3.2 Component-by-component, and whether it is on the critical path

| component | measured activity | on the critical path? | basis |
|---|---|---|---|
| descriptor fetch | 337 cycles (B0 − B1), single line, DRAM idle behind it | **yes, at startup only** | exact delta |
| index-array fetch | 1 024 lines; 6 054 cycles when removed | **yes — the dominant term** | exact delta (B0 − B2) |
| waiting for index responses | `idx_wait_stall` 3 733 cycles (9.5 % of the run) | it *is* the mechanism by which the above acts, not an extra cost | counter |
| target address generation | 16 384 addresses at 8/cycle; AG utilisation **5.2 %** | **no** | `bulk_ag_util_x1000 = 52` |
| page-directory organization | 8 192 cycles (2 visits/entry at 4/cycle) | **yes during the first fill, no afterwards** | see §3.3 |
| window fill / seal | 3 windows: full 1, deadline 1, end 1; 4 096 cycles of seal work | **yes during the first fill** | counters + timeline |
| admission / memory backpressure | 20 577 dTLB refusals, 15 513 slot stalls | **no** — the DRAM is 97 % busy anyway | §3.4 |
| target memory latency | mean 775, p50 868 cycles (A: 826 / 868) | **no** — throughput-bound, not latency-bound | counters |
| response handling / fanout | 4 096 busy cycles, 16 384 elements, peak backlog **4** | **no** | `fanout_max_backlog` |
| completion / retirement | drain 944 cycles (2.4 %) | tail only | timeline |

The engine is not a latency machine on W0 — it does not improve per-access latency at all (mean 775
vs 826, p50 identical). It is a *traffic* machine, and on W0 the traffic goes the wrong way.

### 3.3 The organizer is on the critical path exactly once

`win_admit` adds one visit per entry and `win_seal` adds one more (`st_.org_backlog += b.n_ent`),
drained at `org_lanes = 4` per cycle; 16 384 entries × 2 visits / 4 = **8 192 cycles**, which is what
`bulk_org_cycles` reports to the cycle. That is 20.8 % of the run, yet B2/B3 sit at 91 % bank
utilisation with 5 950 bank-idle cycle-equivalents — i.e. the organizer is *not* the steady-state
limit.

It is the limit exactly once: the first window. With the oracle (B3), admission is the only thing
between submission and the first target request, and the first target issues at **cycle 2 045**
against a predicted 8 192 entries / 4 per cycle = **2 048**. The empty startup is the organizer
admitting the first window. After that, filling window *k+1* overlaps issuing window *k*
(`org_overlap = 1`, two buffers, `win_total_max = 12 000` of 16 384 entries resident).

A second-order confirmation that the fill is the constraint: `seals_deadline = 1`. The second window
did not reach 8 192 entries within `batch_deadline = 4 096` cycles — i.e. the fill rate was below
2 entries/cycle, because index values were not arriving fast enough (§5).

### 3.4 The "refused submissions": two events, one name

`bulk_issue_stall` in the M6 report (36 090 on W0, 228 995 on W1) counted two different things:

| event | site | W0 windowed | W0 legacy | W1 windowed |
|---|---|---|---|---|
| `MemorySystem::submit` refused (`tlb_probe` false) | `win_issue`, `issue` | **20 577** | 24 135 | **159 874** |
| no free residency slot (`free_.empty()`) | `win_issue` (`bulk.cpp:693`) | **15 513** | — | **69 121** |

and the refusal itself has exactly two causes, both now counted (`memory.cpp:129,148`):

| cause | A (SIMT) | B (engine) |
|---|---|---|
| matching dTLB MSHR already has `dtlb_parked = 2` waiters | **71 222** | **20 577** |
| every dTLB MSHR occupied by another VPN | **0** | **0** |

Those totals split by port (per-cause counters added for the W1 diagnostic, see
`m6-w1-diagnostic-report.md` §3.2): on W0 the target-issue port accounts for **15 960** of the
windowed arm's 20 577 and the index stream for **4 617**. With the index removed the index port drops
to zero and the target port *rises* to **18 478** — a net −2 099 against 6 054 fewer cycles, which is
precisely why a refusal count is not a cost.

So on this machine **every** refused submission is the page-level park limit: a request whose page
translation is in flight, with two siblings already parked on it. No refusal is MSHR exhaustion.

The split also explains the number the M6 report published: on W1 at W = 8 192 it quoted
**228 995 "refused submissions"**, and 159 874 + 69 121 = **228 995** exactly — that figure was the
sum of dTLB refusals and slot-exhaustion stalls, not refusals alone.

A refusal is not a lost cycle. The request keeps its slot and is retried next cycle; the DRAM is
97 % busy while it happens. The evidence that it is not the binding constraint is the oracle run:
B3 removes 1 025 lines of traffic and 2 049 cycles of index dependency and its refusals only fall
from 20 577 to 18 478 — while its cycles fall by 6 391. Refusals track *page-level* behaviour, and
W0's page set (64 pages, 16-entry 4-way dTLB) is small enough that nearly every page is resident
(`tlb0_hits` 4 960 of 5 121 reads).

The legacy engine's 24 135 refusals plus 43 863 buffer-full stalls are a different story: with one
request per element it holds four slots per line and cannot keep 128 distinct misses in flight. That
is the mechanism behind its 84.6 % bank utilisation (§2.2).

---

## 4. The index oracle: what the index array actually costs

### 4.1 Definition and guarantees

`idx_oracle = 1` supplies `index[i]` from `BulkWorkload::index_values` — the same values that were
written into the memory image — instead of fetching the index array. It is an **idealized
diagnostic, not a realizable hardware configuration**: no machine can know an indirect access's
targets without reading them. It exists only to put a number on "acquiring the indices".

What the oracle does **not** change (each is checked in `run_index_oracle_diag` and in the regression
suite):

| invariant | how it is enforced | W0 result |
|---|---|---|
| same target addresses | every address re-derived from the index *image* at retirement | `addr_mismatch = 0` |
| same output values | `(elem, addr, value)` hash over all 16 384 elements vs ground truth from the image | identical in all rows |
| same target requests | 4 096 in every B row | 4 096 |
| same target latency | same `MemorySystem`, same path, same cache/DRAM state rules | mean 775 cy |
| no extra residency | `peak_slots ≤ R` asserted per row | 128 = R |
| same organization | same `win_seal`, dedup, page order, fanout, W, D | `org 8 192`, `fan 4 096` |
| same hierarchy | same L1/L2/TLB/walkers/DRAM | all rows |
| traffic accounting | `dram_reads = requests + PTE fetches − L2 hits − L2 joins` | holds for all five rows |

### 4.2 Results (W0: 16 384 elements, 4 096 lines, R = 128, W = 8 192)

| row | cycles | requests (desc/idx/tgt) | DRAM | row hits | row conf | bank % | avg slots | peak | L1 reads | L1 joins | L2 reads | dTLB miss | walks | tlb refuse |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A: SIMT | **30 573** | 0 / 0 / 4 096 | 4 101 | 3 842 | 257 | **98.98** | 110.6 | 128 | 4 096 | 0 | 4 224 | 128 | 64 | 71 222 |
| B0 measured | **39 322** | 1 / 1 024 / 4 096 | 5 128 | 4 753 | 373 | **97.01** | 103.5 | 128 | 5 121 | 0 | 5 283 | 161 | 81 | 20 577 |
| B1 no descriptor | 38 985 | 0 / 1 024 / 4 096 | 5 126 | 4 753 | 371 | 97.78 | 104.4 | 128 | 5 120 | 0 | 5 280 | 160 | 80 | 20 577 |
| B2 index oracle | **33 268** | 1 / 0 / 4 096 | 4 103 | 3 841 | 260 | 91.06 | 99.0 | 128 | 4 097 | 0 | 4 227 | 129 | 65 | 18 478 |
| B3 oracle + no desc | **32 931** | 0 / 0 / 4 096 | 4 101 | 3 842 | 257 | 91.89 | 100.0 | 128 | 4 096 | 0 | 4 224 | 128 | 64 | 18 478 |

Engine-side detail:

| row | idx wait stalls | tlb refusals | slot stalls | org cycles | fanout cycles | first tgt | last tgt | last index issued | last index done |
|---|---|---|---|---|---|---|---|---|---|
| B0 | 3 733 | 20 577 | 15 513 | 8 192 | 4 096 | 4 431 | 38 377 | 13 202 | 14 181 |
| B1 | 3 733 | 20 577 | 15 513 | 8 192 | 4 096 | 4 094 | 38 040 | 12 865 | 13 844 |
| B2 | **0** | 18 478 | 9 819 | 8 192 | 4 096 | 2 382 | 32 322 | — | — |
| B3 | **0** | 18 478 | 9 819 | 8 192 | 4 096 | 2 045 | 31 985 | — | — |

### 4.3 Reading the deltas

* **Descriptor: 337 cycles, 3.9 %.** Removing it moves the first target issue from 4 431 to 4 094 and
  the total from 39 322 to 38 985 — exactly 337, with the same 1 024 index requests, the same
  4 096 target requests and one fewer DRAM read. A single-line fetch on an idle DRAM is pure
  serial latency, and it is the first thing a real implementation would hide (issue the descriptor
  and the first index lines in the same cycle). It is small because it is charged once.
* **Index array: 6 054 cycles, 69.2 %.** This is the answer to the milestone's question. It is not
  one cost but two, and the experiment separates them:
  * *traffic*: 1 024 lines × ≈14.9 bank-cycles = **15 240 bank-cycles = 7 620 cycle-equivalents**
    of DRAM service the SIMT arm never pays;
  * *dependency*: the first target cannot issue until half the array has been admitted, which costs
    **2 049 cycles of startup** (4 431 → 2 382);
  * the net effect is **6 054, not 7 620**, because the index traffic occupies **1 799
    cycle-equivalents of bank time that would otherwise have been idle** (B2's 5 950 idle
    cycle-equivalents fall to B0's 2 352). That is the honest marginal cost: on a bandwidth-bound
    workload, extra traffic is partly free if the machine already has bubbles — and here it has 5 950
    of them.
* **Organization: 2 358 cycles, 27.0 %**, measured with *identical* traffic (4 101 DRAM reads,
  3 842 row hits, 257 conflicts, 128 dTLB misses, 64 walks — B3 equals A on every one). Of that,
  **2 046 is the empty startup** (§3.3) and ≈312 is the difference in steady-state bank-idle.

The B3 row is the cleanest single result in this report: **with the index array removed the engine's
traffic is byte-for-byte SIMT's, and it still costs 2 358 cycles — all but ~310 of it before the
first target request is issued.**

### 4.4 The same experiment on W1 (for contrast)

| row | cycles | requests | DRAM | row hits | dTLB miss | bank % | avg slots |
|---|---|---|---|---|---|---|---|
| A: SIMT | 270 753 | 16 384 | 16 417 | 277 | 15 835 | 78.21 | 95.0 |
| B0 measured | **233 475** | 17 409 | 17 444 | 14 304 | 2 256 | 60.37 | 48.4 |
| B2 index oracle | 225 560 | 16 385 | 16 419 | 14 337 | 1 594 | 56.49 | 43.7 |
| B3 oracle + no descriptor | 225 223 | 16 384 | 16 417 | 14 338 | 1 593 | 56.56 | 43.7 |

On W1 the index array costs 7 915 cycles (3.4 %) — the same traffic and the same 2 049-cycle
startup — but the engine *wins* anyway (1.160×, or 1.202× with a free index) because page grouping
removes 13 579 dTLB misses and 16 000 row conflicts. The startup cost is real on both workloads and
is simply larger than the benefit on W0.

---

## 5. Can index acquisition overlap target execution?

**After the first seal, yes — and the measurements show it working.** The implementation:

* `stream_index` (`bulk.cpp:481-528`) issues index lines in element order, capped at
  `min(idx_window, R/2) = 64` lines in flight in windowed mode — 1 024 elements of look-ahead.
* `win_admit` (`bulk.cpp:546-598`) consumes `idx_done_`, keyed by the first element of each index
  line. An index line that arrives out of order is held (`idx_lines_ooo = 296`) and consumed in
  element order; **it does not wait for the whole array**, only for the line that covers the next
  element. Consumption is incremental per line.
* `win_tick` calls `issue` → `win_issue` → `win_fanout` → `stream_index` → `win_admit`
  (`bulk.cpp:794-870`), so target issue has priority over index issue for the R slots, and index
  traffic only uses what is left.

The measured consequence is that the index stream is *deliberately* spread across a third of the
run: the **last index line is requested at cycle 13 202 and filled at 14 181**, while targets are
being issued from 4 431 to 38 377. From cycle ~14 200 the two streams have fully merged and the
engine is a plain DRAM-limited target stream.

**Before the first seal, no — and this is the exact source-level dependency:**

1. `win_seal` triggers on `b.n_ent >= cfg_.batch_window` (`bulk.cpp:851`). Admission stops at W
   entries, so **the first target request requires W elements to have been admitted**, which requires
   `W × index_bytes / 64` index lines to have *returned*: at W = 8 192 with 4 B indices, **512 of the
   1 024 lines — half the index array**.
2. `batch_deadline` (4 096 cycles) can seal a partial window, but only after a window has been
   filling that long; it fires once here (`seals_deadline = 1`), which is itself evidence that the
   fill was index-limited.
3. There is no mechanism to issue a *prefix* of a window: `b.order` is built at seal time from the
   whole `b.lines` set, and `win_issue` walks it in order. Until the window is sealed, `iss_` is −1
   and `win_issue` returns immediately.

Measured cost of that dependency: **2 049 cycles** of first-issue delay (4 431 → 2 382 with the
oracle) plus one deadline seal. Measured cost of the *traffic*: 6 054 − 2 049 ≈ 4 000 cycles.

`idx_wait_stall = 3 733` counts the cycles where admission found the next chunk missing. That is the
stall's *duration*, not its cost: much of it overlaps with target issue that is already running.

---

## 6. Bottleneck identification

In order of what actually limits cycles:

1. **DRAM bank service time (binding in both arms).** 2 banks × ≈14.8 cycles per line. A uses
   60 522 of 61 146 offered bank-cycles (98.98 %); B0 uses 76 292 of 78 644 (97.01 %). Neither arm
   has spare bandwidth. **Every cycle of the W0 gap that is not bank-idle is DRAM service time.**
2. **Index-array traffic (+1 024 lines, +20 % of B's reads) — 6 054 cycles.** Inherent to indirect
   access. Not removable without changing what the operation *is* (or compressing the index).
3. **The first-window fill — 2 046 cycles of 100 % idle DRAM.** The organizer admits at 4 entries
   per cycle and nothing issues until W entries are resident. This is the only mechanical
   inefficiency of the engine itself, and it is a *policy* choice (seal trigger), not a resource.
4. **Descriptor serialization — 337 cycles.** One line, on an otherwise idle machine.
5. Not bottlenecks on W0, despite appearing in the counters: dTLB refusals (1), slot exhaustion (2),
   fanout (peak backlog 4), address generation (5.2 % utilisation), page-directory capacity
   (32 pages resident of 512), L1/L2 MSHRs (never the limit: 16-entry L1 MSHR is enough for a
   2-bank DRAM), and organization bandwidth in steady state.

---

## 7. Ranked improvements (none implemented)

Each entry states the measurement it is anchored to and is labelled **[M]** measured,
**[A]** analytical estimate from a measured quantity, or **[H]** hypothesis.

| # | change | estimated benefit on W0 | cost / trade-off | class |
|---|---|---|---|---|
| 1 | **Seal on a page cohort**: issue a page's lines as soon as *k* of them are buffered (or seal the first window at a small W and grow it), instead of waiting for W elements | removes most of the 2 046-cycle empty startup; B0 39 322 → ≈37 300 (≈0.82× instead of 0.778×) | more batches → more seal visits (`org_lanes` traffic) and more per-batch fixed work; the M6 W = 2 048 dip shows small batches can thrash | **[A]** anchored on the measured 2 046 + B3 |
| 2 | **Widen the organizer** `org_lanes 4 → 8` | halves admission+seal time (8 192 → 4 096 cycles), cutting the startup ≈half: ≈1 000 cycles | 8 entries/cycle of organization bandwidth; more SRAM ports; still leaves the index traffic untouched | **[A]** |
| 3 | **Fetch the descriptor and the first index lines together** | ≈337 cycles (the exact B0 − B1 delta) | trivial; removes a serial 337-cycle head | **[M]** |
| 4 | **Raise the index look-ahead** (`idx_window > R/2`) | ≈0 on W0: the DRAM is the limit, so this only *reorders* index vs target traffic; it can shorten the first fill but steals slots from targets | targets lose slots; the M5 report already measured a ≈1 % loss for a bounded index window on W1 | **[A]**, consistent with M5 §6.3 |
| 5 | **Compress the index array** (4 B → 2 B offsets or deltas) | halves index traffic: 512 lines ≈ 3 800 cycle-equivalents of service, net ≈1 900 cycles once bubble-filling is accounted | index format change, range limits, extra decode; changes the descriptor contract | **[A]** |
| 6 | Keep the index array resident across descriptors (multi-descriptor pipelining) | on W0, saves the whole 6 054 if the array is already in L2 | out of scope (multi-descriptor is M7); needs an L2 large enough to hold it against the data stream | **[H]** |

The two that matter are 1 and 2: together they would take the engine's *own* overhead from 2 358
cycles to a few hundred, leaving W0 at ≈37 000 cycles, still ≈21 % behind SIMT. **The remaining loss
would be entirely the index array's traffic, and no organization change can remove it.** The
architecturally honest conclusion stands from M6: on a fully coalesced stream with a 2-bank DRAM, a
bulk indirect engine cannot win, because it reads 25 % more data on a machine with 1 % headroom.

---

## 8. Measured vs analytical vs hypothesis; reproducibility

| claim | status |
|---|---|
| all cycle counts, request counts, DRAM/TLB/cache counters in §2–§6 | **measured** (`experiment=diag`, `experiment=ab`) |
| the four-way decomposition closes to 8 749 exactly; the service/idle decomposition closes to 8 749 exactly | **measured** (arithmetic on measured counters) |
| 2 046-cycle startup = 8 192 entries / 4 per cycle | measured event, analytical attribution |
| 7 620 cycle-equivalents of index service time; 1 799 absorbed by idle | **analytical** from measured reads × measured t/line, cross-checked against the 6 054 measured delta |
| improvement benefits in §7 rows 1, 2, 4, 5 | **analytical estimates**, anchored on measured deltas — not implementations |
| §7 row 6, cross-window dedup loss (H4), alias behaviour | **hypothesis**, not measured |

Reproduction:

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests        # 3 887 checks, 0 failures

# the diagnostic (this report's tables)
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=diag \
    --set wl=W0 --set kappa=1 --set P=256 --set batch_window=8192
# same on W1
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=diag --set batch_window=8192

# published baselines, unchanged
./bin/dx100sim --config configs/ab_controlled.cfg                      # 270753 / 278034
./bin/dx100sim --config configs/ab_controlled.cfg --set wl=W0 --set kappa=1 --set P=256
                                                                       #  30573 /  44773
... --set batch_window=8192                                            #  30573 /  39322
```

Regression evidence that nothing published moved:

* `./bin/dx100_tests`: **3 887 checks, 0 failures**, 51 groups (49 previous + `m6_index_oracle`,
  `m6_w0_diagnostic`). `m6_window_advantage` still reports A = 270 753, B = 233 475, 1.160×.
* M4a/M4b determinism locks, `vortex_ref` (280 432 / 16 417) and the A/B test config unchanged.
* The legacy path (`batch_window = 0`) reproduces 278 034 on W1 and 44 773 on W0 exactly, and the
  default A/B prints 270 753 / 278 034.
* Two new tests pin this report's numbers: `m6_index_oracle` (A 4 089 / B0 6 347 / B1 6 010 /
  B2 5 490 / B3 5 153 on the small test config — same ordering, same traffic identities) and
  `m6_w0_diagnostic` (30 573 / 39 322 / 38 985 / 33 268 / 32 931, prefix 4 432, 4 101 DRAM reads in
  B3, 20 577 parked refusals).

### Limitations of this investigation

1. **One workload family.** W0 is a maximal-coalescing stream on a 2-bank DRAM; it is the *worst*
   case for a bulk engine and the *best* case for SIMT's coalescer. Nothing here generalises to
   other bank counts, cache sizes, or index widths without re-running.
2. **The oracle is not buildable.** It bounds the cost of index acquisition; it is not an
   implementation proposal and its 32 931 cycles are not an achievable number.
3. **`org_lanes = 4`, `fanout_lanes = 4` are rate models**, not synthesis results. Improvements 1–2
   are estimated against that model.
4. **No measurement of cross-window dedup loss** (needs a workload whose reuse distance brackets W).
5. **Single descriptor, no write path, no masks** — unchanged from M5/M6, so the engine's costs are
   still understated.
6. **Incidental finding, not acted on:** the trace generator's line set uses
   `stride = LINE_BYTES / lam` (`src/workload.cpp:34-40`), which becomes **0** when
   `lam = Lambda / P > 64` (e.g. `P = 4`), collapsing the line set. W0 (P = 256, λ = 64) and W1
   (P = 512, λ = 32) are unaffected. Recorded here so a future sweep does not silently use a
   degenerate workload.

---

## 9. Answer to the milestone question

> Why does the accelerator take 39 322 cycles on W0 when SIMT takes only 30 573?

Because on W0 the machine is already saturated and the engine asks it for 25 % more work:

* **7 885 cycles (90.1 %) are extra DRAM service time** for 1 027 extra line reads — 1 024 index
  lines, 1 descriptor line, 34 extra PTE fetches, minus 32 L2 hits. Those reads are the operation's
  input; SIMT does not pay them because its indices are already in registers. No amount of
  organization, deduplication, ordering or caching removes them.
* **864 cycles (9.9 %) are extra DRAM idle time**, and the oracle experiment shows essentially all of
  it is **one empty startup**: with the index array supplied for free and SIMT-identical traffic, the
  engine still needs 2 046 cycles to admit its first window before a single target request leaves.
  That part is avoidable — it is a seal-policy and organizer-bandwidth choice, not a resource limit —
  and it is worth ≈2 000 cycles, or 5 % of the run.
* The engine is not losing to MSHR pressure, TLB pressure, fanout, address generation, page-directory
  capacity or slot starvation. Each was measured and each is idle or nearly idle on this workload.

So the split is: **≈73 % inherent to indirect access (index + descriptor), ≈27 % avoidable
organization ramp, of which the ramp is ~87 %.** With the ramp removed the engine would still lose
W0 by ≈21 %, and it should: a workload whose SIMT coalescer is already optimal is the one case where
a bulk indirect engine has nothing to add and one unavoidable cost.
