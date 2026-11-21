# M17 Phase 2 — SIMT concurrency: LSU pending vs L1 MSHRs

**Question.** How does `lsu_pending` — the per-core outstanding-load cap — affect the
comparison, independently of L1 MSHR capacity?

**Answer.** The two are different resources and they move the comparison in
opposite directions. At κ = 0.25 the cap is *nearly free for the SILICON but not
for the ratio*: capping the SIMT arm at 8 outstanding loads costs it 3.3 %
(220 019 → 227 313 cycles) yet makes the hybrid 5.0 % faster (124 406 →
118 242), because the SIMT client stops holding pool slots the engine can use.
Bulk speedup therefore rises from **1.769× to 1.922×**. A cap of 32 changes
almost nothing (A +0.01 %, speedup 1.807×). At the wider 2×16K window the same
cap gives **2.200×** against 2.112× uncapped (128 MSHRs).

The important negative result is that the cap costs the SIMT arm almost nothing
at 32 and only 3.3 % at 8 even though it reduces measured mean concurrency from
87.9 to 24.5 and 8.0 requests. The uncapped arm's 87.9 "outstanding requests" are
mostly residency slots waiting at a 16-entry MSHR array and on translation, not
requests making progress. **Configured concurrency, measured concurrency and
useful concurrency are three different quantities in this model**, and only the
third moves the cycle count.

---

## 1. What `lsu_pending` actually constrains

Read from the source, then confirmed by measurement.

| property | finding | where |
|---|---|---|
| who is capped | the **SIMT core only** | `src/system.cpp:165`, `:220`; `src/bulk.cpp:2296` — all three call sites test `cfg_.lsu_pending && in_sys[c.id] >= cfg_.lsu_pending` inside the per-core issue loop |
| what is capped | requests **submitted by that core** and still in the system | `in_sys[c.id]` is incremented on `mem_->submit(...)` success and decremented on completion |
| what happens when full | the core is **skipped for that cycle**; no request is formed | `continue` before the issue block |
| does it bound the engine | **no** | the engine's admission is bounded by the shared `R` pool and its own `idx_reserve` guard, not by `lsu_pending` |
| published value | **0 (off)** in `configs/ab_controlled.cfg`, deliberately | cfg comment: the SIMT arm is bounded by `R` alone |
| Vortex hardware default | **8** (`VX_CFG_LSU_PENDING_SIZE`, asserted in `tests.cpp:449`) | `configs/vortex_ref.cfg` |
| enforcement granularity | checked once per cycle, then up to `warps_per_core` requests may be admitted | measured overshoot below |

**Measured overshoot.** The cap is checked before the warps issue, so it can be
crossed by up to `warps_per_core − 1` = 3 requests inside one cycle. Measured
peak hierarchy inflight: **11 at cap 8** and **35 at cap 32** — exactly cap + 3
in both cases, at both MSHR settings. Reported peak occupancy therefore exceeds
the configured cap by a fixed, explainable amount.

## 2. The 2×2 factorial

κ = 0.25, W = 8 192, N = 16 384, mean of five paired seeds. `lsu_pending = 0` is
the published controlled baseline.

| L1 MSHR | `lsu_pending` | SIMT-only | SD | hybrid | SD | bulk-only | A/C | mean inflight (A) | peak inflight (A) |
|---|---|---|---|---|---|---|---|---|---|
| 16 | 0 (off) | 220 019 | 1 710 | 124 406 | 1 134 | 138 380 | **1.7686** | 87.9 | 128 |
| 16 | 8 | 227 313 | 1 763 | 118 242 | 1 006 | 138 380 | **1.9224** | 8.0 | 11 |
| 16 | 32 | 220 045 | 1 768 | 121 800 | 1 066 | 138 380 | 1.8066 | 24.5 | 35 |
| 128 | 0 (off) | 211 495 | 1 562 | 120 988 | 944 | 136 164 | **1.7481** | 86.2 | 128 |
| 128 | 8 | 227 313 | 1 763 | 117 984 | 1 036 | 136 164 | **1.9266** | 8.0 | 11 |
| 128 | 32 | 215 116 | 1 690 | 120 421 | 975 | 136 164 | 1.7864 | 24.3 | 35 |

At the wider window (W = 16 384), against the uncapped points of the reproduction
report (105 716 and 100 162):

| L1 MSHR | `lsu_pending` | SIMT-only | hybrid | A/C |
|---|---|---|---|---|
| 16 | 0 (off) | 220 019 | 105 716 | 2.0812 |
| 16 | 8 | 227 313 | 107 356 | **2.1174** |
| 16 | 32 | 220 045 | 106 780 | 2.0607 |
| 128 | 0 (off) | 211 495 | 100 162 | 2.1115 |
| 128 | 8 | 227 313 | 103 324 | **2.2000** |
| 128 | 32 | 215 116 | 102 114 | 2.1066 |

Paired per-seed speedups for every cell are in `m17_tables.md` (P2); the spread
within a configuration is ≤ 1.1 %, well inside every effect discussed here.

**Three facts from this table:**

1. **The cap binds below the MSHR array.** With `lsu_pending = 8` the SIMT-only
   arm is *identical* at 16 and 128 MSHRs (227 313 both). Once fewer than 16
   requests may be outstanding, the size of the MSHR file cannot matter. This is
   the cleanest demonstration that the two capacities are separate resources and
   that a factorial design is required to separate them.
2. **`lsu_pending = 32` is not a stronger baseline.** It leaves the SIMT arm
   within 0.01 % of uncapped (220 045 vs 220 019 at 16 MSHRs) while still
   changing the ratio (1.769 → 1.807) through the hybrid. Treating 32 as "more
   realistic" would be a choice about *whose* concurrency is being counted, not a
   neutral improvement.
3. **The hybrid gains more than the SIMT arm loses** at cap 8: A +3.3 %,
   C −5.0 %. The mechanism is in §3.

## 3. Why the hybrid gets faster when SIMT is capped

Pool occupancy in the hybrid, measured per client:

| L1 MSHR | `lsu_pending` | pool slots held by SIMT (peak / mean) | by the engine (peak / mean) | hierarchy inflight (peak / mean) |
|---|---|---|---|---|
| 16 | 0 | 128 / 8.8 | 120 / 51.7 | 128 / 58.9 |
| 16 | 8 | 64 / 4.7 | 120 / 59.3 | 120 / 61.3 |
| 16 | 32 | 120 / 7.3 | 120 / 56.4 | 120 / 60.9 |

Capping SIMT halves its mean pool occupancy (8.8 → 4.7 slots) and the engine
takes up the difference (51.7 → 59.3 slots, +14.7 %). Total hierarchy inflight
rises only 4 % (58.9 → 61.3), so this is not "more concurrency"; it is
**reallocation of a shared, fixed budget from the client that cannot use it to
the client that can**. The SIMT client's own formation refusals fall from 13 319
to 542 at the same time: with a cap it stops queueing requests it would only have
to retry.

At 2×16K the pool is already nearly saturated by the engine (mean 101.5 slots),
so the same reallocation has little room and the hybrid *slows* slightly
(105 716 → 107 356 at cap 8) even though the ratio still improves.

## 4. Admission-advantage control

The brief asks whether the engine receives an unintended admission advantage.
Three checks:

| check | result |
|---|---|
| is the engine subject to `lsu_pending`? | **No** — all three call sites are per-core SIMT issue loops. This is a property of the parameter's meaning (a per-core LSU outstanding-load pool), not a mechanism added here. |
| does the engine bypass the shared budget? | **No** — its slots come from the same 128-entry pool, its peak occupancy is 120 (it stops at `R − idx_reserve = 120`), and it refuses to issue when free ≤ 8: 40 836 refusals at lsu 0, 33 436 at cap 8. |
| does capping SIMT change the engine's own run? | **No** — bulk-only cycles are *byte-identical* across `lsu_pending` 0/8/32 (138 380 at 16 MSHRs, 136 164 at 128, for every seed). The cap does not reach arm B at all. |

So the honest statement is: the engine gains no *bypass*, but any
`lsu_pending > 0` necessarily **shifts the shared budget toward the engine**,
because the cap is applied to one client only. That is device asymmetry inherent
to a per-core LSU cap, and it is why the published controlled baseline sets it
to 0 and why M17 reports both.

## 5. Configured vs measured vs useful concurrency

| quantity | uncapped | cap 32 | cap 8 | where measured |
|---|---|---|---|---|
| configured outstanding loads | `R` = 128 | 32 | 8 | config |
| peak hierarchy inflight (A) | 128 | 35 | 11 | `inflight_max` |
| mean hierarchy inflight (A) | 87.9 | 24.5 | 8.0 | `inflight_occ / cycles` |
| peak L1 MSHR occupancy (A) | 16 | 16 | 11 | `l1_mshr_max` |
| cycles (A) | 220 019 | 220 045 | 227 313 | run |

The SIMT arm holds 87.9 slots on average while only 16 can be in the L1 MSHR
array. The other ~72 are resident requests waiting to *enter* the array, and the
slot is held for the whole retry. That is why the pool number overstates memory
parallelism by roughly 5.5× at the published configuration, and why cutting the
mean to 24.5 requests costs 0.01 % of the runtime: **the arm is bound by
translation and DRAM service latency, not by how many requests it may hold.**

Corroboration: the SIMT arm's mean L1 stall cycles fall from 16.5 M to 8.7 M
when the MSHR array is widened to 128 (reproduction report, §5), yet cycles fall
only 3.9 %. Retry cycles are cheap when the waits are the bottleneck.

## 6. What this part establishes

1. **`lsu_pending` is an independent axis.** It binds below the MSHR array
   (identical SIMT cycles at 16 and 128 MSHRs when capped at 8), so the 2×2
   factorial is necessary and neither knob can be used as a proxy for the other.
2. **Its effect on the comparison is real but bounded:** +0.15 in speedup at
   cap 8 with 16 MSHRs (1.769 → 1.922), +0.18 at 128 MSHRs, and up to +0.09 at
   the wider window. It is not the 0.47 swing M16 measured at N = 65 536, κ = .75,
   where SIMT held essentially the whole pool.
3. **The engine is not capped and does not bypass the pool.** Bulk-only results
   are invariant to `lsu_pending`, which is the strongest available evidence that
   the cap is one-sided.
4. **A cap that costs the SIMT arm 3 % can flatter the hybrid by 5 %.** Any
   headline speedup must state which cap was in force; the M5.1 baseline's choice
   of 0 is a choice, and Vortex's hardware default of 8 gives a *different*
   comparison at the same hardware.

## 7. Limitations

* **One workload family.** All 50 P2 cells are W1 at κ = 0.25, N = 16 384. The
  M16 result at κ = .75 / N = 65 536 shows the effect is much larger there; M17
  does not re-derive that, it only shows the κ = .25 magnitude.
* **Five seeds, one configuration per point.** SD within a point is ≤ 1.1 %;
  the cap effects (≥ 2 %) exceed it, but the third decimal of any ratio is not
  meaningful.
* **The overshoot of 3 requests is a model property** of a once-per-cycle cap
  check with four warps. A real LSU might check per warp or per bank; the model
  does not have that choice.
* **`lsu_pending` is not priced.** The model charges nothing for the per-core
  outstanding-load bookkeeping, so "cap 8 is free" is a statement about this
  model's cost model, not about area or timing.
* **Arm B's invariance is a strong check but not a proof of isolation**: if a
  future change made the engine read `lsu_pending`, the invariance test in §4
  would catch it, but nothing in the model enforces the separation structurally.
