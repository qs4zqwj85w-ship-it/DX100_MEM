# M20 — Can the Simulator Faithfully Represent Wider SIMT?

This document answers the M20 brief's "critical modeling requirement" *before*
any result is quoted, because it decides what the numbers in
`m20-width-sweep-results.md` are allowed to mean.

**Verdict.** The simulator can represent a wider SIMT **instruction stream** —
how many lanes form an instruction, and how many cache lines that instruction
touches after intra-warp dedup. It **cannot** represent a wider SIMT **memory
pipe**: there is no lane-width-dependent coalescer throughput, LSU width, MSHR
depth, port count, address-generation rate, or per-lane response tracking. Every
such resource is fixed at its Vortex-calibrated 4-lane value.

Consequently the width sweep is a **controlled architectural sensitivity model**.
The 4-lane configuration is the calibrated reference; 8 lanes is a modest
extrapolation of the same model; 16 and 32 lanes are sensitivity points whose
absolute numbers are *not* predictions for a real GPU of that width, and are not
presented as such anywhere in M20.

---

## 1. The complete footprint of `lanes`

An exhaustive search of `sim/src` for `cfg_.lanes` / `c.lanes` returns **six**
sites, and none of them is a memory-pipe resource:

| site | role |
|---|---|
| `workload.cpp:48`, `:138` | the synthetic generator groups addresses into instructions of `lanes` lanes |
| `workload.cpp:61` | trace-loader validation: header lanes must equal `cfg.lanes` (an assertion, not a behaviour) |
| `system.cpp:91` | `latch_charge = ab_charge_latch ? lanes : 0` — **disabled in every published config** |
| `system.cpp:267` | assertion: `latch_core_max <= lanes` |
| `bulk.cpp:2214`, `:2442` | same `latch_charge` expression / same assertion |
| `ab.cpp:154`, `:349` | config echo / assertion |

Everything else that determines how requests are formed is **width-independent**:

| resource | value | scales with `lanes`? |
|---|---|---|
| L1 | 256 lines × 4 ways, 2-cycle | no |
| L2 | 16 384 lines × 8 ways, 12-cycle | no |
| L1 MSHR | 16 per core | **no** |
| L2 MSHR | 128 | **no** |
| residency slots `R` | 128 (`pool_slots=128` is a diagnostic override of the same number) | **no** |
| submission attempts/cycle | `warps_per_core` = 4 | **no** |
| per-core outstanding cap | `lsu_pending` = 0 (uncapped) | no |
| line granularity | 64 B, no sector transfers | no |
| AG throughput (engine) | `ag_lanes` = 8 addresses/cycle | no (engine side) |

## 2. The brief's checklist, item by item

| requirement | modelled? | how | faithful at 16–32 lanes? |
|---|---|---|---|
| **execution-group behaviour** | **yes** | the trace carries `lanes` explicitly; one instruction covers up to `lanes` accesses and retires as a unit | partially — the group is a formation unit only; there is no per-warp scheduler, no warp slots, no divergence handling |
| **memory coalescing granularity** | **yes, at 64 B** | `System::run` coalesces an instruction into distinct cache lines (`cur_lines`), unbounded by width | granularity is faithful (line-level dedup), but a real coalescer works in 32 B sectors; the model has no sector merge |
| **transactions per instruction** | **yes** | `requests_generated += cur_lines.size()` — one per distinct line, formed in a single cycle regardless of count | **no** — a 32-lane scatter forms up to 32 transactions instantly; real hardware spends several cycles doing so |
| **outstanding memory requests** | **yes** | shared slot pool `R = 128`; `peak_slot`, `inflight_max` are counters | **no** — `R` does not grow with width, so a wider machine gets no more MLP |
| **MSHR and LSU admission** | **yes, fixed depth** | `l1_mshr = 16`, `l2_mshr = 128`, admission by MSHR allocation and the shared pool | **no** — depth is a 4-lane-era number; M18's `mshr128` cells show what 8× depth is worth (§6) |
| **address-generation throughput** | SIMT: **no**; engine: yes | the SIMT front end forms addresses for free once the instruction is formed; the engine's AG is capped at `ag_lanes = 8`/cycle | **no** for SIMT — this is the single most optimistic assumption for wide lanes |
| **response and dependency tracking** | **partially** | responses retire per *line*, not per lane; an instruction's latch (`cur_pos`/`cur_lines`) tracks its own outstanding lines | a real warp tracks per-lane completion and can retire lanes independently; the model tracks per instruction only |

## 3. What the model therefore does represent, exactly

For a width `L`, relative to the 4-lane trace of the same program:

1. **Instruction count falls** as `≈ 4/L` for the dense streams (`colidx`,
   `value`, `rowptr`) — measured: bcsstk25 SIMT instructions 151 499 → 92 587 →
   62 903 → 46 979 at 4/8/16/32.
2. **Intra-warp dedup improves**: one instruction covering 32 consecutive 4 B
   indices spans 128 B = 2 lines instead of 4 instructions × 1 line each. This is
   a real effect of wider warps and it is modelled correctly.
3. **Nothing else.** The number of *addresses* is identical, the compulsory lines
   are identical, the DRAM traffic is identical, and the caches, MSHRs, ports and
   slot pool are identical.

The measured consequence is in `m20-width-sweep-results.md`: requests fall much
more slowly than instructions (bcsstk25: 3.2× fewer instructions, 1.6× fewer
requests), and cycles fall more slowly still (1.13×) because the machine is
memory-bound. A wider front end is not where the time is.

## 4. Two artifacts, in opposite directions

Because the memory pipe is width-independent, the model contains one artifact
that **favours** wide SIMT and one that **penalises** it. Both are worth naming
because they bound the interpretation:

* **Zero-cost transaction formation (favours wide).** All distinct lines of an
  instruction become requests in the cycle the instruction is formed. At 32 lanes
  a scattered gather would need several cycles in a real LSU; here it is free. So
  wide SIMT's request cost is understated.
* **Single-latch head-of-line blocking (penalises wide).** A core cannot form its
  next instruction until every line of the current one has been allocated a slot
  (`system.cpp`: `if (c.cur_pos >= c.cur_lines.size() && c.next < c.instrs.size())`).
  A 32-lane instruction holding 20–32 slots therefore blocks formation while a
  4-lane instruction never does. Measured: the coalescer latch peaks at 20 for
  dubcova3 at 32 lanes, 8 at 4 lanes.

They do not cancel in general, so M20 reports the width trend as a **sensitivity**
and never as a prediction. Note the direction: the second artifact inflates the
Bulk advantage at high width, and the advantage still *shrinks* with width
(§`m20-width-sweep-results.md`), so the qualitative conclusion is conservative.

## 5. What would make a width a validated configuration

A defensible 32-lane model needs at least: sector-granular coalescing (32 B), a
transaction-formation rate (sectors or lines per cycle), MSHR/LSU depth scaled
with width and validated against an implementation, per-warp independent tracking
instead of one latch, and a warp-slot scheduler. None of that exists here, and
adding it would be an architecture change, which M20's terms of reference forbid.

The M20 sweep therefore labels its points:

| width | label | why |
|---|---|---|
| 4 | **Vortex-calibrated reference** | the configuration every M5–M19 result is calibrated at |
| 8 | controlled extrapolation | same model, one doubling; still within the range where the missing pipe width is second-order |
| 16 | architectural sensitivity | no pipe model; report direction, not absolute cycles |
| 32 | architectural sensitivity (not a GPU) | strongly affected by both artifacts of §4 |

## 6. How much is the missing knob worth?

The one width-dependent resource M18 already ablated is L1 MSHR depth. Running
the identical 4-lane cells at `l1_mshr = 16` (calibrated) and `l1_mshr = 128`
(eight times deeper, a proxy for a wider machine's deeper pipe):

| dataset | A cycles, mshr 16 | A cycles, mshr 128 | gain from 8× MSHR |
|---|---:|---:|---:|
| bcsstk25 | 461 688 | 455 871 | 1.3 % |
| qc2534 | 762 503 | 742 629 | 2.6 % |
| lnsp3937 | 57 014 | 55 834 | 2.1 % |
| dubcova3 | 3 522 146 | 3 330 465 | 5.4 % |

Compare with what width alone is worth at fixed depth (§`m20-width-sweep-results.md`):
11.8 % on bcsstk25 from 4 → 32 lanes. So an eight-fold deeper MSHR — more than a
32-lane design would plausibly need relative to a 4-lane one — buys a fifth to a
half of what width buys. The missing pipe scaling is therefore **not** large
enough to reverse the width trend, though it is large enough to matter for any
absolute claim. That is exactly why the sweep is reported as a sensitivity and the
conclusion is stated as a direction with a bound.

## 7. Consequence for the M20 conclusion

* Conclusions **can** be drawn about the *direction and rough magnitude* of the
  width effect, because the dominant mechanism (intra-warp dedup and instruction
  count on a memory-bound kernel) is modelled.
* Conclusions **cannot** be drawn about the *absolute* cycles of a 16- or 32-lane
  GPU, about its cache/DRAM behaviour at sector granularity, or about any design
  whose advantage depends on MLP scaling with width.
* The 4-lane row of every table is the calibrated reference and may be quoted
  without these caveats; the wider rows may only be quoted with the label.
