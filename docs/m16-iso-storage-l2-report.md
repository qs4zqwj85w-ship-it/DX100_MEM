# M16 Part C — iso-storage L2 comparison

**Question.** If the accelerator's storage budget were invested in a conventional
larger L2 instead, would it buy the same performance?

**Short answer.** For the two irregular workloads, **yes — and more**:

| workload | SIMT, 1 MiB L2 | hybrid, 1 MiB L2 + 2 × 8K window | SIMT, iso-storage L2 |
|---|---|---|---|
| W1 κ=.75 | 294 127 | **244 977** | **241 810** (1.875 MiB) / 236 822 (2 MiB) |
| DUP | 439 025 | **308 354** | **270 620** (1.875 MiB) / 262 192 (2 MiB) |
| PSCR | 7 619 639 | **2 485 737** | 7 619 639 (no change at any capacity) |
| STRID | 34 952 | 34 952 | 34 952 (no change) |
| STRIDmix | 52 800 | **43 021** | 52 800 (no change) |

At matched storage the enlarged-L2 SIMT baseline beats the hybrid by 1.3 % on W1
and 13.9 % on DUP, and loses by 3.07× on PSCR. Capacity cannot substitute for the
window on a page-sparse workload, and the window cannot beat capacity on a
footprint that merely needs to fit. The two mechanisms are not interchangeable —
they win on different traces, which is the finding Part B's ablation predicted.

---

## 1. Storage accounting (measured, not estimated)

| item | size | source |
|---|---|---|
| L2 at 16 384 lines | 1 048 576 B (1 MiB) of data | `configs/ab_controlled.cfg`; 8-way, 64 B lines |
| Bulk window, M6 layout, 2 × 8K | **933 856 B** | `bulk_win_vec_bytes`, measured in every cell of this matrix |
| Bulk window, fused layout, 2 × 8K | **245 744 B** | M15 `fused-window-representation-report.md` §3 |
| distance from 1 MiB to each tested L2 | 20 480 → 1.25 MiB (+256 KiB); 30 720 → 1.875 MiB (+896 KiB); 32 768 → 2 MiB (+1 MiB); 40 960 → 2.5 MiB (+1.5 MiB) | derived |

**The budget-matched points are 30 720 lines for the M6 window (+896 KiB, i.e.
917 504 B against the window's 933 856 B — 16 KiB short) and 20 480 for the fused
window (+256 KiB against 245 744 B — 16 KiB over).** The line granularity of the
L2 cannot hit either budget exactly, so both brackets are run, and both are
reported separately from the 2 MiB point so
that the "2 MiB diagnostic" of M15 is reproduced without being conflated with an
exact match. The comparison is an *opportunity-cost* statement: a 1 MiB L2 plus
0.89 MiB of window SRAM versus a 1.875 MiB L2. It is not a claim that the two are
physically equivalent — an L2 line is 64 B of data plus tags, while the window is
a register-like array with a different access pattern and latency.

## 2. What is compared

Three arms per cell, one materialized trace:

1. **A — conventional SIMT with the original 1 MiB L2** (the published baseline).
2. **A — conventional SIMT with an enlarged L2**: 1.25, 1.875, 2 and 2.5 MiB.
   This is the same SIMT arm, so "enlarged L2" is the only variable.
3. **C — hybrid (SIMT + Bulk) with the original 1 MiB L2**, window 2 × 8K.

Arm C is *not* re-run at the enlarged capacities as a comparison arm; the cells do
contain those runs (they come free in the same invocation) and they are reported
in `build/wm/m16_tables.md` as context, but they are not the iso-storage claim.

`κ`, `N`, seed and window are identical across arms; only `l2_lines` changes, and
it changes for both arms inside a cell, so the A-vs-C ratio at a given capacity is
still paired.

## 3. Results

Means of 3 paired seeds. SDs are printed per cell in `build/wm/m16_tables.md`;
the largest SIMT-arm SDs in this sweep are 2 035 cycles (W1, 1 MiB) and 12 608
cycles (PSCR, all capacities — a seed-level effect on the 7.6 M run, well below
the 3× effects being compared).

### 3.1 W1, κ = .75, N = 65 536 — irregular footprint, 16 384 lines

| L2 | SIMT-only | SD | hybrid | SD | SIMT speedup vs 1 MiB | A/C at this L2 | L2 hit % (A) | DRAM reads (A) |
|---|---|---|---|---|---|---|---|---|
| 1 MiB (published) | 294 127 | 2 035 | 244 977 | 389 | 1.000 | 1.2006 | 26.2 | 21 698 |
| 1.25 MiB | 276 904 | 1 802 | 229 756 | 507 | 1.062 | 1.2052 | 31.7 | 20 081 |
| **1.875 MiB (iso)** | **241 810** | 1 133 | 172 756 | 654 | **1.216** | 1.3997 | 42.6 | 16 869 |
| 2 MiB (M15 diagnostic) | 236 822 | 831 | 167 091 | 360 | 1.242 | 1.4173 | 44.2 | 16 397 |
| 2.5 MiB | 236 742 | 778 | 166 717 | 370 | 1.242 | 1.4200 | 44.2 | 16 392 |

W1's footprint is 16 384 lines = exactly the 1 MiB L2, so the 1 MiB point is the
worst case for both arms and 1.25 MiB already helps. **At matched storage the
enlarged L2 (241 810) is 1.3 % faster than the hybrid (244 977)**, and the
acceleration is visible in the counters: SIMT's own L2 hit rate doubles
(26.2 % → 42.6 %) and its DRAM reads fall 22 %.

### 3.2 DUP — duplicate concentration

| L2 | SIMT-only | SD | hybrid | SD | SIMT speedup vs 1 MiB | A/C | L2 hit % (A) | DRAM reads (A) |
|---|---|---|---|---|---|---|---|---|
| 1 MiB | 439 025 | 239 | 308 354 | 703 | 1.000 | 1.4238 | 45.0 | 29 375 |
| 1.25 MiB | 375 385 | 484 | 273 404 | 1 007 | 1.170 | 1.3730 | 54.0 | 24 562 |
| **1.875 MiB (iso)** | **270 620** | 1 209 | 209 097 | 232 | **1.622** | 1.2942 | 69.0 | 16 588 |
| 2 MiB | 262 192 | 1 207 | 201 626 | 567 | 1.674 | 1.3004 | 70.5 | 15 807 |
| 2.5 MiB | 262 155 | 1 223 | 196 716 | 421 | 1.675 | 1.3327 | 70.5 | 15 804 |

DUP is the most cache-limited family in the study: 1 MiB holds 45 % of its
reuse, and every extra 64 KiB converts into cycles. At matched storage the L2
baseline (270 620) is **1.139× faster than the hybrid (308 354), 12.2 % fewer
cycles** — a much larger margin than W1's, and the opposite of what the dedup
result in Part B might suggest.

### 3.3 PSCR, κ = 0 — page/translation-sensitive

| L2 | SIMT-only | hybrid | A/C | L2 hit % (A) | DRAM reads (A) | page walks |
|---|---|---|---|---|---|---|
| 1 MiB | 7 619 639 | 2 485 737 | **3.0653** | 61.0 | 65 217 | 16 380 |
| 1.25 MiB | 7 619 639 | 2 485 737 | 3.0653 | 61.1 | 64 985 | 16 380 |
| 1.875 MiB | 7 619 639 | 2 485 737 | 3.0653 | 61.7 | 64 043 | 16 380 |
| 2 MiB | 7 619 639 | 2 485 737 | 3.0653 | 61.8 | 63 836 | 16 380 |
| 2.5 MiB | 7 619 639 | 2 485 732 | 3.0653 | 62.5 | 62 690 | 16 380 |

**Every additional byte of L2 buys PSCR exactly zero** (2.5× the capacity moves
the run by 5 cycles in 2.5 M). The SIMT arm's hit rate barely moves (61 % → 62.5 %)
because the misses are compulsory and the cost is translation and issue, not
reuse. Whatever the accelerator is doing on PSCR, it is not cache.

### 3.4 STRID and STRIDmix — controls

STRID is 34 952 cycles in both arms at every capacity (93.8 % L2 hit rate, 1 029
DRAM reads: the footprint is tiny). STRIDmix is 52 800 / 43 021 in A / C at every
capacity; a 2.5× L2 changes neither because its 1 024-line footprint already fits
in the L1. These two families confirm that the sweep's variable is real: where
there is nothing to cache, capacity does nothing, so the W1/DUP effects are not
an artefact of the configuration change itself.

---

## 4. Requester-level view (W1 κ=.75, `cache_diag` on)

The hybrid at 1 MiB and at 2 MiB, split by requester. This is the clearest
evidence for *why* capacity matters, and it is the same instrumentation M15 used.

| L2 | requester | L2 reads | L2 hits | of which cross | evictions | cross-evictions |
|---|---|---|---|---|---|---|
| 1 MiB | SIMT | 12 277 | 0 | 0 | 4 610 | 520 |
| 1 MiB | engine index | 1 027 | 0 | 0 | 0 | 0 |
| 1 MiB | engine target | 16 421 | **2 198** | **2 182** | 14 222 | 6 010 |
| 2 MiB | SIMT | 12 277 | 0 | 0 | **2** | 2 |
| 2 MiB | engine index | 1 027 | 0 | 0 | 0 | 0 |
| 2 MiB | engine target | 16 421 | **12 232** | **12 196** | **610** | 605 |

Two things follow. First, the engine's cross-client hits rise 2 182 → 12 196 and
its evictions collapse 14 222 → 610: the 1 MiB L2 is exactly the size of the
window's footprint, so the window evicts what it needs (M15 §2), and doubling the
L2 removes that. Second, the SIMT client's own hit rate is **zero at both sizes**
in the hybrid: on this trace SIMT streams once and never re-reads, so all of the
capacity benefit that shows up in §3.1's arm A accrues in the *SIMT-only* run,
where the same 16 384 lines are touched four times each. The two arms therefore
benefit from capacity for different reasons — reuse in A, cross-client survival
in C — and that asymmetry is why the iso-storage comparison is close on W1 and
lopsided on DUP.

---

## 5. Interpretation

### 5.1 Does a larger L2 capture the same reuse that dedup captures?

**No, and the numbers are unambiguous.** Dedup captures *intra-window duplicates*
(Part B: worth 0.53 % on DUP, because those lines were already hits), while L2
capacity captures *temporal reuse across windows* (1.62× on DUP at matched
storage). They are different reuse classes with different values, and only one of
them is worth much on this machine.

### 5.2 Does Bulk retain advantages unrelated to cache reuse?

**Yes, on exactly one family.** PSCR is unchanged by 2.5× the L2 and 3.07× faster
in the hybrid; Part B showed that 3.07× comes from page ordering and cohorts
(walks 50 872 → 16 380, row hits 5.7 % → 55.4 %). That advantage is a
*translation-and-locality* advantage, not a capacity advantage, and no amount of
L2 substitutes for it.

### 5.3 Where the opportunity cost lands

| workload | cheaper way to get the same (or better) result | verdict |
|---|---|---|
| W1 κ=.75 | +896 KiB of L2 | **L2 wins by 1.3 %** at equal storage |
| DUP | +896 KiB of L2 | **L2 wins by 1.139× (12.2 % fewer cycles)** at equal storage |
| PSCR | nothing; capacity is irrelevant | **window wins by 3.07×** |
| STRID | nothing needed | tie |
| STRIDmix | nothing needed | window wins 1.227× at any capacity |

Read with Part B: the accelerator's value is concentrated on workloads whose
bottleneck is *organized access* (pages, translation, row locality), not on
workloads whose bottleneck is *capacity*. On the latter, the same silicon spent
on L2 is the better purchase in this model.

---

## 6. Limitations

* **Cache capacity is compared against the window only.** The accelerator also
  spends on the descriptor path, the index ring and the organizer; those are not
  in the 933 856 B and are not compensated here. A full area comparison is the
  audit's job, not this one's.
* **L2 geometry is held at 8 ways.** Growing `l2_lines` at fixed associativity
  changes the set-index mapping as well as the capacity, so part of each step could
  be an associativity effect at the new footprint. The confound is small here
  (the W1 footprint is 16 384 lines with 2 048 sets at 1 MiB and 4 096 sets at
  2 MiB — both ≫ 8 ways) but it is not zero, and a fixed-capacity way sweep would
  separate them.
* **1 MiB → 2.5 MiB is a modelling range, not a silicon proposal.** The published
  L2 is 1 MiB (Vortex VX_config.toml); larger geometries are labelled diagnostic
  everywhere and are not a configuration recommendation.
* **PSCR's invariance is partly structural.** Its misses are compulsory and its
  cost is translation; a trace with genuine cross-window reuse *and* page sparsity
  would be the interesting hybrid case and is not in the matrix.
* **Both arms read the same DRAM with the same `t_hit`/`t_miss`.** A larger L2
  would in reality change the DRAM access pattern (fewer, more clustered reads),
  which this model charges identically per line.

---

## 7. Reproduction

```bash
cd DX100_MEM/sim && bash build/wm/m16_matrix.sh   # section C is the L2 sweep
python3 build/wm/m16_extract.py                   # table §C
```

The iso-storage W1 point at 1.875 MiB is the §8 command of
`m16-simt-mlp-sensitivity-report.md` with `--set l2_lines=30720` and without
`--set l1_mshr`; the 1 MiB point omits `--set l2_lines` entirely.
