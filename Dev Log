# Bulk GPU Architecture — Development Log

> **Project:** Bulk GPU Architecture Research  
> **Repository:** `DX100_MEM`  
> **Simulator:** Standalone C++17 trace-driven memory-system simulator  
> **Development history:** M1–M34  
> **Current status:** M34 frozen  
> **Last documented milestone:** November 22, 2026

---

## 1. Project Overview

This project investigates whether a specialized bulk memory-request organization architecture can improve irregular GPU memory access beyond conventional SIMT execution, coalescing, caching, and request scheduling.

The original research question was:

> Can extending Vortex DXA with bulk indirect address generation and request management improve irregular GPU memory access beyond conventional SIMT execution, coalescing, caching, and request scheduling?

The project began as an attempt to extend and instrument the Vortex GPU simulator. The required architectural mechanisms were not present in the original implementation, however, and the integration cost was substantial. The project therefore transitioned to a standalone C++ simulator that retained the central memory-system question while allowing controlled experiments.

The resulting architecture, referred to as **Bulk**, explores a different way to organize irregular memory requests. Rather than immediately issuing requests in their original instruction order, Bulk gathers a large set of indirect accesses, organizes them by virtual page, and schedules requests across multiple page cohorts.

The research evolved through several major stages:

1. Building a conventional SIMT memory-system baseline.
2. Implementing a functional bulk indirect-access engine.
3. Discovering that address generation alone provides no meaningful advantage.
4. Introducing page-granular organization and multi-page scheduling.
5. Establishing controlled comparisons against increasingly stronger SIMT baselines.
6. Testing real workloads and identifying the operating conditions where Bulk helps or hurts.
7. Investigating translation throughput, DRAM organization, and hardware resource costs.
8. Refining the architecture and fixing correctness and liveness defects.
9. Conducting full-scale evaluation and sensitivity campaigns.
10. Freezing the architecture at M34 after studying DRAM bank-parallelism scaling.

The current project is a research prototype and a trace-driven memory-system model. Its results are architectural evidence within the modeled environment, not measured silicon performance or a prediction of absolute GPU speedup.

---

## 2. Project Principles and Experimental Methodology

A central objective throughout development was to make the comparison between conventional SIMT and Bulk scientifically meaningful.

### 2.1 Shared memory system

Both architectures use the same downstream memory hierarchy:

- Translation structures and page-table walkers
- L1 cache and MSHRs
- Shared L2 cache and MSHRs
- DRAM request queue and controller
- Memory timing parameters
- Request completion and retirement mechanisms

The main architectural difference is the front-end request generation and organization path.

This makes the comparison an experiment on request organization rather than a comparison between two unrelated memory systems.

### 2.2 Shared request budget

The simulator uses a global outstanding-request budget:

\[
R = 128
\]

The budget is shared between requesters. Neither architecture is allowed to obtain an advantage simply by receiving a larger global request capacity.

### 2.3 Evidence and verification

The project progressively developed a strict verification process:

- Neutral configurations must reproduce previous behavior.
- Request generation and completion must satisfy conservation identities.
- Configurations must be matched explicitly.
- Repeated runs must be deterministic.
- Historical results must remain reproducible.
- Configuration and experiment artifacts are checksummed.
- Independent extraction and verification scripts are used where practical.

The project increasingly treats reproducibility and accounting as part of the architecture research itself.

---

# 3. Chronological Development History

## M1 — Foundational Simulator and Baseline Construction

**Period:** Initial development, before November 17, 2026

The project began with the design and construction of a standalone memory-system simulator.

The original design schedule described several implementation increments, including the L2 and DRAM system, translation structures, page-table walkers, coalescing, request slots, and hit-under-miss behavior. The surviving repository evidence does not contain separate M1, M2, and M3 milestone reports or independent campaign directories.

For this development log, those early increments are consolidated into one foundational milestone: **M1 — Foundational Simulator and Baseline Construction**.

### Objectives

- Build a self-contained simulator capable of evaluating the research question.
- Establish a conventional memory-system baseline.
- Model both conventional SIMT requests and bulk-generated requests through one shared memory hierarchy.
- Establish the request-budget and experimental-accounting framework.

### Implementation

The initial simulator developed the following components:

- L1 and L2 translation structures
- Page-table walkers
- Walk cache
- L1 and L2 caches
- MSHR-based miss handling
- DRAM request scheduling
- Request slots and outstanding-request tracking
- Coalescing and hit-under-miss behavior
- Shared memory-system infrastructure

The baseline validation report records **47 checks with zero failures**.

The resulting design included:

| Component | Initial modeled configuration |
|---|---:|
| L1 TLB | 16 sets × 4 ways |
| L2 TLB | 512 sets × 4 ways |
| L2 TLB MSHRs | 8 |
| Page walkers | 2 |
| Walk cache | 16 entries |
| L1 cache | 256 sets × 4 ways |
| L2 cache | 16,384 sets × 8 ways |
| DRAM | 16 banks × 1,024 rows |
| Global request budget | 128 |

These were initial simulator design assumptions. They were not yet a faithful transcription of the Vortex reference configuration.

### Research significance

The most important outcome was the establishment of a common experimental environment.

The four original design mechanisms were kept conceptually separable:

1. Bulk indirect address generation.
2. Request organization and reordering.
3. Duplicate elimination.
4. Translation optimization.

These are research mechanisms, not development milestone numbers.

### Key artifacts

- `docs/standalone-simulator-design.md`
- `docs/dx100-experiment-design-v3.md`
- `docs/vortex-memory-recon.md`
- `docs/p0-implementation-report.md`
- `docs/baseline-validation-report.md`
- `validation/p0_counters_test.cpp`

---

## M4 — Vortex-Grounded SIMT Baseline Calibration

**Date:** November 17, 2026

### Objective

Make the conventional baseline reflect the actual Vortex reference configuration rather than relying on the simulator's initial design assumptions.

### Changes

A dedicated configuration was introduced:

`sim/configs/vortex_ref.cfg`

The configuration transcribed the relevant Vortex parameters, including:

- One core
- Four warps
- Four threads
- Issue width of one
- Four LSU lanes
- Two DRAM banks
- L2 latency of four cycles
- Zero link latency

The DRAM bank mapping was also corrected.

### Bug discovered: DRAM bank aliasing

The original mapping used:

\[
bank = line \bmod banks
\]

For the W1 permutation workload, the stride pattern caused every line to map to bank zero at the tested configuration.

This produced a misleading DRAM bottleneck.

The correction introduced an XOR-based bank hash, following the general approach used by Vortex's bank-hash configuration.

### Results

| Measurement | Before correction | After correction |
|---|---:|---:|
| W1 cycles | 191,560 | 138,907 |
| DRAM blocked cycles | 118,919 | 60,436 |

The M4a neutral-equivalence test also passed exactly at **86,308 cycles**, with an identical completion hash.

### Conclusion

The initial simulator baseline was not yet Vortex-grounded. This milestone established a more defensible conventional reference and made the bank-mapping correction part of the permanent validation requirements.

---

## M5 — First Functional Bulk Indirect-Access Engine

**Date:** November 17, 2026

### Objective

Implement a functional bulk engine without making any performance claim.

### Architecture

The first BulkEngine used a descriptor-driven pipeline:

1. Descriptor fetch.
2. Index-stream acquisition.
3. Address generation.
4. FIFO request buffering.
5. Element identity and completion tracking.

The engine generated addresses using:

\[
addr = data\_base + index[e]\cdot w_d
\]

Requests remained subject to the shared \(R=128\) budget.

### Results

The engine was functionally implemented and passed the existing baseline-equivalence gates.

No performance superiority was claimed.

### Conclusion

Bulk address generation alone was not sufficient to establish an architectural advantage.

The next research question became whether the engine could organize requests more effectively than conventional SIMT.

---

## M5.1 — Controlled A/B Evaluation

**Date:** November 17, 2026

### Objective

Determine whether enabling the Bulk engine actually helps when the two arms have identical environmental conditions.

### Changes

- Both arms use the same configuration.
- Configuration identity is checked by the A/B driver.
- Residency budgets are enforced.
- Cache and translation paths are matched.
- FIFO issue is used.
- Deduplication is disabled.

### Results

The controlled experiment found no meaningful benefit from enabling the engine alone.

| Workload | Bulk overhead |
|---|---:|
| W1 | +2.7% cycles |
| W0 | +46% cycles |

The engine also generated more DRAM traffic.

The earlier apparent advantage was attributable to capacity and memory-path asymmetries rather than the accelerator itself.

### Conclusion

This was an important negative result. It demonstrated that bulk address generation and request coordination do not automatically produce better memory behavior.

The architecture needed a mechanism that changed the organization of memory requests.

---

## M6 — Page-Granular Visibility Window

**Date:** November 17–18, 2026

### Objective

Give the engine visibility across thousands of independent accesses and use that visibility to organize memory transactions.

### Architectural changes

M6 introduced:

- Two window buffers
- A page directory
- Per-line request records
- Window sealing
- Page-ascending request order
- Duplicate detection
- Fan-out and element-completion handling
- Independent controls for deduplication and page ordering

The original FIFO path was preserved.

### Key observation

The W1 permutation workload exhibited:

- 98.3% DRAM row conflicts
- 96.6% dTLB misses
- Almost no L1 MSHR joins

This suggested that page-level request organization could improve both translation behavior and DRAM locality.

In the simulator's original mapping, a 4 KiB page covered four DRAM rows.

### Results

At a window size of 8,192:

| Measurement | SIMT | Bulk |
|---|---:|---:|
| W1 cycles | 270,753 | 233,475 |
| Row hits | 277 | 14,304 |
| dTLB misses | 15,835 | 2,256 |

Bulk achieved **1.160× speedup** on W1.

However, W0 regressed to **0.778×**.

Deduplication contributed nothing on W1, while page ordering contributed nothing on W0.

### Bottleneck diagnosis

The W1 diagnostic showed that the issue port was waiting for translations for 69% of the run.

The dominant limitation was serialized page-walk latency, not index acquisition bandwidth.

### Conclusion

Page-granular organization was promising, but a single page issue cursor could not adequately hide translation latency.

This motivated multi-page scheduling.

---

## M7 — Translation-Aware Multi-Cohort Scheduling

**Date:** November 18, 2026

### Objective

Reduce translation serialization without simply increasing TLB capacity.

### Changes

The single issue cursor was replaced by independent page cohorts.

Each cohort maintained its own cursor, allowing the engine to work across multiple pages while some translations were outstanding.

New controls included:

- `page_sched`
- `page_cohorts`

### Results

The initial W1 result was approximately:

- 233,475 → 156,907 cycles
- 32.80% fewer cycles
- 1.726× speedup versus SIMT

DRAM service remained essentially unchanged, while DRAM utilization increased from approximately 60.37% to 90.07%.

### Correction

An M8 addendum found that scheduler-installation cycles were counted but not charged to the issue port.

Corrected values were recorded without rewriting the original M7 report.

### Conclusion

The main limitation was request admission and scheduling across translations, rather than simply the amount of translation storage.

Multi-cohort scheduling became a permanent architectural mechanism.

---

## M8 — Pipelined Index Acquisition

**Date:** November 18, 2026

### Objective

Remove coarse blocking in the index acquisition path.

### Changes

- Double-buffered index ring
- Per-chunk readiness tracking
- Bounded reservation
- Configurable index-buffer capacity

### Results

The new pipeline helped when the index stream fit within the available buffering.

It regressed at larger workload sizes:

- At \(N=65,536\), M8 was 11.2% slower than M7.
- At \(N=262,144\), it was 12.7% slower.

Increasing the index ring from 64 to 128 lines brought the \(N=65,536\) result back to approximately parity.

Adversarial workloads also revealed significant sensitivity:

| Workload type | Observed speedup |
|---|---:|
| Page-scattered | 2.23× |
| Permutation | 1.73× |
| Strided | 1.26× |
| Page-local | 1.05× |
| Hotspot | 0.40× |
| High duplication | 0.61× |
| Contiguous streaming | 0.79× |

### Verification findings

The campaign's diagnostic system produced 34 violation messages. Investigation found two diagnostic-accounting defects, not simulator correctness defects.

Both were corrected by adding additional accounting invariants.

### Conclusion

Index buffering is a capacity-sensitive design dimension. A larger pipeline is not automatically better.

---

## M9 — SIMT/Bulk Dispatch and Window Management

**Date:** November 18, 2026

### Objective

Allow different operations to use different execution paths and prevent window-management policy from truncating work prematurely.

### Changes

- `OpClass` metadata
- Class filtering and compaction
- Hybrid execution driver
- Independent slot pools
- Window policies E0–E4
- Revised E3 deadline calculation

### Results

The G1/G2/G3 equivalence gates passed.

The hybrid reproduced each pure execution path to the cycle.

The original E0 policy could truncate a filling window, resulting in unnecessary window fragmentation and a significant performance loss.

E3 corrected this behavior by measuring the deadline from the last successful admission.

E3 removed the major truncation problem, with a residual workload-dependent cost.

### Conclusion

Dispatch and window management became part of the architecture.

E3 was selected as the default window policy.

The dispatch mechanism itself was still based on trace metadata, not a genuine compiler-directed or runtime selector.

---

## M10 — Production Window Configuration

**Historical reconstruction:** Partial

No dedicated M10 report or campaign directory survives in the repository.

The operating point can nevertheless be reconstructed from later reports.

### Recovered configuration

- Two 8,192-entry windows
- `batch_deadline = 4096`
- E3 sealing policy
- 128 residency slots
- Two page walkers
- Four dTLB MSHRs
- Two DRAM banks
- 64 index-buffer lines

### Historical headline

The W1 configuration reported:

- SIMT: 270,753 cycles
- Bulk: 125,581 cycles

This produced the historical 2.16× headline.

The later M17 investigation showed that this headline mixed different \(\kappa\) conditions and was not a valid matched comparison.

### Conclusion

M10 established the first production operating point, but its original report, experiment matrix, and run count are unavailable.

---

## M11 — DRAM Scheduler Audit

**Date:** November 18, 2026

### Objective

Determine whether a separate DRAM organizer could improve performance beyond page ordering and the existing FR-FCFS controller.

### Changes

The DRAM controller gained diagnostic instrumentation:

- Requester tags
- Per-requester counters
- Per-bank and per-row measurements
- Queue and scheduler statistics
- Diagnostic address mappings
- Requester separation for SIMT, index, PTE, and Bulk target traffic

M11.1, M11.2, and M11.3 extended the audit, corrected interpretations, and added observability.

### Findings

The residual DRAM scheduler opportunity on the M10 headline workload was below 1%.

The queue was global rather than independently partitioned per bank.

The audit also identified an 8 MiB mapping periodicity artifact and corrected the interpretation of earlier visibility estimates.

### Conclusion

The existing page ordering and FR-FCFS controller already captured most of the useful row locality in the production mapping.

A separate row table or address sorter was not justified at this stage.

---

## M12 — Window Capacity Scaling

**Date:** November 18, 2026

### Objective

Test whether larger windows consistently improve performance.

### Experiment

A 90-cell campaign varied:

- Window size
- Workload size
- Workload type
- Random seed

### Results

The effective target-request capacity was bounded by:

\[
R - idx\_reserve = 120
\]

The limit was reached in every W1 cell.

Larger windows helped when they allowed multiple passes to be combined, but did not provide a universal capacity benefit.

At \(N=65,536\), a 16K window regressed by 4.1%.

A 32K window improved some larger workloads, primarily because deduplication could combine additional reuse.

### Conclusion

The 8K operating point remained defensible. Larger windows were useful only under particular reuse-distance and workload conditions.

---

## M13 — Cross-Window Reuse Directory

**Date:** November 19, 2026

### Objective

Capture target-line reuse across consecutive windows without requiring a much larger window.

### Changes

A bounded reuse directory retained completed 64-byte target lines.

### Results

The W1 reuse distance was approximately one complete pass over the footprint.

At a directory capacity equal to the footprint, the design could outperform a 2×32K window configuration.

However, it required approximately 1.59 MB of storage, compared with roughly 541 KB for the window pair.

A smaller directory did not capture enough reuse to justify its cost.

### Conclusion

The reuse directory was not retained as a production mechanism.

A larger window was more storage-efficient because it retained references rather than entire cache lines.

---

## M14 — κ Sweep and Storage Measurement

**Historical reconstruction:** Partial

No dedicated M14 report or campaign directory survives.

The following findings are recoverable from later reports:

- A \(\kappa\) sweep used five paired seeds.
- Admission-stage work accounted for approximately 0.4% of cycles in the measured case.
- A `sizeof`-based storage probe was developed.
- A nominal storage-density assumption of 0.045 µm²/bit was introduced for rough area estimation.

These density-based area estimates were explicitly assumptions, not synthesis results.

The surviving probe is:

`sim/build/wm/audit_sizes.cpp`

---

## M15 — Fused Window Representation

**Date:** November 19, 2026

### Objective

Reduce the storage overhead of the window representation without changing architectural behavior.

### Changes

A fused representation packed window metadata into compact structures.

The fused representation used:

- An 8-byte packed word
- A 4-byte metadata field
- A 1-byte slot field
- A 1-byte flag field

### Results

The fused layout was approximately 3.80× smaller than the unfused C++ representation in the measured configuration.

The 2×32K configuration removed approximately 1.3 MiB of redundant representation storage.

Cycle counts remained identical, and the M10 anchors reproduced exactly.

### Limitations

The fused implementation imposed constraints on configuration combinations, including deduplication, page ordering, window size, and reuse-directory settings.

### Conclusion

The fused representation became the basis of the later production architecture, although it was not immediately made the default at M15.

---

## M16 — Stronger SIMT Baseline and Mechanism Ablation

**Date:** November 19, 2026

### Objective

Test whether Bulk's advantage survives a stronger SIMT baseline and determine which mechanisms actually produce the benefit.

### Experiments

188 cells evaluated:

- L1 MSHR scaling
- Outstanding-request scaling
- Mechanism ablation
- Equal-storage L2 comparisons

### Results

Increasing SIMT L1 MSHRs from 16 to 256 reduced the advantage from approximately 1.201× to 1.171×.

The shared request budget became the next limitation.

Disabling page-aware scheduling caused significant regressions:

- W1: +28.7%
- DUP: +45.6%
- PSCR: +207%

Deduplication contributed only approximately 0.0–0.5%.

An enlarged L2 could outperform the Bulk design on some workloads but was substantially worse on translation-sensitive workloads.

### Conclusion

Page-aware scheduling was the principal mechanism behind the surviving advantage.

Deduplication was retained as a secondary cost-avoidance mechanism.

Storage capacity and request organization were not interchangeable.

---

## M17 — Reproduction of the Original Performance Claim

**Date:** November 19, 2026

### Objective

Reconcile the original 2.16× and 2.54× claims under matched experimental conditions.

### Finding: the historical ratios were invalid

The underlying cycle counts reproduced:

| Configuration | Cycles |
|---|---:|
| SIMT | 270,753 |
| Bulk 2×8K | 125,581 |
| Bulk 2×16K | 106,456 |

However, the original ratios combined SIMT and Bulk measurements from different \(\kappa\) settings.

The matched-\(\kappa\) results were:

| Configuration | Matched speedup |
|---|---:|
| 2×8K | 1.753× |
| 2×16K | 2.068× |

The earlier 2.16× and 2.54× figures were retired.

### Mechanism findings

- Disabling page ordering reduced the 2×8K result from 1.769× to 0.991×.
- Reducing the cohort count to one reduced performance substantially.
- The advantage was primarily associated with ordering the same compulsory misses more effectively.

### Conclusion

The research moved away from reporting the original headline numbers and toward matched, reproducible comparisons.

---

## M18 — Real Irregular Workloads

**Date:** November 19, 2026

### Objective

Evaluate the architecture using real sparse-matrix and graph workloads instead of only synthetic traces.

### Workloads

- SuiteSparse SpMV:
  - `bcsstk25`
  - `qc2534`
  - `lnsp3937`
  - `dubcova3`
- SNAP BFS:
  - `facebook_combined`
  - `email-Enron`
  - `wiki-Vote`

### Results

The SpMV results showed a narrower advantage than the synthetic experiments.

Depending on the accounting comparison, Bulk was approximately 4.7–23.6% faster when each architecture paid for its own index acquisition.

Under identical instruction-stream accounting, results ranged from approximately 17.5% slower to 18.3% faster.

Page ordering contributed no more than 0.5% in these SpMV experiments.

The surviving mechanism was primarily index-stream consolidation: moving index acquisition away from a four-lane front-end into line-granular out-of-band fetching.

### BFS limitation

The single-level indirect-access engine could not represent the chained gather and scatter behavior required by the tested BFS workloads.

### Conclusion

The real-workload results were narrower and differently caused than the synthetic results.

---

## M18A — Execution Equivalence and Accounting Audit

**Date:** November 19, 2026

### Objective

Verify that the real-workload comparisons represented equivalent logical computation and correctly accounted for memory activity.

### Findings

The audit confirmed that the traces represented the same computation.

It also showed that the Bulk engine could fetch the same index bytes using substantially fewer requests.

A lane-width sensitivity study showed that the advantage declined as the modeled SIMT width increased.

The 32-lane `dubcova3` SIMT case deadlocked and was retained as an invalid measurement rather than silently discarded.

### Conclusion

The apparent advantage was sensitive to the front-end width and request-formation assumptions.

---

## M19 — Accounting and Operation-Equivalence Audit

**Date:** November 19, 2026

### Objective

Verify the fairness of the SIMT and Bulk comparisons and correct any trace-generation errors.

### Defect discovered

The trace builder incorrectly placed the payload array and calculated its following gap using the row-pointer stride rather than the payload stride.

The issue affected:

- `dubcova3` under the relevant row-prefix configuration
- BFS traces

### Correction

Corrected traces were rebuilt and rerun under new `m19_*` artifact names.

The original M18 artifacts were preserved.

Independent parsers were written to avoid reproducing the same extraction bug.

### Conclusion

The audit did not reverse the overall M18 conclusions, but it established stronger accounting practices for later campaigns.

---

## M20 — SIMT Width Sensitivity

**Date:** November 19, 2026

### Objective

Determine whether Bulk's advantage survives a wider conventional SIMT front-end.

### Important modeling limitation

The `lanes` parameter changes trace formation. It does not model a wider GPU memory pipeline, LSU, coalescer throughput, MSHR depth, or address-generation rate.

Therefore:

- Four lanes represent the calibrated reference.
- Eight lanes are a controlled extrapolation.
- Sixteen and thirty-two lanes are architectural sensitivities, not GPU performance predictions.

### Results

The advantage declined substantially as width increased.

For `bcsstk25`, the measured speedup fell from 1.182× to 1.058×.

`qc2534` fell below parity.

### Conclusion

The earlier advantage was partly a property of the request-starved baseline and the modeled front-end width.

---

## M21 — Workload Characterization and Performance Boundaries

**Date:** November 19, 2026

### Objective

Identify which workload properties predict whether Bulk is useful.

### Experiment

The campaign evaluated eight controlled indirect-access workloads and two boundary probes, with primary experiments and ablations.

### Results

Selected speedups at 2×8K included:

| Workload | Speedup |
|---|---:|
| `w4_duplow` | 2.994× |
| `w8_wslarge` | 1.532× |
| `w1_uniform` | 1.159× |
| `w5_sequential` | 1.132× |
| `w7_wssmall` | 1.087× |
| `w6_zipf` | 1.074× |
| `w9_seqsmall` | 1.064× |
| `w10_runresid` | 1.041× |

The analysis identified two major predictors:

1. Whether the target set exceeds L2 capacity.
2. How much address-order entropy exists in the gather.

A third factor—the index-stream cost relative to the useful work—determines whether the advantage survives.

Duplicate rate alone was not predictive.

### Conclusion

The policy should not be “indirect access means Bulk.”

Bulk is most useful for sufficiently large, unordered target streams where page organization can improve memory behavior.

---

## M22 — Storage, Area, Power, and Logic Cost Audit

**Date:** November 19, 2026

### Objective

Determine whether the measured performance benefits justify the hardware storage and logic complexity.

### Results

The audit distinguished unfused and fused storage representations.

| Representation | 2×8K storage |
|---|---:|
| Unfused build U | 959,728 B |
| Fused build F | 279,792 B |

The audit used an assumed density of 0.045 µm²/bit for nominal area estimation, with a wide uncertainty range.

No synthesis, SRAM compiler, floorplan, or power measurement was performed.

### Findings

The window was load-bearing: removing it caused the engine to lose across the evaluated regimes.

The fused design reduced the estimated storage cost substantially.

However, the audit also found that the strongest 1.5–3.0× performance regime had not yet been observed on a real matrix.

### Conclusion

The architecture remained promising only if its hardware complexity could be reduced.

This led directly to the M23 architecture and baseline reconstruction campaign.

---

## M23 — Architecture Archetype, Baselines, Real Workloads, Translation, and DRAM Investigation

**Period:** November 19–20, 2026

M23 was a large campaign consisting of multiple sub-stages.

Its purpose was to freeze a reproducible architecture, establish better baselines, evaluate real workloads, and determine which memory-system mechanisms actually explained the observed gains.

### M23A — Architecture archetype

The simulator's default configuration was not the published architecture.

The project therefore introduced an explicit architecture configuration and froze the fused implementation as the reference archetype.

Fused and unfused implementations were verified to produce identical cycles under the tested configurations.

### M23B — Baseline inventory

Forty exact-value test sites were reviewed.

Historical values were retained, re-anchored, or explicitly classified as stale.

This established the principle that a baseline must always have a named configuration.

### M23-A — Frozen architecture configurations

The project established explicit configurations for:

- Conventional SIMT
- DX/Bulk 2×8K
- DX/Bulk 2×16K
- DX/Bulk 2×32K

The real-workload campaign included Criteo embedding, Orkut graph frontiers, and ogbn-products sampling.

Selected results showed substantial advantages over the historical M0 baseline, but those results were later revised after resource matching.

### M23-F0 — Translation audit

A source-level audit found:

- The dTLB MSHR file capped independent walks.
- The walk itself cost 300 cycles, not 150.
- The apparent 150-cycle relationship was a throughput identity involving two concurrent walkers.
- Page-walk completion was a fixed delay and did not wait for its own PTE memory requests.

The audit also reproduced a real liveness defect in a 32-lane `dubcova3` SIMT configuration.

### M23-F1 — Ideal translation sensitivity

Making translation free produced a surprising result: Bulk became slower than M0 on every evaluated workload.

This demonstrated that translation was central to the historical advantage.

It also showed that simply increasing translation admission reduced the apparent advantage.

### M23-G — DRAM and request-generation investigation

The G-series examined:

- Request generation
- L2 MSHR sensitivity
- Issue sensitivity
- DRAM queue starvation
- SIMT request ordering
- Bulk locality organization
- DRAM mapping sensitivity
- Cross-graph generalization
- Sparse-matrix locality
- Translation and DRAM decoupling

Several experiments showed that additional DRAM visibility or a separate DRAM sorter did not consistently improve the production mapping.

The G6 campaign ultimately established a more effective streaming configuration with:

- Eight page walkers
- Eight dTLB MSHRs
- Eight page cohorts
- A fused 16K window
- Page-cohort admission and deferred translation behavior

### Conclusion

M23 transformed the project from an early accelerator prototype into a more carefully specified architecture.

It also revealed that the historical baseline was too weak and that translation behavior needed much more rigorous treatment.

---

## M24 — Page Organization, Resource Matching, HBM, and Hardware Cost

**Date:** November 21, 2026

M24 consisted of eight major sub-stages.

### M24-A — Four-way organization experiment

The experiment independently varied:

- Page organization
- Explicit DRAM organization

Page organization produced large improvements, approximately 55.7–73.7% in selected workloads.

The explicit DRAM organizer contributed only 0.00–0.01% on the production mapping.

The dominant mechanism was translation-side request organization and improved dTLB MSHR and walker utilization.

### M24-B — GPU-style HBM controller

A separate HBM controller was implemented with:

- Pseudochannel controllers
- Command timing
- Row activation and precharge timing
- Refresh behavior
- Bounded FR-FCFS scheduling
- Multiple channels

This provided a more GPU-style memory-system sensitivity model.

However, the production architecture continued to use the legacy DDR path.

The experiments also showed that the benefit of DRAM organization depended strongly on the physical address mapping.

### M24-C — Resource-matched SIMT baseline

The historical SIMT baseline had fewer translation resources than Bulk.

The resource-matched baseline increased the relevant SIMT resources to match Bulk.

This dramatically reduced the historical Bulk advantage.

### M24-D — Strict resource-matched comparison

The campaign audited configuration resources and compared the two architectures under strict matching.

It established a resource-matching protocol covering 33 resource rows and 44 fields, with zero common-resource mismatches.

The surviving Bulk advantage was workload-dependent.

### M24-E — Mechanism ablation

The ablation confirmed that page-cohort organization was the critical mechanism.

Removing it caused severe regressions.

### M24-F — Workload generalization

The campaign expanded the workload panel to 31 traces.

The results showed that the architecture was not universally beneficial.

Some workloads strongly benefited, while others were neutral or regressed.

### M24-G — Resource scaling

The campaign investigated capacity and resource sensitivity, including ready-pool and window-related parameters.

It also exposed a liveness edge involving large L2 MSHR configurations combined with a single cohort.

### M24-H — Hardware cost audit

The audit estimated approximately 690.5 KiB of packed functional storage in the relevant 16K configuration.

It also identified large timestamp arrays that were simulator instrumentation rather than hardware structures.

### Conclusion

M24 established page-cohort organization as the central architectural mechanism, made the DRAM sorter optional, and formalized strict resource matching.

---

## M25 — Architecture and Selector Audit

**Date:** November 21, 2026

### Objective

Audit the architecture's efficiency, evaluate the selector, and propose a more practical implementation.

### Findings

The supposed compiler-directed selector did not exist.

The implementation used static trace metadata to classify operations.

The effective policy was approximately “indirect access means Bulk,” which was incorrect for several workloads.

The offline selector analysis proposed a more conservative three-gate policy based on:

- Number of pages
- Sequential fraction
- Index-stream share

The offline policy showed lower regret than the original classification.

However, the selector was not implemented or validated through new simulation cells.

### DRAM organization audit

The separate DRAM organizer was found to provide almost no benefit under the production mapping.

A small improvement appeared under an alternative mapping, but this did not justify retaining it as a default mechanism.

### Proposal

M25 proposed:

- Keep SIMT as a first-class execution path.
- Use a conservative selector.
- Simplify the DRAM organization path.
- Reduce unnecessary buffering.
- Fix liveness defects before implementing further optimization.

### Conclusion

M25 was an audit and design proposal, not an implementation milestone.

---

## M26 — Liveness Defect Investigation

**Date:** November 21, 2026

### Objective

Investigate known liveness failures before attempting further architecture changes.

### Defect discovered

A request could enter the in-flight state after a dTLB hit and then lose the translation entry before the request completed its next lookup.

The hit path did not preserve the translated physical page number.

The request could then repeatedly perform lookups without reissuing the required probe.

### Instrumentation

M26 introduced detailed request-lifecycle diagnostics:

- Per-request state transitions
- Transition counts
- Recent transition history
- Per-VPN records
- Aggregate lifecycle counters
- Deterministic termination dumps

The instrumentation was gated and verified not to change normal behavior when disabled.

### Conclusion

The project identified a real translation hit/eviction race and built the evidence needed to fix it safely.

---

## M27 — Translation Race Fix and Production Configuration

**Date:** November 21, 2026

### Objective

Correct the translation liveness defect and establish a clean production configuration.

### Changes

The translated physical page number was stored in the request on a TLB hit.

This ensured that an intervening eviction could not invalidate the request's translation result.

Additional changes included:

- No-progress detection
- Shared slot accounting for Bulk translation probes
- Removal of the experimental DRAM sorter from the production path
- Production configuration cleanup

### Results

The Orkut BFS anchor reproduced:

| Architecture | Cycles |
|---|---:|
| M0-RM | 17,904,629 |
| Bulk | 6,131,444 |

Speedup:

\[
2.9201\times
\]

The test suite reached **4,967 checks with zero failures**.

### Conclusion

M27 repaired a correctness defect and established the production Bulk configuration used as the basis for later milestones.

---

## M28 — Storage Accounting and Architecture Freeze

**Date:** November 21, 2026

### Objective

Audit the complete Bulk storage implementation and establish a frozen architecture baseline.

### Findings

The audit found that earlier Bulk storage accounting omitted duplicated arrays and timestamp structures.

The implementation was reorganized to remove redundant storage.

### Changes

- M28-D consolidated duplicated metadata.
- M28-E/F moved timestamp arrays into the engine-owned representation where one copy was sufficient.
- Storage accounting was updated to distinguish functional structures from simulator instrumentation.

The final M28-F allocated storage was approximately 939,680 B, while functional storage was approximately 677,536 B under the audit's definitions.

### Freeze

`docs/m28-architecture-baseline.md` recorded the architecture, configuration, hashes, and behavior.

The baseline was preserved for later experiments.

### Conclusion

M28 established a reproducible architecture baseline and corrected the storage inventory.

---

## M29 — Full-Scale Evaluation

**Date:** November 22, 2026

### Objective

Evaluate the frozen M28-F architecture against the resource-matched SIMT baseline across a broad workload panel.

### Experiment

- 26 workloads
- 26 paired comparisons
- 133 logs
- Three diagnostic cells marked invalid and preserved
- Independent audit

### Results

Bulk achieved:

\[
1.3225\times
\]

geometric-mean speedup over M0-RM.

An ideal per-workload selector could reach:

\[
1.3630\times
\]

The difference was approximately 3.1% in the aggregate comparison.

### Workload behavior

Large, unordered target streams often benefited substantially.

Examples included:

- `criteo_2k`
- `orkut_hub`
- `orkut_bfs`
- `w4_duplow`
- `criteo_16k`

Several workloads regressed:

- `dubcova3`: 0.833×
- `qc2534`: 0.844×
- `n3_clu4m`: 0.853×
- `w1_uniform`: 0.882×
- `bcsstk25`: 0.916×

### Selector finding

The seven selector misroutes were not primarily translation failures.

They were cases where Bulk's additional engine overhead outweighed the benefit of request organization.

### Conclusion

M29 established the first broad resource-matched workload evaluation.

It showed that Bulk is useful in specific irregular-access regimes, not universally superior to SIMT.

---

## M30 — Translation Sensitivity and Page-Size Investigation

**Date:** November 22, 2026

### Objective

Determine how the architecture's advantage changes when translation resources and page sizes are varied.

### Experiments

The campaign evaluated:

- Baseline translation resources
- Increased walker and MSHR configurations
- Larger TLB reach
- Larger page sizes
- Ideal translation

### Results

The aggregate advantage declined as translation resources increased:

| Configuration | Geometric-mean speedup |
|---|---:|
| C0 | 1.3225× |
| C1 | 1.1962× |
| C2 | 1.1050× |
| C3 | 1.0796× |

The C3 result had one liveness guard hit and therefore was not a fully valid 26-workload panel.

Larger TLB reach reduced the advantage toward parity.

Under ideal translation, the aggregate result was approximately 0.9924×.

Only three workloads retained meaningful advantages.

### Page-size caveat

The page-size sensitivity experiment exposed a page-unit bug in Bulk admission.

The 64 KiB and 2 MiB conclusions from the affected campaign were therefore invalidated or retained as diagnostic-only evidence.

### Conclusion

Bulk's performance advantage was highly sensitive to translation behavior.

The ideal-translation results also showed that translation was not the only mechanism involved: a small subset of workloads retained benefits associated with DRAM locality.

---

## M31 — Scheduling, Pipeline, Translation, and Storage Diagnostics

**Date:** November 22, 2026

M31 was divided into six major investigations.

### M31-S1 — Cohort scheduling efficiency

Alternative cohort policies and capacities were tested.

The default eight-cohort policy was already near the useful operating point.

Increasing cohort count or changing selection policy produced negligible improvements.

The release budget, rather than cohort selection policy, constrained most cycles.

### M31-S2 — Internal Bulk pipeline throughput

The internal organizer stages were examined.

No major internal pipeline bottleneck was found.

The DRAM release path dominated the blocked cycles in the selected workloads.

Increasing organizer issue widths did not produce meaningful improvements.

### M31-S3 — Translation versus residual datapath

The campaign compared normal and ideal translation behavior.

The large reduction in Bulk's aggregate advantage showed that translation was central to the measured performance.

Three workloads retained meaningful advantages under ideal translation.

Their results were associated with improved within-page DRAM locality, not simply increased request concurrency.

### M31-R1 — Buffer capacity

The campaign investigated whether the architecture could save substantial storage by reducing buffer capacities.

Only approximately 3.19% of the measured storage was safely removable under the tested constraints.

The ready pool could not be reduced below 2,048 entries without a liveness failure in `criteo_16k`.

### M31-R2 — Metadata compression

Several metadata fields were narrowed.

Combined compression saved approximately:

\[
260,096\text{ B}
\]

or 27.68% of the measured storage baseline, with identical cycle results.

### M31-R3 — Lifetime sharing

Page-directory scratch storage could be shared safely during window sealing.

The savings were approximately 8,192 B.

More aggressive whole-window sharing reduced storage but introduced a performance penalty.

### Conclusion

M31 established that the architecture's internal scheduling and pipeline were already reasonably efficient.

It also showed that substantial storage reductions were possible through representation changes, but not by indiscriminately shrinking the primary window or ready pool.

---

## M32 — Translation Supply and DRAM Capacity

**Date:** November 22, 2026

### Objective

Determine whether conventional SIMT's limitations were caused by insufficient translation concurrency and whether Bulk was limited by DRAM capacity.

### Experiment

The campaign included:

- 408 runs
- 204 paired comparisons
- Walker scaling
- MSHR scaling
- DRAM bank scaling
- DRAM timing sensitivity
- Queue-depth sensitivity
- Capacity comparisons

### Translation findings

Increasing walker count alone did not improve the tested cases when MSHRs remained the limiting resource.

Translation concurrency depended on both the walker pool and the MSHR capacity.

The conventional SIMT baseline was translation-starved in a subset of workloads, while Bulk could be limited by DRAM service capacity.

### DRAM findings

Bulk was close to the theoretical two-bank service ceiling in several baseline cases.

Increasing bank count reduced Bulk cycles substantially.

The campaign also showed that larger DRAM capacity and row count alone did not provide meaningful improvements under the tested conditions.

### Conclusion

The architecture's performance was governed by the interaction between translation supply and DRAM bank parallelism.

The next question became whether the observed DRAM limit was really row-buffer serialization, bank occupancy, or another memory-system resource.

---

## M33 — DRAM Bottleneck Diagnostic

**Date:** November 22, 2026

### Objective

Determine which DRAM resource was actually limiting Bulk in the existing model.

### Method

M33 analyzed the existing M32 logs and the source implementation.

No new simulation was required.

The audit reconstructed the DRAM timing accounting and checked whether the relevant counters closed exactly.

### Findings

The legacy DDR model charges:

- 14 cycles for a row hit
- 26 cycles for a row activation/miss

The full service latency is charged to a single bank.

The model does not contain:

- A separate ACT/PRE/RD command timeline
- Explicit precharge and activation timing
- A shared data-bus occupancy model
- A complete representation of external transfer bandwidth

The measured Bulk bottleneck was therefore bank occupancy within the simulator's DRAM model.

Several workloads operated at 92.9–99.9% of theoretical bank-cycle capacity.

### Important limitation

The model cannot separate intra-bank row-operation serialization from independent shared data-transfer bandwidth.

Consequently, conclusions about an external data-bus bottleneck or a realistic SALP ceiling cannot be established from this DDR model.

### Conclusion

Bulk was genuinely DRAM-limited in the baseline configuration, but the limiting resource was the modeled bank occupancy.

M33 motivated the final bank-parallelism scaling campaign.

---

## M34 — DRAM Bank-Parallelism Scaling and Architecture Freeze

**Date:** November 22, 2026

### Objective

Determine how Bulk and conventional SIMT respond when both receive more DRAM bank parallelism while total memory capacity remains constant.

### Experimental design

Two architectures were compared:

- M0-RM: resource-matched conventional SIMT
- Bulk: the frozen production architecture

Three bank configurations were evaluated:

- 2 banks
- 8 banks
- 16 banks

Capacity remained fixed at 8 MiB.

The campaign used four workloads:

- `orkut_bfs`
- `n4_uni4m`
- `w4_duplow`
- `w1_uniform`

The experiment consisted of:

- 24 primary runs
- 4 deterministic repeats

### Results

**Cycle counts**

| Workload | 2B M0-RM | 2B Bulk | 8B M0-RM | 8B Bulk | 16B M0-RM | 16B Bulk |
|---|---:|---:|---:|---:|---:|---:|
| `orkut_bfs` | 17,904,629 | 6,131,444 | 17,753,178 | 4,176,182 | 17,751,839 | 4,138,516 |
| `n4_uni4m` | 2,541,068 | 1,161,240 | 2,541,068 | 435,740 | 2,541,068 | 422,037 |
| `w4_duplow` | 3,785,634 | 1,511,956 | 3,785,614 | 731,402 | 3,785,614 | 721,351 |
| `w1_uniform` | 215,444 | 244,133 | 172,027 | 127,924 | 170,132 | 125,301 |

**Speedup of Bulk over M0-RM**

| Banks | Orkut BFS | n4_uni4m | w4_duplow | w1_uniform | Arithmetic mean | Geometric mean |
|---|---:|---:|---:|---:|---:|---:|
| 2 | 2.9201× | 2.1882× | 2.5038× | 0.8825× | 2.1237× | 1.9384× |
| 8 | 4.2511× | 5.8316× | 5.1758× | 1.3448× | 4.1508× | 3.6243× |
| 16 | 4.2894× | 6.0210× | 5.2480× | 1.3578× | 4.2290× | 3.6832× |

The eight-bank geometric mean is **3.6243×**. The arithmetic mean is **4.1508×**.

### Main observations

From two to eight banks:

- M0-RM improved by 0.00–0.85% on three workloads.
- M0-RM improved by 20.15% on `w1_uniform`.
- Bulk improved by approximately 31.89–62.48%.

From eight to sixteen banks, improvements were much smaller.

This indicates a scaling knee in the tested workloads around the eight-bank configuration.

### Translation becomes the next limitation

At higher bank counts, Bulk's translation throughput became increasingly important.

The modeled walk cost remained 300 cycles, with eight walkers.

At sixteen banks, several Bulk workloads approached the translation-throughput floor, while DRAM occupancy fell substantially.

### Address-mapping caveat

The bank-count parameter also changes the address tiling.

The number of distinct `(bank,row)` pairs remains constant, but the number of lines per pair changes.

A 4 KiB page spans:

- Four pairs at two banks
- Eight pairs at eight banks
- Sixteen pairs at sixteen banks

Therefore, M34 does not isolate a pure bank-count effect.

The observed improvement includes the effects of the changed mapping.

A separate diagnostic holding the row expression fixed while varying bank count would be needed to isolate the pure bank-parallelism contribution.

### Validation

- Request conservation: 24/24
- DRAM accounting identities: 24/24
- Capacity checks: 24/24
- Configuration matching: 12/12
- Historical baseline reproduction: 14/14
- Deterministic repeats: 4/4
- M34 artifact hashes: 97/97

### Conclusion

M34 demonstrated that the Bulk advantage did not disappear when the conventional baseline received more DRAM bank parallelism.

Bulk benefited substantially from increased bank parallelism, then approached a translation-throughput limit.

The project was frozen at M34.

---

# 4. Architecture at the End of M34

## 4.1 Conventional SIMT path

The conventional path uses:

1. Trace-driven instruction requests.
2. SIMT-style coalescing.
3. Program-order request issue.
4. Shared request budget.
5. Translation and page walks.
6. L1 and L2 caches.
7. DRAM scheduling.
8. Completion and retirement.

## 4.2 Bulk path

The Bulk path uses:

1. Descriptor submission.
2. Index-stream acquisition.
3. Indirect address generation.
4. Window admission.
5. Window sealing and organization.
6. Page-cohort scheduling.
7. Translation-aware admission and deferral.
8. Shared translated ready pool.
9. Release into the shared memory hierarchy.
10. Completion fan-out.

## 4.3 Central architectural contribution

The most important mechanism is **page-granular request organization combined with multi-page cohort scheduling**.

The engine exposes a large set of irregular requests and organizes them before they enter the shared memory system.

The architecture primarily improves the organization and utilization of memory parallelism already available to a GPU.

It does not eliminate the need to translate distinct virtual pages, and virtual-page grouping does not guarantee DRAM row locality under arbitrary physical mappings.

---

# 5. Baseline Naming and Current Comparison

The final architecture freeze established the following naming convention.

| Name | Meaning |
|---|---|
| M0 | Original conventional SIMT baseline |
| M0A | Translation-updated SIMT baseline at the original two-bank geometry |
| M0B | Translation-updated SIMT baseline at the eight-bank geometry |
| M1–M34 | Bulk architecture development milestones |
| M34 | Final frozen Bulk architecture |

The primary current comparison is:

**M34 Bulk vs M0B**

M0A and M0B use the same translation-resource configuration. Their DRAM geometry differs.

The M0B configuration is the resource-matched SIMT configuration at eight banks and 8 MiB.

Historical experiment labels should not be retroactively changed unless their actual configuration matches the frozen definition.

---

# 6. Important Failed Approaches and Corrections

The following negative results are part of the research history.

| Approach or assumption | Finding | Outcome |
|---|---|---|
| Bulk address generation alone | No measurable benefit under controlled conditions | Replaced by organization-focused design |
| Deduplication as the primary mechanism | Approximately 0–0.5% in key ablations | Retained as secondary cost avoidance |
| Single-page issue cursor | Exposes translation latency | Replaced by multi-cohort scheduling |
| Larger windows always improve performance | Capacity and reuse-distance dependent | 16K production point retained |
| Cross-window reuse directory | Too much storage for the benefit | Not retained |
| Original 2.16×/2.54× headline | Mixed \(\kappa\) settings | Retired |
| Historical weak SIMT baseline | Translation resources were mismatched | Resource-matched baseline introduced |
| DRAM row sorter | Redundant or harmful under production mapping | Removed from production |
| More DRAM visibility | Little or no benefit in tested campaigns | Not retained |
| Static indirect-to-Bulk selector | Misroutes workloads where SIMT is better | Recognized as a limitation |
| Aggressive buffer reduction | Liveness and performance penalties | Conservative capacities retained |
| Larger page sizes | Affected by page-unit and mapping confounds | Not used as evidence for production conclusions |
| Pure bank-count interpretation of M34 | Address tiling changes with bank count | Pure effect remains unisolated |

The project deliberately preserves earlier reports and artifacts when corrections are made. Historical results are not silently rewritten.

---

# 7. Hardware Resource and Implementation Considerations

Storage accounting evolved substantially as the implementation became more detailed.

The project distinguishes:

- Functional storage
- Simulator allocation
- Packed minimum representation
- Instrumentation overhead
- Assumed silicon area

These quantities are not interchangeable.

The final M31 resource work found that the window represented approximately 92.4% of Bulk-specific storage in the relevant inventory.

Metadata compression saved approximately 260 KB in the tested representation.

However, further aggressive reductions to the window and ready pool could cause significant performance loss or liveness failures.

### Logic and timing limitations

The largest sequential operation is window sealing, which sorts a window by virtual line and element identity.

The simulator does not charge realistic hardware cycles for this sorting operation.

No synthesis-backed area, frequency, power, or timing-closure result exists.

All area estimates in the historical reports are based on explicit density assumptions.

---

# 8. Current Limitations

The M34 architecture is frozen, but several limitations define the scope of its conclusions.

### 8.1 Translation model

Page walks use a fixed delay:

\[
2 \times 150 = 300 \text{ cycles}
\]

PTE traffic exists, but walk completion does not wait on its own PTE fetches.

This means translation latency cannot respond realistically to memory pressure.

### 8.2 DRAM model

The production DDR path uses an opaque per-bank occupancy model.

It does not model a complete command timeline or a shared data bus.

The results therefore establish bank-occupancy behavior within this model, not a complete physical DRAM bottleneck decomposition.

### 8.3 GPU execution model

The simulator does not model:

- Warp scheduling
- Execution units
- Scoreboards
- Instruction issue contention
- A realistic memory network
- Full multi-SM scheduling and fairness

It is a trace-driven memory-system model, not a complete GPU microarchitecture.

### 8.4 Selector

The dispatch mechanism is based on static trace annotations.

It is not yet a compiler-generated cost model or a runtime adaptive selector.

### 8.5 Workload coverage

M34 uses four selected workloads and one seed.

It does not establish universal performance across all GPU applications.

### 8.6 Address mapping

Bank count and row tiling change together in the M34 decoder.

The pure effect of bank count has not been isolated.

### 8.7 Hardware implementation

No synthesis, power, frequency, or silicon area results exist.

Simulator storage and density-based area estimates must not be interpreted as implementation measurements.

---

# 9. Current Project Status

<PRIVATE_DATE> — M34 is the final frozen architecture.

The freeze record is:

`docs/m34-architecture-freeze.md`

The final production Bulk configuration is:

`configs/m27_bulk.cfg`

The current resource-matched SIMT comparison configuration is:

`build/wm/m24d/m24d_simt.cfg`

The M34 eight-bank comparison geometry is a runtime override.

### Freeze verification

| Artifact | Verification |
|---|---:|
| M34 hashes | 97/97 |
| M32 hashes | 431/431 |
| M33 hashes | 9/9 |
| M34 request conservation | 24/24 |
| M34 configuration matching | 12/12 |
| M34 historical anchor reproduction | 14/14 |
| M34 deterministic repeats | 4/4 |

The freeze documentation records that no further milestone, feature, experiment, or commit is authorized under the current freeze.

The project is therefore currently in a **completed experimental and documentation phase**, with M34 as the final evaluated architecture.

---

# 10. Research Conclusions

The project began by asking whether bulk indirect address generation could improve irregular GPU memory access.

The development history produced a more specific conclusion.

Bulk address generation alone did not provide a meaningful advantage. The architecture became effective after adding page-granular request organization and multi-page scheduling.

The strongest benefits appeared in sufficiently large, unordered irregular workloads where the conventional request stream had difficulty exposing and organizing memory parallelism.

The experiments also showed that:

- Translation concurrency can dominate performance.
- DRAM bank parallelism can become the next limiting resource.
- Page ordering can improve both translation-side organization and, under suitable mappings, DRAM locality.
- More storage, more visibility, or more aggressive scheduling does not automatically improve performance.
- A stronger resource-matched SIMT baseline substantially reduces the apparent advantage.
- Bulk is not universally superior and can regress on cache-resident or already-efficient workloads.

M34 adds an important final observation: increasing DRAM bank parallelism does not eliminate Bulk's advantage in the tested workload set. Instead, Bulk benefits substantially until translation throughput becomes the dominant constraint.

The central research result is therefore:

> **Bulk primarily improves the organization and utilization of irregular memory parallelism that a GPU already possesses. Its effectiveness depends on workload characteristics, translation resources, memory-system organization, and the physical address mapping.**

This is the conclusion supported by the current simulator and experimental record. It is not yet a claim about measured silicon performance or a universal GPU speedup.

---

# 11. Key Repository Artifacts

All paths below are relative to the repository root.

## Project origin and design

- `docs/standalone-simulator-design.md`
- `docs/dx100-experiment-design-v3.md`
- `docs/vortex-memory-recon.md`
- `docs/p0-implementation-report.md`
- `docs/baseline-validation-report.md`

## Architecture and implementation

- `sim/src/sim.hpp`
- `sim/src/bulk.cpp`
- `sim/src/memory.cpp`
- `sim/src/system.cpp`
- `sim/src/workload.cpp`
- `sim/src/hbm.cpp`
- `sim/src/ab.cpp`

## Major milestone reports

- `docs/m4-baseline-calibration-report.md`
- `docs/m5-bulk-engine-implementation-report.md`
- `docs/m5.1-controlled-ab-report.md`
- `docs/m6-bulk-window-implementation-report.md`
- `docs/m7-translation-aware-scheduling-report.md`
- `docs/m8-pipelined-index-acquisition-report.md`
- `docs/m9-dispatch-implementation-report.md`
- `docs/m11-dram-scheduler-audit.md`
- `docs/m12-window-capacity-scaling-report.md`
- `docs/m13-cross-window-reuse-report.md`
- `docs/fused-window-representation-report.md`
- `docs/m16-combined-findings.md`
- `docs/m17-combined-findings.md`
- `docs/m18-combined-findings.md`
- `docs/m19-accounting-audit-report.md`
- `docs/m20-combined-findings.md`
- `docs/m21-combined-findings.md`
- `docs/m22-combined-findings.md`

## M23–M28

- `docs/m23-plan.md`
- `docs/m23-a-architecture-and-baseline.md`
- `docs/m23-f0-translation-audit.md`
- `docs/m23-f1-translation-sensitivity.md`
- `docs/m24-a-four-way-organization-evaluation.md`
- `docs/m24-c-resource-matched-simt-baseline.md`
- `docs/m24-d-strict-resource-matched-comparison.md`
- `docs/m24-e-mechanism-ablation.md`
- `docs/m24-h-hardware-cost-audit.md`
- `docs/m25-adaptive-selection-design.md`
- `docs/m26-stage-b1-diagnostic-report.md`
- `docs/m27-gpu-architecture-audit.md`
- `docs/m28-architecture-baseline.md`

## Final evaluation and freeze

- `docs/m29-full-scale-evaluation.md`
- `docs/m29-a-independent-audit.md`
- `docs/m30-translation-sensitivity.md`
- `docs/m30-b-page-size-sensitivity.md`
- M31 reports and campaign directories
- `docs/m32-design-audit.md`
- `docs/m32-translation-supply.md`
- `docs/m32-dram-capacity.md`
- `docs/m32-conclusions.md`
- `docs/m33-dram-bottleneck-diagnostic.md`
- `docs/m34-bank-parallelism-scaling.md`
- `docs/m34-architecture-freeze.md`

## Experiment data and verification

- `build/wm/m29/`
- `build/wm/m30/`
- `build/wm/m31/`
- `build/wm/m32/`
- `build/wm/m33/`
- `build/wm/m34/`
- `tools/m29_extract.py`
- `tools/m29a_audit.py`
- `tools/m30_extract.py`
- `tools/m32_extract.py`
- `tools/m33_extract.py`
- `tools/m34_extract.py`
- `tools/m34_mapping.py`
- `tools/m34_verify.py`

---

*End of development log.*
