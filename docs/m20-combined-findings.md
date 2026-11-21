# M20 — Combined Findings

**Question.** Does Bulk 2×8K or 2×16K outperform a properly modelled wider SIMT
baseline when both perform identical logical SpMV work?

**Answer.** No — not *as a general property*. Bulk's advantage at the
Vortex-calibrated 4-lane baseline is real and reproducible, but it is an advantage
over a **request-starved baseline**, not an intrinsic advantage of the Bulk
architecture. As SIMT widens, the advantage shrinks on every workload, disappears
and inverts on one, and on none of the four does it hold at its M18 size.

With the brief's four permitted conclusions: **option 2, "shrinks
substantially"**, plus the qualifier that on qc2534 it goes further and becomes
option 3, **"disappears"**.

---

## 1. Summary table: win or loss, every width, every workload

`S_faithful` = A(full)/C(noidx) — each arm pays its own index acquisition (M19 §1).
2×8K primary; 2×16K in brackets.

| workload | 4 lanes | 8 lanes | 16 lanes | 32 lanes |
|---|---:|---:|---:|---:|
| bcsstk25 | **1.182** win | **1.100** win | **1.065** win | **1.058** win |
|  | (1.177) | (1.100) | (1.064) | (1.058) |
| qc2534 | **1.047** win | **0.942** LOSS | **0.930** LOSS | **0.930** LOSS |
|  | (1.025) | (0.922) | (0.910) | (0.911) |
| lnsp3937 | **1.236** win | **1.188** win | **1.160** win | **1.160** win |
|  | (1.237) | (1.193) | (1.158) | (1.158) |
| dubcova3¹ | **1.180** win | **1.135** win | **1.125** win | **1.124** win |
|  | (1.177) | (1.132) | (1.121) | (1.120) |

¹ row-prefix experiment: 45 000 of 114 599 rows, complete `x`, marked as such
throughout.

Mean over the three square matrices: **1.155 (4 lanes) → 1.077 (8) → 1.052 (16) →
1.050 (32)**; over all four workloads 1.162 → 1.091 → 1.070 → 1.068. The first
doubling of SIMT width removes more than half of the mean advantage (0.078 of
0.155); the next two doublings together remove only 0.027, because what is left is
the target merge and the engine's fixed overhead, neither of which lane width
touches.

## 2. What was actually established

1. **The 4-lane results reproduce exactly.** Every 4-lane cell matches M18/M19 to
   the digit (including M19's corrected dubcova3 1.180/1.177), and the M17 anchor
   reproduces 220 123 / 138 924 / 125 581. The width experiment changed one
   parameter and nothing else.
2. **DRAM traffic is width-invariant** on every workload: A's reads are identical
   across widths on the three square matrices (50 245 / 87 447 / 5 511) and vary by
   6 lines in 383 000 on the dubcova3 prefix. Widening SIMT changes request
   formation, never compulsory traffic, so any cycle difference is a request-path
   difference — which is what makes the comparison clean. The Bulk engine reads
   *fewer* DRAM lines than SIMT on dubcova3 (381 455 vs 383 142 at 4 lanes) because
   its merge removes duplicate gathers.
3. **The advantage is dominated by the index stream's request amplification at
   4 lanes**, not by the gather. bcsstk25 at 4 lanes: SIMT reads the index array in
   79 573 requests where the engine needs 15 766 (5.05×). Widen to 16 lanes and
   SIMT needs 38 073 (2.42×), because 16 lanes × 4 B is exactly one 64 B line.
4. **The engine's request counts never change with width** (index 15 766, target
   3 431, invariant), so the *engine* side of every comparison is a constant and
   the sweep measures only the baseline's improvement.
5. **The hybrid benefits from width far less than the baseline does**: C(noidx)
   moves −1.4 % / −0.4 % / −2.9 % / +0.09 % from 4 to 32 lanes (bcsstk25, qc2534,
   lnsp3937, dubcova3) against A's −11.8 % / −11.5 % / −8.9 % / −4.7 %. Its
   critical path is the engine's own index acquisition, which
   `bulk_ag_cycles == C(full)` confirms at 8 lanes and above.
6. **The one workload that inverts (qc2534) does so for a structural reason that
   survives a 2× residency-budget probe.** Doubling the shared pool cuts `C(full)`
   12–13 % and leaves `S_f` at 0.942. The hybrid's floor (~725 000 cycles) does not
   move; 8-lane SIMT simply gets under it.
7. **2×16K is never better than 2×8K at wide lanes** (it is worse on qc2534 at
   every width, worse on lnsp3937 above 4 lanes, and equal on bcsstk25). Larger
   windows buy locality but cost fill time, and fill time is exactly what wide SIMT
   stops paying for. No per-width window tuning was done, and none is justified by
   these data.
8. **Dubcova3 behaves like bcsstk25, and better**: 1.180 → 1.135 → 1.125 →
   1.124, a win at every width that loses 31 % of its surplus. Its index array is
   the largest of the four (1.62 M nnz = 6.5 MB of index, amplification 4.99× →
   1.59×), so the index half of its advantage decays like qc2534's; but its
   *gather* half was always small (only 5.8–7.4× request reduction, because its
   146 689 columns leave almost no duplicate columns inside an 8K window), so there
   was little left to lose and the result stays positive.

## 3. The mechanism, in one paragraph

Bulk wins at 4 lanes because it does two things the baseline cannot: it reads the
index array as a dense line stream (1 request per 16 indices instead of 4), and it
merges duplicate gathers and sorts them by line (136 747 requests → 3 431). Wider
SIMT takes the first away — at 16 lanes a SIMT index group is already exactly one
cache line, so the baseline approaches the engine's own density, limited only by
row-boundary fragmentation and unaligned group starts (amplification floors at
1.5–3.3×, never reaching 1×). Wider SIMT does *not* take the second away: the
gather's 31–71× request advantage is intact at 32 lanes. But the gather is
L2-resident and cheap per request, while the index stream is compulsory DRAM
misses; so removing the index stream is where the cycles were, and that is the
half that wider warps erode. Meanwhile the engine's own cost does not fall — its
index acquisition stretches to fill the whole run at 8 lanes and above, and its
residency-slot stalls double — so the price of the trade stays fixed while the
prize shrinks. qc2534, with the largest index array (41 % of baseline runtime),
crosses over at the first doubling. Full analysis: `m20-mechanism-analysis.md`.

## 4. The dubcova3 caveat

Dubcova3 is a **row-prefix experiment**: 45 000 of 114 599 rows (30.7 %) with the
complete `x` vector, so it exercises every column and the full address range while
doing 30.7 % of the rows. Consequences, stated plainly:

* absolute costs are not the full matrix's, and S_f is not comparable to a
  full-matrix number;
* the prefix's row-length distribution is not the matrix's, so its index
  amplification is specific to the prefix;
* its `x` is 1.17 MB (18 337 lines), the only one of the four that marginally
  exceeds the 1 MB L2 (`x` is 123 KB / 20 KB / 31 KB on the other three), which is
  why its merge *reduces* DRAM reads (383 142 → 381 455 for 2×8K, 382 159 at 8
  lanes) — it is the only matrix where duplicate gathers would have missed.

It is reported because the brief requires the fourth matrix and because its
qualitative behaviour (win that shrinks) matches bcsstk25.

## 5. Controls, and how each was met

| control | status |
|---|---|
| no per-workload Bulk tuning | one policy, one parameter set; only `lanes` and the window differ |
| same logical work and accounting | three arms from one M9 run per cell; identical traces per (matrix, width); M19 audited the accounting |
| no free index acquisition | A(full) vs C(noidx) charges each arm its own acquisition; same-trace pairing also reported (results §1) |
| no free target addresses | target gather is charged its lines: `x` is 18 337 / 1 931 / 317 / 494 lines for dubcova3 / bcsstk25 / qc2534 / lnsp3937 |
| only 2×8K and 2×16K | the only two windows run, at all four widths |
| ≥3 paired seeds where randomness exists | **no randomness exists on the traced path** — `seed` parameterises only the synthetic generator. Three repeat runs of one cell are bit-identical (409 708 / 0 / 423 380, same hashes, same engine counters). Three seeds would be theatre; three identical repeats are the stronger statement. |
| preserve M17/M18 data | no `m17_*` / `m18_*` / `m19_*` file modified (all dated before the M20 sweep); M20 writes only `m20_*` |
| full regression | `./bin/dx100tests` — **4 470 checks, 0 failures** |
| M17 anchor | **220 123 / 138 924 / 125 581** — exact |

## 6. Robustness

* **Window sensitivity**: results §1 reports both windows for every cell. The
  verdict does not depend on the window: 2×16K is never a win where 2×8K is a
  loss, and 2×8K is never a loss where 2×16K is a win.
* **Residency-budget probe** (diagnostic only, §6 of the mechanism report):
  doubling `pool_slots` cuts the hybrid's `C(full)` by 12–13 % and moves `S_f` by
  +0.004 at 4 lanes and 0.000 at 8 lanes. The shared-pool coupling inflates the
  *price* but does not create the verdict.
* **Determinism**: three identical runs, above.
* **Cross-check**: the 4-lane column reproduces published M18/M19 values on four
  matrices, which is a strong end-to-end check on trace generation, the M19 layout
  fix, and the extraction script.

## 7. What this study does not establish

1. **It is not a 32-lane GPU performance claim.** The width knob models
   instruction formation only (validation report §2). Wider real hardware would
   also widen LSU issue, add MSHRs and likely improve the coalescer — all of which
   would help the baseline, so the direction of the measured trend is safe, but the
   magnitudes beyond 8 lanes are sensitivity analysis, not prediction.
2. **It does not say Bulk is useless.** It says Bulk's advantage is conditional on
   the baseline being request-starved. Against long-row matrices at 4 lanes it is
   1.05–1.24×; against 32-lane formation it is 0.93–1.16×.
3. **It does not measure a fused-window or cross-row coalescer.** The remaining
   structural cost on the SIMT side is row-boundary fragmentation; a coalescer that
   merges across row boundaries would erode the last `ΔA_idx` surplus, and this
   model does not have one.
4. **Dubcova3 is a prefix**, §4.
5. **No write path exists** in these traces (all are read-only SpMV/BFS inputs), so
   DRAM writes are 0 by construction, not by measurement.

## 8. Where the remaining advantage lives

The advantage that survives every width is the **target merge**: 3 431 requests
against 105 572 at 32 lanes on bcsstk25, and it is the only term that does not
decay with lane width. But it is also the term that pays least per request, because
`x` is L2-resident on the three square matrices, and it is the term that shrinks
when the column space gets wide: on dubcova3 (146 689 columns) the merge is only
5.8–7.4× instead of 31–71×, because an 8K window then covers just 5.6 % of the
column space.

The consequence for the project is uncomfortable but clear: Bulk's value
proposition as a *SpMV index accelerator* is a 4-lane-era result. If the baseline
going forward is 8 lanes or wider, the architectural case cannot rest on the dense
index stream — wider warps match it. What is left is the merged gather, and even
that is only worth many cycles on narrow, L2-resident column spaces, which is
exactly where the baseline is already cheapest.

## 9. Deliverables

| file | contents |
|---|---|
| `m20-simt-width-model-validation.md` | what `lanes` models, checklist per width, labelling rules |
| `m20-width-sweep-results.md` | all 4 matrices × 4 widths × 2 windows, absolute cycles/requests/DRAM, both speedups, per-metric behaviour |
| `m20-mechanism-analysis.md` | four-arm decomposition, index vs gather split, qc2534 inversion, pool probe |
| `m20-combined-findings.md` | this file: verdict, summary table, controls, limitations |
| `tools/m20_extract.py` | `summary` / `memory` / `mechanism` / `split` extraction from the 64 cells |
| `build/wm/m20_matrix.sh`, `m20_matrix.log` | sweep driver and log (64 cells) |
