# M23-E — Synthetic κ sweep and real-workload locality reference

Final experiment of M23. Architecture and baseline are the frozen M23-A ones;
nothing here redesigns DX-100. The purpose is a controlled reference curve over
the **existing** κ generator, and an honest attempt to place M23-B, M23-C and
M23-D on it.

---

## 1. κ: the definition that already exists

Taken from `sim/src/workload.cpp` (unchanged; this subsection reads it, it does
not modify it):

```cpp
const bool regular = uni(rng) < cfg.kappa;          // per instruction
t.instr_class[i] = regular ? OpClass::Simt : OpClass::Bulk;
if (regular) {
  // L contiguous elements: a *linear* run of the set
  u32 li = seq_cursor % lines.size();
  seq_cursor += max(1, (L + elems_per_line - 1) / elems_per_line);
  ...                                // lanes walk 0..L-1 of one line, then next line
} else {
  li = next_line_idx();              // permuted draw over the Λ-line set
  s  = slot_dist(rng);               // random slot inside the line
}
```

So κ is **the probability that an instruction is drawn from a sequential sweep
of the line set** rather than a permuted random draw. It is fixed per
instruction when the trace is built, and `kappa_real` (printed in every run) is
the measured fraction of instructions whose lanes are contiguous:

| κ configured | 0.0 | 0.1 | 0.2 | 0.3 | 0.4 | 0.5 | 0.6 | 0.7 | 0.8 | 0.9 | 1.0 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `kappa_real` measured | 0.000 | 0.100 | 0.200 | 0.300 | 0.400 | 0.500 | 0.600 | 0.700 | 0.800 | 0.900 | 1.000 |

Realisation is exact at every point, on every seed.

**Verified against the generator, not assumed.** The brief's interpretation —
"increasing κ makes the workload more regular/localised and increasingly
favourable to conventional SIMT" — holds, but *for a specific reason worth
stating*: at κ = 1 the accesses are a linear sweep, and the footprint (512 pages)
fits entirely inside the 512-entry L2 TLB, so M0 issues 128 page walks in the
whole run. At κ = 0 the same 16 384 accesses are a permutation of the same line
set: still only 512 walks, but each one is far from its predecessor and the
SIMT arm cannot keep 128 requests in flight. κ therefore moves *both*
instruction contiguity and page-transition order; it does not move footprint
size, line reuse, or fanout. That distinction drives §5.

No new parameter is introduced and κ is not redefined. `rho_dup = 0` keeps the
duplicate-ablation knob out of the sweep, so κ is the only variable.

## 2. Sweep

| parameter | value | note |
|---|---|---|
| N | 16 384 elements | constant |
| lanes / elem_bytes | 4 / 4 B | constant |
| Λ (Lambda) | 16 384 lines | constant |
| P | 512 pages (2 MiB) | constant on the primary axis |
| κ | 0, 0.1 … 1.0 (11 points) | the only variable |
| seed | 1, 2, 3 | identical inputs across the four configurations |
| rho_dup | 0 | pure permutation at κ = 0 |

Every cell runs the identical trace: the sweep is a loop over `--set kappa`,
and the four configurations differ only in the DX-100 block (M23-A).

**M17 anchors preserved.** Re-running the documented M17 commands on the
unmodified `configs/ab_controlled.cfg` reproduces the published numbers exactly:

| anchor | expected | measured |
|---|---:|---:|
| κ = 0, SIMT (A) | 270 753 | **270 753** |
| κ = 0.25, SIMT (A) | 220 123 | **220 123** |
| κ = 0.25, 2×8K dispatched (C) | 125 581 | **125 581** |
| κ = 0.25, bulk arm (B) | 138 924 | **138 924** |

κ = 0.25 is inside the sweep as the 0.2/0.3 pair; the M17 anchor itself keeps
its own historical configuration and is not overwritten.

## 3. Results

Mean of three seeds. Cycles, then speedup over the M0 run of the same κ.

| κ | M0 | 2×8K | 2×16K | 2×32K | sp 8K | sp 16K | sp 32K |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.0 | 262 284 | 148 836 | 129 036 | 129 036 | 1.762 | 2.033 | 2.033 |
| 0.1 | 242 299 | 137 938 | 117 367 | 117 367 | 1.757 | 2.064 | 2.064 |
| 0.2 | 222 615 | 127 135 | 106 182 | 106 182 | 1.751 | 2.097 | 2.097 |
| 0.3 | 202 584 | 116 599 | 96 212 | 96 212 | 1.737 | **2.106** | **2.106** |
| 0.4 | 182 966 | 106 413 | 89 667 | 89 667 | 1.719 | 2.041 | 2.041 |
| 0.5 | 162 422 | 91 338 | 91 047 | 91 047 | 1.778 | 1.784 | 1.784 |
| 0.6 | 145 480 | 92 285 | 92 285 | 92 285 | 1.576 | 1.576 | 1.576 |
| 0.7 | 126 216 | 93 617 | 93 617 | 93 617 | 1.348 | 1.348 | 1.348 |
| 0.8 | 108 547 | 94 620 | 94 620 | 94 620 | 1.147 | 1.147 | 1.147 |
| 0.9 | 87 828 | 93 369 | 93 369 | 93 369 | **0.941** | **0.941** | **0.941** |
| 1.0 | 40 281 | 40 281 | 40 281 | 40 281 | 1.000 | 1.000 | 1.000 |

Seed spread (max − min over 3 seeds): ≤ 5.1 % of the mean on M0 (worst cell
κ = 0.9, where the run is short and the tail decides it), ≤ 1.2 % on the DX
configurations; 0 % at κ = 1.

### Translation, cache, DRAM, window (2×16K, and M0 for contrast)

| κ | walks M0 | walks 16K | L2% M0 | L2% 16K | row-hit% M0 | row-hit% 16K | Bulk elems | fanout cyc | window fill | inflight M0/16K |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.0 | 512 | 529 | 5.7 | 3.1 | 5.32 | 93.73 | 16 384 | 16 384 | 16 384 | 128 / 120 |
| 0.1 | 512 | 528 | 7.8 | 5.6 | 5.97 | 93.09 | 14 747 | 14 475 | 14 747 | 128 / 128 |
| 0.2 | 512 | 526 | 10.0 | 7.5 | 7.58 | 92.39 | 13 176 | 12 730 | 13 176 | 128 / 128 |
| 0.3 | 512 | 525 | 12.4 | 9.7 | 9.84 | 91.51 | 11 555 | 11 016 | 11 555 | 128 / 128 |
| 0.4 | 512 | 523 | 14.7 | 14.3 | 13.02 | 90.22 | 9 905 | 9 248 | 9 905 | 128 / 128 |
| 0.5 | 512 | 521 | 17.1 | 16.6 | 18.20 | 88.84 | 8 168 | 7 452 | 8 168 | 128 / 128 |
| 0.6 | 512 | 520 | 18.7 | 18.3 | 24.22 | 87.37 | 6 643 | 5 987 | 6 643 | 128 / 128 |
| 0.7 | 512 | 518 | 20.3 | 19.9 | 33.88 | 85.23 | 4 880 | 4 314 | 4 880 | 128 / 128 |
| 0.8 | 511 | 516 | 21.0 | 20.7 | 46.05 | 83.02 | 3 289 | 2 879 | 3 289 | 128 / 128 |
| 0.9 | 495 | 498 | 20.7 | 20.5 | 64.98 | 82.12 | 1 653 | 1 456 | 1 653 | 128 / 94 |
| 1.0 | 128 | 128 | 5.6 | 5.6 | 93.59 | 93.59 | 0 | 0 | 0 | 57 / 57 |

Row-hit rate is `dram_row_hits / dram_reads` throughout M23-E (fraction of reads
that hit an already-open row). Note this differs from the `acts_sum` denominator
used incidentally in the M23-C/D prose; where the two disagree the difference is
under two percent and M23-E standardises on reads.

### Figures

| figure | file |
|---|---|
| speedup vs κ, all four configurations | `docs/figs/m23e-speedup.svg` |
| execution cycles vs κ (log scale) | `docs/figs/m23e-cycles.svg` |
| page walks per access vs κ | `docs/figs/m23e-walks.svg` |
| L2 and DRAM row-hit behaviour vs κ | `docs/figs/m23e-locality.svg` |
| context-window utilisation vs κ | `docs/figs/m23e-window.svg` |

Generated by `tools/m23e_extract.py` from the logs, with no plotting library
(plain SVG), and the underlying numbers in `build/wm/m23e_sweep.csv`.

### Compiler selection, per κ (2×16K)

| κ | Bulk instructions | SIMT instructions | Bulk elements | Bulk share of elements | selected path |
|---:|---:|---:|---:|---:|---|
| 0.0 | 4 096 | 0 | 16 384 | 100 % | all Bulk |
| 0.1 | 3 687 | 409 | 14 747 | 90 % | mixed |
| 0.2 | 3 294 | 802 | 13 176 | 80 % | mixed |
| 0.3 | 2 889 | 1 207 | 11 555 | 71 % | mixed |
| 0.4 | 2 476 | 1 620 | 9 905 | 60 % | mixed |
| 0.5 | 2 042 | 2 054 | 8 168 | 50 % | mixed |
| 0.6 | 1 661 | 2 435 | 6 643 | 41 % | mixed |
| 0.7 | 1 220 | 2 876 | 4 880 | 30 % | mixed |
| 0.8 | 822 | 3 274 | 3 289 | 20 % | mixed |
| 0.9 | 413 | 3 683 | 1 653 | 10 % | mixed |
| 1.0 | 0 | 4 096 | 0 | 0 % | **all SIMT** |

The rule is the generator's own class metadata (`dispatch = 1`,
`op_class = gen`): a contiguous instruction is SIMT, a scattered one is Bulk.
It is fixed before execution, uses no runtime sample, no fallback, and no
address oracle. At κ = 1 every instruction is contiguous, so **all three DX-100
configurations reduce exactly to M0** — 40 281 cycles, zero descriptors, zero
index lines — which is the expected and required behaviour, not a bug.

## 4. Break-even

* **M0 wins from κ ≈ 0.85 upward.** Between κ = 0.8 (DX 1.147×) and κ = 0.9
  (DX **0.941×**) the sign changes; at κ = 1.0 the two are identical by
  construction.
* The κ = 0.9 point is a **real regression, +5.9 % over M0** (93 369 vs 87 828
  cycles), and it is not a selection error: 3 683 of 4 096 instructions were
  correctly routed to SIMT. It is an *engine* cost that the residue no longer
  amortises — 1 653 Bulk elements spread over 413 instructions, which cost
  `bulk_slot_stall = 33 456` cycles of contention in the shared 128-slot pool
  and hold the last Bulk element to cycle 93 553 of 93 554 (seed-1 run).
  The SIMT side actually improved (issue stalls 139 010 vs M0's 275 982); the
  engine's tail ate the gain.
* **Per-operation selection cannot fix this**, because every individual decision
  was right. What is missing is a *whole-kernel* rule — "if the expected Bulk
  element count is below some fraction of N, dispatch nothing" — which the
  frozen M23-A architecture does not have. Recorded as a finding for a future
  milestone; the preferred architecture is **not** changed here.
* **16K vs 32K: identical at every κ** (byte-for-byte, including seeds). With
  N = 16 384 the trace never contains more than 16 384 elements, so a 16 K
  window already spans the whole run and 32 K can add nothing. This is a
  limitation of the sweep's scale, not evidence about 32 K — M23-D measured
  1.224× → 1.481× for 16K → 32K at 1.2 M accesses.
* **8K vs 16K** separates only while Bulk elements exceed 8 192, i.e. κ ≤ 0.5;
  from κ = 0.6 on all three DX cells are identical because the window never
  fills.

## 5. Mapping the real workloads onto κ

Measured M0 arms of the three real experiments (M23-B, M23-C bfs frontier,
M23-D):

| workload | accesses | lines | pages | ρ_line | κ_real | walks/acc | L2 % | row-hit % | working set |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| B — Criteo embedding | 638 976 | 64 252 | 10 670 | 0.899 | 0.037 | 0.297 | 85.9 | 5.97 | 3.9 MiB |
| C — com-Orkut gather | 1 200 003 | 100 282 | 2 048 | 0.916 | 0.000 | 0.260 | 71.1 | 29.8 | 6.1 MiB |
| D — ogbn-products | 1 200 003 | 689 990 | 38 246 | 0.425 | 0.000 | 0.979 | 66.3 | 0.14 | 42.1 MiB |

Synthetic reference on the two axes (M0 arm):

| axis | κ | ρ_line | walks/acc | L2 % | row-hit % | pages |
|---|---:|---:|---:|---:|---:|---:|
| P=512 (2 MiB) | 0.0 | 0.000 | 0.031 | 5.7 | 5.32 | 512 |
| P=512 | 0.5 | 0.431 | 0.031 | 17.1 | 18.20 | 512 |
| P=512 | 1.0 | 0.750 | 0.008 | 5.6 | 93.59 | 128 |
| P=8192 (32 MiB) | 0.0 | 0.000 | 0.969 | 64.5 | 0.13 | 8 192 |
| P=8192 | 0.25 | 0.229 | 0.759 | 63.4 | 0.12 | 7 749 |
| P=8192 | 0.5 | 0.431 | 0.545 | 61.6 | 0.16 | 6 439 |
| P=8192 | 0.75 | 0.611 | 0.328 | 57.3 | 0.10 | 4 442 |
| P=8192 | 1.0 | 0.750 | 0.125 | 46.9 | 0.05 | 2 048 |

The P = 8192 axis is a **supplementary reference**, not part of the κ curve: it
varies footprint (the one thing κ cannot vary) at five κ points, and it exists
only because the primary 2 MiB footprint is smaller than every real workload.

Nearest synthetic κ for each measured dimension:

| dimension | B | C | D |
|---|---|---|---|
| instruction contiguity (`κ_real`) | κ = 0.04 | κ = 0 | κ = 0 |
| line reuse (`ρ_line`) | **beyond κ = 1** | **beyond κ = 1** | κ ≈ 0.5 |
| page transitions (`walks/acc`, P=8192 axis) | κ ≈ 0.75 | κ ≈ 0.83 | κ ≈ 0.0 |
| DRAM row locality | κ ≈ 0.0 | κ ≈ 0.66 | κ ≈ 0.0 (P=8192) |
| cache hit rate | ≈ κ = 0 (P=8192) but far above it | κ ≈ 0 (P=8192) | κ ≈ 0 (P=8192) |

**No scalar κ.** The three workloads occupy ranges that do not intersect:
B ≈ 0.0–0.75, C ≈ 0.6–0.85 with reuse past the axis end, D ≈ 0.0–0.5. Reporting
"B is κ = 0.75" from one matching metric would contradict its `κ_real = 0.037`
and its row-hit rate.

Why: at κ = 0 the generator produces a **permutation of a 16 384-line set** —
zero duplicates by construction (`ρ_line = 0.000`). Real gathers always reuse
lines heavily (0.43–0.92). κ's high end produces *sequential* streams, which
make M0 fast by themselves; the real workloads instead have M0 slow
(0.98 walks/access for D) with heavy reuse that only a window can exploit. The
two families fail in opposite directions, so neither end of the κ axis can
represent them.

Which environment *does* line up: on the P = 8192 axis at κ = 0, `walks/acc`
0.969 vs D's 0.979, row-hit 0.13 % vs D's 0.14 %, L2 64.5 % vs 66.3 %. D is a
near-match to κ = 0 at a footprint-matched axis, and its higher `ρ_line` (0.425
vs 0.000) is the only dimension that pulls it toward κ = 0.5.

Real-workload speedups are therefore **not** predicted by the synthetic curve,
and the report does not use κ to explain them:

| workload | measured 16K speedup | synthetic 16K at matched walks/acc |
|---|---:|---:|
| D | 1.267× | 1.925× (κ = 0, P=8192) |
| B | 2.323× | 1.054× (κ = 0.75, P=8192) |
| C | 2.936× | 1.054× (κ = 0.75, P=8192) |

D under-performs the matched synthetic point because its footprint is 4.7×
larger (38 246 pages), so dedup removes less. B and C over-perform by 2–3×
because their reuse is line-level and clustered, which the synthetic permutation
never generates.

What κ captures: instruction contiguity, and through it sequential page order and
the resulting walk count when the footprint fits the TLB.
What it does not: footprint size, line-level reuse, clustered-but-unordered page
revisits, fanout, and multi-line vectors.

## 6. Analysis questions

1. **How does DX performance change as the workload becomes regular?** It rises
   to a peak at κ ≈ 0.3 (2.106× at 16K) and then falls monotonically to parity
   at κ = 1. The peak exists because the engine keeps 70 % of the elements
   (enough to amortise its prologue) while the SIMT share is already sequential.
2. **At what κ does M0 become competitive or superior?** Competitive from
   κ ≈ 0.8 (1.147×), superior from κ ≈ 0.85; at κ = 0.9 DX is 5.9 % slower.
3. **Does selection make the appropriate decision?** Yes at the operation level
   (κ = 1 → 4 096 SIMT, 0 Bulk; κ = 0 → 4 096 Bulk), and the regression at 0.9
   is not a misrouting but unamortised engine overhead.
4. **Does a larger window move the break-even point?** No. 8K, 16K and 32K
   coincide from κ = 0.6 upward; below that 16K dominates 8K. The break-even κ
   is a property of the selection residue, not of capacity.
5. **Does 32K give meaningful gains over 16K?** In this sweep, none at all —
   identical cycles at every κ and every seed. Explained by N = 16 384 < 16 K
   window occupancy; M23-D, at 1.2 M accesses, measured a real 16K → 32K gain.
6. **Do the real workloads resemble low-, medium- or high-κ behaviour?**
   D ≈ low (κ ≈ 0), C ≈ medium (0.6–0.85 on translation, past the axis on
   reuse), B ≈ low-to-medium (0.0–0.75). None is a single point.
7. **Are real results explained by page, cache or DRAM locality?** Page
   transitions: `walks × 150` accounts for 100.0 % of M0 cycles in B, 100.0 % in
   D, and 95.5 % in C. Cache and DRAM locality modulate the remaining few
   percent only.
8. **Does selection avoid regressions on regular workloads?** It avoids
   *misrouting* (κ = 1 is exactly M0), but not the κ = 0.9 regression. That is
   the one place where the frozen design loses to M0, and it is reported rather
   than tuned away.

## 7. Correctness and accounting

* 152 runs (132 sweep + 20 supplementary): `conservation = OK`,
  `bulk_addr_mismatch = 0` and `dispatch_unaccounted = 0` in every one.
* Value equivalence: `simt_completion_hash` is identical between M0 and the DX
  configurations at the same κ (e.g. κ = 0.5: 541 972 954), so the two paths
  consume the same addresses and produce the same values.
* Identical inputs across architectures: one trace per κ, replayed by all four
  configurations; `accesses`, `instrs`, `lines`, `pages`, `rho_line` and
  `kappa_real` match across configurations at every κ.
* Element accounting: Bulk + SIMT elements = N at every κ (16 384).
* Bulk overhead charged, not hidden: index lines (1 024 at κ = 0, matching
  `16 384 × 4 / 64`), descriptor fetch, fanout cycles, slot stalls.
* M17 anchors reproduced exactly (§2); no historical file was modified.
* Full regression suite: `./bin/dx100_tests` → **4 735 checks, 0 failures**.

## 8. Limitations

1. **N = 16 384 caps the capacity axis.** A 16 K window already spans the whole
   trace, so 16K and 32K are indistinguishable and the κ curve says nothing
   about window capacity above 16 K. The real-workload arms of M23 are the
   evidence there.
2. **P = 512 means the footprint fits the TLB** (512 entries). The primary
   curve therefore cannot show walk-dominated behaviour; the supplementary
   P = 8192 axis exists to expose it, and is labelled as an axis, not a κ point.
3. **κ = 0 has zero line reuse by construction** (permutation). Real gathers have
   ρ ≈ 0.43–0.92, so the low end of the curve is a different regime from every
   real workload, not merely a different point on the same line.
4. **Single seed at the supplementary axis** (5 points, seed 1); the primary
   sweep uses three seeds.
5. **No whole-kernel dispatch decision** exists in the frozen architecture, which
   is why κ = 0.9 regresses. Documented, not fixed.

## 9. Reproducibility

```bash
# sweep: 11 x 4 x 3 seeds + 5 x 4 footprint-matched cells
bash build/wm/m23e_sweep.sh

# tables + figures
python3 tools/m23e_extract.py       # -> build/wm/m23e_sweep.csv, docs/figs/m23e-*.svg

# one cell by hand (κ = 0.3, 2x16K)
cd sim && ./bin/dx100sim --config configs/m23_dx16k.cfg --set kappa=0.3 --set seed=1 \
    --set dram_diag=1 --set cache_diag=1 --all

# M17 anchors (unmodified historical configuration)
./bin/dx100sim --config configs/ab_controlled.cfg --set rho_dup=0 --set elem_bytes=4 \
  --set bulk_wl=E0 --set bulk_max_elems=262144 --set max_cycles=200000000 \
  --set bulk_index_base=8388608 --set bulk_desc_line=98304 --set page_sched=1 \
  --set idx_mode=2 --set experiment=m9diag --set pool_slots=128 --set cache_diag=1 \
  --set Lambda=16384 --set P=512 --set bulk_stream=0 --set N=16384 --set seed=1 \
  --set kappa=0.25 --set batch_window=8192 --all      # -> 220123 / 138924 / 125581
```

Logs: `build/wm/m23e_{cfg}_k{κ}_s{seed}.txt` and
`build/wm/m23e_p8k_{cfg}_k{κ}.txt`. Sweep driver: `build/wm/m23e_sweep.sh`.
Extractor: `tools/m23e_extract.py`.
