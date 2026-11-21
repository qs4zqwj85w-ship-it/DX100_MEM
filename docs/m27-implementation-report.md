# M27 — Implementation report: correctness, resource fairness, Bulk cleanup

**Scope.** Implements the corrections identified in `docs/m27-gpu-architecture-audit.md`. The
architecture is unchanged: a conventional SIMT GPU model plus two custom functions — a dispatch
scheduler and a page-organization accelerator. No new architectural feature was added, no timing
parameter was changed, no cache/TLB/DRAM policy was replaced, and no historical artifact was modified.

**Status at a glance**

| | |
|---|---|
| tests | **4 967 checks, 0 failures** (was 4 940 checks / 6 failures) |
| the two M26 liveness cells | **now complete** (they failed by design before) |
| SIMT-only anchor | **byte-identical** to the preserved M24-D log |
| Bulk anchors | ±0.05 % cycles from the correctness fixes; the sorter's removal is ±0.2 % |
| verify status | every change below was run; items marked *not fully verified* say why |

---

## 1. Files modified

| file | lines | change | phase |
|---|---|---|---|
| `sim/src/sim.hpp` | `Req`: `ppn`, `ppn_ok`; `tlb_probe` signature; `ReqKind::Xlate`; `Config::m24_dram_org` default `false`; `Config::eng_core`; `BulkStats::tr_slot_stall`; `BulkEngine::ecore()`; `MemorySystem::live_limit()/last_progress_cycle()/liveness_tripped()/outstanding_report()` + state | 1-5 |
| `sim/src/memory.cpp` | liveness bound in the constructor; `tlb_probe` hands back the PPN; `submit` captures it; stage 1 consumes it; phase-1 retire emits a `done` message; progress stamps; `outstanding_report()` | 1-3 |
| `sim/src/bulk.cpp` | engine `eng_core` validation; **three** translation-probe sites take/release a residency slot; `attribute()` handles `ReqKind::Xlate`; all seven request sites use `ecore()`; `live_check` in `run_hybrid` and the loop in `run_bulk`; dispatch guard message + `eng_core` range check; `m24_dram_org` guard narrowed; `g6c` attempt counter ordering | 2-5 |
| `sim/src/system.cpp` | `stall_abort` gate replaced by the permanent liveness check | 2 |
| `sim/src/util.cpp` | parses `eng_core` | 5 |
| `sim/tests/tests.cpp` | five new tests (+27 checks) | 6 |
| `configs/m27_bulk.cfg` | **new** production Bulk config: `m24_dram_org = 0`, otherwise identical to the pinned M24-D template | 4 |

Not modified: any `build/wm/m24*` artifact, any `docs/m24-*`/`m25-*`/`m26-*` report, any timing
parameter, the cycle guard, the cache replacement policy, the DRAM controller or its scheduling
policy, the page-cohort organizer, the dispatch scheduler's role, any Vortex file.

---

## 2. The TLB defect and its exact correction

**Before.** `tlb_probe` answered a dTLB hit with a *promise* and nothing else:

```cpp
// memory.cpp:236 (pre-M27)
if (e.valid && e.vpn == vpn) { e.lru = ++lru_clock_; ready = cycle + 1; return true; }
```

The request then carried only `ready`. Stage 1 re-read the array one cycle later
(`memory.cpp:1088-1119`); if a fill had replaced the entry in that cycle, the request found nothing,
pushed itself back on the queue, and repeated the *lookup* forever. No other structure owned it, so no
event could ever retire it. Every miss path had a guarantee (a parked waiter on a dTLB MSHR); the hit
path had none.

**After.** The translation is handed to the request at the cycle the hit is detected, and consumed
from the request:

```cpp
// tlb_probe: the hit now also returns the value it matched
if (ppn_out) *ppn_out = e.ppn;          // memory.cpp, hit branch
// submit: the request owns it
req.ppn = ppn; req.ppn_ok = (ppn != NONE);
// stage 1: use what we own; only the miss path polls
if (r.ppn_ok) { ppn = r.ppn; r.ppn_ok = false; found = true; /* refresh LRU */ }
else if (ideal_translation) { ... }
else { /* array lookup: a fill the model promised will deliver */ }
```

Properties that matter:

- **Latency unchanged.** The request still waits `cycle + 1` before the stage-1 step runs; the value
  is simply available when it gets there.
- **Eviction cannot invalidate an accepted translation** — the value lives in the request, not in an
  evictable array. The array's LRU age is still refreshed on consumption, so the *cache* behaviour of
  other clients is unchanged.
- **Miss handling untouched.** A miss still parks a waiter on an MSHR, the fill still wakes it through
  the array, `parked`/`refuse_full`/`refuse_parked` are untouched.
- **No duplicate work, no lost ownership.** `ppn_ok` is cleared on consumption and on any refused
  re-submission, so a retried `submit` can never carry a stale result.
- **Both arms.** The fix is in `submit`/`tick`, which SIMT and Bulk share; the M26 cells instance
  stranded client-0 *and* client-2 requests, and both now complete.

**How the race is reproduced deterministically** (`m27_tlb_hit_eviction`): 4 dTLB entries, **1 way**,
512 pages. Every fill replaces the live entry, so the hit→eviction window is exercised on essentially
every access: hits 13, misses 2 035, evictions 2 019, and the run completes in 28 469 cycles with
`inflight == 0` and `completed == requests_generated`. Before the fix the same shape stranded a
request; that is exactly the M26 signature at 1/335 000 frequency in a larger configuration.

---

## 3. The liveness invariant

`stall_abort` was a test harness switch (M26). Liveness is now a property of every run.

**State.** `MemorySystem::last_progress` is stamped on every successful admission (`submit`) and every
retirement (stage-1 and stage-2). It answers "when did the model last do something", not "when did the
front-end last try".

**Bound.** Computed once per `MemorySystem` from the model's own worst-case service times:

```
live_limit = 8 × ( levels × walk_latency_per_level
                 + (t_miss + l2_latency + l1_latency) × (l2_mshr + R + 16) )
```

With the pinned baseline (2 × 150, 26+4+2, 64+128+16) that is **375 056 cycles** for the M27 test
configuration and **55 648** for the M24-D pinned one. It is derived, not tuned: it is the time for
every outstanding request to queue behind every other at the slowest bank, plus one full walk, times
eight. It cannot fire on a healthy run, because a healthy run retires far more often than once per
55 000 cycles, and `m27_tlb_hit_eviction` confirms it does not.

**Detection.** All three run loops check `(work remains) && liveness_tripped(cycle, bound)` and stop
with a violation that names the outstanding requests:

```
liveness: no admission or retirement for 37505 cycles (bound 37504); 0 request(s) outstanding
```

(`outstanding_report()` prints up to eight requests with `id`, `vpn`, `stage`, `core`, `client`,
`ready`, `probed`, plus the pending-fill count.)

**Separation from a slow workload.** The check is about *progress*, not duration: a run that is simply
long keeps retiring, so `cycle - last_progress` stays small. A run where work remains but nothing has
moved cannot finish by definition, because no event is left that could retire anything.

**No silent results.** Every trip sets `res.ok = false`, which makes the driver print
`conservation=VIOLATED [...]`, report `conservation_ok=0`, and exit non-zero (observed: exit 1 on the
aborted M26 cells before the fix). Performance numbers from such a run cannot be mistaken for data.

**Applied to both arms.** `Sim::run(Mode)` (SIMT), `Sim::run_hybrid` (dispatch), `Sim::run_bulk`
(pure Bulk). `stall_abort` survives only as a *tighter bound* for tests, default 0 = use the derived
bound; it can no longer disable the check.

**Test** (`m27_liveness_stall`): `dtlb_mshr = 0` means no probe can ever be admitted — a permanently
stuck system with zero requests in flight and slots still held. The run stops at cycle 37 505 with a
violation containing "liveness", **not** at the cycle guard (500 000 000).

---

## 4. Translation resource accounting, before and after

### 4.1 Before (measured from the M26 instrumentation)

| resource | SIMT request | Bulk phase-1 probe | Bulk target |
|---|---|---|---|
| residency slot `R` | held for the whole lifetime | **none** (`slot = 0xffffffff`) | held from release to fill |
| dTLB MSHR / parked waiter | yes on a miss | yes on a miss | no (pre-translated) |
| L2 TLB MSHR | yes on a miss | yes on a miss | yes on a miss (L2 path) |
| walker | yes on a miss | yes on a miss | yes on a miss |
| L1 / L2 MSHR | after translation | never (retires before any cache) | yes |
| PA buffer | n/a | n/a | `g6b_pa_capacity = 4096` entries |

The asymmetry was the first row: a SIMT request occupies a slot *while it waits up to 300 cycles for a
walk*, so it cannot overlap that wait with other work; the engine's probes occupied nothing, so the
engine could keep all 128 slots on data with 4 096 translations in flight. Measured consequence in the
M26 dump: `g6b_ready_peak = 4 096` (the cap) with `inflight` at 3.

### 4.2 After

| resource | SIMT request | Bulk phase-1 probe | Bulk target |
|---|---|---|---|
| residency slot `R` | unchanged | **held from submit to translation retirement** | unchanged |
| dTLB / L2 TLB / walker / MSHRs | unchanged | unchanged | unchanged |
| PA buffer | n/a | n/a | unchanged (page organization) |

Implementation: each of the three probe sites (`win_issue_g6a`, `win_issue_g6b`, `win_admit_g6c`)
takes a slot with `alloc_slot()` and refuses admission while `free_count() <= tgt_reserve()`, so the
index-stream reserve is still honoured; the request is submitted with that slot; `MemorySystem`
announces a phase-1 retirement on the same `done` list as every other release; `attribute()` releases
the slot under `ReqKind::Xlate`. A refused `submit` frees the slot immediately, so a probe never leaks
one.

### 4.3 What this changed, measured

| cell | probe-capacity effect | evidence |
|---|---|---|
| criteo 16K | ready pool peak **4 096 → 2 184** (production 2 062) | `bulk_g6b_ready_peak` |
| criteo 16K | translation admission stalled on the budget **69 323** times (`bulk_tr_slot_stall`) | new counter |
| criteo 16K | `inflight_max` = **128 = R** exactly (was unobservable before) | `m27_xlate_slots` |
| both cells | cycles +0.05 % / +0.001 % versus the same config before the change | §8 |

So the correction does not "artificially restrict" the engine beyond the shared budget: the engine can
still hold a page's worth of translations (the pool is R = 128 slots, and it reaches exactly that),
but it can no longer exceed what every other client gets.

---

## 5. DRAM sorter and the production default

**What was there.** `m24_dram_org = true` made `win_order_fused` sort the sealed window by physical
`(row, bank, line)` and install that order as the release order, so the engine's release stream was
DRAM-aware. `sched_vis` / `sched_group` add a per-release scan that consults
`MemorySystem::dram_row_open`.

**What changed.** `Config::m24_dram_org` now defaults to **false**, and with it:

- `win_order_fused` returns before building `ford`, so the sealed (ascending-line) order is the
  release order — page organization only;
- no `dram_bank_row` computation, no `dram_row_open` consultation, no physical sort;
- `sched_vis`/`sched_group` remain available but are off in the production config.

**Preserved.** Page cohorts (`page_sched`, `page_cohorts = 8`), the window organiser
(`batch_window = 16384`, `win_fused`), intra-line dedup, the sealed page order (`page_order`), the
dispatch scheduler, the DRAM controller and FR-FCFS. Nothing was deleted: setting `m24_dram_org = 1`
re-enables the historical organizer, and `build/wm/m24d/m24d_bulk.cfg` still sets it.

**The unambiguous default** is the new file `configs/m27_bulk.cfg`: the pinned M24-D template with
exactly one semantic change, documented in its header, plus `Config::m24_dram_org = false` as the
built-in default. The guard that rejected `m24_dram_org = 0` for every arm was narrowed to the five
M24-A policies whose *definition* is the organizer (1, 2, 3, 5); for all other arms the flag now means
what it says.

**Verified** (`m27_bulk_no_dram_org`): on the production config `bulk_org_runs == 0` (the sorter never
ran), `win_entries > 0` (page organization still ran), `desc_completed == 1`; on the same cell with
`m24_dram_org = 1`, `org_runs = 13 097`.

---

## 6. Multi-core limitation

**What was there.** The engine hard-coded `r.core = 0`, so under `mem_path = l1` all of its traffic
entered **core 0's private L1**. `Sim::run_hybrid` already refused `cores != 1`, but with a message
about slot vectors; the pure Bulk arm (`run_bulk`) had no check at all.

**What changed.**

- `Config::eng_core` names the SM the engine enters (`0xffffffff` = unset). Used at all seven engine
  request sites instead of the literal 0.
- `run_bulk` refuses `cores > 1 && mem_path == "l1" && eng_core` unset, with a message that says why
  (a multi-SM engine is not modelled). The bypassing paths, which touch no per-core cache, still run.
- `run_hybrid` refuses `cores != 1` with an explicit statement of the limitation (slot vector per core
  **and** one engine SM), and refuses an out-of-range `eng_core`.
- Both are `fail()` violations, not exceptions escaping the driver: a configuration error fails the
  run and is reported.

**Verified** (`m27_eng_core`): `cores = 2`, `eng_core` unset → run fails with a message containing
"single-core"; `cores = 2`, `eng_core = 7` → fails naming `eng_core`.

**Not done:** a Multi-SM engine (per-core dTLB/L1 ownership, a bus to the engine). That is a design
change, outside this stage.

---

## 7. Tests executed and results

`./bin/dx100_tests`: **84 tests, 4 967 checks, 0 failures.**

| # | required test | test | result |
|---|---|---|---|
| 1 | original TLB hit/eviction race completes | `m27_tlb_hit_eviction` | pass — 13 hits / 2 035 misses / 2 019 evictions, `inflight = 0` |
| 2 | SIMT translation requests complete correctly | `m27_tlb_hit_eviction`, `m26_liveness_map3_s3` | pass |
| 3 | Bulk translation requests complete correctly | `m26_liveness_cohorts1` | pass — 13 097/13 097 probes, 762 028 cycles (was stuck) |
| 4 | misses and MSHR waiters still work | whole suite (walks, `tlb_capacity`, `walks_analytic`, `bulk_translation_and_cache`) | pass |
| 5 | request conservation remains valid | every test asserts it; `m27_xlate_slots` adds `enter == leave + inflight` | pass |
| 6 | liveness detects stranded requests | `m27_liveness_stall` | pass — trips at the bound, names "liveness" |
| 7 | normal SIMT dispatch unchanged | `m24d_simt.cfg` orkut/criteo replay | **byte-identical** |
| 8 | Bulk dispatch + page organization functional | `m27_bulk_no_dram_org`, all `bulk_*`, `m6_*`, `m13_*` | pass |
| 9 | both paths use the shared infrastructure | `m27_xlate_slots` (`inflight_max = R`), `bulk_translation_and_cache` | pass |
| 10 | default Bulk config no longer uses the sorter | `m27_bulk_no_dram_org` (`org_runs == 0` vs 13 097) | pass |

Regression detail (the two M26 cells, which were the whole point):

| cell | before M27 | after M27 |
|---|---|---|
| `map3_s3` | stranded at ≥ 8 292 977 cycles, `inflight = 1`, 6 failing checks | **completes**: 3 266 313 cycles, `inflight = 0`, `stale = 0`, 108 142/108 142 probes |
| `cohorts1` | stranded at ≥ 5 364 058 cycles, `inflight = 3` | **completes**: 762 028 cycles, `inflight = 0`, `stale = 0`, 13 097/13 097 probes |

---

## 8. Performance anchors

All numbers produced by the corrected binary, labelled as such. Historical values are the preserved
files, read only.

### 8.1 SIMT: no change at all

```
./bin/dx100sim --config ../build/wm/m24d/m24d_simt.cfg \
               --set dataset=../build/wm/m23_criteo_16k.bin --all
→ diff against build/wm/m24d/m24d_simt_criteo.txt : IDENTICAL (every counter, byte for byte)
```

The TLB fix and the liveness check cost nothing on a run where the race does not occur, which is the
strongest available evidence that the change is surgical.

### 8.2 Bulk, like-for-like (same binary, same trace, only `m24_dram_org` differs)

| cell | sorter ON (`m24d_bulk.cfg`) | sorter OFF (`configs/m27_bulk.cfg`) | Δ |
|---|---|---|---|
| criteo 16K, cycles | 3 268 960 | **3 263 860** | −0.156 % |
| criteo 16K, DRAM reads / row hits / conflicts | 103 587 / 32 911 / 70 674 | 103 583 / 32 578 / 71 003 | hits −1.0 % |
| orkut BFS, cycles | 6 130 567 | **6 131 444** | +0.014 % |
| orkut BFS, DRAM reads / row hits / conflicts | 644 230 / 390 074 / 254 154 | 644 288 / 390 024 / 254 262 | hits −0.01 % |

**Reading.** Removing the DRAM sorter is worth **−0.156 %** on criteo and **+0.014 %** on orkut: it is
noise in both directions. That is the expected result and the reason the sorter is gone: under the
production map with an 8 MiB working set at 8 MiB of capacity there is no row sharing to exploit (M26
§8, M27 §8.4), so the organizer's visits and the reordered release stream are pure cost. The page
cohort organization, which is the mechanism we intend to keep, is untouched and still runs
(`win_entries` unchanged, cohorts = 8).

### 8.3 Bulk versus the preserved M24-D log

| cell | historical log | M27, same config | Δ | cause |
|---|---|---|---|---|
| criteo 16K | 3 267 387 | 3 268 960 | +0.048 % | translation probes now hold slots (§4.3) |
| orkut BFS | 6 130 494 | 6 130 567 | +0.0012 % | same |

Both arms report `desc_completed = 1`, `elems_done = 294 912` (criteo) / all elements (orkut), and
pass every conservation check. Small request-count differences were observed (criteo `issued`
224 934 → 224 939, orkut 898 763 → 898 725, ≤ 0.02 %): they come with the pool-pressure change from
charging probes, and I did **not** fully attribute them beyond that — flagged as an open detail in §10,
not as a result.

---

## 9. Historical results affected

| artifact | status |
|---|---|
| `build/wm/m24d/*`, `build/wm/m25*`, `build/wm/m26/*` | **untouched** (read only) |
| `docs/m24-*`, `docs/m25-*`, `docs/m26-*` | **untouched** |
| M24-D Bulk performance numbers | **superseded as production** by `configs/m27_bulk.cfg`; the old numbers remain valid *for the sorter-on configuration they were produced with* |
| M25/M24-A mapping conclusions | already corrected in M26-B1/M27; nothing here changes them |
| SIMT numbers | **still exact** — the SIMT arm reproduces byte-identically |
| the 6 failing M26 configurations | **no longer fail**; their earlier "results" (aborted runs) were never usable and stay unusable as measurements |

No measurement was relabelled or deleted. The production default changed; the historical files still
describe what they described.

---

## 10. Remaining known limitations

### Implemented and verified

1. A dTLB hit is retained by the request; eviction cannot strand it (SIMT + Bulk; deterministic test).
2. Liveness is an invariant on every run, with outstanding-request diagnostics and a derived bound.
3. Bulk translation probes obey the shared residency budget (`inflight_max == R`, refused admissions
   counted, index-stream reserve honoured).
4. The production Bulk path performs page organization only (sorter off by default, `org_runs == 0`),
   with the historical organizer still selectable.
5. The engine's home SM is named; multi-core dispatch is refused with a stated reason, and an
   out-of-range name is refused.
6. Request conservation holds everywhere; 4 967 checks, 0 failures.

### Implemented but not fully verified

7. **The `eng_core` routing itself.** It is exercised only for `cores = 1` (where it is 0). A
   multi-core `run_bulk` with `eng_core = 1` compiles and is routed, but no cell was run: dispatch
   refuses `cores != 1` by design, and a multi-core pure-Bulk cell is not part of the current
   experimental set.
8. **The probe-slot cost across the whole M24/M25 campaign.** Measured on two anchors only
   (+0.05 %, +0.001 %). Larger or more translation-bound cells may move more; that is a measurement
   to make, not a property to assume.
9. **The ±0.02 % request-count difference** versus the preserved logs (§8.3) is observed and bounded
   but not attributed line by line.

### Not implemented (deliberately)

10. `xlate_done_` still matches translations to a window by **position** (`x.seq & 0xffffffff`), which
    M27 §7.2 flagged as a latent hazard. It is unreachable today (`abort_desc` only runs before any
    work starts) and fixing it means giving every window an identity through the memory interface — a
    broader change than this stage's mandate. It should be done before any abort path is added.
11. The 8 192-entry `xlate_done_` overflow remains a counted drop rather than a checked invariant, for
    the same reason (it would need a backpressure path through the ring).
12. `l1_mshr = 128 = R` was left alone: it is a *configuration* choice that removes L1 MSHR pressure,
    not a defect, and changing it would invalidate every historical comparison at once.

### Remaining architectural limitations

13. The DRAM model is unchanged: 2 banks in the baseline, `t_hit 14`/`t_miss 26`, one command per
    cycle, no bus occupancy, no `tRRD`/`tFAW`/refresh, single channel. Absolute bandwidth/latency
    claims remain unsupported; only relative comparisons are meaningful.
14. The SIMT front-end is trace-driven: no warp scheduler, scoreboard, execution or issue contention.
15. Dispatch remains single-core. A multi-SM engine (per-core caches, a bus, a fairness rule) is a
    design task, not a bug fix.
16. The M3 mapping cells remain an aliasing study, not a physical mapping; `M0` is the production map.
