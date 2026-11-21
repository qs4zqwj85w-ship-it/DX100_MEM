# M16 — Combined findings

**One paragraph.** Against the strongest conventional SIMT baseline this model can
express, the Bulk accelerator keeps a 17–19 % advantage on the cache-interference
workload and a 49 % advantage on the mixed one, so the advantage is not an MSHR
artefact. That advantage is, however, almost entirely **page-aware scheduling**:
removing page ordering and the cohort scheduler costs 28.7 % / 45.6 % / 207 % on
W1 / DUP / PSCR, while **deduplication is worth 0.0–0.5 %** because the duplicates
it folds are already L2 hits or MSHR joins. With both mechanisms off the
accelerator is *slower* than plain SIMT (0.93–1.00×), so request visibility alone
buys nothing measurable. And on the two workloads where the bottleneck is capacity
rather than organization, the same storage spent on L2 is the better buy: at
matched storage the enlarged-L2 baseline is 1.3 % faster on W1 and 1.14× faster
on DUP, while on PSCR — where the accelerator wins by 3.07× — 2.5× the L2 capacity
changes the runtime by 5 cycles in 2.5 million.

Everything below is measured in this milestone: **188 cells, 0 gate failures, 0
value-hash mismatches, 0 address mismatches**, 3 paired seeds per sweep point, and
all historical anchors reproduced exactly.

---

## 1. Baseline credibility — does Bulk still outperform a stronger SIMT baseline?

**Yes, by 17–19 % at κ = .75 and 49 % at κ = .25, after the baseline has been made
as strong as the model allows.**

| conventional baseline | κ = .75 (A/C) | κ = .25 (A/C) |
|---|---|---|
| published (L1 MSHR 16, L2 MSHR 64, R 128) | **1.2006** | **1.5208** |
| L1 MSHR 64 | 1.1689 | 1.4922 |
| L1 MSHR 128 (≡ 256, budget-bound) | 1.1711 | 1.4912 |
| all capacities scaled (L1 128, L2 512, R 1024) | **1.1919** | not run |
| L1 MSHR 128 + `lsu_pending = 8` (Vortex default) | **1.510** | not run |
| L1 MSHR 128 + `lsu_pending = 32` | **1.647** | not run |

Three qualifications, all measured:

* **The improvement from a stronger baseline is real but bounded.** 16 → 128 L1
  MSHRs takes SIMT-only from 294 127 to 279 215 cycles (−5.1 %) and trims Bulk's
  advantage by ~3 percentage points. 8× the residency budget (R 128 → 1024) takes
  SIMT-only another 1.4 % and leaves the ratio at 1.19. The published baseline is
  MSHR-starved, not bandwidth-starved.
* **L1 MSHRs ≥ 128 are inert** because the 128-slot residency budget becomes the
  bound (byte-identical cycles at 128 and 256). An "MSHR sweep" and an "MLP sweep"
  are different experiments above that point, and they are reported separately.
* **The largest single lever is not MLP at all.** A per-core outstanding-load cap
  (`lsu_pending`, an existing Vortex parameter set to 0 in the controlled config)
  makes the *hybrid* 10–27 % faster by stopping SIMT from occupying all 128 shared
  slots, while making SIMT-only slower. The accelerator's measured advantage
  therefore depends on how the shared budget is divided as much as on either
  front-end's capability — a resource-sharing property that the M5.1 baseline
  deliberately removed and that any future comparison should report explicitly.

---

## 2. Mechanism attribution — dedup, page-aware scheduling, request visibility

| mechanism | W1 κ=.75 | DUP | PSCR κ=0 | status |
|---|---|---|---|---|
| deduplication | **0.00 %** | **+0.53 %** | **0.00 %** | measured, negligible here |
| page ordering (`page_order`) | **+28.7 %** | **+45.6 %** | **+207 %** | measured, dominant |
| cohort scheduler (`page_sched`) | **+7.9 %** | **+23.7 %** | **+74.9 %** | measured, workload-dependent |
| both page-aware controls | +28.7 % | +45.6 % | +207 % | measured, sub-additive (§2.3) |
| request visibility alone (both off) | **0.933×** | **0.974×** | **0.998×** | measured: no benefit |

### 2.1 Deduplication is not where the gains are (on these traces)

DUP folds 10 442 duplicate elements (21 % of the engine's requests) and gains
0.53 %; the traffic counters show why — turning dedup off adds 10 442 L1/L2 probes
and **8 DRAM reads**. The duplicate line was already being fetched, so those probes
were always hits. Dedup saves *probe bandwidth*, and this machine is not
probe-limited. W1 (3 duplicates) and PSCR (0) cannot show a dedup effect at all.

This is a genuine negative result, and it is conditional rather than universal: a
trace whose duplicates are *demand misses* — separated in time, not covered by the
L2 — would show more. That trace is not in this milestone (see §5).

### 2.2 Page-aware scheduling is the whole advantage

Ordering is the larger half everywhere, and its signature is in the DRAM row-hit
rate (W1 89.2 % → 46.9 %, DUP 80.3 % → 20.7 %, PSCR 55.4 % → 5.7 %) plus, on PSCR,
page walks (16 380 → 50 872 when ordering is removed). The cohort scheduler's
signature is stalls instead: DUP's `slot stall` rises 210 937 → 376 012 when the
scheduler is removed, because a single issue cursor cannot skip a page that is
still being walked.

### 2.3 They are coupled, and the coupling is sub-additive

Removing ordering costs 70 204 cycles and removing cohorts 19 398 on W1 — but
removing both costs 70 378, not 89 602. Each mechanism is worth more when the
other is present, so **no isolated percentage is claimed for "page-aware
scheduling" as a single mechanism**; only the joint figure and the two marginals.

### 2.4 Request visibility alone measures zero

With both mechanisms off the accelerator is 0.2–6.7 % *slower* than plain SIMT on
the same trace. The concurrency and request visibility that Bulk provides on its
own — without ordering and without cohort scheduling — produce no measurable
advantage. This is the direct answer to the milestone's central question, on these
workloads.

---

## 3. Resource opportunity cost — would a larger conventional L2 do the same?

Storage accounting is measured, not estimated: the 2 × 8K window occupies
**933 856 B** (M6 layout; 245 744 B fused), so the iso-storage L2 points are
1.875 MiB (+896 KiB) for the M6 window and 1.25 MiB (+256 KiB) for the fused one.

| workload | hybrid @ 1 MiB | SIMT @ 1.875 MiB | SIMT @ 2 MiB | verdict at matched storage |
|---|---|---|---|---|
| W1 κ=.75 | 244 977 | **241 810** | 236 822 | L2 faster by 1.3 % |
| DUP | 308 354 | **270 620** | 262 192 | L2 faster by 1.139× (12.2 % fewer cycles) |
| PSCR | **2 485 737** | 7 619 639 | 7 619 639 | accelerator 3.07× faster |
| STRID | 34 952 | 34 952 | 34 952 | tie (nothing to cache) |
| STRIDmix | **43 021** | 52 800 | 52 800 | accelerator 1.227× |

* **Capacity substitutes for organization on irregular footprints, and wins.**
  W1 and DUP are capacity-limited; the L2 is the cheaper way to buy their reuse.
* **Capacity does nothing where the bottleneck is organization.** PSCR's runtime
  is flat to 2.5× L2 (5 cycles in 2.5 M) because its misses are compulsory and its
  cost is translation; there, only the window's page ordering and cohorts help.
* **Dedup and L2 are not substitutes.** Dedup captures intra-window duplicates
  worth 0.5 %; capacity captures cross-window reuse worth 1.62× on the same trace.
  Different reuse classes, different values.
* This is an opportunity-cost comparison, not a claim of physical equivalence. The
  window is register-like SRAM with different timing and no tags; the L2 number is
  data capacity at 8 ways, and the 1.25/1.875/2.5 MiB geometries are diagnostic,
  not a proposed configuration.

---

## 4. Architectural conclusion

**Demonstrated, measurable value**

1. **Page-aware issue ordering** (pages ascending, lines ascending within a page):
   +28.7 % / +45.6 % / +207 %, with the DRAM row-hit rate as its mechanism. This is
   the single most valuable mechanism in the design and the one that survives every
   ablation.
2. **The M7 cohort scheduler**: +7.9 % / +23.7 % / +74.9 % on top of ordering, by
   not stalling a whole window behind one page walk. Worth its two cohort records.

**Workload-dependent**

3. **Deduplication**: 0.0–0.5 % here. It is a probe-bandwidth saving whose value
   depends on duplicates being demand misses, which they are not in W1/DUP/PSCR
   under a 1 MiB L2. Keep the mechanism (it costs no extra storage — it *is* the
   sort) but do not attribute the accelerator's value to it, and do not spend area
   on a bigger fold.
4. **The window's capacity itself** (8K vs 32K): the M15 result stands and M16
   sharpens it — a 32K window's footprint equals the 1 MiB L2 and loses 7.5 % at
   κ=.75, while the same storage as L2 is a net win on capacity-limited traces.
5. **The hybrid's advantage under a shared budget** depends on the split. With a
   realistic per-core `lsu_pending`, the advantage is 1.51–1.65×; with the
   controlled baseline's uncapped SIMT, 1.18–1.20×. Both are true; neither is
   "the" number.

**Unproven / not supported by this evidence**

6. **"Request visibility/concurrency adds performance"** — measured at 0 with both
   mechanisms off. It is a necessary substrate, not a source of speedup.
7. **The fused window representation** buys storage (4.00×) and no cycles; M15
   proved it exact and M16 does not change that. It is a capacity decision, not a
   performance one.
8. **Cross-window reuse (M13)** was not re-opened here; nothing in M16 supports
   revisiting it, and nothing refutes it either.

**Recommendation for further development.** Continue work on translation- and
locality-oriented organization (ordering, cohorts, page-dir sizing), where this
model shows large, reproducible effects; stop attributing value to deduplication
without a trace that demonstrates it; and treat any iso-storage claim honestly —
on capacity-limited workloads a larger L2 is the better purchase in this model.

---

## 5. Limitations

**Simulator limits.**

* **One core, four warps, one shared 1 MiB L2 for every experiment.** The shared
  budget finding (§1) would only intensify with more cores; nothing here should be
  extrapolated to a multi-core GPU.
* **The memory system charges fixed `t_hit`/`t_miss` (14/26 cycles)** and ignores
  queue-occupancy effects, bank-level parallelism beyond the hash, and write
  traffic. The row-hit improvements Part B measures are therefore *not* priced: a
  real controller could convert them into more or less time than the model does.
* **MSHR and queue arrays are free.** A 256-entry L1 MSHR array or a 1024-slot
  residency pool is a CAM with area and access-time costs the model does not model;
  Part A's "more MLP does not help" conclusion is therefore about *demand*, not
  about whether such a baseline is buildable.
* **`lsu_pending` is a pure admission cap** with no bookkeeping cost, so §1's
  1.51–1.65× figures are an upper bound on what that lever buys in silicon.
* **No per-request latency histogram for the SIMT arm**, so "more MLP queues more
  requests without shortening latency" is inferred from cycle counts and mean
  inflight, not observed directly.
* **L2 associativity held at 8 ways** across a 1.0–2.5 MiB sweep, so a small part
  of each capacity step is a set-index change (§6 of the iso-storage report).

**Scope limits.**

* **Three workloads for the ablation** (W1 κ=.75, DUP κ=.25, PSCR κ=0) at one
  window size, one cohort count and one `park_retry`. The ablation does not cover
  κ = .50, N = 16 384, or 32K windows.
* **The dedup result is a negative result with a known missing case**: a trace with
  duplicates that are demand misses rather than hits. Until that is run, "dedup is
  worth ≈0" is conditional on this workload set.
* **2 × 16K was not re-run** (M14's claim was not revisited, by instruction).
* **Large-L2 geometries are diagnostic**, not a configuration proposal: the
  published L2 is 1 MiB and M16 does not change any default.

**What cannot yet be said.** Nothing in M16 supports a silicon performance claim
for either design, and nothing in M16 measures area, power or timing. The
statements above are model statements about cycle counts under one calibrated
configuration, replicated across seeds and identical traces.

---

## 6. Reproducibility

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests        # 4 470 checks, 0 failures
bash build/wm/m16_matrix.sh                             # 188 cells
python3 build/wm/m16_extract.py                         # all tables
python3 build/wm/m16_plot.py                            # the Part A figure
```

Reports: `m16-simt-mlp-sensitivity-report.md` (Part A), `m16-bulk-mechanism-ablation-report.md`
(Part B), `m16-iso-storage-l2-report.md` (Part C). Measured tables:
`sim/build/wm/m16_tables.md`. Raw cells: `sim/build/wm/m16_*.txt`, one per row.

**What M16 changed in the tree.** One diagnostic block in `src/ab.cpp` that prints
per-arm counters (`m16 <arm>_<key>=`) under the existing `cache_diag` flag, plus
the matrix/extractor/plot scripts under `sim/build/wm/`. No mechanism, policy,
default, workload, trace or RTL file was modified; cycle neutrality and all
historical anchors were re-verified afterwards.
