# M23-G6D — Translation throughput scaling: the 11-cell experiment

**Complete: 11 / 11 cells, every validation check passed, the G6C baseline reproduced to the cycle.**
G6C (2 walkers, 2 cohorts, 4 dTLB MSHRs, 300-cycle walks) is the frozen baseline; only `walkers`,
`page_cohorts`, `dtlb_mshr` and `walk_latency_per_level` move, all on the command line.

## 0. Answer in one paragraph

Translation throughput is **exactly** `realized walker concurrency / 300 walks per cycle`, and every
cell obeys `cycles = walks × 300 / avg_active_walkers` to better than 0.01 %. Raising concurrency
therefore pays, and all three resources that bound it had to move together. On Orkut: **2→4 walkers
at the frozen width buys −22.1 %** (16 505 468 → 12 864 500) because only ~2.5 pages can be in flight;
**8 walkers buy nothing over 4** (identical cycles to the cycle), because `dtlb_mshr = 4` caps pages in
flight at 4. Widening admission (`page_cohorts 2→4`, `dtlb_mshr 4→8`) with 4 walkers buys a further
**−33.7 %** (→ 8 533 634); widening cohorts 4→8 buys only **−0.6 %** (→ 8 479 357) because the four
walkers are then 95 % busy. Giving the widened machine 8 walkers buys another **−27.7 %** →
**6 130 494 cycles (−62.9 % vs G6C, 8.00× M0)**, and there it **stops: the busier DRAM bank is busy
98.7 % of the run**. Halving the modelled walk service instead (`walk_latency_per_level 150→75`) lands
on the *same* wall from the other side, **6 116 784 cycles, 98.9 % bank occupancy** — two independent
routes to the same 6.05 M-cycle DRAM floor. The L2 TLB MSHR array is never a limit (`walk_stalls = 0`
in all 11 cells, even when at capacity 96 % of cycles). The readiness buffer fills naturally
(93 → 2 045 entries average, empty cycles 93 % → 3.8 %) but **row locality does not follow**: Orkut's
row-hit rate moves 60.49 % → 60.53 % and the DRAM service time is *identical* (18.74 cycles) in every
single cell. Under this model the next constraint is DRAM, not translation — for Orkut it already is.

---

## 1. Freeze and verification

| Item | Hash (SHA-256, 16 hex) | Note |
|---|---|---|
| `sim/configs/m23g6b_base.cfg` | `abc91fab3ba094c7` | **unchanged**, byte-identical to M23-G6C |
| trace Orkut BFS | `11cc8da4cfaf9a99` | unchanged (`build/wm/m23_orkut_bfs.bin`) |
| trace LiveJournal BFS | `7363867a364c7a9b` | unchanged |
| trace ogbn-products train gather | `c20ce42a72b04468` | unchanged (`build/wm/m23_ogbn_train_100d.bin`) |
| `src/bulk.cpp` | `9f098e9fb5433b4d` | +1 histogram accumulation, +2 export loops |
| `src/memory.cpp` | `9916f28f6bb2900a` | +2 histogram increments |
| `src/sim.hpp` | `cb8752b5e52654e7` | +3 histogram arrays |
| `src/system.cpp` | `5a314ed8ae75153f` | +2 export loops |
| `bin/dx100sim` | `0c731f09f31fc4d2` | instrumented build |
| `bin/dx100_tests` | `e5ebb2f98202c417` | **4 767 checks, 0 failures** |

**The only source change is instrumentation.** Three per-cycle occupancy histograms were added
(`tlb[].mshr_hist[17]`, `l2tlb.occ_hist[17]`, `BulkStats::g6b_ready_hist[13]`, exported as
`tlb0_mshr_h0..16`, `l2tlb_occ_h0..16`, `bulk_g6b_ready_h0..12`). No threshold, arbitration, capacity
or timing value was touched, and no counter is read by a decision.

**Equivalence evidence, not assertion.** Cell `A0` was run on the instrumented binary and compared
key-by-key with the stored M23-G6C Orkut `bulk_org = 8` log:

* `cycles = 16 505 468`, exactly the published G6C baseline;
* every one of the 653 pre-existing keys is **byte-identical** (`diff` of the two logs with only the
  47 new histogram keys filtered out returns nothing; the new log carries 700 keys = 653 + 47);
* the full suite passes (4 767 checks).

Historical artifacts were not touched: the M23-G6C matrix in `build/wm/m23g6c/`, its reports, and all
earlier logs are unchanged. The corrected walker-utilization values are in **this** report; the G6C
report's numbers are left as published, with a note in §2 on why its divisor was accidentally right.

## 2. Configurations, cells and the corrected utilization metric

Every cell: `configs/m23g6b_base.cfg`, `bulk_org = 8`, `batch_window = 16384`, `g6b_pa_capacity =
4096`, P2 release order, `park_retry = 1`, `R = 128`, L1 256×4, L2 16 384×8/64 MSHRs, dTLB 16×4 with
`dtlb_parked = 2`, L2 TLB 512×4 with 8 MSHRs, 2 banks × 4096 rows × 1 KiB, `t_hit 14` / `t_miss 26`,
FR-FCFS, `bank_hash = 1`. Cell command:

```
cd DX100_MEM/sim && ./bin/dx100sim --config configs/m23g6b_base.cfg \
  --set dataset=../build/wm/<trace>.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=<2097152|16777216> --set batch_window=16384 --set bulk_org=8 \
  --set walkers=<W> --set page_cohorts=<C> --set dtlb_mshr=<M> \
  --set walk_latency_per_level=<L> --all
```

| Cell | Workload | Walkers | Cohorts | dTLB MSHR | Walk lat/level | Purpose |
|---|---|---:|---:|---:|---:|---|
| A0 | Orkut | 2 | 2 | 4 | 150 | frozen G6C baseline |
| A1 | Orkut | 4 | 2 | 4 | 150 | Stage A: walkers |
| A2 | Orkut | 8 | 2 | 4 | 150 | Stage A: walkers |
| B0 | Orkut | 4 | 2 | 4 | 150 | Stage B: *identical to A1 by construction* |
| B1 | Orkut | 4 | 4 | 8 | 150 | Stage B: admission width |
| B2 | Orkut | 4 | 8 | 8 | 150 | Stage B: admission width |
| S1 | Orkut | 8 | 8 | 8 | 150 | supplementary: walkers at the widened width |
| C0 | Orkut | 4 | 8 | 8 | 150 | Stage C control (**= B2**) |
| C1 | Orkut | 4 | 8 | 8 | **75** | Stage C: fixed-service sensitivity |
| DLJ | LiveJournal | 4 | 8 | 8 | 150 | Stage D |
| DOGBN | ogbn train | 4 | 8 | 8 | 150 | Stage D |

`S1` is one cell beyond the prescribed A/B/C/D tables, run because Stage B left "can 8 walkers be
supplied once admission is wide?" unanswered and it is the milestone's core question. It is labelled
supplementary everywhere below and no conclusion rests on it alone (C1 reaches the same wall).

**Corrected walker utilization.** `walker_busy` is the per-cycle sum of busy walkers, so

```
walker_util      = walker_busy / (walkers × cycles)        # the divisor is the configured walker count
active_walkers   = walker_busy / cycles                    # average number of walkers doing work
idle walker-cycles = walkers × cycles − walker_busy
```

The M23-G6C extractor used `BANKS × cycles`. The semantics check confirms `walker_busy` is a sum over
walkers (`memory.cpp:298`, `ptw.walker_busy += busy` where `busy` counts walkers with `walk_busy_[i] >
cycle`), so banks were simply the wrong resource; they coincided only because `walkers == banks == 2`.
With 8 walkers the old formula would have reported 262 % (A2: 31.3 % is the true value) — the new
`tools/m23g6d_extract.py` parses `walkers=N` from each log's own header and refuses a cell whose
header disagrees with its name, so a cell cannot be scored against the wrong divisor.

**Two provenance details worth stating.** (a) `page_cohorts` is pure capacity: `win_seal` installs
`min(page_cohorts, 16)` cohorts of 32 B and every per-cohort vector is sized from `b.coh.size()`;
`win_admit_g6c`'s loop, oldest-ready-first ordering, `park_retry` deferral and buffer coupling are
byte-identical code at every cohort count. Nothing about the algorithm changes with C. (b) The
admission port's per-cycle budget stays `eng_issue = 4` probes at every width, which is ample (the
accepted rate is ≤ 0.135 runs/cycle); a larger cohort count costs scan work, not port cycles.

## 3. Stage A — walker count (Orkut, cohorts 2 / MSHR 4 frozen)

| Cell | Walkers | Cycles | vs A0 | avg active walkers | walker util | walker idle cycles | accepted |
|---|---:|---:|---:|---:|---:|---:|---:|
| A0 | 2 | 16 505 468 | — | 1.952 | 97.60 % | 790 936 | 823 786 |
| A1 | 4 | 12 864 500 | **−22.06 %** | 2.505 | 62.63 % | 19 229 300 | 823 516 |
| A2 | 8 | 12 864 500 | **−22.06 %** | 2.505 | 31.32 % | 70 687 300 | 823 516 |

A1 and A2 are **identical to the cycle**. Four walkers raise realized concurrency from 1.952 to 2.505
pairs (+28 %), which the −22 % cycle change matches almost exactly (`walks × 300 / 2.505` =
12 864 000). Eight walkers change nothing at all, and the reason is visible in the MSHR counters:
`dtlb_mshr = 4` allows at most four distinct pages to hold a translation, and occupancy is 2.53 of 4 on
average, so there is no fifth page for a fifth walker to take. **The walker count is the limit only up
to the page-supply ceiling; here that ceiling is the MSHR array, and it binds at 4.**

## 4. Stage B — admission width (Orkut, 4 walkers)

| Cell | Cohorts | MSHR | Cycles | vs A0 | vs A1 | avg active walkers | dTLB MSHR avg / at cap | defer depth avg / max |
|---|---:|---:|---:|---:|---:|---:|---|---|
| B0 (= A1) | 2 | 4 | 12 864 500 | −22.06 % | 0.00 % | 2.505 | 2.53 / 23.8 % | 1.50 / 2 |
| B1 | 4 | 8 | 8 533 634 | −48.30 % | **−33.66 %** | 3.776 | 5.60 / 35.2 % | 2.49 / 4 |
| B2 | 8 | 8 | 8 479 357 | −48.63 % | **−34.09 %** | 3.799 | 7.50 / 91.5 % | 4.48 / 8 |

B0 is the same configuration as A1 and produced the same cycles to the cycle — the determinism check
the plan intended. `page_cohorts = 2` was indeed the injection-width limit: **two cohorts → 2.5 pages
in flight; four cohorts → 3.78**. But **cohorts 4→8 buys 0.6 %**, and the reason is not the cohorts:
at B1/B2 the four walkers are 94.4 / 95.0 % busy, i.e. they are the limit. B2's dTLB MSHR array is
*also* at capacity 91.5 % of cycles, which looks like a second limit — §6 shows it is not.

## 5. Stage C — the fixed walk service (Orkut, best configuration)

Best of A/B: 4 walkers, 8 cohorts, 8 MSHRs (B2 = C0 = 8 479 357). C1 halves the modelled service to
150 cycles per walk.

| Cell | Walk lat/level | Cycles | vs A0 | service/walk | avg active | walker util | busier-bank busy | bank occupancy |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| C0 (= B2) | 150 | 8 479 357 | −48.63 % | 300 | 3.799 | 94.98 % | 6 112 022 | 72.1 % |
| C1 | 75 | 6 116 784 | **−62.94 %** | 150 | 2.634 | 65.85 % | 6 051 220 | **98.9 %** |
| S1 (8 walkers) | 150 | 6 130 494 | −62.86 % | 300 | 5.256 | 65.70 % | 6 053 308 | **98.7 %** |

Halving the service does **not** halve the cycles (8 479 357 → 6 116 784 is −27.9 %, not −50 %),
because with 150-cycle walks the same page-supply keeps only 2.63 walkers busy and the machine lands on
the DRAM floor instead: 6 051 220 bank-busy cycles = 98.9 % of the run. **This is the bottleneck
transition the brief asked for**, and it is reached by two independent routes — shorter walks (C1) and
more walkers (S1) — which agree to within 0.2 %.

## 6. The scaling law, and what actually bounds concurrency

Every cell satisfies, exactly:

```
walker_busy = walks × 300          (or ×150 in C1 — the service term, measured, no walk-cache hits)
cycles      = walks × service / avg_active_walkers = walks × service × cycles / walker_busy
```

so the only free variable is realized concurrency, and it is set by the smallest of four supplies:

| Cell | walkers | open cohorts | dTLB MSHR | pages in flight (L2 TLB occ avg) | **realized walkers** | which supply bound it |
|---|---:|---:|---:|---:|---:|---|
| A0 | 2 | 2 | 4 | 2.78 | 1.952 | walkers |
| A1/A2 | 4 / 8 | 2 | 4 | 2.51 | 2.505 | cohorts (2) + MSHR (4) |
| B1 | 4 | 4 | 8 | 5.57 | 3.776 | walkers (94 %) |
| B2 | 4 | 8 | 8 | 7.47 | 3.799 | walkers (95 %) |
| S1 | 8 | 8 | 8 | 5.27 | 5.256 | **DRAM** (98.7 %) |
| C1 | 4 | 8 | 8 | 5.06 | 2.634 | **DRAM** (98.9 %) |

**A full MSHR array is not proof that it is the constraint.** B2 sits at dTLB MSHR capacity 91.5 % of
cycles; adding four walkers (S1) *lowers* that occupancy to 62.6 % while raising throughput 38 %. The
array was full because walks drained slowly, not because pages were queued behind it. The correct test
is whether adding the resource raises throughput, and here it did — until DRAM.

**The L2 TLB MSHR array never limits anything.** `l2tlb_walk_stalls = 0` in all 11 cells, including B2
(at capacity 90.75 % of cycles) and DLJ/DOGBN (96.4 %). It cannot: an L2 TLB MSHR is only allocated for
a page that already holds a dTLB MSHR, so its occupancy is `≤ dtlb_mshr` and the *smaller* array gates
first. We never raised `l2tlb_mshr` because no cell gives a reason to.

## 7. Stage D — cross-workload

| Workload | G6C baseline | best cell | cycles | vs G6C | ×M0 | vs P0 | vs P2 | vs G6B | walker util | MSHR at cap | bank occ. |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut | 16 505 468 | S1 (8/8/8) | 6 130 494 | **−62.86 %** | 8.004 | −63.3 % | −74.4 % | −76.4 % | 65.7 % | 62.6 % | 98.7 % |
| Orkut | 16 505 468 | B2/C0 (4/8/8) | 8 479 357 | −48.63 % | 5.787 | −49.3 % | −64.6 % | −67.3 % | 95.0 % | 91.5 % | 72.1 % |
| LiveJournal | 28 256 445 | DLJ (4/8/8) | 14 146 860 | **−49.93 %** | 4.753 | −50.0 % | −56.1 % | −63.9 % | **99.07 %** | **97.2 %** | 50.5 % |
| ogbn train | 199 738 512 | DOGBN (4/8/8) | 98 563 897 | **−50.65 %** | 2.103 | −50.7 % | −71.6 % | −73.7 % | **99.83 %** | **97.0 %** | 90.7 % |

The same configuration generalises (both baselines are reproduced exactly by A0's twin arms in the G6C
matrix, and both D cells pass every conservation check), but the *binding resource* differs by
workload: LiveJournal and ogbn sit at **99.1 % and 99.8 % walker utilization** with the MSHR array at
97 % of capacity, i.e. they are still translation-bound at four walkers. Orkut is already at the DRAM
wall. That is exactly the information the milestone needed.

## 8. Required measurements

**Translation.** `tlb0_hits/misses`: 78.63 % hit rate on Orkut in *every* cell (dTLB behaviour is
unchanged by walkers); LiveJournal 60.50 %, ogbn 70.98 %. dTLB evictions are **not exported** by the
model (`tlb[].evict` is accumulated in `tlb_fill` but no `tlb0_evict` key is printed; only
`l2tlb_evict` is) — a measurement gap, listed in §10. L2 TLB: `l2tlb_evict ≈ walks` in every cell
(106 866–106 917 on Orkut, 186 354 LJ, 1 311 403 ogbn). Walk-cache hits 0 in all 11. PTE fetches
= 2 × walks (214 756–214 858 Orkut; 2 623 830 ogbn).

| Cell | walks | walks/1k cyc | runs released/cyc | walk floor @W | gap to floor | busy/cycles | idle walker-cycles |
|---|---:|---:|---:|---:|---:|---:|---:|
| A0 | 107 400 | 6.51 | 0.0499 | 16 110 000 | +2.45 % | 1.952 | 790 936 |
| A1/A2 | 107 429 | 8.35 | 0.0640 | 8 057 175 @4 | +59.7 % | 2.505 | 19 229 300 |
| B1 | 107 410 | 12.59 | 0.0965 | 8 055 750 @4 | +5.93 % | 3.776 | 1 911 536 |
| B2/C0 | 107 378 | 12.66 | 0.0971 | 8 053 350 @4 | +5.29 % | 3.799 | 1 704 028 |
| S1 | 107 405 | 17.52 | 0.1344 | 4 027 688 @8 | +52.2 % | 5.256 | 16 822 452 |
| C1 | 107 407 | 17.56 | 0.1347 | 4 027 762 @4 | +51.9 % | 2.634 | 8 356 086 |
| DLJ | 186 866 | 13.21 | 0.0515 | 14 014 950 @4 | +0.94 % | 3.963 | 527 640 |
| DOGBN | 1 311 915 | 13.31 | 0.0850 | 98 393 625 @4 | +0.17 % | 3.993 | 681 088 |

"Gap to floor" is large exactly where realized concurrency is below the configured walker count
(A1/A2 because pages are capped at 2.5, S1/C1 because DRAM caps the run); it is ≤ 6 % in the four
cells where the walkers are genuinely saturated, which is the signature of a well-fed translation
pipeline.

**Admission.** All cells: `attempts = accepted + refused`, `refuse_other = 0`, `dup_submit = 0`,
deferral events = refusals, deferral depth ≤ cohorts. Refusals split parked/full: A0 24.5 M/7.5 M,
B1 24.6 M/7.0 M, B2 40.2 M/22.2 M, S1 19.9 M/11.0 M, C1 18.6 M/10.9 M, DLJ 54.2 M/55.8 M,
DOGBN 721.8 M/48.8 M. Deferral age (first refusal → admission) tracks the walk: 316 (A0), 305 (B1),
505 (B2), 244 (S1), 252 (C1), 472 (DLJ), 555 (DOGBN) — always ≈ service + queue. Cohort openings are
constant per workload (106 286–106 301 Orkut, 185 896 LJ, 1 302 691 ogbn), i.e. one cohort per page per
window: the policy never re-opens or churns pages. `bulk_g6c_admit_idle_cycles` falls from 97.5 % to
85.1 % as throughput rises; as established in the G6D investigation, this counter means "every open
cohort was deferred in this cycle", not "no work".

**Ready buffer.**

| Cell | avg | peak | empty cycles | ≥ 1024 entries | mean residence | no-ready cycles | backpressure | translation/DRAM overlap |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A0 | 93.3 | 4096 | 92.97 % | 2.77 % | 1 870 | 91.38 % | 0.71 % | 47.17 % |
| A1 | 141.0 | 4096 | 88.25 % | 4.12 % | 2 203 | 86.58 % | 0.99 % | 54.06 % |
| B1 | 476.2 | 4096 | 60.24 % | 15.52 % | 4 934 | 58.17 % | 2.45 % | 76.86 % |
| B2/C0 | 481.3 | 4096 | 58.58 % | 15.59 % | 4 956 | 57.53 % | 2.78 % | 72.97 % |
| S1 | 2 045.3 | 4096 | 3.84 % | 74.49 % | 15 224 | 1.82 % | 8.11 % | 68.65 % |
| C1 | 2 111.1 | 4096 | 3.51 % | 76.28 % | 15 678 | 1.53 % | 9.70 % | 66.90 % |
| DLJ | 59.0 | 4096 | 94.82 % | 1.74 % | 1 144 | 93.69 % | 0.60 % | 59.24 % |
| DOGBN | 23.8 | 4096 | 85.56 % | 0.23 % | 279 | 84.52 % | 0.07 % | 95.44 % |

The buffer is never the translation limit: backpressure stays ≤ 9.7 % and is a *consequence* of
translation outrunning DRAM, not a cause of anything.

**DRAM.** Identical in every cell: reads 644 016–651 193 (Orkut), 697 141 (LJ), 9 826 463 (ogbn);
bytes 41.2–41.7 MB, 44.6 MB, 628.9 MB; rows used 8 185 / 8 192 / 8 192; service time **18.74 / 20.46 /
18.20 cycles per read, unchanged from the G6C baseline in every Orkut and LJ cell**.

| Cell | reads | row-hit | bank0 busy | bank1 busy | occupancy of busier bank | queue occupancy | starved cycles |
|---|---:|---:|---:|---:|---:|---:|---:|
| A0 | 651 193 | 60.49 % | 6 078 156 | 6 125 930 | 37.1 % | 7.96 | 65.32 % |
| A1 | 650 844 | 60.47 % | 6 076 658 | 6 122 506 | 47.6 % | 12.13 | 51.50 % |
| B1 | 649 355 | 60.48 % | 6 063 516 | 6 106 738 | 71.6 % | 29.12 | 30.55 % |
| B2/C0 | 649 735 | 60.47 % | 6 066 480 | 6 112 022 | 72.1 % | 30.81 | 26.89 % |
| S1 | 644 142 | 60.53 % | 6 015 668 | 6 053 308 | **98.7 %** | 60.31 | 0.82 % |
| C1 | 644 016 | 60.53 % | 6 015 308 | 6 051 220 | **98.9 %** | 60.44 | 0.64 % |
| DLJ | 697 141 | 46.14 % | 7 148 918 | 7 116 480 | 50.5 % | 9.66 | 47.93 % |
| DOGBN | 9 826 463 | 65.04 % | 89 381 462 | 89 416 772 | 90.7 % | 20.92 | 7.86 % |

**End-to-end, conservation.** Every cell: `conservation_ok = 1`, `conservation_violations = 0`,
`bulk_tgt_req_gen == bulk_tgt_req_done == bulk_lines_issued == bulk_g6b_runs_released`,
`bulk_g6b_tr_requests == bulk_g6b_tr_completed == bulk_org_runs == bulk_g6c_admit_accepted`,
`bulk_elems_done == bulk_elems_total` (1 199 876 Orkut, 1 199 762 LJ, 8 393 035 ogbn),
`bulk_win_entries == bulk_elems`, `bulk_addr_mismatch = bulk_residency_violations =
bulk_issue_monotonic_violations = 0`, `bulk_g6b_ready_occ_sum == bulk_g6b_tr_wait_dram`,
`ready_peak ≤ capacity`, no `max_cycles` termination, `bulk_g6c_refuse_other = 0`,
`dup_submit = 0`, deferral depth ≤ cohorts, `walker_max ≤` configured walkers, `tlb0_mshr_max ≤`
capacity, and each log's own `walkers= / dtlb= mshr= / l2tlb= mshr=` header matches its cell name.

## 9. Critical analysis

**1. Do 2→4/8 walkers increase actual simultaneous walks?** 2→4: yes, 1.952 → 2.505 (+28 %), −22.1 %
cycles, at 2 cohorts. 4→8 at the same width: **no, not by one cycle** (A1 ≡ A2), because
`dtlb_mshr = 4` limits pages in flight to 4 and only ~2.5 arrive. 4→8 at the widened width (B2 → S1):
yes, 3.799 → 5.256 (+38 %), −27.7 % cycles. Walkers convert only when pages are available.

**2. Is `page_cohorts = 2` limiting injection width?** Yes. B0 → B1 (2 → 4 cohorts, MSHR 4 → 8):
−33.66 %, pages in flight 2.53 → 5.60. But 4 → 8 cohorts (B1 → B2) gives −0.6 %: four cohorts already
saturate four walkers. Injection width was *a* limit at 2 and is not one at 4.

**3. Do more dTLB MSHRs keep more distinct pages in flight?** Yes, and they are necessary: at 2
cohorts (A1) occupancy is 2.53 of 4; at 4 cohorts/8 MSHRs, 5.60; at 8/8, 7.50. But the MSHR array is
only the *binding* resource when the walkers are fast enough to drain it (S1 lowers it to 5.31 while
raising throughput). The correct statement is that MSHRs set the page-supply ceiling and walkers set
the drain rate; either can be binding.

**4. Does the L2 TLB MSHR array become a bottleneck?** No. `walk_stalls = 0` in all 11 cells, even
when the array is 96 % occupied (DLJ). It is structurally shadowed by the dTLB array: an L2 TLB MSHR
requires a dTLB MSHR on the same page, and `l2tlb_max_occ ≤ dtlb_mshr` in every cell (4/4, 8/8, 8/8).
No cell justifies raising it.

**5. Does translation completion throughput scale with walker count?** Throughput equals realized
concurrency / service, exactly: 6.51 → 17.52 walks per 1 000 cycles (Orkut). It scales with walker
count only while walkers are the smallest supply — the three-regime pattern above.

**6. When does DRAM become dominant?** Orkut: at 6.05 M cycles (S1 98.7 %, C1 98.9 % of the run), i.e.
already at the best configuration; further translation work cannot pay. LiveJournal: not yet — its
DRAM floor is 7.15 M against 14.15 M cycles (50.5 %), so ~−50 % remains, gated by walkers/MSHRs. ogbn:
almost — 89.4 M against 98.6 M (90.7 %), so at most ~−9 % remains.

**7. Does the 75-cycle control approach a DRAM-limited regime?** Yes, precisely: C1 = 6 116 784 with
6 051 220 bank-busy cycles (98.9 %). The run is DRAM-limited, not walk-limited (walkers 65.9 % busy).
C1 and S1 agreeing to 0.2 % is the strongest evidence in this report that the wall is DRAM.

**8. Does the best configuration generalise?** Yes on cycles (−49.9 % LJ, −50.7 % ogbn), but *not* on
which resource binds: LJ (99.07 % walker busy, 97.2 % MSHR at cap) and ogbn (99.83 %, 97.0 %) are still
translation-bound whereas Orkut is DRAM-bound. The same knob turn is correct everywhere; its value is
not.

**9. Does ready-buffer occupancy increase naturally?** Yes, monotonically and strongly: 93 → 141 →
476 → 2 045 entries average; empty cycles 93.0 % → 3.8 %; ≥ 1 024 entries 2.8 % → 74.5 %; mean residence
1 870 → 15 224 cycles; no-ready cycles 91.4 % → 1.8 %. The buffer absorbs the faster translation exactly
as designed, and starts to backpressure (8–10 %) only once DRAM saturates.

**10. Does higher translation throughput improve DRAM row locality?**
Four separate observations, which are not equivalent:

* **More translated runs available**: yes — resident runs 93 → 2 045 (Orkut), 3.8 → 23.8 (ogbn).
* **More distinct (bank,row) candidates visible at once**: yes — the normalized release order can now
  draw from 74.5 % of cycles with ≥ 1 024 resident runs instead of 2.8 %.
* **Higher row-hit rate**: **essentially no.** Orkut 60.49 % → 60.53 % (+0.04 pt); LiveJournal
  46.14 % → 46.14 % (identical); ogbn 62.73 % → 65.04 % (**+2.31 pt**, the one real gain).
* **Lower mean DRAM service time**: **no on two of three** — 18.74 cycles in every Orkut cell, 20.46
  in LJ, and only ogbn moves (18.47 → 18.20, −1.5 %).

The mechanism is that the P2 release key already fixes the *sequence*; translation throughput changes
only the *arrival rate* into that sequence, so row hits stay put. ogbn gains because its window
(16 384 runs) is 4× the buffer, so at baseline the scheduler pops almost immediately (buffer avg 3.8)
and effectively releases in arrival order; with 23.8 resident runs it follows the P2 key more closely.
Orkut's 11 132-run windows already fit comfortably enough that extra depth adds nothing. Translation
does reduce DRAM *reads* slightly (Orkut 651 193 → 644 142, −1.1 %; ogbn 9 924 795 → 9 826 463, −1.0 %)
by raising L2 hit rates for target lines — a bandwidth effect, not a locality effect.

## 10. Model-fidelity limitations

1. **The walk is a fixed 300-cycle delay.** It is `levels × walk_latency_per_level`, charged from the
   issue cycle; C1 simply halves that constant. Every absolute number in this report is controlled by
   an [A] parameter. What is transferable is the *structure* — `cycles = walks × service / realized
   concurrency`, and the resource ordering — not the cycle counts.
2. **PTE traffic does not gate walk completion.** `ptw_tick` issues real `l2_probe`s for both levels and
   discards their ready time; a walk cannot be slowed by memory. Under load (ogbn: 2.62 M PTE fetches,
   10 % of DRAM reads) a latency-gated walker would be slower, so these numbers are an upper bound on
   translation throughput.
3. **No page-walk cache.** The 16-entry FIFO records 0 hits in all 11 cells; at 1 451–2 553 pages per
   window, no small cache could help, so this is a fidelity gap with no effect here.
4. **Simplified two-level translation, no configurable page size.** Sv32, 4 KiB pages, one leaf PTE
   line per 16 pages. A real hierarchy would have intermediate-level caching.
5. **The two levels are charged serially inside one walker**, so a real PTW that pipelines level 1 of
   walk B against level 2 of walk A would get more per walker at the same latency.
6. **Measurement gaps.** dTLB *evictions* are accumulated but not exported (adding the key would have
   required re-running all 11 cells for a field no conclusion depends on); there is no counter for the
   number of distinct pages in the footprint (walks and `l2tlb_evict` are the proxies); and the
   ready-buffer histogram is sampled only while a window is being issued (its coverage is checked:
   ≥ 90 % of cycles, and its bucket bounds are verified to bracket the exact occupancy integral).
7. **Non-invariance at the margin, reported rather than hidden.** The window seal is deadline-driven,
   so the *run count* is not perfectly invariant across cells: Orkut 823 516–823 786 runs, ogbn
   8 380 095 (513 windows) vs the G6C baseline's 8 380 253 (514 windows), out of 1 199 876 / 8 393 035
   elements. The element stream is identical and conserved; the difference is ≤ 0.03 %, moves the walk
   count by ≤ 0.1 % (107 400 → 107 429; ogbn 1 312 297 → 1 311 915), and changes no ranking.
8. **No ping-pong buffering, no DRAM scheduling change, no window change** — as instructed, so this
   report says nothing about whether a deeper translation/DRAM decoupling would help; the DRAM results
   (§9.6, §9.10) suggest it would not, because the wall is bank service time, not buffer depth.
9. **No physical-hardware claim.** All of this is one simulator at fixed latencies.

## 11. Recommended next architecture experiment

1. **Finish the walker ladder on the two workloads that are still translation-bound** (2 cells):
   `walkers=8` at `cohorts=8`, `dtlb_mshr=8` on LiveJournal and ogbn, using the frozen configuration.
   Prediction from the Orkut B2→S1 step (+38 % when occupancy falls from 91.5 % to 62.6 %):
   LiveJournal ≈ 10.2 M (−28 % further, still above its 7.15 M DRAM floor), ogbn ≈ 92 M (−7 % further,
   stopping at its 89.4 M floor). If LiveJournal instead stays flat, its MSHR array — not its walkers —
   is the next thing to raise (16 MSHRs, which also needs `page_cohorts ≥ 8`).
2. **Then stop scaling translation.** On Orkut, and shortly on ogbn, more walkers cannot pay: the
   busier bank is 98.7–90.7 % occupied. The next milestone should attack **DRAM service** — the
   measured 18.74 cycles/read at 60.5 % row hits — which is exactly the territory of the ping-pong /
   deeper-decoupling and bandwidth work this brief excludes. That is the honest recommendation: the
   translation ceiling has been found, and it is not translation any more.

## 12. Reproduction

```
./build/wm/m23g6d_run.sh plan                    # A0..B2, S1, C0, C1, DLJ, DOGBN
./build/wm/m23g6d_run.sh A0 A1 A2 B0 B1 B2 S1 C0 C1 DLJ DOGBN
python3 tools/m23g6d_extract.py                  # 11/11 cells, all checks passed, writes summary.csv
```

Raw logs: `build/wm/m23g6d/g6d_<CELL>.txt`; reduced table: `build/wm/m23g6d/summary.csv`; run log
`build/wm/m23g6d_run.log` (`EXIT=0`). Total host time ≈ 25 min (ogbn dominates). Checks actually run:
the full unit suite on the instrumented binary (4 767 checks, 0 failures), the A0 key-by-key diff
against the stored G6C log, and the 11-cell validation table in §8. Not run: any cell outside the 11
listed, `l2tlb_mshr` sweeps (no cell justifies one), and anything touching DRAM scheduling or the
window.
