# M25 — Proposed architecture and experiment plan

**Status: proposal only. Nothing in this document has been implemented, and no cell of its experiment
plan has been run.** It is written from the three M25 audits (`m25-simt-bulk-efficiency-audit.md`,
`m25-dram-organization-audit.md`, `m25-adaptive-selection-design.md`) and the M25 literature review.
It is submitted for architectural review before any source change.

**The direction the brief asks for, restated against the evidence:**

* *SIMT stays first-class* — it already is, mechanically (§1 of the efficiency audit); what is
  missing is a selector, not a front-end.
* *A compiler sends unsuitable work to SIMT* — worth **+32.8 %** over always-SIMT and **+1.24 %**
  over always-Bulk on the 31-trace matrix, at 0.33 % regret (§3).
* *Bulk keeps only what pays* — the cohort admission (×3.3 for 96 B), the fused window and the
  dedup stay; the DRAM organizer (114 688 B, ≈0 on the production map) does not.
* *Redundant DRAM sorting goes only where the mapping evidence supports it* — §2.2 and the
  mapping-aware rule in the DRAM audit §6.

---

## 1. Ranked changes

Ranking criteria, in the brief's order: (1) expected performance, (2) storage saved, (3) logic and
critical-path simplification, (4) energy, (5) implementation risk, (6) evidence strength. The
ranking below is by *expected value per unit of risk*, and the column that decides each rank is
named.

| # | change | perf | storage | logic | energy | risk | evidence |
|---|---|---|---|---|---|---|---|
| **1** | **Selector v1**: a compiler pass producing the class map + the three-gate policy of the selection design, with SIMT as the default on any missing input | **+32.8 %** vs always-SIMT, **+1.24 %** vs always-Bulk, 0.33 % regret on 31 traces | 0 (one bit per instruction, not in the engine) | +1 pass, no hardware | follows the perf | **medium** (needs a real compiler; the model's metadata must be replaced, not extended) | **strong** for the policy, **none** for the compiler |
| **2** | **Drop `ford`/`fpos` by default** (organizer off; keep the switch for mapping-flagged deployments) | 0.0002 % on the production map; loses 1.5 % (Orkut M3) and 3.7 % (`criteo_2k` at production geometry) where it pays | **−114 688 B packed (16 % of the engine)** | removes the 16 384-entry sort at seal and the `fpos` inverse build | removes the sort's dynamic power; no leakage model | **low** (one knob, already validated as inert) | **strong** (M24-A + 6 new cells + G6F + G3 ideal) |
| **3** | **Engine targets allocate in L1** (remove `bypass_l1` on target requests only; index/PTE stay bypassed) | [H] recovers an unknown share of the 8–16 % SpMV losses and 2–4 % sequential losses; bounded above by the SIMT arm's L1 hit rate (5.4–37.8 %) | 0 (uses the existing L1) | one bit; but it puts engine traffic in the same L1 the SIMT front-end uses, so the two classes contend | likely positive (fewer L2/DRAM reads) | **medium** (changes the hybrid contention picture; M9's gates would need re-running) | **hypothesis** — no measurement exists |
| **4** | **Pool depth 4 096 → 512** (and a 256 point in the sweep) | M24-G: 16 entries already capture 90–100 % of the depth's value; 4 096 is 8–256× oversized | **−57 344 B packed** (3 584 × 16 B) | smaller heap (9 → 6 sift levels) | positive | **medium** (the pool is also the translation/DRAM distance buffer; the 4 096 peak is reached on 5 of 31 M24-F traces) | **medium** (M24-G, one sweep) |
| **5** | **Liveness guarantee**: if the pool is full and every cohort is parked, force-release the oldest translated run (or force-admit one translation) | 0 (or −ε) | 0 | +a small arbiter and a state bit | 0 | **low to implement, high if skipped** | **strong that the defect exists** (two independent triggers, §6.5 of the audit); none for the fix |
| **6** | **Per-client DRAM attribution** (`cc0` split into fill/return vs everything else) | 0 | 0 | 0 | 0 | **very low** | n/a — it is an instrument |

**Why #2 is not #1 despite saving 16 % of the engine:** it is a *cost* reduction with a measured
performance floor, and it does not make Bulk win anywhere it currently loses. #1 is the only change
that alters the outcome on traces the architecture currently mishandles.

**Why #3 is not #1 despite being free storage-wise:** it is a hypothesis. The efficiency audit shows
the L1 is *a* mechanism (§3, §5.1) but cannot quantify its share, because Bulk's `cc0` stream is
unattributed and SIMT's index loads are untagged. Running #3 before #6 would produce a number nobody
could attribute.

---

## 2. What each change means architecturally

### 2.1 Selector v1 (change #1)

**Compiler side** (the part that does not exist and must be built):

1. An IR pass that finds a loop whose memory operand is `base + load(idx[k]) * width` with `idx`
   affine in the induction variable — the only static classifier (gate G0).
2. Legality: no store to the target region inside the loop; no cross-iteration dependence; the index
   array not written by the same loop. Any failure → SIMT (this is DX100's condition set, and it is
   stricter and better justified than M24's single regularity bit).
3. Tiling to `batch_window` with one descriptor per tile, `elem_base` advanced — M24's
   `build_bulk_workload` writes exactly one descriptor for the whole trace, which is a model
   simplification, not an architecture.
4. The three-gate estimate (pages, sequential fraction, index share) from profile data when
   available, from the trip count and the index array's footprint otherwise; **any missing input →
   SIMT**.
5. Emit the class map and the descriptors; the simulator's `op_class="gen"` path must be *replaced*
   by a real metadata input, not extended.

**Hardware side:** unchanged. `run_hybrid` already takes a class map, shares `R`, and ticks once.

**Region boundary:** the decision is per loop nest. A mid-region switch is **out of scope** until a
drain path exists (§2.5).

### 2.2 Organizer (change #2)

Three options were weighed; the proposal is the third.

| option | storage | logic | perf |
|---|---|---|---|
| keep `ford`/`fpos` everywhere (status quo) | 114 688 B | 16 K sort + inverse | 0 on production, +1.5–3.7 % on mapped/scrambled |
| remove outright | 0 | 0 | loses those 1.5–3.7 % |
| **keep the switch, default off, present only in the flag-enabled build** | 114 688 B *in that build* | same | same |

The switch already exists (`m24_dram_org`), is validated as inert for org 0/4/6 and for all
historical cells, and the audit gives the integrator a three-row rule to set it (DRAM audit §6). What
changes is the **default and the ownership**: it becomes an instance-time configuration of the memory
map, not an always-on pipeline stage.

### 2.3 L1 for engine targets (change #3)

`win_emit_fused` sets `r.bypass_l1 = true` on every target request. Dropping that bit for targets
(and only targets — index and PTE requests must stay out of L1 or they evict the data) makes the
engine a normal L1 client. Consequences to measure, not assume:

* the L1 is per core and the engine submits on core 0, so in a hybrid the engine would contend with
  core 0's SIMT stream — the shared-`R` invariant still holds but the *hit* behaviour changes;
* dedup already removes intra-window duplicates, so the L1's value is **cross-window** reuse
  exactly where the audit says Bulk loses (`n3` 2.63 target fetches per distinct line, `n1` 1.0);
* the engine's fill path would install lines it never reads again (a 64 B line is read once *per
  window*), so allocation policy matters: **no-allocate on the last use** is not expressible, so the
  experiment must compare `bypass_l1 = 0` against `bypass_l1 = 0` + a per-window "last use" hint.

### 2.4 Pool depth (change #4)

`g6b_pa_capacity = 4096` costs 65 536 B as allocated (4 096 × 16 B) / 57 344 B packed. M24-G's
knee is at 8–16 entries for *performance*, but the pool is also what decouples translation from
release: with 512 entries the engine can still hold 512 translated runs, which is 32× the residency
budget `R = 128` and 8× the measured mean occupancy on the traces where it matters (3 574 on `n1`,
3 210 on `n5`, 1 412 on `dubcova3`). 512 is the smallest depth that never binds on the traces whose
mean occupancy is below it; `n1` and `n5` would bind, which is why they are in the sweep.

### 2.5 Liveness (change #5)

Two independent triggers, both on `criteo_2k`: `page_cohorts = 1` (M24-G) and the map-3 page
permutation (M24-D). Both end with a full pool and no admissible release. The proposal is the
smallest state change that makes forward progress structural:

* on a cycle where the pool is full, no release was possible, and every open cohort is parked,
  release the oldest *translated* run regardless of cohort order (one entry), or force-admit one
  translation for the parked cohort with the oldest `resume_at`.

This is a policy change to `win_issue_g6b`, not a new structure, and it must be validated by the
existing `dx100_tests` plus a new test that reproduces both triggers.

### 2.6 Instrumentation (change #6)

Split `cc0` in a Bulk run. Until that exists, 62–74 % of the engine's DRAM reads on the SpMV traces
cannot be attributed, and change #3 cannot be evaluated cleanly.

---

## 3. Controlled experiment plan

Every experiment below is a **separate milestone**; none is authorised by this document. Common
environment: the M24-D production pair (`build/wm/m24d/m24d_{bulk,simt}.cfg`), `R = 128`,
`batch_window = 16 384`, `bulk_org = 8`, `g6b_pa_capacity = 4096`, `page_cohorts = 8`,
`m24_dram_org = 1`, one seed, `max_cycles = 500 000 000`, and the M24-F 31-trace manifest
(`build/wm/m24f/traces.tsv`) so that every cell is comparable to the published matrix.

### E1 — out-of-sample selector (validates change #1's policy, not its compiler)

| | |
|---|---|
| cells | the 31 M24-F traces × 2 arms (Bulk, SIMT) = 62, but **reused**: the matrix exists |
| new work | freeze the three thresholds on the 12 M21/M18 traces only; evaluate on the 19 traces never used to choose them; report accuracy and regret per held-out group |
| config | unchanged; the policy is applied offline to `diag.csv` |
| metrics | per-trace chosen mode, regret, aggregate regret, the 9 "win ≥ 10 %" and 4 "lose ≥ 5 %" counts, seed-2 excluded |
| gate | held-out regret ≤ 1 % and no held-out trace with a ≥ 10 % loss selected as Bulk |
| cost | 0 simulator cells |

### E2 — engine L1 allocation (change #3)

| | |
|---|---|
| cells | 8 traces (`qc2534`, `bcsstk25`, `dubcova3`, `n3_clu4m`, `n1_seq8m`, `n5_seq8m_s4`, `w4_duplow`, `criteo_2k`) × 3 arms (`bypass_l1 = 1` control, `bypass_l1 = 0`, `bypass_l1 = 0` + last-use hint) = 24 |
| config | production pair; the new knob `eng_l1_alloc ∈ {0,1,2}` |
| metrics | cycles, `l1_reads/hits/evict` per client, L2 and DRAM reads per client, row hits, activations, walks, `peak_total_outstanding ≤ R` every cycle, conservation, `addr_mismatch = 0` |
| gate | conservation and the shared-`R` invariant must hold in **every** cell; the all-SIMT hybrid must still equal the pure SIMT run to the cycle (M9 G1) after the change |
| risk | the engine's L1 allocations can evict a hybrid's SIMT stream; the gate must include the `dispatch = 1` mixed-class cells at κ = 0.25/0.5 |

### E3 — pool depth with the liveness fix (change #4 + #5)

| | |
|---|---|
| cells | `g6b_pa_capacity ∈ {128, 256, 512, 1024, 4096}` × 6 traces (`criteo_2k`, `orkut_hub`, `n1_seq8m`, `n5_seq8m_s4`, `n3_clu4m`, `w4_duplow`) = 30 |
| prerequisite | the liveness fix must land first, or `criteo_2k` cells will hit the guard and the sweep will be unreadable |
| metrics | cycles, `bulk_g6b_full_cycles`, `bulk_g6b_release_slot_stall`, ready-pool mean/peak, conservation, `bulk_desc_completed = 1` |
| gate | every cell completes below 100 M cycles; the depth chosen must not raise cycles on any trace by > 2 % |
| storage claim | 512 saves 57 344 B packed; report the measured cost, not the modelled one |

### E4 — organizer, mapping, queue depth (change #2)

| | |
|---|---|
| cells | `m24_dram_org ∈ {0,1}` × 5 mappings (M0, M1, M2, M3, **4 KiB-row geometry**) × 3 traces (`orkut`, `livejournal`/`criteo_2k`, `ogbn`) × `dram_q ∈ {0, 32}` = 60 |
| why `dram_q` | every cell to date runs `dram_q = 0`, so the controller's reorder window is whatever the 64 L2 MSHRs admit (~60). The organizer's value must *rise* as that window shrinks; if it does not, the removal is unconditional and stronger |
| metrics | cycles, row hits, row activations, bank busy, `bulk_org_runs`/`org_cycles`, `m24_rel_inversions`/`rel_pairs`, L2/DRAM reads per client, requests per activation |
| gate | the chosen default must be within 0.5 % of the better of the two orders on M0 and must not lose > 2 % on M3 or the 4 KiB-row geometry |
| cost | 60 cells, ~40 min at the M24-A throughput |

### E5 — region boundary and switching cost (needed before any hybrid deployment)

| | |
|---|---|
| cells | one mixed workload per trace family, each split into 2–8 regions with a class boundary between them; `dispatch = 1`, `dispatch_order ∈ {0,1}` |
| new counters | start-up (submission → first element) and drain (last element → idle) **per region**, not per run |
| metrics | total cycles vs the same regions run entirely SIMT and entirely Bulk; the sum of per-region start-ups; whether `peak_total_outstanding` stays ≤ R across a boundary |
| gate | a boundary must cost ≤ 5 % of the region's cycles, or the selector must not place one there |
| blocker | requires the drain path of §2.5 plus a region concept the model does not have |

### Validation requirements common to all five

1. `sim/bin/dx100_tests` must pass with **no reduction** in check count (4 924 today), and every
   new mechanism adds its own invariant test.
2. Every pure-path anchor must reproduce to the cycle: the 8 M5.1 anchors, the 4 M24-D primaries,
   and M9's G1/G2 gates.
3. `conservation_ok = 1`, `addr_mismatch = 0`, `bulk_residency_violations = 0`,
   `peak_total_outstanding ≤ R` in every cell.
4. A liveness test per experiment: at least one cell at `page_cohorts = 1` and one at map-3, both on
   `criteo_2k`, must terminate.
5. Every report states which numbers are measured, which are derived and which are hypotheses, and
   which checks did not run.

---

## 4. What this milestone deliberately did not do

* No source file was modified; `sim/src` is byte-identical to the end of M24-H.
* No historical artifact was touched: M24-D/E/F/H logs, configs and documents are read-only inputs.
* No Vortex build, no Vortex RTL change, no full re-simulation of any published matrix.
* No architecture was redesigned. Changes #1–#6 are proposals awaiting review; the only work
  executed here is the audit and six mapping cells (E4's 4 KiB-row point), which are new
  measurements of the *existing* architecture at a geometry the matrix did not contain.

---

## 5. The one-sentence proposal

**Keep conventional SIMT as the default, put a real compiler-driven selector in front of the engine
with a conservative three-gate rule, let the engine allocate in L1 and stop sorting its window by
DRAM row, shrink its pool to 512 entries, and fix the two liveness edges — in that order, each
behind its own gate, none of it before review.**
