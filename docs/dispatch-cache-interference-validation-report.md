# Dispatch, cache interference and workload generality — validation report

**Scope.** Three questions left open by M13/M14 and the hardware-cost audit:
does compiler-directed dispatch route operations as designed; why does 2 × 32K
regress on the κ = .75 hybrid workload even though the Bulk-only arm improves;
and do the buffer-capacity results generalize beyond W1. This is a validation
milestone: no dispatch rule, window policy, cache policy, baseline or default is
changed. The only source changes are instrumentation that is off unless
`cache_diag=1`, plus the separate experimental representation of
`fused-window-representation-report.md`.

Everything below is measured from the simulator in this tree
(`./bin/dx100sim`, `./bin/dx100_tests`). Reproduction commands are in §7 and the
raw logs are `sim/build/wm/m15_*.txt` (202 cells).

**Headline answers.**

| question | answer |
|---|---|
| Is SIMT dispatch correct? | Yes. One metadata predicate, evaluated before any request exists; classification is bit-identical across buffer capacities; unaccounted operations are zero in all 202 cells. |
| Does the SIMT path itself regress at 2 × 32K? | No. Its per-requester cache counters are **identical** at 8K and 32K, and its completion time moves by **+186 cycles (+0.15 %)**. |
| What causes the regression? | The **Bulk** stream loses **2 198 L2 hits** (13.1 % → **0.0 %**) that it was getting from lines the *SIMT* stream had brought into the shared L2. The 32K window's line footprint is the whole 1 MiB L2, so one window evicts the cache it was reusing. |
| How much of the difference is SIMT vs Bulk? | **100 % Bulk.** ΔC = +18 378 cycles; Δ(SIMT completion) = +186 (1.0 % of it), Δ(Bulk completion) = +18 378 (100 %). |
| Is it cache interference, scheduling, or something else? | **Cache capacity.** Enlarging the shared L2 to 2 MiB reverses the sign: 32K becomes **−2.23 %** (faster). Reversing the arbitration tie-break changes the absolute level of both configurations but leaves the gap's sign and mechanism intact. |
| Does the 32K advantage generalize? | Yes, conditionally. It wins where the engine is the throughput limiter (PSCR −48 %/−74 %, DUP −27 %) and is exactly neutral where it is idle (STRID 0.00 %) or L2-resident (STRIDmix ±0.1 %). The predictor is *window line footprint vs shared L2*, not the workload family. |

---

## 1. Dispatch: implementation and correctness

### 1.1 What the code actually does

Dispatch is **not** a runtime policy, a sampler, or a reaction to buffer
availability. There is one predicate, applied to compiler/workload metadata
before any request or descriptor exists (`sim/src/bulk.cpp`):

* `build_op_classes()` (`workload.cpp`) fills a `ClassMap` — one byte per memory
  instruction — from the metadata source named by `Config::op_class`. The
  published source is `gen`: the class the *generator* recorded when it built the
  instruction (`Trace::instr_class`, written at `workload.cpp:88-90`). The
  `pattern` source recomputes the same decision from addresses with the
  contiguity rule `kappa_real` uses, and exists only as a cross-check. `simt` and
  `bulk` are uniform maps used by the equivalence gates G1/G2.
* `build_bulk_workload(cfg, trace, err, &cm)` walks the trace once and copies the
  **byte address of every Bulk-class element** into the index array, in trace
  order, in the same pass that assigns `w.elem_ids[j]`. A SIMT-class element is
  never written into the index array, so the engine has nothing to fetch for it.
  With `cm == nullptr` — every published M5–M8 path — the selection is the whole
  trace, which is why those results are unchanged.
* `run_hybrid()` filters each core's instruction list with the same `cm` and then
  runs two front-ends over one `MemorySystem` and one `SlotPool`. **Nothing in
  the driver classifies**: `ClassMap` is an input, and the comment at the
  dispatch site states the predicate is evaluated "before any request is formed".

There is no fallback path and no re-classification: an operation cannot change
class mid-run, because the only object that could change (`cm`) is `const` in the
driver and already consumed by the workload builder.

### 1.2 Routing, admission, completion, and what is rejected or retried

Measured per cell (`m15` counters; `A` = SIMT only, `B` = bulk only, `C` =
dispatched). The κ = .75, N = 65 536, W = 8 192, seed 1 cell, which is
representative:

| quantity | SIMT path | Bulk path | identity |
|---|---|---|---|
| instructions classified | 12 277 | 4 107 | 16 384 = `trace_instr` |
| elements classified | 49 108 | 16 428 | 65 536 = `trace_accesses` |
| admitted | 12 277 line requests formed | 16 428 elements into windows, 16 421 target lines issued | — |
| submitted to hierarchy | 12 277 | 17 449 (1 descriptor + 1 027 index + 16 421 target) | — |
| completed | 12 277 | 16 428 elements; 16 421 line fills | — |
| rejected / faulted | 0 | 0 (`addr_faults = 0`, `addr_mismatch = 0`) | — |
| retried | 112 765 formation refusals (pool empty at that instant) | 12 130 index-readiness stalls, 2 625 window-full stalls | retries never duplicate work: a refusal leaves no slot and no request |

Three identities are asserted rather than assumed, and all three hold in every
one of the 202 cells (`dispatch_unaccounted = 0`):

1. `simt_elems + bulk_elems == trace_accesses` (every element in exactly one class);
2. `simt_instr + bulk_instr == trace_instr` (every instruction classified);
3. `bulk_elems_done + bulk_addr_faults == bulk_elems` and `simt_completed ==
   simt_requests_generated` (every element completed exactly once, by the path
   that owns it).

No operation reaches both paths: the compaction is checked element by element at
start-up (`a SIMT-classified element reached the bulk index array` is a hard
failure), and every engine address is re-derived from the index image at
completion (`addr_mismatch = 0` everywhere). No operation reaches neither path:
the sum in (1) is exact and the conservation block fails the run otherwise.

### 1.3 The realized κ and the class mix

κ is a *generator* parameter: the probability that an instruction is regular.
The realized mix therefore varies around it by sampling, and is identical for a
given seed regardless of capacity — which is the property Part A asks for:

* **Classification is capacity-independent.** For all 30 (κ, N, seed) triples
  compared across W = 8 192 and W = 32 768, `simt_instr`, `bulk_instr`,
  `simt_elems` and `bulk_elems` are **identical** — the same trace, the same
  metadata, the same partition. The matrix prints the paired rows side by side.
* At κ = .75, N = 65 536, seed 1: 12 277 SIMT instructions carrying **49 108
  elements (74.9 %)** and 4 107 bulk instructions carrying **16 428 elements
  (25.1 %)**. That is the intended reading of κ = .75: roughly three quarters of
  the *logical work* is regular and goes to SIMT. (The instruction count differs
  from 75/25 because a regular instruction covers L=32 contiguous elements of one
  line set while an irregular one covers 32 scattered elements; the element
  share, 74.9 %, is the meaningful figure and it matches κ.)
* Changing W changes neither the classification nor the workload. The SIMT-only
  arm is bit-identical across capacities (κ = .75, seed 1: A = 296 473 at both),
  which is the direct evidence that the trace and the SIMT-class element set did
  not move. The bulk-only and hybrid arms do move (B = 283 215 at 8K vs 209 860
  at 32K) — that is the *engine's capacity* changing, not the partition.

**Conclusion: dispatch is correct and is left untouched.** This milestone did not
modify `build_op_classes`, `build_bulk_workload`, `run_hybrid`, the SIMT
front-end, the engine or the workload generator.

---

## 2. Per-client measurements through the shared hierarchy

### 2.1 Instrumentation

The hierarchy already carried a requester tag end to end — the M11.3 client index
on `Req` → `DQ` (0 = SIMT, 1 = engine index, 2 = page-walker PTE, 3 = engine
target) — but only the DRAM scheduler consumed it. This milestone extends the
same tag to the L1 and L2 (`Config::cache_diag`, off by default):

* per-requester L1 and L2 reads, hits, misses, MSHR joins, allocations,
  evictions and stalls, incremented at the exact points the aggregate counters are
  incremented, including the same "a refused probe is a stall, not a read" rule,
  so `reads == hits + misses` holds per requester;
* **cross-requester attribution**: each L1/L2 entry records the requester that
  last touched it (1 B/line, diagnostic only), so a hit is classified self or
  cross, and an eviction is charged to the fill that caused it and classified by
  who had used the victim;
* per-requester issue/completion counts, first/last issue and completion cycle,
  and summed latency;
* DRAM reads, row hits/conflicts, queue occupancy and cross-requester
  activations per requester (`dram_diag`, unchanged code path).

Nothing in this block is read by a probe, an MSHR allocation, a replacement, an
arbitration or a cycle decision; the fields are written only when the flag is
set. Verified: with `cache_diag` off and on, all three arms produce the same
cycles at both capacities (`296 473/283 215/244 823` and
`296 473/209 860/264 541`), and the three historical M10 anchors reproduce
exactly (`220 123/138 924/125 581`, `169 826/127 138/94 035`,
`120 110/116 804/95 194`).

**Cost of the instrumentation, stated honestly:** 1 byte per L1 and L2 line for
the "last requester" tag (16 KiB at the 1 MiB L2, 256 B at the 16 KiB L1) and one
counter block per requester. It is diagnostic state that a production design
would not carry.

### 2.2 The measurement, κ = .75, N = 65 536, seed 1

| requester | L1 reads | L1 hit | L2 reads | L2 hit | L2 hit% | L2 evict | evict-cross | hit-cross | DRAM reads | last done |
|---|---|---|---|---|---|---|---|---|---|---|
| **W = 8 192** | | | | | | | | | | |
| SIMT | 12 277 | 0 | 12 277 | 0 | 0.0 % | 4 610 | 520 | 0 | 12 277 | 127 065 |
| engine index | 1 027 | 0 | 1 027 | 0 | 0.0 % | 0 | 0 | 0 | 1 027 | 17 719 |
| page walker | 0 | 0 | 1 060 | 1 013 | 95.6 % | 10 | 10 | 0 | 37 | — |
| engine target | 16 421 | 0 | 16 421 | **2 198** | **13.4 %** | 14 222 | 6 010 | **2 182** | 14 223 | 244 822 |
| total | 29 725 | 0 | 30 785 | 3 211 | 10.4 % | — | — | — | 27 564 | 244 822 |
| **W = 32 768** | | | | | | | | | | |
| SIMT | 12 277 | 0 | 12 277 | 0 | 0.0 % | 4 610 | 520 | 0 | 12 277 | 127 378 |
| engine index | 1 027 | 0 | 1 027 | 0 | 0.0 % | 0 | 0 | 0 | 1 027 | 17 217 |
| page walker | 0 | 0 | 1 060 | 1 017 | 95.9 % | 10 | 10 | 0 | 37 | — |
| engine target | 16 385 | 0 | 16 385 | **0** | **0.0 %** | 16 384 | **8 192** | **0** | 16 385 | 264 540 |
| total | 29 689 | 0 | 30 749 | 1 017 | 3.3 % | — | — | — | 29 726 | 264 540 |

Five facts follow directly, and each is a counter, not an inference:

1. **SIMT's own cache behaviour is identical** at both capacities: same L1 reads,
   same (zero) hits, same 4 610 L2 evictions, same 520 cross-evictions, same
   12 277 DRAM reads. SIMT streams each line exactly once (κ = 1 instructions are
   contiguous runs sweeping the line set), so a 0 % L2 hit rate is its expected
   behaviour, not a symptom.
2. **The engine's 13.4 % hit rate comes almost entirely from the other client**:
   2 198 hits, of which 2 182 are cross hits — lines SIMT had fetched. (Five-seed
   means: 2 144 hits, 2 140 cross.) The engine itself touches each of its 16 384
   lines essentially once (`lines_issued = 16 421`, `dedup_merged = 8`), so it
   has almost no self-reuse to lose.
3. **At 32K that reuse disappears completely** (2 198 → 0). The engine's evictions
   rise to 16 384 — one full L2 — and half of them (8 192) displace a line
   another requester had used.
4. **DRAM traffic rises by 2 162 reads (+7.8 %)** while **row locality improves**
   (row hits 24 557 → 27 719, conflicts 3 005 → 2 005). The extra traffic is
   purely the lost hits: fewer hits means more misses means more lines fetched.
5. **The SIMT completion time moves by +313 cycles on this seed and +186 on
   average** while the engine's moves by +19 718 / +18 378.

### 2.3 What the model cannot attribute

* **Reuse distance is not instrumented per access.** The model records window
  line sets (M13's `m13_dump`) and per-requester hit rates, but there is no
  per-line re-reference counter. The quantity that plays the role of reuse
  distance here is measured directly instead: the *distinct target lines a window
  requests* (16 384 at 32K, ≈ 7 447 per window at 8K) against the *L2 line count*
  (16 384). That is the arithmetic in §3.2 and it is exact; a histogram of
  per-line reuse distances would be additional evidence, not different evidence.
* **Queue-occupancy attribution is sampled, not continuous.** `dram_diag` gives
  per-requester queue occupancy summed per cycle and its peak, and cross-requester
  activation counts; it does not record which requester's request blocked which
  other one at a given cycle. The FR-FCFS decision is unchanged and unaudited.
* **The walker is a third client.** Its PTE traffic is tagged (client 2) and shows
  a 95.6 % L2 hit rate, i.e. page walks are cheap here and are not a confound.
  It is reported rather than folded into either compute client.

---

## 3. The κ = .75, N = 65 536 anomaly: attribution

### 3.1 Reproduction

Five paired seeds, identical traces, only `batch_window` differing
(mean of 5; per-seed rows in the matrix output):

| W | A (SIMT only) | B (bulk only) | C (dispatched) | C vs 8K | SIMT done | bulk done | DRAM reads | bulk L2 hit% |
|---|---|---|---|---|---|---|---|---|
| 8 192 | 293 899 | 283 121 | **245 041** | — | 127 359 | 245 040 | 27 540 | 13.1 % |
| 32 768 | 293 899 | 209 721 | **263 419** | **+7.50 %** | 127 545 | 263 418 | 29 676 | 0.0 % |

This reproduces the M14 result exactly (+7.50 %), with B improving by 25.9 % in
the same pair. The anomaly is therefore a genuine property of the hybrid, not a
measurement artefact.

### 3.2 The mechanism

A window at 32K holds the descriptor's *entire* bulk set: 16 428 elements over
16 384 distinct lines. The shared L2 is 16 384 lines. So:

* the engine's window line footprint **equals** the L2, and
* the window's target stream is issued in ascending line order, i.e. it is a
  sweep of the whole footprint,

which together mean the window's own issue *evicts the cache*: one eviction per
line — 16 384 on seed 1, 16 315 averaged. Anything the cache held before —
including every line SIMT had fetched for the engine to reuse — is gone by the
time the sweep reaches it. The engine's cross hits — 2 140 averaged, 2 182 on
seed 1 — are exactly what perishes.

At 8K the same footprint is split into three windows of ≈ 7 447 lines each. Two
windows fit inside the 16 384-line L2, so a line SIMT fetched while window *i*
was issuing survives into window *i+1*: the measured 13.4 % hit rate.

The second window at 32K exists (the trace has 16 428 > 16 384 elements), which
is why `sealed = 1` (full) and `bulk_win_full_stall` drops from 2 625 to 259 —
the engine is not short of window space, it is short of cache.

### 3.3 Controlled diagnostics

**(a) L2 capacity** — the decisive experiment. Same workload, same window, only
`l2_lines` changed (labelled diagnostic; the published L2 is 16 384 lines):

| L2 | size | C at W=8192 | C at W=32768 | 32K vs 8K | bulk L2 hit% (32K) |
|---|---|---|---|---|---|
| 4 096 lines | 256 KiB | 260 561 | 264 541 | +1.53 % | 0.0 % |
| 8 192 lines | 512 KiB | 260 381 | 264 541 | +1.60 % | 0.0 % |
| 32 768 lines | 2 MiB | 167 199 | **163 468** | **−2.23 %** | 74.9 % |
| 65 536 lines | 4 MiB | 166 748 | **163 468** | **−1.97 %** | 74.9 % |

The regression is monotone in L2 size and **reverses sign** at 2 MiB, where the
engine's own working set fits and it stops depending on the other client's fills
(74.9 % hit rate). A scheduling or arbitration cause could not behave this way.
Note also that at 2 MiB and 4 MiB the 32K number is *identical* (163 468): once
the window's footprint fits, more cache buys the engine nothing.

**(b) Arbitration order** — `dispatch_order` reversed (SIMT first instead of the
engine first; a documented tie-break, not a policy):

| order | W=8192 | W=32768 | gap | SIMT done | bulk done |
|---|---|---|---|---|---|
| 0 (engine first) | 244 823 | 264 541 | +8.06 % | 127 065 / 127 378 | 244 822 / 264 540 |
| 1 (SIMT first) | 263 638 | 276 615 | +4.92 % | 120 441 / 120 441 | 263 637 / 276 614 |

Reversing the tie-break makes **both** configurations worse (SIMT wins slots
earlier but the bulk stream — which must finish last in both — loses more than
SIMT gains). The gap keeps its sign and its order of magnitude. Arbitration
therefore modulates the absolute level and the SIMT completion time, but it does
not create or remove the anomaly.

**(c) SIMT-in-isolation** is not a separate arm, and does not need to be: the
SIMT-class instruction list, the coalescing, the request order and the addresses
are identical across capacities, and §2.2 shows its cache counters are
bit-identical. The only thing that changed for SIMT is the completion *time*, by
0.15 %, which is the residue of sharing a slot pool and a DRAM queue with a
differently-shaped neighbour.

### 3.4 Ledger

| component | 8K (mean) | 32K (mean) | delta | share of ΔC |
|---|---|---|---|---|
| C cycles | 245 041 | 263 419 | **+18 378** | 100 % |
| Bulk last completion | 245 040 | 263 418 | +18 378 | **100.0 %** |
| SIMT last completion | 127 359 | 127 545 | +186 | 1.0 % (overlapped, not additive) |
| engine L2 hits | 2 144 | 0 | −2 144 | cause (2 140 of them cross hits) |
| engine L2 evictions | 14 180 | 16 315 | +2 135 | cause (cross: 6 052 → 8 192) |
| DRAM reads | 27 540 | 29 676 | +2 136 | consequence |
| DRAM row hits | 24 559 | 27 672 | +3 113 | a *benefit*, insufficient to compensate |
| row conflicts | 2 979 | 2 002 | −977 | a *benefit*, insufficient to compensate |

The regression is one mechanism with two consequences: the engine's sweep evicts
the shared L2, so it loses the cross-client hits it depended on, so it must fetch
those lines from DRAM (+2 136 reads) and finishes 18 378 cycles later. Better
DRAM row locality at 32K (+3 113 row hits) is real but worth far less than the
lost hits, because at this granularity the request stream is latency-bound, not
row-bound.

---

## 4. Workload generality

Configurations: 2 × 8K vs 2 × 32K only (2 × 16K was retired by M14 and is not
re-examined). Paired seeds (three per cell for the controls, five for W1),
identical logical traces within a pair. Families are the existing generator
geometries used by M12/M13 — no new workload was invented:

* **W1** — random permutation of a Λ = 16 384-line set, P = 512 (the published
  geometry), κ = .25/.50/.75, N = 16 384/65 536.
* **STRID** — Λ = 1 024, P = 64, κ = 1: every operation regular, so arm C is
  SIMT-only and the engine is idle. A control, not a win.
* **STRIDmix** — the same geometry with κ = .25, so the engine is actually used on
  a small L2-resident footprint.
* **PSCR** — Λ = 16 384, P = 2 048, κ = 0: every operation indirect, maximum page
  dispersion.
* **DUP** — W1 geometry, κ = .25, ρ_dup = 0.5: half the target-line draws are
  duplicates, so a window can fold them.

| family | N | C 8K | C 32K | 32K vs 8K | speedup 8K | speedup 32K | lines issued 8K/32K | dedup 8K/32K | windows 8K/32K | engine L2 hit% | DRAM reads 8K/32K | page walks 8K/32K |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| PSCR | 65 536 | 2 485 737 | **647 042** | **−73.97 %** | 3.07× | **11.78×** | 65 536/32 768 | 0/32 768 | 8.0/2.0 | 0.0/0.0 | 69 879/37 015 | 16 380/4 161 |
| PSCR | 16 384 | 623 115 | **322 401** | **−48.26 %** | 3.08× | **5.94×** | 16 384/16 384 | 0/0 | 2.0/1.0 | 0.0/0.0 | 17 556/17 540 | 4 093/2 065 |
| DUP | 65 536 | 308 354 | **224 383** | **−27.23 %** | 1.42× | **1.96×** | 38 694/24 492 | 10 442/24 644 | 6.3/2.0 | 32.4/15.7 | 33 354/27 858 | 693/561 |
| STRID | 65 536 | 34 952 | 34 952 | **0.00 %** | 1.000× | 1.000× | 0/0 | 0/0 | — | — | 1 029/1 029 | 64/64 |
| STRID | 16 384 | 22 664 | 22 664 | **0.00 %** | 1.000× | 1.000× | 0/0 | 0/0 | — | — | 1 029/1 029 | 64/64 |
| STRIDmix | 65 536 | 43 021 | 43 057 | +0.08 % | 1.227× | 1.226× | 6 258/2 048 | 42 923/47 133 | 6.3/2.0 | 100.0/100.0 | 4 108/4 108 | 113/113 |
| STRIDmix | 16 384 | 21 383 | 21 359 | −0.11 % | 1.267× | 1.269× | 2 048/1 024 | 10 165/11 189 | 2.0/1.0 | 97.2/94.4 | 1 796/1 796 | 77/77 |

Stalls and overlap in the same cells (mean of 3 seeds for the controls, 5 for
W1; `buffer full` is zero everywhere, so it is omitted from the prose but kept
in the logs):

| workload | N | W | fill wait | window full | index wait | slot stall | SIMT formation refusals | peak residency |
|---|---|---|---|---|---|---|---|---|
| PSCR | 65 536 | 8 192 | **1 834 061** | 4 685 | 23 033 | 0 | 0 | 85 |
| PSCR | 65 536 | 32 768 | **0** | 10 686 | 27 997 | 0 | 0 | 112 |
| DUP | 65 536 | 8 192 | 209 316 | 4 172 | 27 556 | 214 892 | 42 335 | 128 |
| DUP | 65 536 | 32 768 | **0** | 9 002 | 41 612 | 155 063 | 56 016 | 128 |
| W1 κ=.75 | 65 536 | 8 192 | 64 812 | 2 619 | 12 125 | 214 821 | 112 925 | 128 |
| W1 κ=.75 | 65 536 | 32 768 | **0** | 258 | 13 926 | 239 636 | 113 049 | 128 |
| STRIDmix | 65 536 | 8 192 | 8 884 | 12 268 | 13 730 | 18 723 | 20 530 | 128 |
| STRIDmix | 65 536 | 32 768 | **0** | 9 984 | 22 230 | 880 | 22 640 | 128 |
| STRID | 65 536 | 8 192 | 0 | 0 | 0 | 0 | 18 563 | 128 |

The `fill wait` column is the sharpest single number in the table: it counts
cycles in which the generator had elements and **no window was accepting them**.
At 8K, PSCR spends **1 834 061 cycles** in that state — the window is the
throughput limiter, and 32K removes it completely. That is why PSCR gains 74 %
instead of the ~8 % a pure-cache argument would predict. In W1 κ=.75 the same
column falls from 64 812 to 0, and the run nevertheless gets *slower*: there the
engine was never window-starved, so the extra capacity buys nothing and the lost
cache reuse (also visible in `slot stall`, 214 821 → 239 636, i.e. more backlog
waiting on residency) is what remains.

### 4.1 Which characteristic predicts the outcome

Three measurable conditions, in order of predictive power:

1. **Window line footprint vs shared L2 capacity.**
   `lines_issued_per_window ≈ W`. When `W ≲ L2_lines/2`, consecutive windows
   still find each other (and the other client's fills) resident: 32K is safe.
   When `W ≳ L2_lines`, a window evicts the cache it depends on: 32K costs ~8 %.
   This ratio explains the sign of the W1 κ=.75 result (+7.5 %), and it explains
   the L2 sweep in §3.3(a) exactly: the sign flips only once the cache is large
   enough that a 32K window stops displacing what it needs. It is *not* the whole
   story where the engine was window-limited rather than cache-limited at 8K —
   PSCR and STRIDmix are decided by the conditions below, and in those cells the
   ratio's prediction and the measurement disagree in sign. The matrix does not
   contain W1 at κ = .50/.25, so this section makes no claim about those points.
2. **Duplicate concentration.** DUP wins because a 32K window can fold 24 644
   duplicates against 10 442 at 8K, cutting issued target lines by 37 %
   (38 694 → 24 492). The engine's L2 hit rate *falls* (32.4 % → 15.7 %) and it
   still wins by 27 %: fewer requests beat better reused requests. Dedup, not
   cache, is the operative mechanism here.
3. **Share of the workload the engine owns.** STRID (engine idle) is exactly
   neutral; STRIDmix (engine owns 25 % of a 1 024-line footprint that is fully
   L2-resident) is neutral to ±0.1 %; PSCR (engine owns everything) gains most.
   The absolute gain scales with the fraction of work in the engine.

A fourth, secondary effect: **page-walk pressure**. PSCR at 32K performs 4 161
walks against 16 380 (N = 65 536) — the larger window keeps more pages in one
open run, so the cohort scheduler has fewer distinct pages to acquire per window.
That is worth a large share of PSCR's −74 %, and it is a *translation* benefit,
distinct from dedup and cache.

### 4.2 What does not generalize

The M14 claim "2 × 16K is dominated" was made on W1 geometry and is not revisited
here. The claim "32K is better" is **not** a property of the engine: it is a
property of the (window, L2, workload) triple. For W1 at κ = .75 with a 1 MiB L2
it is worse, and that is the only configuration in this matrix where 32K loses by
more than a rounding error (STRIDmix at N = 65 536 is +0.08 %, i.e. noise) — a
configuration the earlier milestones never tested with per-requester attribution.

**No compiler dispatch rule is derived from W1 alone.** The workload family is
chosen by the compiler, but the *capacity* must be chosen against the cache
budget, which is a system property available at build time: `W ≲ L2_lines/2` is
the rule this data supports, and it is a capacity rule, not a dispatch rule.

---

## 5. Configuration recommendations

1. **Keep compiler-directed dispatch exactly as implemented.** It is correct,
   exhaustive, capacity-invariant and inert. No change is warranted.
2. **Keep the published 1 MiB L2 and 2 × 8K window as the default.** With this
   cache, 8K is the only capacity that wins at κ = .75 and it loses nothing that
   matters elsewhere: the 32K advantage only materialises on workloads where the
   engine owns the whole stream (PSCR) or where dedup is rich (DUP), and those
   can select 32K explicitly.
3. **Treat 32K as a workload-directed choice with a stated precondition**, not a
   generic upgrade: it requires `32 768 ≲ L2_lines / 2`, i.e. a ≥ 2 MiB shared
   L2, *or* a workload whose duplicate/page structure pays for the lost hits. The
   diagnostic shows that with 2 MiB the κ = .75 anomaly reverses to −2.23 %; the
   cheapest fix for the anomaly is therefore cache, not engine.
4. **Do not add an engine-side reuse buffer (M13) to repair this.** The lost hits
   are cross-client hits on lines the *other* client fetched; an engine-private
   buffer would have to duplicate the L2 to recover them, and M13 already showed
   the reuse is not there within the engine's own stream.
5. **Keep `dispatch_order = 0`** (engine first). SIMT-first lowers the SIMT
   completion time by 6.6 K cycles but lengthens both runs by 4–8 %.
6. **Do not tune the window against W1 alone.** PSCR and DUP move in the opposite
   direction from W1 κ=.75, by much larger factors. Any capacity decision must be
   validated on at least those three families.

---

## 6. Limitations and unresolved questions

* **Reuse distance is not instrumented per access** (§2.3). The footprint/L2 ratio
  is measured exactly and explains all four capacity results, but a per-line
  reuse-distance histogram would strengthen it.
* **The 2×16K point was not re-run**, by instruction. A result here could justify
  revisiting it (e.g. 16K at κ=.75 with a 1 MiB L2 should sit between 8K and 32K
  and may be the better default at that κ); it was out of scope.
* **`dispatch_order = 1` interacts with the pool partition** and its +8 % cost was
  not decomposed further. It is a diagnostic, not a proposal.
* **L2 sizes other than 16 384 lines are diagnostic.** The 2 MiB and 4 MiB runs
  are labelled as such everywhere and are not presented as a configuration
  recommendation — only as evidence about the mechanism.
* **The 1-byte "last requester" tag per cache line is instrumentation**, not
  design. A production counter set would need either that tag or a sampled
  attribution scheme; the model cannot say which is cheaper.
* **No silicon claim is made.** Every cycle count is from the calibrated model;
  the DRAM/row figures are model behaviour under the configured address map.
* **Row-locality benefit at 32K is real but unpriced.** The model charges a fixed
  `t_hit`/`t_miss` per line, so it cannot say how much of the +3 113 row hits a
  real controller would convert into time. That is the one place where the
  measured +7.5 % could be optimistic against 32K.

---

## 7. Reproduction

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests      # 4470 checks, 0 failures
bash build/wm/m15_matrix.sh && bash build/wm/m15_matrix2.sh   # 202 cells
python3 build/wm/m15_extract.py                        # the tables above
```

Single cell (the anomaly), instrumentation off and on:

```bash
./bin/dx100sim --config configs/ab_controlled.cfg --set rho_dup=0 --set elem_bytes=4 \
  --set bulk_wl=E0 --set page_sched=1 --set idx_mode=2 --set experiment=m9diag \
  --set pool_slots=128 --set dram_diag=1 --set cache_diag=1 --set kappa=0.75 \
  --set N=65536 --set seed=1 --set batch_window=32768 \
  --set Lambda=16384 --set P=512 --set bulk_stream=0
```

Every cell in the matrix reports `M9 GATES PASSED`; the log's last line states
the cell and failure counts. Historical anchors are unchanged and are re-checked
by the matrix's κ = .25/.50/.75, N = 16 384, W = 8 192, seed 1 rows.
