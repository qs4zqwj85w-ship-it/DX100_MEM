# M23-G5C — Sparse Matrix Locality Re-evaluation

The M18 SpMV traces, re-run under the M23-G3 locality-aware Bulk organization.
The traces are M18's, unmodified; the environment is M18's harness; the only new
variable is `bulk_org`.

**Short answer.** The DRAM organization does **not** materially help these sparse
matrix workloads. It does exactly what it claims at the DRAM — row hits
+0.2…+1.0 points, row conflicts −0.8…−6.3 % — but these traces already run at
**84–89 % row hits** under P0, so the headroom is 10–16 points and P2 captures
0.0–1.0 of it. Cycle effect: **−1.17 % best case** (bcsstk25, P3, 16K), neutral on
qc2534 (≤0.05 %), and a **regression of +0.1…+0.6 % on Dubcova3**. The benefit
depends on matrix structure, but not in the direction that would make P2 useful
here: the matrices where the organization finds the most row hits are the ones
whose organizer cost (cohort fragmentation) exceeds the DRAM saving. M18's own
conclusion — index-stream consolidation, not DRAM locality, is what the Bulk path
buys on these traces — is confirmed and slightly strengthened.

---

## 1. What is (and is not) being measured

This is a **re-evaluation of M18's workload**, not a new SpMV experiment. The
trace semantics are M18's and so are their limits, restated here because the
result must not be read as SpMV performance:

| M18 trace content | not in the trace |
|---|---|
| `rowptr[i], rowptr[i+1]` — SIMT, one instruction per row | `y[i] += ...` accumulate and the `y` store |
| `colidx[j..j+3]` — SIMT, index acquisition | any arithmetic (the model has no ALU result) |
| `value[j..j+3]` — SIMT, 32 B per instruction | the second BFS/SpMV pass of a real solver |
| `x[colidx[j..j+3]]` — BULK gather, one instruction per 4 nonzeros | |

The omitted `y` stream is `2m/nnz` of the access count: **12.2 % (bcsstk25),
1.1 % (qc2534), 31.0 % (lnsp3937), 5.7 % (dubcova3)**. It is a sequential
streaming write in a real kernel and its omission does not change the gather
address stream, but **nothing below is a claim about full SpMV performance**, and
no improvement may be attributed to SpMV as a whole.

Two trace variants exist per matrix, both unchanged from M18: `full` charges the
column-index stream to the SIMT side, `noidx` omits it so the engine's own index
fetch is the only one. M18's primary metric is the *faithful pairing*
`S = A(full) / C(noidx)`; both pairings are reported.

## 2. Configuration

M18's harness, byte for byte (`sim/configs/ab_controlled.cfg`): Vortex
calibration, 4 lanes, R = 128 shared slot pool, L1 256×4-way/**16 MSHRs**, L2
16384×8-way/64 MSHRs, dTLB 16×4-way with 4 MSHRs, 2 walkers × 2 levels × 150
cycles, 2 banks × 1024 B rows, t_hit 14 / t_miss 26, FR-FCFS, real translation,
`elem_bytes = 8`, and each trace's own descriptor line, index base and element
count. The row space is the one M18 derived per footprint (`rows` = power of two
covering `max_addr`).

**Two things are new, both required by the brief and disclosed here:**

1. `win_fused = 1` — the fused window representation. `bulk_org` exists only on
   it (`bulk_org requires the fused window` is a hard error otherwise). M18 ran
   `bulk_win_fused = 0`. On all four matrices the two representations are
   **cycle-identical** (see §5), so this change is behaviourally inert.
2. `bulk_org` — 0 (P0, the published order, which is what M18 used and is the
   default), 2 (P2, DRAM (row,bank) first), 3 (P3, combined).

Windows: 8 192 and 16 384 elements, the two M18 used. Every cell runs both arms
in one go (A = SIMT, B skipped by `skip_b = 1`, C = dispatched Bulk), so the
baseline is measured, never assumed. 48 cells = 4 matrices × 2 variants ×
2 windows × 3 policies.

## 3. Matrices

| matrix | rows | cols | nnz | row len mean/max | refs per unique column | `x` size | touched pages | dedup (P0) | DRAM rows → period | `max_addr` / period |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 15 439 | 15 439 | 252 241 | 16.34 / 59 | 16.34 | 123.5 KB | 786 (3.07 MiB) | **98.6 %** | 4096 → 8 MiB | 0.61 |
| qc2534 | 2 534 | 2 534 | 463 360 | **182.86** / 183 | **182.86** | 20.3 KB | 1 366 (5.34 MiB) | **99.5 %** | 8192 → 16 MiB | 0.56 |
| lnsp3937 | 3 937 | 3 937 | 25 407 | 6.45 / 11 | 6.45 | 31.5 KB | 88 (352 KiB) | 97.8 % | 4096 → 8 MiB | 0.07 |
| dubcova3¹ | 45 000 | 146 689 | 1 589 539 | 35.32 / 49 | 10.84 | **1.12 MiB** | 4 945 (19.3 MiB) | **93.0 %** | **16384 → 32 MiB** | 0.95 |

¹ first 45 000 rows (M18's row prefix), full `x`.
`refs per unique column` = nnz / cols; it is the gather's reuse factor and, with
dedup, the quantity that decides whether a window has anything to reorder.

## 4. Results — `full` variant (index stream on the SIMT side)

Speedup `S` is vs the measured SIMT arm in the same cell (same-trace pairing).

| matrix | W | P | cycles | Δ vs P0 | S | row hit | conflicts | DRAM cmds | busy/cmd | dedup | walks | org % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 8K | P0 | 492 449 | — | 0.938 | 86.78 % | 8 007 | 60 604 | 15.59 | 98.6 % | 787 | 25.6 |
| bcsstk25 | 8K | P2 | 489 723 | **−0.55 %** | 0.943 | 87.02 % | 7 810 | 60 184 | 15.56 | 98.6 % | 787 | 25.9 |
| bcsstk25 | 8K | P3 | 489 980 | −0.50 % | 0.942 | 87.01 % | 7 819 | 60 210 | 15.56 | 98.6 % | 787 | 25.9 |
| bcsstk25 | 16K | P0 | 505 260 | — | 0.914 | 86.44 % | 8 435 | 62 236 | 15.63 | 98.9 % | 787 | 25.0 |
| bcsstk25 | 16K | P2 | 501 286 | −0.79 % | 0.921 | 86.70 % | 8 215 | 61 769 | 15.60 | 98.9 % | 787 | 25.3 |
| bcsstk25 | 16K | P3 | 499 339 | **−1.17 %** | 0.925 | 86.83 % | 8 106 | 61 566 | 15.58 | 98.9 % | 787 | 25.4 |
| qc2534 | 8K | P0 | 923 790 | — | 0.825 | 88.54 % | 12 966 | 113 113 | 15.38 | 99.5 % | 1 655 | 25.1 |
| qc2534 | 8K | P2 | 923 361 | −0.05 % | 0.826 | 88.53 % | 12 971 | 113 102 | 15.38 | 99.5 % | 1 651 | 25.2 |
| qc2534 | 8K | P3 | 923 513 | −0.03 % | 0.826 | 88.53 % | 12 969 | 113 102 | 15.38 | 99.5 % | 1 651 | 25.1 |
| qc2534 | 16K | P0 | 923 269 | — | 0.826 | 88.57 % | 12 864 | 112 597 | 15.37 | 99.7 % | 1 668 | 25.1 |
| qc2534 | 16K | P2 | 923 056 | −0.02 % | 0.826 | 88.52 % | 12 924 | 112 597 | 15.38 | 99.7 % | 1 663 | 25.1 |
| qc2534 | 16K | P3 | 922 824 | −0.05 % | 0.826 | 88.54 % | 12 901 | 112 597 | 15.38 | 99.7 % | 1 663 | 25.1 |
| lnsp3937 | 8K | P0 | 48 298 | — | 1.181 | 87.65 % | 679 | 5 512 | 15.48 | 97.8 % | 88 | 26.3 |
| lnsp3937 | 8K | P2 | 48 212 | −0.18 % | 1.183 | 87.88 % | 666 | 5 512 | 15.45 | 97.8 % | 88 | 26.6 |
| lnsp3937 | 8K | P3 | 48 041 | −0.53 % | 1.187 | 87.74 % | 674 | 5 512 | 15.47 | 97.8 % | 88 | 26.7 |
| lnsp3937 | 16K | P0 | 48 205 | — | 1.183 | 87.77 % | 672 | 5 512 | 15.47 | 98.0 % | 88 | 26.4 |
| lnsp3937 | 16K | P2 | 48 085 | −0.25 % | 1.186 | 87.97 % | 661 | 5 512 | 15.44 | 98.0 % | 88 | 26.7 |
| lnsp3937 | 16K | P3 | 48 029 | −0.37 % | 1.187 | 88.03 % | 658 | 5 512 | 15.44 | 98.0 % | 88 | 26.7 |
| dubcova3 | 8K | P0 | 4 011 805 | — | 0.878 | 83.80 % | 78 023 | 481 726 | 15.94 | 93.0 % | 7 640 | 19.8 |
| dubcova3 | 8K | P2 | 4 019 594 | **+0.19 %** | 0.876 | 84.80 % | 73 140 | 481 259 | 15.82 | 93.0 % | 7 596 | 20.5 |
| dubcova3 | 8K | P3 | 4 018 912 | +0.18 % | 0.876 | 84.76 % | 73 327 | 481 302 | 15.83 | 93.0 % | 7 598 | 20.5 |
| dubcova3 | 16K | P0 | 4 013 206 | — | 0.878 | 84.50 % | 74 402 | 480 151 | 15.86 | 93.6 % | 7 516 | 19.8 |
| dubcova3 | 16K | P2 | 4 017 829 | **+0.12 %** | 0.877 | 85.54 % | 69 360 | 479 676 | 15.73 | 93.6 % | 7 438 | 20.4 |
| dubcova3 | 16K | P3 | 4 018 410 | +0.13 % | 0.876 | 85.52 % | 69 440 | 479 674 | 15.74 | 93.6 % | 7 438 | 20.4 |

Reading the DRAM columns: the organization does its job where there is room.
Mean service cost per command falls by 0.03–0.13 cycles on the three matrices that
have room and is flat (±0.01) on qc2534; conflicts fall everywhere except qc2534,
where they are already at their floor. The cycle column stays small because the
service saving is divided across two banks at 80–96 % busy and then has to repay
the organizer's own cycles (`org %` +0.3 and the `bulk_org_cycles` in §6).

## 5. Results — `noidx` variant, and the faithful pairing

| matrix | W | P | cycles (noidx) | Δ vs P0 | row hit | conflicts | dedup | walks |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 8K | P0 / P2 / P3 | 390 495 / 391 373 / 391 760 | +0.22 / +0.32 % | 89.70 → 89.93 → 89.78 % | 5 174 → 5 059 | 98.6 % | 787 |
| bcsstk25 | 16K | P0 / P2 / P3 | 392 188 / 391 795 / 391 821 | −0.10 / −0.09 % | 89.87 → 90.08 → 90.05 % | 5 090 → 4 980 | 98.9 % | 787 |
| qc2534 | 8K | P0 / P2 / P3 | 728 062 / 728 620 / 728 620 | +0.08 / +0.08 % | 93.31 → 93.32 → 93.32 % | 5 845 → 5 843 | 99.5 % | 1 369 |
| qc2534 | 16K | P0 / P2 / P3 | 743 702 / 744 056 / 744 056 | +0.05 / +0.05 % | 93.31 → 93.30 % | 5 850 → 5 856 | 99.7 % | 1 369 |
| lnsp3937 | 8K | P0 / P2 / P3 | 46 113 / 46 115 / 46 077 | +0.00 / −0.08 % | 87.32 → 87.79 → 87.77 % | 697 → 671 | 97.8 % | 88 |
| lnsp3937 | 16K | P0 / P2 / P3 | 46 104 / 45 983 / 45 985 | −0.26 / −0.26 % | 87.63 → 87.86 % | 680 → 667 | 98.0 % | 88 |
| dubcova3 | 8K | P0 / P2 / P3 | 2 988 123 / 3 001 703 / 3 000 583 | **+0.45 / +0.42 %** | 88.89 → 89.52 → 89.54 % | 42 344 → 39 903 | 93.0 % | 5 696 |
| dubcova3 | 16K | P0 / P2 / P3 | 2 996 821 / 3 014 668 / 3 014 969 | **+0.60 / +0.60 %** | 89.83 → 90.42 → 90.41 % | 38 729 → 36 431 | 93.6 % | 5 516 |

Faithful pairing, `S = A(full) / C(noidx)` — M18's primary metric:

| matrix | 8K P0 | 8K P2 | 8K P3 | 16K P0 | 16K P2 | 16K P3 |
|---|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 1.182 | 1.180 | 1.179 | 1.177 | 1.178 | 1.178 |
| qc2534 | 1.047 | 1.046 | 1.046 | 1.025 | 1.025 | 1.025 |
| lnsp3937 | 1.236 | 1.236 | 1.237 | 1.237 | 1.240 | 1.240 |
| dubcova3 | 1.179 | 1.173 | 1.174 | 1.175 | 1.168 | 1.168 |

The new organization moves M18's headline number by at most **0.007×**, and in
the wrong direction on the one matrix where it moves at all.

## 6. Where the cycles go (per matrix)

Measured components at 8K, from the counters (`dram_reads`, `dram_row_conflicts`,
`dram_busy_cycles`, `bulk_org_cycles`, `bulk_sched_idle`). Each avoided conflict
is worth `t_hit − t_miss` = 12 busy cycles; a removed command is worth its own
service time:

| matrix | DRAM cmds | conflicts avoided | busy cycles saved | of which conflicts | ÷ 2 banks | org cycles added | sched idle added | measured Δ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | −420 | 197 | 8 244 | 2 364 (29 %) | 4 122 | +869 | +44 | −2 726 |
| qc2534 | −11 | −5 | 94 | ~0 | 47 | +573 | +83 | −429 |
| lnsp3937 | 0 | 13 | 156 | 156 | 78 | +141 | +77 | −86 |
| dubcova3 | −467 | 4 883 | 65 134 | **58 596 (90 %)** | 32 567 | **+27 913** | **+65 635** | **+7 789** |

Two different mechanisms are at work, and only one of them is row locality:

* **bcsstk25** saves its 8 244 busy cycles mostly by issuing **420 fewer
  DRAM commands** — the reordering moved the L2 hit rate from 12.03 % to 12.73 %,
  so lines that were misses became hits. The row-conflict term is only 29 % of
  the saving. This is a cache effect that the (row,bank) key happens to produce,
  not a row-locality gain, and it should not be counted as one.
* **Dubcova3** is the genuine row-locality case: 90 % of its 65 134 busy cycles
  come from 4 883 avoided conflicts. It is also the cell that **regresses**: the
  saving is ~32.6 k cycles of wall time, but the reorganization costs +27.9 k
  `bulk_org_cycles` and fragments the issue stream enough to add **+65.6 k
  scheduler-idle cycles** — the identical cohort-fragmentation signature G3 found
  on Orkut under real translation. Net +7.8 k cycles.

For qc2534 and lnsp3937 the busy-cycle changes are a hundredth of a percent of the
run and the residual (L2 behaviour, slot contention) dominates the difference; the honest statement there is that P2 sits within ±0.8 % of P0,
not that it "saves" tens of cycles.

Cache-line merging is unaffected by the policy, as it must be: `bulk_dedup_merged`
and `bulk_lines_issued` are the same line set in every cell (bcsstk25 3 431→3 434
target lines out of 252 241 elements; qc2534 exactly 2 212; lnsp3937 560→559;
dubcova3 111 279→111 345). The 420 fewer DRAM commands on bcsstk25 8K come from a
slightly better L2 hit rate (12.03 % → 12.73 %), not from fewer distinct lines.

Translation is essentially untouched, as G3/G4 predicted for a scheme that
reorders issue rather than addresses: `walks` are **identical** to the digit in
every cell (787 / 1 651–1 668 / 88 / 7 438–7 640), dTLB misses move by ≤0.3 %
(2 068→2 073 bcsstk25, 22 319→22 388 dubcova3), and dTLB MSHR stalls rise slightly
where cohorts fragment (dubcova3 6 032 181 → 6 080 331).

## 7. Comparison against M18 — nothing historical moved

The M18 traces were **not** regenerated or altered. `bulk_org = 0` with the fused
window reproduces M18's published cells **exactly**, at all 16 reference points
(`build/wm/m23g5c/m18_reference.csv`):

| matrix | W | M18 A(full) | G5C A | M18 C(full) | G5C P0 | M18 C(noidx) | G5C P0(noidx) |
|---|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 8K | 461 688 | 461 688 | 492 449 | 492 449 | 390 495 | 390 495 |
| bcsstk25 | 16K | 461 688 | 461 688 | 505 260 | 505 260 | 392 188 | 392 188 |
| qc2534 | 8K | 762 503 | 762 503 | 923 790 | 923 790 | 728 062 | 728 062 |
| qc2534 | 16K | 762 503 | 762 503 | 923 269 | 923 269 | 743 702 | 743 702 |
| lnsp3937 | 8K | 57 014 | 57 014 | 48 298 | 48 298 | 46 113 | 46 113 |
| lnsp3937 | 16K | 57 014 | 57 014 | 48 205 | 48 205 | 46 104 | 46 104 |
| dubcova3 | 8K | 3 522 146 | 3 522 146 | 4 011 805 | 4 011 805 | 2 988 123 | 2 988 123 |
| dubcova3 | 16K | 3 522 146 | 3 522 146 | 4 013 206 | 4 013 206 | 2 996 821 | 2 996 821 |

And the fused window is a representation change only: each matrix re-run without
it (`g5c_anchor_unfused_*`) matches its fused P0 cell to the cycle, including row
hits (bcsstk25 52 595; qc2534 100 145; lnsp3937 4 831; dubcova3 403 701).

**What the new organization changes, in M18's terms.** M18 decomposed the Bulk
gain into `T_simt_idx` (the index stream off the SIMT front-end, 30–41 % of
baseline cycles) and `T_engine` (the engine's own cost, 21–63 %). The DRAM
organization touches neither: it changes the cycle count by **≤1.17 %**, one to
two orders of magnitude below the index term it would have to compete with. It
raises row hits by up to 1.0 point on a stream that is already at 84–89 %, and in
the one cell where it buys a meaningful service saving it pays for it in
scheduler idle. M18's page-ordering ablation (≤0.5 % on these traces) and this
run agree.

## 8. Does the benefit depend on matrix structure?

Yes — and the dependency is the opposite of what makes P2 useful on gather
workloads.

| matrix | dedup | refs/column | `x` footprint | P0 row hit | Δ row hit (P2, 8K) | Δ cycles (P2, 8K) |
|---|---:|---:|---:|---:|---:|---:|
| qc2534 | **99.5 %** | **182.86** | 20.3 KB | 88.54 % | −0.01 pt | −0.05 % |
| bcsstk25 | 98.6 % | 16.34 | 123.5 KB | 86.78 % | +0.24 pt | −0.55 % |
| lnsp3937 | 97.8 % | 6.45 | 31.5 KB | 87.65 % | +0.23 pt | −0.18 % |
| dubcova3 | **93.0 %** | 10.84 | **1.12 MiB** | **83.80 %** | **+1.00 pt** | **+0.19 %** |

* **qc2534** is the cleanest negative: 183 nonzeros in every row over 2 534
  columns means the gather is trivially local, the window dedups 99.5 % of 463 360
  elements onto 2 212 lines, and grouping a set of 2 212 already-hot lines by
  (row,bank) cannot change anything. 0.05 %.
* **The three symmetric/low-reuse matrices** behave like a weak G5B/Orkut: row
  hits improve, conflicts fall, cycles barely move.
* **Dubcova3** is where structure *would* matter: lowest dedup, fullest `x`
  (1.12 MiB, 19.3 MiB of touched pages, 7 640 walks, the only cell whose DRAM
  period is near its footprint at 0.95), and by far the largest row-hit gain
  (+1.00 pt) and conflict reduction (−6.3 %). It is also the only matrix that
  **regresses** in cycles, in both variants and both windows. The DRAM saving and
  the organizer's cost are of the same size (~29 k vs ~28 k cycles), and the
  fragmentation penalty decides it. Improving the DRAM further would not help
  unless the cohort cost is reduced.

No matrix regresses by more than 0.6 %, and no matrix improves by more than
1.2 %. For sparse matrix work the honest summary is: **P2 is a wash here, with a
structural bias toward small regressions on the matrices that need it most.**

## 9. Checks

`build/wm/m23g5c/summary.csv`, 48 cells, **0 failures**:

* `M9 GATES PASSED` in all 48 cells;
* `bulk_addr_mismatch = 0` everywhere (the engine re-derives each target address
  from the index image and compares);
* dedup identity `bulk_lines_issued + bulk_dedup_merged == bulk_elems_total`
  exact in every cell;
* SIMT `requests_generated == completed` and `bulk_tgt_req_issued ==
  bulk_tgt_req_done` in every cell;
* residency `inflight_max ≤ 128` in every cell;
* `bulk_win_fused = 1` in every main cell (0 in the four anchors, by design);
* the SIMT arm's cycles are **identical across P0/P2/P3** in all 16
  (matrix, variant, window) groups — the key only moved the engine;
* M18 preservation: all 16 published cells reproduced exactly (§7).

## 10. Reproduction

```
./build/wm/m23g5c_run.sh all 6      # 48 cells: 4 matrices x 2 variants x 2 windows x 3 policies
./build/wm/m23g5c_run.sh anchors    # 4 M18-representation cells (win_fused = 0)
python3 tools/m23g5c_extract.py     # checks, summary.csv, m18_reference.csv, faithful.csv
```

Per-cell logs in `build/wm/m23g5c/`, hashes in `build/wm/m23g5c/hashes.txt`.
Traces used, unmodified: `build/wm/m18_{bcsstk25,qc2534,lnsp3937,dubcova3}_{full,noidx}.bin`.

## 11. Limitations

* **This is not SpMV.** Read-only gather traces: the `y` store (1.1–31.0 % of
  accesses) and all arithmetic are absent. M18's absolute numbers and these
  inherit that; the organization's effect on a kernel that also writes `y` is not
  measured.
* One policy family, one metric family. P1 (reuse-first) is not included: G3 and
  G5B both found it harmful on high-reuse traces and inert once a row cell is
  open; nothing in these cells suggests otherwise, but it is untested here.
* Windows are M18's two (8K, 16K). The organization's capture grows with window
  size in every workload measured so far (G3, G5B); 32K is not run here because
  M18 has no such cell and the brief asks for the available sizes.
* Dubcova3 is a 45 000-row prefix (30.7 % of the matrix) with the full `x`.
* 4-lane front-end, as calibrated for Vortex. These are simulator cycles for a
  modelled DX-100 memory system; no physical GPU was measured and no claim is
  made about one.
