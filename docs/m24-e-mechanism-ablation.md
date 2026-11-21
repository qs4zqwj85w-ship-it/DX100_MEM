# M24-E — M24 Bulk architectural mechanism ablation

**Scope.** An ablation study of the production M24 Bulk architecture (M23-G6D S1:
fused 16K window, page-cohort admission, streaming release, 4096-entry ready pool,
physical-order organizer, tiled index acquisition). One mechanism is switched off or
re-sized per cell, everything else is byte-identical to the M24-D baseline, and the cost is
measured on the four established traces. No mechanism is added, retuned or reimplemented,
and no simulator source line changed.

Tags: **[M]** measured in this milestone · **[Q]** quoted from a prior milestone's
measurement · **[H]** hypothesis, explicitly not established by measurement.

## 1. Result

| mechanism | removing it costs (slowdown) | storage it occupies | what happens without it |
|---|---|---|---|
| **page-cohort admission** (A) | **1.210× – 3.861×** | 256 B | walkers fall from 65.7 % to 18.3 % busy, dTLB MSHRs from 5.31 to 1.48 of 8 |
| **ready pool, 4096 → 1** (B) | 5.530× – 8.021× | 65 536 B | pool is full and empty 97–99 % of cycles: translation and release ping-pong |
| **ready pool, 4096 → 16** (B) | 1.000× – 1.108× | 65 536 → 256 B | only Orkut and LiveJournal notice |
| **fused window + streaming pipeline** (D) | 1.000× – 1.112× | 475 KB vs 1 851 KB | the unfused M6–M9 window, 3.9× larger, is 8.5–11.2 % slower on two workloads |
| **index tiling** (E) | 1.000× – 1.020× | 2 720 B | index lines compete in the shared FIFO |
| **DRAM-aware release** (C) | **1.000× – 1.001×** | 65 KB + 0.2–0.3 M cycles | activations rise 0.03–1.48 %; **cycles do not move** |

Two mechanisms do essentially all of the work: cohort admission, and having *any* pool
deeper than one entry. Two are worth ~1–11 % and one — the one usually credited, the
physical-order organizer — is worth nothing on the production address map, because the
sealed page order is already the physical order (§6).

The most important number in this milestone is the last row. The entire G6B→G6I stack
(streaming translation/release decoupling, physical organization, cross-page pipeline) is
worth **8.5–11.2 % on two workloads and 0 % on the other two**, when compared with the
simpler M9-era unfused window on the same traces and the same resources (§7). It is not
worth 3.6×: the 3.6× in row 1 is what cohort admission is worth *inside* the streaming
pipeline that the later milestones added, and the pipeline exists because cohort admission
is what feeds it.

## 2. Evidence audit — what was reused, and why

| prior work | what it establishes | how it is used here |
|---|---|---|
| **M24-A** four-way page/DRAM evaluation | page factor = 55.7–73.7 % of runtime; DRAM factor = 0.00 % ± 0.01 % on 10 of 11 cells, −1.5 % only at map M3; organizer costs 205 938 cycles on Orkut | reused as the C-factor evidence and its explanation; **not re-run** (M3 sweep is its own 44-cell matrix) |
| **M21** mechanism ablation (2×8K, unfused, synthetic) | page *ordering* is the whole large win on w4 (2.994 → 1.001 without it); cohorts are worth 1.833 → 2.994 at width 1 → 2; four cohorts add 0.03 and can cost | reused as the ordering/dedup evidence, which **cannot be re-measured in the fused architecture** (§2.3) |
| **M22** area/power + storage accounting | per-entry widths; window SRAM is the second-largest term after L2 | reused for the entry widths and the predicted footprints in §7.2 |
| **M23-G6B** streaming decoupling | buffer backpressure ≤ 0.14 % of cycles, mean occupancy 1.1–33.8 of 4096 — the pool was never a bottleneck on the G6B traces | reused as the prior for §5; the sweep here contradicts it for shallow pools |
| **M23-G6C** admission deferral | P0-style deferral is what lets a refused page not stop the port | reused as the definition of mechanism A |
| **M23-G6H/G6I** physical organization | the organizer's own cost and its order | reused in §6.2 |
| **M24-C / M24-D** | the resource-matched baseline and its four workloads | the baseline cells of every table here are the M24-D `bulk` primaries, **not re-run** |

New in M24-E: 58 ablation cells on the M24-D traces. Nothing already measured was
re-measured; where a prior result could be re-derived from a switch that exists in the
production config, it was re-derived and quoted alongside.

### 2.3 What cannot be isolated, and why

Four hard constraints in `BulkEngine::BulkEngine` (`sim/src/bulk.cpp:328–371`). They are
not experimental choices; they are the architecture's own preconditions:

| constraint | consequence for this study |
|---|---|
| `win_fused` requires `dedup = 1` and `page_order = 1` | **dedup and page ordering cannot be ablated in the fused architecture at all.** Their contribution is measurable only in the unfused arm, which is why M21's numbers are quoted rather than replaced |
| `bulk_org` (2..8) requires `win_fused = 1` | the fused representation and the streaming/organization pipeline cannot be separated. Cell D is a **bundle** (§6.1) |
| `bulk_org = 8` requires `page_sched = 1` and `page_cohorts > 0` | the cohort ablation has to be `bulk_org = 7` (streaming release, single cursor). Cell A therefore measures the cohort *pipeline*, not a field |
| `m24_dram_org = 0` requires `bulk_org` 7 or 8 | the DRAM factor can only be ablated inside the streaming arms — which is exactly where it is defined |

Every one of these is reported as a limitation of the design (§11), not papered over.

## 3. Method

**Base.** `build/wm/m24d/m24d_bulk.cfg` — the production M24 Bulk with all 94 fields
pinned, unchanged. Every cell is that file plus `--set` overrides; nothing else moves, so
all cells share caches, MSHRs, walkers, `R = 128`, page size, DRAM timing and address map.

**Matrix.** 58 cells = 14 ablations × 4 workloads + 2 equivalence cells.

| stage | cells | overrides applied to the base |
|---|---|---|
| A cohorts | 4 | `bulk_org=7 page_sched=0 page_cohorts=0` |
| A width | 16 | `page_cohorts = 1, 2, 4, 16` |
| A equivalence | 2 | `bulk_org=7 page_sched=1 page_cohorts=8` (Orkut, ogbn) |
| B pool | 16 | `g6b_pa_capacity = 1, 16, 64, 512` |
| C DRAM order | 4 | `m24_dram_org=0` |
| D fused | 4 | `win_fused=0 bulk_org=0` |
| E index | 12 | `idx_mode = 0, 1, 3` |

62 cells in total with the four reused baselines. `build/wm/m24e_run.sh`, cell list in
`build/wm/m24e/cells.tsv`.

**Effect checks.** A cycle count alone cannot show that a switch took effect, so every cell
is checked against the model's own policy counters before its cycles are used:

| ablation | counter proof |
|---|---|
| cohorts off | `bulk_org_policy=7`, `bulk_sched_on=0`, `bulk_g6c_admit_accepted=0` |
| cohorts = N | `bulk_sched_cohorts=N`, `bulk_sched_on=1`, `bulk_g6c_admit_accepted>0` |
| pool = N | `bulk_g6b_pa_capacity=N`, `bulk_g6b_ready_peak ≤ N` |
| DRAM order off | `bulk_org_runs=0` (the organizer never runs), `bulk_org_policy=8` |
| unfused | `bulk_win_fused=0`, `bulk_org_policy=0`, `bulk_g6b_pa_capacity=0` |
| idx_mode = N | `bulk_idx_mode=N` |

## 4. A — page-cohort organization

### 4.1 The switch, and the equivalence that makes it clean

`bulk_org = 7` is the G6B streaming release with a single admission cursor; `bulk_org = 8`
is the same release driven by P0's cohort table. The two equivalence cells run `bulk_org=7`
with `page_sched=1 page_cohorts=8` — the fields set, the scheduler object constructed
(`bulk_sched_on=1`), and nothing reading it:

| workload | `bulk_org=7`, ps=0 coh=0 | `bulk_org=7`, ps=1 coh=8 | verdict |
|---|---:|---:|---|
| Orkut | 21 992 384 | 21 992 384 | **identical** |
| ogbn-products | 42 637 738 | 42 637 738 | **identical** |

So the cohort fields are inert once the admission policy is G6B's single cursor [M]. The
ablation is one mechanism, not two fields.

Both numbers also reproduce M24-A's own page-only and page × DRAM cells on this trace to the
cycle — 21 992 384 and 6 130 494 [Q] — which is a second, cross-milestone determinism check.

### 4.2 Width curve

Slowdown against the production width of 8. `walker%` is `walker_busy / (cycles × 8)`;
`dMSHR` is `tlb0_mshr_occ / cycles`, of 8.

| cohorts | Orkut | LiveJournal | ogbn | Criteo | walker % (Orkut / ogbn) | dMSHR (Orkut / ogbn) |
|---|---:|---:|---:|---:|---|---|
| off (org 7) | 3.587× | 3.861× | 1.210× | 1.236× | 18.3 / 81.5 | 1.48 / 6.59 |
| 1 | 3.589× | 3.864× | 1.211× | 1.233× | 18.3 / 81.5 | 1.48 / 6.58 |
| 2 | 1.886× | 2.050× | 1.039× | 1.034× | 34.8 / 94.9 | 2.81 / 7.67 |
| 4 | 1.115× | 1.226× | 1.002× | 0.999× | 58.9 / 98.4 | 4.76 / 7.95 |
| **8 (production)** | **1.000×** | **1.000×** | **1.000×** | **1.000×** | 65.7 / 98.6 | 5.31 / 7.97 |
| 16 | 0.999× | 0.999× | 1.000× | 0.999× | 65.6 / 98.6 | 5.30 / 7.97 |

Cycles, so that the curve can be read without the ratios:

| cohorts | Orkut | LiveJournal | ogbn | Criteo |
|---|---:|---:|---:|---:|
| off (org 7) | 21 992 384 | 29 188 644 | 42 637 738 | 4 038 325 |
| 1 | 22 000 192 | 29 210 738 | 42 689 686 | 4 028 043 |
| 2 | 11 564 709 | 15 493 333 | 36 638 651 | 3 377 379 |
| 4 | 6 833 481 | 9 266 206 | 35 327 627 | 3 265 040 |
| 8 | 6 130 494 | 7 558 966 | 35 248 957 | 3 267 387 |
| 16 | 6 126 199 | 7 552 675 | 35 243 341 | 3 262 824 |

**One cohort is the same as none.** Width 1 reproduces the single-cursor cycles to within
7 808 cycles (0.04 %) on Orkut and 51 948 (0.12 %) on ogbn. The mechanism is *overlap*, and
with one run open there is nothing to overlap with.

The number of cohorts progressing at once is `page_cohorts` by construction, and the
deferral table never holds more than that: `bulk_g6c_defer_depth_max` is 1, 2, 4, 8 and 16
at widths 1, 2, 4, 8 and 16 on all four workloads [M].

**The curve saturates at four.** Width 4 is within 0.2 % of width 8 on ogbn and Criteo, and
width 16 is marginally *better* than 8 on all four (−4 295, −6 291, −5 616, −4 563 cycles,
0.07–0.09 %) for twice the descriptor storage. The production default of 8 is on the flat
part of the curve; the knee is at 2–4.

### 4.3 What the resource changes say

The walk count does not move: 107 405 at width 8 versus 107 447 with cohorts off on Orkut
(+0.04 %). **Cohorts save no translation work — they remove serialisation.** What changes is
utilisation: walkers 18.3 % → 65.7 % busy and dTLB MSHRs 1.48 → 5.31 of 8 on Orkut,
81.5 % → 98.6 % and 6.59 → 7.97 on ogbn. This reproduces M24-A's page-factor diagnosis
exactly — its Orkut A/C cells report 18.3 % walker utilisation and 1.48 MSHRs, its B/D cells
65.7 % and 5.31 [Q].

### 4.4 Blocked pages

The refused-page behaviour is the mechanism in one counter. At width 8 the admission defers
and retries rather than stopping: 30 866 919 deferrals on Orkut with a deferral depth of 8
(one per cohort) and 138 539 deferred runs readmitted. With cohorts off, the same counter
set reports zero — the single cursor never defers, it waits. On ogbn the deferral count is
274 465 557 at width 8 and 71 996 572 at width 2: the deeper the cohort table, the more a
refused page is bypassed instead of blocking the port [M].

## 5. B — the shared ready pool

### 5.1 Curve

`g6b_pa_capacity` is the only coupling between translation and release: a translated run
waits in this pool until the DRAM scheduler drains it.

| pool | Orkut | LiveJournal | ogbn | Criteo | occupancy (Orkut / ogbn) | full % (Orkut / ogbn) |
|---|---:|---:|---:|---:|---|---|
| 1 | 5.530× | 7.642× | 8.021× | 7.463× | 0.0 / 0.0 | 97.5 / 99.6 |
| 16 | 1.077× | 1.108× | 1.000× | 1.004× | 5.9 / 0.0 | 59.5 / 0.2 |
| 64 | 1.068× | 1.102× | 1.000× | 1.005× | 33.7 / 0.0 | 40.8 / 0.1 |
| 512 | 1.035× | 1.063× | 1.000× | 1.005× | 322.6 / 0.5 | 33.2 / 0.0 |
| **4096 (production)** | **1.000×** | **1.000×** | **1.000×** | **1.000×** | 2 045.3 / 11.5 | 8.1 / 0.0 |

Cycles:

| pool | Orkut | LiveJournal | ogbn | Criteo |
|---|---:|---:|---:|---:|
| 1 | 33 900 681 | 57 762 053 | 282 733 095 | 24 385 529 |
| 16 | 6 605 559 | 8 373 648 | 35 249 069 | 3 280 171 |
| 64 | 6 549 636 | 8 326 656 | 35 248 741 | 3 283 318 |
| 512 | 6 346 277 | 8 033 136 | 35 248 276 | 3 284 879 |
| **4096** | **6 130 494** | **7 558 966** | **35 248 957** | **3 267 387** |

### 5.2 The one-entry collapse

At capacity 1 the pool is **full 97.5 % of cycles and empty 97.3 % of them** — both, because
every translation fills it and every release empties it. Mean occupancy is 0.0 and mean
residence falls to 0.1 cycles: no entry ever waits, because nothing can be admitted until
the previous one has been released. The cost is 5.53–8.02×, and it is not translation work
(walks do not change) but the loss of every overlap between the two phases.

That is the clearest positive statement this milestone can make about the pool: **its value
is decoupling, and one entry is enough to destroy it.**

The same collapse in the other three columns: mean residence 15 223.7 cycles (Orkut) → 0.1,
release throughput 0.1344 → 0.0243 runs per cycle, backpressure 8.1 % → 97.5 % of cycles.
At 16 entries the pool is still backpressured 59.5 % of the time on Orkut and 35.2 % on
LiveJournal; at 512 it is 33.2 % and 15.4 %, with a mean residence of 2 485.8 and 2 715.8
cycles — the backlog is real work in flight, not overhead, which is why shrinking the pool
costs cycles there and costs nothing on ogbn.

### 5.3 Where 4096 is, and is not, over-provisioned

On ogbn-products and Criteo a 16-entry pool is within 0.4 % of the production 4096
(35 249 069 vs 35 248 957; 3 280 171 vs 3 267 387) — those runs are translation-bound
(97.2 % / 87.1 % empty) and never need depth. On Orkut and LiveJournal the production pool
runs full 8.1 % / 1.7 % of cycles at a mean occupancy of 2 045 / 934 entries, and shrinking
it to 512 entries—an 8× cut, to 8 KB—costs 3.5 % / 6.3 %. So the 64 KB pool could be 8×
smaller at a 3.5–6.3 % cost on the two DRAM-bound workloads, and 256× smaller at no cost on
the other two; its depth is what buys the last few per cent on Orkut and LiveJournal [M].

This is a *correction* to the prior: M23-G6B reported the pool was never a bottleneck
(backpressure ≤ 0.14 %, occupancy 1.1–33.8 of 4096). That remains true at 4096; it is not
true below ~512, and this sweep is the first to go below 4096.

## 6. C — DRAM-aware release organization

### 6.1 The switch, and how controlled it is

`m24_dram_org = 0` removes exactly the physical `(row, bank, line)` ordering of the release
and issues the sealed order instead. Same window, same cohorts, same pool, same map, same
DRAM timing, same queue. The target *population* is identical; the order in which it is
released is not, so a small number of L2 outcomes differ:

| workload | reads base | reads `m24_dram_org=0` | delta |
|---|---:|---:|---|
| Orkut | 644 142 | 644 155 | +13 (+0.002 %) |
| LiveJournal | 692 695 | 692 542 | −153 (−0.02 %) |
| ogbn | 1 367 453 | 1 367 594 | +141 (+0.01 %) |
| Criteo | 104 615 | 104 504 | −111 (−0.11 %) |

### 6.2 Result: it acts, and it does not pay

| workload | cycles base | cycles off | change | activations base → off | row hit | organizer cycles |
|---|---:|---:|---:|---|---:|---:|
| Orkut | 6 130 494 | 6 130 484 | **−10** | 254 249 → 254 332 (+0.03 %) | 60.5 → 60.5 % | 805 876 vs 599 938 |
| LiveJournal | 7 558 966 | 7 557 624 | **−1 342** | 372 338 → 372 693 (+0.10 %) | 46.2 → 46.2 % | 782 510 vs 599 882 |
| ogbn | 35 248 957 | 35 247 530 | **−1 427** | 1 217 711 → 1 218 647 (+0.08 %) | 11.0 → 10.9 % | 896 065 vs 599 503 |
| Criteo | 3 267 387 | 3 269 238 | **+1 851** | 70 438 → 71 499 (+1.48 %) | 32.7 → 31.6 % | 174 508 vs 147 456 |

Removing the organizer is **faster** on three workloads and slower on one. It costs
205 938 / 182 628 / 296 562 / 27 052 cycles of organizer work (the difference between the
two `bulk_org_cycles` columns) and removes 0.03–1.48 % of the activations; the row-hit rate
does not move except on Criteo, where it *falls* by 1.1 points when the organizer is on — the
one workload where ordering actively hurts the hit rate while still helping activations.

The net effect on cycles is between −1 851 and +1 427, i.e. **under 0.06 % of the runtime,
in both directions** [M].

The rest of the DRAM columns move the same way — the organizer neither fills the queue nor
shortens the wait:

| workload | bank busy % base → off | mean queue depth base → off | wait per read base → off |
|---|---|---|---|
| Orkut | 98.4 → 98.4 | 60.31 → 60.31 | 574.0 → 574.0 |
| LiveJournal | 93.7 → 93.7 | 53.31 → 53.33 | 581.7 → 582.0 |
| ogbn | 47.9 → 47.9 | 3.57 → 3.59 | 92.0 → 92.5 |
| Criteo | 35.3 → 35.5 | 5.59 → 5.66 | 174.6 → 177.2 |

### 6.3 Why — the same reason M24-A found

M24-A established this on its own matrix [Q], and the explanation is structural: under the
production map a 4 KiB page is 64 lines and a row is 32 lines, so page `p` owns rows `2p`
and `2p+1` and no physical row is fed by two pages (`bulk_g6i_xpage_cells_max = 0`). Ascending
line — which is what the sealed page order already is — is ascending row. Sorting by
physical rank reproduces an order the sealed array already had:

| workload | release pairs going backwards, base | same, organizer off |
|---|---:|---:|
| Orkut | 0.43 % | 0.06 % |
| LiveJournal | 2.83 % | 0.71 % |
| ogbn | 4.10 % | 3.52 % |
| Criteo | 5.89 % | 5.07 % |

Even where the two orders differ by 3–6 % of pairs, re-sorting them changes nothing,
because the differences are translation stragglers, not locality. The one configuration
where the organizer pays is not on this map: M24-A measured **−1.5 % (92 156 cycles)** at map
M3, where a permuted physical map lets two pages share a row and the organizer has a
cross-page group to build [Q].

**Negative result, stated plainly: on the production address map the DRAM-aware release
contributes 0 % of the 1.27–2.92× resource-matched advantage M24-D measured, and it is not
free — it costs 0.2–0.3 M cycles of organizer work per run.**

## 7. D — fused window representation

### 7.1 What the ablation does, and the confound it carries

`win_fused = 0` forces `bulk_org = 0` (§2.3): the fused representation and the whole
G6B/G6C/G6H pipeline are one switch. So this cell compares

* **fused** — 8 B per line-run + 4 B meta + 2 B flags, streaming release, cohort admission,
  physical organizer;
* **unfused** — 16 B per element + 32 B per line + 4 B permutation + 4 B order (M22's
  *Build U*, the representation every M18–M21 published number used), sealed-order issue
  with the M7 cohort *scheduler* but no G6C admission and no organizer.

It is therefore a **bundle**, not a representation-only ablation, and the report does not
pretend otherwise. What makes the bundle interpretable is that its other components are
measured separately in this milestone and are ~0: C is 1.000× (§6), and the pipeline's own
overlap is what the fused arm has and the unfused arm does not.

### 7.2 Storage [M]

Live window storage over the run (`bulk_win_vec_bytes`, peak of the two buffers — the
simulator's own sum of vector capacities, not a formula):

| workload | fused | unfused | ratio |
|---|---:|---:|---:|
| Orkut | 475 136 B | 1 851 392 B | **3.90×** |
| LiveJournal | 474 994 B | 1 851 108 B | **3.90×** |
| ogbn | 474 828 B | 1 850 800 B | **3.90×** |
| Criteo | 368 910 B | 1 245 724 B | **3.38×** |

The measured figures match M22's Build U / Build F accounting [Q] to within the 240 B of
`std::vector` capacity rounding M22 already documented: predicted 1 851 632 B unfused and
475 376 B fused at 2×16K, measured 1 851 392 B and 475 136 B. M22's per-buffer table gives
467 064 B versus 123 000 B at 2×8K — **3.80×** — and 925 816 B versus 237 688 B at 2×16K.
`bulk_win_bytes` (279 040 B) is the M6–M13 design *charge* and is identical in both arms by
construction, so it is not the number to use here.

### 7.3 Performance

| workload | fused cycles | unfused cycles | ratio | L2 reads fused → unfused | mean issue span |
|---|---:|---:|---:|---|---|
| Orkut | 6 130 494 | 6 653 100 | 1.085× | 1 011 218 → 1 076 350 (+6.4 %) | 81 111 → 89 144 (+9.9 %) |
| LiveJournal | 7 558 966 | 8 404 988 | 1.112× | 1 042 562 → 1 130 675 (+8.5 %) | 101 341 → 113 163 (+11.7 %) |
| ogbn | 35 248 957 | 35 248 112 | 1.000× | 3 104 956 → 3 103 838 (−0.04 %) | 469 712 → 475 905 (+1.3 %) |
| Criteo | 3 267 387 | 3 265 234 | 0.999× | 304 815 → 306 046 (+0.4 %) | 180 611 → 180 336 (−0.2 %) |

The fused arm is 8.5–11.2 % faster on Orkut and LiveJournal, identical on ogbn, and 0.07 %
*slower* on Criteo. The two workloads that pay are the two that are DRAM-saturated in the
fused arm (98.4 % and 93.7 % bank busy) and whose target line set is 6.4 MiB and 13 MiB
against a 1 MiB L2.

**Mechanism [H].** The unfused arm issues the same population over a 10–12 % longer span and
generates 6.4–8.5 % more L2 read traffic. The unverified hypothesis is that the longer span
lets lines fall out of L2 between the seal and their use, so some of the requests that hit
in the fused arm miss in the unfused one. The counters are consistent with it (the extra
traffic is L2 reads, not DRAM reads: +0.8 % and +0.4 %) but they do not prove it, and the
opposite is equally consistent on ogbn, where the same span increase costs nothing. This is
labelled a hypothesis and is not used in the ranking.

### 7.4 What this does to the headline

The whole G6B→G6I stack is worth 8.5–11.2 % on Orkut and LiveJournal, 0 % on ogbn and
−0.07 % on Criteo, *relative to the M9-era unfused window*, at 3.4–3.9× the window storage.
The 3.6× that mechanism A shows in §4 is not the same claim: it is the value of cohort
admission **conditional on** the streaming pipeline existing. Remove the pipeline (this
cell) and the same traces run within 11 % without cohort admission at all.

So the correct reading of the M23 line of work is:

* cohort admission is indispensable *to the G6B/G6C streaming pipeline* (3.6× there);
* the streaming pipeline is worth ~10 % over the unfused window it replaced, on the two
  workloads where the write stream is DRAM-bound;
* and it costs 3.4–3.9× the window SRAM plus the ready pool, the organizer and the
  descriptor table.

That is a defensible trade, but it is not the trade the M23 reports implied.

## 8. E — index-stream consolidation

### 8.1 Modes

`idx_mode` selects how the index array is acquired: 0 = the M6/M7 single implicit stream
(values looked up in a completed-line map, element order, competing in the shared FIFO);
1 = two bounded tiles with explicit ownership; 2 = 1 + per-line readiness (production);
3 = 2 + a dedicated index issue port.

| workload | idx_mode 0 | 1 | **2 (production)** | 3 |
|---|---:|---:|---:|---:|
| Orkut | 1.005× | 1.000× | **1.000×** | 1.000× |
| LiveJournal | 1.004× | 0.999× | **1.000×** | 1.000× |
| ogbn | 1.000× | 1.000× | **1.000×** | 1.000× |
| Criteo | 1.020× | 1.007× | **1.000×** | 1.000× |

### 8.2 Result: 0.0–2.0 %, and the dedicated port is non-binding

Turning the tiled acquisition off costs 0.0–2.0 %, worst on Criteo (3 331 245 vs 3 267 387).
Mode 3 is **identical to mode 2 to the cycle on all four workloads** — the dedicated index
port is non-binding at `idx_mshr = 64` and `R = 128`, so it adds a port and changes nothing
measurable. Mode 1 is within 0.1 % of production except on Criteo (0.7 % worse).

The counters say what each mode does rather than how fast it is. Out-of-order index
generation is 1 014 964 on Orkut in mode 2 and in mode 3, and 0 in modes 0 and 1 (no
generation counter to keep). Index wait-stall cycles: 383 229 (mode 0), 755 842 (mode 1),
822 306 (mode 2 and 3).

The reason the tiling matters so little is visible in the stall columns: with no tiling the
index wait is *lower* (383 229 vs 822 306 cycles on Orkut) because there is no tile boundary
to wait for — the cost appears instead as deeper FIFO interference with the target stream.
The two arrangements move the same work around rather than removing it [M].

### 8.3 Faithful accounting of the index stream

The engine does **not** eliminate index traffic, and this report does not claim it does:

| workload | index lines read by the engine | engine DRAM reads | index share |
|---|---:|---:|---:|
| Orkut | 74 993 | 644 142 | 11.6 % |
| LiveJournal | 74 986 | 692 695 | 10.8 % |
| ogbn | 74 938 | 1 367 453 | 5.5 % |
| Criteo | 18 432 | 104 615 | 17.6 % |

Those reads are inside the engine's `dram_reads` and are charged to it. The SIMT arm reads
the same index array as part of its own trace and its index reads are inside *its*
`dram_reads`; the counters do not separate them, and this report does not subtract them from
either side. M24-D's resource-matched comparison is therefore the accounting to use, and it
puts the engine **14.5–24.0 % above SIMT in total DRAM reads**, index array included.

**Negative result, stated plainly: index-stream consolidation is worth 0.0–2.0 %. What the
engine buys is not cheaper index traffic but a request stream with one request per line — the
fold — which is a different mechanism, is not separable from the fused window, and shows up
in M24-C/M24-D as 44 % fewer L2 reads than SIMT (1 011 218 against 1 797 063 on Orkut) and
24.0 % more DRAM reads (644 142 against 519 309).**

## 9. Contribution table

Ranked by measured cycle impact in the production configuration, with the storage each
mechanism occupies at the production settings and its sensitivity across the four workloads.

| rank | mechanism | impact if removed [M] | storage [M] | sensitivity | verdict |
|---|---|---|---|---|---|
| 1 | **page-cohort admission** | 1.210× – 3.861× | 256 B (8 × 32 B) | extreme: 3.86× on LJ, 1.21× on ogbn | indispensable *to the streaming pipeline* |
| 2 | **ready pool, depth 1** | 5.530× – 8.021× | 16 B | uniform: ≥ 5.5× everywhere | a degenerate configuration; proves the pool's purpose |
| 3 | **fused window + pipeline** | 1.000× – 1.112× | 475 KB vs 1 851 KB | two of four workloads | 3.9× storage win, ~10 % cycles on the DRAM-bound pair |
| 4 | **ready pool, depth 16** | 1.000× – 1.108× | 256 B | concentrated: orkut/LJ only | 4096 is 4× over-provisioned on 2 of 4, ~256× on the others |
| 5 | **index tiling** | 1.000× – 1.020× | 2 720 B | criteo only | nearly free either way; mode 3 non-binding |
| 6 | **DRAM-aware release** | 1.000× – 1.001× | 65 KB + 0.2–0.3 M cycles | Criteo only, and *negative* | **0 % on the production map**; −1.5 % at map M3 [Q] |
| — | dedup, page ordering | 0.0–1.99× [Q, M21] | in the representation | not measurable in the fused architecture | M21 measured them on the unfused 2×8K arm |

Ranks 3 and 4 are a tie and are ordered by mean slowdown across the four workloads
(1.049 for the fused window, 1.047 for the 16-entry pool); neither ordering is meaningful.
The gap between rank 1 and everything else is not close: no other mechanism exceeds 1.11×.

Storage context: the production engine's declared structures on Orkut are 279 040 B window
charge + 475 136 B live window vector + 65 536 B pool + 256 B cohorts + 2 720 B index tiles =
**822 688 B**, against a 1 MiB L2 (16 384 × 64 B lines) and a 16 KiB L1 (256 × 64 B).

## 10. Interactions

| pair | evidence | reading |
|---|---|---|
| A × C | M24-A's four-way matrix [Q]: page-only 55.7–73.7 %, DRAM-only 0.00 ± 0.01 %, interaction −92 156 cycles at map M3 only | the organizer has nothing to order until cohorts give it a pool of translated runs *and* the map gives it a shared row |
| A × D | this milestone: cohorts off costs 3.587× inside the pipeline (§4), but the pipeline-free unfused arm is only 1.085× behind the same configuration (§7) | A's value is conditional on the streaming architecture; it is not a property of the trace |
| B × A | pool = 1 costs 5.5–8.0× with cohorts on; cohorts off costs 3.6–3.9× with the pool at 4096 | both attack the same overlap from opposite ends and neither substitutes for the other: with no pool there is nothing to admit into, with no cohorts there is nothing to put in it |
| D × C | the unfused arm has no organizer at all, and is still within 11 % | the organizer is not what makes the pipeline worth having |
| E × D | idx_mode 0 costs 0.0–2.0 % in the fused arm (§8), and the fused/unfused bundle it sits inside is worth up to 11.2 % (§7) | E is smaller than the difference between the two representations, so these cells cannot separate them |

## 11. Validation

* **62 cells** — 58 ablations + 4 reused baselines — pass `tools/m24e_extract.py`:
  `conservation_ok = 1` and a completion hash in every cell; `accesses = trace_accesses`;
  `pte_fetches = 2 × walks`; dTLB/L1/L2 `hits + misses = reads`;
  `row hits + conflicts + 2 = dram_reads`; `activations = conflicts + 2`;
  `dram_reads = dram_arrivals`; `bulk_elems_done = bulk_elems_total`;
  `bulk_tgt_req_gen = done = issued`; `bulk_win_sealed = bulk_batches`;
  `bulk_peak_slot ≤ 128`; `bulk_residency_violations = 0`; `bulk_g6b_ready_peak ≤ capacity`;
  index lines done = requested = submitted in every cell except the unfused arm, where
  `idx_mode` is unused by construction.
* **Every ablation is proved to have taken effect** by the policy counters in §3, not
  inferred from the cycles.
* **Determinism.** Three cells re-run: `coh2/orkut` 11 564 709, `unfused/ogbn` 35 248 112,
  `pool16/criteo` 3 280 171 — identical cycles and completion hashes.
* **No cell hit the cycle guard** and none is truncated; the slowest is `pool1/ogbn` at
  282 733 095 (57 % of the 500 M guard).
* **The production architecture was not modified.** `sim/src` mtimes all precede this
  milestone; `./bin/dx100_tests` reports 4 924 checks, 0 failures; the baseline cells are the
  M24-D primaries, unchanged.

## 12. Limitations

1. **D is a bundle, not a representation ablation** (§7.1). `bulk_org` requires
   `win_fused = 1`, so no cell can hold the pipeline and swap the layout. The storage half
   of D's result is clean; the cycle half is a bundle and is labelled as one.
2. **dedup and page ordering cannot be ablated at all** in the fused architecture
   (`win_fused` requires both). M21's unfused measurements are quoted and not replaced.
3. **A is conditional on the pipeline.** Cohort admission is only definable inside
   `bulk_org = 8`; the 3.6× is the value of the mechanism *given* the streaming release,
   not a general property.
4. **C's negative result is map-specific.** M24-A found −1.5 % at map M3; that result is
   quoted, not re-run, and no claim is made that the organizer is useless in general.
5. **The pool sweep is the first below 4096**, so it is not cross-checked against any prior
   measurement; the 1-entry collapse is a new observation.
6. **One trace per workload, one seed, one map** (map 0) for every cell here. M24-D showed
   the *resource-matched* result is map-invariant to 1.7 %; whether each mechanism's
   contribution is equally map-invariant is not measured, except for C (M24-A covers 4 maps).
7. **Not run:** no `sched_vis` (M23-G6F candidate visibility) ablation, no G6A/G6H/G6I policy
   variants, no HBM (`dram_kind = ddr`), and no synthetic pattern sweep — M21 already covers
   the synthetic axis and re-running it would not test a production mechanism.

## 13. Conclusions

1. **Two mechanisms carry the architecture: cohort admission and the ready pool's depth.**
   Cohort admission is worth 1.21–3.86×, pool depth 1.00–1.11× in the useful range and
   5.5–8.0× if collapsed to a single entry.
2. **The DRAM-aware organizer contributes 0 % on the production map** — −1 851 to +1 427
   cycles across four workloads, while costing 0.2–0.3 M cycles of organizer work and
   removing only 0.03–1.48 % of activations. Its documented value is at a permuted map
   (M24-A: −1.5 % at M3), where a physical row can be shared by two pages.
3. **The fused representation is a storage optimization first and a performance
   optimization second**: 3.4–3.9× less live window storage, 8.5–11.2 % faster on the two
   DRAM-bound workloads, 0 % on the other two.
4. **Index consolidation is worth 0.0–2.0 %**, and mode 3 (dedicated index port) is
   measurably identical to mode 2 — the port never binds.
5. **The pipeline's headline is smaller than it looks.** Compared with the unfused M9-era
   window on identical resources and traces, the entire G6B→G6I stack is worth 8.5–11.2 % on
   two workloads and nothing on the other two, for 3.9× the window storage. The 3.6–3.9×
   figures belong to cohort admission *inside* that stack.
6. **What this means for the architecture:** if the goal is minimum storage for the same
   throughput, the pool can shrink to 512 entries (8 KB, costing 3.5–6.3 % on two workloads
   and nothing on the others) and the organizer can be removed at no cost on map 0. If the goal is
   the last 10 % on DRAM-bound gathers, cohort admission and the streaming release are what
   buy it, and the fused layout is what makes them affordable in SRAM.

## Reproduce

```
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests      # 4 924 checks, 0 failures
cd DX100_MEM && PAR=5 ./build/wm/m24e_run.sh all       # 58 cells, skips finished ones
python3 tools/m24e_extract.py                          # summary.csv, ablate.txt, ALL CHECKS PASSED
python3 tools/m24e_verify_doc.py                       # every figure in this file, recomputed
```

Artifacts: `build/wm/m24e/` (58 cell logs, 3 repeat logs, `summary.csv`, `ablate.txt`,
`cells.tsv` — cell → config → overrides — and `commands.txt`, the exact command line of
every cell, from `./build/wm/m24e_run.sh cmds`), `build/wm/m24e_run.sh`,
`tools/m24e_extract.py`, `tools/m24e_verify_doc.py`. Baselines are the four M24-D `bulk`
cells in `build/wm/m24d/`, reused unchanged; the base configuration is that milestone's
`m24d_bulk.cfg` and is not copied, so there is no second copy of it to drift.
