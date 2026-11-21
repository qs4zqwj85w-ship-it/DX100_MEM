# DX100 ↔ Current Engine: Architecture Gap Analysis (post-M8)

**Scope.** Investigation and documentation only. No simulator source, configuration or test was
modified for this report. Every number cited is either an already-published measurement from
`m4`, `m5.1`, `m6-w0`, `m6-w1`, `m7` or `m8` reports, or a structure read directly from
`DX100_MEM/sim/src`. Where a number is derived rather than measured it is marked *(derived)*;
where a claim is a hypothesis it is marked *(hypothesis)*.

**DX100 reference.** Khadem, Kamalakkannan, Zhu, Poptani, Gu, Dominguez-Trujillo, Talati, Fujiki,
Mahlke, Shipman, Das. *DX100: A Programmable Data Access Accelerator for Indirection.* ISCA 2025
(52nd), arXiv:2505.23073v2, DOI 10.1145/3695053.3731015. Section numbers refer to that version.

**Our engine.** Standalone C++17 simulator, `Mode::Bulk` path, M4 baseline through M8. Ports:
`R = 128` outstanding line requests, L1 256 lines/4-way/16 MSHR, L2 16 384 lines/8-way/64 MSHR,
dTLB 16 entries × 4-way with 4 MSHRs and 2 parked, L2 TLB 512 entries × 4-way with 8 MSHRs,
DRAM 2 banks × 4 096 rows × 1 KiB rows, `t_hit = 14`, `t_miss = 26`, `link = 0`, `dram_q = 0`
(unbounded queue, FR-FCFS) — all from `configs/ab_controlled.cfg`.

**Headline result of the comparison.** We are not missing DX100's headline mechanism. Index-driven
address generation, per-element readiness, bounded outstanding tracking, within-window
deduplication and response fanout all exist and are measured. What DX100 has that we do not is a
**scratchpad-centred tile abstraction with a word-granular request/reconstruction memory**, and
what we have that DX100 does not is **real translation-aware scheduling** (M7) — DX100 removes
that problem by assumption (huge pages, 256 preloaded PTEs, §3.6). The remaining measured gaps are
narrow and mostly about *capacity and arbitration*, not about missing units.

---

## 0. Method and evidence rule

A mechanism is classified **implemented** only if I read the structure that performs it in
`sim/src` and there is a measurement showing it working. A counter that exists without a
mechanism behind it is not evidence (the M8 work found exactly one such artefact in the other
direction: the M6/M7 index buffer was a `std::map` with no capacity charge, i.e. an *unaccounted*
mechanism — `m8-pipelined-index-acquisition-report.md` §3).

Classifications:

| Label | Meaning |
|---|---|
| **1 — Implemented** | The structure exists, is exercised by default, and a measurement confirms its effect. |
| **2 — Partial** | It exists at a different granularity, for a subset of cases, or its effect is delivered by a different mechanism. |
| **3 — Missing, valuable** | Absent, and there is measured evidence that its absence costs something. |
| **4 — Not appropriate** | Absent by design; copying it would fight our architecture (see §2). |

---

## 1. Mechanism-by-mechanism comparison

### 1.1 Summary table

| # | DX100 mechanism (ISCA'25 §) | Ours | Class | Evidence / reason |
|---|---|---|---|---|
| 1 | Stream Access Unit, regular index acquisition (§3.3) | M8 `IdxRing` + `idx_stream()` | **2 Partial** | We stream index *lines* into a bounded descriptor ring with per-line readiness; DX100 streams *words* into a scratchpad tile with per-element finish bits. Ours is address-typed, not data-typed. |
| 2 | Indirect Access Unit, target address generation (§3.2) | `generate()` → `win_admit()` | **1 Implemented** | One target address per element, index sourced from memory. Measured 16 384 addresses on W1 with `addr_mismatch = 0`. |
| 3 | Fill / Request / Response phases (§3.2 stages 1–3) | `win_admit` / `win_emit` / `on_target` + fanout | **2 Partial** | All three exist and are pipelined, but the phases are *window*-scoped and fused into one state machine; DX100's Response stage reconstructs words into a destination tile, ours routes values to `val_[]` and per-line fanout. |
| 4 | Scratchpad, tiles, tile ready bits (§3.5) | window `Batch` (8 192 entries) + `dir_mask` + `val_[]` | **2 Partial** | We have one big working set per window and no tile abstraction; no software-visible scratchpad, no `TD/TS1/TS2/TC` operand roles. |
| 5 | Per-element readiness / finish bits (§3.5) | `IdxRing.ready/cnt/used`, `LineReq`, `elems_done` | **1 Implemented** | M8: 12 064 elements generated out of element order on W1 with byte-identical output; unready slots never consumed (test group `m8_index_acquisition`). |
| 6 | Request Table, outstanding tracking (§3.3, 128 entries) | `Pending` + `free_` (R=128), `unissued_`, `idx_mshr`, L1/L2 MSHRs | **1 Implemented** | Same depth (128) as DX100; `idx_mshr` (M8) is a sub-bound inside R, not additive. `residency()` includes `idx_q_`. |
| 7 | Row Table (BCAM per bank, 64 rows) + Word Table linked list (§3.2) | page directory `dir_vpn`/`dir_mask` + `LineReq` chains | **2 Partial** | We group by 4 KiB page (= 2 DRAM rows exactly, M6 §3.2) and dedup by a 64-bit line mask; there is no `(CH,RA,BG,BA,RO,CO)` table and no word-level linked list because a line never needs word reconstruction for a gather. |
| 8 | DRAM row/bank/channel scheduling, bank-group interleave (§3.2 stage 2, §3.6) | `dram_tick()` FR-FCFS, open-row first | **2 Partial** | The MC *is* FR-FCFS with an unbounded queue: W1 row hits 14 243 of ~17 400 accesses (81.6 %). Missing: multiple channels, bank groups, and any upstream ordering by bank beyond page order. |
| 9 | Duplicate merging + response reconstruction (§3.2, Word Table) | `dir_mask` bit test + k-element fanout | **1 Implemented** | Eliminated 12 288 L1 MSHR joins on W0; exact (`dir_mask` is a 64-bit cover of a 64-line page). *Cross-window* dedup is missing (§3.5). |
| 10 | Translation: 256-entry TLB, huge pages, preloaded PTEs (§3.6) | dTLB 16×4 + 4 MSHR/2 parked, L2 TLB 512×4 + 8 MSHR, 4 KiB pages, **M7 translation-aware cohorts** | **1 Implemented (stronger)** | M7 uses translation state to *order* issue (1 536 run installs, cohorts 2 → 156 912 cy). DX100 has no equivalent; it assumes the problem away. |
| 11 | Range Fuser (fuse short range loops) + ALU unit (§3.4) | — | **3 Missing, low value for us** | `BulkDesc` has `P`, `kappa`, `count` — a closed-form irregularity, not a data-dependent range; no condition tile, no per-element ALU. No measured workload demands it (W0 is the closest and its loss is translation-driven, §3.2). |
| 12 | LLC integration, coherence, direct-DRAM bypass (§3.6) | L1 → L2 → DRAM, no bypass, no coherence model | **2 Partial / 4** | We have the hierarchy DX100 bypasses. Coherence is *not modelled at all* (gather is read-only). DX100's `H` bit (snoop at fill, route to LLC or DRAM at request) is a real mechanism we lack — and cannot use until we model coherence. |
| 13 | Operation dispatch: 8-instruction ISA, MMIO queue, Controller (§3, §3.5) | `Mode::Bulk` + one `BulkDesc` at a time | **2 Partial** | Descriptor has `count/idx_base/idx_bytes/P/kappa`; no opcode (load/store/RMW), no predicate, no queue of descriptors, no per-unit dispatch. Ordinary loads unaffected. |

### 1.2 Notes where the classification is arguable

**#1 — is the M8 ring a Stream Access Unit?** Structurally, yes: bounded outstanding index
requests (`idx_mshr`), out-of-order response handling, per-line destination mapping, independent
acquisition and consumption state, explicit arbitration with target traffic (`idx_reserve`).
What it deliberately does not do is *materialise the index words into a buffer*: the descriptor
records which line covers which elements, and `generate()` reads the line's bytes at consumption
time. That is a legitimate design point for a gather engine (a line's 16 indices are consumed
once, in order), but it means there is no index tile to hand to a second-level indirection. **DX100
gets `A[B[C[i]]]` for free from this; we would need the index ring to become a word buffer.** That
is the single most consequential structural difference in the whole comparison.

**#3/#4 — Fill/Request/Response without a scratchpad.** Our phases are:
`win_admit` (fill: index→address, insert into the page directory), `win_emit`/`win_issue`
(request: cohort-scoped, translation-gated), `on_target`+`fanout` (response: one fill retires k
elements into `val_[]`). DX100's Response stage exists because its result is a *packed tile in a
scratchpad*; ours is a value stream consumed by `val_[]`. Because we never pack, we never need a
Word Table linked list — the `LineReq` chain plays that role, keyed by line, not by word.

**#8 — the honest baseline question.** Our MC is FR-FCFS with `dram_q = 0`, i.e. **unbounded and
optimistic** relative to DX100's fixed 32-entry request buffer per channel (§5). Our row-hit rate
(81.6 % on W1 after M7/M8) is therefore not comparable to DX100's "7.6 column accesses per row":
theirs is a gain against a 32-entry window, ours is what an unbounded reorder window plus page
ordering already delivers. **Any future row-aware claim must be stated as "beyond FR-FCFS with an
unbounded queue", which is a much weaker claim than DX100's.**

**#12 — DX100's `H` bit has no analogue here and should not be copied.** DX100 snoops at fill time
and stores a per-column hit bit in the Row Table, then routes to LLC or straight to DRAM. We have
no coherence protocol to snoop, and our gather is read-only, so the `H` bit would always read
"miss" — it would be pure overhead. It becomes necessary only if stores/RMW appear.

---

## 2. Architectural differences and their implications

DX100 is a **shared, MMIO-programmed accelerator sitting next to the memory controllers, owning
exclusive write access to its regions**. We are a **GPU-side engine that shares the LSU's
translation, cache hierarchy and MSHR budget**. Nine consequences:

| Axis | DX100 | Ours | Consequence |
|---|---|---|---|
| **Cache coherence** | Coarse: exclusive write ownership of an indirect region (SWMR, §6.6), plus a per-line `V` bit on scratchpad. No fine-grained atomics. | Not modelled; gather is read-only, so no coherence events exist. | We cannot claim any store/RMW capability until a coherence story exists. Conversely DX100's exclusivity assumption is *unavailable* on a GPU sharing data with texture/raster/tensor clients — this is the argument that keeps our research distinct. |
| **Memory consistency** | Region lock during the ROI; cores do not modify arrays (§4.2 Legality). | Not modelled; the engine's writes are to `val_[]` outside the memory system. | Any future store path forces us to take a position on ordering vs SIMT warps. None of M4–M8 needed one. |
| **Translation ownership** | Engine owns a 256-entry TLB, preloaded by API, huge pages assumed. | Shared: the engine is one client (`r.core = 0`) of core 0's 16-entry dTLB with 4 MSHRs, 2 parked; L2 TLB shared, 512 entries. | **Our engine is 25× tighter on translation and we turned that into M7's win** (cohorts re-probe rather than replay). DX100's design would be *worse* here. |
| **SIMT interaction** | Cores hand work over by MMIO and poll a ready bit; the engine is out of the core's execution path. | Engine competes with warps for the same L1 ports, MSHRs and cram slots; `R = 128` is the whole machine's budget. | Any M8-style reservation (`idx_reserve = 8`) is a *tax on SIMT*, not just on the engine. Measured: still net positive (W1 156 912 → 156 419), but the accounting must always be stated this way. |
| **Request arbitration** | Controller dispatches to 4 units; Request Generator interleaves Row Table slices; MC buffer is 32/channel. | Single `unissued_` FIFO + per-kind sub-bounds; MC is FR-FCFS/unbounded; index-before-target order is mandatory (reversing it cost +43 445 cycles on W1). | Priority inversion is the recurring hazard, and it is a *scheduling* problem, not a bandwidth one. |
| **Scratchpad vs cache** | 2 MB scratchpad, 32 × 16K tiles, 4 ports; 3.57 mm² of 4.06 mm² total area (§6.5). Indirect data bypasses LLC entirely. | No scratchpad. Targets flow through L2 (16 384 lines) with a 64-entry MSHR pool. | We cannot reproduce the "preserve cache for core traffic" benefit (DX100's MPKI reduction) without a bypass path. That is a *hypothesis* worth one cheap experiment (§3.6), not a proven gap. |
| **Multiple descriptors** | Instruction queue; multiple pending DX100 instructions; Range Fuser output feeds Indirect unit. | Exactly one descriptor at a time (`Run` per simulation). | **Missing capability with plausible upside**: two independent gathers cannot overlap, so index acquisition of descriptor *n+1* cannot hide behind target drain of *n*. Never measured — no workload with two gathers exists in M4–M8. |
| **Independent streams** | SLD/SST streams are independent of ILD's indirect stream inside one instruction (index tile vs target tile). | Index and target are the only two streams, and M8 pipelines them. | We already have the *two-stream* property DX100 relies on; we lack multi-*instruction* concurrency. |
| **Completion / synchronisation** | Scratchpad ready bit per tile, polled by the core (`wait` API). | `desc_completed`, per-element `val_`, `elems_done`; no blocking primitive, no fence. | Adequate for a read-only gather; insufficient the moment stores or inter-warp ordering appear. |

**What must not be copied.** (a) The scratchpad: 2 MB of SRAM to fix a cache-pollution problem that
our L2 MSHR pool already addresses differently, at 88 % of DX100's area. (b) The `H` bit / snoop
path: no coherence to snoop. (c) Exclusive region ownership: not available on a GPU. (d) Huge-page
assumption: it deletes the mechanism that gave us M7's 1.49× over M6.

---

## 3. Where the next performance actually is

Measured baseline chain (cycles, W1 / W0):

| | W1 | W0 |
|---|---:|---:|
| A — SIMT | 270 753 | 30 573 |
| B — M5.1 FIFO bulk | 278 034 (+2.7 %) | 44 773 (−46.4 %) |
| M6 — page-directory window | 233 475 | 39 322 |
| M7 — translation-aware cohorts | 156 912 | 39 530 |
| M8 — pipelined index acquisition | 156 419 | 38 775 |

### 3.1 Ranked opportunities

| Rank | Opportunity | Severity (measured) | Potential | Cost / complexity | Regression risk |
|---|---|---|---|---|---|
| **1** | **W0's translation-driven slot churn and residual 28 % loss vs SIMT** | W0: `tlbref` 22 219, `slotstall` 22 612, index `wait` 5 210 (13.4 %), on a 38 774-cycle run. W0 is still *slower than SIMT* (38 775 vs 30 573). | Largest measured headroom in the programme: closing it would be the first configuration where bulk wins **both** workloads. | Medium — the dTLB refuses (`dtlb_parked = 2`) and requests are re-probed per cycle; a run-level park/retry counter already exists. | Low on W1 (translation path is separable); medium on A/B neutrality. |
| **2** | **Index-ring capacity policy** | E1: 202 755 vs 192 480 (**+5.34 %**, measured M8 §5.1); ring holds 128 of 2 048 lines. | Parity on E1; possibly small gains on long W1 (unmeasureable today, see §5). | Low — size the ring from `ceil(count/per)` against an SRAM budget. | Low, but SRAM grows 5.1 KB → 8–16 KB. |
| **3** | **Multi-descriptor / independent streams** | Unmeasured (no workload). Structural: one descriptor at a time. | *Hypothesis only.* Would let descriptor *n+1*'s index fill overlap descriptor *n*'s target drain — the same trick M8 played inside one descriptor. | Medium-high: queue, per-descriptor residency split, completion. | Medium: arbitration changes touch every mode. |
| **4** | **Row/bank-aware issue beyond page order** | W1 row hits 14 243 / 81.6 %; M6 §3.2 proves page order *is* row order in this map (rows per page = 2, cross-page sharing = 0). | Bounded: ceiling ≈ 1.05× on W1 *(derived: 81.6 % → 93.8 % of ~17 400 accesses × 12 cycles saved)*. | Low to try (sort `order` by `(bank,row)` instead of line), but **no multi-channel/bank-group model exists**, so the DX100-style interleave story cannot be tested. | Low. |
| **5** | **Index/target arbitration (dedicated port)** | M8: mode 3 == mode 2 at `idx_mshr = 64`; mode 3 much worse below (mshr 8 → 184 831). | **Zero measured value** unless the table is as deep as R/2. | Already built. | — |
| **6** | **Cross-window deduplication** | W1 is a permutation: nothing to dedup (M6 §1.1). W0 already dedups inside the window (`dir_mask`). | Only for repeated-index workloads, which we do not have. | Medium (needs a cache of recent lines per page set). | Medium. |
| **7** | **LLC bypass / cache-friendliness** | Theoretical. L2 MSHR = 64 < R = 128, so the L2 MSHR pool *may* cap the window. | Unknown; one cheap measurement (L2 MSHR sweep 64 → 128) would settle it. | Very low (config only). | None. |
| **8** | **Range Fuser / ALU / predicates** | No measured demand. | Would add `if (cond)` and fused ranges — the W0 `kappa` generator already models the range shape. | High (new units + ISA). | High. |

### 3.2 Measured vs theoretical, stated plainly

**Measured opportunities** (a number exists that would improve): ranks 1, 2, 4, 5.
**Theoretical** (no number, plausible mechanism): ranks 3, 6, 7, 8. Ranks 6 and 8 additionally have
*no workload that would exercise them* in the current generator, which makes them speculative.

**What is explicitly exhausted:** index acquisition pipelining (M8: −0.31 % W1, −1.91 % W0;
the free-index oracle bounds all remaining index time at 8 623 cycles = 5.5 % on W1), and
translation-aware scheduling (M7: 233 475 → 156 912, and cohorts 4 only reaches 154 807 = 1.3 %
further). Both were the right milestones and both are now near their ceilings.

---

## 4. Recommended future architecture

Coherent target, assembled only from mechanisms we have measured, plus the two structural
additions ranks 1–3 imply.

```
                     ┌──────────────────────── GPU front end (unchanged) ──────────────────────┐
                     │  SIMT warps ──► LSU ──► coalescer ──► L1 ──► L2 ──► MC                    │
                     └───────────────┬──────────────────────────────────────────▲──────────────┘
                                     │ descriptor (count, idx_base, idx_bytes,  │ fills
                                     │  P, kappa, mode, target_base)            │
                                     ▼                                          │
   ┌──────────────────────── DESCRIPTOR QUEUE (new, rank 3) ────────────────────┴──────────────┐
   │  N descriptors, admission control, per-descriptor residency share                          │
   └───────┬────────────────────────────────────────────────────────────────────────────────────┘
           ▼
   ┌── CONTROLLER ───────────────────────────────────────────────────────────────────────────────┐
   │  dispatch • completion • per-descriptor residency accounting (R split, idx_reserve)          │
   └───┬──────────────────────┬───────────────────────────┬──────────────────────────────────────┘
       ▼                      ▼                           ▼
 ┌───────────────┐   ┌────────────────────┐   ┌──────────────────────────────────────────────┐
 │ INDEX STREAM  │   │ ADDRESS GENERATOR  │   │ PAGE-DIRECTORY WINDOW (organizer)            │
 │ (M8, extend   │──►│ generate():        │──►│ dir_vpn/dir_mask: exact 64-line cover per    │
 │  to word      │   │ idx→addr, 1/elem   │   │ 4 KiB page; WinEnt chains; dedup by bit test │
 │  buffer for   │   │                    │   └───────────────┬──────────────────────────────┘
 │  multi-level) │   └────────────────────┘                   ▼
 └───────┬───────┘                                    ┌──────────────────────────┐
         │ index lines                                │ COHORT SCHEDULER (M7)    │
         ▼                                            │ runs opened per page;    │
   dTLB → L2 TLB → walker (SHARED with LSU)           │ issue the run translation│
         ▲                                            │ lets through first       │
         │ translate                                  └──────────┬───────────────┘
         └───────────────────────────────────────────────────────┘
                                     │ bounded target requests (R, minus index share)
                                     ▼
                          SHARED L1/L2/MSHR path ──► FR-FCFS MC ──► DRAM
                                     │
                                     ▼
                        RESPONSE FANOUT (one fill → k elements)
                                     │
                                     ▼
                          val[] / completion  →  consumer
```

### 4.1 Block ownership: shared with the LSU vs dedicated

| Block | Owner | Why |
|---|---|---|
| L1 data path, L2, MSHRs, MC (FR-FCFS) | **Shared** | The engine is a client of the same hierarchy; giving it private caches would delete the traffic it is meant to organise and make every measurement incomparable. |
| dTLB / L2 TLB / walker | **Shared** | M7's win comes precisely from *sharing* translation state with the scheduler. A private TLB (DX100's 256 entries) would need preloading and would lose that coupling. |
| Coalescer | **Shared** (for SIMT) / **dedicated bypass** | The engine's addresses are indirect; running them through the warp coalescer is what M5.1 showed to be redundant (`L4` in M6 §1). The window's `dir_mask` supersedes it. |
| Index stream + ring | **Dedicated** | Own outstanding-request sub-bound, own readiness state, no LSU involvement. |
| Address generator | **Dedicated** | Trivially cheap; nothing in the LSU computes `idx→addr`. |
| Page directory / organizer | **Dedicated** | 5.1 KB descriptor SRAM + `dir_mask`; must see the whole window. |
| Cohort scheduler | **Dedicated** | Its input is engine-private order, not cache state. |
| Descriptor queue (proposed) | **Dedicated** | Only needed for concurrency; shared with nothing. |
| Completion / scoreboard | **Shared primitive, engine-specific granularity** | A descriptor completes when its last element's value is delivered; element order is not observable to the consumer. |

### 4.2 What the architecture deliberately does not have

No scratchpad, no word tables, no per-word reconstruction, no coherence agent, no exclusive region
ownership, no huge-page requirement, no fixed-size MC request buffer. Each omission is either an
area win (DX100: scratchpad = 88 % of its area) or a consequence of our environment (coherence does
not exist to be snooped).

---

## 5. Ranked next milestones

Ordered by measured severity × confidence, not by ambition.

| Rank | Milestone | Evidence it is worth doing | Expected result | Cost | Risk |
|---|---|---|---|---|---|
| **M9** | **Translation-slot churn and W0's residual loss.** Audit the `dtlb_parked = 2` refusal path used by cohort re-probe; make the cohort scheduler park at the run level instead of re-probing per cycle. | W0: 22 219 `tlbref`, 22 612 `slotstall`, 13.4 % index wait, and W0 remains 26.8 % *slower* than SIMT. | **Hypothesis:** most of the 22 k refusals become one park per run; W0 target ≈ 1.15–1.25× vs A. | Medium | Low |
| **M9b** | **Index-ring capacity policy** (folded with M9 if small): size from `ceil(count/per)` under an 8–16 KB cap. | E1 +5.34 % measured regression, cause quantified (M8 §7). | Parity on E1; no change on W1/W0. | Low | Low |
| **M10** | **Descriptor queue and concurrent operations.** Two descriptors in flight, residency split, per-descriptor completion. | No measurement; structural gap (§1.1 #13, §2). | Unknown; the mechanism is the one that could overlap two gathers. | Medium-high | Medium |
| **M11** | **Row/bank-aware issue order after translation**, and a multi-channel/bank-group DRAM model to make it meaningful. | W1 row hits 81.6 %; ceiling ≈ 1.05×; DX100-style interleave is untestable with `banks = 2`, no channels. | ≤ 1.05× on W1; enables a defensible row story later. | Medium (model work first) | Low on cycles, high on comparability |
| **M12** | **Store/RMW + coherence story**, prerequisites for predicates/ALU. | None — no workload, no coherence model. | Would open DX100's `IST/IRMW` territory but requires exclusive regions, which a GPU cannot assume. | High | High |

**Explicitly not next:** Range Fuser/ALU (no demand), cross-window dedup (nothing to dedup on our
workloads), LLC bypass (one config experiment, not a milestone).

---

## 6. Risks and unresolved questions

1. **Our DRAM model is more optimistic than DX100's baseline.** `dram_q = 0` (unbounded), 2 banks,
   no channels, no bank groups, `t_hit/t_miss = 14/26` (1.86× vs DDR4's ~2.7×). Any row-scheduling
   claim measured here is a claim against an already-perfect reorder window. **Resolve before M11.**
2. **No coherence, no consistency, no stores.** Every statement about DX100's `H` bit, exclusive
   regions or RMW has no analogue in our model; we cannot measure their value, only argue it.
3. **`N ≥ 32 768` on W1 trips a pre-existing diagnostic invariant** ("the index array is not exactly
   4096 requests of B0"), identically in `idx_mode` 0 and 2. Long-W1 comparisons currently use
   W0-32k. This blocks the "longer workload" evidence for W1 and should be fixed before M10.
4. **No strided-index workload.** The generator varies `idx_bytes` (E1) but not `idx_stride`, so the
   ring's stride claim (M8 §3) is argued from the slot mapping, not measured.
5. **Single descriptor = no concurrency evidence.** M10's upside is entirely unmeasured; it could
   be zero if descriptor *n+1*'s index traffic simply competes with *n*'s targets for the same R.
6. **W0's 22 k dTLB refusals are not yet attributed.** `tlbref` counts probes refused because the
   matching MSHR is at `dtlb_parked`; whether that is one long park or many short ones changes
   M9's whole design. **Measure the distribution before implementing.**
7. **Cross-report consistency.** M7's published figures are pre-audit; the audit (M8 §9) moved W1
   cohorts 2 from 156 907 to 156 912 and cohorts 4 from 154 673 to 154 807. Any future milestone
   must compare against the corrected numbers and say so.
8. **DX100's own numbers are not reproducible here** — different core count, 2 DDR4 channels,
   51.2 GB/s, 4-core CPU. Our 1.73× (A → M8 on W1) and DX100's 2.9× headline are not comparable
   quantities; do not tabulate them side by side.

---

## 7. One-paragraph verdict

Across the thirteen mechanisms, one is fully implemented and *better than DX100* (translation-aware
scheduling), five are fully implemented at our granularity (address generation, per-element
readiness, outstanding tracking, within-window dedup/fanout, FR-FCFS row scheduling), five are
partial by design (stream unit as a descriptor ring, phases without a scratchpad, row/word tables
as a page directory, no coherence path, one descriptor), and two are missing with no measured
demand (Range Fuser/ALU), one of which (multi-descriptor concurrency) is the only structural
capability gap left. The measured work that remains is not architectural novelty — it is
**W0's translation-slot churn (22 219 refusals, still 26.8 % slower than SIMT)** and **index-ring
capacity (E1 +5.34 %)**. M8 closed the index-acquisition question; M7 closed the scheduling
question; the next honest milestone is making W0 actually win.
