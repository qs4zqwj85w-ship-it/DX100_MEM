# M23-G6C — P0-style translation admission deferral inside G6B

**Status: implementation and correctness validation only.** The M23-G6C performance matrix was not
launched, no arm was compared and no performance claim is made here. `bulk_org = 8` is the new arm;
`bulk_org = 7` (G6B), 6 (G6A), 0 (P0) and 2 (P2) are unchanged.

---

## 1. The P0 behaviour that was reused

`BulkEngine::win_issue_sched` (`sim/src/bulk.cpp`) is P0's admission/issue policy. Four rules, all of
them reused verbatim in the new translation-admission loop:

| Rule | P0 code | G6C code |
|---|---|---|
| Up to `page_cohorts` page runs open at once (`page_cohorts = dtlb_mshr / dtlb_parked = 2`) | `win_open_cohort`: claim the maximal span of the sealed order that maps to one page into a free `Batch::Cohort` | `g6c_open_cohort`: identical, over the *stored* run array |
| Oldest ready run wins; a run whose deadline has not expired is skipped | select loop over `b.coh`, `x.resume_at > cycle` → `continue` | same loop, unmodified |
| A dTLB refusal is a *deferral event*, not the end of the cycle: `resume_at = cycle + park_retry`, `++sched_defer`, next run tried in the same cycle | `rc == 2` branch | `!mem_.submit(...)` branch, `++g6c_defer_events`, `continue` |
| Installing a run costs the scheduler port one cycle and issues nothing in it | `sched_busy_` | `g6c_busy_` (the admission port, not the DRAM port) |
| Only a cycle in which *every* open run is deferred is a lost cycle | `sched_idle` | `g6c_admit_idle` |

The retry interval is P0's own `Config::park_retry` (never 0, so a run cannot be parked permanently),
and the cohort table is P0's own `Batch::Cohort` array `b.coh` — already sized `page_cohorts` and
charged 32 B per descriptor by `win_seal`. G6C allocates no new cohort storage.

## 2. Refusal semantics (established from `memory.cpp`, unmodified)

A translation probe (`Req::xlate_only`) enters `MemorySystem::submit` → `tlb_probe` and stops at the
translation fill, so the dTLB is its **only** refusal source. Two causes exist:

| Cause | Condition | Temporary? | Scope | Counter |
|---|---|---|---|---|
| `refuse_parked` | the matching dTLB MSHR already holds `dtlb_parked` (2) parked requests | yes — the walk fill retires the MSHR | one page | `bulk_g6c_refuse_parked` |
| `refuse_full` | no dTLB MSHR is free (`dtlb_mshr = 4`, all four held by other pages) | yes — a fill frees a slot | global | `bulk_g6c_refuse_full` |

Both are backpressure on the same 4 MSHRs; neither is permanent, and no probe can be refused by the
L2 TLB, a walker or the cache hierarchy because it never reaches them. Deferring is therefore always
sound, but it is **not** guaranteed to find a usable resource elsewhere: with `dtlb_parked = 2` and
`dtlb_mshr = 4`, two other pages can hold all four MSHRs, in which case every cohort is deferred and
the cycle is counted in `bulk_g6c_admit_idle`. The instrumentation reports that case rather than
hiding it.

## 3. What changed

| File | Change | SHA-256 |
|---|---|---|
| `sim/src/bulk.cpp` | validation (`bulk_org ≤ 8`; org 8 requires `page_sched = 1`, `page_cohorts > 0`, `g6b_pa_capacity > 0`); routing `bulk_org == 8` into `win_issue_g6b`; the admission block split into G6B's cursor and the new `win_admit_g6c`; new `g6c_open_cohort`, `g6c_work_left`; `win_emit_fused` pretranslated path and post-translation wait accounting extended to org 8; 18 `bulk_g6c_*` keys in both export functions | `ba13e595…` |
| `sim/src/sim.hpp` | `bulk_org = 8` documented; 18 `BulkStats::g6c_*` counters; `Batch::g6c_init/g6c_open_pos/g6c_submitted`, `g6c_coh_def`, `g6c_coh_age`; `g6c_admit()`, `win_admit_g6c`, `g6c_open_cohort`, `g6c_work_left`, `g6c_busy_` | `f0bcf8e4…` |
| `sim/tests/tests.cpp` | `m23_g6c_admission_deferral` (32 checks), registered in the suite | `9f063636…` |
| `sim/src/memory.cpp` | **unchanged** — the refusal is classified at the caller from the dTLB's own counter deltas, so the memory model needed no new return value | `adf43b86…` (same as G6A/G6B) |
| `sim/src/util.cpp` | **unchanged** — org 8 takes no new configuration key | `4747a2ae…` |

Untouched: P0, P2, G6A and G6B code paths, the 4 096-entry ready buffer, physical bank/row/line
metadata, `win_emit_fused`'s release semantics, `win_fused` storage, translation resources and
latencies, window size, traces, configs and every historical log and report.

## 4. The new admission loop, and why it is safe

Each cycle, in order: completions land in `ready` (G6B, unchanged) → **admission** → DRAM release
(G6B, unchanged). Admission walks `fmeta` (stored order, i.e. ascending line, so a 4 KiB page's 64
runs are contiguous, exactly the unit P0's sealed window has) with up to `page_cohorts` cohorts open:

1. oldest cohort whose `resume_at ≤ cycle` wins;
2. if none, open the next page run (`g6c_open_cohort`), charging the port one cycle;
3. if neither is possible, the cycle is lost only if the buffer had room (`g6c_admit_idle`);
4. submission is bounded by `eng_issue` and by the ready buffer (`ready.size() + in flight ≥
   capacity` stops admission — G6B's backpressure, counted as such, not as a refusal);
5. a refusal defers that cohort for `park_retry` cycles and the loop continues with another cohort
   **in the same cycle**;
6. an accepted probe advances only that cohort's cursor and is released into `ready` the cycle its
   own translation lands — a partially translated page can already have runs at the DRAM.

*No loss, no duplication.* Cohorts partition `[0, open_pos)`; a cursor advances only on an accepted
probe; a re-probe is the same run index. `g6c_dup_submit` counts a submission of an already-submitted
run — structurally impossible — and is checked at 0.
*Fairness.* `resume_at` is finite (`park_retry ≥ 1`) and selection is oldest-first, so every cohort is
re-probed; the worst case is bounded by the MSHR fill that caused the refusal.
*Bounded state.* `page_cohorts` cohorts (P0's table) plus 12 B per cohort for the deferral episode.

## 5. Instrumentation

`bulk_g6c_admit_attempts`, `_accepted`, `_refused`, `_refuse_parked`, `_refuse_full`, `_refuse_other`
(must be 0), `_defer_events`, `_deferred_readmitted`, `_defer_then_other_admit` (refusal then a
*different* page admitted in the same cycle), `_defer_age_sum`/`_defer_age_max` (cycles from a
cohort's first refusal to the admission that ended the episode), `_defer_depth_sum`/`_defer_depth_n`/
`_defer_depth_max` (cohorts deferred at the event), `_cohorts_opened`, `_admit_idle_cycles`,
`_dup_submit_blocked`, `_windows`. dTLB/L2-TLB MSHR occupancy, walker utilisation, ready-buffer
occupancy/residence and DRAM issue/completion are the existing `dtlb_*`, `l2tlb_*`, `walker_*`,
`bulk_g6b_ready_*`/`bulk_g6b_tr_wait_*` and `bulk_tgt_*` counters. `bulk_g6b_tr_requests`/
`_completed` are shared with G6B because they are the same event; G6B's `bulk_g6b_tr_stall_cycles`
(a cycle count of a *stopped cursor*) has no G6C counterpart and stays 0 for org 8.

## 6. Differences from P0 that prevented literal reuse

1. **The port.** P0 submits a target request, so it has a residency refusal (`rc == 1`) that ends the
   cycle because nothing else can move. A probe holds no residency slot, so that branch has no
   analogue; the ready buffer's capacity plays the role, and it pauses the whole window.
2. **The unit of release.** In P0 a cohort is both the admission and the issue unit. In G6B the
   release side is a priority queue over translated runs in P2 order, so `b.coh` describes admission
   only, and `sched_*` remain the DRAM scheduler's counters rather than being recycled.
3. **The ordering key.** Admission walks stored order (`fmeta`), not `ford`: translation cannot see
   the physical order, and P0's page sequence is the stored one.
4. **Refusal classification.** `MemorySystem::submit` returns a bool. Rather than change the memory
   model, the caller takes the delta of `refuse_parked`/`refuse_full` around the call; a probe
   increments exactly one of them when it refuses.
5. **One port, two decisions.** P0's `sched_busy_` charges the shared issue port. G6C charges its own
   `g6c_busy_`, because the DRAM release must keep running in the same cycle (that is the milestone).

## 7. Validation actually run

```
cd DX100_MEM/sim && make -j8                       # exit 0
./bin/dx100_tests                                  # 4767 checks, 0 failures
./bin/dx100_tests m23_g6c_admission_deferral       # 32 checks, 0 failures
```

The new test (`bulk_org = 8` vs 7, `dtlb_mshr = 2`, `dtlb_parked = 2`, 512-run windows) checks, as
counters: every run admitted exactly once and every probe completed
(`admit_ok == tr_reqs == tr_done`, `dup_submit == 0`, `refuse_other == 0`); refusals were deferred and
another page was admitted in the same cycle (`refused = 19 521`, `defer_events = refused`,
`defer_then_other_admit = 535`); deferred work was revisited and admitted (`readmitted = 160`,
age max 304 cycles); runs of one page share its translation (`tr_reqs > walks`); the deferral is
bounded (`depth_max = 2 = page_cohorts`, `tlb_max_parked ≤ 2`); streaming is intact
(`overlap > 0`, `runs_released == tr_done`, `ready_peak ≤ capacity`); and the answers are identical
element for element (values and addresses) although the retirement order — and therefore the
order-dependent `completion_hash` — may differ. A second arm (one line per page, `park_retry = 16`,
4 cohorts) confirms eventual completion with `depth_max = 4`: `156 432` cycles, `38 000` refusals,
`1 008` re-admissions.

**Historical G6B reproduction.** The Orkut 16 K P7 cell was re-run and diffed against the stored
`build/wm/m23g6b/g6b_orkut_w16384_p7.txt`: every pre-existing line is identical; the only difference
is the 18 new `bulk_g6c_*` keys, all 0. P0/P2/G6A anchors are pinned by the existing suite, which
passes.

**Liveness on the real workload.** One smoke run of `bulk_org = 8` on `m23_orkut_bfs.bin` completes in
23 s of host time with `conservation=OK`, `bulk_elems_done = bulk_elems = 1 199 876`,
`bulk_g6b_tr_requests = _completed = bulk_g6b_runs_released = 823 786`, `bulk_addr_mismatch = 0`,
`bulk_residency_violations = 0`, `bulk_issue_monotonic_violations = 0`, `tlb_max_parked = 2`,
`g6c_dup_submit_blocked = 0`, `defer_depth_max = 2`, `refuse_other = 0`, and the existing invariant
`ready_occ_sum == tr_wait_dram` holds exactly. Its cycle total was not interpreted: it is a single
unrepeated run, not a measurement.

One behavioural cross-check of the reuse: on this trace G6C produces `32 067 246` deferral events
against P0's `31 816 599` (`bulk_sched_defer`, same configuration, `build/wm/m23g6b/
g6b_orkut_w16384_p0.txt`), i.e. the same order of retry traffic — G6C is running P0's deferral rule,
not a cheaper approximation of it. G6B on the same trace reports `bulk_g6b_tr_stall_cycles =
25 632 931` and no deferrals at all; G6C reports that counter as 0 by construction (§5).

## 8. Remaining limitations

* **No performance result.** Whether P0's utilisation can be combined with G6B's independent DRAM
  scheduling is exactly what the un-run matrix would measure. Nothing here supports a speed claim.
* **Retry traffic is P0's, and it is large**: 32.9 M probe attempts for 0.82 M admissions on Orkut
  (≈40 attempts per admission), because `park_retry = 1` re-probes every deferred cohort every cycle.
  This is inherited rather than introduced, and it is counted, not modelled as free.
* **Deferral cannot create capacity.** When four MSHRs are held by two other pages, every cohort is
  deferred and `g6c_admit_idle` grows; the counter is reported so that this is visible, not smoothed.
* `bulk_g6c_defer_age_*` and `_depth_*` are per *episode*: a cohort that defers, is admitted, and
  defers again contributes two episodes.
* The ready-buffer capacity was not swept for org 8 (G6B's 64–16 384 sweep stands; the buffer code is
  shared and unchanged).
* No Vortex RTL change, no physical GPU claim.
