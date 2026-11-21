# M17 — Combined findings

**Central question.** Under what workload conditions and architectural
configurations does the Bulk accelerator outperform a well-provisioned
conventional SIMT memory system?

**One-paragraph answer.** It outperforms when the conventional front-end's
requests reach DRAM in an order that destroys row locality, and it wins by
fixing that order. On W1 at N = 16 384 the SIMT arm's DRAM row-hit rate is
**3.9 %** at κ = 0.25 and **1.4 %** at κ = 0; the engine, which issues ascending
by page and then by line, holds **83.7–87.5 %** at every κ. That gap is worth
**1.75–2.08×** at κ ≤ 0.25 and it survives an 8× larger L1 MSHR array, a 1.5 MiB
larger L2, and every LSU-pending setting tested. It closes when the workload
itself supplies the locality: SIMT's row-hit rate rises monotonically with κ to
93.6 %, and at κ = 1.00 the speedup is exactly 1.000. It can also be removed by
configuration: with page ordering disabled the hybrid is **0.991×** at κ = 0.25,
and with the cohort scheduler reduced to one it is **0.758×** at κ = 0.75. The
advantage is therefore a property of *ordering a scattered stream*, not of the
accelerator's storage, its concurrency, or its existence.

No silicon performance claim is made anywhere in this milestone. Every number is
a simulator measurement of the existing model.

---

## 1. Can the original 2.16× and 2.54× results be reproduced?

**The cells reproduce exactly; the ratios do not correspond to any single
experiment.**

| claim | arithmetic | reproduced? |
|---|---|---|
| SIMT-only 270 753 | W1, κ = **0**, N = 16 384 | **yes, exact** (`P0_k0_w8192_s1`) |
| Bulk 2×8K 125 581 | W1, κ = **0.25**, 2×8K | **yes, exact** (`P1_w8192_m16_s1`) |
| Bulk 2×16K 106 456 | W1, κ = **0.25**, 2×16K | **yes, exact** (`P1_w16384_m16_s1`) |
| 2.16× | 270 753 / 125 581 | arithmetic over **two different κ** |
| 2.54× | 270 753 / 106 456 | arithmetic over **two different κ** |

Matched-κ speedups for the same cells:

| κ | 2×8K | 2×16K |
|---|---|---|
| 0.25 (where the Bulk cells were measured) | **1.753** | **2.068** |
| 0 (where the SIMT anchor was measured) | 1.791 | 1.913 |

Both claimed figures are exactly **1.230×** their matched-κ value, and 1.230 is
270 753 / 220 123 — the κ = 0 : κ = 0.25 ratio of the SIMT arm itself. The entire
discrepancy is the κ mismatch. No document in `DX100_MEM/docs` states 2.16× or
2.54×; they are ratios that can be formed from two correctly measured anchors, not
measurements.

The reproductions are exact to the cycle, gates pass, and the SIMT arm is proved
identical under both the M5.1 (`experiment=ab`) and M9 (`experiment=m9diag`)
drivers at both κ for all three seeds.

## 2. How much of the advantage survives 128 L1 MSHRs?

**Essentially all of it — between 1.2 % less and 1.5 % more, depending on the
window.**

| configuration | κ | 16 MSHRs | 128 MSHRs | change |
|---|---|---|---|---|
| 2×8K | 0.25 | 1.7686 | **1.7481** | −1.2 % |
| 2×16K | 0.25 | 2.0812 | **2.1115** | +1.5 % |
| 2×8K | 0 | 1.8061 | 1.7658 | −2.2 % |
| 2×16K | 0 | 1.9260 | 2.0365 | +5.7 % |

The arms move in the same direction and by similar amounts (SIMT-only −3.9 %,
2×8K hybrid −2.7 %, 2×16K hybrid −5.3 % at κ = 0.25), so the ratio is close to
invariant. The 2×16K configuration gains slightly because a larger window can use
the extra MSHRs; 2×8K cannot.

An 8× MSHR increase is also not a *better* baseline in any absolute sense: the
published configuration's 16 L1 MSHRs sit behind a 128-slot residency pool that
overstates concurrency by 5.5× (§5), so "more MSHRs" and "more MLP" are not the
same intervention.

## 3. How much does LSU pending capacity independently affect the comparison?

**It is an independent axis with a real but modest effect at this geometry, and
it is one-sided.**

| L1 MSHR | `lsu_pending` | SIMT-only | hybrid | A/C |
|---|---|---|---|---|
| 16 | 0 (published) | 220 019 | 124 406 | 1.7686 |
| 16 | 8 (Vortex default) | 227 313 | 118 242 | **1.9224** |
| 16 | 32 | 220 045 | 121 800 | 1.8066 |
| 128 | 0 | 211 495 | 120 988 | 1.7481 |
| 128 | 8 | 227 313 | 117 984 | **1.9266** |
| 128 | 32 | 215 116 | 120 421 | 1.7864 |

* **Independent of the MSHR array:** with `lsu_pending = 8` the SIMT-only arm is
  identical at 16 and 128 MSHRs (227 313 both), because the cap binds below the
  array. This is why the two cannot be used as proxies for one another.
* **One-sided by construction:** all three call sites test `in_sys[core]` inside
  the SIMT issue loop. The bulk-only arm is **byte-identical** across
  `lsu_pending` 0/8/32 for every seed (138 380 at 16 MSHRs, 136 164 at 128).
* **The cap costs the SIMT arm little and pays the engine more:** capping at 8
  costs SIMT 3.3 % but makes the hybrid 5.0 % faster, by halving the SIMT client's
  mean pool occupancy (8.8 → 4.7 slots) and letting the engine take 14.7 % more.
  This is reallocation of a fixed shared budget, not more concurrency.
* **Peak occupancy exceeds the cap by exactly 3** (11 at cap 8, 35 at cap 32),
  because the cap is checked once per cycle before up to `warps_per_core`
  requests are admitted. Any reader of "peak inflight" needs this caveat.

Which number is "the" answer depends on a policy choice, not on the hardware:
Vortex's default is 8, the controlled baseline chose 0, and the two give 1.92×
and 1.77× for the same engine on the same trace.

## 4. How does Bulk performance scale with κ?

**The engine's absolute time is nearly flat in κ; SIMT's falls steeply; the ratio
therefore falls to parity.**

| κ | distinct lines | SIMT-only | hybrid (2×8K) | hybrid (2×16K) | A/C 2×8K | A/C 2×16K |
|---|---|---|---|---|---|---|
| 0 | 16 384 | 272 638 | 150 952 | 141 559 | 1.806 | 1.926 |
| 0.10 | 14 853 | 252 071 | 140 629 | 124 193 | 1.793 | 2.030 |
| 0.25 | 12 636 | 220 019 | 124 406 | 105 716 | 1.769 | 2.081 |
| 0.50 | 9 319 | 168 884 | 93 542 | 93 172 | 1.805 | 1.813 |
| 0.75 | 6 376 | 119 664 | 95 140 | 95 140 | 1.258 | 1.258 |
| 1.00 | 4 096 | 40 281 | 40 281 | 40 281 | 1.000 | 1.000 |

κ is not the explanation; the row-hit rate is:

| κ | row hit % SIMT | row hit % hybrid | dTLB misses SIMT | dTLB misses hybrid |
|---|---|---|---|---|
| 0 | 1.4 | 87.5 | 15 867 | 1 626 |
| 0.25 | 3.9 | 83.7 | 12 206 | 1 719 |
| 0.50 | 12.8 | 88.1 | 8 145 | 1 189 |
| 0.75 | 37.1 | 83.8 | 4 086 | 1 208 |
| 1.00 | 93.6 | 93.6 | 256 | 256 |

κ also changes the footprint (16 384 → 4 096 lines) and the duplicate count
(0 → 12 288), so any two of these four statistics can be used to tell the same
story. The report uses the row-hit and dTLB counters because they are the ones
that differ *between the arms at the same κ*, which is what a comparison needs.

**Two regimes.** At κ ≤ 0.25 the engine is faster on its own (1.59–1.81×)
and the hybrid tracks it, with overlap adding 3–11 %. At κ ≥ 0.50 the engine is
barely faster alone (1.02× at κ = .75) and almost all of the hybrid's 1.26× comes
from running both clients at once. Those are different reasons to want the
accelerator and they should not be reported as one number.

## 5. At what workload characteristics does Bulk begin to lose its advantage?

Four measured transitions, in order of how much they matter:

| condition | measurement | why |
|---|---|---|
| the stream already has row locality | κ = 1.00 → **1.000×**; κ = 0.75 → 1.258× | SIMT's row-hit rate reaches 93.6 %, matching the engine (93.6 %); there is no locality left to add |
| page ordering removed | κ = 0.25 → **0.991×**; κ = 0.50 → 1.013×; κ = 0.75 → 1.009× | row hits collapse 83.7 % → 13.1 %; the engine becomes strictly worse than SIMT |
| cohort scheduler reduced to one | κ = 0.25 → 1.114×; κ = 0.50 → 1.045×; κ = 0.75 → **0.758×** | bank utilisation 86.4 % → 54.4 % at unchanged locality; at κ = .75 the engine alone is 1.36× slower than SIMT |
| window shorter than the reuse distance | N = 65 536, κ = 0.25, 2×16K → **1.455×** (vs 2.133× at 2×32K) | the window cannot fold the second pass, so the dedup reach is lost |

And one boundary that is not a loss but a correct refusal: at κ = 1.00 the
dispatcher sends nothing to the engine (arm C is byte-identical to arm A). The
engine on its own would be 5 % slower there (42 301 vs 40 281 cycles).

So Bulk does not lose on *large* workloads or *small* footprints as such. It
loses when its ordering advantage is absent (locality already present) or has been
switched off (no page order, one cohort), and when its window is too small to
span the reuse distance.

## 6. Does 2×16K consistently beat 2×8K?

**No — only where there is locality or dedup left to capture.**

| κ | 2×8K | 2×16K | difference |
|---|---|---|---|
| 0 | 150 952 | 141 559 | −6.2 % |
| 0.10 | 140 629 | 124 193 | −11.7 % |
| 0.25 | 124 406 | 105 716 | **−15.0 %** |
| 0.50 | 93 542 | 93 172 | −0.4 % |
| 0.75 | 95 140 | 95 140 | 0.0 % |
| 1.00 | 40 281 | 40 281 | 0.0 % |

Two independent reasons for the pattern:

1. **At κ ≤ 0.25 the bigger window gives the cohort scheduler more runs to keep
   open and the fanout more elements per fill**, so the engine approaches the
   DRAM service rate: the hybrid's own run improves 11–15 %.
2. **At κ ≥ 0.50 the hybrid is already at the engine's floor** (at κ = .75 the
   2×16K hybrid is within 1 % of its own bulk-only arm, 95 140 vs 95 900), so
   window size has nothing left to buy.

**2×32K is degenerate at N = 16 384** and the matrix shows it rather than assuming
it: all five seeds at both MSHR settings are cycle-identical to 2×16K, because one
16 384-entry window already holds the whole stream. When the stream is longer
than the window the axis becomes a **dedup-reach** axis, not a capacity axis:

| N = 65 536, κ = 0.25 | hybrid | A/C | lines issued | dedup merges |
|---|---|---|---|---|
| 2×16K | 442 070 | 1.4549 | 49 187 | 32 |
| 2×32K | 301 502 | **2.1333** | 32 747 | 16 472 |

That 47 % gain is 16 472 duplicate accesses folded because the window finally
spans the reuse distance. Calling it "capacity" would be wrong.

## 7. How much of the advantage comes from page-aware scheduling and cohorts?

**All of it at low κ, and it is the only thing keeping the accelerator alive at
high κ.**

| κ | full | page order off | cohorts 1 | scheduler off |
|---|---|---|---|---|
| 0.25 | **1.7686** | 0.9909 | 1.1143 | 1.1143 |
| 0.50 | **1.8054** | 1.0127 | 1.0449 | 1.0449 |
| 0.75 | **1.2578** | 1.0091 | **0.7584** | 0.7584 |

Decomposition, from the counters:

| mechanism | effect | counter |
|---|---|---|
| **page order** | row-hit rate 83.7 % vs 13.1 %, dTLB misses 1 707 vs 12 053, at nearly unchanged bank utilisation (86.4 vs 74.8) | locality |
| **cohort scheduler** | bank utilisation 86.4 % vs 54.4 % at *identical* row-hit rate (83.7 % both) | concurrency across page runs |

They are separable and additive in the wrong direction: removing both is worse
than removing either (0.991× with ordering off, 0.758× with cohorts off at
κ = .75). The default of 2 cohorts is within 1.7–2.3 % of the best tested (4),
so the published default is retained.

The single most important number in this milestone: **the same engine spans
0.758× to 1.848× depending only on which existing switches are on.** A speedup
claim that does not name the scheduling configuration is under-specified.

## 8. Can an enlarged conventional L2 reproduce the benefit?

**No.**

Giving the SIMT baseline the accelerator's measured window storage
(901 824 B, i.e. 917 504 B of extra L2 = 30 720 lines) makes it **0.57 %** faster
and moves the speedup from 1.7686 to **1.7770**. At 2.5 MiB the reserve is fully
saturated (evictions 0) and the result is identical.

The reason is measured, not assumed: at that capacity the SIMT arm issues
**12 559 DRAM reads against 12 636 distinct lines** — 99.4 % compulsory misses.
The engine issues **6.1 % more** DRAM reads than the SIMT arm (13 329 against
12 559, from index and descriptor traffic) and finishes in 56 % of the cycles.
The accelerator does not avoid misses; it orders them.

At κ = 0.50 and 0.75 the extra capacity changes the ratio by less than 0.005, in
the direction that slightly favours the hybrid. Capacity is the right resource
for a footprint-bound workload (M15 measured 28.8 % for SIMT from 8 192 → 32 768
lines at κ = .75, N = 65 536); it is not the resource that explains this win.

## 9. Which architecture has the strongest performance?

**Neither, unconditionally. The measured ordering is:**

| condition | strongest | margin |
|---|---|---|
| κ ≤ 0.25, page-aware scheduling on | **Bulk hybrid** | 1.75–2.11× over SIMT-only, 2.20× with the Vortex-default LSU cap at 2×16K |
| κ ≤ 0.25, page ordering off | **SIMT** | hybrid is 0.991×, i.e. 1 % slower |
| κ = 0.50 | **Bulk hybrid** | 1.79–1.81×, but only 1.01× with ordering off |
| κ = 0.75 | **Bulk hybrid** | 1.24–1.26×, and **SIMT wins 1.32×** if cohorts are reduced to one |
| κ = 1.00 | **SIMT** | parity by dispatch; engine alone is 5 % slower |
| scattered stream with a long reuse distance (N = 65 536, κ = .25) | **Bulk hybrid**, but window-size dependent | 2.13× at 2×32K vs 1.45× at 2×16K |

The honest summary: **Bulk is the stronger architecture exactly where the
conventional front-end's request order is poor and the Bulk window is long enough
to reorder it, and it is a net loss everywhere else.** That is a conditional
statement about ordering and window reach, and no amount of MSHRs, LSU pending
slots or L2 capacity changes which side of it a workload falls on, because none of
those is an ordering mechanism.

## 10. Limitations

**Model and scope**

* One core, four warps, one trace at a time. The residency pool is per-chip in
  this model, so the sharing effects in §3 would sharpen with more cores but are
  not measured there.
* W1 geometry only (Λ = 16 384, P = 512, λ = 32). M8's map shows page density
  moves SIMT by up to 3.9× at fixed κ, so this is one slice of the space.
* κ and ρ are not crossed; κ > 0 with ρ > 0 remains untested (M15/M16's DUP
  workload is the nearest proxy).
* No silicon, RTL or synthesis claim. All numbers are the existing C++ model.

**Measurement**

* Five paired seeds per point; SD is 0.7–1.5 % of the mean, so differences below
  ~2 % are reported as "no change" rather than as effects.
* Row-hit rate is a locality proxy: the DRAM charges fixed 14/26-cycle timings
  and ignores queue occupancy, so the ordering advantage is a consequence of
  counted conflicts, not of a modelled controller's quality.
* "Mean hierarchy inflight" counts residency slots held, including requests
  waiting to enter the MSHR array; it overstates useful concurrency by ~5.5× at
  the published configuration and is never used as the explanation for a cycle
  difference.
* dTLB misses and dTLB stall cycles disagree by 7× under reordering (12 053
  misses, but flat 174 k stall cycles), because most misses hit the 512-entry L2
  TLB. Neither counter alone is a cost.

**Integrity**

* 490 cells, **0 gate failures**, 0 value-hash mismatches, `addr_mismatch = 0`,
  `dispatch_unaccounted = 0` everywhere.
* Historical anchors reproduce: 270 753 (κ = 0), 220 123 / 138 924 / 125 581,
  220 123 / 107 146 / 106 456 — all exact.
* `cache_diag = 0` and `= 1` produce identical cycles (220 123 / 138 924 /
  125 581 verified at the primary point).
* Regression suite: 4 470 checks, 0 failures.
* Two harness defects found and fixed during the run, both recorded here rather
  than hidden: the matrix script aborted on an unexpanded variable at P4 (fixed,
  and P4–P6 re-run from a resume script; no cell was silently dropped), and the
  first extractor version captured the seed in its grouping key so means were
  per-seed (fixed; the tables quoted in these reports are from the corrected
  extractor).

**Not checked**

* No replay of the M10–M16 anchors beyond the three quoted above.
* The 2×32K capacity axis was not exercised at intermediate N (between 16 384 and
  65 536), so the transition from "degenerate" to "dedup reach" is bracketed by
  two points, not traced.

## 11. Reproducibility

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests      # 4 470 checks, 0 failures
bash build/wm/m17_matrix.sh                            # P0-P3 (278 cells)
bash build/wm/m17_matrix_resume.sh                     # P4-P6 (212 cells)
python3 build/wm/m17_extract.py                        # -> build/wm/m17_tables.md
python3 build/wm/m17_plot.py                           # -> docs/m17-sweeps.svg
```

Raw outputs for all 490 cells are `build/wm/m17_<tag>.txt`; the manifest is
`build/wm/m17_matrix.log` (one line per cell: tag, exit code, gate, unaccounted
dispatches, cycles A/B/C).

| report | scope |
|---|---|
| `m17-original-reproduction-report.md` | Phase 0–1: reconciliation, configuration differences, exact reproduction |
| `m17-simt-concurrency-report.md` | Phase 2: LSU pending × L1 MSHRs |
| `m17-kappa-sensitivity-report.md` | Phase 3: κ, workload statistics, window capacity |
| `m17-scheduler-ablation-report.md` | Phase 4: page order and cohorts |
| `m17-iso-storage-report.md` | Phase 5: L2 capacity equivalence |
| this file | the nine questions, limitations |
