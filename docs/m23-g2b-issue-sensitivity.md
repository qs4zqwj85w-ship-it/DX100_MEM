# M23-G2B — SIMT instruction-issue sensitivity

**Question.** Does the SIMT memory-instruction issue rate limit the supply of
DRAM requests?

**Answer in one line.** No. With ideal translation the front-end admits a memory
instruction in **11.1 % (Criteo) / 2.0 % (ogbn)** of cycles, submits
**0.36 / 0.08** distinct-line requests per cycle out of a 4-per-cycle path, and
still lands **99.8 % of the DRAM model's sustained command rate** — because
88.9-98.0 % of the cycles in which no instruction is issued are cycles in which
the coalescer is already holding an instruction whose lines have nowhere to go
(the 128-entry `R` pool is full). Two instructions per cycle would put requests
into a queue that is already full, not into the DRAM. Requests stop progressing
at the `R` slot pool, and the rate at which they leave it is set by a DRAM that
issues one command per **24.2-25.8 bank-cycles**.

The two-wide variant was **not implemented**: on this front-end the
one-instruction-per-cycle rule is the coalescer's data structure, not a width
parameter, so supporting it faithfully is a redesign of the mechanism under
measurement (§2). An exact instrumentation-only substitute exists and needs no
code change at all (§3).

---

## 1. What was run

Four cells: Criteo and ogbn-products train, ideal translation, at the frozen
L2 MSHR capacity (64) and at 128. 64 is the baseline; 128 is a contrast because
M23-G2A showed that at 64 the front-end spends 60 M / 976 M cycles being refused
L2 MSHR allocations. If the issue and supply rates are the same at both, they
are set by neither.

| field | value |
|---|---|
| front-end | SIMT, `dispatch = 0`, `batch_window = 0` (the frozen M23 `m0` arm) |
| core | `cores` 1, **`warps_per_core` 4 (unchanged)**, `lanes` 4, `elem_bytes` 4 |
| budget | `R` 128, `lsu_pending` 0, `mem_path = l1` |
| L1 / L2 | 256x4 / 2 cy, `l1_mshr` 128; 16384x8 / 4 cy, `l2_mshr` **64 or 128** |
| translation | `ideal_translation = 1` (no dTLB, no walker, no PTE traffic) |
| DRAM | 2 banks x 4096 rows, 1024 B rows, `t_hit` 14, `t_miss` 26, FCFS, hash |
| clock | 400 MHz, 64 B per command |

Configuration `sim/configs/m23g2b_base.cfg` — field-identical to
`configs/m23g2a_base.cfg` (verified: the two files differ in comments only) —
and **no simulator source was changed for this milestone**. The binary is the
one M23-G2A ran: `sim/bin/dx100sim` `2556823908ba3b6b…`,
`system.cpp` `2aaf3e4d…`, `memory.cpp` `0f44e6f4…`, `sim.hpp` `9f4586ef…`.
All four cells reproduce the corresponding M23-G2A logs **byte for byte**.

```bash
./build/wm/m23g2b_run.sh          # four cells -> build/wm/m23g2b/
python3 tools/m23g2b_extract.py   # summary.csv, checks.txt
```

---

## 2. Why two instructions per cycle was not implemented

The brief allows declining it if the simulator cannot support it with a small
isolated change. It cannot, and the reason is structural.

The admission rule is one `if` in the baseline loop (`sim/src/system.cpp:185`):

```cpp
if (c.cur_pos >= c.cur_lines.size() && c.next < c.instrs.size()) {   // :185
  const u32 id = c.instrs[c.next];
  c.cur_lines.clear(); c.cur_pos = 0;                                // :188-189
  for (u32 i = begin[id]; i < begin[id + 1]; ++i) {                  // :190
    const u64 line = tr_->acc[i].line;
    if (std::find(...) == c.cur_lines.end())                         // :192
      c.cur_lines.push_back(line);                                   // :193
  }
  c.next++; s.inc("instr_issued"); ...                               // :195-197
}
```

1. **The rule is the state, not a knob.** `cur_lines` + `cur_pos` hold exactly
   one instruction's line list and its drain cursor, and the predicate itself
   requires the *previous* instruction to be fully staged. Wrapping this body in
   `for (n = 0; n < 2; ++n)` can never admit a second instruction in the same
   cycle: after the first iteration `cur_pos == 0 < cur_lines.size()` unless the
   instruction produced zero lines. There is no cycle-width variable to raise.
2. **A faithful 2-wide design is a new coalescer.** It needs a second line list
   and cursor, a second latch bound (the invariant at `:267` is
   `latch_core_max <= lanes`, and two resident instructions make the latch
   structurally `2 x lanes`), an interleaved order into the shared `held` FIFO
   and `seq` space (`:199-208`), and an arbitration rule between the two lists
   for free `R` slots. That is the front-end being measured, rewritten.
3. **The cheap shortcut breaks the experiment's own control.** Concatenating two
   instructions into the existing `cur_lines` would dedup lines *across*
   instructions at `:192`, so `requests_generated` would fall below the baseline
   per-instruction coalescing and the address stream would no longer be the one
   the frozen traces define — the one thing a supply experiment must hold fixed.
4. **It could not have changed the outcome.** Even a perfect 2-wide coalescer
   cannot raise the request rate above 4 per cycle, because submission is capped
   by `cap = warps_per_core` (`:170`), and **one** instruction per cycle already
   supplies 3.26-3.96 distinct lines — enough to occupy every submit slot. The
   measured submit utilisation is 2-9 %.

Since `warps_per_core` is explicitly not a substitute for issue width, no
variant of §2.2 was run: raising it would have changed the warp count, which is
a different experiment (M23-G1 already swept it).

---

## 3. Smallest instrumentation-only alternative

No new counter is required. Two already-exported counters give the brief's
"eligible but unissued" exactly:

- **`eligible_unissued_cycles = cycles - instr_issued`.** The trace is static and
  an instruction is pending in every cycle until the last one is admitted
  (`instr_issued == trace_instr` at the end of all four cells), and the *only*
  gate on admission is the one-per-cycle predicate at `:185` — line formation
  into `held` is gated by slots (`:199`), but coalescing itself is not. So every
  cycle without an admission is a cycle in which an instruction was eligible and
  not issued, and the count is exact, not an estimate.
- **`simt_slot_full_cycles`** (written at `:246`: a core holds formed lines and
  has no free `R` slot) says *why* those cycles were idle: 99.7-100 % of them are
  `R`-budget backpressure, i.e. the coalescer was not short of an instruction
  slot but of a destination.

The pair bounds what any issue widening could win without simulating it: an
instruction admitted in one of those cycles could not have formed a request,
because the pool it forms into was full. If a direct counter were ever wanted,
the smallest addition is one line inside the existing `if` under a new
default-off flag — but it would restate `cycles - instr_issued` and would change
the hash of the file every published SIMT number was produced by, so it was not
done.

---

## 4. Measurements

Per cycle, over the whole run. `m64` is the frozen baseline, `m128` the
no-allocation-failure setting. Ceilings are properties of the frozen design:
1 instruction/cycle (one coalescer), 4 addresses/cycle (`lanes`), 4 submits/cycle
(`warps_per_core`).

| metric (per cycle) | ceiling | Criteo m64 | Criteo m128 | ogbn m64 | ogbn m128 |
|---|---|---|---|---|---|
| memory instructions issued | 1 | 0.1094 | **0.1109** (11.1 %) | 0.0198 | **0.0200** (2.0 %) |
| eligible-but-unissued cycles | — | 89.06 % | **88.91 %** | 98.02 % | **98.00 %** |
| … of which `R`-budget starvation | — | 99.74 % | **99.69 %** | 99.98 % | **99.99 %** |
| coalescer stalled on a full `R` pool (`slot_full_cycles / cycles`) | — | 88.82 % | **88.63 %** | 98.00 % | **97.99 %** |
| addresses generated | 4 | 0.6322 | **0.6405** | 0.0783 | **0.0790** |
| distinct-line requests generated | 4 | 0.3565 | **0.3612** | 0.0783 | **0.0790** |
| … per instruction | 4 | 3.258 | 3.258 | 3.958 | 3.958 |
| submit utilisation (`issued / warps`) | 4 | 8.91 % | **9.03 %** | 1.96 % | **1.98 %** |
| L1 misses | — | 0.2346 | 0.2373 | 0.0783 | 0.0790 |
| L2 misses | — | 0.0815 | 0.0825 | 0.0766 | 0.0773 |
| DRAM arrivals | — | 0.0815 | **0.0825** | 0.0766 | **0.0773** |
| DRAM completions | — | 0.0815 | **0.0825** | 0.0766 | **0.0773** |
| DRAM sustained capacity (2 banks / mean service) | 1 | 0.0819 | **0.0827** | 0.0772 | **0.0775** |
| DRAM arrivals as % of that capacity | 100 % | 99.51 % | **99.77 %** | 99.23 % | **99.75 %** |
| DRAM bank-busy | — | 99.51 % | **99.77 %** | 99.23 % | **99.75 %** |
| total cycles | — | 1 010 690 | **997 570** | 15 323 803 | **15 190 916** |

Peak `R` occupancy is 128 with 132 outstanding (the four-line coalescer latch
sits outside the budget, as designed) in all four cells; the held queue never
exceeds 4 and the staging buffer never exceeds `lanes` = 4.

### 4.1 Issue capacity is not the limit — it is barely used

The front-end issues 0.11 instructions per cycle on Criteo and 0.02 on ogbn,
against a ceiling of 1. Those are 9x and 50x below the limit. The instruction
stream is never short of work: every trace instruction is admitted
(`instr_issued == trace_instr` = 110 592 / 303 162), and the aggregate count of
"an instruction was available and none was admitted" is 88.9 % / 98.0 % of
cycles. But in **99.7 % / 100.0 %** of those cycles the coalescer was holding
lines it could not place: `simt_slot_full_cycles` is 884 195 of 886 978 eligible
cycles on Criteo and 14 885 713 of 14 887 754 on ogbn. The front-end is idle
because the destination is full, not because it cannot fetch an instruction.

### 4.2 The request path is 2-9 % used, and the DRAM is 99.8 % used

One instruction yields 3.26 (Criteo) or 3.96 (ogbn) distinct lines — i.e. the
coalescer already presents about as many requests as the submit path can carry
(`warps_per_core` = 4, `system.cpp:170`). Actual submission is 0.36 / 0.08
requests per cycle, 9.0 % / 2.0 % of that width. Neither the 4-submit width nor
the 1-instruction width is anywhere near binding.

What *is* binding is downstream. The DRAM model issues one command per
**24.18** (Criteo) or **25.82** (ogbn) bank-cycles, so across two banks its
sustained rate is 0.0827 / 0.0775 commands per cycle. Measured arrivals are
0.0825 / 0.0773 — **99.8 % of that capacity** — with both banks busy 99.8 % of
the time. The pipeline is a queue in front of a bus that is already issuing as
fast as its own row-hit mix allows.

### 4.3 Where additional requests stop progressing

```
instr admission   1/cycle         used  2-11 %   <- not the limit
  coalesce        <=4 lines/instr  used 81-99 %   <- full, but only to feed 4 submits
  submit          <=4/cycle        used  2-9 %   <- not the limit
  R slots         128              used 100 % (peak 128, 88.6-98.0 % of cycles full)  <- first wall
  L2 MSHR         64 / 128         used 95.7-99.9 %                                  <- one stage deeper
  DRAM command slot 1/cycle         used 99.8 % of the model's sustained rate        <- the wall
```

An extra instruction per cycle adds requests at the second line of that list.
They would enter the `held` queue (never longer than 4 today), then wait for an
`R` slot that is occupied 88.6-98.0 % of the time, then join a DRAM queue that
is 120 commands deep (M23-G2A) in front of a bus at 99.8 % of its capacity.
Deeper supply buys latency, not commands. This is also why the 64 → 128 L2 MSHR
change barely moves the rates here (cycles -1.3 % / -0.9 %, M23-G2A): the
bottleneck is two stages below the front-end in every case.

---

## 5. Verification

All four cells pass (`tools/m23g2b_extract.py`, `build/wm/m23g2b/checks.txt`):

| check | result |
|---|---|
| `conservation=OK`, `conservation_ok=1`, no violations | 4/4 |
| `instr_issued == trace_instr` (every instruction admitted) | 4/4 |
| `requests_generated == requests_accepted == completed` | 4/4 |
| `requests_unissued_end == 0` | 4/4 |
| `dram_bytes == 64 x dram_reads` | 4/4 |
| `l2_reads == l2_hits + l2_misses` | 4/4 |
| `dram_reads == l2_mshr_allocs == dram_svc_events` (arrivals = completions = fills) | 4/4 |
| `staging_max <= lanes` (4), `peak_slot <= R` (128) | 4/4 |
| `walks = 0`, `pte_fetches = 0` (ideal translation inert) | 4/4 |

**Determinism / cross-milestone check.** Each of the four logs is byte-identical
to the corresponding M23-G2A log for the same trace and capacity
(`diff` empty), which is an independent reproduction of that milestone's cells
from a fresh runner and a re-declared config.

**Regression suite.** `./bin/dx100_tests` → **4 735 checks, 0 failures**. No
simulator source file was edited for M23-G2B, so the suite validates the same
binary that produced every cell here.

---

## 6. Limitations

- One core, 4 warps, 4 lanes; no SMT, no multiple issue anywhere in the model.
  "Instruction per cycle" here means a memory instruction presented to a single
  coalescer, not a GPU's warp-scheduler issue slots.
- `ideal_translation = 1` by request: these cells say nothing about the
  translation-bound behaviour of the same traces (M23-B/C/D, M23-F1), where a
  dTLB MSHR, not issue width, is the first limiter.
- The DRAM model has no data bus and issues one 64 B command per cycle
  (M23-G0); "capacity" in §4 is a command rate derived from the model's own
  service times, not a bandwidth measurement.
- The 2-wide question is answered by measurement of the *existing* front-end
  plus the structural argument in §2, not by executing a 2-wide variant. If a
  2-wide coalescer is ever built, this report's numbers are the baseline it must
  be compared against.

---

## 7. Files

| path | what |
|---|---|
| `docs/m23-g2b-issue-sensitivity.md` | this report |
| `sim/configs/m23g2b_base.cfg` | instrument configuration (fields identical to `m23g2a_base.cfg`) |
| `build/wm/m23g2b_run.sh` | four-cell runner |
| `tools/m23g2b_extract.py` | extractor, derived rates and accounting checks |
| `build/wm/m23g2b/g2b_{criteo,ogbn}_m{64,128}.txt` | raw logs |
| `build/wm/m23g2b/summary.csv` | one row per cell |
| `build/wm/m23g2b/checks.txt` | per-cell verdicts, trace identity |
| `build/wm/m23g2b/hashes.txt` | config / runner / extractor / binary / source / trace SHA-256 |

No historical config, trace, log, report or source file was modified.
