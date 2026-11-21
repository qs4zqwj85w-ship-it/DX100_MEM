# M11.3 — Additional diagnostic experiments (Part 7) and combined interpretation (Part 8)

Companion to `m11-2-dram-scheduler-observability-report.md` (instrumentation, FR-FCFS
behaviour, FIFO control, Vortex fidelity). This document adds the five diagnostic
experiments and the combined interpretation. Nothing in the production path changed:
FR-FCFS is still the default, every published configuration is untouched, and the new
switches default to values that reproduce the published binaries' behaviour bit-for-bit.

Evidence labels: **[S]** source fact · **[M]** measured (log archived under
`sim/build/wm/m113_*.txt`, command in the report) · **[D]** derived arithmetic ·
**[H]** hypothesis not yet tested.

---

## 0. Controls, new instrumentation, and validation

### 0.1 What was added for Part 7

| Addition | File | Gate / default |
| --- | --- | --- |
| Requester split: 0 SIMT, 1 engine **index**, 2 PTE, 3 engine **target** (was 0/1/2 with one engine tag) | `bulk.cpp` ×3, `sim.hpp` | data only, never read by a decision |
| Per-requester queue occupancy (`client_q_sum/q_max`), activations (`client_conf`), cross-requester interference (`client_interf`) | `sim.hpp`, `memory.cpp`, `system.cpp`, `ab.cpp` | `dram_diag`, default **off** |
| Diagnostic address maps `dram_map = 0..5` | `sim.hpp`, `memory.cpp`, `util.cpp` | default **0 = production**, which skips the switch entirely |
| `bank_last_client_` (owner of the last access per bank) | `sim.hpp` | diagnostic only, written inside the `dram_diag` block |

`dram_map` modes **[S]** (`memory.cpp`, applied to every requester in the run):

| mode | mapping | purpose |
| --- | --- | --- |
| 0 | production (`bank = hash(line) % banks`, `row = (line/banks)/16 % rows`) | control |
| 1 | address bit 23 (8 MiB) moves the line to the other bank and the upper row half | Experiment C |
| 2 | `bank = 0` | Experiment D |
| 3 | `bank = line % banks` | Experiment D (intended "balanced") |
| 4 | `row = 0` | Experiment D (ideal row locality) |
| 5 | `bank = 0, row = 0` | Experiment D (maximum concentration) |

### 0.2 Configuration record

Every run below uses the published M9 harness and differs only in the listed keys:

```
--config configs/ab_controlled.cfg --set N=16384 --set rho_dup=0 --set elem_bytes=4 \
  --set bulk_wl=E0 --set batch_window=8192 --set bulk_max_elems=262144 \
  --set max_cycles=200000000 --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
  --set page_sched=1 --set idx_mode=2 --set experiment=m9diag --set pool_slots=128 \
  --set dram_diag=1
W1: --set Lambda=16384 --set P=512 --set kappa=0.25 --set bulk_stream=0
W0: --set Lambda=16384 --set P=512 --set kappa=0.25 --set bulk_stream=1
```

| experiment | additional keys | runs |
| --- | --- | --- |
| A | `l1_mshr=16|32|64`; `l2_mshr=128`; `l1_mshr=32 l2_mshr=128`; `l1_mshr=64 l2_mshr=256`; probe `pool_slots=256` | 7 |
| B | none (arms A/B/C come from one `m9diag` run) | reuses W1 and W0 |
| C | `dram_map=1` on W1 and W0 | 2 |
| D | `dram_map=0,2,3,4,5` | 5 |
| E | `seed=1..5` × `frfcfs=1,0` on W1 and W0 | 20 |

### 0.3 Validation

| Check | Result |
| --- | --- |
| `make -j4` | clean, no warnings |
| `./bin/dx100_tests` | **4032 checks, 0 failures** |
| Published anchors | W1 κ=0.25 C = **125 581**, W0 κ=0.25 C = **18 142**, E0 control = **153 475**, W1 A = **220 123** — all unchanged **[M]** |
| Instrumentation inert | W1 with `dram_diag=0` vs `1`: 125 581 both, completion hashes `2645928636 / 1417164640` both **[M]** |
| `dram_map=0` is the production map | same run, same hashes as before the switch existed **[M]** |
| Request conservation | every one of the **38** archived runs reports `M9 GATES PASSED` (element conservation, address match, `target requests generated == issued`) **[M]** |

---

## 10. Experiment A — DRAM visibility sensitivity

**Question.** Does the 16-entry L1 MSHR limit how much the DRAM scheduler can see?

W1 κ=0.25, seed 1, FR-FCFS, arm C (M10 hybrid). `dram_q = 0` in every run, i.e. the DRAM
queue is **unbounded** **[S]** — so any depth limit observed here comes from admission
upstream, never from queue capacity.

| configuration | C cycles | q mean | q peak | cand/dec | eligible/dec | bank-blocked/dec | hit % | case B | case C | acts | act/row | wait mean | svc: activations | L2 hit % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `l1_mshr=16` (published) | **125 581** | 10.66 | **16** | 5.79 | 0.36 | 5.43 | 83.8 | 1 611 | 595 | 2 206 | 2.10 | 98.5 | 57 356 | 10.40 |
| `l1_mshr=32` | **122 083** (−2.79 %) | 19.30 | **31** | 7.13 | 0.43 | 6.70 | 84.3 | 1 638 | 489 | 2 127 | 2.03 | 173.4 | 55 302 | 10.29 |
| `l1_mshr=64` | **121 891** (−2.94 %) | 32.29 | **62** | 7.30 | 0.44 | 6.86 | 84.4 | 1 592 | 528 | 2 120 | 2.02 | 289.6 | 55 120 | 10.18 |
| `l2_mshr=128` only | 125 581 (0.00 %) | 10.66 | 16 | 5.79 | 0.36 | 5.43 | 83.8 | 1 611 | 595 | 2 206 | 2.10 | 98.5 | 57 356 | 10.40 |
| `l1=32 + l2=128` | 122 083 (−2.79 %) | 19.30 | 31 | 7.13 | 0.43 | 6.70 | 84.3 | 1 638 | 489 | 2 127 | 2.03 | 173.4 | 55 302 | 10.29 |
| `l1=64 + l2=256` | 121 891 (−2.94 %) | 32.29 | 63 | 7.30 | 0.44 | 6.86 | 84.4 | 1 599 | 521 | 2 120 | 2.02 | 289.7 | 55 120 | 10.21 |
| probe: `pool_slots=256`, l1=16 | 124 177 (−1.12 %) | 11.23 | 16 | 5.79 | 0.36 | 5.43 | 83.7 | 1 648 | 571 | 2 219 | 2.11 | 102.6 | 57 694 | 10.60 |

Starved / blocked cycles (arm C) **[M]**: `l1=16` 11 665 / 100 328 → `l1=32` 8 096 / 100 400 →
`l1=64` 6 235 / 102 069; `pool=256` 7 426 / 103 162.

**Findings**

1. **The 16-entry L1 MSHR was the visibility limiter, and it was binding.** Raising it to 32
   or 64 raises the *observed* DRAM queue peak from 16 to 31 and 62 — the queue tracks the
   MSHR budget, not any queue limit (there is none: `dram_q = 0`). Candidate visibility per
   decision rises 5.79 → 7.30, eligible candidates 0.36 → 0.44, and the run shortens by
   2.8–2.9 % **[M]**.
2. **The L2 MSHR is slack.** Raising `l2_mshr` 64 → 128 alone changes *nothing at all* —
   every column of that row is identical to the baseline, down to the last cycle count
   **[M]**. So the "64 × 2 = 128 residency budget" reasoning in the config comment does not
   describe what binds at run time; the per-core L1 array does.
3. **The residency pool is not binding either.** A labelled probe with `pool_slots = 256`
   leaves the queue peak at 16 and buys only 1.1 % **[M]**. (This probe changes an M10
   architecture parameter and is *not* a proposal — it exists to rule the pool out.)
4. **More visibility lengthens each request's wait while shortening the run.** Mean queue
   wait 98.5 → 289.6 cycles as depth goes 10.7 → 32.3 **[M]**. Nothing is wasted: starved
   cycles fall from 11 665 to 6 235 and the surplus is turned into throughput. But the
   marginal return collapses after 32 (2.79 % → 2.94 % for double the array).
5. **Visibility does not make the scheduler smarter.** Hit rate moves only 83.8 → 84.4 % and
   activations 2 206 → 2 120 **[M]**. The gain comes from keeping the banks busier, not from
   better row selection — consistent with M11.2's finding that the scheduler already takes
   every eligible hit it sees.

**Caveat.** `l1_mshr = 16` is a published M10 parameter. This experiment measures what the
parameter costs; changing it is an architecture decision outside this milestone, and no
default was changed.

---

## 11. Experiment B — requester isolation

Arms A (SIMT only), B (bulk only) and C (hybrid) come from one `m9diag` run each, so the
trace, the class map, the seed and the DRAM parameters are identical across the three.

### 11.1 W1 κ=0.25, seed 1, FR-FCFS

| arm | cycles | DRAM reads | row hits | hit % | note |
| --- | --- | --- | --- | --- | --- |
| A SIMT only | 220 123 | 12 783 | 508 | **4.0** | whole trace on the SIMT path |
| B bulk only | 138 924 | 13 771 | 11 611 | **84.3** | same trace, engine only |
| C hybrid | 125 581 | 13 589 | 11 383 | **83.8** | 4 008 SIMT + 12 376 bulk elements |

Per requester in the **hybrid** arm C **[M]**:

| requester | reads | hits | hit % | activations | of them cross-requester | mean queue | peak queue |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SIMT | 1 002 | 913 | 91.1 | 89 | 61 | 0.98 | 15 |
| engine index | 774 | 675 | 87.2 | 99 | 61 | 0.62 | 14 |
| PTE fetch | 35 | 0 | 0.0 | 35 | 31 | 0.04 | 2 |
| engine target | 11 778 | 9 795 | 83.2 | 1 983 | 31 | 9.03 | 15 |

Per requester in the **bulk-only** arm B **[M]**: index 1 024 / 953 (93.1 %), target
12 712 / 10 658 (83.8 %), PTE 35 / 0.

### 11.2 W0 κ=0.25 (streamed), seed 1

A = 11 052, B = 17 267, C = 18 142. In the hybrid arm the engine issued **770 index lines and
1 target line** — it spent the run starved on index fetch and its target stream never reached
the DRAM queue **[M]**. W0's hybrid arm is therefore 64 % *slower* than its own SIMT baseline
(18 142 vs 11 052); it is a pathological configuration, not a contention sample.

### 11.3 Findings

1. **Hybrid creates contention, not scheduling diversity.** The engine's target hit rate is
   83.8 % alone and 83.2 % in the hybrid — a 0.6-point loss **[M]**. Whatever diversity the
   two requesters could in principle provide is not visible in the row-hit statistics.
2. **Cross-requester interference is measurable and small.** 184 of 2 206 activations (8.3 %)
   in the hybrid interrupt a bank whose last access came from a different requester (SIMT 61,
   index 61, PTE 31, target 31) **[M]**. At 12 bank-cycles each shared over two banks that is
   ≈ 1 104 cycles, ≈ 0.9 % of the run **[D]**. The walker is the most "disruptive" per
   request — 31 of its 35 activations are cross-requester — because its lines alias onto data
   rows (see §12).
3. **The value of hybrid execution is parallelism, not memory scheduling.** C (125 581) is far
   faster than A (220 123) and B (138 924), but that is because the work is split, not because
   the two streams schedule better together. A DRAM-side organizer therefore has no
   hybrid-specific prize to collect.
4. **The SIMT row-hit rate is not comparable across arms.** SIMT scores 4.0 % alone (12 783
   lines, the whole irregular trace) and 91.1 % in the hybrid (1 002 lines — the
   kernel-metadata slice of the class map). The addresses differ, so this is a class-map
   artifact, not evidence that hybrid execution helps SIMT **[D]**. The equivalent-logical-
   workload requirement is met for bulk-vs-hybrid (same 12 376 targets) and not for
   SIMT-alone-vs-hybrid, which the class map forbids.

---

## 12. Experiment C — address-mapping sensitivity (8 MiB periodicity)

The M11.1 observation **[S]**: `row = (line/banks)/16 % rows` with 4 096 rows of 1 KiB wraps
every **8 MiB**, and `bulk_index_base = 0x00800000` is exactly 8 MiB, so index line *i* and
data line *i* map to the same (bank, row). `dram_map = 1` moves address bit 23 (8 MiB) into
both the bank and the upper half of the row space, which removes that correspondence without
touching anything below 8 MiB.

| workload | map | C cycles | target hit % | index hit % | index↔target cross acts | bank 0 / bank 1 reads | activations | distinct rows |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| W1 | 0 | **125 581** | 83.2 | 87.2 | 61 + 31 | 6 807 / 6 782 | 2 206 | 1 050 |
| W1 | 1 | **124 819** (−0.61 %) | 83.2 | 87.5 | 61 + 31 | 6 806 / 6 783 | 2 198 | 1 076 |
| W0 | 0 | **18 142** | — | 87.4 | 59 + 1 | 901 / 891 | 192 | 90 |
| W0 | 1 | **18 211** (+0.38 %) | — | 87.4 | 61 + 1 | 901 / 891 | 193 | 116 |

**Findings**

1. **The correspondence is real but immaterial.** Removing it moves the run by −0.61 % on W1
   and **+0.38 %** on W0 — inconsistent in sign and smaller than seed noise (§14: ±0.9 %
   standard deviation on W1) **[M]**. There is no measured penalty to recover.
2. **Index↔target interference is already tiny.** 61 index activations follow a target access
   and 31 target activations follow an index access, out of 99 and 1 983 respectively
   **[M]** — and the counts are *unchanged* by the decoupling map. The two streams barely meet
   at the same bank at the same time because they occupy different phases of the run (index
   fetch 0 → 12 866, target issue 4 115 →) **[M]**.
3. **Index-index locality is unaffected** (87.2 → 87.5 %) — the index stream's own row reuse
   comes from its own linear sweep, not from where it aliases onto data **[M]**.
4. **The walker is the visible victim.** All 31 of its activations in W1's hybrid are
   cross-requester **[M]**: its PTE lines alias onto data rows, so each PTE fetch tends to
   re-activate a row the engine just used. It is 0.25 % of traffic; the effect stays
   sub-percent.

**Conclusion.** The 8 MiB aliasing is a genuine modelling artifact (source fact) with no
measurable performance consequence in these workloads (measurement). It stays documented; it
is not worth a production mapping change.

---

## 13. Experiment D — bank-distribution stress

W1 κ=0.25 geometry, seed 1, `dram_map` alone varied. Logical accesses, seeds and every other
parameter are identical; only the physical bank/row placement changes, and the same map
applies to SIMT, both engine streams and the walker.

| map | placement | C cycles | hit % | bank0 / bank1 reads | activations | distinct rows | act/row | cand/dec | bank-blocked/dec | case B | case C | wait | svc: acts |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | production (balanced banks, distributed rows) | **125 581** | 83.8 | 6 807 / 6 782 | 2 206 | 1 050 | 2.10 | 5.79 | 5.43 | 1 611 | 595 | 98.5 | 57 356 |
| 2 | one bank, distributed rows | **216 158** (+72.1 %) | 84.3 | 13 589 / 0 | 2 135 | 1 025 | 2.08 | 3.71 | 3.49 | **0** | 2 135 | 232.5 | 55 510 |
| 3 | round-robin banks, production rows | **210 202** (+67.4 %) | 84.3 | 13 188 / 403 | 2 134 | 1 050 | 2.03 | 3.44 | 3.23 | 33 | 2 101 | 225.0 | 55 484 |
| 4 | balanced banks, one row each | **117 172** (−6.71 %) | **100.0** | 6 806 / 6 783 | **2** | **2** | 1.00 | 11.43 | 10.55 | 1 | 1 | 86.1 | 52 |
| 5 | one bank, one row | **190 616** (+51.8 %) | 100.0 | 13 589 / 0 | 1 | 1 | 1.00 | 14.57 | 13.50 | 0 | 1 | 203.9 | 26 |

**Findings**

1. **Bank count is the dominant structural parameter.** Forcing all traffic onto one bank costs
   **+72.1 %**; the single bank is busy 215 866 of 216 158 cycles (99.9 %), i.e. the run becomes
   pure command serialisation **[M]/[D]**. Production runs at 86.3 % bank occupancy
   (216 718 of 2 × 125 581). Any scheduling conclusion in this model is therefore a statement
   *inside* a two-bank world, and the two-bank model is doing much more work than the
   scheduler is.
2. **Row locality is worth at most 6.7 %, measured.** Map 4 makes every access a row hit
   (100.0 %, 2 activations instead of 2 206) and saves **8 409 cycles = 6.71 %** **[M]**. This
   is the empirical ceiling for *any* row-ordering intervention on this workload — a perfect,
   clairvoyant organizer cannot beat it, because it cannot remove an activation that map 4
   leaves in place. Compared with M11.2's 5.5 % window-boundary bound, this is the tighter and
   more defensible number, and it is measured rather than derived.
3. **Case B is structurally impossible with one bank.** Under map 2 the case-B counter is
   **exactly 0** while case C absorbs all 2 135 activations, and blocked cycles jump to
   202 194 **[M]**. With a single bank, a cycle in which the wanted row is busy is a cycle in
   which *nothing* is eligible, so no decision is taken and case B cannot arise. This is a
   structural validation of the M11.2 case split and a reminder that case B — 73 % of W1's
   activations in production — is a *two-bank* phenomenon.
4. **"Balanced" is not the goal; locality is.** Map 3 was built as the balanced-bank
   alternative and turned out near-identical to the single-bank case (+67.4 %). The reason is
   arithmetic, not scheduling: the workload's target lines are all **even** (M11.2 §4), so
   `bank = line % 2` sends every target to bank 0 and only the index (403 reads) to bank 1
   **[D]**. The construct tests concentration twice, not balance once. Production (map 0) is
   already balanced to within 1 % (6 807 / 6 782) and is the balanced reference.
5. **Concentration raises wait, not hit rate.** Hit % barely moves between maps 0 and 2
   (83.8 → 84.3), while mean wait goes 98.5 → 232.5 **[M]**. The stress changes where the time
   goes, not how well the scheduler picks rows.
6. **Two banks mask the scheduler's cheapest win.** Under map 2 the controller still takes
   every eligible hit (case A = 0), but it can no longer substitute across banks, so the only
   lever left is row reuse. A two-bank model flatters a row-hit policy relative to a
   single-bank one; both are reported so the reader can see the difference.

---

## 14. Experiment E — multi-seed stability

Five seeds (1–5) per cell, each cell a complete separate run; no averaging across geometries.
Seeds are paired (same seed, both policies), so both the marginal statistics and the paired
difference are reported.

**Absolute cycles, W1 κ=0.25** (mean ± stdev, min–max over 5 seeds):

| arm | FR-FCFS | FIFO |
| --- | --- | --- |
| A SIMT only | 220 019 ± 1 710 (218 419–222 752) | 222 597 ± 1 707 (220 961–225 220) |
| B bulk only | 138 380 ± 619 (137 536–138 924) | 138 507 ± 633 (137 659–139 094) |
| C hybrid | 124 406 ± 1 134 (123 125–125 581) | 125 118 ± 1 017 (123 954–126 328) |

**Absolute cycles, W0 κ=0.25:**

| arm | FR-FCFS | FIFO |
| --- | --- | --- |
| A SIMT only | 11 119 ± 239 (10 853–11 500) | 11 119 ± 239 (10 853–11 500) |
| B bulk only | 17 281 ± 425 (16 810–17 926) | 17 383 ± 310 (17 097–17 886) |
| C hybrid | 18 203 ± 127 (18 078–18 404) | 18 941 ± 135 (18 810–19 152) |

**Paired policy difference (FIFO − FR-FCFS), same seed:**

| cell | mean Δ | stdev | range | mean % | sign |
| --- | --- | --- | --- | --- | --- |
| W1 A | +2 578 | 131 | +2 447 … +2 718 | **+1.17 %** | all 5 positive |
| W1 B | +127 | 103 | +31 … +298 | +0.09 % | all 5 positive |
| W1 C | +712 | 231 | +309 … +882 | **+0.57 %** | all 5 positive |
| W0 A | +0.2 | 0.4 | 0 … +1 | 0.00 % | — (pure SIMT is policy-blind) |
| W0 B | +102 | 133 | **−40** … +287 | +0.61 % | 4 of 5 positive |
| W0 C | +738 | **15** | +714 … +748 | **+4.05 %** | all 5 positive |

**Findings**

1. **The geometric spread is ~2 % on W1 and ~6 % on W0**, which is why single-seed percentages
   from M11.2 need their paired form to be trusted. The paired deltas are far tighter
   (stdev 15–231 cycles) than the absolute spread (1 134 on W1 C) **[M]**.
2. **FR-FCFS ≥ FIFO in every cell, and the W1 result is seed-robust**: 5 of 5 seeds agree, +0.57 %
   on the hybrid arm, +1.17 % on the SIMT-only arm **[M]**. W0's hybrid arm is the one large,
   extremely stable effect (+4.05 %, stdev 15 cycles).
3. **The bulk-only ordering is nearly a coin flip.** W0 B has one seed where FIFO wins by 40
   cycles **[M]**. Taken with W1 B (+0.09 %), the conclusion is that row-hit priority matters
   little when the engine issues a single ordered stream; it matters when two requesters
   interleave (arm C) or when the SIMT path dominates (arm A).
4. **No published anchor moved**: seed 1 reproduces 220 123 / 138 924 / 125 581 (W1) and
   11 052 / 17 267 / 18 142 (W0) exactly **[M]**.

---

## 15. Combined interpretation (Part 8)

All percentages are against W1 κ=0.25 arm C = **125 581 cycles**, seed 1, and are measured
unless marked otherwise. They are not additive: several of them remove the same idle cycles.

| # | Limitation | Measured effect | Evidence |
| --- | --- | --- | --- |
| 1 | **Bank timing / bank count** (structural) | one bank: **+72.1 %**; production bank occupancy 86.3 % | §13 |
| 2 | **Address-mapping artifact → row-locality ceiling** | all-hits ideal map: **−6.71 %** | §13 |
| 3 | **DRAM visibility (L1 MSHR = 16)** | `l1_mshr=32`: **−2.79 %**, `=64`: **−2.94 %**; queue peak 16 → 31 → 62 | §10 |
| 4 | **Scheduler selection (FR-FCFS vs FIFO)** | **0.57 %** (W1 C), 1.17 % (W1 A), 4.05 % (W0 C); case A = 0 always | §14, M11.2 |
| 5 | **SIMT/Bulk contention** | 8.3 % of activations are cross-requester ≈ **0.9 %**; bulk hit rate −0.6 pt | §11 |
| 6 | **Translation / admission** | not separable in these runs; no evidence it gates the hybrid arm, whose banks stay 86.3 % busy | §10–§11 |

### 15.1 Ranking

1. **Bank parallelism is the strongest measured effect by a wide margin** (+72 % when removed).
   It is a property of the *model*, not a knob anyone would change, but it explains why the
   scheduling levers look small: with two banks the controller can hide most of its own
   mistakes, and with one bank no scheduler choice can help.
2. **The address-mapping / row-locality pool is 6.7 %**, and it is a *ceiling*, not an
   opportunity: it is what perfect row knowledge would be worth. Everything a Bulk-side
   organizer could do — Row Table, sorter, row-aware issuing — draws from this same 6.7 %.
3. **Visibility is a real 2.8–2.9 %**, and it is upstream of the DRAM: it comes from the
   per-core L1 MSHR array, not from queue capacity (`dram_q` is unbounded and the L2 MSHR is
   slack — both verified by controls that changed nothing).
4. **Scheduler selection is worth 0.6 % on the flagship workload**, and FR-FCFS already
   collects all of it (case A = 0 in every measured cell).
5. **Contention is sub-percent.** Hybrid execution costs about 0.9 % in cross-requester
   activations and buys its speed from parallelism, not from memory-level diversity.
6. **Translation/admission** does not show up as an independent limiter once the L1 MSHR is
   accounted for; a dedicated experiment (changing dTLB structure) would be needed to bound it,
   and that changes an unrelated mechanism, so it is documented as a limitation instead.

### 15.2 Is a new Bulk-side DRAM organizer justified?

**No — and the case is now bounded from both sides.**

* **Upper bound (measured, not estimated):** 6.71 %. An implementable organizer cannot remove
  activations that a perfect all-hits mapping leaves in place, and it must operate on the same
  window contents.
* **Lower bound (measured):** the within-window ordering available to such an organizer is
  already ≈ 1.07 activations per row against a floor of 1.00 (M11.2 §4.1) — about 0.35 % of
  cycles.
* **The scheduler is not leaving hits on the table:** case A = 0 in every workload, every arm
  and every mapping, including the stress maps.
* **The remaining time is bank-busy time**, not visible-but-unserved requests: 86.3 % bank
  occupancy, 73 % of activations case B (hits present, banks busy).

The two levers that measured larger than an organizer are *not* DRAM-side: bank parallelism
(model structure) and L1 MSHR capacity (an M10 architecture parameter, worth 2.9 %). If the
project wants another percent or two, that is where the evidence points — not at a new
organizer, and not at a mapping change, which measured at ±0.6 % with inconsistent sign.

**Suggested next step, if any:** a bounded, reversible experiment on `l1_mshr = 32` across the
full published workload set (it is a one-key configuration change, no new architecture), with
the M9 anchors and gates as the acceptance test. That is a decision for the architecture owner,
not part of this milestone; nothing was changed in the defaults here.

### 15.3 What this milestone did *not* do

No Row Table, physical-address sorter, or new bulk scheduling structure was implemented. The
only additions are diagnostic: per-requester counters behind `dram_diag` (default off) and a
`dram_map` switch whose default reproduces the production mapping exactly. FR-FCFS remains the
default policy, every published configuration is unchanged, and every published anchor
(125 581 / 18 142 / 153 475 / 220 123) reproduces bit-for-bit.

### 15.4 Limitations of the evidence

1. One footprint (N = 16 384) and one geometry family; Experiment E quantifies seed noise but
   not geometry dependence.
2. The two-bank DRAM model cannot represent bank-group, channel or refresh effects (§13.1).
   Conclusions about "bank timing" are about *this* two-bank timing.
3. `dram_map` changes the physical placement of a fixed logical trace. It is a controlled
   mapping experiment, not a microarchitectural proposal, and map 4's 100 % hit rate is by
   construction unreachable in a real cache/DRAM pair.
4. The SIMT-alone arm uses a different address slice than the hybrid's SIMT share (§11.3), so
   SIMT hit rates are not comparable across arms.
5. The `pool_slots = 256` probe changes an M10 budget parameter; it is included only to rule
   the residency pool out as the visibility limiter and is not a recommendation.
