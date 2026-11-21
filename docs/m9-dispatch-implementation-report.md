# M9 — Compiler-Directed SIMT/Bulk Dispatch: Implementation Report

**Status: implemented and verified (S1–S3 complete).** All five gates required before any
performance number — G1 all-SIMT equivalence, G2 all-bulk equivalence, G3 hybrid correctness,
G4 dispatch ordering, G5 resource accounting — pass. The published M4–M8 baselines are unchanged:
all 7 anchor workloads reproduce on both arms, `dx100_tests` is 4 032 checks / 0 failures.

Design and as-built rationale: `docs/m9-dispatch-design.md` (see its new §11 for the delta between
the design and what shipped). Everything below was run in this working tree.

---

## 1. What was implemented

| File | Change |
|---|---|
| `sim/src/sim.hpp` | `OpClass { Simt = 0, Bulk = 1 }`; `ClassMap` + `build_op_classes()`; `Trace::instr_class`; `BulkWorkload::elem_ids`; `build_bulk_workload(..., const ClassMap* = nullptr)`; `Config::{dispatch, op_class, dispatch_order}`; `Sim::run_hybrid()`; `M9Result`, `run_m9`, `print_m9`. |
| `sim/src/workload.cpp` | `build_trace` records the generator's own class decision per instruction (`instr_class`); `build_op_classes` turns it into a `ClassMap` (`op_class=gen`), or recovers it from the addresses with the same contiguity rule `kappa_real` uses (`op_class=pattern`), or produces a uniform map for the gates (`simt`, `bulk`). |
| `sim/src/bulk.cpp` | `build_bulk_workload` applies the class filter **before** the descriptor is constructed: `sel[j]` is the trace-access index of element `j`, `desc.count = sel.size()`, and the index array is generated only for `sel`. `elem_ids` carries the original identity. `Sim::run_hybrid` is the composite driver. |
| `sim/src/system.cpp` | One branch at the top of `Sim::run()`: `if (cfg_.dispatch) return run_hybrid();`. Nothing else. |
| `sim/src/util.cpp` | Parses `dispatch`, `op_class` (validated: `gen`/`pattern`/`simt`/`bulk`), `dispatch_order`; accepts `experiment=m9`; echoes the three in `to_string`. |
| `sim/src/main.cpp` | `experiment=m9` branch, placed before the `run_ab` fallthrough. |
| `sim/src/ab.cpp` | `truth_hash`/`engine_hash` fold `elem_ids[i]` instead of the compaction position (identical when `elem_ids` is empty, i.e. every published path); `run_ab` rejects `dispatch != 0`; `run_m9` (three arms + gates) and `print_m9`. |
| `sim/src/memory.cpp` | **Untouched.** |
| `sim.hpp`, `system.cpp`, `bulk.cpp`, `ab.cpp`, `main.cpp`, `util.cpp` (diagnostic, addendum) | `CycleSample` / `CycleTrace`, `Sim::observe()`, one guarded `if (cyc_)` observation at the end of each driver's cycle body, `experiment=m9diag`, `print_m9_diag`. Read-only: nothing reads a sampled field, so the drivers take the same decisions with the trace compiled in (proved by the anchor sweep in §8). |
| `sim.hpp`, `bulk.cpp`, `ab.cpp`, `util.cpp` (diagnostic, second addendum) | `SlotAges` histogram; `SlotPool::born`/`ages`/`now`, so `take` stamps the cycle and `give` tallies the age (both null unless a trace is attached); `BulkStats::{slot_take,slot_give,tgt_refuse_*,win_backlog}` and the attribution recorded at the refusal site in `win_emit`; per-cycle readiness/progress flags in `CycleSample`; `Config::pool_slots` (diagnostic capacity override, default 0 = published arithmetic) with `s.set("slot_pool_slots", ...)`; report sections [10]–[12]. All counters; no decision reads them. |

`Sim::run(cfg.mode)` (the SIMT baseline) and `Sim::run_bulk(w)` (the M8 path) are unchanged; the
hybrid driver has its own copy of the SIMT issue/coalesce sequence rather than a filtered
`Sim::run`, so the published path cannot be perturbed by dispatch. G1 is the guard that the copy
is faithful.

### 1.1 The dispatch itself

```cpp
// bulk.cpp, build_bulk_workload(): the filter runs before a descriptor exists.
for (u32 i = 0; i < tr.acc.size(); ++i) {
  if (cm && cm->at(tr.acc[i].instr) != static_cast<u8>(OpClass::Bulk)) continue;
  sel.push_back(i);                       // original element identity
}
if (sel.empty()) return w;                // armed == false: no descriptor, no index array
d.count = static_cast<u32>(sel.size());   // compaction is desc.count, nothing else moves
```

```cpp
// bulk.cpp, Sim::run_hybrid(): the SIMT front-end receives its share of the same instructions.
for (u32 i = 0; i < tr_->n_instr; ++i) {
  if (cm.at(i) != static_cast<u8>(OpClass::Simt)) continue;   // the dispatch
  if (blocked ? (i / chunk == c) : (i % cores == c)) cores[c].instrs.push_back(i);
}
```

The engine still keys on `desc.elem_base + pos`, so `BulkEngine`, `BulkDesc`, `Req` and
`MemorySystem` needed no change. Completion routing uses the S2 `SlotPool::owner` table
(`owner == 0` → SIMT front-end, `owner == 1` → engine), not a tag in `Req::slot`.

---

## 2. Gate G1 — all-SIMT equivalence

`op_class=simt` holds the trace fixed and forces every instruction onto the SIMT path. C must
reproduce A exactly: cycles, every hierarchy counter, the completion hash, and zero bulk
acquisition.

```bash
cd DX100_MEM/sim
R="--config configs/ab_controlled.cfg --set experiment=m9 --set N=16384 --set rho_dup=0 \
   --set elem_bytes=4 --set bulk_wl=E0 --set seed=1 --set batch_window=8192 \
   --set bulk_max_elems=262144 --set max_cycles=200000000 \
   --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
   --set page_sched=1 --set idx_mode=2"

./bin/dx100sim $R --set Lambda=16384 --set P=256 --set kappa=1 --set op_class=simt   # W0
./bin/dx100sim $R --set Lambda=16384 --set P=512 --set kappa=0 --set op_class=simt   # W1
./bin/dx100sim $R --set Lambda=16384 --set P=2048 --set kappa=0 --set op_class=simt  # PSCR
./bin/dx100sim $R --set Lambda=1024  --set P=64  --set kappa=1 --set op_class=simt   # STRID
./bin/dx100sim $R --set Lambda=2     --set P=1   --set kappa=0 --set op_class=simt   # HOT
./bin/dx100sim $R --set Lambda=256   --set P=4   --set kappa=0 --set op_class=simt   # DUP
./bin/dx100sim $R --set Lambda=1024  --set P=16  --set kappa=0 --set op_class=simt   # PGLO
```

| Workload | A cycles | C cycles | A hash | C hash | descriptors | index lines | gate |
|---|---:|---:|---:|---:|---:|---:|---|
| W0 (Λ 16384, P 256, κ 1) | 30 573 | **30 573** | 4 117 390 930 | **4 117 390 930** | 0 | 0 | PASS |
| W1 (Λ 16384, P 512, κ 0) | 270 753 | **270 753** | 145 307 694 | **145 307 694** | 0 | 0 | PASS |
| PSCR (P 2048) | 1 908 638 | **1 908 638** | — | — | 0 | 0 | PASS |
| STRID (Λ 1024, P 64, κ 1) | 22 664 | **22 664** | — | — | 0 | 0 | PASS |
| HOT (Λ 2, P 1) | 4 361 | **4 361** | — | — | 0 | 0 | PASS |
| DUP (Λ 256, P 4) | 6 831 | **6 831** | — | — | 0 | 0 | PASS |
| PGLO (Λ 1024, P 16) | 17 797 | **17 797** | — | — | 0 | 0 | PASS |

Compared key by key (`run_m9` fails the run on any difference): `cycles`, `requests_generated`,
`slot_allocs`, `completed`, `completion_hash`, all L1/L2/DRAM/TLB counters and `walks`,
`pte_fetches`, `replays`, `inflight_max`, `simt_peak_slot`, `simt_staging_max`. W0 and W1 also
match the published SIMT anchors (30 573 / 4 117 390 930 and 270 753 / 145 307 694). Zero
descriptors and zero index lines are checked as an explicit G1 condition, not inferred.

**Note.** With no bulk element C writes no index array, so its memory image differs from A's by
the index/descriptor region only. That region is outside the trace footprint, so the timing
identity above is exact; the check is the hierarchy state, which is compared in full.

---

## 3. Gate G2 — all-bulk equivalence

`op_class=bulk` forces every instruction onto the engine, on the *same* trace, through the
compacted path (`elem_ids[j] = j` degenerates to the M8 workload).

```bash
./bin/dx100sim $R --set Lambda=16384 --set P=512 --set kappa=0 --set op_class=bulk  # W1
./bin/dx100sim $R --set Lambda=16384 --set P=256 --set kappa=1 --set op_class=bulk  # W0
# ... and the same for PSCR, STRID, HOT, DUP, PGLO
```

| Workload | B cycles (published M8) | C cycles | B hash | C hash | gate |
|---|---:|---:|---:|---:|---|
| W1 | 156 419 | **156 419** | 1 011 476 448 | **1 011 476 448** | PASS |
| W0 | 38 775 | **38 775** | 1 860 312 448 | **1 860 312 448** | PASS |
| PSCR | 857 517 | **857 517** | — | — | PASS |
| STRID | 17 953 | **17 953** | — | — | PASS |
| HOT | 10 867 | **10 867** | — | — | PASS |
| DUP | 11 190 | **11 190** | — | — | PASS |
| PGLO | 17 018 | **17 018** | — | — | PASS |

Every published M8 cycle count is reproduced by arm B in this build, and C then matches B on
~90 counters: `bulk_desc_submitted/completed/cycles_sum`, `bulk_idx_lines_req/done/ooo`,
`idx_elems_req/done`, `idx_tile_swaps`, `idx_ready_max`, `bulk_tgt_addr_gen`,
`tgt_req_gen/issued/done`, `tgt_ooo`, `elems_total/done`, `addr_faults`, occupancy
(`buf_occ`, `buf_max`, `unissued_max`, `win_occ_max`, `win_total_max`, `dir_occ_max`), every stall
counter, the scheduler counters, `org_cycles`, `fanout_cycles/elems/max_backlog`, `ag_cycles`,
all seal/batch counters, the whole timeline (`first_desc_done` … `last_fill`), all L1/L2/DRAM/TLB
counters, `walks`, `pte_fetches`, `inflight_max`, `replays` and the latency percentiles.
Output equality is checked three ways: `engine_hash` (element id, address, loaded value folded in
element order) equals between C and B; `truth_hash` (image-derived expectation) equals between C
and B; `addr_mismatch == 0`. Conservation is `Ok` in both arms.

**Disclosure.** No M4–M8 report recorded a bulk `completion_hash`, so there is no historical value
to reconstruct. G2's hash equality is C against B re-run in this build. The historical anchor that
*does* exist — the cycle count — is reproduced exactly for all seven workloads, which is what makes
the comparison meaningful.

---

## 4. Gate G3 — hybrid correctness

Λ = 16384, P = 512, `op_class=gen`, three mixed points.

```bash
for k in 0.25 0.5 0.75; do ./bin/dx100sim $R --set Lambda=16384 --set P=512 --set kappa=$k; done
```

| κ | instructions (SIMT + bulk) | elements (SIMT + bulk) | C cycles | descriptors | index lines | value hash vs expectation | violations |
|---:|---|---|---:|---:|---:|---|---:|
| 0.25 | 1002 + 3094 | 4008 + 12376 | 153 475 | 1 | 774 | equal | 0 |
| 0.50 | 2031 + 2065 | 8124 + 8260 | 119 798 | 1 | 517 | equal | 0 |
| 0.75 | 3083 + 1013 | 12332 + 4052 | 95 194 | 1 | 254 | equal | 0 |

Checked per run, each a hard failure in `run_hybrid` or `run_m9`:

* **Partition is exhaustive and disjoint**: `simt_elems + bulk_elems == 16384`; counted twice, once
  from the class map and once from the compacted descriptor.
* **Identity survives compaction**: for every `j`, `cm[acc[elem_ids[j]].instr] == Bulk` and
  `index_values[j] * elem_bytes == acc[elem_ids[j]].addr`. `engine_hash` folds `elem_ids[j]`, so a
  mis-mapped element changes the hash.
* **Each element completes once**: `bulk_elems_done + bulk_addr_faults == bulk_elems`;
  SIMT `requests_generated == completed`; `elems_done` cannot exceed `elems_total` (engine asserts).
* **No slot/ownership error**: `SlotPool::consistent()` every cycle, `residency() == 0` at the end,
  the engine's `residency_violations == 0` and `addr_mismatch == 0`, no completion for a free slot,
  no double release (`give` returns false on either).
* **Exactly one descriptor iff bulk work exists**: 1 in all three rows; 0 in every G1 row.
* **Residency within the shared pool**: `peak_total_outstanding <= slot_pool_total` (128).
* **Index volume**: `idx_lines_req == ceil(bulk_elems / 16)` (774, 517, 254).
* **SIMT work is never indexed**: implied by the element check above and by
  `bulk_elems` being the descriptor count.

No lost completion, duplicate completion, address fault or conservation violation was observed at
any point.

---

## 5. Gate G4 — dispatch ordering

Ordering is fixed by construction, and the instrumentation shows it in the run:

1. `run_m9` builds the `ClassMap` (`build_op_classes`), then the `BulkWorkload`
   (`build_bulk_workload`), **then** constructs the `Sim`, then calls `run_hybrid`. Both derivations
   therefore complete before any cycle exists — before any descriptor object, before any index
   value, before any request.
2. Inside `run_hybrid`, `eng_->submit(w.desc)` happens before the first `mem_->tick`. The
   instrumented timeline confirms it: `bulk_first_idx_issue = 1` in every run (the first index
   request leaves in the first cycle of execution), `bulk_first_desc_done = 586`,
   `bulk_first_admit = 586`, and the first target issue is at 13 350 / 21 918 / 30 344 for
   κ = 0.25 / 0.5 / 0.75. Nothing index-shaped precedes the submission.
3. The index array is generated from `sel` only, so a SIMT-classified element is not merely ignored
   by the engine, it is never written: `print_m9` reports "12376 of 16384 elements entered the
   index array; 4008 SIMT elements were never indexed" (κ = 0.25).
4. Per-cycle order is `mem_->tick` → route completions by `SlotPool::owner` → release/attribute →
   engine generation → SIMT generation (`dispatch_order = 0`, the default). `dispatch_order = 1`
   swaps the last two steps for a sensitivity check (κ = 0.5: 138 059 cycles vs 119 798, gates
   still pass); it changes no classification and is not used for any number reported here.

`op_class=pattern` reproduces the `gen` map exactly on every tested workload (κ = 0.5:
2031 + 2065 instructions, 8124 + 8260 elements, identical 119 798 cycles), which is the cross-check
that the annotation is the generator's decision and not an artefact.

---

## 6. Gate G5 — resource accounting

Three quantities are reported separately and never summed into a single budget claim.

| Profile | slot pool | coalescer staging (peak) | max formed | A peak resident | B engine residency | C peak resident |
|---|---:|---:|---:|---:|---:|---:|
| `ab_charge_latch = 0` (shipped, `configs/ab_controlled.cfg:127`) | 128 | 4 (A) / 1 (C, κ=0.5) | ≤ 132 | 128 (+1 latched) | 128 | 128 |
| `ab_charge_latch = 1` (M6 fairness profile) | 124 | 4 (A) / 1 (C) | ≤ 128 | 124 | 128 | 124 |

* **Slot residency** is `SlotPool` occupancy: slots actually taken by a request, split by owner
  (SIMT vs engine) and reported as a peak, per path and total.
* **Coalescer staging** is the unformed remainder of the current instruction (`cur_lines - cur_pos`),
  bounded by `lanes = 4`. It is *not* a slot. At `ab_charge_latch = 0` it is free, so the shipped
  profile can have up to **128 + 4 = 132** accesses formed or resident at once; the design does not
  claim a strict 128 total under this profile.
* **Total effective capacity** = pool + staging, and it is stated per profile above. At
  `ab_charge_latch = 1` the pool is `R - lanes = 124` so that pool + latch = 128, which is the
  fairness profile M6 used.
* B's `bulk_buf_max` is the *engine's* residency, a different quantity from a slot count, and is
  reported in its own column rather than compared against A's slot peak.
* Comparisons in §2–§4 and §7 use the shipped `ab_charge_latch = 0` profile for every arm, i.e. the
  same profile as the corresponding baseline.

Hybrid under `ab_charge_latch = 1` (κ = 0.5) passes the same gates with pool 124, C peak 124
(SIMT 124, engine 116), staging 1, `addr_mismatch = 0`.

---

## 7. Measured result (mixed κ)

Only after G1–G5 passed. Λ = 16384, P = 512, `ab_charge_latch = 0`, M8 bulk profile
(`page_sched=1 idx_mode=2`), one descriptor, `dispatch_order = 0`. A and B are the two existing
paths on the same trace; C is the dispatched run.

| κ | A (SIMT only) | B (bulk only) | C (dispatched) | C / A | C / B | C vs min(A,B) |
|---:|---:|---:|---:|---:|---:|---:|
| 0.00 | 270 753 | 156 419 | **156 419** | 0.578 | 1.000 | 1.000 |
| 0.25 | 220 123 | 145 032 | **153 475** | 0.697 | 1.058 | **1.058** (loses) |
| 0.50 | 169 826 | 131 199 | **119 798** | 0.705 | 0.913 | **0.913** |
| 0.75 | 120 110 | 118 689 | **95 194** | 0.792 | 0.802 | **0.802** |
| 1.00 | 40 281 | 42 305 | **40 281** | 1.000 | 0.952 | 1.000 |

Why the sign changes between κ = 0.25 and κ = 0.50 is diagnosed cycle by cycle in **§9**.

Reading, not tuning:

* At κ = 0 and κ = 1 there is nothing to dispatch, and C collapses to B and to A respectively —
  which is exactly what G1 and G2 require and what makes the two middle rows trustworthy.
* Dispatch wins for κ ≥ 0.50 (up to 19.8 % below the better single path at κ = 0.75) and loses to
  all-bulk at κ = 0.25 by 5.8 %. The mixed result is not monotone in the way §7.4 of the design
  hypothesised. §9 measures the shape directly: it is not a simple function of class balance, and at
  the κ = 0.25 point it depends on the shared pool's capacity (§9.15).
* At κ = 0.25 the engine's first target issue is at cycle 13 350 of a 153 475-cycle run, so the
  accelerator spends the first ~9 % of the run in descriptor fetch and index acquisition while the
  SIMT front-end is already saturating the hierarchy. That is measured; the earlier reading that the
  4008 SIMT elements are then "paid for at SIMT's per-element rate" is a per-element arithmetic on
  two phase lengths and is **withdrawn** as an explanation — see §9.10 for why it is only an
  accounting split, and §9.12–§9.15 for what the contention measurement actually shows.
* Index traffic falls with the compacted count (774 / 517 / 254 lines vs B's 1024), which is the
  expected effect of filtering before derivation.
* Classification policy was **not** varied, tuned, or informed by these numbers: HOT and DUP remain
  bulk-capable, χ is untouched, and the class map is the generator's own draw.

`dispatch_order = 1` (SIMT generation before engine generation within a cycle) is 15 % slower at
κ = 0.5 (138 059 vs 119 798). It is reported as a sensitivity, not as a candidate configuration.

---

## 8. Regression

```bash
cd DX100_MEM/sim && make -j4                     # clean, -Wall -Wextra, no warnings
./bin/dx100_tests                                # 4032 checks, 0 failures
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=ab ...  # status: OK
```

* The 14 published anchor runs (7 workloads × {A, M8-B}) reproduce exactly — the A and B columns of
  §2 and §3 are those runs. Re-verified after the §9 instrumentation was compiled in, again after
  the contention instrumentation of §9.12–§9.15, and again after the index-pipeline / DRAM-scheduler
  instrumentation of §9.18: 14/14 MATCH, `experiment=ab` `status: OK` on all seven workloads,
  `dx100_tests` 4 032 / 0, and the three diagnostic runs return 153 475 / 119 798 / 95 194 exactly as
  before the change. All 21 capacity cells of §9.15 and all 21 cells of §9.18 (9 primary, 12 control)
  pass the M9 gates. The diagnostic is inert by measurement, not by assertion.
* The published A/B driver is unaffected: `experiment=ab` still returns `status: OK` with the
  established numbers, and it now **rejects** `dispatch != 0` with an explicit violation rather than
  silently ignoring it, so the two-arm comparison can never be mistaken for a three-arm one.
* `experiment=diag` unchanged.
* Determinism: two identical κ = 0.5 runs produce byte-identical output (`diff` clean).

---

## 9. Diagnostic — shared-slot contention and the κ dependence

No architecture, policy or scheduler change was made for this section. The instrument is:

* one read-only observation per simulated cycle (`CycleSample`), enabled only by `experiment=m9diag`;
* a claim/release stamp inside `SlotPool` (`take` writes the cycle, `give` tallies the age), also
  active only while a trace is attached;
* a refusal attribution recorded at the refusal site in `win_emit`, plus per-cycle readiness and
  progress flags derived from the counters;
* `Config::pool_slots`, a diagnostic-only override of the shared pool's capacity
  (§9.15). No published configuration sets it and arms A and B never read it.

When no trace is requested, the observation is a single null check, the pool's two diagnostic
pointers are null, and the drivers execute exactly the statements they executed before.
**Inertness was proved, not assumed:** with this instrumentation compiled in, all 14 published anchor
runs still reproduce exactly (§8), `experiment=ab` is still `status: OK`, `dx100_tests` is still
4 032 / 0, and the three diagnostic runs below return the same cycles as the previous revision
(153 475 / 119 798 / 95 194). Every run re-evaluates the M9 gates on its own data.

**What this revision changes.** The accounting identity `C = S + E_C` stands, but it is now labelled
as an accounting split rather than a model (§9.10). The earlier readings that the engine's "marginal
price" for regular elements explains the sign, and that the two paths "never execute concurrently",
are **not supported** by the sharper measurements: refusals are attributed event by event and turn
out to be bidirectional and κ-dependent (§9.12); residency overlap is common while progress overlap
is rare (§9.13); and at half the shared capacity the κ = 0.25 regression disappears with the
compaction unchanged (§9.15).

```bash
cd DX100_MEM/sim && make -j4
for k in 0.25 0.50 0.75; do
  ./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=m9diag \
    --set N=16384 --set Lambda=16384 --set P=512 --set kappa=$k --set rho_dup=0 \
    --set elem_bytes=4 --set bulk_wl=E0 --set seed=1 --set batch_window=8192 \
    --set bulk_max_elems=262144 --set max_cycles=200000000 \
    --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
    --set page_sched=1 --set idx_mode=2
done
```

Definitions used below, all fixed before the runs: **S** = the cycle of the last SIMT completion in
C (the end of the SIMT phase); **E_C = C − S** = the remainder, during which only the engine
completes anything; **A** and **B** are the two single-class arms at the same κ, footprint and seed.

### 9.1 Timeline (item 1)

| κ | arm | first request | first SIMT completion | last SIMT completion | first bulk element | last bulk element | run end |
|---:|---|---:|---:|---:|---:|---:|---:|
| 0.25 | A | 1 | 338 | 220 123 | – | – | 220 123 |
| 0.25 | B | 0 | – | – | 4 637 | 145 031 | 145 032 |
| 0.25 | **C** | 1 | 848 | **14 374** | 13 623 | 153 474 | **153 475** |
| 0.50 | A | 1 | 338 | 169 826 | – | – | 169 826 |
| 0.50 | B | 0 | – | – | 4 637 | 131 198 | 131 199 |
| 0.50 | **C** | 1 | 848 | **23 079** | 22 149 | 119 797 | **119 798** |
| 0.75 | A | 1 | 338 | 120 110 | – | – | 120 110 |
| 0.75 | B | 0 | – | – | 4 637 | 118 688 | 118 689 |
| 0.75 | **C** | 1 | 848 | **31 510** | 30 362 | 95 193 | **95 194** |

| κ | B: first index issue | B: index done | B: first target | C: first index issue | C: index done | C: first target | C: descriptor fetch done (B) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 1 | 89 290 | 4 115 | 1 | 90 347 | **13 350** | 586 (337) |
| 0.50 | 1 | 90 591 | 4 115 | 1 | 8 812 | **21 918** | 586 (337) |
| 0.75 | 1 | 90 305 | 4 115 | 1 | 3 897 | **30 344** | 586 (337) |

Two facts that matter later. First, C's SIMT phase is short at every κ — 9.4 %, 19.3 %, 33.1 % of
the run — and the engine's *last* element lands within one cycle of the end. Second, C's first target
issue sits just below S in all three runs (13 350 < 14 374; 21 918 < 23 079; 30 344 < 31 510), i.e.
the engine begins its target work shortly before the SIMT phase ends, not long after it.

### 9.2 Index acquisition and overlap with SIMT (item 2)

| κ | C index window | C descriptor fetch done | B descriptor fetch done | SIMT requests completed when the window opens | SIMT completion cycles inside the window | bulk element completion cycles inside the window |
|---:|---|---:|---:|---:|---:|---:|
| 0.25 | 1..90 347 | 586 | 337 | 0 of 1 002 | 1 002 | 4 884 |
| 0.50 | 1..8 812 | 586 | 337 | 0 of 2 031 | 493 | 0 |
| 0.75 | 1..3 897 | 586 | 337 | 0 of 3 083 | 176 | 0 |

* At κ = 0.25 the **entire** SIMT phase (1 002 of 1 002 requests) is executed inside the index
  acquisition window, which stays open until cycle 90 347 — 6.3× past S. The engine is therefore
  admitting elements from a stream that is still arriving, which is why its occupancy is low in
  §9.4 even though the pool is almost empty.
* At κ = 0.50 and 0.75 the index stream finishes *before* the SIMT phase does (8 812 and 3 897 cycles
  against S = 23 079 and 31 510), so after S the engine is not waiting on index lines; §9.12 measures
  what it is waiting on instead.
* The descriptor fetch takes 586 cycles in C against 337 in B — the same line, 249 cycles later,
  because it is fetched through a hierarchy that the SIMT client is saturating.

### 9.3 Completion split (item 3)

| κ | C: last SIMT completion | C: last bulk element | cycles both paths completed | cycles only SIMT | cycles only bulk | cycles neither | cycles both paths hold slots |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 14 374 | 153 474 | **15** | 987 | 12 091 | 140 383 | 11 953 |
| 0.50 | 23 079 | 119 797 | **27** | 2 003 | 7 736 | 110 033 | 9 973 |
| 0.75 | 31 510 | 95 193 | **25** | 3 058 | 3 583 | 88 529 | 5 063 |

**Both paths complete an element in the same cycle on 15, 27 and 25 cycles respectively** out of
153 475, 119 798 and 95 194. What the measurements support is narrower than "the paths never run
concurrently": they share the pool for 11 953 / 9 973 / 5 063 cycles (§9.13), but useful progress in
the same cycle is rare, and each path's *phase* dominates its own interval — SIMT completes nothing
after S and the engine's first target issue lands just before S (§9.1). §9.10 treats `C = S + E_C`
as the accounting identity it is, and §9.14–§9.17 measure the contention that produces this shape.

### 9.4 Shared SlotPool occupancy (item 4)

128 slots at `ab_charge_latch = 0`. Means are over the whole run of that arm; the peak column is C's.

| κ | arm | SIMT mean slots | engine mean slots | coalescer latch mean | hierarchy inflight mean |
|---:|---|---:|---:|---:|---:|
| 0.25 | A | **127.5** | 0.0 | 2.3 | 88.0 |
| 0.25 | B | – | n/a (own pool) | – | 61.4 |
| 0.25 | C | 7.2 | **59.7** | 0.1 | 64.9 |
| 0.50 | A | **127.4** | 0.0 | 2.1 | 77.2 |
| 0.50 | B | – | n/a (own pool) | – | 54.2 |
| 0.50 | C | 19.8 | **37.8** | 0.2 | 46.7 |
| 0.75 | A | **127.3** | 0.0 | 1.7 | 58.7 |
| 0.75 | B | – | n/a (own pool) | – | 50.6 |
| 0.75 | C | 39.2 | **9.0** | 0.3 | 20.7 |

Arm B has no shared pool (the engine owns its private slot list at `R = 128`, peak residency
`bulk_buf_max = 128`), so a pool column would be meaningless there; its occupancy is reported through
the hierarchy inflight mean, which is the engine's own residency because it is the only client.

C's occupancy over time (cycle: slots SIMT / engine / free, hierarchy inflight):

| cycle (κ = 0.25) | SIMT | engine | free | inflight |
|---:|---:|---:|---:|---:|
| 0 | 1 | 65 | 62 | 1 |
| 15 347 | 0 | 11 | 117 | 11 |
| 30 695 | 0 | 7 | 121 | 7 |
| 46 042 | 0 | 4 | 124 | 4 |
| 61 390 | 0 | 16 | 112 | 16 |
| 76 737 | 0 | 14 | 114 | 14 |
| 92 085 – 138 127 | 0 | **120** | 8 | 120 |
| 153 475 | 0 | 0 | 128 | 0 |

At κ = 0.25 the engine holds **4–17 of 128 slots for cycles 23 959–83 858** while 111–124 slots sit
free — it cannot admit elements faster than the index stream delivers them (§9.2) — and only reaches
120 slots after cycle 92 085. At κ = 0.50 and 0.75 the SIMT client holds **all 128 slots (free = 0)**
for most of its phase (e.g. κ = 0.5 at cycles 11 979, 23 959; κ = 0.75 at 9 519, 19 038, 28 558).
These are occupancies; §9.12 attributes the refusals that occur at those occupancies, and they are
not always caused by the other client.

### 9.5 Translation (item 5)

| κ | arm | dTLB misses | refusals: parked | refusals: MSHR full | walks | max parked/entry |
|---:|---|---:|---:|---:|---:|---:|
| 0.25 | A | 12 298 | 212 | 290 183 | 512 | 2 |
| 0.25 | B | 2 305 | 158 013 | 0 | 541 | 2 |
| 0.25 | **C** | 2 375 | **183 436** | 0 | 539 | 2 |
| 0.50 | A | 8 213 | 3 078 | 286 126 | 512 | 2 |
| 0.50 | B | 2 323 | 155 088 | 12 | 541 | 2 |
| 0.50 | **C** | 1 820 | **210 050** | 8 | 522 | 2 |
| 0.75 | A | 4 077 | 19 651 | 255 993 | 512 | 2 |
| 0.75 | B | 2 069 | 143 031 | 2 068 | 538 | 2 |
| 0.75 | **C** | 1 206 | **242 146** | 0 | 517 | 2 |

The two single-class arms are throttled by *different* translation limits: SIMT misses on most probes
and saturates the 4-entry miss station (`refuse_full ≈ 2.6–2.9 × 10⁵`), while the engine's page
ordering converts the same footprint into 5× fewer misses and hits the *per-page park limit instead*
(`refuse_parked ≈ 1.4–1.6 × 10⁵`, `refuse_full ≈ 0`). C inherits the engine's regime (misses 2 375 /
1 820 / 1 206, full refusals 0) but pays **16 %, 35 % and 69 % more parked refusals than B** as κ rises.
Walk counts are essentially unchanged (539/522/517 against B's 541/541/538), so the extra refusals are
not extra walks — they are more requests waiting behind the same pages.

### 9.6 L1/L2/DRAM (item 6)

| κ | arm | L1 requests | L1 MSHR joins | L2 requests | L2 hits | DRAM reads | row hits | row conflicts | bank busy | blocked | starved |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | A | 13 378 | 9 | 14 378 | 1 593 | 12 783 | 508 | 12 273 | 326 262 | 154 288 | 53 053 |
| 0.25 | B | 14 146 | 0 | 15 228 | 1 344 | 13 872 | 10 797 | 3 073 | 231 108 | 122 062 | 9 099 |
| 0.25 | C | 14 153 | 4 | 15 214 | 1 573 | 13 609 | 10 470 | 3 137 | 228 194 | 106 514 | 33 353 |
| 0.50 | A | 10 291 | 24 | 11 267 | 1 868 | 9 397 | 1 204 | 8 191 | 229 874 | 109 744 | 50 686 |
| 0.50 | B | 10 938 | 0 | 12 020 | 1 586 | 10 427 | 7 623 | 2 802 | 179 626 | 104 657 | 16 116 |
| 0.50 | C | 10 809 | 1 | 11 851 | 1 922 | 9 900 | 7 842 | 2 056 | 163 296 | 78 909 | 30 990 |
| 0.75 | A | 7 135 | 14 | 8 119 | 1 704 | 6 413 | 2 373 | 4 038 | 138 262 | 67 437 | 46 261 |
| 0.75 | B | 7 878 | 0 | 8 954 | 1 513 | 7 438 | 5 201 | 2 235 | 130 976 | 70 867 | 40 385 |
| 0.75 | C | 7 390 | 0 | 8 424 | 1 757 | 6 666 | 5 591 | 1 073 | 106 224 | 55 963 | 32 566 |

C's target stream keeps the engine's row behaviour: row hits 75–84 % and row conflicts 1 073–3 137,
against A's 4–24 % hits and 4 038–12 273 conflicts at the same footprint. C's `DRAM starved` is 2–3.7×
B's (33 353 / 30 990 / 32 566 against 9 099 / 16 116 / 40 385) — a large fraction of C's cycles have
the DRAM bank idle with requests waiting elsewhere, which is the signature of a phase that is not
keeping the banks fed.

The engine's own organization, B against C on the same configuration:

| κ | elements | target requests | dedup merges | page-run opens | run deferrals | window occupancy peak | batches |
|---:|---|---:|---:|---:|---:|---:|---:|
| 0.25 B | 16 384 | 13 121 | **3 263** | 1 534 | 154 069 | 8 192 | 3 |
| 0.25 C | 12 376 | 12 376 | **0** | 1 536 | 147 018 | 4 736 | 3 |
| 0.50 B | 16 384 | 9 913 | **6 471** | 1 516 | 150 920 | 8 192 | 3 |
| 0.50 C | 8 260 | 8 260 | **0** | 1 044 | 134 966 | 4 736 | 3 |
| 0.75 B | 16 384 | 6 853 | **9 531** | 1 399 | 141 149 | 8 192 | 3 |
| 0.75 C | 4 052 | 4 052 | **0** | 512 | 125 018 | 4 052 | 1 |

**C's compacted stream has no deduplication at all** — `target requests == elements` at every κ — while
B's merges 3 263 / 6 471 / 9 531 duplicate lines. C therefore issues 1.00 target request per element
against B's 0.80 / 0.61 / 0.42, and reads 1.10 / 1.20 / 1.65 DRAM lines per element against B's
0.85 / 0.64 / 0.45. C's page runs also shorten: 0.124 / 0.126 / 0.126 opens per element against B's
0.094 / 0.093 / 0.085.

### 9.7 Memory-system utilization and no-progress periods (item 7)

| κ | arm | cycles hierarchy empty | SIMT issue refused | engine slot stall (free ≤ 8) | engine index wait | engine window full |
|---:|---|---:|---:|---:|---:|---:|
| 0.25 | A | 2 | 73 161 | – | – | – |
| 0.25 | B | 4 | – | 58 859 | 6 061 | 2 811 |
| 0.25 | C | 4 | 8 571 | **70 752** | 7 333 | 1 839 |
| 0.50 | A | 2 | 72 842 | – | – | – |
| 0.50 | B | 4 | – | 46 997 | 6 051 | 2 822 |
| 0.50 | C | 4 | 18 491 | **44 355** | 5 615 | 1 549 |
| 0.75 | A | 2 | 69 509 | – | – | – |
| 0.75 | B | 4 | – | 39 726 | 6 579 | 2 838 |
| 0.75 | C | 2 | 29 126 | **27 466** | 2 639 | 1 181 |

`engine slot stall` is the engine's own guard in `win_issue` (`bulk.cpp:938`): it declines to issue a
target while the pool has ≤ `idx_reserve` (8) free slots. It fires for 46 %, 37 % and 29 % of all
cycles in C, but it mixes two different blocks — a genuinely empty pool and the 8 slots reserved for
the index stream — so it is **not** by itself a measure of contention with SIMT. §9.12 splits it event
by event. `hierarchy empty` is 2–4 cycles in every arm, so neither path is ever starved for lack of
work; both are waiting on the pool or on translation.

### 9.8 Overhead inside the measured cycles (item 8)

| component | charged to C's cycle count? | measured |
|---|---|---|
| classification, compaction, index-array generation | **No** | host-side, completed before cycle 0; C's first request arrives at cycle 1, the same as A's |
| descriptor fetch | Yes | 586 cycles (`bulk_first_desc_done`), against 337 in B |
| index acquisition | Yes | window 1..90 347 / 8 812 / 3 897; 774 / 517 / 254 lines |
| target stream | Yes | `bulk_tgt_req_gen` = 12 376 / 8 260 / 4 052 lines |
| synchronization / barriers / handshakes | **None exist** | one clock, one `mem_->tick` per cycle; `run_hybrid` adds no cycle and no handshake |
| slot-pool bookkeeping | **No** | host-side, inside the same cycle |

There is no dispatch, compaction or synchronization cost hidden in C's cycle count. The only dispatch
cost that appears in time is the descriptor fetch, and it is 249 cycles of extra latency inside a
153 k / 120 k / 95 k-cycle run — i.e. 0.16 %, 0.21 %, 0.26 %.

### 9.9 Traffic comparison

| κ | line requests A / B / C | DRAM bytes A / B / C | C ÷ A | C ÷ B |
|---:|---|---|---:|---:|
| 0.25 | 13 378 / 14 146 / 14 153 | 818 112 / 887 808 / 870 976 | 1.065 | **0.981** |
| 0.50 | 10 291 / 10 938 / 10 809 | 601 408 / 667 328 / 633 600 | 1.054 | **0.949** |
| 0.75 | 7 135 / 7 878 / 7 390 | 410 432 / 476 032 / 426 624 | 1.039 | **0.896** |

C always moves less DRAM traffic than B and more than A. At κ = 0.25 C moves **1.9 % less traffic than
B and still takes 5.8 % longer**, so the extra traffic is not what costs the regression.

### 9.10 Accounting decomposition (not a causal attribution)

`S` and `E_C` are two measured spans of C's own timeline, so `C = S + E_C` is an identity by
definition, not a model. `E_C` is **not** an isolated engine execution time: it is everything that
remains after the last SIMT completion, including any interference the engine still suffers, any
drain, and any tail. The table below is therefore an accounting split of C's cycles, and nothing in
it should be read as a per-element price:

| κ | S (last SIMT completion, cycles) | S ÷ C | E_C = C − S | E_C ÷ C | B (measured) | C − B |
|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 14 374 | 9.4 % | 139 101 | 90.6 % | 145 032 | **+8 443** |
| 0.50 | 23 079 | 19.3 % | 96 719 | 80.7 % | 131 199 | **−11 401** |
| 0.75 | 31 510 | 33.1 % | 63 684 | 66.9 % | 118 689 | **−23 495** |

The identity `C − B = S − (B − E_C)` follows algebraically from the definitions (+8 443, −11 401,
−23 495). It reorganises the comparison; it does not explain it. In particular:

* `E_C` is not "the time the engine needs for the bulk share". It is the tail of a run in which the
  engine had the pool largely to itself, and it contains any residual translation/DRAM interference
  from the SIMT phase.
* `B − E_C` was previously read as "the engine's marginal price for the regular elements". Nothing
  in this experiment isolates that quantity. It is the difference between two durations measured in
  machines with different occupancies and different amounts of other work in flight. Obtaining the
  real marginal cost would need a run in which only the regular-share elements are dispatched (or a
  paired trace with those elements removed), which was not run.
* What *is* directly measured is the request-level consequence of compaction: B's engine merges
  3 263 / 6 471 / 9 531 duplicate lines, while C's compacted stream merges **zero** and therefore
  issues 1.00 target request per element against B's 0.80 / 0.61 / 0.42 (§9.6). That is a real
  effect of the class split on the engine's traffic. How much of the sign it explains is not
  measured — §9.17 shows the sign at κ = 0.25 reverses with the dedup behaviour untouched.

### 9.11 Verdict: which mechanism dominates

| candidate cause | measured status |
|---|---|
| **shared-resource contention** | **Real, bidirectional, and directly attributed.** SIMT holds 127.3–127.5 of 128 slots (mean) during its phase and free slots are 0. Refusals are attributed at the instant they occur (§9.14): at κ = 0.25 the engine's target refusals happen while it holds 109.6 slots itself and SIMT 11.3 (and 89 % of them have 1–8 free, i.e. the index reserve, not a full pool);   at κ = 0.75 they happen while SIMT holds 125.5 and the engine 2.1. Simultaneous progress is 132–134 cycles in the whole run (§9.13). |
| **acquisition latency** | **Not the cause of the κ = 0.25 loss.** The descriptor fetch is 586 cycles (249 later than B) and C's first target issue is 13 350 — *before* S = 14 374. The prologue is largely hidden behind the SIMT phase. It does explain the engine's low occupancy in 23 959–83 858 (4–17 of 128 slots, 111–124 free) because the index window stays open to 90 347. |
| **additional traffic** | **Not the cause.** C moves 1.9 % *less* DRAM traffic than B at κ = 0.25 and still loses. Within C's engine phase the stream is traffic-heavier per element (1.10 vs 0.85 DRAM lines/element) because compaction removes the duplicates, but the aggregate is lower. |
| **useful overlap** | **Rare, not absent, and measurable.** 15 / 27 / 25 cycles of simultaneous completion; 134 / 132 / 134 cycles in which both clients made a forward step while both were active; 11 953 / 9 973 / 5 063 cycles in which both held slots. Residency overlap is common and progress overlap is not. |
| **what decides the sign** | At the published capacity, the balance of the two phase lengths `S` and `E_C` — an accounting fact, not a per-element price (§9.10). Why `E_C` is long at low κ is not settled by that split: §9.17 shows it is a *capacity-response* effect, not a fixed cost of the regular elements, because halving the shared pool shortens `E_C` by 28 883 cycles at κ = 0.25 with the engine's traffic unchanged. |

Secondary, measured, and worth recording: C's engine span per bulk element rises with κ —
11.24 → 11.71 → 15.72 cycles/element against B's 8.85 → 8.01 → 7.24 — with 512 page-run opens for
4 052 elements at κ = 0.75 and the engine's mean occupancy down at 9.0 slots. C still wins at
κ = 0.75 because 12 332 elements leave the engine entirely. The reading that the engine's *efficiency*
falls (rather than that its phase contains fewer elements and a different pool occupancy) is an
inference from those counts, not a measurement.

### 9.12 Allocation, refusal and release (item 1)

Every claim and every release is counted at the pool operation itself, and every refusal is
attributed at the instant it happens by reading the real owner holdings — not reconstructed from an
end-of-cycle snapshot and not inferred from a stall counter. All values below are direct counter
readings from `experiment=m9diag` at R = 128, latch free.

| κ | SIMT claims | SIMT cycles with a line ready and no slot | engine claims | engine target refusals | of which free = 0 | of which 0 < free ≤ 8 |
|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 1 002 | 12 239 | 160 169 | 70 752 | 7 693 | **63 059** |
| 0.50 | 2 031 | 19 764 | 143 744 | 44 355 | 15 226 | 29 129 |
| 0.75 | 3 083 | 27 124 | 129 325 | 27 466 | **23 541** | 3 925 |

Who held the slots when the refusal happened (mean over refusal events):

| κ | SIMT refused while holding | … engine holding | … free | engine refused while SIMT holding | … engine holding | … free |
|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 77.2 | 50.8 | 0.0 | 11.3 | **109.6** | 7.0 |
| 0.50 | 104.4 | 23.6 | 0.0 | 46.6 | **76.4** | 4.9 |
| 0.75 | 120.4 | 7.6 | 0.0 | **125.5** | 2.1 | 0.4 |

Reconciliation: claims − releases = 0 in every run, and `free = 0 + reserved = slot_stall` exactly
(70 752 / 44 355 / 27 466), so the attribution partitions the engine's refusals without a residual.

**The engine is not blocked by "SIMT holding the pool" at κ = 0.25.** Its refusals there happen with
109.6 of 128 slots in its own hands and 11.3 with SIMT, and 89 % of them have between 1 and 8 free
slots — that is the engine's own guard (`idx_reserve = 8`, `bulk.cpp:938`), which keeps the last 8
slots for the index stream. The picture inverts with κ: at 0.75, 86 % of refusals are on a genuinely
empty pool while SIMT holds 125.5 slots. The SIMT side is complementary — when SIMT cannot form a
line, SIMT itself already holds 77.2 / 104.4 / 120.4 slots, and the engine holds the remainder.

One more measured fact that reframes "refusals": the engine claims 160 169 / 143 744 / 129 325
slots but issues only 13 151 / 8 778 / 4 307 requests (descriptor + index + target). The difference —
147 018 / 134 966 / 125 018 claims, i.e. **92 / 94 / 97 %** — is a take and a release inside the same
cycle: a target whose submission the hierarchy refused on translation. Those claims never occupy a
slot between cycles (see §9.14), so they are issue *opportunities* lost, not capacity held.

### 9.13 Readiness versus progress (item 2)

Over the cycles in which both clients still had work to do (the SIMT phase), per cycle:

| κ | both active | both progressed | only SIMT | only engine | neither | blocked: both / SIMT only / engine only / neither |
|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 14 374 | **134** | 1 994 | 817 | 11 429 | 7 845 / 4 394 / 1 745 / 390 |
| 0.50 | 23 079 | **132** | 4 247 | 748 | 17 952 | 15 370 / 4 394 / 2 866 / 449 |
| 0.75 | 31 510 | **134** | 6 565 | 478 | 24 333 | 23 528 / 3 596 / 3 938 / 448 |

"Progress" is a forward step of any kind: a SIMT request formed, issued or completed; an engine
target issued, a line or element completed, or an index line fetched. "Blocked" is a residency
refusal in that cycle. "Neither progressed" is dominated by ordinary memory latency (neither path
has a completion or an issue slot in that cycle), so it is not evidence of contention by itself.

Two cross-tabulations sharpen it:

| κ | SIMT blocked, engine progressed | SIMT blocked, engine did not | engine blocked, SIMT progressed | engine blocked, SIMT did not |
|---:|---:|---:|---:|---:|
| 0.25 | 589 | 10 601 | 1 568 | 7 558 |
| 0.50 | 434 | 17 058 | 3 817 | 14 072 |
| 0.75 | 189 | 23 416 | 6 177 | 21 153 |

The "SIMT only" blocked column is dominated by the engine's prologue — the span in which SIMT is
already residency-blocked and the engine has no target-bearing window yet. It is not κ-dependent: a
diagnostic check at κ = 0.25 with `batch_window` 1 024 / 4 096 / 8 192 gives 776 / 3 660 / 4 394 for
that column (first target issue 10 129 / 12 912 / 13 350), which is why it is almost constant across κ
at the published W = 8 192.

**Simultaneous residency is common; simultaneous useful progress is not.** Both clients held slots in
11 953 / 9 973 / 5 063 cycles, but both made a forward step in only 134 / 132 / 134 of them — the same
order as the completion counts in §9.3. And a SIMT client that is blocked for a slot is not a window
in which the engine runs: the engine progressed in only 589 / 434 / 189 of the 11 190 / 17 492 /
23 605 such cycles. Both clients are, most of the time, waiting on the same hierarchy.

### 9.14 Slot lifetime (item 3)

Claims are stamped when the slot is taken and tallied when it is released, so the sample is the whole
population (claims tallied = claims made, 1002/1002, 160 169/160 169, and so on). Claims released in
the same cycle they were made are counted separately instead of being averaged into a request
latency; they are the translation-refused submissions of §9.12.

| κ | client | requests | mean (cy) | p50 | max (cy) | zero-age claims |
|---:|---|---:|---:|---|---:|---:|
| 0.25 | SIMT | 1 002 | 1 099.6 | 1024–2047 | 1 967 | 0 |
| 0.25 | engine | 13 151 | 697.1 | 512–1023 | 1 309 | 147 018 |
| 0.50 | SIMT | 2 031 | 1 167.1 | 1024–2047 | 1 967 | 0 |
| 0.50 | engine | 8 778 | 516.5 | 256–511 | 1 463 | 134 966 |
| 0.75 | SIMT | 3 083 | 1 209.5 | 1024–2047 | 1 967 | 0 |
| 0.75 | engine | 4 307 | 198.7 | 64–127 | 1 221 | 125 018 |

`Σ ages` equals the occupancy integral exactly in every row (e.g. 1 101 834 for SIMT and 9 167 959
for the engine at κ = 0.25), which is the Little's-law cross-check that the tally and the occupancy
observations describe the same slots.

SIMT requests are the long-lived ones: 1.6× / 2.3× / 6.1× the engine's mean, and their lifetime is
nearly flat across κ (1 100 → 1 210 cycles, +10 %) while the engine's falls 3.5×. Neither path has a
few pathological long requests — the maxima are 1 967 and 1 309–1 463 cycles, i.e. the same order as
the means. The asymmetry is structural rather than a long-tail effect: SIMT continuously refreshes
~127 in-flight requests, so its per-request lifetime is the memory system's queueing delay at that
occupancy, while the engine's stream both shortens and shrinks as κ rises. The engine's large
zero-age population is a *burst* of attempts that hold no slot between cycles, so it does not take
capacity from SIMT; it loses issue opportunities to translation.

### 9.15 Controlled capacity sensitivity (item 4)

Diagnostic only: `Config::pool_slots` overrides the shared pool's capacity in `run_hybrid` and
nothing else. R stays 128, so the engine's internal limits (`idx_reserve = 8`, index window, page
scheduler) are the published ones. No published configuration sets it, and arms A and B never read
it. For the controls, A and B were re-run with `R = N`, which is the same total budget given to a
single-client machine.

| capacity | κ = 0.25: C / A / B | κ = 0.50: C / A / B | κ = 0.75: C / A / B |
|---:|---|---|---|
| 32 | 136 518 / 220 139 / 160 197 | 110 122 / 169 812 / 145 921 | 102 833 / 119 902 / 129 553 |
| 64 | **125 778** / 220 123 / 150 406 | **94 763** / 169 826 / 136 029 | 95 899 / 120 087 / 120 921 |
| 96 | 145 468 / 220 123 / 145 306 | 113 233 / 169 826 / 131 340 | 95 605 / 120 118 / 118 480 |
| **128 (published)** | 153 475 / 220 123 / 145 032 | 119 798 / 169 826 / 131 199 | 95 194 / 120 110 / 118 689 |
| 160 | 158 974 / 220 123 / 144 788 | 126 090 / 169 826 / 130 717 | 94 785 / 120 134 / 117 636 |
| 192 | 162 465 / 220 123 / 144 687 | 127 453 / 169 826 / 130 684 | 94 437 / 120 123 / 118 280 |
| 256 | 162 826 / 220 111 / 147 425 | 127 752 / 169 826 / 132 069 | **93 900** / 120 122 / 119 155 |

All 21 cells report `M9 GATES PASSED` (G3 on each mixed cell; G1/G2 are separate all-simt/all-bulk
runs that the override cannot reach, since only `run_hybrid` reads it). Three measured responses:

1. **A is insensitive to capacity** (≤ 0.1 % across 32…256). SIMT does not convert outstanding slots
   into throughput at this footprint; its 127-slot occupancy is not a throughput requirement.
2. **B is shallow:** 145 032 → 144 687 (κ = 0.25), 131 199 → 130 684 (κ = 0.50), 118 689 → 117 636
   (κ = 0.75) over 128…192, and worse above.
3. **C is non-monotone**, with a minimum that moves right as κ rises: 64 at κ = 0.25 and 0.50, and at
   κ = 0.75 the whole 96–256 range lies within 1.8 % of the best cell. At κ = 0.25 the published 128
   is 27 697 cycles *worse* than 64.

Phase split at the minima (same definitions as §9.10):

| point | S | E_C | C | vs published 128 |
|---|---:|---:|---:|---:|
| κ = 0.25, pool 64 | 15 560 | 110 218 | 125 778 | S +1 186, **E_C −28 883**, C −27 697 |
| κ = 0.50, pool 64 | 23 811 | 70 952 | 94 763 | S +732, **E_C −25 767**, C −25 035 |
| κ = 0.75, pool 256 | 30 831 | 63 069 | 93 900 | S −679, E_C −615, C −1 294 |

So the whole of the low-κ gain is in the engine's span; the SIMT phase gets slightly *slower*, as it
must when it has half the slots. At equal aggregate capacity the hybrid is ahead of the dedicated
engine at every κ: 125 778 vs 150 406, 94 763 vs 136 029, 95 899 vs 120 921 for 64 slots each.

What this does and does not establish:

* It **does not** show that capacity is the cause of the κ = 0.25 regression in the sense of "the
  hybrid needs more slots". More capacity makes it worse. The regression at 128 is a
  capacity-response effect of sharing a pool that is larger than the useful concurrency of this
  machine; it disappears at 64 while the class map and the engine's stream are unchanged (12 376
  target requests and 0 dedup merges at both capacities; DRAM reads 13 575 vs 13 609).
* It **does not** support "the improvement is free". 64 slots is half the published budget, and at
  160–256 the runs are 5.5 k–9.4 k cycles *slower* at κ = 0.25 with a larger pool than published.
* The mechanism behind the non-monotonicity was **inferred** here, not measured. The signature at
  κ = 0.25, pool 64 against pool 128: DRAM row hits 11 443 vs 10 470, row conflicts 2 130 vs 3 137,
  DRAM starved 11 685 vs 33 353 cycles, bank busy 215 634 vs 228 194 over 18 % fewer cycles, and
  engine target refusals 45 690 vs 70 752. §9.18 replaces this inference with a controlled measurement
  and finds that the row-hit and starvation differences follow the window partitioning the pool induces,
  not the number of requests in flight: with the partial-seal deadline removed, row-hit rate is
  83.8–84.3 % at 64, 96 and 128 slots alike while the in-flight count still differs by 18 requests, and
the capacity spread collapses from 18 % to 1.1 %.
* The comparison is **C-only** for the pool override; A and B are unchanged controls, and the
  `R = N` columns are a separate machine (their internal limits move with R), so the two must not be
  read as one experiment.

### 9.16 Measured versus inferred

**Directly measured** (a counter, a per-cycle observation, or an event recorded at the point where it
happened): every number in §9.1–§9.9; S, E_C and B in §9.10; all of §9.12–§9.15, including the
refusal attributions (read at the refusal instant), the claim/release counts, the lifetime histogram
(each claim stamped and tallied), the readiness/progress flags, and the capacity sweep's cycle
counts. The identities `C = S + E_C`, `C − B = S − (B − E_C)`, `claims − releases = 0`,
`free=0 + reserved = slot_stall`, and `Σ ages = occupancy integral` are arithmetic on measured
values, i.e. bookkeeping checks — they are not evidence about causes.

**Inferred** (a causal reading, stated as such):

1. That the *shape* of the run (one phase then the other) is produced by pool contention rather than
   by the workload or the driver. Measured: free slots 0 during S; the attribution of refusals in
   §9.12 (which is genuinely bidirectional and κ-dependent); 132–134 cycles of simultaneous progress
   against 5 063–11 953 cycles of simultaneous residency (§9.13). The causal step from "the pool is
   full" to "therefore the engine cannot overlap" is still an interpretation: the counter shows the
   engine was refused, not that it would have progressed otherwise.
2. That compaction's removal of duplicate lines is *a* reason C's engine stream issues one request per
   element where B issues 0.80 / 0.61 / 0.42. The request and merge counts are measured; that each
   request exists *because* its duplicate partner left the class is not something the simulator
   labels. §9.15 also shows the κ = 0.25 sign reversing with the merge count still at zero, so this
   effect is not sufficient to explain the sign.
3. ~~That the low-κ engine span is lengthened by oversubscription of translation and DRAM scheduling.~~
   Corrected by §9.18: the span is lengthened by the interaction between the shared pool and the
   window-seal policy (3 windows instead of 2, the generator blocked for 79 045 cycles with no window
   to fill). The row-conflict and starvation differences (2 130 vs 3 137; 11 685 vs 33 353 cycles) are
   real but follow that partitioning: they disappear when the deadline is removed while concurrency is
   left free to differ. The dTLB-refused term (72 790 vs 72 181 cycles) is capacity-independent.
4. That SIMT's near-flat lifetime (1 100 → 1 210 cycles) reflects a queueing delay at fixed
   occupancy rather than a property of its requests. The lifetimes and occupancies are measured; the
   queueing reading is an interpretation of them.
5. The location of the capacity minima (64, 64, 96–256) and of the published-capacity crossover
   (between κ = 0.25 and 0.50) are properties of this footprint and seed. Nothing here shows they are
   universal, and no policy was changed to move them.

### 9.17 Limitations of this diagnostic

1. One seed (`seed = 1`), one footprint (Λ = 16384, P = 512). The capacity response was measured at
   seven pool sizes, but only at this footprint; the crossover and the minima are not claimed to be
   elsewhere.
2. Sampling is once per cycle, at the end of the cycle. Intra-cycle order (`dispatch_order = 0`:
   engine progress, then SIMT generation) is not visible; a `dispatch_order = 1` run at κ = 0.5 gives
   138 059 cycles against 119 798, so the intra-cycle order is not neutral, but no conclusion here
   depends on it.
3. S is the last SIMT completion, not a hard phase boundary; C has 5 063–11 953 cycles in which both
   clients hold slots. The decomposition is therefore exact only in the sense that E_C is *defined* as
   C − S; its interpretation as "the engine's phase" rests on the overlap counts in §9.3.
4. `bulk_slot_stall` alone does conflate "the other client holds the pool" with "the engine's own
   targets hold the pool"; §9.12 does not use it for attribution. The attribution comes from the
   owner holdings read at the refusal instant, and the two partitions reconcile exactly with the
   stall counter.
5. The capacity experiment is diagnostic-only and C-only. `pool_slots` changes no other parameter,
   but any pool size above R permits more than R line requests outstanding, which is outside the
   published residency claim; such a point is a capacity experiment, not a free improvement. The
   `R = N` columns for A and B are a different machine (their internal limits scale with R) and are
   controls, not the same experiment.
6. Peak-residency columns in `print_m9` are per-client maxima and can be reached at different cycles;
   when the pool is overridden they do not sum to the pool size. No statement here uses that sum.
7. Lifetime percentiles are bucket bounds (the histogram is logarithmic), so `p50` reads as a range,
   not an exact cycle. Zero-age claims are excluded from the request statistics and reported
   separately.
8. Arm B has no shared pool, so its pool columns are reported as n/a and its claim lifetime is not
   instrumented (the tally lives in `SlotPool`, which only the hybrid constructs); its residency is
   the hierarchy inflight, which is exact only because it is the only client.
9. The `m9diag` traces allocate about 90 B per cycle per arm (≈ 50 MB for a 220 k-cycle run). They are
   not enabled in any published configuration.
10. Stores remain unmodelled (see §10), so "traffic" means read traffic plus the descriptor/index
    image.

### 9.18 Slot capacity, index readiness and target scheduling (follow-up experiment)

This section asks *why* a 64-slot pool beats the published 128 at κ = 0.25 rather than *that* it
does. Nothing in the architecture, the dispatch policy, the index-acquisition unit, the scheduler or
the published configurations was changed; only read-only instrumentation and the existing
`pool_slots` diagnostic override were used. Every run below reports `M9 GATES PASSED`, and the
published cells reproduce their previous cycle counts exactly
(153 475 / 145 468 / 125 778, 119 798 / 113 233 / 94 763, 95 194 / 95 605 / 95 899 for 128 / 96 / 64).

#### 9.18.1 Configuration and identity of work

One configuration: `configs/ab_controlled.cfg` with `N = Λ = 16 384`, `P = 512`, `ρ_dup = 0`,
`elem_bytes = 4`, `bulk_wl = E0`, `seed = 1`, `batch_window = 8 192`, `bulk_max_elems = 262 144`,
`page_sched = 1`, `idx_mode = 2`, R = 128, latch free, and `pool_slots ∈ {64, 96, 128}`.
A and B never read the override, so their cycle counts are a control that is identical in every
column of every table below.

| κ | pool | C | A | B | index lines | bulk elements | target lines | dTLB-refused issue cycles |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 128 | 153 475 | 220 123 | 145 032 | 774 | 12 376 | 12 376 | 72 790 |
| 0.25 | 96 | 145 468 | 220 123 | 145 032 | 774 | 12 376 | 12 376 | 71 459 |
| 0.25 | **64** | **125 778** | 220 123 | 145 032 | 774 | 12 376 | 12 376 | 72 181 |
| 0.50 | 128 | 119 798 | 169 826 | 131 199 | 517 | 8 260 | 8 260 | 66 848 |
| 0.50 | 96 | 113 233 | 169 826 | 131 199 | 517 | 8 260 | 8 260 | 66 694 |
| 0.50 | **64** | **94 763** | 169 826 | 131 199 | 517 | 8 260 | 8 260 | 67 380 |
| 0.75 | 128 | **95 194** | 120 110 | 118 689 | 254 | 4 052 | 4 052 | 62 070 |
| 0.75 | 96 | 95 605 | 120 110 | 118 689 | 254 | 4 052 | 4 052 | 62 072 |
| 0.75 | 64 | 95 899 | 120 110 | 118 689 | 254 | 4 052 | 4 052 | 62 079 |

The identity of the work is the same in all nine cells: the same class map, the same 12 376 / 8 260 /
4 052 target lines, the same index array. Only the shared pool's capacity differs. The last column is
the number of cycles in which the target issue port had a sealed line ready and the dTLB refused it
(state C1 of §9.18.4): it is flat across capacity and is the largest single term in every run.

#### 9.18.2 Measurement definitions

All new counters are stamped at the event site, never derived from an occupancy snapshot:

| quantity | where it is recorded |
|---|---|
| index line claimed into the acquisition ring | `idx_stream` (`idx_lines_req`) |
| index line submitted to the hierarchy | `issue` / `idx_issue` on a successful `submit` (`idx_sub`) |
| index line returned | `on_index` (`idx_lines_done`) |
| index element taken for address generation | successful `idx_take` (`idx_consumed`) |
| address generated | `win_admit`, after the address is formed and admitted (`tgt_addr_gen`) |
| AG stalled for readiness | failed `idx_take` (`idx_wait_stall`) |
| … and target capacity was free at that stall | same site, `free_count() > idx_reserve` (`idx_wait_cap`) |
| target refused for residency | `win_emit`, `free_count() <= idx_reserve` (`slot_stall`) |
| generator has work but no fill window | `ag_remaining() > 0 && !win_filling()`, sampled per cycle |
| DRAM candidates per decision | `dram_tick`, entries with `bank_ready <= cycle` at the issuing decision |
| ready index elements / ring occupancy | acquisition ring (`ready && used < cnt`), sampled per cycle |

Per-cycle events are differences of consecutive samples; the sample is taken at the end of the cycle,
after both clients have made progress. `idx_wait_cap` is zero in every cell: whenever the generator
starved for an index element, the pool was at or below the target reserve. The one qualification is
that `idx_reserve` (8 slots) is reserved for the index stream, so "free" there excludes the share the
index stream may still claim.

The request's six states are evaluated in this priority order, first match wins, over engine-active
cycles: **D** a target was submitted; **B** the target port was refused for residency; **C** capacity
was free and a sealed line was waiting yet nothing was issued (C1 issue refused by the dTLB, C2 the
generator had work and no fill window, C3 other); **A** the generator stalled for an index value while
target capacity was free and no sealed line was waiting; **E** no target was issued but an index line
was submitted or returned while a target was outstanding; **F** none of the above. A cycle that merely
waits for a target fill is F, never A: A requires a failed index lookup.

#### 9.18.3 The index pipeline: earlier acquisition, lower occupancy, identical burstiness

| κ | pool | lines submitted | first…last return | AG stalls | …with capacity free | mean free at stall | mean ready elems | mean ring lines | max gen/cycle | cycles generating | claims refused (no slot) |
|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 128 | 774 | 1…90 347 | 7 333 | 0 | 2.0 | 1 049.7 | 70.9 | 8 | 1 547 | 496 |
| 0.25 | 96 | 774 | 1…89 633 | 5 266 | 0 | 0.5 | 1 126.1 | 75.3 | 8 | 1 547 | 562 |
| 0.25 | 64 | 774 | 1…**7 947** | 3 396 | 0 | 0.0 | 29.8 | 4.9 | 8 | 1 547 | 1 118 |
| 0.50 | 128 | 517 | 1…8 812 | 5 615 | 0 | 0.1 | 22.5 | 7.1 | 8 | 1 033 | 342 |
| 0.50 | 96 | 517 | 1…7 498 | 3 990 | 0 | 0.1 | 17.9 | 4.9 | 8 | 1 033 | 250 |
| 0.50 | 64 | 517 | 1…4 193 | 2 525 | 0 | 0.0 | 1.5 | 2.8 | 8 | 1 033 | 337 |
| 0.75 | 128 | 254 | 1…3 897 | 2 639 | 0 | 0.1 | 1.5 | 2.6 | 8 | 507 | 0 |
| 0.75 | 96 | 254 | 1…3 318 | 2 045 | 0 | 0.0 | 1.5 | 2.3 | 8 | 507 | 0 |
| 0.75 | 64 | 254 | 1…2 237 | 1 282 | 0 | 0.0 | 0.0 | 1.3 | 8 | 507 | 337 |

Four direct answers to the questions this experiment was asked to settle:

1. **Index responses arrive much earlier at 64 slots** at κ = 0.25: all 774 index lines have returned by
   cycle 7 947, against 90 347 at 128 (11× earlier). At κ = 0.50 it is 4 193 against 8 812.
2. **Buffer occupancy is lower, not higher, at 64.** The acquisition ring holds 4.9 lines on average
   against 70.9 at 128; ready index elements average 29.8 against 1 049.7. The smaller pool does not
   build a lead in the ring — it drains the ring.
3. **Address generation is exactly as bursty at every capacity.** Peak 8 elements/cycle and the same
   number of generating cycles (1 547 / 1 033 / 507 for κ = 0.25 / 0.50 / 0.75) in all nine cells. What
   changes is the *span* over which those cycles occur: the last address is generated at 7 947 (64)
   against 90 347 (128) at κ = 0.25.
4. **Index acquisition is not limited by translation, latency or slot scarcity in the claim path.**
   Claims refused for residency are *higher* at 64 (1 118 vs 496 at κ = 0.25), so the larger pool is not
   starving the index stream of slots. What it starves is consumption: the ring sits at its bound and
   further claims wait for elements to be consumed, which is what makes the array take 90 k cycles to
   arrive at 128.

#### 9.18.4 Readiness versus target progress

Cycles per state over the whole run (engine-active cycles; states are exhaustive and exclusive):

| κ | pool | A | B | C1 dTLB | C2 no window | C3 other | D issued | E | F |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 128 | **0** | 63 494 | 72 790 | 663 | 2 050 | 8 701 | 0 | 5 776 |
| 0.25 | 96 | **0** | 57 967 | 71 459 | 694 | 1 752 | 8 054 | 0 | 5 541 |
| 0.25 | 64 | **0** | 41 470 | 72 181 | **1** | 1 143 | 6 344 | 0 | 4 638 |
| 0.50 | 128 | **0** | 41 098 | 66 848 | 663 | 1 613 | 4 638 | 0 | 4 937 |
| 0.50 | 96 | **0** | 35 831 | 66 694 | **1** | 1 192 | 4 009 | 0 | 5 505 |
| 0.50 | 64 | **0** | 19 582 | 67 380 | **1** | 671 | 2 247 | 0 | 4 881 |
| 0.75 | 128 | **0** | 27 223 | 62 070 | 0 | 509 | 1 352 | 0 | 4 039 |
| 0.75 | 96 | **0** | 28 254 | 62 072 | 0 | 507 | 1 312 | 0 | 3 459 |
| 0.75 | 64 | **0** | 29 677 | 62 079 | 0 | 502 | 1 262 | 0 | 2 378 |

* **State A never occurs**, at any capacity or κ: there is no cycle in which target capacity was free
  and no index element was ready. Index readiness never idles the target port in this machine, because
  a sealed window always holds lines the port can issue (mean sealed backlog 2 274–6 340 lines).
* **C1 is a capacity-independent wall**: 72 790 / 71 459 / 72 181 cycles at κ = 0.25, i.e. 47 %, 49 % and
  57 % of the three runs. The run cannot be shorter than this term plus the issue work, and the pool
  size barely moves it.
* **B is the term the pool controls** (63 494 → 41 470 at κ = 0.25; 41 098 → 19 582 at κ = 0.50).
* **E is zero everywhere and D carries few index moves** (80 / 5 / 0 at κ = 0.25): the engine has one
  issue port at `eng_issue = 4`, and index and target submissions almost never advance in the same
  cycle. Overlap in this design is a window/ring phenomenon, not a cycle-level one.
* **C2 collapses from 663 to 1 cycle at 64** at κ = 0.25 and 0.50 — the generator is never left with
  work and no window to admit it into. That is the single largest qualitative difference between the
  capacities, and it is measured directly, not inferred from a stall counter.

A second measurement makes the same point from the feeder's side: the generator has elements left and
no fill window accepting them for **79 045 / 80 034 / 1 cycles** at 128 / 96 / 64 (κ = 0.25) and
**82 889 / 1 / 1** (κ = 0.50). Twelve-bucket timelines, aligned to cycle 0, show where that time sits:

| bucket (128 slots) | A | B | C | targets issued | index returns | AG stalls | mean ready | mean DRAM q |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0–12 788 | 0 | 8 106 | 2 | 0 | 672 | 5 541 | 540.8 | 13.2 |
| 12 789–25 578 | 0 | 1 274 | 11 110 | 939 | 0 | 0 | 2 048.0 | 5.2 |
| 25 579–76 737 (4 buckets) | 0 | 0 | 12 56x | ~780 each | 0 | 0 | 2 048.0 | 4.3–4.6 |
| 76 738–89 526 | 0 | 1 439 | 10 956 | 1 098 | 65 | 1 045 | 1 815.7 | 5.6 |
| 89 527–102 316 | 0 | 10 891 | 466 | 1 458 | 37 | 747 | 0.0 | 13.4 |
| 102 317–153 475 (4 buckets) | 0 | 9 846–11 154 | 183–1 552 | 1 327–1 520 each | 0 | 0 | 0.0 | 13.2–13.4 |

At 64 slots the same table has one prologue bucket (3 396 AG stalls, 774 index returns, 6 366
slot-refusals, no target issued) and eleven buckets of steady issue at 1 007–1 299 targets each, with
**zero** AG stalls after cycle 10 480.

#### 9.18.5 Target-memory scheduling

| κ | pool | targets | DRAM reads | row hits | conflicts | starved cycles | bank busy | row-hit % | mean q | cands/decision | mean in flight | peak in flight | issues per issuing cycle |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 128 | 12 376 | 13 609 | 10 470 | 3 137 | 33 353 | 228 194 | 76.9 | 9.0 | 6.5 | 54.7 | 120 | 1.42 |
| 0.25 | 96 | 12 376 | 13 606 | 10 569 | 3 035 | 25 226 | 226 928 | 77.7 | 9.6 | 6.5 | 40.4 | 88 | 1.54 |
| 0.25 | 64 | 12 376 | 13 575 | 11 443 | 2 130 | 11 685 | 215 634 | **84.3** | 10.7 | 6.6 | 27.4 | 56 | 1.95 |
| 0.50 | 128 | 8 260 | 9 900 | 7 842 | 2 056 | 30 990 | 163 296 | 79.2 | 8.2 | 6.4 | 33.7 | 120 | 1.78 |
| 0.50 | 96 | 8 260 | 9 935 | 7 946 | 1 987 | 23 585 | 162 958 | 80.0 | 8.9 | 6.5 | 24.1 | 88 | 2.06 |
| 0.50 | 64 | 8 260 | 9 876 | 8 710 | 1 164 | 10 892 | 152 256 | **88.2** | 10.1 | 6.6 | 15.0 | 56 | 3.68 |
| 0.75 | 128 | 4 052 | 6 666 | 5 591 | 1 073 | 32 566 | 106 224 | 83.9 | 6.8 | 6.5 | 6.7 | 114 | 3.00 |
| 0.75 | 96 | 4 052 | 6 666 | 5 597 | 1 067 | 32 636 | 106 152 | 84.0 | 6.8 | 6.6 | 6.3 | 81 | 3.09 |
| 0.75 | 64 | 4 052 | 6 666 | 5 603 | 1 061 | 32 688 | 106 080 | 84.1 | 6.9 | 6.6 | 6.0 | 50 | 3.21 |

At κ = 0.25 and 0.50 a smaller pool is associated with fewer requests in flight, more row hits, fewer
conflicts, fewer DRAM-empty cycles and shorter bank-busy time — while the number of requests and the
number of candidates per scheduling decision are unchanged (13 609 vs 13 575 reads; 6.5 vs 6.6
candidates). At κ = 0.75 nothing moves: 6–7 requests in flight is already below the point where the pool
matters, the queue is empty for a third of the run, and all three capacities land within 0.7 %.

What the table alone cannot say is *which way* the causation runs. The next control answers that.

#### 9.18.6 Two controls

The pool size changes two things at once at low κ: target concurrency, and how long a window takes to
fill. Two diagnostic overrides separate them. Neither changes any published configuration; both are
C-only, with A and B untouched.

**Control 1 — `batch_window = 16 384` (W above the whole element stream).** The stream is 12 376
elements, so W should never bind. Measured: window occupancy peak 4 736 / 5 840 / 8 880 and 3 / 3 / 2
batches with 2 / 2 / 1 deadline seals. W was *not* the binding term — the 4 096-cycle `batch_deadline`
was. Cycles: 153 475 / 145 468 / **121 226** (κ = 0.25) and 119 798 / 113 233 / **93 769** (κ = 0.50): the
96/128 columns are unchanged to the cycle and the 64 column improves by 4 552. A larger window does not
remove the capacity effect; it slightly enlarges it.

**Control 2 — `batch_deadline = 10⁸` (seal only when full or at the end of the descriptor).** This keeps
W = 8 192 as published and removes only the partial seal:

| κ | pool | C | batches | window peak | first seal | last seal | AG no-window cycles | row-hit % | starved | mean in flight | B | D |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 128 | **125 581** | 2 | 8 192 | 8 200 | 13 646 | 1 | 83.8 | 11 665 | 45.6 | 36 614 | 6 345 |
| 0.25 | 96 | **124 418** | 2 | 8 192 | 6 347 | 10 757 | 1 | 84.2 | 9 177 | 37.0 | 38 387 | 6 452 |
| 0.25 | 64 | **125 778** | 2 | 8 192 | 4 114 | 7 947 | 1 | 84.3 | 11 685 | 27.4 | 41 470 | 6 344 |
| 0.50 | 128 | **94 035** | 2 | 8 192 | 8 226 | 10 291 | 1 | 87.8 | 10 763 | 15.8 | 14 565 | 2 333 |
| 0.50 | 96 | **94 263** | 2 | 8 192 | 6 347 | 8 412 | 1 | 88.1 | 10 643 | 15.4 | 16 809 | 2 303 |
| 0.50 | 64 | **94 763** | 2 | 8 192 | 4 126 | 6 191 | 1 | 88.2 | 10 892 | 15.0 | 19 582 | 2 247 |

The capacity effect disappears: at κ = 0.25 the three capacities land within 1.1 % (and 96 is now
fastest), at κ = 0.50 within 0.8 % and *monotone in the opposite direction* (128 fastest). Two
measurements make the reason visible:

* the generator has work and no fill window for **1** cycle at every capacity, against 79 045 / 80 034 /
  1 under the published deadline;
* the row-hit rate becomes capacity-insensitive (83.8 / 84.2 / 84.3 at κ = 0.25, 87.8 / 88.1 / 88.2 at
  κ = 0.50) although the mean in-flight count still differs by 18.2 requests (45.6 vs 27.4 at κ = 0.25).

So the 7.4-point row-hit gap in §9.18.5 is *not* a function of how many requests are in flight. It
appears only when the published deadline is in play, and it vanishes when the deadline is removed while
concurrency is left free to differ. The measured mechanism is window turnover: at 128 slots the
published policy seals windows at 4 736 entries (58 % of W) because the fill takes longer than 4 096
cycles, so 12 376 elements need three windows; at 64 slots windows reach the full 8 192 entries in
4 114 cycles, so two windows suffice. In the three-window case the last 4 184 elements cannot be
admitted until a window retires, which happens at cycle 90 347 (the last seal), and the feeder is idle
for 79 045 cycles with nothing to admit. Remove the deadline and all nine window statistics become
capacity-independent.

#### 9.18.7 Where the saved cycles come from

The state counts partition the run, so the difference between two cells can be split exactly:

| κ = 0.25, 128 → 64 | ΔB | ΔC1 | ΔC2 | ΔC3 | ΔD | ΔE | ΔF | ΔC |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cycles | −22 024 | −609 | −662 | −907 | −2 357 | 0 | −1 138 | **−27 697** |
| % of the saving | 79.5 | 2.2 | 2.4 | 3.3 | 8.5 | 0 | 4.1 | 100 |

This is an accounting identity over measured state counts, not a causal attribution: it says the run
is shorter because it contains 22 k fewer residency-refused cycles and 2 357 fewer issuing cycles for the
same 12 376 issues (1.95 issues per issuing cycle instead of 1.42). The control in §9.18.6 supplies the
causal reading, because removing the deadline removes ~27 900 of those 27 697 cycles at 128 while
leaving 64 unchanged.

The remaining terms: the SIMT phase is 1 186 cycles *longer* at 64 (15 560 against 14 374), consistent
with §9.15's S column; simultaneous residency drops from 11 953 to 2 509 cycles (both paths holding
slots) while simultaneous *progress* stays at 134 vs 87 cycles — the smaller pool removes residency
overlap, not useful overlap. No path is starved: both clients complete every request and every element
in every cell, and `M9 GATES PASSED` holds in all of them.

#### 9.18.8 Measured, derived, inferred

**Directly measured.** All cycle counts and per-pool A/B controls; the state counts of §9.18.4; index
claims/submissions/returns, consumed elements, generated addresses and AG stalls (§9.18.3); ring and
window occupancy, `ag_remaining`-without-`win_filling` (§9.18.6); DRAM reads, row hits, conflicts,
starved/blocked/busy cycles, queue depth and eligible candidates per decision (§9.18.5); in-flight target
counts; the two control tables.

**Derived (arithmetic on measurements, no mechanism claimed).** "Issues per issuing cycle" (issues ÷
cycles-with-an-issue); the ΔC partition of §9.18.7; the percentages of the run; the 11× / 18 % / 7.4-point
comparisons.

**Causal interpretation, stated as such.**
1. That the low-κ capacity response is produced by the interaction between the shared pool and the
   window seal policy, not by index readiness and not by DRAM concurrency per se. The support is the
   control: with the deadline removed, the capacity spread at κ = 0.25 falls from 18 % to 1.1 % and the
   row-hit gap from 7.4 points to 0.5, while concurrency is still free to differ by 18 requests. That the
   *link* from pool size to window fill rate is the admission rate in the prologue (1.0 vs 2.0 entries per
   cycle) is an inference.
2. That the 64-slot rank at the published deadline is a *packing* outcome rather than a smooth capacity
   response. Measured: the batch count flips 3 → 2 between 96 and 64 at κ = 0.25 but between 128 and 96
   at κ = 0.50, and the flip point tracks the element count, not the pool curve. Why a 2-entry/cycle
   rather than 1-entry/cycle fill rate decides that flip is arithmetic on the deadline, but the simulator
   does not label an admission cycle with its blocking resource.
3. That the row-hit and DRAM-starvation differences at the published deadline follow the batch
   partitioning (three windows live over the run instead of two) rather than the pool size. The
   correlation is measured (row-hit moves with batch count, not with in-flight count in the control); the
   ordering is an interpretation.

**Unresolved.**
1. Why admission runs at half rate at 96/128 in the first 5 k cycles. The candidate counters are the AG's
   index stalls (5 541 vs 3 396 in bucket 0), the organizer's backlog gate and the index line return
   interval (1 per 19 cycles vs 1 per 10); no counter attributes an admission cycle to one of them.
2. Whether the 64-slot advantage survives at other footprints. The element count decides the batch
   count, so a footprint whose stream fits one window, or one that needs four, would move the flip point.
   §9.15's minima (64, 64, 96–256) were measured at this footprint only, and this experiment does not
   generalize them.
3. Whether the same effect appears at other `batch_deadline` values. Only the published 4 096 and a
   no-deadline control were run; no value between them was swept.
4. The `idx_oracle` diagnostic is not wired into the hybrid driver (`run_m9` never calls
   `set_index_oracle`), so the proposed feeder-neutral control could not be run; the runs that set it are
   bit-identical to the baseline and are not reported as a control.

#### 9.18.9 Conclusion

At κ = 0.25 and 0.50 the 64-slot pool is faster than 96 or 128, and the strongest supported explanation
is **neither** better index/target overlap **nor** better DRAM scheduling as such:

* Index readiness never idles target capacity (state A = 0 in all nine cells), and the address generator
  is equally bursty at every capacity. Index responses do arrive 11× earlier at 64, but that is a
  consequence of the feeder not being blocked, not the cause of the speedup: the AG's task is finished by
  cycle 7 947 at 64 precisely because there is nothing left for it to wait for.
* Fewer requests in flight does *not* buy row locality: with the deadline removed, 45.6 and 27.4
  requests in flight give the same 83.8–84.3 % row-hit rate and the same cycle count within 1.1 %.
* The measured mechanism is **window turnover**: under the published `batch_deadline = 4 096` the larger
  pool fills windows at half the rate (4 736 vs 8 192 entries per window, 3 vs 2 batches), the generator
  spends 79 045 cycles with work and no window to admit into, and the target stream is issued from three
  windows instead of two. Removing the deadline removes the entire effect at 128 slots (−27 894 cycles,
  −18.2 %) and leaves 64 slots unchanged.

No allocation policy is proposed here, and no published configuration was changed. The result is a
statement about *this* machine at *this* footprint: the 64-slot cell is not a better use of the budget, it
is a cell in which a seal policy that is tuned to the window size happens to pack the stream into fewer
windows. Any decision about the pool or the deadline needs its own experiment at other footprints and
deadlines, and the two knobs should be examined together rather than one at a time.

#### 9.18.10 Limitations of this experiment

1. One seed, one footprint (Λ = 16 384, P = 512), one window size. The batch-count flip is a function of
   the element count, so the capacity ranking is not claimed elsewhere.
2. The two controls change a published parameter (`batch_window`, `batch_deadline`). They are diagnostic
   cells, clearly labelled, and neither is proposed as a configuration.
3. State classification is a priority-ordered partition evaluated once per cycle at the cycle boundary;
   intra-cycle order (`dispatch_order = 0` runs engine-progress then SIMT-generation) is not visible, so a
   cycle in which the port was refused and also issued is counted as D, not B.
4. `idx_wait_cap` uses the *reserve-adjusted* notion of free capacity (`free_count() > idx_reserve`), so
   "capacity free" is the engine's own definition of a slot it may take, not an empty pool.
5. DRAM row hits are run totals; there is no per-cycle attribution of a hit to a batch, so the link from
   batch partitioning to row locality is correlational.
6. Arm B has no shared pool, so all pool-dependent columns are C-only; B is the same machine in every
   cell and is reported for context only.
7. A and B were re-run in each cell and are bit-identical, which is the inertness check for the added
   instrumentation; `dx100_tests` (4 032 checks) and the published anchors are unchanged.

## 10. Limitations and scope exclusions

1. **Stores are not modelled.** `MemorySystem` has no write path, so "output correctness" here means
   loaded values keyed by the original element identity, plus the index image the engine consumes.
   A store-carrying workload would need write-back support that predates M9.
2. **The compiler is emulated, twice, and both are stated as such.** `op_class=gen` hands over the
   generator's own `regular`/`irregular` draw, recorded at trace construction and carried as
   metadata; `op_class=pattern` recovers the same decision from the addresses with the contiguity
   rule `kappa_real` uses. Neither is a compiler pass, and neither exists in any ISA here. The two
   agree on every workload tested. `op_class=simt|bulk` are uniform maps that exist only so G1/G2
   can hold the trace fixed and vary the annotation alone.
3. **One descriptor.** Multi-descriptor dispatch is not implemented and no engine change was made
   to support it; the initial experiment is single-descriptor by construction.
4. **`cores = 1` only.** The SIMT front-end keeps a private slot vector per core, so S > 1 would
   need a partitioned pool. `run_hybrid` and `run_ab` both refuse S > 1 rather than approximate it.
5. **No historical bulk hash.** See §3.
6. **Memory-image asymmetry.** A and B apply the full reference workload; C applies the compacted
   one. The index/descriptor region is disjoint from the trace footprint, so this cannot affect
   timing or the target stream, but the two images are not byte-identical when the class map is
   mixed. It is why §2 compares hierarchy state rather than images.
7. **Residency claim.** With `ab_charge_latch = 0` the shipped profile allows 128 slots plus up to 4
   latched accesses (132 formed). No strict 128-total statement is made anywhere in this report.
8. **Classification policy untouched.** No tuning, no runtime sampling, no fallback, no mid-run
   switch, no third class. The two-class map is computed once, before execution, and the engine is
   armed (`armed = true`) only when the map selects at least one element.
