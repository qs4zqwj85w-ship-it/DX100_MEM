# M23-G6I — Continuous translation and global DRAM scheduling

**Question.** Is translation decoupled from DRAM scheduling, so that completed translations from any
virtual page enter one shared physical-address pool and the DRAM organizer consumes from the combined
population?

**Answer.** Yes — and it already was. The shipped streaming pipeline (`bulk_org = 8`, M23-G6C, which is
the arm every G6H baseline was measured on) runs translation admission and DRAM release as three
independent blocks in the same cycle, with one shared ready pool and one coupling: the pool's capacity.
Measured on Orkut S1: **255.9 distinct virtual pages in the pool at an average instant (803 peak)**,
**689 distinct physical `(bank,row)` cells**, 8/8 page descriptors translating simultaneously,
**70.3 % of releasing cycles have a probe in flight**, and **12.9 % of consecutive releases come from a
different page**. No page-completion barrier exists on that path.

The one place a translation→DRAM barrier *did* exist is M23-G6A (`bulk_org = 6`), the two-phase arm that
G6B replaced; §1.3 shows the exact lines. Nothing in the shipped path waits for a page.

Doubling the pool to 8 192 raises occupancy 13 % and cells 8 %, and changes cycles by **+0.001 %**
(Orkut), **−0.019 %** (LiveJournal) and **+0.040 %** (ogbn train). The reason is architectural and
measured in §5: with a 4 KiB page over 1 KiB rows, a page owns exactly four physical cells and never
shares a row with another page inside an 8 MiB working set — so page boundaries were never cutting rows.
What leaves a cell at 2.97 runs is translation *depth*, not page coupling.

Everything below is measured with the frozen G6D–G6H configuration; only `g6b_pa_capacity` and the new
diagnostic knob move. Historical artifacts were read, never written.

## 0. Verdict in three lines

| Claim | Evidence |
|---|---|
| Translation was already independent of DRAM release. | source: three disjoint blocks in `win_issue_g6b`, no DRAM state read by admission (§1); measurement: 255.9 pages/pool, 70.3 % of release cycles with probes in flight (§4, §6) |
| The pool already accumulates across pages, and its only backpressure is capacity. | 803 pages and 2 059 cells in one 4 096-entry pool; pool full 8.1 % of cycles; no per-page queue or limit exists (§2, §4) |
| A bigger pool does not buy time or row hits. | 8 192 entries: +13.2 % occupancy, +4.5 % runs per cell, row hit 60.5290 % → 60.5284 %, cycles +0.001 % (§5, §6) |

## 1. Was there a page→DRAM dependency? (source audit)

### 1.1 The shipped pipeline, per cycle

`BulkEngine::win_issue_g6b` (`bulk.cpp`) is the whole path for `bulk_org = 7/8`. Each tick it runs
three independent blocks on the window under issue:

```cpp
// (1) translations that landed this cycle join the ready buffer
while (mem_.take_xlate(x)) { …ready_push(b, b.fpos[li]); }

// (2) translation admission: page-aware, capacity-bounded
if (g6c) win_admit_g6c(cycle, b);      // <= page_cohorts page descriptors, oldest first
else { while (tbudget > 0 && b.g6b_tr_next < n) { if (ready.size()+pend >= cap) break; … } }

// (3) DRAM release: the most local translated line that exists right now
while (budget > 0 && !b.ready.empty()) { pos = b.ready.front(); … win_emit_fused(b, li, cycle); }
```

* Block (2) reads exactly four things: the window's run array, the cohort descriptors, the dTLB
  (through `mem_.submit`), and `held = ready.size() + pend` against `cap`. It reads **nothing** about the
  DRAM: not `pops`, not `b.pending`, not `b.g6b_cell_valid`, not the open row.
* Block (3) reads the pool and the physical key. It writes nothing admission reads.
* The order of the blocks inside a cycle is fixed, but neither block can *block* the other: (2) has no
  `break` on (3) and (3) has none on (2).

So the single dependency between the stages is the capacity rule at the top of (2):

```cpp
const u64 pend = static_cast<u64>(g6c ? b.g6c_submitted : b.g6b_tr_next) - b.g6b_tr_done;
const u64 held = static_cast<u64>(b.ready.size()) + pend;   // pool + in flight
if (more && held >= cap) ++st_.g6b_backpressure;            // and the admitters stop
```

That is exactly the backpressure the brief asks for: finite storage, applied to admission, released as
soon as the pool drains.

### 1.2 Multiple pages at once

`win_admit_g6c` keeps `Config::page_cohorts` page descriptors open. Each cycle it picks the **oldest
open page with a runnable cursor**, submits up to `eng_issue` probes from it, and on a dTLB refusal parks
that page at `cycle + park_retry` and immediately continues with the next open page:

```cpp
u32 idx = … first open cohort with pos < end and resume_at <= cycle …
if (idx == 0xffffffffu) { if (g6c_open_cohort(b)) continue; … break; }
if (!mem_.submit(r, cycle)) { … c.resume_at = cycle + park_retry; refused_idx = idx; continue; }
```

There is no "one page at a time" rule, and no completion handshake between a page's DRAM runs and the
next page's probes. Measured: `pages_open_max = 8` (the configured `page_cohorts`, i.e. all of them),
mean 5.66 busy descriptors per sampled cycle on Orkut, and `bulk_g6c_defer_then_other_admit = 545 271`
events where a refused page's cycle was spent admitting a *different* page.

### 1.3 Where a barrier really existed

M23-G6A (`bulk_org = 6`) is the one arm with a hard phase barrier, and its own source says so:

```cpp
// The barrier is structural, not timed. A window's targets cannot be released
// before its own last translation returns … (win_issue_g6a)
```

That is a *window* barrier (phase 1 of a window must finish before phase 2 starts), and M23-G6B replaced
it with the pool. Every G6H and G6I baseline runs `bulk_org = 8`, so no such barrier is present in the
numbers here.

Two limits do remain and are structural, not page-related: **one window issues at a time**
(`if (iss_ < 0) adopt a sealed window`), and **`page_cohorts` pages may be translating at once**. Both are
configured resources the brief says to preserve, and both are measured below (74 windows, 8 cohorts).

## 2. Architecture before and after

What the brief describes (page-coupled, not what the code does):

```
window ──► page i translate ──► page i DRAM ──► page i+1 translate ──► page i+1 DRAM ──► …
                 ^                     |                   (waits for the previous page)
                 └─────────────────────┘
```

The pipeline that was measured (G6B/G6C, unchanged by G6I):

```
                ┌──────────────── translation admission (2) ────────────────┐
window runs ────►│ 8 page descriptors, oldest-runnable first, dTLB MSHRs    │──► probes ─┐
 (16 384 elems)  │ refusal => park + try another page (no page barrier)      │            │
                └──────────────────────────────────────────────────────────┘            ▼
                                                                              dTLB / L2 TLB / walkers
                                                                                         │
   ┌──────────────── pool (1) ◄──────────────────────────────────────────────────────────┘
   │  one shared 4 096-entry (or 8 192) physical-address pool, any page
   │  +----------------------------------------------------------------+
   │  | run | physical line | translation cycle |  pool position (P2 rank) |
   │  +----------------------------------------------------------------+
   └───────▲──────────────────────────────────────────────┬───────────────────────────────┘
   insert on  ▲                                          │ release (3): global min (row,bank,line)
   translation│                                          ▼
   completion │                                   L1 → L2 → L2 MSHR → DRAM queue → FR-FCFS
              └──────────── capacity only ──────────────  (no page completion, no window reset)
```

The two stages share storage and nothing else: a probe's completion inserts a run, a release removes the
run with the globally smallest physical key. Neither waits for the other's progress.

## 3. Exact source-level changes

No pipeline behaviour was changed, because the dependency the brief posits does not exist on this path.
The changes are the instrumentation needed to *prove* it, plus one knob:

| File | Change |
|---|---|
| `sim.hpp` | `Config::g6i_diag` (0/1) and `Config::g6i_sample_period` (512 cycles); 24 `BulkStats::g6i_*` counters; `BulkEngine::note_g6i_release` / `g6i_sample` declarations; sample scratch (VPN set + per-cell page/count/multi arrays) |
| `bulk.cpp` | the two diagnostic functions; one hook per release (`if (cfg_.g6i_diag) note_g6i_release(...)`); one hook per cycle for the exact overlap counters; one sample call at the end of `win_issue_g6b`; 24 exports at both `run_bulk` sites |
| `util.cpp` | parse `g6i_diag` / `g6i_sample_period`; echo `g6i_diag` only when set, so every existing log's config block stays byte-identical |
| `tests/tests.cpp` | `test_m23_g6i_continuous_pipeline` (39 checks) |

`pick_group`, `pick_candidate`, `win_emit_fused`, the admitted-residency gate, the cache hierarchy, the L2
MSHR file, the DRAM map/timing and FR-FCFS are untouched — `sched_group` stays 0 and `memory.cpp` and
`system.cpp` are not modified.

Populations the counters keep separate, as the brief requires:

| Population | Counter |
|---|---|
| runs still awaiting translation (in flight or not yet admitted) | `bulk_g6i_untr_*` = `fmeta.size() - tr_done` |
| completed translations waiting to enter the pool | **structurally 0**: `take_xlate` → `ready_push` in the same cycle, and capacity is reserved by `held = pool + in-flight` before submission |
| runs in the pool | `bulk_g6b_ready_occ_sum` (per cycle), `bulk_g6i_samp_pool_*` (per sample), `bulk_g6b_ready_h0..h12` |
| runs already released | `bulk_g6b_release_pops`, `bulk_tgt_req_issued / done` |

## 4. Translation and pool occupancy measurements

Per workload, `A1` = pool 4 096, `B` = pool 8 192; nothing else differs. Occupancy is per cycle
(`bulk_g6b_ready_occ_sum / cycles`), the page/cell counts are per sample (`g6i_sample_period = 512`
cycles), `untr` is the window's runs not yet translated at a sample.

| Workload | pool | tr/cyc | pool mean | pool peak | full | empty | pages in pool mean / max | cells in pool mean / max | runs/cell | untranslated mean | descriptors busy (mean / max) |
|---|---:|---:|---:|---:|---:|---:|---|---|---:|---:|---|
| Orkut S1 | 4 096 | 0.134 | 2 045.3 | 4 096 | 8.11 % | 3.84 % | 255.9 / **803** | 689.3 / 2 059 | 2.97 | 3 687 | 5.66 / 8 |
| Orkut S1 | 8 192 | 0.134 | 2 318.6 | 8 192 | 2.21 % | 3.84 % | 277.3 / **1 060** | 747.6 / 2 829 | 3.10 | 3 416 | 5.45 / 8 |
| LiveJournal | 4 096 | 0.052 | 59.0 | 4 096 | 0.60 % | 94.82 % | 3.7 / 426 | 11.3 / 1 264 | 5.24 | 3 911 | 7.90 / 8 |
| LiveJournal | 8 192 | 0.052 | 82.5 | 8 192 | 0.35 % | 94.79 % | 4.3 / 462 | 13.5 / 1 368 | 6.13 | 3 888 | 7.89 / 8 |
| ogbn train | 4 096 | 0.085 | 23.8 | 4 096 | 0.07 % | 85.56 % | 3.8 / 632 | 7.6 / 1 231 | 3.13 | 8 212 | 7.94 / 8 |
| ogbn train | 8 192 | 0.085 | 33.8 | 8 192 | 0.05 % | 85.52 % | 5.4 / **1 276** | 10.1 / 2 255 | 3.34 | 8 205 | 7.94 / 8 |

Pool occupancy distribution (Orkut, 4 096, `bulk_g6b_ready_h*`, log2 buckets): empty 3.84 %,
1 024–2 047 11.9 %, 2 048+ **46.2 %** (2 831 942 cycles) — the pool sits in its top bucket half the
time, so it is a working buffer, not a queue that is usually empty. Admission opened 106 292 page
descriptors over 74 windows (≈1 436 pages per window) and none of them was held for another page's
DRAM work.

The occupancy integral equals the summed sojourn exactly on all seven cells
(`bulk_g6b_ready_occ_sum == bulk_g6b_tr_wait_dram` = 12 538 861 068 on Orkut, verified in the check
script): the pool holds each run 15 224 cycles on Orkut (17 258 at 8 192), 1 144 (1 600) on LiveJournal
and 279 (398) on ogbn. That is the pipeline's true depth metric: a run waits ~15 000 cycles between its
translation completing and its release — roughly 1 600× the 9.6-cycle mean DRAM service of one read.

## 5. Cross-page population

| Workload | pool | pages/sample mean | pages max | cells mean | cells max | runs/cell | cells fed by ≥2 pages (mean / max) | page switches among releases | releases while translating |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|
| Orkut | 4 096 | 255.9 | 803 | 689.3 | 2 059 | 2.97 | 0 / 0 | 12.94 % | 70.3 % |
| Orkut | 8 192 | 277.3 | 1 060 | 747.6 | 2 829 | 3.10 | 0 / 0 | 12.94 % | 68.8 % |
| LiveJournal | 4 096 | 3.7 | 426 | 11.3 | 1 264 | 5.24 | 0 / 0 | 34.92 % | 95.1 % |
| LiveJournal | 8 192 | 4.3 | 462 | 13.5 | 1 368 | 6.13 | 0 / 0 | 34.92 % | 95.6 % |
| ogbn | 4 096 | 3.8 | 632 | 7.6 | 1 231 | 3.13 | 0.26 / 140 | 26.66 % | 100.0 % |
| ogbn | 8 192 | 5.4 | 1 276 | 10.1 | 2 255 | 3.34 | 0.89 / 406 | 26.66 % | 99.8 % |

**The pool is cross-page, decisively.** On Orkut an average sample holds 256 distinct virtual pages and
803 at peak; on LiveJournal and ogbn the pool is nearly empty most of the time (59 and 24 runs) but its
bursts still carry 426–1 276 pages. Separate pages also alternate inside the *release* stream: 12.9 % of
consecutive Orkut releases, 34.9 % on LiveJournal and 26.7 % on ogbn come from a different page than the
previous release — the organizer is not walking a page, it is walking the physical key order.

**Cells fed by two pages are essentially absent, and that is structural.** With `row_bytes = 1 024`
(16 lines per bank-row) and `row = (line/32) % 4096`, one physical row index covers 32 consecutive lines
(16 per bank) and the map repeats every 8 MiB. A 4 KiB page is 64 consecutive lines, so page `p` owns
rows `2p` and `2p+1` — **four physical cells, 16 lines each, all of them inside that page**. Two distinct
pages can share a row only if their page numbers differ by 2 048 (8 MiB). The pool holds at most 1 060
pages (Orkut) with an 8 MiB-periodic map, so a live set spanning less than 8 MiB of pages cannot contain
a shared row: measured 0 cross-page cells in every one of the 11 957 Orkut and LiveJournal samples, and
0.26–0.89 per sample on ogbn, whose bursts do reach across the period (peaks of 140 and 406 cells).
Virtual-page boundaries therefore never cut a physical row in this map; for the two BFS traces grouping
by page and grouping by row are the same partition, offset by at most one row.

## 6. DRAM organization effectiveness and performance

| Workload | pool | cycles | vs 4 096 | DRAM reads | row hit | activations | reads/act | conflicts | same-row revisit gap (mean / max cycles) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Orkut S1 | 4 096 | 6 130 494 | — | 644 142 | 60.5290 % | 254 249 | 2.5335 | 254 247 | 51 581 / 5 446 432 |
| Orkut S1 | 8 192 | 6 130 526 | +0.001 % | 644 151 | 60.5284 % | 254 257 | 2.5335 | 254 255 | 51 581 / 5 446 464 |
| LiveJournal | 4 096 | 14 146 860 | — | 697 141 | 46.1440 % | 375 452 | 1.8568 | 375 450 | 153 562 / 8 886 677 |
| LiveJournal | 8 192 | 14 144 216 | −0.019 % | 697 151 | 46.1454 % | 375 448 | 1.8569 | 375 446 | 153 544 / 8 886 964 |
| ogbn train | 4 096 | 98 563 897 | — | 9 826 463 | 65.0368 % | 3 435 646 | 2.8602 | 3 435 644 | 95 759 / 4 421 828 |
| ogbn train | 8 192 | 98 602 949 | +0.040 % | 9 826 974 | 65.0395 % | 3 435 560 | 2.8604 | 3 435 558 | 95 757 / 4 421 828 |

Reading:

1. **Occupancy is not the constraint.** +13.2 % pool entries (2 045 → 2 319), +8.4 % pages, +8.4 % cells,
   +4.5 % runs per cell, and the row-hit rate moves by **0.0006 pt** on Orkut (−5 hits in 644 151) and
   +0.0027 pt on ogbn. Cycles move by +0.001 %, −0.019 %, +0.040 % — three different signs, all inside
   a rounding error of the workload.
2. **The organizer was never starved of population.** Even at 4 096 it chooses among 689 cells and 2 045
   runs per decision, and the DRAM already performs *fewer* activations than releases (254 249
   activations for 823 642 releases, 0.31 per release) because FR-FCFS merges hits inside its own
   60-deep queue. Adding 13 % more candidates cannot create a hit the controller's window would not
   have found.
3. **The same-row revisit gap explains the ceiling.** A physical row is served once every 51 581 cycles
   on Orkut (95 759 on ogbn) — 8.4× the run's own pool sojourn. A cell's run burst is consumed within one
   window and the row is not seen again for tens of thousands of cycles, so the only way to lengthen a
   burst is to have more of that cell *translated* at the same time, which is a translation-depth
   problem, not a page-coupling one.

## 7. Answers to the ten architectural questions

1. **Did the implementation wait for DRAM scheduling before advancing to the next page?** No. §1.1: the
   admission block reads the pool's size and the dTLB, nothing about DRAM state; the only gate is
   `held = pool + in-flight >= cap`. The one barrier of this kind in the code base is M23-G6A's
   phase-1 → phase-2 window barrier (`bulk_org = 6`), which M23-G6B removed and which no cell here runs.
2. **Can multiple virtual pages contribute simultaneously to `b.ready`?** Yes — 255.9 pages on average,
   803 at peak (4 096 pool); 277.3 / 1 060 at 8 192.
3. **Does translation continue while the organizer releases?** Yes — 70.3 % of Orkut's releasing cycles
   have a probe in flight, 95.1 % on LiveJournal, 100.0 % on ogbn; 8/8 page descriptors are busy at peak.
4. **Does the pool actually contain many pages at once?** Yes: 803 (Orkut), 426 (LiveJournal), 1 276
   (ogbn) at peak, and 12.9 %–34.9 % of consecutive releases switch page.
5. **Does occupancy increase versus G6H?** Only if the pool is enlarged: G6H's pool is the same 4 096
   entries (2 045.3 mean here, identical cell). At 8 192 it rises 13.2 % to 2 318.6.
6. **Do distinct physical `(bank,row)` groups increase?** With the bigger pool, yes: 689.3 → 747.6 mean
   (+8.4 %), 2 059 → 2 829 peak.
7. **Does the number of requests per physical row improve?** Marginally: 2.97 → 3.10 (+4.5 %) on Orkut,
   5.24 → 6.13 on LiveJournal, 3.13 → 3.34 on ogbn. The ceiling is 16 lines per cell, and the gap is
   translation depth, not pool space.
8. **Does the DRAM row-hit rate improve?** No: 60.5290 % → 60.5284 % (Orkut), 46.1440 % → 46.1454 %
   (LiveJournal), 65.0368 % → 65.0395 % (ogbn). All within ±0.003 pt.
9. **Does total execution time improve?** No systematic effect: +0.001 %, −0.019 %, +0.040 %.
10. **What limits it after decoupling?** Translation, and the DRAM controller's own window:
    * translation: 31 690 561 admission attempts for 823 642 admissions (38.5 per run), 30 866 919 dTLB
      refusals (19 900 452 parked on the page's own MSHR, 10 966 467 with no MSHR free), admission idle
      86.8 % of all cycles, 8/8 descriptors busy, 3 687 untranslated runs in the window every sample;
    * the pool is full only 8.1 % of cycles, so storage is *not* the binding resource;
    * downstream, the DRAM queue (mean 60.8 entries) and FR-FCFS already extract the available row hits,
      which is why more candidates do not convert into fewer cycles (§6.2).

## 8. Correctness and determinism

`./bin/dx100_tests` → **4 806 checks, 0 failures** (4 767 before; +39 new).

| Requirement | Result |
|---|---|
| G6H baseline reproduction, feature off | `A0`/Orkut vs `build/wm/m23g6h/g6h_G0r_orkut.txt`: **761 keys, 0 differ**; same check on LiveJournal and ogbn with `A1` |
| Diagnostic is inert | `A0` vs `A1` (same config, `g6i_diag` 0/1): **761 keys, 0 differ**, cycles 6 130 494 both |
| Request conservation | `bulk_g6b_tr_requests == tr_completed == runs_released`; `bulk_tgt_req_gen == issued == done`; `cc3_issue == cc3_done` — all seven cells |
| No duplicate translation / release | `bulk_g6c_dup_submit_blocked = 0`; `admit_accepted == tr_requests`; the release pops the pool position it chose (`bulk_g6g_sel_mismatch = 0` in the G6H runs) |
| No lost requests | `requests_unissued_end = 0`, `bulk_elems_done == bulk_elems_total` in every cell |
| Nothing released before translation | every release re-derives `(bank,row)` from `b.fpa[li]`, which is only written by `take_xlate` (§1.1 block 1) |
| No occupancy beyond capacity | `bulk_g6i_pool_over = 0` in all seven cells; peak 4 096 / 8 192 = capacity |
| No deadlock under a full pool | the diagnostic run with a **32-entry** pool completes; the test uses an 8-entry pool (8 923 full-pool cycles) and still retires every element |
| No starvation | DRAM releases continue while admission is parked (70–100 % of release cycles overlap), and admission continues while the DRAM is busy (pool full only 0.05–8.1 % of cycles) |
| Determinism | Orkut `A1` repeated: **byte-identical log**; the test repeats the same config and compares cycles and every `g6i` counter |
| Conservation / limits | `conservation_ok = 1`, `conservation_violations = 0`, `l2_mshr_max = 64`, `dram_max_q ≤ 63`, `dram_q_full = 0`, `dram_missed_hit = 0`, `bulk_residency_violations = 0` |

Targeted tests, all in `test_m23_g6i_continuous_pipeline`:

| Required test | Assertion |
|---|---|
| Multiple virtual pages in one pool | `g6i_vpn_max >= 2`, `g6i_pages_open_max >= 2` |
| Different pages, same physical row | mapping test: two VA lines 8 MiB apart, different pages, equal `(bank,row)`; plus `xpage_cells <= cells` consistency in the run |
| Pool fills then drains | 8-entry pool: `g6b_full_cycles > 0`, `peak <= 8`, `admit_accepted > 8`, run completes |
| Translation resumes after capacity frees | same cell: `admit_accepted > capacity` proves admit → drain → admit |
| Translation while releasing | `g6i_rel_with_tr > 0`, `g6b_overlap_cycles > 0`, and `switch + same + windows == releases` |
| Many pages, no page barrier | `g6i_rel_vpn_switch > 0`, `g6i_pool_empty < cycles`, `pages_open_max >= 2` |

## 9. Storage and resource implications

* **The pool.** 4 096 entries × 16 B = 64 KiB declared (`bulk_g6b_pa_bytes`), 8 192 → 128 KiB. The
  experiment changed nothing else; the 8 192 cell is the same design with a deeper buffer.
* **The diagnostic is not a design.** Samples allocate a 16 384-slot VPN set (128 KiB) and four
  8 192-entry per-cell arrays (128 KiB) plus a per-cell last-release timestamp (64 KiB). None of it is
  charged to the modelled SRAM, all of it is empty unless `g6i_diag` is set, and the report's occupancy
  numbers are per-sample, not per-cycle, for exactly that reason.
* **A real implementation needs no sorter.** The organizer reads one position per decision (the heap
  front) and the pool is already ordered by physical key at seal time; the extra page/cell counters a
  hardware monitor would want are O(1) updates on push/pop.
* **Translation resources were not increased.** 8 walkers, 8 cohorts, 8 dTLB MSHRs, 150 cycles/level, R
  and the cache geometry are the frozen G6D winners; the 8 192 cell re-runs them unchanged.

## 10. Remaining bottlenecks and limitations

1. **Translation is the wall.** 38.5 dTLB attempts per admitted run and 86.8 % admission-idle cycles on
   Orkut mean the dTLB/walker subsystem, not the pool, sets the depth. Any further gain from "more
   population" has to come from more translated lines per unit time — more walkers or MSHRs, which the
   brief forbids — or from a trace whose pages are cheaper.
2. **Rows are almost page-private in this map.** 4 KiB pages over 1 KiB rows means a page owns its four
   cells outright, and cross-page row sharing needs pages 8 MiB apart: measured zero on Orkut and
   LiveJournal, and only 0.26–0.89 cells per sample on ogbn, whose live set does span that distance. A map
   with rows larger than a page, or a randomized physical allocator, is the configuration in which
   cross-page accumulation could matter — this map's near-zero is a property of the map, not a general law.
3. **Two cells, two workloads each.** LiveJournal and ogbn were run at both capacities as diversity
   checks, not as a matrix; no other window sizes, pool sizes or dispatch points were tried.
4. **The pool's occupancy is bursty.** On LiveJournal the pool is empty 94.8 % of cycles and on ogbn
   85.6 %, so the mean page count (3.7 / 3.8) understates the bursts (426 / 632 pages) that the grouping
   actually sees.
5. **G6A's barrier is quoted, not re-measured.** The historical two-phase arm was not re-run; the claim
   that the shipped path has no barrier rests on the source in §1.1 and on the event counts in §5.
6. **The diagnostic itself is sampled.** Occupancy, sojourn, full/empty cycles and release overlap are
   exact (per cycle or per event); pages/cells per pool are per sample (512 cycles), so their means are
   time-averages, not per-decision counts.

## 11. Recommendation

**Do not change the pipeline.** Virtual-page boundaries already are not DRAM scheduling boundaries, the
pool is already shared and cross-page, and the only backpressure is its capacity — which is not the
binding constraint (8.1 % of cycles). Doubling it moves cycles by ±0.02 % and row hits by ±0.003 pt.
The remaining levers, in the order the measurements support them:

1. **Translation throughput** (walkers/dTLB MSHRs): 38.5 attempts per admitted run, 86.8 % idle.
2. **The DRAM controller's arbitration window** (M23-G6E's 7.4 % ceiling; `case_b`/`case_c` conflicts).
3. **A map where rows are not page-aligned**, if cross-page row sharing is the property worth testing.

## Reproduce

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests          # 4806 checks, 0 failures
cd .. && ./build/wm/m23g6i_run.sh orkut                    # A0, A1, B  (~4 min)
cd .. && ./build/wm/m23g6i_run.sh livejournal              # A1, B      (~1 min)
cd .. && ./build/wm/m23g6i_run.sh ogbntrain                # A1, B      (~8 min)
cd .. && python3 tools/m23g6i_extract.py                   # tables, summary.csv, all identities
```

Per-cell command (Orkut, 8 192-entry pool, instrumented):

```bash
cd DX100_MEM/sim && ./bin/dx100sim --config configs/m23g6b_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 --set bulk_org=8 \
  --set walkers=8 --set page_cohorts=8 --set dtlb_mshr=8 \
  --set walk_latency_per_level=150 --set g6b_pa_capacity=8192 \
  --set g6i_diag=1 --set sched_vis=0 --set sched_group=0 --all
```
