# M23-F1 — Ideal Translation and Translation-Admission Sensitivity

**Status.** Experimental study. Historical M18–M23 traces, configurations, logs and
reports are untouched. `configs/m23_{m0,dx8k,dx16k,dx32k}.cfg` and the fused
`win_fused` implementation are unchanged. The only source change is one
flag-gated diagnostic path for ideal translation (§2), which reproduces the
historical Criteo M0 log **byte for byte** when the flag is off (§2.3).

**Labels.** **[V]** verified by measurement, **[I]** inferred from counters plus
source, **[H]** hypothesis needing a further test, **[L]** simulator limitation.

---

## 0. Answer first

| question | answer |
|---|---|
| How much of DX-100's advantage is translation? | **All of it.** With translation made free, DX is slower than M0 on every workload: ideal speedup **0.894 / 0.949 / 0.985 / 0.982** (Criteo / Orkut / ogbn train / ogbn uniform) against normal **2.323 / 2.936 / 1.267 / 1.224**. |
| Does DX keep an advantage without translation? | **No** — it loses 1.6 %–11.9 % of cycles, and reads 6.6 %–25 % more DRAM lines while doing so. |
| Is the advantage an artifact of `dtlb_mshr=4`? | **No, but its size is.** Raising admission shrinks DX's lead monotonically (Criteo 2.32 → 1.95, Orkut 2.94 → 1.47, ogbn 1.27 → 1.22) without ever reaching 1. The advantage survives the 4→32 MSHR sweep; it does not survive zero walk cost. |
| Effective walker concurrency | `min(walkers, dtlb_mshr, l2tlb_mshr = 8)`. Measured `walker_max` = 2 / 4 / 8 exactly at those limits; 16 and 32 walkers never exceed `walker_max = 8`. |
| ogbn M0 at 12 in flight | Translation admission, not `R`. As `dtlb_mshr` grows 4→8→16→32 the same run reaches **12 → 18 → 33 → 52** in flight, and M0 gets **3.99×** faster. |
| Newly found | DX deadlocks in **3 of 12** high-concurrency cells from the M23-F0 dTLB fill/eviction defect; higher translation concurrency makes it *more* likely. Labelled rescues are reported, not silent substitutions. |

---

## 1. What was run

All cells use the historical traces unchanged, with the historical workload
arguments:

| id | trace | extra workload args |
|---|---|---|
| `b` | `build/wm/m23_criteo_16k.bin` | `bulk_max_elems=1048576` |
| `c` | `build/wm/m23_orkut_bfs.bin` | `bulk_max_elems=2097152` |
| `d` | `build/wm/m23_ogbn.bin` (train sampler) | `bulk_max_elems=2097152` |
| `u` | `build/wm/m23_ogbn_uniform.bin` | `bulk_max_elems=2097152` |

Common to every cell: `--set bulk_index_base=0x80000000 --set
bulk_desc_line=0x1FFFFC0 --set dram_diag=1`. Architectures are the frozen
**M0** (`configs/m23_m0.cfg`) and **DX 2×16K** (`configs/m23_dx16k.cfg`).

Runner: `build/wm/m23f1_run.sh {p1|p2|p2c}` — logs and timings in
`build/wm/m23f1/`, one file per cell, nothing overwritten.

---

## 2. Ideal translation: implementation

### 2.1 The limitation and the minimal fix

The model has no way to express "correct translation, zero cost": every access
must pass `MemorySystem::tlb_probe`, which allocates a dTLB MSHR, queues an
L2-TLB probe and (on a miss) a walk whose completion is `cycle + levels × 150`.
Turning `walk_latency_per_level` down does **not** remove the admission limit or
the PTE traffic, so it is not ideal translation.

The smallest change that removes translation *and nothing else* bypasses the
three translation steps for a request while keeping the VA→PA function exactly
as `tlb_fill` installs it (`ppn = vpn + PPN_OFFSET`):

| file | function | change |
|---|---|---|
| `sim/src/sim.hpp` | `struct Config` | new field `bool ideal_translation = false;` (default off) |
| `sim/src/util.cpp` | `Config::parse` | parse `ideal_translation` |
| `sim/src/util.cpp` | `Config::to_string` | echo ` ideal_translation` **only when set**, so the off-case log text is unchanged |
| `sim/src/memory.cpp` | `MemorySystem::tlb_probe` | if set: count the probe as a hit (`st.reads++, st.hits++`), `ready = cycle`, return true — no MSHR, no L2 TLB, no walker |
| `sim/src/memory.cpp` | `MemorySystem::tick` (stage 1) | if set: `ppn = r.vpn + PPN_OFFSET` instead of the `tlb_e_` lookup |

Everything else — caches, MSHR files, DRAM and its scheduler, address maps, `R`,
the Bulk engine, fused storage, compiler-directed selection — is untouched. Both
architectures go through the identical path, and the mapping is the same one the
real TLB installs, so **data addresses and data-access counts are unchanged**.

Configurations: `sim/configs/m23f1_m0_ideal.cfg` and
`sim/configs/m23f1_dx16k_ideal.cfg`, each a byte copy of the frozen file plus the
single line `ideal_translation = 1` (verifiable with `diff`).

### 2.2 Reproduce

```bash
cd sim
W="--set dataset=../build/wm/m23_criteo_16k.bin --set bulk_index_base=0x80000000 \
   --set bulk_desc_line=0x1FFFFC0 --set bulk_max_elems=1048576 --set dram_diag=1"
./bin/dx100sim --config configs/m23_m0.cfg            $W --all > ../build/wm/m23f1/b_m0_normal.txt
./bin/dx100sim --config configs/m23f1_m0_ideal.cfg    $W --all > ../build/wm/m23f1/b_m0_ideal.txt
./bin/dx100sim --config configs/m23_dx16k.cfg         $W --all > ../build/wm/m23f1/b_dx16k_normal.txt
./bin/dx100sim --config configs/m23f1_dx16k_ideal.cfg $W --all > ../build/wm/m23f1/b_dx16k_ideal.txt
```

### 2.3 Verification that ideal mode is ideal — and inert when off

* **Off-case is byte-identical.** `b_m0_normal` with the flag absent diffs clean
  against the historical `build/wm/m23b_m0.txt` (28 511 774 cycles).
* **Translation is genuinely gone** in every ideal cell: `tlb0_hits == tlb0_reads`
  (100 %), `walks = 0`, `pte_fetches = 0`, `walker_busy = 0`, `l2tlb_reads = 0`.
* **The data stream is preserved:** `accesses` is identical (638 976 / 1 200 003)
  and every DX element count is identical between normal and ideal
  (`bulk_elems` 294 912 / 1 199 876 / 1 199 005; `bulk_instr` 73 728 / 301 148 /
  302 827; `simt_instr` 36 864 / 43 / 335).
* **[L] One caveat.** The number of *line requests* the DX engine forms moves by
  ≤ 0.08 % between normal and ideal (Criteo 224 877 → 224 899; Orkut 898 877 →
  898 698; ogbn 1 262 297 → 1 262 139) because the engine's dedup/merge depends on
  which elements are resident in the window at a given cycle. The *accesses* and
  *elements* are identical; only window-residency-dependent merging shifts. M0's
  request count is identical to the request (360 326 / 1 200 003 / 1 200 003).

---

## 3. Phase 1 — results

### 3.1 Consolidated comparison

| Workload | M0 normal | DX normal | Normal speedup | M0 ideal | DX ideal | Ideal speedup |
|---|---:|---:|---:|---:|---:|---:|
| B Criteo | 28 511 774 | 12 273 324 | **2.323** | 1 010 690 | 1 131 020 | **0.894** |
| C Orkut BFS | 49 067 545 | 16 714 538 | **2.936** | 5 766 215 | 6 077 523 | **0.949** |
| D ogbn train | 176 184 939 | 139 088 324 | **1.267** | 15 323 803 | 15 562 234 | **0.985** |
| D ogbn uniform | 176 800 539 | 144 443 603 | **1.224** | 15 283 690 | 15 571 490 | **0.982** |

How much each architecture gains from removing translation:

| workload | M0 cycles removed | DX cycles removed | DX net vs M0 *with* free translation |
|---|---:|---:|---:|
| Criteo | 96.5 % | 90.8 % | **+11.9 % slower** |
| Orkut | 88.2 % | 63.6 % | **+5.4 % slower** |
| ogbn train | 91.3 % | 88.8 % | **+1.6 % slower** |
| ogbn uniform | 91.4 % | 89.2 % | **+1.9 % slower** |

The normal-translation ratios reproduce M23-B/C/D exactly (2.323 / 2.936 / 1.267),
which is the control that the traces and configurations are the historical ones.

### 3.2 Supporting counters

| cell | cycles | L1 rd / hit % | L2 rd / hit % | DRAM rd | row hits | activations | infl_max | replays |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| b M0 normal | 28 511 774 | 360 326 / 34.9 | 614 590 / 85.9 | 86 886 | 5 190 | 81 696 | 13 | 0 |
| b M0 ideal | 1 010 690 | 360 326 / 34.2 | 232 973 / 64.6 | 82 380 | 10 870 | 71 510 | 128 | 60 482 736 |
| b DX normal | 12 273 324 | 224 877 / 36.0 | 304 688 / 65.9 | 103 617 | 32 120 | 71 497 | 119 | 50 023 |
| b DX ideal | 1 131 020 | 224 899 / 35.6 | 141 589 / 28.5 | 101 257 | 32 829 | 68 428 | 128 | 60 452 517 |
| c M0 normal | 49 067 545 | 1 200 003 / 2.1 | 1 799 004 / 71.1 | 519 337 | 154 848 | 364 489 | 128 | 21 719 386 |
| c M0 ideal | 5 766 215 | 1 200 003 / 2.1 | 1 173 483 / 56.2 | 513 650 | 157 554 | 356 096 | 128 | 362 273 765 |
| c DX normal | 16 714 538 | 898 877 / 0.0 | 1 112 436 / 41.4 | 651 309 | 393 609 | 257 700 | 122 | 51 242 313 |
| c DX ideal | 6 077 523 | 898 698 / 0.0 | 898 633 / 28.4 | 643 661 | 389 516 | 254 145 | 128 | 331 610 938 |
| d M0 normal | 176 184 939 | 1 200 003 / 0.0 | 3 548 791 / 66.3 | 1 194 355 | 1 661 | 1 192 694 | 12 | 0 |
| d M0 ideal | 15 323 803 | 1 200 003 / 0.0 | 1 199 462 / 2.1 | 1 173 937 | 9 199 | 1 164 738 | 128 | 975 683 147 |
| d DX normal | 139 088 324 | 1 262 297 / 0.0 | 3 116 566 / 54.5 | 1 369 035 | 145 089 | 1 223 946 | 73 | 30 351 |
| d DX ideal | 15 562 234 | 1 262 139 / 0.0 | 1 262 105 / 0.8 | 1 251 952 | 143 429 | 1 108 523 | 128 | 861 877 889 |
| u M0 normal | 176 800 539 | 1 200 001 / 0.0 | 3 556 909 / 66.4 | 1 193 961 | 1 291 | 1 192 670 | 11 | 0 |
| u M0 ideal | 15 283 690 | 1 200 001 / 0.0 | 1 199 345 / 2.2 | 1 172 661 | 9 054 | 1 163 607 | 128 | 973 102 800 |
| u DX normal | 144 443 603 | 1 257 894 / 0.0 | 3 183 636 / 55.8 | 1 357 779 | 126 664 | 1 231 115 | 74 | 30 772 |
| u DX ideal | 15 571 490 | 1 258 024 / 0.0 | 1 257 959 / 1.0 | 1 245 575 | 125 400 | 1 120 175 | 128 | 868 650 093 |

### 3.3 Interpretation

**[V] 1. The advantage is translation, and only translation.** Under identical
ideal treatment DX is *slower* on all four workloads, by 1.6 % (ogbn) to 11.9 %
(Criteo). There is no workload in this set where DX retains a non-translation
advantage.

**[V] 2. PTE traffic is a visible fraction of the memory system, not just a
latency.** In the normal runs the page-walker's fetches are 2 × walks of the L2
reads (Criteo M0: 380 152 of 614 590 = 62 % of L2 reads; Orkut M0: 624 762 of
1 799 004 = 35 %). Removing them drops the L2 hit rate (85.9 → 64.6 on Criteo)
because the PTE lines that were being re-hit are simply gone.

**[V] 3. Removing PTE traffic improves DRAM row behaviour.** Criteo M0 keeps the
same useful reads (86 886 → 82 380, −5 %) but row hits rise 5 190 → 10 870 while
activations fall 81 696 → 71 510. The PTE stream was interrupting the data row
stream. This is a real cost of real translation that the fixed-delay model hides
because it does not make walks wait on it.

**[V] 4. DX's structural costs become visible as soon as translation stops
hiding them.** Its cycle deficit tracks a DRAM-read excess: Criteo 101 257 vs
82 380 (+23 %), Orkut 643 661 vs 513 650 (+25 %), ogbn 1 251 952 vs 1 173 937
(+7 %). The Bulk engine converts duplicate-heavy gathers into fewer *requests*,
but the index stream is read as its own line stream, which the SIMT path does not
pay for. The extra traffic did not matter while every request cost ~150 cycles of
walk throughput; it decides the comparison when requests are free.

**[I] 5. The engine's own organization, not the caches, is the residual cost.**
On Criteo the DX ideal run has `bulk_org_cycles = 147 456` and
`bulk_fanout_cycles = 140 455` against 1.13 M total, and `bulk_slot_stall` rises
from 448 478 (normal) to 1 003 976 (ideal) as the engine becomes the pacing
client. Data-cache behaviour is not the difference: L1 hit rates are 34–36 % for
both arms.

---

## 4. Phase 2 — translation-admission sensitivity

Normal translation model throughout. Two independent knobs, swept together so
their effects can be separated:

* `walkers` — how many walks may execute at once.
* `dtlb_mshr` — how many *distinct pages* may be in translation at once
  (× `dtlb_parked = 2` requests each).

### 4.1 Effective concurrency established first

| point | walkers | dtlb_mshr | `walker_max` observed | average active walkers | walker util |
|---|---:|---:|---:|---:|---:|
| baseline | 2 | 4 | 2 | 2.00 | 100.0 % |
| walkers only | 16 | 4 | **4** | 3.91 | 97.8 % |
| equal | 8 | 8 | **8** | 7.82 | 97.7 % |
| equal | 16 | 16 | **8** | 7.96 | 99.5 % |
| equal | 32 | 32 | **8** | 7.97 | 99.6 % |

**[V]** Effective concurrency is `min(walkers, dtlb_mshr, l2tlb_mshr = 8)`. The
first point shows the two interventions are not interchangeable: **16 walkers
with 4 MSHRs reach only 4**, because the dTLB MSHR file admits at most four
distinct pages; **8 MSHRs with 8 walkers reach 8**; and beyond that the
**8-entry L2 TLB MSHR file** is the ceiling — 16 and 32 walkers both stop at 8.
The chosen set is therefore the smallest informative one: baseline, walkers-only,
and three diagonal points; 16/16 and 32/32 differ only in MSHR depth.

### 4.2 Cycles and speedup

| workload | point (w, mshr) | M0 cycles | DX cycles | DX speedup | M0 vs its own baseline |
|---|---|---:|---:|---:|---:|
| Criteo | 2, 4 (baseline) | 28 511 774 | 12 273 324 | 2.323 | 1.000 |
| Criteo | 16, 4 | 14 581 317 | 6 322 108 | 2.306 | 1.955 |
| Criteo | 8, 8 | 7 260 692 | *deadlock → 3 349 158* | *2.168* | 3.927 |
| Criteo | 16, 16 | 6 968 595 | 3 351 797 | 2.079 | 4.091 |
| Criteo | 32, 32 | 6 535 921 | 3 348 225 | **1.952** | 4.363 |
| Orkut | 2, 4 | 49 067 545 | 16 714 538 | 2.936 | 1.000 |
| Orkut | 16, 4 | 28 194 915 | 13 139 921 | 2.146 | 1.740 |
| Orkut | 8, 8 | 17 904 629 | 11 825 082 | 1.514 | 2.741 |
| Orkut | 16, 16 | 17 564 465 | 11 801 884 | 1.488 | 2.794 |
| Orkut | 32, 32 | 17 310 374 | 11 799 084 | **1.467** | 2.835 |
| ogbn train | 2, 4 | 176 184 939 | 139 088 324 | 1.267 | 1.000 |
| ogbn train | 16, 4 | 89 284 823 | 71 004 595 | 1.257 | 1.973 |
| ogbn train | 8, 8 | 44 637 463 | 36 648 573 | 1.218 | 3.947 |
| ogbn train | 16, 16 | 44 316 889 | *deadlock → 36 338 923* | *1.220* | 3.975 |
| ogbn train | 32, 32 | 44 162 620 | *deadlock* | — | 3.989 |

*Italic DX cells hit the 200 M cycle guard; the rescue re-run is the same cell
with `dtlb_ways = 8` (§4.4).*

**[V]** M0 gains 1.96–4.36× from translation capacity alone — more than DX gains
(1.42–3.83×), which is why DX's advantage decays monotonically and on no point
reaches 1.0.

### 4.3 Counters behind the effect

Criteo, M0 — the translation subsystem, step by step:

| walkers, mshr | cycles | walks | avg active walkers | dTLB MSHR occ / capacity | `tlb_refuse_full` | `unissued_max` | `inflight_max` | L1 / L2 MSHR max | DRAM busy | DRAM rd | row hits | activations |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2, 4 | 28 511 774 | 190 076 | 2.00 | 3.99 / 4 | 110 739 514 | 126 | 13 | 8 / 7 | 2 196 756 | 86 886 | 5 190 | 81 696 |
| 16, 4 | 14 581 317 | 190 106 | 3.91 | 3.98 / 4 | 56 465 305 | 124 | 12 | 8 / 11 | 2 196 964 | 86 894 | 5 190 | 81 704 |
| 8, 8 | 7 260 692 | 189 253 | 7.82 | 7.95 / 8 | 27 253 723 | 124 | 21 | 13 / 21 | 2 197 000 | 86 882 | 5 161 | 81 721 |
| 16, 16 | 6 968 595 | 184 967 | 7.96 | 15.94 / 16 | 25 119 771 | 124 | 32 | 15 / 20 | 2 195 936 | 86 866 | 5 215 | 81 651 |
| 32, 32 | 6 535 921 | 173 660 | 7.97 | 31.80 / 32 | 21 584 966 | 123 | 50 | 15 / 18 | 2 188 638 | 86 619 | 5 288 | 81 331 |

Criteo, DX 2×16K:

| walkers, mshr | cycles | walks | avg active walkers | MSHR occ / cap | `tlb_refuse_full` | `inflight_max` | L1 / L2 MSHR max | DRAM busy | DRAM rd |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2, 4 | 12 273 324 | 81 493 | 1.99 | 3.96 / 4 | 24 345 088 | 119 | 67 / 64 | 2 308 602 | 103 617 |
| 16, 4 | 6 322 108 | 81 460 | 3.87 | 3.92 / 4 | 11 411 849 | 128 | 68 / 64 | 2 304 296 | 103 588 |
| 8, 8 (rescue) | 3 349 158 | 81 921 | 7.34 | 7.42 / 8 | 4 775 917 | 128 | 68 / 64 | 2 321 566 | 104 129 |
| 16, 16 | 3 351 797 | 82 500 | 7.39 | 13.35 / 16 | 3 505 283 | 128 | 68 / 64 | 2 322 120 | 104 166 |
| 32, 32 | 3 348 225 | 82 503 | 7.39 | 22.22 / 32 | 2 343 771 | 128 | 72 / 64 | 2 323 540 | 104 192 |

**[V] 4.3.1 The dTLB MSHR file is saturated at every setting.** Occupancy tracks
capacity (3.99/4, 7.95/8, 15.94/16, 31.80/32). Adding MSHRs never relieves it —
it only moves the ceiling and halves the refusals. That is why M0 keeps improving
all the way to 32 even though walkers stop at 8.

**[V] 4.3.2 No new bottleneck appears below 8 walkers, and DRAM never binds.**
DRAM busy time is flat at 2.19–2.32 M cycles across a 4.4× cycle range, row hits
and activations are flat, and L1/L2 MSHR occupancy stays at 8–21 of 128/64. The
only subsystem that changes is translation: `inflight_max` on M0 rises 13 → 12 →
21 → 32 → 50 with the MSHR file. Since `2 × dtlb_mshr` bounds the admitted
requests, 32 and 52 exceed it slightly (stage-2 requests), as expected.

**[V] 4.3.3 At ≥8 walkers the two arms stop being walk-bound at different
points.** Cycles ÷ (`walks × 300 / walker_max`):

| cell | ratio | reading |
|---|---:|---|
| b M0 baseline | 1.00 | entirely walk-throughput bound |
| b M0 32, 32 | 1.00 | still walk-bound even at 8 concurrent walks |
| b DX 32, 32 | 1.08 | still ~walk-bound |
| d M0 32, 32 | 1.00 | still walk-bound |
| d DX 8, 8 | 1.05 | still ~walk-bound |
| c M0 8, 8 | 1.53 | walk-bound only in part |
| c DX 8, 8 | **2.94** | **not walk-bound** — something else paces it |

So on Criteo and ogbn, even eight concurrent walkers are not enough to get
translation out of the critical path; on Orkut, DX stops being walk-bound at 8
while M0 does not, and DX's remaining 11.8 M cycles are paced by the engine, not
by translation (`bulk_org_cycles` 599 938, `bulk_fanout_cycles` 764 654,
`bulk_slot_stall` 674 450).

### 4.4 The M23-F0 defect interacts with this sweep

**Three of ten DX cells deadlocked** at the 200 M guard with the F0 signature
(`issued != completed`, `walker_max` reached, one or a few requests stranded):
`b_dx16k_m8w8`, `d_dx16k_m16w16`, `d_dx16k_m32w32`.

This is **not** a new bug and it is **not repaired** here (the brief excludes it).
It is the same dTLB fill/eviction race M23-F0 diagnosed: more admitted translations
means more dTLB fills per tick, which raises the chance that a fill evicts a VPN
installed earlier in the same tick while its requester still waits. The sweep
therefore *increases the trigger rate*: no deadlock in any of the 16 Phase-1
cells (8 normal-translation, 8 ideal), 3 in the 12 high-concurrency DX cells.

To keep the trend measurable I ran two **labelled rescues** (same cell, same
traces, `dtlb_ways = 4 → 8`, i.e. 2 sets × 8 ways instead of 4 × 4, which reduces
same-set collisions):

| cell | as specified | rescue (`dtlb_ways=8`) | gates |
|---|---:|---:|---|
| Criteo DX 8,8 | 200 000 000 (guard) | **3 349 158** | OK |
| ogbn DX 16,16 | 200 000 000 (guard) | **36 338 923** | OK |

They are named `RESCUE_*` in the log directory, are never merged into the sweep
counters, and are reported only as the cycle value that cell would have had.

---

## 5. Phase 3 — baseline fairness

### 5.1 Why ogbn M0 reaches only 12 in flight [V]

`R = 128` bounds slot *residency*, not memory-system occupancy. At the baseline,
124–126 of the 128 slots are held by requests that `tlb_probe` **refused**
(`tlb_refuse_full = 703 491 625`), and only 4 dTLB MSHR entries × 2 parked
waiters can be inside translation. Measured `inflight_max = 12` = 8 in translation
plus ~4 in the cache stage, with `l1_mshr_max = 7` of 128. The global budget is
32× larger than the translation admission limit, so the MSHR file decides.

### 5.2 How it moves when admission improves [V]

| `dtlb_mshr` | waiters (×2) | ogbn M0 `inflight_max` | ogbn M0 cycles | M0 gain |
|---:|---:|---:|---:|---:|
| 4 | 8 | **12** | 176 184 939 | 1.000 |
| 8 | 16 | **18** | 44 637 463 | 3.947 |
| 16 | 32 | **33** | 44 316 889 | 3.975 |
| 32 | 64 | **52** | 44 162 620 | 3.989 |

In-flight concurrency rises monotonically and almost exactly with the MSHR file.
The 12 is therefore a property of the *configured translation admission*, not of
the workload.

### 5.3 Does DX still win after M0 gains concurrency? [V]

Yes, at every configuration tested: 1.257 (16,4), 1.218 (8,8), 1.220 (rescue
16,16), and 1.952 on Criteo at (32,32). But that is not a request-organization
advantage — §4.3.3 shows M0 is *still walk-throughput bound* at (32,32) (ratio
1.00), i.e. M0 is still paying 190 076 walks × 300 / 8 = 7.1 M cycles of
translation. The comparison has not yet reached the regime where translation is
free, and in that regime (§3) DX loses.

### 5.4 Remaining unfairness in M0 [I]

* **The MSHR file is under-provisioned relative to the machine's own budget.**
  4 entries × 2 parked = 8 requests against `R = 128`: 6 % of the outstanding
  capacity. Nothing in the Vortex calibration justifies 4 entries, and the
  baseline's headline speedup is set almost entirely by this number.
* **2 walkers < 4 MSHRs.** The baseline cannot even use the admission capacity it
  declares: `walkers = 2` halves the achievable walk concurrency (measured
  `walker_max` 2 at baseline, 4 with 16 walkers and the same MSHRs).
* **It is not a `mem_path` or cache artifact.** L1/L2 MSHR occupancy is far below
  capacity in every cell, so the baseline is not being throttled by the data path.
* What is **not** unfair: both arms share the same dTLB, the same 300-cycle walk
  and the same walkers. DX wins on Criteo/ogbn because it *needs* 2.3–3.2× fewer
  translations, which is a property of its request organization — but that
  organization only pays while translation is the bottleneck.

Per the brief, no M0 resource outside the defined sweep was raised.

---

## 6. Phase 4 — classification of findings

### 6.1 Verified experimental observations [V]

1. Ideal translation removes translation completely and identically from both
   arms: `walks = 0`, `pte_fetches = 0`, `tlb0_hits = tlb0_reads`, `walker_busy = 0`.
2. Ideal speedup is **0.894 / 0.949 / 0.985 / 0.982**; normal speedup is
   **2.323 / 2.936 / 1.267 / 1.224** (Criteo / Orkut / ogbn train / ogbn uniform).
3. DX is slower than M0 under ideal translation on all four workloads (+1.6 % to
   +11.9 % cycles).
4. Effective walker concurrency is `min(walkers, dtlb_mshr, l2tlb_mshr = 8)`;
   `walker_max` = 2, 4, 8 and never more.
5. M0 reaches 190 076 walks at 100 % walker utilisation on Criteo; cycles ÷
   (`walks × 300 / walker_max`) = 1.00, so the baseline is walk-throughput bound.
6. M0's `inflight_max` rises 12 → 18 → 33 → 52 as `dtlb_mshr` goes 4 → 32, and
   its cycles fall 176.2 M → 44.2 M.
7. DRAM busy time, row hits and activations are flat across the sweep; L1/L2 MSHR
   occupancy never approaches capacity.
8. Three DX cells deadlock from the M23-F0 defect at higher translation
   concurrency; the two rescues complete.

### 6.2 Mechanistic explanations supported by counters [I]

1. **DX's advantage is a translation-throughput advantage.** Its request
   reduction (2.3–3.2× fewer walks) only converts to cycles while each walk costs
   a fixed 300 cycles at a low concurrency; remove the walk and the advantage
   disappears, because the engine's own organization and its extra DRAM reads
   (6.6–25 %) remain.
2. **PTE traffic costs more than its latency.** It occupies 35–62 % of L2 reads and
   degrades DRAM row behaviour (Criteo M0 row hits 5 190 → 10 870 without it).
3. **The dTLB MSHR file, not the walker count and not `R`, is the admission
   limit** at every setting tested: occupancy sits at 98–99.6 % of capacity.
4. **Beyond 8 admitted pages the L2 TLB MSHR file becomes the ceiling**, which is
   why 16/16 and 32/32 give the same `walker_max = 8`.

### 6.3 Hypotheses needing further tests [H]

1. That M0 with *unbounded* translation capacity would converge to its ideal
   1.01 M cycles on Criteo; the sweep only reaches 8 concurrent walks, where M0 is
   still 6.5× above ideal. A walk-latency-free run with admission intact would
   separate "walk throughput" from "walk latency".
2. That the DX deficit under ideal translation is dominated by index-stream DRAM
   reads rather than by engine bookkeeping; the counters are consistent with it
   but a no-index-read ablation was not run.
3. That the Orkut DX plateau at 11.8 M is engine-paced. `bulk_slot_stall` and
   fanout suggest it; no engine-parameter sweep was performed.

### 6.4 Limitations of the simulator [L]

1. **Walk latency is a constant** (`levels × 150`), and PTE fetch completion does
   not gate it. "Ideal translation" therefore removes a delay that was never
   coupled to memory service; a real PTE-miss walk would cost more than 300
   cycles under load.
2. **The engine's request count is timing-dependent** (≤ 0.08 % between normal and
   ideal), because dedup depends on window residency.
3. **The M23-F0 defect is live** and biases any high-concurrency DX cell toward a
   run that cannot finish; three cells had to be rescued.
4. **One core, one socket, 4 lanes, no bandwidth model.** Nothing here is a claim
   about physical GPU performance; all numbers are simulator cycles.

---

## 7. Recommendation for the next translation-model phase

1. **Report the M23-B/C/D advantages as translation-throughput advantages.** The
   honest statement is: "DX-100 removes 2.3–3.2× of the page walks, and on this
   baseline that is the whole of its speedup; with free translation it is 1.6–11.9 %
   slower." The existing reports are not wrong, but they read as if the advantage
   were architectural.
2. **Re-baseline M0's translation admission before further comparison.** The
   4-entry MSHR / 2-walker pair costs the baseline 4× its own runtime and is not
   derived from anything. Either justify it from Vortex or adopt the deepest point
   of this sweep as the reference and restate M18–M23 deltas against it.
3. **Fix the F0 eviction defect before any concurrency study** — it now blocks 30 %
   of the DX cells at exactly the settings a translation study wants.
4. **Then** replace the fixed walk delay with a PTE-arrival completion, because
   that is the only change that makes the remaining questions (PWC, large TLB,
   huge pages) answerable. Under the current model a page-walk cache can only
   remove whole walks, never shorten a walk's service time — which understates
   its value by construction.

---

## Appendix — artifacts

| path | contents |
|---|---|
| `sim/configs/m23f1_m0_ideal.cfg`, `m23f1_dx16k_ideal.cfg` | frozen config + `ideal_translation = 1` |
| `sim/src/sim.hpp` (`Config`), `sim/src/util.cpp` (parse/echo), `sim/src/memory.cpp` (`tlb_probe`, `tick`) | the flag-gated ideal path |
| `build/wm/m23f1/{b,c,d,u}_{m0,dx16k}_{normal,ideal}.txt` | Phase 1, 16 cells |
| `build/wm/m23f1/{b,d}_{m0,dx16k}_m{4w16,8w8,16w16,32w32}.txt` | Phase 2, 16 cells |
| `build/wm/m23f1/c_{m0,dx16k}_m{4w16,8w8,16w16,32w32}.txt` | Orkut cross-check, 8 cells |
| `build/wm/m23f1/RESCUE_*.txt` | the two labelled rescues (§4.4) |
| `build/wm/m23f1/*.time` | wall-clock per cell |
| `build/wm/m23f1/summary.csv` | every parsed counter, all cells |
| `build/wm/m23f1_run.sh` | runner for every cell |
| `tools/m23f1_extract.py` | log parser / table generator |

No historical log, trace, configuration or report was modified.
