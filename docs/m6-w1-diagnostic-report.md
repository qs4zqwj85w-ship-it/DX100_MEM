# M6 W1 diagnostic — what the index array costs, and what would stop the engine anyway

**Scope.** One question, measured against the M6 implementation as it stands:

> If index acquisition were free, how much faster could the bulk accelerator execute W1, and what
> would prevent it from going even faster?

**Answer.**

| | cycles | vs SIMT | vs measured engine |
|---|---|---|---|
| SIMT (A) | 270 753 | 1.000× | — |
| M6 measured (B0) | 233 475 | **1.160×** | — |
| M6, index oracle (B2) | 225 560 | 1.200× | −7 915 (−3.4 %) |
| M6, index + descriptor oracle (B3) | **225 223** | **1.202×** | −8 252 (−3.5 %) |

**Free index acquisition is worth 3.5 % on W1** — 8 252 cycles, of which 7 915 is the index array and
337 the descriptor. The accelerator is *not* index-bound on W1; the index array is 1 024 sequential
lines against a 16 384-line random target stream, and the target stream's own costs dominate.

**What prevents it from going faster is page-walk latency, exposed serially by the issue policy.** In
the oracle run (B3) the target-issue port spends **155 407 of 225 223 cycles (69.0 %)** holding a free
residency slot and a ready line that the dTLB refuses, because the page it is currently issuing has
only two waiters allowed on its in-flight translation. That is 1.2 % away from the arithmetic
identity **512 page walks × 300 cycles = 153 600**. With the port blocked, average residency is
**43.7 of 128 slots** and the DRAM is only **56.6 % busy** — the engine finishes 1.77× above its own
DRAM-service floor of 127 393 cycles.

Three ablations (all outside the controlled environment, §6) establish the causality, each leaving
W1's traffic and row behaviour *identical*:

| ablation | cycles | Δ vs B3 | target-port blocked cycles |
|---|---|---|---|
| B3 (baseline, oracle) | 225 223 | — | 155 407 |
| `dtlb_parked` 2 → 32 | **144 804** | **−80 419 (−35.7 %)** | 74 268 (−81 139) |
| `walkers` 2 → 8 | 225 223 | **0** | 155 407 (0) |
| `walk_latency_per_level` 150 → 50 | **134 049** | **−91 174 (−40.5 %)** | 46 746 |
| `dtlb_entries` 16 → 128 (8-way) | 225 223 | **0** | 155 407 (0) |
| `l1_mshr` 16 → 64 | 223 534 | −1 689 (−0.8 %) | 155 407 (0) |

The two knobs that matter change nothing about traffic (`dram_reads` 16 417, `dram_row_hits` 14 338,
`dram_row_conflicts` 2 077, `dram_busy_cycles` 254 786 in all three of the parked / walker /
walk-latency cases), and the two knobs a hardware budget would buy — more walkers, a bigger TLB —
change **nothing at all**.

The same `dtlb_parked` ablation applied to the *measured* engine takes W1 from 233 475 to **153 378**
cycles (−34.3 %, 1.77× vs SIMT) — larger than everything the index array costs by a factor of ten.

No architecture, ISA, budget or memory-hierarchy change was made; no optimization was implemented.

---

## 1. The experiment

### 1.1 Configuration

`configs/ab_controlled.cfg` with `batch_window = 8 192` — the M6 published W1 cell. Everything is the
controlled A/B environment: one core, `lanes = 4`, `R = 128`, `mem_path = l1`, dTLB 16×4-way with
4 MSHRs / 2 parked, L2 TLB 512×4-way with 8 MSHRs, 2 walkers, 16-entry walk cache, 300-cycle walk,
L1 256×4-way/16 MSHRs, L2 16 384×8-way/64 MSHRs, **2 DRAM banks**, `t_hit = 14`, `t_miss = 26`,
`frfcfs`, `bank_hash`.

W1: `kappa = 0`, `P = 512`, `Lambda = 16 384` → **16 384 distinct lines in 512 pages**, 16 384 elements
of 4 B, one element per line, target order a random permutation. The window holds 8 192 elements, so
two sealed windows cover the descriptor.

### 1.2 The oracle

`idx_oracle = 1` (`src/sim.hpp:103`) makes the engine read `index[i]` from
`BulkWorkload::index_values` — the same values that were written into the memory image — instead of
fetching the index array. It is an **idealized diagnostic, not a hardware proposal**: no machine can
name an indirect access's target without reading it.

What the oracle removes, exactly:

* `stream_index()` returns immediately (`src/bulk.cpp:481-488`): no index line requests, no index
  fills, no index residency, no index translations;
* `win_admit` / `generate` take the value from the side buffer instead of `idx_done_`
  (`src/bulk.cpp:546-598`, `348-413`): no index dependency, and `idx_wait_stall` becomes 0.

What it does **not** touch: target addresses, target requests, target latency, target translation,
deduplication, page order, window size, page-directory capacity, organization throughput, fanout,
residency limits, the shared TLB/caches/MSHRs/DRAM, or completion semantics.

The descriptor is measured **separately**, not folded in: `desc_fetch = 0` (B1, B3) removes the
one-line descriptor read, and B0 − B1 gives its cost with the index array still present.

### 1.3 Source references

| mechanism | location |
|---|---|
| oracle value lookup | `bulk.cpp:556-575` (`win_admit`), `bulk.cpp:361-385` (`generate`) |
| oracle suppresses the index stream | `bulk.cpp:481-488` |
| window admission (element order, `idx_done_` gating) | `bulk.cpp:546-598` |
| seal: sort by line, dedup, page order, page directory | `bulk.cpp:600-684` |
| issue: one request per `LineReq`, breaks on refusal | `bulk.cpp:685-735` |
| fill → fanout of every dependent element | `bulk.cpp:736-793` |
| `win_tick`: organizer, buffers, issue priority, seal trigger | `bulk.cpp:794-880` |
| dTLB probe/refusal (`refuse_parked`, `refuse_full`) | `memory.cpp:107-150` |
| L1 MSHR join, L2 MSHR join | `memory.cpp:353`, `memory.cpp:306` |
| DRAM service, row state | `memory.cpp:422-463` |
| SIMT coalescing (one request per instruction) | `system.cpp:178-188` |
| diagnostic driver | `ab.cpp:run_index_oracle_diag`, `print_index_oracle_diag` |

---

## 2. Correctness

Every claim is checked in `run_index_oracle_diag` (and locked by the `m6_index_oracle` and
`m6_w1_diagnostic` regression groups, which assert `r.ok`).

| requirement | how it is proven | W1 result |
|---|---|---|
| zero target-address mismatches | every element's address is re-derived from the index **image** at retirement and compared (`bulk.cpp:755-790`) | `addr_mismatch = 0` in B0, B1, B2, B3 |
| element-by-element output | FNV over `(elem, addr, value)` for all 16 384 elements, compared with a ground truth computed from the index values and the memory image alone | identical in every row (`hashOK = yes`) |
| identical target-address set | order-independent digest of all addresses, sorted then hashed | `be7df7811576f303` in all five rows |
| no missing or duplicated results | `elems_total = elems_done = 16 384`; `lines_issued + dedup_merged = win_entries`; the sorted multiset hash above | holds |
| identical ordering semantics | per-element address equality is order-sensitive (element *i* keeps its address); B's completion *order* is allowed to differ because the oracle changes when windows drain, so only the set and the per-element mapping are compared | `tgt_ooo = 0` |
| no extra outstanding capacity | `peak_slots ≤ R` per row, `bulk_residency_violations = 0`, `slots_total = 128` in every arm | peak 128 = R |
| no extra memory-system capacity | same `MemorySystem`, same `Config` for TLB/caches/DRAM; `check_capacity` on every arm | no violations |
| traffic accounting | `dram_reads = requests + PTE fetches − L2 hits − L2 joins` for every row | holds exactly |
| the oracle does not invent traffic | B2/B3 `index_req = 0`, `oracle_elems = 16 384`, and *requests* fall by exactly 1 024 (B2) / 1 025 (B3). DRAM reads fall by 1 025 / 1 027: 34 PTE fetches for the index and descriptor pages disappear, while 32 lines those fetches used to pull into the L2 no longer produce hits | holds |

The two oracle rows are therefore *the same computation*, executed with the index values arriving
by a different route.

---

## 3. Measurements

### 3.1 Cycles, traffic, residency

| row | cycles | vs SIMT | requests (desc/idx/tgt) | DRAM reads | row hits | row conflicts | bank busy | bank % | avg residency | peak |
|---|---|---|---|---|---|---|---|---|---|---|
| A: SIMT | 270 753 | 1.000× | 0 / 0 / 16 384 | 16 417 | 277 | 16 138 | 423 518 | **78.21** | 95.0 / 128 | 128 |
| B0 measured | **233 475** | **1.160×** | 1 / 1 024 / 16 384 | 17 444 | 14 304 | 3 138 | 281 896 | **60.37** | 48.4 / 128 | 128 |
| B1 no descriptor | 233 138 | 1.161× | 0 / 1 024 / 16 384 | 17 442 | 14 304 | 3 136 | 281 844 | 60.45 | 48.5 / 128 | 128 |
| B2 index oracle | 225 560 | 1.200× | 1 / 0 / 16 384 | 16 419 | 14 337 | 2 080 | 254 850 | 56.49 | 43.7 / 128 | 128 |
| B3 oracle + no desc | **225 223** | **1.202×** | 0 / 0 / 16 384 | 16 417 | 14 338 | 2 077 | 254 786 | 56.56 | 43.7 / 128 | 128 |

`bank busy` is `row_hits × 14 + (reads − row_hits) × 26`, which reproduces the simulator's
`dram_busy_cycles` to the cycle in every row (e.g. B3: 14 338×14 + 2 079×26 = 254 786).

### 3.2 Translation and the issue port

| row | dTLB reads | dTLB misses | refusals parked | refusals full | L2 TLB hits/misses | walks | walker busy | avg dTLB MSHR | avg L1 MSHR | target-port blocked | index-port blocked | slot stalls | blocked % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A: SIMT | 16 384 | **15 835** | 0 | **290 936** | 15 295 / 512 | 512 | 153 600 | 1.26 | 12.23 | 0 | 0 | 0 | 0.0 |
| B0 measured | 17 409 | 2 256 | **159 874** | 0 | 1 023 / 529 | 529 | 158 700 | 0.70 | 10.33 | **155 501** | 4 373 | 69 121 | 66.6 |
| B1 no descriptor | 17 408 | 2 255 | 159 874 | 0 | 1 023 / 528 | 528 | 158 400 | 0.70 | 10.34 | 155 501 | 4 373 | 69 121 | 66.7 |
| B2 index oracle | 16 385 | 1 594 | 155 407 | 0 | 512 / 513 | 513 | 153 900 | 0.70 | 9.75 | 155 407 | 0 | 64 586 | 68.9 |
| B3 oracle + no desc | 16 384 | 1 593 | 155 407 | 0 | 512 / 512 | 512 | 153 600 | 0.70 | 9.76 | **155 407** | 0 | 64 586 | 69.0 |

`target-port blocked` is `bulk_win_refuse`: cycles in which `win_issue` had a free residency slot, a
line at the head of the issue order, and `MemorySystem::submit` refused it. Because the loop breaks
on refusal, each event is one cycle — this column is a **cycle count**, not a retry count.

### 3.3 Latency, phases, and organization/fanout utilization

| row | mean target latency | p50 | p95 | startup (→ first tgt) | startup idle | issue span | span idle | drain | drain idle |
|---|---|---|---|---|---|---|---|---|---|
| A: SIMT | 1 571 | 1 665 | 1 920 | — | — | — | — | — | — |
| B0 measured | 643 | 524 | 1 259 | 4 432 | 1 156 | 227 847 | 183 858 | 1 196 | 40 |
| B1 no descriptor | 643 | 528 | 1 259 | 4 095 | 534 | 227 847 | 183 858 | 1 196 | 40 |
| B2 index oracle | 600 | 351 | 1 104 | 2 383 | 4 688 | 222 122 | 191 402 | 1 055 | 180 |
| B3 oracle + no desc | 600 | 351 | 1 104 | 2 046 | 4 092 | 222 122 | 191 388 | 1 055 | 180 |

`idle` is bank-idle cycle-equivalents (`2 × cycles − bank busy`), so each row's three phases sum to
its own total: A 58 994; B0 92 527; B3 97 830.

Organization and fanout load (W1, B0/B3 identical because neither depends on the index route):

| resource | value | share of the run |
|---|---|---|
| organizer (`org_lanes = 4`, 2 visits per entry) | 8 192 cycles | 3.6 % |
| fanout (`fanout_lanes = 4`, 1 element per line here) | 16 380 cycles | 7.3 % |
| target address generation (`ag_lanes = 8`) | 16 384 addrs ⇒ ≥2 048 cycles | 0.9 % |
| index stream completion (B0) | last line issued 9 678, filled 10 535 | 4.5 % |

Nothing in the organization/fanout/address-generation path is anywhere near the critical path on W1.

---

## 4. What the oracle changed, and what it did not

| question | answer | evidence |
|---|---|---|
| improves the startup period? | **yes, 2 049 cycles** — identical to W0 | first target issue 4 431 → 2 382; the startup is the organizer admitting 8 192 entries at 4/cycle (2 048) plus the descriptor (337); the index gate adds exactly the time to fetch the first 512 index lines, and that number depends only on the index layout and W, not on the workload |
| reduces translation pressure? | **yes, but not where it matters** | dTLB misses 2 256 → 1 593 (−663), walks 529 → 512 (−17), target-port blocking 155 501 → 155 407 (**−94, −0.06 %**) |
| increases effective request throughput? | **yes, 3.6 %** | 16 384 / 233 475 = 0.0702 → 16 384 / 225 223 = 0.0727 target lines/cycle |
| improves DRAM utilization? | **no — it lowers it** | 60.37 % → 56.56 %: less traffic with the same serialization leaves more bank time idle (185 054 → 195 660 idle bank-cycles) |
| exposes another bottleneck? | **yes, decisively** | target-port blocking is 69.0 % of the oracle run, residency is 43.7 of 128 slots, and the run sits 1.77× above its DRAM-service floor |

Where the 8 252 cycles actually come from — the exact accounting, no modelling:

| quantity | B0 | B3 | Δ |
|---|---|---|---|
| DRAM bank-busy cycles | 281 896 | 254 786 | **−27 110** |
| DRAM bank-idle cycles (of `2 × cycles`) | 185 054 | 195 660 | **+10 606** |
| cycles (`= (busy + idle) / 2`) | 233 475 | 225 223 | **−8 252** |

So removing the index array and the descriptor deletes 27 110 bank-cycles of DRAM service and
**wastes 10 606 of them** (39.1 %) because the engine cannot fill the freed bank time. Per removed
line the index stream was costing **26.4 bank-cycles** (27 110 / 1 027) — a full row activation each,
not the 14-cycle row hit its sequential layout would suggest, because it interleaves with W1's random
target stream (B0 − B3 row conflicts: **+1 061**, row hits: −34; 1 027 index/descriptor lines ⇒
essentially one conflict per index line).

On W0 the same accounting gives 15 770 bank-cycles removed → 12 782 cycle-equivalents saved (81 %
converted, 19 % wasted) at 14.9 bank-cycles per line. W1 converts only 61 % and pays 26.4 per line.

---

## 5. The bottleneck: 512 walks, exposed one page at a time

### 5.1 The identity

W1 touches 512 pages, each exactly once in the address order the trace produces. Each first touch
costs a dTLB miss whose fill is either a 300-cycle walk (512 of them) or a ~6-cycle L2 TLB hit
(1 081 of them). The engine's issue order is page-ascending (`win_seal` step 3, `bulk.cpp:650-657`),
and `win_issue` walks that order **in order**, breaking at the first line it cannot submit
(`bulk.cpp:685-735`). A page's 32 lines can have at most `dtlb_parked = 2` requests waiting on its
translation, so the third request is refused and the port stops until the fill lands:

```
512 pages × 300 cycles of walk latency = 153 600 cycles
measured target-port blocking          = 155 407 cycles   (+1.2 %)
measured walker-busy cycles            = 153 600 cycles   (identical)
```

That is the whole of the W1 bottleneck in one line. The residual 1.2 % is the ~1 081 L2-TLB-hit fills
(~6 cycles each) on pages re-touched in the second window.

### 5.2 Blocked cycles here *are* lost cycles

The milestone warns not to read refusal counts as lost cycles. On W1 the relationship is measured and
it is one-for-one:

| | cycles | target-port blocked cycles |
|---|---|---|
| B3 | 225 223 | 155 407 |
| B3 + `dtlb_parked = 32` | 144 804 | 74 268 |
| **difference** | **−80 419** | **−81 139** |

The port blocks because there is genuinely nothing else to issue: the next line in page order belongs
to the translating page, and the engine has no second page's queue to draw from. The freed cycles
convert almost exactly.

Contrast the same counters on W0, where refusals are *not* the critical path: there the index oracle
removed 4 617 cycles of *index-port* blocking but **added 2 518** to the target port (with no index
stream sharing issue, the port reaches a page's third request sooner and hits the two-parked limit
more often), a net −2 099 against 6 054 cycles of runtime — the other ~4 000 came from traffic. And
contrast `replays` (L1 MSHR retries, 7 303 162 on W1 B3): 32 attempts per cycle, worth **1 689
cycles** when the L1 MSHR is quadrupled. A large counter, a negligible cost.

### 5.3 Why SIMT pays 10× more dTLB misses and loses nothing

| | dTLB misses | refusals parked | refusals full | walks | cycles |
|---|---|---|---|---|---|
| SIMT | **15 835** | 0 | **290 936** | 512 | 270 753 |
| engine B0 | 2 256 | **159 874** | 0 | 529 | 233 475 |

SIMT misses the dTLB on essentially every request (4 lanes → up to 4 pages per instruction, 16-entry
TLB, 512 pages) and saturates its 4 MSHRs instead (`refuse_full = 290 936`, `refuse_parked = 0`).
None of that costs it a cycle, because a refused request stays in the core's held list and the core
keeps submitting **other** requests: its 128 slots are spread over up to 128 pages at once, so any
one page's translation overlaps with traffic to the other 127. Its average dTLB MSHR occupancy is
1.26 of 4 and its L1 MSHRs are 12.23 of 16 — it is busy, but never serialized.

The engine does the opposite: it asks for **one page at a time**, so a single translation has nothing
to overlap with, and it hits the per-page park limit rather than the MSHR pool
(`refuse_full = 0` exactly). Page-ordered issue is what buys W1 its 51.7× row-hit advantage
(277 → 14 304) and its 7× dTLB-miss advantage; it is also what exposes the walk.

### 5.4 The ablations (outside the controlled environment)

These change shared-TLB parameters or the walk model, so they are **attribution tools, not
configurations**; every headline number in this report uses the controlled environment.

| ablation | cycles | Δ | traffic | row hits | dTLB misses | reading |
|---|---|---|---|---|---|---|
| B3 baseline | 225 223 | — | 16 417 | 14 338 | 1 593 | — |
| `dtlb_parked` 2 → 32 | **144 804** | **−80 419** | unchanged | 14 243 | 8 852 | the park limit, not the walk count, is the throttle |
| `walk_latency_per_level` 150 → 50 | **134 049** | **−91 174** | unchanged | 14 338 | 1 593 | 40 % of the run is literally the 300-cycle walk |
| `walkers` 2 → 8 | 225 223 | **0** | unchanged | 14 338 | 1 593 | walker throughput is not the limit: the engine never has more than ~2 translations in flight |
| `dtlb_entries` 16 → 128, 8-way | 225 223 | **0** | unchanged | 14 338 | 1 593 | misses are compulsory and set-conflict-bound; capacity does not help |
| `l1_mshr` 16 → 64 | 223 534 | −1 689 | unchanged | 14 338 | 1 657 | the L1 MSHR is a minor secondary limit |
| `page_order = 0` (oracle) | 272 822 | **+47 599** | unchanged count, 423 590 bank-cycles | **271** | 15 830 | page ordering is worth far more than it costs; without it the engine collapses to SIMT's row behaviour |
| B0 measured + `dtlb_parked = 32` | **153 378** | −80 097 | 17 445 | 14 243 | 10 101 | the same 34 % is available to the *measured* engine, with the index array still in place |

The `walkers = 8` and `dtlb_entries = 128` rows are the most telling: two plausible hardware answers
produce **bit-identical runs**. Neither the rate of walks nor the size of the TLB is the problem; the
problem is that the engine can only have ~2 page translations outstanding and cannot issue anything
while it waits.

---

## 6. Why W0 and W1 respond so differently

| | **W0** (stream) | **W1** (permutation) |
|---|---|---|
| SIMT | 30 573 | 270 753 |
| engine B0 | 39 322 | **233 475** |
| outcome | 0.778× — a **loss** | 1.160× — a **win** |
| index oracle saving | 6 054 (15.4 % of the run) | 7 915 (3.4 %) |
| descriptor | 337 | 337 |
| B3 (both free) | 32 931 | 225 223 |
| B3 vs SIMT | +7.7 % (still a loss) | **1.202×** |
| bank utilization B0 → B3 | 97.0 % → 91.9 % | 60.4 % → 56.6 % |
| avg residency B0 → B3 | 103.5 → 100.0 of 128 | 48.4 → 43.7 of 128 |
| target-port blocked B0 → B3 | 40.6 % → **56.1 %** (rises) | 66.6 % → 69.0 % |
| dTLB misses / walks B0 → B3 | 161 → 128 / 81 → 64 | 2 256 → 1 593 / 529 → 512 |
| extra bank-cycles per index line | 14.9 | **26.4** |
| share of removed bank time recovered as cycles | 81 % | 61 % |
| dominant limit | **DRAM bandwidth** | **exposed page-walk latency** |

**Same mechanism, opposite regimes.** W0 runs 4 096 lines sequentially through 64 pages; the engine is
already at 97 % bank utilization, so the index array's extra traffic has nowhere to hide and converts
almost fully into cycles (+12 782 of 15 770 bank-cycles). W1 runs 16 384 lines randomly through 512
pages; the engine leaves 39–43 % of the bank time idle waiting for walks, so the index traffic slides
into the bubbles (+16 504 of 27 110 bank-cycles) and its removal is worth much less — 3.4 % of a run
that is 6× longer.

The two workloads also disagree on *which* port the index stream blocks. On W0 removing the index took the index port's own blocking to
zero (4 617 cycles) but pushed the target port **up** by 2 518 — and the run still got 6 054 cycles
faster, because on W0 blocked cycles hide behind DRAM service. On W1 the index stream cost 4 373
cycles on its own port and only 94 on the target port, because W1's 512 target pages are the
translation problem and 16 index pages barely disturb them.

And the ordering matters in opposite directions: on W0, page order changes nothing (a stream is
already in page order — measured exactly equal at every W in M6 §4.4); on W1, page order is the whole
1.160× (removing it costs +47 599 cycles and 14 067 row hits). W1's win and W1's bottleneck have the
same cause.

---

## 7. Ranked improvement opportunities (none implemented)

Estimated benefits are anchored on the measured ablations; each is labelled.

| # | change | estimated effect on W1 | cost / trade-off | class |
|---|---|---|---|---|
| 1 | **Interleave page groups at the issue port** — keep 2–4 pages' line queues open so a translating page never blocks the port, instead of strict page-ascending order with a single head | recovers most of the 80 419-cycle gap that `dtlb_parked = 32` demonstrates: W1 ≈ 145–165 k cycles (1.65–1.87× vs SIMT), *without* touching the shared TLB | needs per-page issue state inside the window (bookkeeping, not counted here) and slightly reduces intra-page row locality — measured at parked = 32: 14 338 → 14 243 row hits, −0.7 % | **[M]** effect size measured on the identical mechanism; **[A]** for the reordering form |
| 2 | **Raise the parked-waiter depth** `dtlb_parked` 2 → 8 | 225 223 → ≈145 000 (measured at 32) | it is the calibrated Vortex `REPLAY_DEPTH` and belongs to the shared TLB; deeper parking means 4× the waiters per MSHR, and it raises dTLB misses (1 593 → 8 852 at 32) by keeping more pages live | **[M]** |
| 3 | **Prefetch the next page's translation while the current page issues** | attacks the same 155 k; bounded by #1's number | needs the 4 dTLB MSHRs it is already starved of (avg occupancy 0.70 of 4, so the headroom exists) | **[H]** — no implementation measured |
| 4 | **Reduce the number of walks** (walk each page once per descriptor by grouping pages across windows) | W1 already walks 512 = the page count; the only saving is 17 walks for index/descriptor pages | none available on W1 without changing the workload | **[M]** |
| 5 | **Compress / pre-load the index array** | 7 915 cycles (3.4 %) — the whole index cost, and only if the values can arrive with less than 1 024 lines of traffic | format change; on W1 each index line costs a row activation (26.4 bank-cycles), so a halved index would save ≈4 000 | **[M]** for the total, **[A]** for the compressed form |
| 6 | **Wider L1 MSHR** | 1 689 cycles (0.75 %) | hardware budget; not worth it alone | **[M]** |
| 7 | **More walkers / a bigger dTLB** | **0** (measured, bit-identical) | none — do not pursue | **[M]** |

The ordering is unambiguous: the engine's W1 problem is *where the issue port waits*, worth an order
of magnitude more than everything the index array costs.

---

## 8. Measured, analytical, hypothetical

| claim | status |
|---|---|
| every cycle, request, TLB, cache, DRAM and occupancy number in §3–§6 | **measured** (`experiment=diag`, `experiment=ab`, and the `--all` ablations) |
| the DRAM busy/idle decomposition of the 8 252-cycle gap (27 110 / 10 606) | **measured** counters, exact arithmetic |
| 512 × 300 = 153 600 vs 155 407 blocked cycles | **measured** on both sides; the identity itself is an interpretation |
| Δcycles ≈ Δblocked under `dtlb_parked` (−80 419 vs −81 139) | **measured** |
| "26.4 bank-cycles per index line, i.e. one row activation each" | **measured** totals; the per-line attribution is a division of measured quantities |
| improvement #1's 145–165 k and #2's ≈145 k | **analytical**, anchored on the `dtlb_parked = 32` measurement |
| improvement #3 (translation prefetch) | **hypothesis**, not implemented or measured |
| cross-window dedup loss, alias behaviour | not measured (unchanged from the W0 report) |

### Reproducibility

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests        # 3 908 checks, 0 failures (52 groups)

# the four rows of this report
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=diag --set batch_window=8192

# attribution ablations (outside the controlled environment)
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=none --set mode=bulk \
    --set batch_window=8192 --set idx_oracle=1 --set desc_fetch=0 --set dtlb_parked=32 --all
#   ... --set walkers=8 | --set walk_latency_per_level=50 | --set page_order=0
#   ... --set dtlb_entries=128 --set dtlb_ways=8 | --set l1_mshr=64
# and the measured engine under the same ablation:
#   ... --set dtlb_parked=32          (no idx_oracle / desc_fetch overrides)

# published results, unchanged
./bin/dx100sim --config configs/ab_controlled.cfg                        # 270753 / 278034
./bin/dx100sim --config configs/ab_controlled.cfg --set wl=W0 --set kappa=1 --set P=256
                                                                         #  30573 /  44773
... --set batch_window=8192                                              #  30573 /  39322
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=diag \
    --set wl=W0 --set kappa=1 --set P=256 --set batch_window=8192        # W0 diag, 39322 / 32931
```

Regression status after this milestone's additions (per-cause `win_refuse` / `idx_refuse` counters,
`tlb0_mshr_occ`, the target-address multiset digest, two new test groups):

* `./bin/dx100_tests`: **3 908 checks, 0 failures**, 52 groups. `m6_window_advantage` still A = 270 753,
  B = 233 475, 1.160×; `m6_w0_diagnostic` unchanged; M4a/M4b determinism, `vortex_ref` (280 432 /
  16 417) and the A/B test config unchanged.
* Every published number reproduced: 270 753 / 278 034 (default A/B), 30 573 / 44 773 (W0 legacy),
  30 573 / 39 322 (W0 windowed), 39 322 / 32 931 (W0 diag).
* New groups: `m6_w1_diagnostic` pins this report's five cycle counts, the traffic delta, the blocking
  counts and the address-set hash; `m6_index_oracle` additionally asserts the multiset identity.

### Limitations

1. **One workload, one descriptor.** W1 has exactly one element per line and 512 pages; the walk
   exposure is a function of page count, lines per page and `dtlb_parked`.
2. **The oracle is unbuildable**; its 225 223 cycles are a bound, not an achievable design point.
3. **The ablations change the shared TLB or the walk model** and are not part of the controlled
   comparison; they identify causes, they do not propose configurations.
4. **Organization and fanout remain rate models** (`org_lanes = 4`, `fanout_lanes = 4`); on W1 both are
   far off the critical path (3.6 % and 7.3 %), so the conclusions do not depend on them.
5. **No multi-descriptor pipelining, write path or masks** — unchanged from M5/M6, so the engine's
   costs are still understated.
6. The `dtlb_entries` ablation is set-conflict-bound (`vpn % 16` sets), so it shows that *this* TLB
   shape does not help, not that no TLB could.

---

## 9. Bottom line

Free index acquisition would make the bulk accelerator **3.5 % faster on W1** (233 475 → 225 223,
1.160× → 1.202× versus SIMT). The index array is not what W1 is about: it is 1 024 sequential lines
against 16 384 random ones, and its cost is concentrated in two places that the engine can already
see — a 2 049-cycle startup gate and 1 027 row activations that interleave with the target stream.

What stops it from going further is the interaction of two policies that each look right on their own:
**page-ordered issue** (worth 14 000 row hits and the entire W1 win) and **two parked waiters per
in-flight translation** (the calibrated `REPLAY_DEPTH`). Together they mean the issue port spends 69 %
of the run waiting for the one page it is allowed to work on, which pins residency at 43.7 of 128
slots, which leaves the DRAM 43 % idle, which puts the run 1.77× above its own bandwidth floor. Every
measurement that isolates that interaction — deeper parking, shorter walks, a bigger TLB, more
walkers — moves the run by 0 %, 40 %, 0 % and 0 % respectively, and the one that moves it most
leaves the memory traffic *identical to the cycle*: −80 419 cycles (−35.7 %) against the oracle row
B3 = 225 223, or −88 671 (−38.0 %) against the measured engine B0 = 233 475. Both bases are quoted
because the ablation is usually paired with the measured row; the percentage is only meaningful with
the base attached.

On W0 the engine has the opposite problem: it is already at 97 % of the DRAM's bandwidth, so extra
traffic is the whole story. Its target port is blocked in 40.6 % of cycles and removing the index
*raises* that to 56.1 %, yet the run gets 6 054 cycles faster — every one of those blocked cycles was
hidden behind DRAM service that no longer exists. The two
reports together bracket the design space: a bulk indirect engine needs both a small index footprint
*and* an issue policy that never waits on a single page's translation. W1 has the first and lacks the
second; W0 has neither.
