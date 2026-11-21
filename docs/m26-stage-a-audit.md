# M26 Stage A — source and configuration audit, and implementation plan

**Status: audit and plan only. No source file has been modified in this stage, no new cell has been
run, and no M24/M25 artifact has been touched.** This document is the input to architectural review;
Stage B does not begin until it is approved.

Sources of truth, in order: the current source (`sim/src/*`, 12 458 lines), the *effective*
configuration echoed by each run's own header line, then the M25 reports. Where a report and the
implementation disagree, this document says so and follows the implementation.

---

## 1. What was inspected

| object | size | role |
|---|---|---|
| `sim/src/sim.hpp` | 2 140 | `Config` (every experiment knob), `ClassMap`, `BulkDesc`, stats structs |
| `sim/src/bulk.cpp` | 4 398 | the engine: window seal/order, G6B/G6C streaming, ready pool, release, fanout, driver |
| `sim/src/memory.cpp` | 1 099 | dTLB / L2 TLB / walker, L1 / L2 / L2-MSHR, address map, `submit`/`tick` |
| `sim/src/ab.cpp` | 2 767 | the A/B/C harness: arm A (SIMT), arm B (Bulk), arm C (dispatched), gates |
| `sim/src/system.cpp` | 525 | the pure SIMT front-end and its conservation checks |
| `sim/src/workload.cpp` | 336 | trace construction and `build_op_classes` |
| `sim/tests/tests.cpp` | 3 713 | 4 924 unit checks (the regression suite) |
| `build/wm/m24d/{m24d_bulk,m24d_simt}.cfg` | 130 each | the pinned production pair (one template, one 8-line ARCHITECTURE block) |
| `build/wm/m24d/`, `build/wm/m24g/log/` | 51 + 220 logs | the measured terminal state of both non-terminating cells |

Every workload the brief names already exists as a built trace and appears in the M24-F manifest
(`build/wm/m24f/traces.tsv`, 31 rows, one row per trace with its `elem_bytes`, `index_base`,
`desc_line`, access count and lane width): `orkut_hub` (`m23_orkut_hub.bin`), `livejournal`
(`m23_livejournal_bfs.bin`), `ogbn_uniform` (`m23_ogbn_uniform.bin`), `criteo_2k`
(`m23_criteo_2k.bin`), `w4_duplow` (`m21_w4_duplow_s1_full.bin`), and the M24-F `new` group
`n1_seq8m`, `n3_clu4m`, plus `qc2534`/`lnsp3937` from the `spmv` group. No trace has to be
regenerated for M26.

---

## 2. P0 — the two liveness failures, measured

### 2.1 Terminal state of each trigger (from the existing logs, not re-run)

**Trigger 1 — `bulk_criteo_map3_s3` (M24-D §8.3, `m24_map = 3`, seed 3):**

| counter | value | reading |
|---|---|---|
| `cycles` | 500 000 000 | the guard |
| `issued` / `completed` | 225 654 / 225 653 | **exactly one request never retires** |
| `inflight_occ` (mean over 500 M cycles) | 558 598 366 | one request held for ~442 M of those cycles |
| `bulk_elems_done` / `bulk_elems` | 294 912 / 294 912 | the workload is finished |
| `bulk_desc_completed` | 1 | the descriptor retired normally |
| `bulk_batches` | 19 | every window was sealed |
| `bulk_g6b_tr_requests` / `_tr_completed` | 108 909 / 108 909 | every translation was delivered |
| `bulk_g6b_ready_peak` | 4 096 | the pool's *peak*, not its terminal state |
| `cc0_issue` / `cc0_done` | 98 312 / 98 311 | **the leaked request is untagged (client 0)** |
| `cc1`, `cc3` | 18 432/18 432, 108 910/108 910 | index and target streams are exact |
| `cc2_issue` / `cc2_done` | 108 909 / 0 | translations retire before the stage-2 diagnostic (§2.4) |

**Trigger 2 — `criteo_2k`, `page_cohorts = 1` (M24-G §8, `resolve.txt`):**

| counter | value | reading |
|---|---|---|
| `bulk_elems_done` / `bulk_elems` | 16 382 / 34 816 | **window 1 stalled 2 elements short** |
| `bulk_g6c_windows` | 1 | window 2 was never opened |
| `bulk_g6b_tr_requests` / `_tr_completed` | 6 122 / 6 120 | **two accepted probes never delivered** |
| `bulk_g6b_ready_peak` | **335** | the pool was 8 % full — *not* pinned |
| `bulk_g6c_admit_idle_cycles` | 499 985 362 | admission had nothing admissible |
| `g6c_refuse_parked` / `_full` | 349 080 / 263 | refusals were the parked-park limit, not the pool |
| `issued` / `completed` | 21 875 / 21 873 | two requests never retire |

### 2.2 What this rules out

* **The ready pool is not the cause and not even consistently full.** Trigger 2 completes 47 % of its
  elements with a 335-entry (8 %) pool. Trigger 1 has a complete workload, a complete descriptor and
  a complete translation stream. The M25 statement that both failures are "a ready-pool liveness
  failure" is **not supported**; the pool is a victim (its entries belong to runs whose lines never
  return) or a bystander.
* **Termination accounting is correct.** `desc_completed = 1`, `run_.done >= count` and
  `win_idle()` are the three conditions at `bulk.cpp:3109`; all three held in trigger 1. The run
  reports 500 M because `Sim::idle()` requires `mem_.idle()` (`sim.hpp:785`), i.e. `inflight_.empty()`,
  and one request is still in `inflight_`.
* **It is not a timeout.** Raising `max_cycles` cannot help: nothing in the model can retire a
  request whose translation never appears.

### 2.3 The one unbounded wait in the model

`MemorySystem::tick` (`memory.cpp:1009-1090`) has three stages. Stage 2 retries a refused probe
(`rc == 2`), bounded by the L2 MSHR file draining. Stage 1 is the only place a request can wait
*without any event that is guaranteed to wake it*:

```cpp
if (r.stage == 1) {
  if (r.ready != NONE && cycle < r.ready) { keep.push_back(r); continue; }   // (a) timed wait
  ... look up the dTLB entry for r.vpn ...
  if (!found) { keep.push_back(r); continue; }                               // (b) unbounded spin
```

At (b) the request re-runs a **lookup**. It cannot re-issue a `tlb_probe`. The only thing that can
make the lookup succeed is a `pending_fill_` already in flight for `(vpn, core)`. If that fill is
lost, the request spins forever holding its residency slot; the engine's window cannot retire; the
driver never reaches a fixed point. Every observed number above is consistent with one or two such
requests.

### 2.4 Candidate mechanisms, ranked, and the test that discriminates them

None of these is yet established; Stage B starts by distinguishing them, not by patching.

| # | mechanism | code site | discriminator |
|---|---|---|---|
| **H1** | a dTLB MSHR is cleared without emitting a fill for a parked core | `memory.cpp:387-406` (`m = L2Mshr{}` after `pending_fill_` per `core_mask`) | dump the stuck request's `vpn`, then replay: was a fill ever pushed for that `(vpn, core)`? |
| **H2** | the fill lands but is evicted before the waiting request's next lookup | `memory.cpp:270-290` (`tlb_fill` LRU) vs `1015-1030` | count `tlb_fill(vpn)` against "first successful lookup of a parked request"; a fill with no matching success is H2 |
| **H3** | a completed translation is dropped by the bounded hand-off buffer | `memory.cpp:1039-1042` (`xlate_done_` capped at 8 192 → `xlate_overflow`) | `xlate_overflow` is **declared but never printed by any dump**, so today this cannot be checked at all; the counter must be exported first |
| **H4** | the L2 TLB requeues a probe past `l2tlb_req_per_entry` and its core bit is never added | `memory.cpp:308-325` | compare `l2tlb` join count against `pending_fill_` count per vpn |

H3 fits trigger 2 exactly (`tr_requests − tr_completed = 2`) and is invisible today; H1/H2 fit
trigger 1's single client-0 request. **The first Stage-B action is therefore instrumentation, not a
fix:** export `xlate_overflow`, count `tlb_fill` per core/vpn, and add a `stuck_request` dump that
fires (once) when a stage-1 request has waited more than `walk_latency × levels × 4` cycles, printing
`vpn, core, ready, gen, client, seq, probe_hit, mshr_state`. That single run turns §2.4 into one
answer.

### 2.5 The fix, once the mechanism is known

The *shape* of the fix is already clear and is the same for H1/H2/H3: **a request whose translation
has become unavailable must be able to re-enter the translation path**, rather than waiting on an
event that has already happened. Concretely:

* **H1/H2 (memory system):** at `memory.cpp:1030`, on the first failed lookup *after* the request's
  `ready` cycle has passed, re-issue `tlb_probe(core, vpn, cycle, ready)` (they are idempotent: hit →
  ready in one cycle, join → park, alloc → new walk). Charge it to the same `tlb[core].reads` so the
  cost is visible, and count it as `tlb_reprobe`. This is a monotone progress guarantee, not a
  timing change: no latency parameter moves.
* **H3 (engine hand-off):** make `xlate_done_` backpressure instead of dropping — the request keeps
  its slot and retries the push next cycle (the buffer only fills if the engine is not ticking, which
  is impossible by construction).
* **Assertions:** a request's age at stage 1 exceeding `4 × walk_latency_total` is a hard failure, not
  a spin; `inflight_.size()` must return to 0 in every cell; a run that finishes its workload must
  never tick past `last_completion + drain_bound` without retiring.

**No timeout is increased, no completion condition is weakened, and the guard stays at 500 M.**

### 2.6 Regression tests to be added (Stage B deliverable)

| test | what it pins |
|---|---|
| `tlb_stage1_reprobe` | a synthetic stage-1 request whose dTLB line is force-evicted still completes; cycles identical when no eviction occurs |
| `tlb_fill_reaches_every_parked_core` | every `tlb_probe` join is followed by exactly one fill for that core |
| `xlate_handoff_never_drops` | `xlate_overflow == 0` and `tr_requests == tr_completed` under a saturated hand-off |
| `map3_s3_liveness` | trigger 1's configuration, seeded from a built trace, terminates and conserves |
| `cohorts1_liveness` | trigger 2's configuration terminates and conserves |
| `pool_capacity_boundary` | `g6b_pa_capacity ∈ {1, 2, 8, 4096}` × `page_cohorts ∈ {1, 2, 8}` × `l2_mshr ∈ {8, 64, 128}`: every cell terminates, conservation holds, `peak_ready ≤ capacity` |
| `cohort_count_boundary` | `page_cohorts = 1` and `16` on three traces: no window is left unsealed, `win_incomplete_max ≤ 1` |
| `mshr_count_boundary` | `l2_mshr` from 8 to 256: `inflight_ ≤ R + translations`, every request retires |

---

## 3. P1 — what selection actually is today

### 3.1 The mechanism, exactly

1. `build_op_classes` (`workload.cpp:291`) produces **one bit per instruction**, before execution,
   from `op_class ∈ {gen, pattern, simt, bulk}`. `gen` copies the trace generator's own draw; `pattern`
   recomputes it from addresses; `simt`/`bulk` force a uniform map so the equivalence gates can hold
   the trace fixed.
2. `build_bulk_workload` (`bulk.cpp:3130`) scans the trace once and emits **one descriptor** for
   *every* Bulk-class element in the whole trace (`d.elem_base = 0`, `count = sel.size()`), plus one
   index array in the memory image.
3. `Sim::run_hybrid` (`bulk.cpp:3221`) partitions the SIMT-class instructions across cores
   (`bulk.cpp:3321-3329`) and submits the single descriptor once (`bulk.cpp:3303`).
4. Nothing re-classifies during execution. There is no region, no per-region descriptor, no fallback,
   and no switching cost.

So the brief's §2.1 ("move classification toward memory-region or loop-region granularity") is a real
change, and its scope is narrower than it looks: **the engine already supports a queue of
descriptors** (`BulkDesc::queue_`, `submit`, `finish_desc` at `bulk.cpp:642`, `elem_base` in the
fanout at `bulk.cpp:2663`), and `finish_desc` already requires `win_idle()`, so a descriptor boundary is
already a full drain. What is missing is (a) a region object, (b) N descriptors instead of one,
(c) per-region accounting, (d) a region-aware class map for the SIMT side.

### 3.2 The planned region mechanism (Stage C)

* **Region**: a maximal run of instructions with the same `(class, target base, index array)` tuple,
  identified statically by the builder from the trace's instruction/element structure — the same
  information `build_op_classes` already uses, plus the element→instruction map.
* **Descriptor per region**: `elem_base` = the region's first element id, `count` = its size, its own
  index array slice (`index_base` per region, so no region can read another's indices).
* **Order**: regions are submitted in instruction order; the engine drains region *k* before adopting
  *k+1* (already the behaviour of `finish_desc`), which makes the boundary cost *measurable* rather
  than hidden.
* **SIMT side**: unchanged — it already dispatches per instruction.

### 3.3 Two evaluation modes, kept apart in the code and in the report

| mode | inputs | where it lives | claim it licenses |
|---|---|---|---|
| **O** oracle / trace-informed | the trace's full element→line→page mapping, i.e. the answer | `m26_policy --mode oracle` (offline, on `diag.csv`) | an upper bound only |
| **I** implementable | trip count, index-array footprint and stride from the IR shape; page count and sequentiality *estimated* from the descriptor, not measured | `m26_select` (region table → class map) | the policy a compiler could actually run |

Mode **I** must never read `trace_pages`, `kappa_real`, `trace_lines` or any counter produced by the
simulation. The selector will be a separate translation unit that receives only the region table, and
a unit test will assert that its output is unchanged when every simulation counter is zeroed.

### 3.4 Feature provenance (to appear as a table in `docs/m26-hybrid-selection.md`)

| feature | static | profile | runtime | simulator-only |
|---|---|---|---|---|
| is the access indirect (`base + idx[k]·w`) | ✔ (IR pattern) | — | — | — |
| loop trip count / region size | ✔ | ✔ | — | — |
| index-array footprint | ✔ (array size) | ✔ | — | — |
| distinct target pages (≥512) | — | ✔ (sampled) | ✔ (dTLB misses) | **✔ (exact, `trace_pages`)** |
| sequentiality (<0.5) | — | ✔ (sampled deltas) | — | **✔ (`kappa_real`)** |
| index share (<0.5) | — | ✔ (index bytes / target bytes) | — | **✔ (`idx_dram / dram_reads`)** |

M25's policy used the last row of each — the simulator-only column. That is why mode **I** must
*estimate* the same three quantities from statically available proxies, and why the two modes are
reported side by side with the gap named.

---

## 4. P2 — cache integration, current anatomy

| path | L1 | client tag | MSHR | dedup | evidence in a production cell |
|---|---|---|---|---|---|
| engine index | **bypassed** (`bypass_l1 = true`, `bulk.cpp:1026`) | 1 | shared R, `idx_mshr = 64`, `idx_reserve = 8` | per line, within window | `cc1_l2_hits = 0` in 23/31 cells (M25) |
| engine target | **bypassed** (`bulk.cpp:1912`) | 3 | L2 MSHR file (64), joins counted | `dedup` folds elements sharing a line, **within one window** | `bulk_dedup_merged = 186 003` on `criteo_map3_s3` |
| SIMT | L1 normal | 0 | L1 + L2 files | coalescer | — |

Three facts the audit adds:

1. **`reuse_dir` already exists and is off.** M13 built a 256-set × 4-way directory (`sim.hpp:376`)
   that retains lines across windows, and no published configuration sets it. So §3.3 of the brief
   ("avoid implementing a second large cache") has a concrete answer: measure M13's structure before
   building anything.
2. **The L2 is already a cross-window cache.** 16 384 lines = 1 MiB. A re-referenced line hits in it
   unless it was evicted; whether target re-fetches reach DRAM depends on L2 residency, so
   "cross-window reuse" is partly already measured by `cc3_l2_hits`.
3. **The index stream's traffic is not a cache problem alone.** `cc1_l2_hits = 0` with one read per
   line means no repeat acquisition exists to cache (M25 §3). Making the index pass L1/L2-backed
   therefore changes *latency and port pressure*, not DRAM reads, unless the acquisition order changes
   to introduce reuse. The experiment must state that distinction up front or it will "measure" a
   change that cannot happen.

---

## 5. P3 — DRAM organization, decode and the M24-A discrepancy

**The decode** (`memory.cpp:440-460`), for `row_bytes = 1024`, `banks = 2`, `rows = 4096`, `il = 1`,
`bank_hash = 1`:

```
lines_per_row = 16,  groups = 16
bank = hash(line) % 2                       (hash = x ^ x>>6 ^ x>>12)
row  = (line / (1·2) / 16) % 4096 = (line / 32) % 4096
```

so one row index spans 32 lines = 2 KiB of address space across both banks, and **a 4 KiB page is
exactly 2 rows**. At `row_bytes = 4096` one row index spans 128 lines = 8 KiB, so **two 4 KiB pages
share a row** — the only geometry in which the brief's case 2 (cross-page shared rows) exists, and the
one M25 added (`build/wm/m25/map2/*row4k*`).

**The M3 mapping** is a 4-round Feistel permutation of the 20-bit page number
(`memory.cpp:151-183`), seeded; it is a bijection for any seed and touches no in-page offset. It
therefore *cannot* make two virtual pages share a frame, and it cannot change how many pages a row
holds — it only permutes which frame each page occupies. **M24-A's "M3 makes pages share rows"
is therefore not reproducible from this decoder**, which confirms M25's correction; M24-A's own
`row_bytes` was 1024. The Stage-E experiment will re-state the arithmetic, then measure M0/M1/M2/M3
plus `row_bytes = 4096` with and without the organizer.

**The organizer**: `m24_dram_org = 1` (default) sorts the sealed window's runs into physical
`(row, bank, line)` order at seal (`win_order_fused`, `std::stable_sort(b.ford)`), and `fpos` is the
inverse. `= 0` removes exactly that. Page cohorts (8) are a *different* mechanism: they keep 8 page
runs open so translation refusals do not block the port. The brief's hypothesis ("cohorts are
sufficient") is therefore testable as `m24_dram_org = 0` with `page_cohorts = 8` unchanged.

---

## 6. P4 — storage, from the source rather than from either report

M24-H and M25 quote different totals for the same structures (`fpa + fpos + ford` = 245 760 B in
M24-H §3; M25's "114 688 B" for `ford` + `fpos` alone). Both cannot be right, and the difference is
whether an entry is counted once or per window buffer. **Stage F derives every total from
`sizeof`/declared widths × the counts the code actually allocates**, and reports M24-H's and M25's
figures next to the derived one with the discrepancy explained. No storage claim in an M26 report
will be a citation.

The ready pool (`g6b_pa_capacity = 4096`, 16 B/entry) and `page_cohorts` (32 B/cohort) are swept, not
assumed: `{128, 256, 512, 1024, 2048, 4096}` and `{1, 2, 4, 8, 16}`.

---

## 7. Planned source changes, by stage

| stage | file | function / site | change | risk |
|---|---|---|---|---|
| **B** | `sim/src/memory.cpp` | `tick` stage 1 (1013-1045), `ptw_tick` | stuck-request dump; `tlb_reprobe`; the monotone re-probe fix; `xlate_overflow` → backpressure | **medium**: touches every request's translation path; the cycle counters of every arm can move if a re-probe is charged |
| **B** | `sim/src/sim.hpp` | `BulkStats`, `TlbStats` | new counters (`tlb_reprobe`, `xlate_overflow`, `stuck_dump`) | low |
| **B** | `sim/tests/tests.cpp` | new cases | the eight tests of §2.6 | low |
| **C** | `sim/src/sim.hpp` | new `Region`, `RegionTable` | region metadata | low |
| **C** | `sim/src/workload.cpp` | `build_op_classes` | emit a region table (class per region, not per instruction) | **medium**: every existing gate reads `ClassMap`; the per-instruction map stays as the degenerate 1-region-per-instruction case |
| **C** | `sim/src/bulk.cpp` | `build_bulk_workload` | N descriptors with `elem_base`, per-region index slices | **high**: the single-descriptor assumption is load-bearing in `ab.cpp` (arm B `r.reference`, image hash, `truth_hash`) |
| **C** | `sim/src/bulk.cpp` | `run_hybrid` | submit regions in order; per-region cycle counters; boundary accounting | medium |
| **C** | `sim/src/ab.cpp` | arm C | region map plumbing + the new gates (conservation per region) | medium |
| **D** | `sim/src/bulk.cpp` | `stream_index` / `idx_issue`, `win_emit_fused` | `idx_cache ∈ {0,1,2}` (uncached / L2 / L1+L2); `tgt_l1 ∈ {0,1}` | medium: shared MSHR/port pressure |
| **E** | `sim/src/memory.cpp` | `dram_bank_row`, `pa_page_of_vpn` | **no change** — only configurations (`m24_map`, `row_bytes`) | none |
| **F** | `sim/src/bulk.cpp` | `win_order_fused`, `fpos` build, `win_issue_g6b` | `m24_dram_org = 0` and a *derived-order* option (cohort/ready-pool order without `ford`/`fpos`) | **high**: the release order is what M24-A/M25 measured; any change must reproduce `org 0` to the cycle first |

Nothing in Stage C–F touches `Config` defaults used by a historical cell: every new knob defaults to
the behaviour of the current build, and `tools/m24d_config_audit.py` is extended to assert that the
eight ARCHITECTURE lines still produce the published cells to the cycle.

---

## 8. Experiment matrix (Stage B–F)

All cells use the M24-D production pair unless stated. `R = 128`, `l2_mshr = 64`, `batch_window =
16 384`, `bulk_org = 8`, `g6b_pa_capacity = 4096`, `page_cohorts = 8`, `m24_map = 0`, `seed = 1`,
`max_cycles = 500 000 000`. Conventional baseline is **M0-RM** (`m24d_simt.cfg`); M0 is carried as a
historical column only.

| exp | stage | cells | axis | primary metric | gate |
|---|---|---|---|---|---|
| **L1** | B | 2 | the two triggers, instrumented | does the stuck request identify as H1/H2/H3/H4 | one mechanism, reproducible in a 1-trace cell |
| **L2** | B | 2 | the two triggers, post-fix | cycles, conservation | both terminate < 100 M cycles, conservation ok, identical completion hash to the published cell where it completed |
| **L3** | B | 3 traces × 3 cohorts × 3 l2_mshr × 4 pool sizes = 108 | pool / cohorts / MSHR | termination, `peak_ready` | every cell terminates; no cell > 2 % slower on the fixed config |
| **S1** | C | 9 workloads × 5 selectors (A, B, static, region-I, oracle-O) = 45 | selector | cycles, per-region cycles, accuracy, regret, boundary cost | conservation per region; region-I must never be described as a compiler |
| **S2** | C | 9 × 2 | region size (natural vs coalesced) | boundary + start-up cost per region | boundary ≤ 5 % of the region |
| **C1** | D | 6 traces × 3 (`idx_cache`) | index acquisition | `cc1_l1/l2_hits`, `idx_dram`, `replays`, port stalls, cycles | isolate index only; no target change in the same cell |
| **C2** | D | 6 × 2 (`tgt_l1`) | target L1 lookup | `cc3_l1_hits`, L2/DRAM reads, `l2_mshr_occ`, cycles | isolate target L1 only |
| **C3** | D | 6 × 2 (`reuse_dir`) | cross-window reuse | re-fetch ratio, DRAM reads | isolate reuse only |
| **C4** | D | 6 × 2 (`l2_mshr` 64/128, join counters) | MSHR merging | joins, occupancy, cycles | isolate joining only |
| **C5** | D | 6 × 1 | combined | all of the above | only after C1–C4 are reported |
| **D1** | E | 3 traces × 6 mappings (M0, M1, M2, M3, `row_bytes=4096`, `row_bytes=4096`+M3) × 2 (`m24_dram_org`) = 36 | mapping × sorter | row hits, activations, cycles, release inversions | — |
| **D2** | E | 3 × 2 (`dram_q` 0/32) × 2 | queue depth × sorter | same | sorter value must not depend on an unbounded queue |
| **D3** | E | 3 × 5 (`page_cohorts` 1/2/4/8/16) × 2 | cohorts vs sorter | same | the brief's hypothesis, directly |
| **F1** | F | 3 × 3 representations (status quo / no-`ford`-`fpos` / derived order) | order representation | cycles, storage | derived order reproduces `org 0` to the cycle before being compared |
| **F2** | F | 3 × 6 pool sizes | pool depth | cycles, storage, `peak_ready` | every cell terminates |

Every cell records: the effective config (the simulator's own echo), the simulator build hash
(`sha256 sim/bin/dx100sim`), the trace file and its own `.json` identity, mapping and seed, and
`conservation_ok`. Outputs go to `build/wm/m26/<stage>/`.

---

## 9. What this stage did **not** do

* No source change, no rebuild, no new cell: every number above is read from a log produced before
  this milestone.
* The **root cause is not yet established** — §2.4 lists four candidates and the run that
  discriminates them. Reporting one as the cause without that run would be a guess dressed as a
  finding.
* No performance comparison of any M26 change exists, because no M26 change exists.
* The M24-A/M25 discrepancy is resolved *on paper* from the decoder (§5); the measurement that
  confirms it (D1) is Stage E.
