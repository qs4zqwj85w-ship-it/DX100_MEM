# M23-G6B — Streaming translation / DRAM decoupling

**Status: complete. 15 matrix cells + 10 capacity cells, 0 failures, 0 conservation violations,
published P0/P2 anchors reproduced to the cycle.**

G6A established that a full translation barrier recovers P2's DRAM locality but serialises the
page-walk cost. G6B asks the obvious follow-up: keep the two phases *concurrent*.

**Answer.** The concurrency works and the buffer never becomes the bottleneck — translation is
active in 99.5–100 % of the run and 98.7–100 % of all DRAM work overlaps it, which removes G6A's
serialisation (**−17.3 % / −13.7 % / −16.1 % cycles versus G6A**, Orkut / LiveJournal /
ogbn-products train). It does **not** recover the locality: the release stream fragments into
459 k / 494 k / 5.61 M DRAM cell changes (P2: 285 k / 346 k / 2.03 M), row-hit rate lands at
60.47 / 46.13 / 61.49 % (P0: 60.43 / 46.12 / 62.70 %), i.e. essentially P0 with no cross-page
recovery. The reason is quantitative, not structural: with translation as the throughput limit,
the ready buffer holds only **33.8 / 11.8 / 1.1 entries on average**, about one to two rows, so
the DRAM scheduler has almost nothing to reorder. Consequently G6B is still **+55.2 % / +38.5 % /
+87.7 % slower than P0** and 8–22 % slower than P2.

Everything else — walks, TLB misses, MSHR limits, DRAM timing, caches, the P0/P2 code paths — is
untouched, and P0/P2/G6A/ideal-P2 re-run on the same binary reproduce the G6A matrix exactly.

---

## 1. What was built

`bulk_org = 7`, a third experimental issue policy on the existing fused window. Three components,
each an existing mechanism plus one new coupling point.

**1. Translation scheduler.** Walks the window's runs in *stored* order (ascending line, so a
page's 64 runs stay together — page-aware), and submits one translation-only probe
(`Req::xlate_only`) at a time. Bounded only by the real dTLB MSHRs / `dtlb_parked` / L2 TLB /
walkers / walk latency, and by the ready buffer's capacity. It never consults the DRAM order — it
cannot, the addresses are not translated yet — so P2's row-based cohort ordering is not imposed on
translation.

**2. Ready buffer.** A completed probe drops its run into a priority queue keyed by the run's
position in `ford`, i.e. by physical `(row, bank, line)`. One entry = physical line + translation
cycle + identity (the run index). Capacity is `g6b_pa_capacity = 4096` entries: when
`ready + in-flight ≥ capacity`, admission stops and the cycle is counted as backpressure. Requests
cannot bypass translation: an entry exists only after `tick` produced the address.

**3. DRAM scheduler.** Pops the *smallest* key available — the most local translated line that
currently exists — and releases it through the unchanged `win_emit_fused` path with
`Req::pretranslated`. Same DRAM controller, bank timing, FR-FCFS, cache hierarchy, residency
budget and completion path as P2. Cell changes are counted as cohort openings.

The two run in the same `win_issue_g6b` call, every cycle, in that order. No barrier, no phase
state: a window is adopted, translated and drained in one continuous stream, and retires when its
last line completes.

### Implementation

| file | change |
| --- | --- |
| `sim/src/sim.hpp` | `Config::g6b_pa_capacity`; `Batch::ready` (priority queue of `ford` positions), `fpos`, `g6b_init/tr_next/tr_done/tr_start/cell_valid/bk/rw`; 17 `BulkStats::g6b_*` counters; `#include <queue>/<functional>` |
| `sim/src/bulk.cpp` | `win_issue_g6b` (translation admission + ready buffer + minimising DRAM release); routing at the head of `win_issue`; `win_emit_fused` carries the buffered PA for `bulk_org == 7` and accounts the post-translation wait; validation `bulk_org ≤ 7`, `g6b_pa_capacity > 0` |
| `sim/src/util.cpp` | parse/echo `g6b_pa_capacity` |
| `sim/src/memory.cpp` | **unchanged since G6A** — `xlate_only` and `pretranslated` are G6A's, reused as-is |

The G6A paths (`bulk_org = 6`) and the published paths are untouched; the only shared-code edits
are the two policies' guards (`bulk_org == 6` → `6 || 7`) and the validation bound.

**Liveness.** A non-empty buffer always yields a releasable run (either it issues, or the
residency budget is full and drains); a run only enters the buffer after its translation
completes; translation is admitted whenever `ready + in-flight < capacity`. There is no cycle in
which both can wait — verified in practice: no cell hit `max_cycles`, every window retired, and
`bulk_tgt_req_gen == done == lines_issued` in all 15 cells.

## 2. Configurations and cells

`sim/configs/m23g6b_base.cfg` = `m23g6a_base.cfg` + `g6b_pa_capacity = 4096` (plus the declared
`bulk_org`). All five arms run on this one file; only `bulk_org` and `ideal_translation` move.
16 K window, `R = 128`, L2 16384×8 with 64 MSHRs, dTLB 16×4 with 4 MSHRs, `dtlb_parked = 2`,
L2 TLB 512×4 with 8, 2 walkers, 2 banks × 4096 rows, `t_hit 14` / `t_miss 26`, fused window,
`page_sched = 1`, `page_cohorts = 2`, `org_overlap = 1`.

```
./build/wm/m23g6b_run.sh all 5      # 15 cells
./build/wm/m23g6b_run.sh caps 2     # capacity sweep (10 cells)
python3 tools/m23g6b_extract.py
```

Traces are M23-G6A's: `m23_orkut_bfs.bin` (1 200 003 accesses), `m23_livejournal_bfs.bin`
(1 200 003), `m23_ogbn_train_100d.bin` (8 393 035). Logs `build/wm/m23g6b/*.txt`, hashes
`build/wm/m23g6b/hashes.txt`, table `summary.csv`.

**Anchors** — P0 and P2 are the published policies, so they must reproduce exactly:

| workload | policy | published | measured | |
| --- | --- | --- | --- | --- |
| Orkut | P0 | 16 714 538 | 16 714 538 | OK |
| Orkut | P2 | 23 965 633 | 23 965 633 | OK |
| LiveJournal | P0 | 28 309 390 | 28 309 390 | OK |
| LiveJournal | P2 | 32 260 832 | 32 260 832 | OK |

The G6A and ideal-P2 arms also reproduce the G6A matrix cycle-for-cycle (31 373 796 / 45 440 974 /
446 942 992 and 6 076 339 / 6 848 866 / 74 854 523), which is the check that adding this policy
perturbed nothing.

## 3. Results

### 3.1 Execution time

| workload | P0 | P2 | G6A | **G6B** | ideal-P2 |
| --- | --- | --- | --- | --- | --- |
| Orkut | 16 714 538 | 23 965 633 (+43.38 %) | 31 373 796 (+87.70 %) | **25 938 106 (+55.18 %)** | 6 076 339 |
| LiveJournal | 28 309 390 | 32 260 832 (+13.96 %) | 45 440 974 (+60.52 %) | **39 200 163 (+38.47 %)** | 6 848 866 |
| ogbn train | 199 882 548 | 347 389 697 (+73.80 %) | 446 942 992 (+123.60 %) | **375 172 893 (+87.70 %)** | 74 854 523 |

Speedup versus P0: 0.644× / 0.722× / 0.533×. Versus P2: +8.23 % / +21.51 % / +8.00 % (G6B
slower). Versus G6A: −17.32 % / −13.73 % / −16.06 % (G6B faster).

### 3.2 Translation: unchanged work, still the throughput limit

| workload | cell | walks | PTE fetches | dTLB misses | L2 TLB misses | mean dTLB-MSHR occ (/4) | walker util | mean TLB lat |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Orkut | P0 | 107 399 | 214 798 | 192 014 | 107 399 | 2.75 | 96.4 % | 88 |
| Orkut | P2 | 107 444 | 214 888 | 192 050 | 107 444 | 1.81 | 67.2 % | 83 |
| Orkut | G6A | 107 401 | 214 802 | 192 033 | 107 401 | 1.29 | 51.3 % | 78 |
| Orkut | **G6B** | 107 439 | 214 878 | 192 108 | 107 439 | **1.56** | **62.1 %** | 78 |
| LiveJournal | P0 | 186 923 | 373 846 | 317 832 | 186 923 | 3.24 | 99.0 % | 187 |
| LiveJournal | **G6B** | 186 922 | 373 844 | 317 855 | 186 922 | **1.93** | **71.5 %** | 156 |
| ogbn train | P0 | 1 313 192 | 2 626 384 | 2 588 592 | 1 313 192 | 2.13 | 98.5 % | 94 |
| ogbn train | **G6B** | 1 313 204 | 2 640 808 | 2 588 553 | 1 313 204 | **1.06** | **52.5 %** | 89 |

The translation *work* is P0's to within 0.02 % — walks, TLB misses and PTE fetches are
identical, `tlb0_mshr_max = 4` everywhere. What is not identical is how well admission keeps the
pipeline full: G6B holds only 1.06–1.93 of the 4 dTLB MSHRs on average, against P0's 2.13–3.24,
and its walkers are 62 / 72 / 53 % busy against P0's 96 / 99 / 99 %.

Streaming admission also keeps the translation pipeline busier than the barrier did in every
workload (MSHR occupancy 1.56 / 1.93 / 1.06 against G6A's 1.29 / 1.67 / 0.89), which is part of
why G6B is faster than G6A and not the whole of it.

The cause is visible in the published policy's own counters. P0 refuses a translation probe in
`bulk_issue_stall` (32.3 M cycles on Orkut) but **defers that page cohort and issues another**
(`bulk_sched_defer` = 31.8 M events), so probes from several pages are in flight at once. G6B's
admission is strictly the stored order — one page at a time — so when the current page's MSHR
group (1 + `dtlb_parked` = 3 waiters) is full the port blocks on *that page* and the other MSHRs
go unused. That is why the same walk work takes 25.9 M cycles here and 16.1 M of walker-busy time
in P0. Whether interleaving pages inside translation admission closes the gap is a separate
scheduling policy; per the milestone's instruction it is **not** attempted here.

### 3.3 DRAM organization

| workload | cell | row hits % | row conflicts | bank util % | bytes/cycle | DRAM reads | cell changes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Orkut | P0 | 60.43 | 257 698 | 36.53 | 2.494 | 651 309 | 106 289 |
| Orkut | P2 | 60.47 | 257 473 | 25.47 | 1.739 | 651 339 | 285 044 |
| Orkut | G6A | 60.83 | 254 992 | 19.40 | 1.328 | 651 022 | 284 999 |
| Orkut | **G6B** | **60.47** | 257 438 | 23.53 | 1.607 | 651 220 | **459 300** |
| Orkut | ideal-P2 | 60.53 | 254 024 | 99.22 | 6.779 | 643 579 | 285 016 |
| LiveJournal | P0 | 46.12 | 375 629 | 25.20 | 1.576 | 697 219 | 185 913 |
| LiveJournal | P2 | 50.84 | 342 940 | 21.51 | 1.384 | 697 595 | 346 481 |
| LiveJournal | G6A | 52.89 | 328 770 | 15.09 | 0.983 | 697 898 | 346 414 |
| LiveJournal | **G6B** | **46.13** | 375 544 | 18.20 | 1.138 | 697 145 | **494 000** |
| LiveJournal | ideal-P2 | 52.63 | 327 392 | 99.32 | 6.459 | 691 164 | 347 004 |
| ogbn train | P0 | 62.70 | 3 701 864 | 45.87 | 3.178 | 9 925 604 | 1 303 925 |
| ogbn train | P2 | 66.21 | 3 354 401 | 25.80 | 1.829 | 9 927 758 | 2 031 937 |
| ogbn train | G6A | 74.91 | 2 492 609 | 18.91 | 1.423 | 9 935 534 | 2 034 712 |
| ogbn train | **G6B** | **61.49** | 3 823 321 | 24.64 | 1.694 | 9 928 055 | **5 609 697** |
| ogbn train | ideal-P2 | 76.75 | 2 069 444 | 99.81 | 7.609 | 8 899 407 | 2 033 878 |

Locality recovered, as a fraction of the ideal-P2 gain over P0: **40 % / 0.2 % / negative**.
Orkut has almost nothing to recover (ideal gain 0.10 pt). LiveJournal recovers 0.01 pt of an
available 6.51. ogbn is *worse than P0* by 1.21 pt: the released stream is essentially
translation order with a shallower DRAM queue than P0 keeps (`dram_q_occ_sum/cycles` 1.49 versus
3.30), so FR-FCFS has less to work with than it does in P0.

The cell-change counts say the same thing directly: G6B changes `(bank,row)` cell 1.6× / 1.4× /
2.8× as often as P2. A minimising release over a 1–34 entry window cannot reconstruct the row
runs P2 gets from a whole window.

### 3.4 Overlap, buffer, and starvation

| workload | buffer capacity | peak | mean occupancy | backpressure | DRAM found nothing ready | translation active | targets outstanding | overlap (% of target time) | mean wait for DRAM | max wait |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Orkut | 4096 | 4096 | 33.8 | 36 468 (0.14 %) | 96.78 % | 99.5 % | 30.9 % | **98.7 %** | 1 066 | 30 318 |
| LiveJournal | 4096 | 4096 | 11.8 | 24 220 (0.06 %) | 98.77 % | 99.8 % | 25.8 % | **99.4 %** | 632 | 30 307 |
| ogbn train | 4096 | 3 319 | 1.1 | 0 (0.00 %) | 98.97 % | 100.0 % | 26.1 % | **100.0 %** | 51 | 252 497 |

*Overlap* is a cycle in which at least one translation-only probe is in flight **and** at least one
release of the same window is outstanding. It is 98.7–100 % of all target-outstanding time: the
DRAM phase carries no un-hidden translation cost, which is exactly what G6A could not do.

*DRAM found nothing ready* is the scheduler's own view at release time: 96.8–99.0 % of cycles it
had issue budget and no translated request to issue. The run is translation-limited, and the DRAM
is starved by translation, not by the buffer. `dram_starved_cycles` agrees (77.1 / 83.0 / 78.7 %
of the run with an empty eligible queue).

The buffer is **not** a bottleneck anywhere: backpressure is ≤ 0.14 % of cycles, mean occupancy is
1.1–33.8 of 4096, and the occupancy integral equals the summed buffer residence time exactly
(`bulk_g6b_ready_occ_sum == bulk_g6b_tr_wait_dram` in every cell — the extractor checks this as a
closed-accounting invariant).

Scheduling cost: 459 k / 494 k / 5.61 M cell changes and `bulk_sched_scan` = one step per release
(823 753 / 729 444 / 8 380 079), `sched_idle = 0` (the scheduler never idled with work pending;
it was starved instead), `bulk_slot_stall` 555 k / 184 k / 258 k cycles (residency budget full).

### 3.5 Conservation and correctness

`tools/m23g6b_extract.py`, 15/15 cells: `conservation_ok = 1`, `conservation_violations = 0`,
`bulk_residency_violations`, `bulk_addr_mismatch`, `bulk_issue_monotonic_violations` all 0,
`dispatch_partition_ok = 1`, `bulk_tgt_req_gen == bulk_tgt_req_done`,
`bulk_elems_done == bulk_elems_total` (1 199 876 / 1 199 762 / 8 393 035), no cell hit
`max_cycles`. G6B-specific: probes == runs issued (823 753 / 729 444 / 8 380 079), translations
completed == probes submitted, releases == lines issued, `ready_peak ≤ capacity`, overlap never
exceeds target-outstanding time, and the occupancy/wait identity above.
`./bin/dx100_tests`: **4 735 checks, 0 failures.**

## 4. The seven questions

**1. Does independent translation admission eliminate or reduce the P2 cohort-fragmentation
penalty?**
On the translation side, yes: translation now sees the ascending-line stream and does P0's exact
walk work (107 439 vs 107 399; 186 922 vs 186 923; 1 313 204 vs 1 313 192), where P2's row order
imposed its own stream. On the DRAM side the fragmentation is still there and worse —
459 k / 494 k / 5.61 M cell changes against P2's 285 k / 346 k / 2.03 M — because the release order
is dictated by translation completion, not by the window.

**2. Can the DRAM scheduler recover cross-page row locality while translation is in progress?**
No, not with this buffer size and this production rate: 40 % / 0.2 % / (negative) of the ideal
gain. What it recovers is P0's locality, not P2's. The minimising policy is correct — it always
releases the most local translated line — but the set it chooses from holds 1.1–33.8 lines
(≈ 1–2 rows), so the choice carries no cross-page information.

**3. How much translation/DRAM overlap is achieved?**
Effectively all of it: 98.7 / 99.4 / 100.0 % of target-outstanding time overlaps in-flight
translation, translation is live 99.5 / 99.8 / 100 % of the run, and G6A's serial DRAM phase
(17.7 / 13.9 / 15.9 % of its cycles) disappears — the difference in totals is exactly that
(−17.3 / −13.7 / −16.1 %).

**4. Does the ready buffer become a bottleneck?**
No. Backpressure ≤ 0.14 % of cycles and mean occupancy 1.1–33.8 of 4096. The buffer is idle, not
full: the scheduler spends 96.8–99.0 % of cycles with nothing translated to release.

**5. Is performance better than G6A, P0, and P2?**
Better than G6A (all three). Worse than P2 (+8.2 / +21.5 / +8.0 %). Worse than P0
(+55.2 / +38.5 / +87.7 %). G6B fixes G6A's structural mistake but does not beat the published
policy, because it inherits the translation-throughput limit and does not recover locality.

**6. How sensitive are results to buffer capacity?**
At most 0.3 % in cycles, and zero in row-hit rate — because capacity is never the binding
constraint:

| capacity | Orkut cycles | vs P0 | row hits % | peak occ | backpressure |
| --- | --- | --- | --- | --- | --- |
| 64 | 25 987 346 | +55.48 % | 60.47 | 64 | 0.58 % |
| 256 | 25 959 915 | +55.31 % | 60.47 | 256 | 0.46 % |
| 1 024 | 25 940 047 | +55.19 % | 60.47 | 1 024 | 0.34 % |
| 4 096 (baseline) | 25 938 106 | +55.18 % | 60.47 | 4 096 | 0.14 % |
| 16 384 | 25 938 106 | +55.18 % | 60.47 | 6 985 | 0.00 % |

LiveJournal behaves identically (39 211 531 → 39 200 163, row hits 46.13 % at every capacity,
peak 64→7 499). Provisioning more than ~1 K entries buys nothing; 64 entries costs 0.19 %. The
*achieved* occupancy (12–34) is set by translation production, not by provisioned capacity, which
is why the locality result does not move.

**7. Does the architecture preserve forward progress under real translation?**
Yes. All 15 cells plus 10 capacity cells completed, every window retired, no DNF, no deadlock,
conservation clean, and the release identity (`releases == lines issued == probes`) holds with real
page walks active throughout.

## 5. Limitations

* Translation admission is strictly the window's stored order. The measured consequence (1.06–1.93
  of 4 dTLB MSHRs busy, 52–72 % walker utilisation, versus P0's 2.13–3.24 and 96–99 %) is reported
  as a finding about *this* admission order; no page-interleaved variant was built or measured.
* One buffer size in the baseline matrix (4096) plus the sweep above; no adaptive or per-bank
  partitioning was attempted.
* 16 K window only, as the brief specifies for the first pass.
* The DRAM model here is a read-only fill path: no store stream, no write counter, so DRAM writes
  are absent by workload construction rather than measured zero.
* Ideal-P2 removes the dTLB/L2TLB/walk path entirely; it bounds locality, it is not a realizable
  TLB model.
* The ogbn trace is the 100-D train gather whose P0/P2 numbers are new measurements; only
  Orkut/LiveJournal have external published anchors.
* `bulk_sched_scan` charges one step per release for the priority-queue operation; this is a
  modelled cost of the same shape as P2's per-cohort charge, not a cycle-accurate model of a
  sorter.
* Simulator experiment only: no Vortex RTL change, no physical-GPU claim.

## 6. Reproduction

```
cd DX100_MEM
make -C sim -j8
(cd sim && ./bin/dx100_tests)          # 4 735 checks, 0 failures
./build/wm/m23g6b_run.sh all 5         # 15 cells -> build/wm/m23g6b/*.txt
./build/wm/m23g6b_run.sh caps 2        # 10 capacity cells
python3 tools/m23g6b_extract.py        # checks + summary.csv
```

Hashes (sha256) of the artifacts that define this experiment:

```
adf0dd576e22a57a7d0baa146d33e6b80e205165c20eca825bf830d456ff5796  sim/src/bulk.cpp
adf43b869e9940f3d1eb9787098fa07b585cec06fa02433259d4d5a2d7052c48  sim/src/memory.cpp
54480a83d3af49311da32696c07e95b92375516ef048b68660cbf9a67a9c618f  sim/src/sim.hpp
4747a2aed266d540f0807f02c441d6001c7cdfa3eb061c0939bf8f5de1551003  sim/src/util.cpp
abc91fab3ba094c77800438f0a63847afecd1a1248a05229237f1295b38b4b4b  sim/configs/m23g6b_base.cfg
a489a055a8e9f6ebb9701fa06363df3e41f5eaded4430c8b0f5012b1ae40c46f  build/wm/m23g6b_run.sh
ff5585c0ece9c33e2981562996a910c8eb58ef5765714407c4872f7e74ff6121  tools/m23g6b_extract.py
```

`memory.cpp` is unchanged from M23-G6A because G6B reuses its `xlate_only` / `pretranslated`
hooks verbatim. `bulk.cpp`, `sim.hpp` and `util.cpp` have changed since the G6A report, so that
report's hashes identify its own snapshot, not the current tree; its measured cells are
reproduced exactly by this milestone's P0/P2/G6A arms. Cell-log hashes:
`build/wm/m23g6b/hashes.txt`.

## 7. What this establishes for the milestone

1. **Streaming decoupling is implementable and live.** Translation and DRAM run concurrently with
   no barrier, no phase state and no deadlock; translation is active in 99.5–100 % of the run and
   98.7–100 % of DRAM work overlaps it. G6A's serial tail is gone (−13.7 to −17.3 % cycles).
2. **Its ready buffer is not the constraint, and a bigger one would not help.** Capacity is inert
   from 1 024 to 16 384 entries (≤ 0.02 % in cycles, zero in row hits), because achieved occupancy
   is 1.1–33.8 entries — set by translation throughput, not by provisioning.
3. **Locality is bounded by how much translated work exists at once, not by the ordering rule.**
   The minimising release is exactly right and still recovers ~none of P2's cross-page locality
   (40 % / 0.2 % / negative of the ideal gain), because it chooses from one to two rows.
4. **The remaining gap to P0 is translation admission, not DRAM.** G6B does P0's exact walk work
   but keeps only 1.06–1.93 of 4 dTLB MSHRs busy, while P0 defers a blocked page cohort and keeps
   2.13–3.24 busy (96–99 % walker utilisation against 52–72 %). That is the one measured
   difference that explains why streaming beats G6A and P2 but not P0.
5. **Consequence for the design space.** G6A showed a barrier cannot be paid for; G6B shows that
   removing it recovers the overlap but leaves the DRAM scheduler starved of material to order.
   Any policy that wants both P0's translation throughput and P2's row locality has to fix
   translation *admission* (page-level concurrency) rather than widen the buffer or re-time the
   release. That is a separate experiment and is deliberately not attempted here.
