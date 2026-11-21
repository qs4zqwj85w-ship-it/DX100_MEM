# M23-G6A — Translation first, DRAM second

**Status: complete. 12 cells, 0 failures, 0 conservation violations, published P0/P2 anchors
reproduced to the cycle.**

The experiment asked whether separating virtual-memory translation from physical DRAM
scheduling can keep translation progress while recovering the DRAM locality that P2 shows
under ideal translation.

**Answer.** The DRAM half works: released in P2's (bank,row) order after a full translation
barrier, the row-hit rate matches or slightly exceeds ideal-translation P2 on all three
workloads (+0.30 pt Orkut, +0.26 pt LiveJournal, −1.84 pt ogbn), so the cohort fragmentation
that cost P2 under real translation disappears. The execution-time half fails: the barrier
un-hides the page-walk cost instead of overlapping it, and total time is **0.53× / 0.62× /
0.45× of P0** (Orkut / LiveJournal / ogbn-products train), with DRAM 96–99 % bank-idle during
82–86 % of every run that is spent translating. The workloads are page-walk bound — P0 already
runs at 96–99 % walker utilization — so a phase that cannot overlap walks with DRAM service
pays the whole walk bill serially.

No frozen baseline was modified, no translation or DRAM parameter was changed for this policy,
and no design change was made in response to these results.

---

## 1. What was built

`bulk_org = 6`, a new experimental issue policy on the existing fused window. One window, two
strictly ordered phases.

**Phase 1 — translation.** On adoption, the window's line runs are translated *all* at once, in
the order the window stores them (ascending line). Each run's probe is a translation-only
request (`Req::xlate_only`): it walks the real dTLB → L2 TLB → walker path and retires the
cycle its dTLB lookup succeeds, *before* any cache probe, so nothing it asked for can reach the
DRAM. The physical line it produced is buffered with the cycle it arrived. No target-data
request is issued during this phase.

**Phase 2 — DRAM.** When the last probe returns, every run holds its physical line. The runs are
then released through the **unchanged P2 scheduler**: same `(bank,row)` cohort unit, same
deferral, same `eng_issue` budget, same DRAM controller, same FR-FCFS. The only difference from
P2 is that no run translates again — `Req::pretranslated` carries `pa_line`, and `submit`
installs the request at the hierarchy directly (no probe, no MSHR, no L2 TLB lookup, no walk).

The barrier is structural, not a timer. A window's targets cannot be released before its own
last translation returns, and the engine adopts a window for issue only when none is issuing
(`iss_ < 0`), so no other window can be translating while this one drains. What still overlaps
is window *storage* (the next window admits elements while the current one issues) and the
descriptor/index streams that exist in every configuration — neither is translation of a
window's targets against that window's DRAM traffic.

### Implementation

| file | change |
| --- | --- |
| `sim/src/sim.hpp` | `Config::g6a_pa_capacity`; `Req::xlate_only`, `Req::pretranslated`; `MemorySystem::Xlate` hand-off queue + `take_xlate`; `Batch::fpa/ftr/ftr_gen/ftr_done/g6a_phase/tr_next/tr_done/tr_start/tr_last_done/dr_start/busy_tr_start`; 15 `BulkStats::g6a_*` counters |
| `sim/src/memory.cpp` | `submit`: `pretranslated` requests enter at stage 2 with their buffered PA; `tick` stage 1: an `xlate_only` probe retires into the hand-off queue instead of advancing to stage 2 |
| `sim/src/bulk.cpp` | `win_issue_g6a` (the two-phase driver); routing at the head of `win_issue`; `win_emit_fused` carries the buffered PA and accounts the post-translation wait; ordering: `bulk_org == 6` uses P2's comparator (the P3 tiebreak is now conditioned on `org == 3` instead of `org >= 3`) and P2's `(bank,row)` cohort unit |
| `sim/src/util.cpp` | parse/echo `g6a_pa_capacity` |

Ordering note: `bulk_org = 6` deliberately reuses `win_order_fused`/`win_open_cohort_fused` with
the *P2* keys rather than a new comparator, so phase 2 is the existing organization, not a
re-implementation of it. That one predicate change (`org >= 3` → `org == 3`) is the only edit
in a shared ordering path, and it is inert for P0/P2/P3 — verified by the anchors below.

Translation work is *unchanged by construction*: one probe per line run, exactly as P0 issues,
through the same dTLB/L2TLB/walker structures, bounded by the same `dtlb_mshr = 4` /
`dtlb_parked = 2` / `l2tlb_mshr = 8` / `walkers = 2` / `walk_latency_per_level = 150`. A
translation-only probe holds no residency slot, so phase 1 adds no outstanding-request capacity
beyond the dTLB's own MSHRs.

Nothing was adjusted after the first results were seen.

## 2. Configurations and cells

`sim/configs/m23g6a_base.cfg` is `configs/m23_dx16k.cfg` plus `g6a_pa_capacity = 65536`,
`bulk_max_elems = 16777216` (trace validation bound), `max_cycles = 3e9` (DNF guard) and an
explicit `bulk_org`. `R = 128`, L1 256×4, L2 16384×8, `l2_mshr = 64`, dTLB 16×4 with 4 MSHRs,
L2 TLB 512×4 with 8, 2 walkers, 2 banks × 4096 rows, `t_hit 14` / `t_miss 26`, fused window,
`page_sched = 1`, `page_cohorts = 2`, `org_overlap = 1`, 16 K window. All four arms run on this
one file; only `bulk_org` and `ideal_translation` move, on the command line.

```
./build/wm/m23g6a_run.sh all 4          # 12 cells
python3 tools/m23g6a_extract.py         # checks + tables
```

| workload | trace | accesses |
| --- | --- | --- |
| Orkut BFS | `build/wm/m23_orkut_bfs.bin` | 1 200 003 |
| LiveJournal gather | `build/wm/m23_livejournal_bfs.bin` (SNAP soc-LiveJournal1, 1 048 576-vertex subset) | 1 200 003 |
| ogbn-products train gather | `build/wm/m23_ogbn_train_100d.bin` | 8 393 035 |

The ogbn trace is the 100-dimensional train-split gather built in M23-G5A (7 lines/row,
selected-degree mean 141.9); it had not been run before, so its P0/P2 numbers are first measured
here. Cell logs: `build/wm/m23g6a/*.txt`, hashes in `build/wm/m23g6a/hashes.txt`, table in
`build/wm/m23g6a/summary.csv`.

**Anchors.** P0 and P2 must be the published cycles for "preserved" to mean anything, so the
extractor compares them exactly:

| workload | policy | published | measured | |
| --- | --- | --- | --- | --- |
| Orkut | P0 | 16 714 538 | 16 714 538 | OK |
| Orkut | P2 | 23 965 633 | 23 965 633 | OK |
| LiveJournal | P0 | 28 309 390 | 28 309 390 | OK |
| LiveJournal | P2 | 32 260 832 | 32 260 832 | OK |

(P0 also matches the M23-G3 published Orkut ideal-translation P0 of 6 077 523 via the ideal arm:
6 076 339 for ideal-P2, −0.02 %, i.e. the published "P2 ≈ 0.00 % under ideal translation".)

## 3. Results

### 3.1 Execution time

| workload | P0 | P2 | G6A | ideal-P2 |
| --- | --- | --- | --- | --- |
| Orkut | 16 714 538 | 23 965 633 (+43.38 %) | **31 373 796 (+87.70 %)** | 6 076 339 (−63.65 %) |
| LiveJournal | 28 309 390 | 32 260 832 (+13.96 %) | **45 440 974 (+60.52 %)** | 6 848 866 (−75.81 %) |
| ogbn train | 199 882 548 | 347 389 697 (+73.80 %) | **446 942 992 (+123.60 %)** | 74 854 523 (−62.55 %) |

Speedup versus P0: **0.533× / 0.623× / 0.447×**. G6A is slower than P0 on every workload and
slower than P2 on every workload.

### 3.2 Translation: the same work, different hiding

| workload | cell | probes | dTLB misses | walks | PTE fetches | walker-busy | mean TLB lat |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Orkut | P0 | 898 877 | 192 014 | 107 399 | 214 798 | 32 219 700 | 88 |
| Orkut | P2 | 898 904 | 192 050 | 107 444 | 214 888 | 32 233 200 | 83 |
| Orkut | G6A | 898 948 | 192 033 | 107 401 | 214 802 | 32 220 300 | 78 |
| LiveJournal | P0 | 804 542 | 317 832 | 186 923 | 373 846 | 56 076 900 | 187 |
| LiveJournal | G6A | 804 227 | 317 675 | 186 856 | 373 712 | 56 056 800 | 156 |
| ogbn train | P0 | 8 911 635 | 2 588 592 | 1 313 192 | 2 626 384 | 393 957 600 | 94 |
| ogbn train | G6A | 8 911 657 | 2 595 184 | 1 316 776 | 2 633 552 | 395 032 800 | 89 |

Translation work is identical across P0, P2 and G6A to within 0.3 % (walks −0.00 % / −0.04 % /
+0.27 %), which is what the design intends: the barrier moves translation, it does not change
it. `tlb0_mshr_max = 4` in every real-translation cell — the dTLB MSHRs are saturated in all
policies.

The thing that differs is *walker utilization over the run*:

| workload | P0 walker util | G6A phase-1 walker util | G6A phase-1 / pure walk time |
| --- | --- | --- | --- |
| Orkut | 96.4 % | 62.4 % | 1.60× |
| LiveJournal | 99.0 % | 71.6 % | 1.40× |
| ogbn train | 98.5 % | 52.5 % | 1.91× |

P0 is a page-walk-bound workload: its whole run is almost exactly the two walkers' busy time.
The barrier's phase has only its own window's probes to feed the walkers with, and the probe
order is ascending line, so consecutive runs share a page and `dtlb_parked = 2` limits waiters
per MSHR — the port blocks on the page it is currently filling instead of moving to another
one. That is why phase 1 lasts 1.4–1.9× the pure walk time and why the measured dTLB refusal
rate across phase-1 cycles is 99.3 / 99.6 / 99.7 %.

### 3.3 DRAM organization versus ideal-translation P2

| workload | cell | row hits % | row conflicts | bank util % | bytes/cycle | DRAM reads |
| --- | --- | --- | --- | --- | --- | --- |
| Orkut | P0 | 60.43 | 257 698 | 36.53 | 2.494 | 651 309 |
| Orkut | P2 | 60.47 | 257 473 | 25.47 | 1.739 | 651 339 |
| Orkut | **G6A** | **60.83** | 254 992 | 19.40 | 1.328 | 651 022 |
| Orkut | ideal-P2 | 60.53 | 254 024 | 99.22 | 6.779 | 643 579 |
| LiveJournal | P0 | 46.12 | 375 629 | 25.20 | 1.576 | 697 219 |
| LiveJournal | P2 | 50.84 | 342 940 | 21.51 | 1.384 | 697 595 |
| LiveJournal | **G6A** | **52.89** | 328 770 | 15.09 | 0.983 | 697 898 |
| LiveJournal | ideal-P2 | 52.63 | 327 392 | 99.32 | 6.459 | 691 164 |
| ogbn train | P0 | 62.70 | 3 701 864 | 45.87 | 3.178 | 9 925 604 |
| ogbn train | P2 | 66.21 | 3 354 401 | 25.80 | 1.829 | 9 927 758 |
| ogbn train | **G6A** | **74.91** | 2 492 609 | 18.91 | 1.423 | 9 935 534 |
| ogbn train | ideal-P2 | 76.75 | 2 069 444 | 99.81 | 7.609 | 8 899 407 |

Row-hit rate is `row_hits / (row_hits + row_conflicts)`; bank utilization is
`dram_busy_cycles / (2 × cycles)`. The DRAM model in these workloads is a read-only fill path —
there is no store stream and the simulator keeps no write counter in this configuration, so
writes are 0 by construction of the workload, not a measured zero.

Two things are true at once and should not be conflated:

* **The organization is recovered.** G6A's row-hit rate beats P2's under real translation by
  +0.36 / +2.05 / +8.70 pt and is at or above ideal-P2 (+0.30 / +0.26 / −1.84 pt). On Orkut and
  LiveJournal the barrier's single ordered pass over the whole window is *cleaner* than P2's own
  two-cohort interleaving, which is why it slightly exceeds the ideal reference. On ogbn it
  reaches 74.91 against the ideal 76.75, i.e. 87 % of the ideal gain.
* **The instance of DRAM work is unchanged and the time it is spread over doubles.** Absolute DRAM
  reads are identical across P0/P2/G6A (651 k / 697 k / 9.93 M; ideal is lower only because it
  fetches no page tables), and `dram_busy_cycles` is 12.17–12.21 M in every Orkut cell — but
  spread over 16.7 M cycles in P0 and 31.4 M in G6A. Bank utilization therefore *falls*.

### 3.4 Where the time goes

| workload | windows | phase-1 cycles | % run | phase-2 cycles | % run | phase 1 / window | phase 2 / window | dTLB stall, % of phase 1 | DRAM bank-idle in phase 1 | mean translated-wait-for-DRAM |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Orkut | 74 | 25 804 721 | 82.25 | 5 556 547 | 17.71 | 348 712 | 75 088 | 99.31 | 97.79 | 228 320 |
| LiveJournal | 74 | 39 115 116 | 86.08 | 6 315 516 | 13.90 | 528 583 | 85 345 | 99.56 | 98.52 | 344 257 |
| ogbn train | 513 | 375 985 519 | 84.12 | 70 946 413 | 15.87 | 732 916 | 138 297 | 99.67 | 96.36 | 435 310 |

For scale, P0's window lifetime is 225 872 cycles on Orkut: G6A replaces it with 348 712 cycles
of translation followed by 75 088 cycles of DRAM.

The barrier itself adds no measurable lag: `bulk_g6a_barrier_cycles = 0` in every cell (the
release begins in the tick the last translation lands; ≤ 1 cycle per window). The cost is not a
post-barrier delay, it is the serialization *before* it.

Other window overheads (means over the run's windows):

| workload | cell | fill span | translation | scheduling | drain |
| --- | --- | --- | --- | --- | --- |
| Orkut | P0 | 11 689 | — | 1 436 | 168 |
| Orkut | G6A | 6 933 | 348 712 | 3 851 | 1 645 |
| LiveJournal | P0 | 9 875 | — | 2 512 | 572 |
| LiveJournal | G6A | 6 979 | 528 583 | 4 681 | 1 704 |
| ogbn train | P0 | 10 220 | — | 2 542 | 84 |
| ogbn train | G6A | 7 332 | 732 916 | 3 966 | 1 230 |

Fill span shortens because the next window fills while the current one translates (storage only).
Scheduling cycles rise 2.0–2.7× because phase 2 runs the P2 cohort scheduler at all. Drain rises
9.8×/3.0×/14.6× because a whole window's fanout is released in one burst. All three are
rounding error against phase 1.

A second-order effect worth recording: the burst release oversubscribes the L2 MSHR pool
(`l2_mshr = 64`, residency budget `R = 128`), so re-probes rise from 51 M to 302 M on Orkut,
14 M to 345 M on LiveJournal and 45 M to 3 919 M on ogbn. L2-MSHR-busy cycles rise 2.8× / 4.9×
/ 5.4×. This does not change DRAM order (the cohort scheduler still serializes issue) but it is
part of why phase 2 is not simply "ideal-P2 DRAM time".

### 3.5 Physical-address buffer

| workload | capacity provisioned | peak occupancy | required (peak need) | modelled cost |
| --- | --- | --- | --- | --- |
| all three | 65 536 entries | 16 384 | 16 384 | 16 B/entry → 1 MiB provisioned, 256 KiB used |

Peak occupancy equals the full run count of a window (16 384 runs at a 16 K window, dedup
leaving ≤ 1 run per element): with a full barrier the buffer holds the entire window before the
first release. Required capacity is therefore *one entry per run of the largest window*,
16 384 × 16 B = 256 KiB, and it scales with `batch_window`, not with the working set. The simulator's own realization additionally keeps
`ftr_gen`/`ftr_done` per run (25 B/run realized, 400 KiB at 16 K).

### 3.6 Conservation and correctness

`tools/m23g6a_extract.py`, 12/12 cells: `conservation_ok = 1` and `conservation_violations = 0`
everywhere; `bulk_residency_violations`, `bulk_addr_mismatch`,
`bulk_issue_monotonic_violations` all 0; `dispatch_partition_ok = 1`;
`bulk_tgt_req_gen == bulk_tgt_req_done == bulk_tgt_req_issued == bulk_lines_issued`;
`bulk_elems_done == bulk_elems_total` (1 199 876 / 1 199 762 / 8 393 035); no cell hit
`max_cycles`. G6A-specific: probes == runs issued (823 827 / 729 018 / 8 380 105), translations
completed == probes submitted, runs released == lines issued, `pa_need_max ≤ pa_capacity`, and
`phase-1 + phase-2 ≤ run cycles` in every cell. `./bin/dx100_tests` from `sim/`: **4 735 checks,
0 failures.**

## 4. The five questions

**1. Does translation-first scheduling eliminate the P2 cohort-fragmentation penalty?**
Yes. With every run's address already in hand, the release queue never mixes a translated run
with an untranslated one, so P2's organization runs over a complete set: row hits 60.83 / 52.89
/ 74.91 % versus P2's 60.47 / 50.84 / 66.21 %. On Orkut and LiveJournal it is also above
ideal-P2, because a single ordered pass over the whole window is cleaner than P2's two-cohort
interleave.

**2. Does it recover P2's row-hit improvement under real translation?**
Yes, and more than P2 itself: +0.40 pt over P0 on Orkut (P2: +0.04), +6.77 pt on LiveJournal
(P2: +4.72), +12.21 pt on ogbn (P2: +3.51). In absolute row-hit terms G6A sits +0.30 / +0.26 pt
above ideal-P2 on the two graphs and −1.84 pt below it on ogbn (87 % of the ideal gain there).
DRAM conflicts fall below ideal-P2 on two of three workloads.

**3. Does the full-window barrier introduce excessive idle time or batch latency?**
Yes, decisively. Phase 1 is 82–86 % of every run and the DRAM is 96–98 % bank-idle during it;
the dTLB refuses a ready probe in 99.3–99.7 % of those cycles, and per-window latency rises from
226 k to 424 k cycles (Orkut). Translation is not merely delayed behind DRAM as in P0, it is
*removed* from the overlap that made P0's walk-bound cost affordable: P0 achieves 96–99 %
walker utilization, G6A's phase 1 achieves 52–72 %.

**4. How large must the physical-address buffer be?**
One entry per line run of the window — 16 384 entries (16 B each, 256 KiB) at a 16 K window; the
peak is set by a full window (mean admitted window is 16 214 / 16 213 / 16 360 elements), and
32 K would need 512 KiB. It is a full-window snapshot, not a queue: occupancy at the barrier
equals the whole window.

**5. Does the architecture improve total execution time, rather than merely improving DRAM
counters?**
No. Row hits and conflicts improve exactly as predicted by the ideal-translation reference, and
total time regresses by +87.7 % / +60.5 % / +123.6 % versus P0 (0.53× / 0.62× / 0.45×). This is
the answer to the question the milestone exists for: a sequential decoupling recovers the DRAM
locality but loses the translation/DRAM overlap, and on page-walk-bound workloads that overlap
is worth far more than the row-hit gain.

## 5. Limitations

* The barrier is a *full* window barrier by instruction; no partial-barrier or multi-window
  variant was measured, and none was designed after seeing these results.
* Only the 16 K window was run. The brief makes 8 K/32 K conditional on promising results, and
  the results are not promising: the phase-1 share is set by walk cost per window and would be
  unchanged or worse at 32 K (buffer and phase both scale with the window).
* The ogbn trace is the 100-D train-split gather, 7× the accesses of the two graphs; its P0/P2
  numbers are new measurements, not published ones, so only the Orkut/LiveJournal anchors are
  external.
* `ideal_translation = 1` removes the dTLB/L2TLB/walk path entirely; ideal-P2 is a bound on
  locality, not a model of a realizable TLB.
* DRAM writes are absent from these workloads and from this configuration's counters; the byte
  figures are read traffic plus page-table reads.
* `lat_cache_mean` for G6A intentionally spans both phases (a request's `gen` stays at its
  phase-1 submission), so it is 210 002 / 313 027 / 410 317 cycles — the post-translation wait
  inside the barrier, not a cache latency.
* This is a simulator experiment. No Vortex RTL was changed and no physical-GPU performance is
  claimed or implied.

## 6. Reproduction

```
cd DX100_MEM
make -C sim -j8
(cd sim && ./bin/dx100_tests)                  # 4 735 checks, 0 failures
./build/wm/m23g6a_run.sh all 4                 # 12 cells -> build/wm/m23g6a/*.txt
python3 tools/m23g6a_extract.py                # checks + summary.csv
```

Hashes (sha256) of the artifacts that define this experiment:

```
287981d27442b3c362226ad9ae44196fbebc7085bc0c1753f2cf3f93e9d4127e  sim/src/bulk.cpp
adf43b869e9940f3d1eb9787098fa07b585cec06fa02433259d4d5a2d7052c48  sim/src/memory.cpp
36db5e0e3a5a5de2dcff2aa9e5497bb5bd28df07f5afcd8ebc6753ef9faeb3fd  sim/src/sim.hpp
e1ab7fefcf46b84bbcd02730d3442a2a4b134ba98e3d5e010db16a9f2f525167  sim/src/util.cpp
9ff4bf813a222c999494e57422297e39f0588c827ed8eb6287645142472829e7  sim/configs/m23g6a_base.cfg
324ddd9795a315730d08e22ee44aad170b0520718a57f33f2b39b0e3e0d5849e  build/wm/m23g6a_run.sh
861cb94be641509a879a12cc413a38fbda5e834ef95a810eb78660f08b52d159  tools/m23g6a_extract.py
```

Cell-log hashes: `build/wm/m23g6a/hashes.txt`.

Note: M23-G6B later extended `bulk.cpp`, `sim.hpp` and `util.cpp` with its own policy
(`bulk_org = 7`), so the three source hashes above identify the G6A-era snapshot rather than the
current tree. The G6A cells are reproduced to the cycle by that milestone's P0/P2/G6A arms
(`docs/m23-g6b-streaming-translation-dram-decoupling.md` §2), which is the check that the addition
perturbed nothing. `memory.cpp` is unchanged.

## 7. What this establishes for the milestone

1. Translation-first / DRAM-second is implementable on the frozen model without touching it: the
   translation stream is byte-for-byte the same work (same probes, walks, PTE fetches, MSHRs) as
   P0's, and the DRAM organization is P2's own code.
2. The DRAM locality P2 shows under ideal translation is *fully available* under real
   translation once fragmentation is removed — at or above the ideal reference on both graphs and
   87 % of its gain on ogbn — so the locality
   gap in M23-G3/G4B was a scheduling artifact, not a translation artifact. That is a positive
   result for the P2 organization itself.
3. A full barrier cannot pay for that locality: the cost is the un-hidden walk time, and these
   workloads are walk-bound (P0 walker utilization 96–99 %). G6A's phase 1 does not even reach
   that walker utilization, because a single window's press of one page per MSHR cannot keep two
   walkers fed (52–72 %).
4. Consequently, any successor can only win by overlapping the two phases. Whether that is
   possible without reintroducing fragmentation is exactly the question G6B exists to answer;
   nothing here should be read as having tried it.
