# DRAM Queue-Depth Sensitivity

**Scope.** Measurement only. No simulator source was modified, no parameter swept that is not the
queue depth, no scheduling policy changed (`frfcfs = 1` throughout), and no published result
altered. `R = 128`, the cache/TLB hierarchy, `dram_q`'s companions (`t_hit = 14`, `t_miss = 26`,
`banks = 2`, `row_bytes = 1024`, `bank_hash = 1`, `link = 0`) and every engine parameter are exactly
as published. The published unbounded anchors reproduce to the cycle: W1 = 270 753 / 233 475 /
156 912 / 156 419, W0 = 30 573 / 39 322 / 39 530 / 38 775, PSCR = 1 908 638 / 1 160 292 / 807 159 /
857 517, STRID = 22 664 / 26 397 / 18 876 / 17 953, HOT = 4 361 / 10 580 / 10 580 / 10 867.

**Headline.** The measured M6/M7/M8 improvements are **robust** to finite DRAM queues in this model.
Depths of 16, 32, 64 and unbounded are **bit-identical in all 60 workload×arm cells** — not merely
close, every dumped counter equal — because the controller never sees more than **15–16** pending
requests even with an unbounded queue. Only depth 8 binds, and its worst effect anywhere is
**+2.56 %** (SIMT on W1) against **+1.87 %** for M8, so **every accelerator speedup is preserved or
slightly improved** under the tightest queue tested. The important caveat is that this experiment
therefore does **not** exercise the hypothesis in the brief: in this model the queue is never deep
enough for "broad visibility" to be the mechanism that favours bulk organization.

---

## 1. The queue model as implemented

`dram_q` is a **hard admission gate at L2 MSHR allocation**, not a queue between the L2 and the
controller (`src/memory.cpp:305–316`):

```cpp
if (cfg_.dram_q && dram_wait_.size() >= cfg_.dram_q) {
  l2.misses--; l2.reads--; dram.q_full++; l2.stall_cycles++; return 2;
}
```

Four consequences, all of which matter for reading the results:

1. **`dram_q = 0` means unbounded**, not zero entries: the gate is skipped entirely. The published
   controlled configuration (`configs/ab_controlled.cfg`, `dram_q = 0`) is therefore the unbounded
   case. (The struct default is 32 — `src/sim.hpp:60` — which the config overrides.)
2. **Refusal propagates to the requester.** `l2_probe` returns 2 → `l1_probe` returns 2
   (`src/memory.cpp:342`) → `submit` fails → the request bounces back to the front-end and retries:
   the SIMT front-end keeps it unissued, the engine's `win_emit` returns 2 and re-tries the line on a
   later cycle (`src/bulk.cpp`, `win_emit`). No MSHR is occupied while it waits. This is *stronger*
   backpressure than a real controller, whose L2 MSHRs would hold a request while the DRAM queue is
   full.
3. **`dram_q_full` counts refused admission attempts, not distinct requests.** A blocked request
   retries every cycle, so the counter can exceed the number of requests by orders of magnitude
   (3 787 504 refusals on W0 M7 for a run that issues 5 128 DRAM reads).
4. **Visibility is what FR-FCFS sees in `dram_wait_`** (`src/memory.cpp:424–441`): one command per
   cycle, open-row first, then oldest arrival. Starvation of a request is not bounded except by
   queue capacity, and `arrive` only breaks ties *within* a class.

The model has **no occupancy histogram and no occupancy integral** — only `dram_max_q` (peak) and
`dram_q_full` (`src/sim.hpp:263–266`, exported at `src/system.cpp:392–399`). See §5.1; this is
documented rather than fixed, because the brief restricts source changes to correctness defects and
none was found.

---

## 2. Design

* **Workloads (5):** W0 (`kappa=1, P=256`), W1 (`kappa=0, P=512`), PSCR (`P=2048`),
  STRID (`Lambda=1024, P=64, kappa=1`), HOT (`Lambda=2, P=1`). All `N = 16 384`, `seed = 1`,
  `batch_window = 8192`, `bulk_max_elems = 262 144`, `max_cycles = 200 000 000`.
* **Arms (4):** A = SIMT; M6 = `page_sched=0, idx_mode=0`; M7 = `page_sched=1, idx_mode=0`;
  M8 = `page_sched=1, idx_mode=2`.
* **Depths (5):** 8, 16, 32, 64, and 0 (unbounded).
* **100 runs**, every one `conservation=OK` (generated == issued == completed, every element retired,
  `lines issued + merged == elements`, `addr_mismatch == 0`).
* **Workload trustworthiness.** The engine's conservation checks pass in all 100 runs. HOT and STRID
  were the two workloads whose *diagnostic* identities fired before M8 audit §1; those identities
  were corrected there and all 21 adversarial diag runs now report OK, so both are used here without
  qualification. The extracted numbers are raw counters, not diag rows, so no arm is privileged.

---

## 3. Results

### 3.1 Total cycles

| W0 | depth 8 | 16 | 32 | 64 | unbounded |
|---|---:|---:|---:|---:|---:|
| A | 30 521 | 30 573 | 30 573 | 30 573 | 30 573 |
| M6 | 39 369 | 39 322 | 39 322 | 39 322 | 39 322 |
| M7 | 39 502 | 39 530 | 39 530 | 39 530 | 39 530 |
| M8 | **38 747** | **38 775** | **38 775** | **38 775** | **38 775** |

| W1 | depth 8 | 16 | 32 | 64 | unbounded |
|---|---:|---:|---:|---:|---:|
| A | 277 674 | 270 753 | 270 753 | 270 753 | 270 753 |
| M6 | 235 587 | 233 475 | 233 475 | 233 475 | 233 475 |
| M7 | 159 585 | 156 912 | 156 912 | 156 912 | 156 912 |
| M8 | **159 348** | **156 419** | **156 419** | **156 419** | **156 419** |

| PSCR | depth 8 | 16 | 32 | 64 | unbounded |
|---|---:|---:|---:|---:|---:|
| A | 1 908 638 | 1 908 638 | 1 908 638 | 1 908 638 | 1 908 638 |
| M6 | 1 160 292 | 1 160 292 | 1 160 292 | 1 160 292 | 1 160 292 |
| M7 | 808 610 | 807 159 | 807 159 | 807 159 | 807 159 |
| M8 | **861 581** | **857 517** | **857 517** | **857 517** | **857 517** |

| STRID | depth 8 | 16 | 32 | 64 | unbounded |
|---|---:|---:|---:|---:|---:|
| A | 22 622 | 22 664 | 22 664 | 22 664 | 22 664 |
| M6 | 26 397 | 26 397 | 26 397 | 26 397 | 26 397 |
| M7 | 19 179 | 18 876 | 18 876 | 18 876 | 18 876 |
| M8 | **18 167** | **17 953** | **17 953** | **17 953** | **17 953** |

| HOT | depth 8 | 16 | 32 | 64 | unbounded |
|---|---:|---:|---:|---:|---:|
| A | 4 361 | 4 361 | 4 361 | 4 361 | 4 361 |
| M6 | 10 580 | 10 580 | 10 580 | 10 580 | 10 580 |
| M7 | 10 580 | 10 580 | 10 580 | 10 580 | 10 580 |
| M8 | 10 867 | 10 867 | 10 867 | 10 867 | 10 867 |

**The columns for 16, 32, 64 and unbounded are the same numbers in every row of every table, and a
counter-by-counter diff confirms 0 differences in 60/60 cells.** Depth 8 is the only depth that
changes anything.

### 3.2 Speedup versus SIMT, and its change at depth 8

| Workload | Arm | 1.0 depth 8 | unbounded | Δ cycles | Δ speedup |
|---|---|---:|---:|---:|---:|
| W0 | M6 | 0.775 | 0.778 | +0.12 % | −0.29 % |
| W0 | M7 | 0.773 | 0.773 | −0.07 % | −0.10 % |
| W0 | M8 | 0.788 | 0.788 | −0.07 % | −0.10 % |
| W1 | M6 | 1.179 | 1.160 | +0.90 % | +1.64 % |
| W1 | M7 | **1.740** | **1.726** | +1.70 % | **+0.84 %** |
| W1 | M8 | **1.743** | **1.731** | +1.87 % | **+0.68 %** |
| PSCR | M6 | 1.645 | 1.645 | 0.00 % | 0.00 % |
| PSCR | M7 | 2.360 | 2.365 | +0.18 % | −0.18 % |
| PSCR | M8 | 2.215 | 2.226 | +0.47 % | −0.49 % |
| STRID | M6 | 0.857 | 0.859 | 0.00 % | −0.19 % |
| STRID | M7 | 1.180 | 1.201 | +1.60 % | −1.79 % |
| STRID | M8 | **1.245** | **1.262** | +1.19 % | −1.35 % |
| HOT | all | 0.412 / 0.412 / 0.401 | same | 0.00 % | 0.00 % |

Largest change in any speedup anywhere: **1.8 %** (STRID M7). The sign is not uniform — W1 improves,
STRID and PSCR erode slightly, W0 and HOT are flat.

### 3.3 Queue occupancy, refusals and starvation

Occupancy is reported as: peak (`dram_max_q`), the fraction of cycles with an **empty** queue
(`dram_starved_cycles`), the fraction **non-empty but with no dispatchable bank**
(`dram_blocked_cycles`), and refused admissions at depth 8. The three time fractions partition the
run (starved + blocked + dispatch ≈ cycles; the residual is the dispatch cycles).

| Workload | Arm | peak @∞ | refusals @8 | starved @∞ | starved @8 | blocked @∞ | blocked @8 |
|---|---|---:|---:|---:|---:|---:|---:|
| W0 | A | 15 | 3 010 722 | 299 (1.0 %) | 299 (1.0 %) | 26 174 | 26 125 |
| W0 | M6 | 15 | 3 509 652 | 1 267 (3.2 %) | 1 294 | 32 928 | 32 951 |
| W0 | M7 | 15 | 3 787 504 | 1 013 (2.6 %) | 1 013 | 33 390 | 33 365 |
| W0 | M8 | 15 | 3 455 631 | 374 (1.0 %) | 374 | 33 274 | 33 249 |
| W1 | A | 15 | 24 063 486 | 54 554 (20.2 %) | 54 554 | 199 783 | 206 704 |
| W1 | M6 | 16 | 9 390 217 | 35 773 (15.3 %) | 35 824 | 180 259 | 182 350 |
| W1 | M7 | 15 | 10 321 501 | 1 441 (0.9 %) | 2 794 | 138 027 | 139 379 |
| W1 | M8 | 16 | 9 398 945 | 680 (0.4 %) | 806 | 138 294 | 141 130 |
| PSCR | A | 7 | 0 | 1 750 664 (91.7 %) | same | 141 462 | same |
| PSCR | M6 | 15 | 299 515 | 914 437 (78.8 %) | 914 423 | 228 286 | 228 301 |
| PSCR | M7 | 15 | 267 757 | 612 451 (75.9 %) | 616 647 | 177 137 | 174 392 |
| PSCR | M8 | 15 | 273 139 | 663 272 (77.4 %) | 661 658 | 176 673 | 182 351 |
| STRID | A | 16 | 29 361 | 7 582 (33.5 %) | 7 568 | 14 054 | 14 029 |
| STRID | M6 | 16 | 711 090 | 5 807 (22.0 %) | 5 677 | 18 535 | 18 669 |
| STRID | M7 | 15 | 1 207 828 | 2 865 (15.2 %) | 2 725 | 13 956 | 14 403 |
| STRID | M8 | 15 | 845 016 | 1 904 (10.6 %) | 1 655 | 13 994 | 14 461 |
| HOT | A | 2 | 0 | 4 320 (99.1 %) | same | 38 | same |
| HOT | M6/M7 | 15 | 286 681 | 2 960 (28.0 %) | 2 986 | 6 590 | 6 565 |
| HOT | M8 | 15 | 272 256 | 3 225 (29.7 %) | 3 251 | 6 612 | 6 587 |

Three things to read off this table:

* **Peak occupancy is 2–16 in every cell**, i.e. the controller's visibility under an unbounded
  queue is at most 16 requests. A cap of 16 is therefore not a restriction at all.
* **Refusals at depth 8 are enormous (up to 3.8 M) yet cycles move by ≤ 2.6 %**, because each
  refusal is a one-cycle retry of a request that was not on the critical path. `dram_q_full` is a
  backpressure *event* count, not a measure of lost throughput.
* **The page-organized arms keep the controller busier than SIMT** where locality matters. On W1
the starved fraction is 20.2 % (A), 15.3 % (M6), **0.9 % (M7)** and **0.4 % (M8)**; on STRID it is
33.5 % (A) against 15.2 % (M7) and 10.6 % (M8). Same total DRAM traffic, far better sustained
supply — the queue-level signature of page-organized issue. On W0 every arm is under 3.3 % and the
difference is small.

### 3.4 DRAM traffic, row locality, bank utilization

Depth 8 versus unbounded (unchanged columns omitted):

| Workload | Arm | DRAM reads ∞ → 8 | row-hit % ∞ → 8 | conflicts ∞ → 8 | bank util ∞ → 8 |
|---|---|---|---|---|---|
| W0 | A | 4 101 → 4 098 | 93.7 → 93.8 | 257 → 254 | 99.0 → 99.0 |
| W0 | M8 | 5 128 → 5 125 | 91.8 → 91.8 | 420 → 420 | 99.1 → 99.1 |
| W1 | A | 16 417 → 16 417 | **1.7 → 1.2** | 16 138 → 16 220 | 78.2 → 76.4 |
| W1 | M6 | 17 444 → 17 414 | 82.0 → 81.9 | 3 138 → 3 147 | 60.4 → 59.8 |
| W1 | M7 | 17 445 → 17 413 | 81.6 → 81.7 | 3 201 → 3 190 | 90.1 → 88.4 |
| W1 | M8 | 17 446 → 17 413 | 81.6 → 81.6 | 3 212 → 3 195 | 90.4 → 88.5 |
| PSCR | A | 16 513 → 16 513 | 0.3 → 0.3 | 16 457 → 16 457 | 11.2 → 11.2 |
| PSCR | M7 | 17 572 → 17 572 | 47.8 → 47.7 | 9 166 → 9 188 | 22.1 → 22.0 |
| PSCR | M8 | 17 573 → 17 573 | 45.4 → 45.2 | 9 590 → 9 636 | 21.1 → 21.0 |
| STRID | A | 1 029 → 1 026 | 87.3 → 87.5 | 129 → 126 | 35.2 → 35.1 |
| STRID | M7 | 2 056 → 2 052 | 88.4 → 87.7 | 237 → 250 | 83.8 → 82.8 |
| STRID | M8 | 2 056 → 2 052 | 89.0 → 88.6 | 224 → 231 | 87.7 → 86.8 |
| HOT | all | 4 / 1 030 | 50.0 / 93.1 | 1 / 68 | 0.9 / 70-72 |

**Row locality survives the cap.** The one exception is instructive: W1 SIMT's row-hit rate falls
1.7 % → 1.2 %, i.e. SIMT loses what little open-row reuse it had — but the absolute numbers are so
small (**277 → 195** row hits out of 16 417 reads) that the real effect is on bank utilization
(78.2 → 76.4 %). For the
engine arms row-hit rate is flat within 0.7 points while bank utilization drops 1.7 points: a
shorter queue reduces the *overlap* between banks, not the locality of the stream.

W1 SIMT's 1.7 % row-hit rate against M7/M8's 81.6 % at identical DRAM read counts (16 417 vs 17 445)
is the whole page-organization story, and it is unchanged by the queue cap.

### 3.5 Latency, translation and scheduler blocking

| Workload | Arm | mean lat ∞ → 8 | p95 ∞ → 8 | walks | TLB MSHR stalls | schedIdle ∞ → 8 | slotStall ∞ → 8 |
|---|---|---|---|---|---|---|---|
| W1 | A | 1 571 → 1 627 | 1 920 → 1 949 | 512 | 290 936 | 0 | 0 |
| W1 | M6 | 643 → 661 | 1 259 → 1 236 | 529 → 530 | 159 874 | 0 | 69 121 → 70 924 |
| W1 | M7 | 669 → 704 | 1 206 → 1 220 | 533 | 155 582 → 154 726 | 74 815 → 74 451 | 71 769 → 74 780 |
| W1 | M8 | 623 → 653 | 1 120 → 1 141 | 544 | 161 532 → 161 922 | 77 894 → 78 230 | 68 629 → 71 106 |
| PSCR | M7 | 307 → 314 | 639 → 643 | 5 326 → 5 335 | 1 598 105 → 1 600 744 | 794 871 → 796 366 | 0 |
| STRID | M8 | 252 → 276 | 878 → 973 | 81 | 23 236 → 23 820 | 9 437 → 9 751 | 1 710 → 1 462 |
| HOT | M8 | 354 → 353 | 485 → 485 | 18 | 4 561 | 0 | 0 |

Mean target latency rises **5–9 %** under the cap (W1: M8 +4.8 %, M7 +5.2 %, SIMT +3.6 %; STRID M8
+9.5 %), which is the direct cost of a shorter queue — but total cycles move by far less because the
extra latency is absorbed by the residency budget rather than serialized. Translation is essentially
untouched (walks ±1, TLB stalls within 1 %), consistent with the queue being downstream of
translation. M7's scheduler idle and slot stalls are unchanged in magnitude.

---

## 4. Answers

**Q1 — Does M7 retain its W1 advantage with finite queues?**
**Yes, at every depth, and it slightly improves at the tightest one.** M7/SIMT = 1.726× unbounded,
**1.740×** at depth 8 (+0.84 %), because the cap costs SIMT 2.56 % but M7 only 1.70 %. M7 also stays
ahead of M8 at all depths on W1 (156 912 vs 156 419 unbounded; 159 585 vs 159 348 at 8).

**Q2 — Does bulk visibility remain useful when the controller cannot accept every request?**
**Yes.** Across all five workloads the accelerator speedups are unchanged (W0, HOT), slightly
improved (W1: +0.68 % M8, +0.84 % M7) or slightly eroded (PSCR −0.5 %, STRID −1.4 % at worst). No
cell collapses, and the largest movement anywhere is 1.8 %. Refusal counts in the millions produce
sub-2 % cycle changes, which is itself the finding: in this model the queue-depth limit is absorbed
by retry and residency slack rather than by lost throughput.

**Q3 — Does finite queue capacity reduce the benefit of page-organized issue?**
**Marginally, and not uniformly.** The page-organized arms lose a little where the workload already
had high row locality (STRID M7 1.201 → 1.180, PSCR M7 2.365 → 2.360; row-hit rate −0.7 pt, bank
utilization −1.0 to −1.7 pt) and *gain* on W1, where the baseline has essentially no locality to
lose. Mechanism: a shorter queue reduces the number of candidates FR-FCFS can reorder, which trims
bank overlap (−1.7 pt on W1) but does not undo locality that is built into the issue order.

**Q4 — Does the SIMT baseline benefit disproportionately from queue restrictions?**
**No — the opposite on the workload that matters.** SIMT is the *largest* loser at depth 8 on W1
(+2.56 %, versus +1.70 % M7 and +1.87 % M8), and it is unaffected on PSCR and HOT (its peak
occupancy there is 7 and 2, so the cap never binds). It gains a negligible 0.17–0.19 % on W0 and
STRID — the only two cells where any arm gets faster with a restricted queue, consistent with a
shorter queue suppressing slightly counter-productive FR-FCFS reordering in an already
row-local stream.

**Q5 — At what queue depth do performance results stabilize?**
**At 16 entries, exactly.** Depths 16, 32, 64 and unbounded are bit-identical in all 60
workload×arm cells, because the unbounded peak occupancy is 15–16 everywhere (PSCR SIMT is the
outlier at 7, HOT SIMT at 2). The threshold is a property of how many requests this model can have
in flight at the controller — the L2's 64 MSHRs minus those in service — not a property of the
controller's queue. Depth 8 is the only tested depth that binds, and it binds in the middle of the
model's own operating range rather than at its edge.

---

## 5. Limitations

### 5.1 The occupancy *distribution* is not measurable in this model

The brief asks for a DRAM queue occupancy distribution. The simulator exposes only `dram_max_q`
(peak) and `dram_q_full` (`src/sim.hpp:263–266`); there is no histogram and no occupancy integral.
What is reported instead (§3.3) is the peak plus the exact partition of cycles into *empty*,
*non-empty but undispatchable*, and *dispatch*. Adding a 65-bucket histogram would be a source
change, and the brief permits one only for a correctness defect — none was found, so the limitation
is documented rather than fixed. A histogram would add the shape (e.g. whether occupancy sits at
2–3 or clusters near the cap) that the peak cannot show; the bit-identical result at 16+ already
bounds the support.

### 5.2 The experiment cannot test the hypothesis it was commissioned for

The premise is that an unbounded queue gives FR-FCFS "broad visibility" that may favour bulk request
organization. **In this model that visibility never exceeds 16 entries**, so unbounded and depth 16
are the same experiment. The reason is structural: the queue only holds requests that have an L2
MSHR allocated but no DRAM service yet, and the 64-entry L2 MSHR together with fixed 14/26-cycle
timing keeps that set small. A real HBM controller queues 64–256 requests. **The measured
robustness therefore says "the accelerator gains do not depend on more than 16 entries of controller
visibility", not "they would survive a 64-entry real controller unchanged."** Testing the stronger
claim needs a deeper MLP (larger `R`/`l2_mshr`) or a queue placed downstream of the MSHRs, neither
of which this brief authorizes.

### 5.3 Queue semantics that differ from a real controller

* Refusal is applied **at L2 MSHR allocation and propagates to the requester**, which retries
  immediately; a real design parks the request in an L2 MSHR and stalls only admission. The
  simulated backpressure is stronger, so the measured insensitivity is, if anything, conservative.
* One command per cycle, **no bank groups, no refresh, no writeback or dirty-eviction traffic, no
  read/write turnaround**, fixed `t_hit`/`t_miss` independent of queue depth, and a single shared
  queue rather than per-bank queues.
* FR-FCFS has **no starvation bound**: row-hit priority can defer a request indefinitely and only
  `arrive` breaks ties within a class. With a finite queue, capacity implicitly bounds starvation;
  with an unbounded queue it does not. No starvation was observed here (`dram_blocked_cycles` stays
  a small fraction and every run completes), but the model cannot prove its absence.
* `dram_q_full` counts retry attempts, so it must not be read as a count of distinct blocked
  requests (§1, item 3).

### 5.4 Experimental scope

* One size (N = 16 384), one seed. The large-N W1 points where M8 regresses versus M7
  (`m8-scaling-experiment.md`) were **not** re-run here; at those sizes peak occupancy may exceed 16
  and the queue cap could matter more. That is the obvious follow-up.
* HOT and PSCR SIMT have peak occupancy 2 and 7, so no tested cap ≥ 8 can affect them; their rows
  are controls, not measurements of queue sensitivity.
* Five workloads, all with `rho_dup = 0` or small-Λ duplication; the duplicate-heavy regime
  (DUP/PGLO) was not included because it was not in the requested set.
* Nothing here changes any published number: the unbounded column reproduces every published
  baseline to the cycle, and no configuration file was edited.

---

## 6. Reproduction

```
cd DX100_MEM/sim && make -j4

# one cell (W1, M8, depth 8); vary dram_q in {8,16,32,64,0} and the arm flags
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=none --set mode=bulk \
  --set N=16384 --set Lambda=16384 --set P=512 --set kappa=0 --set seed=1 \
  --set batch_window=8192 --set bulk_max_elems=262144 --set max_cycles=200000000 \
  --set page_sched=1 --set idx_mode=2 --set dram_q=8 --all
```

Arms: A `mode=baseline`; M6 `mode=bulk page_sched=0 idx_mode=0`; M7 `mode=bulk page_sched=1
idx_mode=0`; M8 `mode=bulk page_sched=1 idx_mode=2`. Workload knobs: W0 `Lambda=16384, P=256,
kappa=1`; W1 `Lambda=16384, P=512, kappa=0`; PSCR `Lambda=16384, P=2048, kappa=0`; STRID
`Lambda=1024, P=64, kappa=1`; HOT `Lambda=2, P=1, kappa=0`. `--all` is required for
`dram_max_q`/`dram_q_full`, which `print_ab` does not show.
