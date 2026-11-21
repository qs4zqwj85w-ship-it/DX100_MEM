# M18A — Lane-Width Sensitivity of the Bulk Index-Fetch Advantage

**Question.** The M18 acceleration claim rests on the engine acquiring the index
stream as *one dense sequential object* instead of as per-element SIMT lane
loads. A wider SIMT warp coalesces those per-lane loads better, so a natural
challenge is: *how much of the advantage is an artefact of the 4-lane front end
the simulator is calibrated at?* This report answers that with a 4/8/16/32-lane
sweep on bcsstk25 and dubcova3.

**Short answer.** The advantage decays monotonically with warp width — bcsstk25
1.182× → 1.100× → 1.065× → 1.058×, dubcova3 1.179× → 1.133× → 1.124× — and it
does not reverse anywhere in the measurable range. The 32-lane point is **not
measurable on dubcova3**: the SIMT-only arm deadlocks (§5).

---

## 1. What `lanes` actually controls

`lanes` is a *trace-formation* parameter, not a machine-width parameter. Its
complete footprint in the simulator:

| site | effect |
|---|---|
| `tools/build_trace.py --lanes` | groups consecutive accesses of a stream into instructions of `lanes` lanes; changes `instructions_simt`, `instructions_bulk` |
| `system.cpp:91` | `latch_charge = ab_charge_latch ? lanes : 0` (off in every published config, so no budget effect here) |
| `system.cpp:267` | assertion only: `latch_core_max <= lanes` |
| `bulk.cpp:2214` | same `latch_charge` expression |
| `bulk.cpp:2442` | assertion only: `latched <= lanes` |

No LSU width, MSHR count, cache geometry, issue port or DRAM parameter reads
`cfg_.lanes`. Therefore the sweep varies **exactly two things**, both faithful to
what a wider warp does to a memory trace:

1. **instruction formation** — a 16-lane instruction retires 16 lane slots at
   once, so the number of issue events falls;
2. **intra-warp dedup** — within one instruction, lane addresses that fall in the
   same cache line become one line request, so the *request* stream shrinks even
   though the *address* stream is identical.

It does **not** model a wider LSU (more requests per cycle), a deeper MSHR file,
or more L1 ports. This is a bounded front-end sensitivity, not a calibrated
wide-GPU configuration — see §6.

## 2. Experiment

Twelve traces were rebuilt from the same matrices and the same builder
(`--lanes 8|16|32`; lanes 4 uses the M18 traces) and each was run through the
identical wrapper
(`sim/build/wm/m18_run.sh`, window 8 192, `pool_slots=128`, `idx_mode=2`,
`page_sched=1`, `cache_diag=1`, `skip_b=1`). Access counts are unchanged by
construction; instruction counts fall:

| dataset | lanes | accesses | SIMT instr (`full`) | Bulk instr |
|---|---:|---:|---:|---:|
| bcsstk25 | 4 | 787 601 | 151 499 | 68 030 |
| bcsstk25 | 8 | 787 601 | 92 587 | 38 574 |
| bcsstk25 | 16 | 787 601 | 62 903 | 23 732 |
| bcsstk25 | 32 | 787 601 | 46 979 | 15 770 |
| dubcova3 | 4 | 4 858 617 | 864 468 | 409 734 |
| dubcova3 | 8 | 4 858 617 | 498 726 | 226 863 |
| dubcova3 | 16 | 4 858 617 | 288 508 | 121 754 |
| dubcova3 | 32 | 4 858 617 | 167 258 | 61 129 |

(Instruction counts from the trace sidecars; element counts are the M18 values and
identical at every width — the sweep regroups work, it does not change it. Both
front ends are regrouped, which is why `Bulk instr` falls as well while
`bulk_elems` and the engine's index-line count do not. All 12 cells report
`M9 GATES PASSED` except the 32-lane dubcova3 SIMT arm, §5.)

Provenance: the 12 lane cells were produced by the simulator as it stood before
M18A's print-only addition of nine `m15` diagnostic keys (no decision reads them);
the eight primary M18 cells were re-run after it. The full suite (4 470 checks)
and the M17 anchor (220 123 / 138 924 / 125 581) reproduce identically on the
rebuilt binary, so the two batches are behaviourally the same code.

## 3. Result

`A` = SIMT-only baseline on the full trace, `C` = dispatched, `noidx` = the same
program with the SIMT index loads removed (M18's instrument, see
`m18a-execution-equivalence.md`).

**Faithful speedup** `S_f = A(full) / C(noidx)` — each arm pays for its own index
acquisition. **Same-trace** `S_s = A(full) / C(full)` is shown for contrast; it
double-charges the index stream and is not an architectural claim.

### bcsstk25 (252 241 elements, 4.0 % of the 2 MB L2)

| lanes | A(full) | C(full) | A(noidx) | C(noidx) | **S_f** | S_s |
|---:|---:|---:|---:|---:|---:|---:|
| 4 | 461 688 | 492 449 | 308 704 | 390 495 | **1.182** | 0.938 |
| 8 | 422 756 | 396 901 | 286 505 | 384 324 | **1.100** | 1.065 |
| 16 | 409 708 | 423 380 | 277 165 | 384 663 | **1.065** | 0.968 |
| 32 | 407 291 | 422 560 | 273 581 | 384 957 | **1.058** | 0.964 |

### dubcova3 (1 589 539 elements, 45 000-row prefix)

| lanes | A(full) | C(full) | A(noidx) | C(noidx) | **S_f** | S_s |
|---:|---:|---:|---:|---:|---:|---:|
| 4 | 3 522 146 | 4 011 805 | 2 461 123 | 2 988 123 | **1.179** | 0.878 |

> M19 note, added during the M19 accounting audit (no number above changed): the
dubcova3 traces used in this sweep carry a layout defect — the row-pointer array
was placed inside `x`. Rebuilt with the corrected builder, the 4-lane row reads
A 3 531 289 / C(noidx) 2 992 191 / **S_f = 1.180**. The defect is ≤0.3 % on this
dataset and does not affect the decay trend. See `m19-corrected-metrics.md`.
| 8 | 3 388 793 | 3 795 767 | 2 416 275 | 2 989 798 | **1.133** | 0.893 |
| 16 | 3 358 324 | 3 803 409 | 2 410 220 | 2 987 886 | **1.124** | 0.883 |
| 32 | *invalid* | 3 816 591 | 2 408 574 | 2 989 270 | — | — |

Two facts about the shape of the result:

* **`C(noidx)` is width-invariant** (bcsstk25 390 495 → 384 957; dubcova3
  2 988 123 → 2 989 270, within 1.5 %). The engine's index acquisition does not
  depend on how the SIMT warps are shaped, because it is not consuming the SIMT
  request stream at all.
* **`A(full)` improves with width** (bcsstk25 −11.8 %, dubcova3 −4.7 % from 4 to
  32 lanes) and then flattens by 16 lanes. Wider warps do help the baseline, and
  they help it on exactly the stream the engine is claiming.

## 4. Why it decays — the consolidation ratio

The mechanism is visible directly in the request counts. The engine's index
acquisition is **invariant**: 15 766 line requests for bcsstk25 and 99 347 for
dubcova3 at *every* width. The SIMT side's index cost, obtained by differencing
`full` against `noidx`, falls as warps widen:

| dataset | lanes | A line requests, full | A line requests, noidx | index-stream requests | engine index requests | **ratio** |
|---|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 4 | 323 809 | 244 236 | 79 573 | 15 766 | **5.05×** |
| bcsstk25 | 8 | 254 854 | 202 843 | 52 011 | 15 766 | **3.30×** |
| bcsstk25 | 16 | 217 549 | 179 476 | 38 073 | 15 766 | **2.41×** |
| bcsstk25 | 32 | 197 927 | 167 357 | 30 570 | 15 766 | **1.94×** |
| dubcova3 | 4 | 1 948 034 | 1 452 166 | 495 868 | 99 347 | **4.99×** |
| dubcova3 | 8 | 1 500 305 | 1 181 208 | 319 097 | 99 347 | **3.21×** |
| dubcova3 | 16 | 1 254 783 | 1 037 725 | 217 058 | 99 347 | **2.18×** |
| dubcova3 | 32 | 1 105 025 | 946 586 | 158 439 | 99 347 | **1.59×** |

The compulsory traffic never moves: `A` DRAM reads are 50 245 (bcsstk25) and
382 419 (dubcova3) at every width, and the engine's index DRAM reads are 15 766 /
99 347 — the same lines either way. What widens changes is the *request
amplification factor*: at 32 lanes a SIMT warp already coalesces four index
elements per request, so the engine's advantage over the warp is 1.6–1.9× rather
than 5×. The speedup decays with the ratio, as it should: **the M18 gain is
mostly the difference between a 5× and a 1.6× request amplification, and it is
therefore bounded by how well the competing SIMT warp coalesces.**

## 5. The 32-lane dubcova3 SIMT arm deadlocks — reported, not hidden

`bcsstk25` passes at 32 lanes. `dubcova3` `full` at 32 lanes does not:

```
VIOLATION: arm A: conservation violated
VIOLATION: A: cycle limit exceeded (deadlock guard)
VIOLATION: A: completed == requests_generated: 1105024 != 1105025
M9 GATES FAILED
```

Measured state at the guard (200 000 000 cycles; the same arm takes 3.36 M cycles
at 16 lanes):

| counter | value |
|---|---:|
| `A` cycles | 200 000 000 (guard) |
| peak slot-resident | 128 / 128 (`R`) |
| coalescer staging (peak) | 20 (< 32 = `lanes`, so *not* a latch overflow) |
| `A_dram_starved_cycles` | 196 665 814 |
| `A_l1_stall_cycles` | 283 273 042 |
| `A` line requests generated | 1 105 025 |
| `A` line requests completed | 1 105 024 |

What this is and is not:

* It is a **permanent stall**, not a slow run: the guard is 200 M cycles, ~60×
  the arm's own runtime at 16 lanes, and the machine sits at full residency
  (128/128 slots) with 20 lines waiting for a slot. Staging is below `lanes`, so
  the coalescer latch is not the constraint.
* It is confined to the SIMT-only arm on the index-laden trace: `C(full)`,
  `A(noidx)` and `C(noidx)` all complete and pass gates at 32 lanes on the same
  matrix. The extra pages of the index array (1 552 of 4 945 in the full trace)
  are what push the arm into the stalled state.
* The exact circular wait was **not** diagnosed. It sits in the translation and
  refill path (`dram_starved_cycles` = 98 % of the run means the DRAM is idle
  while requests are blocked upstream), and narrowing it further would mean
  changing the front end's page-walk parking/admission policy — a redesign, which
  M18A's terms of reference exclude.

**Consequence for the audit:** the 32-lane point is excluded from the dubcova3
row and the sweep's conclusion rests on 4/8/16. That is enough to establish the
trend (monotone decay, no reversal); it is not enough to claim what happens at
32 lanes on that matrix. Reported as a follow-up defect rather than patched.

## 6. Why this is a sensitivity and not a wide-GPU result

Even where the cells pass, `lanes` widens one thing only. A real 32-lane GPU
would also have:

* a wider LSU, so more than one line request formed per cycle;
* a deeper MSHR file (`l1_mshr = 16` here is a 4-lane-era number);
* more L1 banks/ports and a different sector policy;
* correspondingly different DRAM scheduling.

None of that is modelled by changing `lanes`, and nothing in M18A introduces it
by dividing instruction counts or multiplying throughput. The sweep therefore
answers exactly one question — *how much of the M18 advantage survives wider SIMT
warp formation at fixed memory system* — and answers it in a range where the
machine is internally consistent. The extrapolation "the gain would also hold on
a real 32-lane GPU with a commensurately wider LSU" is **not** established by
these numbers and is not claimed in `m18a-index-accounting-audit.md` §5.

## Reproduce

```
cd DX100_MEM
bash build/wm/m18_lanes.sh          # 12 cells, ~20 min, prints ok/FAIL per cell
```

The script's body is the whole experiment: for each `L` in 8/16/32 and each
matrix it builds `m18a_<m>_<v>_l<L>.bin` with `tools/build_trace.py spmv --mtx …
--variant <full|noidx> --lanes L` (dubcova3 adds `--max-rows 45000`), then runs
`./sim/build/wm/m18_run.sh <trace> 8192 <m>_<v>_w8192_l<L>`. The lanes=4 cells
are the M18 primary cells (`build/wm/m18_<m>_<v>_w8192.txt`) and are not
rebuilt.
