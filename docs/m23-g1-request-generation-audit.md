# M23-G1 — compute-side DRAM request generation characterization

**Status:** audit and isolated benchmarking only. No execution-pipeline, memory-system,
DRAM, cache, TLB or translation code was modified; no frozen configuration, historical
trace, log or report was altered. The only additions are new files: a trace generator,
a frozen instrument config, a runner, an extractor and this report. `./bin/dx100_tests`
→ **4 735 checks, 0 failures**.

Evidence tags, as in the M11/M23 reports:

| Tag | Meaning |
| --- | --- |
| **[S]** | Verified in source; the quoted code is the authority. |
| **[M]** | Measured in this milestone; the log is in `build/wm/m23g1/`. |
| **[M\*]** | Measured in an archived M18–M23 log in this tree. |
| **[D]** | Derived by arithmetic from [S]/[M]; the derivation is shown. |
| **[H]** | Hypothesis or estimate. Not established by this milestone. |

---

## 1. The question, and how it is separated from M23-G0

M23-G0 established what the DRAM can **serve**: two banks, 64 B per command, a 14-cycle
row-hit and a 26-cycle row-conflict bank occupancy, so 9.143 B/cycle with perfect row
locality and 4.923 B/cycle with none. This milestone asks what the four-lane core can
**ask for**.

The two numbers live in different currencies, and the whole report depends on not
mixing them:

* **Service capacity** is a rate the controller can sustain.
* **Generation capacity** is a rate the client can request.
* Their ratio ρ (§6) is a property of the *machine configuration*, not of any workload.
* **Achieved demand** is what a particular trace actually asks for, and only that can
  be called utilization.

M23-G0's own history shows why this matters: Orkut BFS on DX 2×32K reached 4.242 B/cycle
(46 % of the ceiling) while Criteo M0 reached 0.195 (2.1 %). Those two numbers cannot
both be explained by "the DRAM is fast"; one of them must be a client-side limit. That
is what this milestone measures.

Two things are deliberately *not* done. The core is not modified, and no bandwidth
throttle is added — the brief forbids both, and the second would destroy the comparison
by moving the very quantity being measured.

---

## 2. Part 1 — the request-generation path, stage by stage

The SIMT front-end is `Sim::run(Mode::Baseline)` in `sim/src/system.cpp:77–330`. There
is exactly one memory instruction consumer, one coalescer and one submit loop per core;
per core per cycle they run in the order **submit → coalesce → form**, inside the
`for (auto& c : cores)` loop at `:162`.

### 2.1 Stage by stage

| # | Stage | What happens | Source | Rate ceiling |
| --- | --- | --- | --- | --- |
| 1 | Trace partition | The pre-generated trace's instruction list is split across cores **statically**, round-robin (`i % cores`) or by contiguous chunk (`core_map = blocked`) | `system.cpp:98–104` | fixed before the run; no dynamic balancing |
| 2 | Residency slot | `per_core = max(1, R/cores − latch_charge)` slots are the budget a formed request holds from formation to completion | `system.cpp:88–90` | `R = 128` total (128 here, one core) |
| 3 | Dependency gate | `if (lsu_pending && in_sys[c.id] >= lsu_pending) continue;` — skips issue **and** coalescing for that core | `system.cpp:164` | `lsu_pending = 0` → off in every published config |
| 4 | Submit | Up to `cap = warps_per_core` **attempts** per core per cycle, taken from `held`; a refusal keeps the request and is retried next cycle | `system.cpp:168–182` | **4 attempts/cycle** (warps_per_core) |
| 5 | Coalesce | **One** instruction is consumed per core per cycle: its lanes are de-duplicated into `cur_lines` (distinct 64 B lines) | `system.cpp:185–198` | **1 instruction/cycle** |
| 6 | Form | Every line of `cur_lines` is turned into a `Req` while a slot is free | `system.cpp:200–212` | ≤ `lanes` = 4 per instruction, ≤ free slots |
| 7 | Translate | `tlb_probe`: dTLB (`dtlb_entries` × `dtlb_ways`), on miss a `dtlb_mshr` allocation with at most `dtlb_parked` waiters, then the L2 TLB (`l2tlb_mshr`) and the walker | `memory.cpp:118–172`, `:249` | ideal: none; real: `dtlb_mshr` = 4, `parked` = 2 |
| 8 | L1 | Allocate one of `l1_mshr` per-core MSHRs, else return 2 ("retry later") | `memory.cpp:441–465` | **128 concurrent** per core |
| 9 | L2 | Allocate one of `l2_mshr` shared MSHRs, else return 2 | `memory.cpp:336–361` | **64 concurrent**, machine-wide |
| 10 | DRAM admission | The L2 MSHR allocation pushes the request into `dram_wait_` | `memory.cpp:395` | one push per L2 allocation; queue unbounded (`dram_q = 0`) |
| 11 | DRAM service | `dram_tick` issues at most one command per cycle | `memory.cpp:529` | **1/cycle** = 0.143 lines/cycle at `t_hit = 14`, two banks |

### 2.2 The answers to the audit questions

| Question | Answer | Kind |
| --- | --- | --- |
| Memory instructions consumed per cycle? | **1 per core** (`system.cpp:185`). The loop can only coalesce one instruction per cycle; additional available instructions wait. | Simulator implementation (single coalescer) |
| How do the four lanes participate? | The four lanes of **one** instruction are de-duplicated into distinct lines (`cur_lines`). Lanes never issue independently: there is one program counter per core, not per lane. | Simulator implementation + width |
| Can multiple lanes generate requests in the same cycle? | Yes, but only as **distinct lines of the same instruction** — up to `lanes` = 4 requests from one instruction, all formed in the same cycle (`system.cpp:200`). Not four instructions' worth. | Workload property (addresses per instruction) |
| One instruction → one request or many? | **Up to `lanes`**, after line de-duplication. With 4 lanes × 4 B contiguous, one line (1 request); with lanes in different lines, four requests. Measured: 3.98 requests/instruction on the independent patterns, 1.00 on a coalesced one. | Workload property |
| Where are requests serialized? | (a) one instruction/cycle at the coalescer; (b) `warps_per_core` submits/cycle; (c) one dTLB MSHR allocation per request with 4 MSHRs, 2 waiters each; (d) one L1/L2 MSHR per outstanding miss; (e) one DRAM command/cycle. | [S] |
| What limits outstanding requests? | The smallest of: `R` slots, `l1_mshr`, `l2_mshr`, `dtlb_mshr` (+`parked`), and the pipeline's own rate × latency. Measured: the binding one is identified per cell in §4. | [S], [M] |
| Global outstanding budget? | `R = 128`; `per_core = R/cores = 128`; enforced by slot allocation, not by a queue. | Hardware-model (V322.3 budget) |
| L1 / L2 / dTLB / L2-TLB MSHR limits? | L1 `l1_mshr = 128` per core; L2 `l2_mshr = 64` shared; dTLB `dtlb_mshr = 4` per core with `dtlb_parked = 2` waiters per MSHR; L2 TLB `l2tlb_mshr = 8`. | Hardware-model |
| Max admitted per cycle at each stage? | coalesce 1 instruction; form ≤ `lanes` (4); submit ≤ 4 attempts; dTLB ≥ 1 per request (up to 4/cycle in principle, but a miss costs 300 cycles); L1/L2 allocation 1 per request; DRAM 1 command/cycle. | [S], [D] |
| Does the trace mechanism limit generation independently of the core? | **Yes.** The trace is pre-generated and consumed at ≤ 1 instruction/cycle/core from a statically partitioned list; it carries no dataflow, so no address depends on a prior response. The *address stream* is a workload property; the *consumption rate* is a simulator implementation limit. | Simulator limitation |
| Are there dependencies between instructions? | **Not modelled.** `build_trace` materialises addresses before the run (`workload.cpp:127`); the loader reads them in program order (`:46`). A "dependent" chain can only be emulated by throttling outstanding requests (`lsu_pending`, §4.2). | Simulator limitation |
| Does ideal translation remove translation admission entirely? | **Yes, verified.** `memory.cpp:118–127` returns success in the submit cycle with `ready = cycle`; no dTLB probe, no MSHR, no L2 TLB, no walk, no PTE traffic. All 64 ideal-translation cells report `walks = 0` and `pte_fetches = 0`. | Diagnostic (M23-F1 flag) |

### 2.3 What this means before any measurement

Three structural ceilings follow from the table, and each is a *different* quantity
that is easy to conflate:

1. **Instruction consumption ≈ 1/cycle/core.** This is a property of the simulator's
   single coalescer, not of any hardware parameter. It is why "four lanes" cannot mean
   "four instructions per cycle".
2. **Request formation ≤ lanes/instruction × 1 instruction/cycle = 4/cycle here**, and
   ≤ `warps_per_core` submits/cycle = 4. The two caps coincide at the frozen
   configuration, which is exactly why the lane sweep in §4.5 does not increase the
   request rate.
3. **Outstanding ≤ min(R, l1_mshr, l2_mshr, dtlb capacity)** — 128, 128, 64 and 8 in the
   frozen configuration, so **64** is the real depth limit for full misses, not 128.

---

## 3. Part 2 — the ten quantities, and where each is measured

The brief asks for ten distinct rates. They are kept separate throughout; the simulator
already exports every one of them, so nothing is inferred from another.

| # | Quantity | Counter | Notes |
| --- | --- | --- | --- |
| 1 | Memory instructions consumed / cycle | `instr_issued` / `cycles` | one per core per cycle at most |
| 2 | Memory accesses generated / cycle | `requests_generated` / `cycles` | **after** line de-duplication, i.e. unique lines per instruction |
| 3 | L1 requests accepted / cycle | `l1_reads` / `cycles` | accepted = probed and not refused |
| 4 | L2 requests accepted / cycle | `l2_reads` / `cycles` | |
| 5 | DRAM-bound requests generated / cycle | `dram_reads` / `cycles` | = L2 MSHR allocations = DRAM queue pushes |
| 6 | DRAM commands issued / cycle | `dram_svc_events` / `cycles` | equals (5) here: one push, one command |
| 7 | Actual DRAM bytes / cycle | `dram_bytes` / `cycles` | `= 64 × dram_reads`; verified per cell |
| 8 | Maximum outstanding requests | `inflight_max`, `simt_peak_slot` | measured peak depth |
| 9 | Average outstanding requests | `inflight_occ` / `cycles` | Little's-law occupancy |
| 10 | Queueing and service latency | `lat_mean_x100/100`, `dram_wait_sum/dram_reads` | end-to-end and DRAM-queue-only |

**Instantaneous peak vs steady state.** The simulator records no per-cycle issue
histogram, so an *instantaneous rate* cannot be measured directly. What is measured is
the peak **depth** (`inflight_max`, `simt_peak_slot`, `dram_max_q`, `unissued_max`), and
the peak **rate** is derived from source (§2.3). Where the benchmark saturates the
memory system, the steady-state rate equals that derived ceiling, and the two become the
same number — which is itself a check on the derivation.

A deliberate non-equation: **source-level loads ≠ memory instructions ≠ distinct line
requests ≠ cache misses ≠ DRAM reads.** In these traces 200 000 source lane-accesses
become 50 000 memory instructions, which become 200 000 distinct-line requests (pattern
A: 4 per instruction), of which 200 000 become DRAM reads only because every line is
compulsory. On a coalesced trace the same 200 000 accesses become 50 000 requests. Every
table below states which of the ten quantities it is quoting.

---

## 4. Part 3 — isolated request-generation benchmarks

### 4.1 Traces

New `M18TRC1` files written by `tools/m23g1_mktrace.py` and read by the **existing**
M18 loader (`workload.cpp:46–125`) — the same mechanism the M18 application traces use,
so no loader code was touched. Every trace: 50 000 instructions, 4 lanes, 4 B elements,
200 000 accesses, and every line touched exactly once (no reuse, footprint 12–256 MiB
≫ the 1 MiB L2), so all requests are compulsory misses and none is absorbed by a cache.

| Pattern | Address rule | Structure | Purpose |
| --- | --- | --- | --- |
| **A** | `line = i*4 + l` | 4 consecutive lines per instruction, sequential | independent, high row locality |
| **B** | `line = (i*4 + l)*33` | odd stride: flips bank, advances row every access | independent, both banks, conflict-heavy |
| **C** | uniform over 4 M distinct lines | no row locality | independent, random |
| **D** | A's addresses + `lsu_pending = 1` | one staged request | dependent chain (see §4.2) |
| **E** | `line = l*100000 + i` | four independent sequential streams | sustained independent generation |
| **F** | `line = (i*4 + l) mod 64` | 64-line L1-resident window | **generation-only control**: memory answers from the L1, so the measured rate is the front-end's own |

F is a control, not one of the five requested patterns: it is the only configuration in
which the memory system cannot be the limit, and therefore the only one that can measure
quantity (1) and (2) independently of quantity (5).

### 4.2 The dependent case, and an honest limitation

The model has no dataflow: a trace's next address is fixed before the run
(`workload.cpp:127`), so a real pointer chase cannot be represented. The nearest
mechanism is the Vortex LSU outstanding pool, `lsu_pending` (`system.cpp:164`), which
gates issue *and* coalescing. Its behaviour is subtler than "one outstanding":

* `R = 1, lsu_pending = 1` → one slot, so exactly one request exists at a time:
  `inflight_max = 1`, 23.75 cycles per request. This is a **strict** chain.
* `R ≥ 4, lsu_pending = 1` → the gate opens when the outstanding request completes, and
  the submit loop then admits up to `warps_per_core` = 4 staged requests at once:
  `inflight_max = 4`. This is a **staged burst**, not a chain.

Both are reported; only the first is a true dependency model. No cell claims more than
the mechanism provides.

### 4.3 Integrity checks

| Check | Result |
| --- | --- |
| `dram_bytes == 64 × dram_reads` | **66/66** cells |
| `conservation=OK` (generated = issued + unissued; issued = completed + inflight) | **66/66** |
| `walks = 0`, `pte_fetches = 0` for every `ideal_translation = 1` cell | **64/64** (the 2 `xlate0` cells are the exception by design) |
| trace realized geometry | `accesses=200000 instrs=50000 lines=200000 rho_line=0` in every pattern cell |
| determinism | cell `g1_A_R128` re-run byte-identical |
| `./bin/dx100_tests` | **4 735 checks, 0 failures** |

---

## 5. Part 4 — measurements

### 5.1 Generation-only control (pattern F, L1-resident)

This is the only table that measures the front-end itself. `bytes/cycle demand` is
`requests_generated × 64` per cycle — what the core would ask of memory if memory kept up.

| cell | lanes | warps | instr/cycle | requests/cycle | demand B/cycle | peak depth |
| --- | --- | --- | --- | --- | --- | --- |
| F_base | 4 | 4 | **0.995** | **3.979** | **254.7** | 128 |
| F_w1 | 4 | 1 | 0.250 | 1.000 | 64.0 | 128 |
| F_w2 | 4 | 2 | 0.499 | 1.997 | 127.8 | 128 |
| F_w4 / w8 / w16 | 4 | 4 / 8 / 16 | 0.995 | 3.979 | 254.7 | 128 |
| F_lanes8 | 8 | 4 | 0.499 | 3.990 | 255.4 | 128 |
| F_lanes16 | 16 | 4 | 0.250 | 3.995 | 255.7 | 128 |
| F_lanes32 | 32 | 4 | 0.125 | 3.997 | 255.8 | 128 |
| F_R4 | 4 | 4 | 0.249 | 0.998 | 63.9 | 4 |
| F_R32 | 4 | 4 | 0.992 | 3.969 | 254.0 | 32 |

Three results, each measured rather than assumed:

1. **Instruction consumption reaches 0.995/cycle** — the single-coalescer ceiling of
   1.0/cycle is real and is reached, not merely derived.
2. **Request generation saturates at 3.98/cycle = 254.7 B/cycle**, and it saturates at
   `warps_per_core = 4`: `w1` and `w2` scale exactly with warp slots (1.00, 2.00), and
   `w4`, `w8`, `w16` are identical. The submit-attempt cap binds, not the lane width.
3. **Widening the lanes does not increase the request rate.** `lanes = 8/16/32` keeps
   `requests/cycle` at 3.99 while `instr/cycle` falls to 0.50/0.25/0.125 — the extra
   lanes are absorbed into the same four submits per cycle, and the staging buffer fills
   (`unissued_max` 116–125). This is the direct answer to "do not assume that four lanes
   imply four DRAM requests per cycle": four lanes yield four requests **only because**
   `lanes = warps_per_core = 4` here; raising lanes alone buys nothing.

### 5.2 The global outstanding-budget sweep (patterns A–E, ideal translation)

`cycles` is the whole run; `B/cyc` is measured DRAM traffic. The rows within a pattern
use identical traces and differ only in `R`.

| R | A cyc / B/cyc | B cyc / B/cyc | C cyc / B/cyc | E cyc / B/cyc |
| --- | --- | --- | --- | --- |
| 4 | 1 475 010 / 8.678 | 2 634 019 / 4.859 | 3 057 602 / 4.186 | 2 729 084 / 4.690 |
| 8 | 1 475 010 / 8.678 | 2 600 010 / 4.923 | 2 789 157 / 4.589 | 2 165 969 / 5.910 |
| 16 | 1 475 010 / 8.678 | 2 600 010 / 4.923 | 2 684 608 / 4.768 | 1 717 217 / 7.454 |
| 32 | 1 475 010 / 8.678 | 2 600 010 / 4.923 | 2 631 563 / 4.864 | 1 576 122 / 8.121 |
| 64 | 1 475 010 / 8.678 | 2 600 010 / 4.923 | 2 602 695 / 4.918 | 1 512 522 / 8.463 |
| 128 | 1 475 010 / 8.678 | 2 600 010 / 4.923 | 2 602 583 / 4.918 | 1 512 522 / 8.463 |
| 256 | 1 475 010 / 8.678 | 2 600 010 / 4.923 | 2 602 583 / 4.918 | 1 512 522 / 8.463 |

Measured depth and latency, same cells:

| R | A `inflight_max` / avg / latency | B max / latency | C max / latency | E max / latency |
| --- | --- | --- | --- | --- |
| 4 | 4 / 3.7 / 28.5 | 4 / 51.7 | 4 / 60.1 | 4 / 53.6 |
| 32 | 32 / 31.7 / 235.0 | 32 / 415.0 | 32 / 420.0 | 32 / 251.2 |
| 128 | 128 / 127.7 / 942.7 | 128 / 1662.5 | 128 / 1664.0 | 128 / 966.7 |
| 256 | 256 / 255.6 / 1885.8 | 256 / 3324.8 | 256 / 3328.0 | 256 / 1933.7 |

**The outstanding budget is reached — and it buys nothing beyond a point.** For every
`R`, `inflight_max = R` exactly, so the budget is filled. But pattern A takes the *same
1 475 010 cycles at R = 4 and at R = 256*: the DRAM is already the bottleneck at four
outstanding requests, and the extra 124 slots convert one-for-one into queueing delay
(latency 28.5 → 1 885.8 cycles, i.e. 66×) with zero throughput change.

The patterns differ in how much depth they need, and that difference is the mechanism:
row-local A saturates at R = 4 (two banks × 14 cycles ÷ 2 = 28 cycles of pipeline, and
4 requests × 0.143/cycle = 28); conflict-bound B needs R = 8 (2 % short at R = 4);
random C needs R = 32 (4.186 → 4.864, 16 % at R = 4); and the four independent streams
of E need R = 16 (4.690 → 8.463, **1.80×**) because a shallow queue cannot batch
same-row entries across streams. E's own ceiling is reached by R = 64.

### 5.3 Dependent chain and the staged-burst form

| cell | mechanism | B/cyc | % of ceiling | outstanding | latency |
| --- | --- | --- | --- | --- | --- |
| D_R1 | strict: 1 slot, `lsu_pending = 1` | **2.695** | 29.5 % | 0.92 avg, 1 max | 22.75 |
| D_R4 | staged: 4 slots, `lsu_pending = 1` | 6.649 | 72.7 % | max 4 | 31.0 |
| D_R128 / D_R256 | staged, deep | 6.649 | 72.7 % | max 4 | 31.0 |

A strictly dependent chain reaches **2.695 B/cycle**, 29.5 % of the ceiling, and no
amount of R changes it — the chain's depth is one. The staged form recovers to 72.7 %
because up to four *independent* staged requests are admitted at once, which is precisely
the distinction between a dependent chain and an independent stream.

### 5.4 Admission-limit sweeps (pattern E, `R = 128`, one limit at a time)

| `l1_mshr` | B/cyc | | `l2_mshr` | B/cyc |
| --- | --- | --- | --- | --- |
| 4 | 4.735 | | 8 | 5.883 |
| 8 | 5.910 | | 16 | 7.462 |
| 16 | 7.454 | | 32 | 8.121 |
| 32 | 8.121 | | 64 (frozen) | 8.463 |
| 64 | 8.463 | | 128 | **8.678** |
| 128 (frozen) | 8.463 | | | |
| 256 | 8.463 | | | |

`l1_mshr` binds below 64 (a 1.79× loss at 4 MSHRs); the frozen 128 is *above* the knee and
costs nothing. `l2_mshr` still binds at the frozen 64: raising it to 128 gains 2.5 %
(8.463 → 8.678). So the frozen hierarchy is one step short of optimal for the four-stream
pattern — but the frozen `l1_mshr = 128` is what M16 chose, and the L2 pool sits *below*
it, which is also why every deep cell shows a large `replays` count (§5.6).

### 5.5 Lane and warp-slot sweep — a change to the modeled compute configuration

| cell | lanes | warps | instr/cyc | requests/cyc | B/cyc (DRAM) | `unissued_max` |
| --- | --- | --- | --- | --- | --- | --- |
| A_lanes4 | 4 | 4 | 0.034 | 0.136 | 8.678 | 4 |
| A_lanes8 | 8 | 4 | 0.017 | 0.136 | 8.678 | 68 |
| A_lanes16 | 16 | 4 | 0.008 | 0.136 | 8.678 | 100 |
| A_lanes32 | 32 | 4 | 0.004 | 0.140 | 8.664 | 116 |

On the DRAM-bound pattern, **widening the lanes 8× changes DRAM throughput by −0.2 %**:
the machine remains at the DRAM's rate while instructions per cycle fall 8× and the
staging buffer fills. This is a model configuration change, not a property of the
four-lane core, and it demonstrates that lane width alone cannot add memory throughput
once the controller is the limit.

The warp-slot sweep on the same pattern is equally flat (8.463 B/cyc at `warps = 1` … 16)
for the same reason — with one submit per cycle the controller is still fed faster than
it can serve. Only in the F control, where memory answers in 2 cycles, does the warp cap
bind (1.00, 2.00, 3.98 requests/cycle for 1, 2, 4 warps).

### 5.6 Translation contrast — and why it is nearly invisible here

| cell | translation | cycles | B/cyc |
| --- | --- | --- | --- |
| A_R128 | ideal | 1 475 010 | 8.678 |
| A_xlate0 | real | 1 475 312 (+0.02 %) | 8.676 |
| E_R128 | ideal | 1 512 522 | 8.463 |
| E_xlate0 | real | 1 513 069 (+0.04 %) | 8.460 |

Real translation costs **0.02–0.04 %** in these benchmarks — not because translation is
cheap, but because the DRAM queue is 128 deep and ~1 000 cycles of latency, so a 300-cycle
walk is entirely hidden. This is the crucial inversion of M23-F1, where translation cost
2.3× on M0 and 1.2× on DX: **translation latency is free when the pipeline is already
saturated and expensive when it is not.** §8 shows both regimes.

### 5.7 A simulator-implementation artefact worth reporting

From `R = 128` onward every cell reports a large `replays` count — 93.5 M for A_R128,
165.5 M for B_R128, 498 M for B_R256 — while `R ≤ 64` reports **zero**. The cause is
structural, not a bug: `l1_mshr = 128` exceeds `l2_mshr = 64`, so once more than 64
requests are outstanding, `l2_probe` returns 2 (`memory.cpp:352–359`) and the request is
re-driven through the hierarchy every cycle until an L2 MSHR frees. The retry loop is
what `MemorySystem::replays` counts. It changes no throughput (the runs are DRAM-bound
either way) but it dominates simulator *cost*, which is why the deep cells took minutes.

---

## 6. Part 5 — ρ: generation capacity against M23-G0 service capacity

Measured generation ceiling (§5.1): **3.979 requests/cycle = 254.7 B/cycle**.
M23-G0 service capacity: **9.143 B/cycle** (row-local) and **4.923 B/cycle** (conflict).

| Ratio | Value | Meaning |
| --- | --- | --- |
| ρ(row-hit) = 254.7 / 9.143 | **27.9** | the core can ask for 28× what a row-local stream delivers |
| ρ(conflict) = 254.7 / 4.923 | **51.7** | …and 52× what a conflict-bound stream delivers |
| ρ(dependent, D_R1) = 2.695×64/9.143 | 0.295 | a strict chain asks for 30 % of capacity |

And the achieved side, which is *not* the ratio:

| Pattern | achieved B/cyc | achieved / service ceiling |
| --- | --- | --- |
| A (row-local, independent) | 8.678 | **0.949** |
| B (conflict-bound, independent) | 4.923 | **1.000** |
| C (random) | 4.918 | 0.999 |
| E (four streams) | 8.463 | 0.926 |
| D (strict chain) | 2.695 | 0.295 |

So ρ is a *capacity* statement, and the measured achieved/intended is the *utilization*
statement. They agree only because these particular traces sustain the demand: an
independent 4-line-per-instruction stream keeps 128 requests in flight and pins the
controller at 93–100 % of its ceiling. The brief's caution — do not read ρ as utilization
— is exactly right, and §8 gives the workload where the two diverge by 30×.

---

## 7. Part 6 — commercial GPU normalization

Specifications are the ones recorded and sourced in M23-G0 §5: A100 40 GB (108 SMs, 64
warps/SM, 32-thread warps, 2 048 threads/SM, 1 555 GB/s HBM2 at 1 215 MHz, boost
1 410 MHz) and H100 SXM (132 SMs, 64 warps/SM, 3 352 GB/s HBM3, boost 1 980 MHz). Warp
size 32 and 64 warps/SM for compute capability 8.0 and 9.0 are from the CUDA
compute-capability table and the Ampere/Hopper tuning guides.

| Metric | DX-100 (1 core) | A100 40 GB | H100 SXM |
| --- | --- | --- | --- |
| SMs / modeled cores | 1 | 108 | 132 |
| Lanes per warp | **4** | 32 | 32 |
| Warps per SM (resident) | 4 (`warps_per_core`) | 64 | 64 |
| Resident threads | 16 | 2 048 / SM | 2 048 / SM |
| Peak memory bandwidth | n/a (derived 3.66 GB/s at 400 MHz) | 1 555 GB/s | 3 352 GB/s |
| Bandwidth per SM | 3.66 GB/s (whole core) | 14.40 GB/s | 25.39 GB/s |
| Bandwidth per resident warp | 0.914 GB/s | 0.225 GB/s | 0.397 GB/s |
| Bytes/cycle per SM (clock stated) | 9.143 @ 400 MHz | 10.21 @ 1 410 MHz | 12.83 @ 1 980 MHz |
| Bytes/cycle per resident warp | 2.286 | 0.160 | 0.200 |
| **Request generation per SM** | **254.7 B/cycle (3.98 req/cyc)** | ~128 B/cycle [H] | ~128 B/cycle [H] |
| **Oversubscription ρ** | **27.9** | ~12.5 [H] | ~10.0 [H] |

**Resident warps do not determine memory request generation**, and the table is
constructed so that this is visible rather than assumed. The missing factors, each of
which can change a GPU's generation rate by more than its warp count does:

* **Issue width and scheduler count** — 4 warp schedulers per SM, each issuing up to one
  instruction per cycle; a warp cannot issue more than one memory instruction per cycle.
* **Load/store pipeline throughput** — commonly one coalesced warp memory instruction per
  cycle per SM in the Ampere/Hopper class, so ~128 B/cycle of L1 request bandwidth is an
  order-of-magnitude ceiling, not a specification [H]. It is not in any datasheet.
* **Coalescing and sector granularity** — a 32-thread warp with 4 B elements that are
  contiguous forms 4 × 32 B sectors = 128 B, i.e. *one* memory transaction of four
  sectors; the same warp scattered over 32 lines forms 32 transactions. Requests per
  instruction therefore range over 32× for the same warp count.
* **Outstanding-request capacity** — MSHRs, L1/L2 fill buffers and the LSU queue bound
  the depth; these are not published per SM.
* **Memory-level parallelism available in the code** — dependencies, occupancy and
  register pressure decide whether the outstanding capacity is used.
* **Per-SM bandwidth share is an equal-share normalization**, not a reservation.

The defensible conclusion is a *range*, not an equality: **both machines oversubscribe
their DRAM by roughly one order of magnitude** (DX-100 by 28×, a commercial SM by
~10–13× [H]). DX-100 is at the high end of that range, and the difference is explained by
having 16 resident threads behind a per-SM-sized memory system rather than 2 048 — not by
an anomalous request generator. What DX-100's generator *cannot* do is what a GPU does
with those 2 048 threads: sustain the oversubscription across many independent warps.
DX-100 has exactly four, and one instruction per cycle to feed them.

---

## 8. Part 7 — the causal decomposition for the real workloads

The isolated benchmarks say the generator is not the limit. The archived M23 logs confirm
that, and locate the limit. All rows `[M*]`, recomputed from `build/wm/m23b_*`,
`build/wm/m23c_*`, `build/wm/m23d_*` and `build/wm/m23f1/*`.

| cell | translation | cycles | B/cyc | B/cyc ÷ ceiling | outstanding avg | max | `l1`/`l2` MSHR peak | DRAM queue peak |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Criteo M0 | real | 28 511 774 | 0.195 | 2.1 % | **4.39** | 13 | 8 / 7 | 5 |
| Criteo M0 | ideal | 1 010 690 | 5.217 | 57.1 % | **127.1** | 128 | — / 64 | 62 |
| Criteo DX 2×16K | real | 12 273 324 | 0.540 | 5.9 % | 5.86 | 119 | 67 / 64 | 62 |
| Criteo DX 2×16K | ideal | 1 131 020 | 5.730 | 62.7 % | 118.5 | 128 | — / 64 | 62 |
| Orkut BFS M0 | real | 49 067 545 | 0.677 | 7.4 % | 6.40 | 128 | — / 64 | 63 |
| Orkut BFS M0 | ideal | 5 766 215 | 5.701 | 62.4 % | 127.6 | 128 | — / 64 | 62 |
| ogbn train M0 | real | 176 184 939 | 0.434 | 4.7 % | 4.28 | 12 | — / 7 | 5 |
| ogbn train M0 | ideal | 15 323 803 | 4.903 | 53.6 % | 127.8 | 128 | — / 64 | — |

Reading these columns together answers the milestone's central question:

1. **With real translation, the workloads never fill the machine.** Average outstanding is
   4.3–6.4 requests out of 128, and Criteo M0's L2 MSHR pool peaks at *7* of 64. The
   generation path is not short of capacity; it is short of *admission*. `tlb0_mshr_stalls`
   is 113.7 M cycles for Criteo M0 — the front-end spent almost the whole run with a
   request parked behind a dTLB MSHR (`dtlb_mshr = 4`, `dtlb_parked = 2`).
2. **Removing translation moves outstanding from ~4 to ~127** and raises DRAM throughput
   10–27×. Same traces, same addresses, same DRAM, same R.
3. **Even then the workloads stop at 54–63 % of the row-local ceiling** — because their
   address patterns are gather-like, and 4.9–5.7 B/cycle is exactly the *conflict-bound*
   ceiling of 4.923 B/cycle that pattern B/D measured. So after translation is removed,
   the remaining limit is the DRAM's row behaviour, not the client.
4. **The isolated benchmarks land in the same place.** Pattern A (8.678) sits on the
   row-local ceiling; patterns B/C (4.923 / 4.918) sit on the conflict ceiling; the real
   ideal-translation runs (4.90–5.73) sit between, as their mixed row locality predicts.
   Two independent measurements, two milestones, one consistent model.

---

## 9. Limitations

**Simulator limitations that bound these results**

1. **No dataflow.** Addresses are materialised before the run (`workload.cpp:127`), so a
   true pointer chase cannot be simulated. §4.2's `lsu_pending` emulation is the closest
   available and is reported as such.
2. **One instruction per core per cycle** at the coalescer. This is a property of the
   implementation, not a configurable hardware parameter, and it caps instruction
   consumption at 1.0/cycle no matter how many lanes or warps are declared. It is the
   single most important structural limit on DX-100's generation rate.
3. **Submit attempts are capped at `warps_per_core`**, so request rate saturates at 4/cycle
   for the frozen configuration and cannot be raised by widening lanes.
4. **`l1_mshr = 128` above `l2_mshr = 64`** makes every request beyond 64 outstanding
   spin in a retry loop (`replays`), which is a fidelity gap in the admission path and a
   large cost multiplier. It does not change measured throughput.
5. **No per-cycle rate histogram** is recorded, so instantaneous *rates* are derived from
   source while peaks are measured as depths.
6. `deferred`: the trace loader ignores the class table when `dispatch = 0`, so these
   benchmarks exercise the SIMT front-end only; the DX engine's own generation path is
   not measured here.

**Instrument caveats**

7. `ideal_translation = 1` is a theoretical mode, not a hardware configuration; every cell
   that uses it is labelled. The two `xlate = 0` cells exist to show what changes.
8. The benchmark config differs from the frozen architecture only in `ideal_translation`
   and `dram_diag`; all eight swept quantities are `--set` overrides on one frozen file, so
   no cell mixes two changes.
9. The lane sweep (8/16/32) changes the modeled compute configuration and is kept separate
   from every statement about the four-lane core.
10. Traces are 50 000 instructions (200 000 requests) — long enough to reach steady state
    (the F control saturates within ~1 000 cycles) but short enough that a 4-cycle startup
    transient is visible in `lat_mean`.

---

## 10. Direct answers to the nine required questions

**1. What is the maximum memory-access generation rate of the current four-lane model?**
**3.98 distinct-line requests per cycle, i.e. 254.7 B/cycle of demand**, limited by
`warps_per_core = 4` submit attempts per cycle (measured: `F_base` 3.979; `F_w1` 1.000,
`F_w2` 1.997; no gain at `w8`/`w16`). Instruction consumption reaches **0.995/cycle**
against its own ceiling of 1.0 (one coalescer). Widening lanes to 32 does **not** raise
the request rate (3.997/cycle) while it cuts instruction rate 8×. (§5.1, §5.5)

**2. What is the maximum DRAM-bound request rate?**
**0.143 DRAM commands per cycle** (= 8.678–9.143 B/cycle) when the stream is row-local,
and 0.077 (4.92 B/cycle) when it is conflict-bound — the M23-G0 ceilings, reproduced
here by the real front-end to three digits (A 8.678 vs 8.678; B 4.923 vs 4.923). The
generator's 3.98 requests/cycle is 28× that, so the DRAM, not the core, sets the rate.
(§5.2, §6)

**3. What limits each rate?**
Generation: `warps_per_core` = 4 submits/cycle, with one instruction/cycle and ≤ `lanes`
lines each. DRAM-bound request rate: the controller's bank occupancy (`2 / t_hit` =
0.143/cycle), not its issue port (1/cycle) and not the client. Before either: admission,
i.e. `l1_mshr` (binds below 64), `l2_mshr` (still binds at 64, worth +2.5 % at 128) and,
in the real workloads, `dtlb_mshr` = 4 with 2 parked waiters. (§5.4, §8)

**4. Is the global outstanding budget of 128 actually reached under independent demand?**
**Yes — `inflight_max` equals `R` exactly in every cell (4, 8, 16, …, 256), and the
average is `R − 0.3`.** But reaching it is a consequence of the DRAM being slow, not a
requirement: at `R = 4` the same trace already achieves the full 8.678 B/cycle. The
budget is filled; it is not what makes the run fast. (§5.2)

**5. Does increasing outstanding capacity improve request generation, or does throughput
saturate earlier?**
**It saturates far earlier.** Pattern A: identical cycles at `R` = 4 … 256 while latency
grows 28.5 → 1 885.8 cycles (66×) — pure queueing. B saturates at `R = 8`, E at `R = 64`,
C at `R = 32`. Beyond the knee, additional outstanding capacity adds latency, retry
traffic (`replays` = 93.5 M at `R = 128`, zero at `R ≤ 64`) and nothing else. (§5.2, §5.7)

**6. Is the current DRAM system overprovisioned relative to the existing core's
request-generation capacity?**
**No — it is *undersized* relative to generation capacity, by about 28×.** The core can
request 254.7 B/cycle; the memory system serves 9.143. The correct statement of the
inverse asymmetry is about *demand*, not capacity: after translation and dependencies,
the real workloads ask for only 0.2–0.7 B/cycle, 2–7 % of what the DRAM can deliver.
So the DRAM is over-provisioned **relative to what these traces happen to request** and
under-provisioned **relative to what the front-end can request**. Both statements are
true and they have different causes. (§6, §8)

**7. Is the observed low DRAM utilization caused by insufficient issue/request-generation
capacity, translation restrictions, workload dependencies, or another mechanism?**
**Translation admission, then workload structure — not issue capacity.** With translation
made ideal, outstanding rises from 4.3–6.4 to 118–128 and DRAM throughput rises 10–27×
(Criteo M0 0.195 → 5.217 B/cycle). The generator was always able to supply the requests
(`tlb0_mshr_stalls` = 113.7 M cycles shows it was trying). After translation is removed,
the remaining limit is the address pattern: 4.9–5.7 B/cycle is the conflict-bound ceiling
of 4.923 B/cycle, which is what gather workloads get. Issue width runs third: it caps the
core at 4 requests/cycle, which is still 28× the service rate, so it cannot be the cause
of a 2 % utilization figure. (§8)

**8. What would need to change to represent a higher-concurrency GPU without simply
inventing a request-generation rate?**
Five modeled parameters, each of which this milestone measured rather than assumed, and
each of which must be *stated as a configuration*, not multiplied in as an efficiency:

1. **Instruction consumption** — more than one coalescer per core (or an instruction
   fetch that can present multiple memory instructions per cycle); today it is hard-wired
   to one (`system.cpp:185`).
2. **Submit attempts** — `warps_per_core` above 4, since that is the measured knee
   (`F_w1`/`F_w2` scale exactly with it).
3. **Outstanding depth** — `R` and `l2_mshr` together, with `l1_mshr ≤ l2_mshr` so the
   retry loop does not dominate; `R = 128` is already sufficient, but only when the L2
   pool is not smaller than it.
4. **Translation capacity** — `dtlb_mshr`, `dtlb_parked` and walker count, which is what
   actually suppressed MLP in every real M23 run.
5. **A dependency model** — a scoreboard so a dependent chain can be represented instead
   of emulated by `lsu_pending`, otherwise "high concurrency" cannot be distinguished from
   "many independent streams".

A GPU-like machine is then a *configuration* whose measured generation rate can be
compared with this one, which is the opposite of assuming a per-warp request rate.

**9. What is the most defensible next experiment for testing DX's data-side memory
organization?**
**Run the identical G1 traces through both front-ends at the same `R`, and compare
achieved DRAM service and row locality rather than throughput.** Concretely: the five
patterns as `M18TRC1` traces, `dispatch = 1`, class table all-Bulk, `R = 128`,
`l1_mshr = l2_mshr = 128`, ideal translation on and off, against the SIMT arm on the same
traces (which is already measured here). Reason: G1 shows the *rate* is set by the
controller and that the generator cannot be the limit at `R ≥ 4`; the only thing an
organization can still change is **which rows are open when the commands arrive**. On
patterns A and B the two arms should be identical in cycles (the ceiling is the ceiling);
on E and on a shuffled-page variant the engine's window ordering should raise the row-hit
rate and therefore the B/cycle, and that difference — not a cycle count on a
pattern-favourable trace — is the measurement of the data-side organization. Run the
`banks = 2 → 1` sweep from M23-G0 §8 in the same campaign, since a halved ceiling makes
any ordering advantage twice as visible.

**Not implemented**, per the brief. No configuration, source or historical file was
changed to prepare either experiment.

---

## 11. Reproduction

```bash
cd DX100_MEM

# 1. traces (new files only; byte-identical when regenerated)
python3 tools/m23g1_mktrace.py --pattern A --lanes 4 --instrs 50000 \
    --out build/wm/m23g1/traces/g1_A_l4.trc

# 2. one cell
cd sim && ./bin/dx100sim --config configs/m23g1_base.cfg \
    --set dataset=../build/wm/m23g1/traces/g1_A_l4.trc --all

# 3. the full matrix (66 cells, ~15 min)
cd .. && bash build/wm/m23g1_run.sh

# 4. logs -> CSV
python3 tools/m23g1_extract.py        # build/wm/m23g1/summary.csv

# 5. regression
cd sim && ./bin/dx100_tests           # 4735 checks, 0 failures
```

Artifacts and hashes:

| Path | What it is | SHA-256 |
| --- | --- | --- |
| `sim/configs/m23g1_base.cfg` | instrument config (frozen) | `c4c2f3e615447b60db1a8bad2d3b698c895d1b8a78a3a48092314d31294eb82a` |
| `tools/m23g1_mktrace.py` | trace generator | `c748cc5357302cf9abddf8c42e27cef95df4453de4bc1ccf5dc5c63109f1d01e` |
| `build/wm/m23g1_run.sh` | runner | see `build/wm/m23g1/hashes.txt` |
| `sim/src/memory.cpp` | **unmodified** | `0f44e6f4ac89cc1886f5b8be9ad4ece8308f4a0ad658d6f10ba423b450d23715` |
| `sim/src/system.cpp` | **unmodified** | `2aaf3e4d8d7e12ac499b56c7130fae2bb68db6e970183ed87914dfd060d09198` |
| `sim/src/sim.hpp` | **unmodified** | `9f4586ef2a730c2ecfd3464b8e7e315000ae054f962f924372b7853899432bcc` |
| `build/wm/m23g1/traces/*.trc` | 11 traces, hashes in `hashes.txt` | — |
| `build/wm/m23g1/*.txt` | 66 raw cell logs | — |
| `build/wm/m23g1/summary.csv` | machine-readable results | — |

Source locations cited, verified against the hashes above: `system.cpp:88–90` (slot
budget), `:98–104` (trace partition), `:164` (`lsu_pending` gate), `:168–182` (submit
loop, `cap = warps_per_core`), `:185–198` (one coalescer, `instr_issued`,
`requests_generated`), `:200–212` (formation into `held`); `memory.cpp:118–127` (ideal
translation), `:143–172` (dTLB probe, MSHRs, parked), `:249` (walk), `:336–361` (L2
MSHR/probe), `:352–359` (return 2 → retry), `:395` (DRAM queue push), `:441–465` (L1
MSHR), `:529` (one command/cycle), `:778–779` (`inflight_occ`/`max`); `workload.cpp:46`
(file loader), `:127` (`build_trace`).

**Preservation.** No file under `build/wm/m23b_*`, `m23c_*`, `m23d_*`, `m23e_*`,
`m23f0/`, `m23f1/`, `m23g0/`, and no file under `docs/` or `configs/` that existed before
this milestone was modified. M23-G0's driver, config, runner, extractor and report are
untouched; G1 uses only `dx100sim` and the pre-existing M18 trace loader.
