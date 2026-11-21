# M22 — Combined Findings

**Question.** Do the Bulk engine's measured performance benefits justify its
hardware implementation costs?

**Answer.** Yes for the mechanism, no for the implementation as shipped. The window
is load-bearing — removing it drops the engine below SIMT in every regime — and it
delivers 11.6–15.4 % on the real matrices and up to 66–82 % on scattered >L2
gathers. But the configuration that produced every published number is **build U**
(unfused), which costs 937 KiB / 0.345 mm² nominal for performance that **build F**
(fused, already implemented, M15-measured equivalent) delivers at 273 KiB /
0.101 mm². A third of the shipped window's area is a page directory that no issue
decision reads.

**Verdict: 2 — promising only after reducing hardware complexity.**

The four options, scored:

| option | verdict | why |
|---|---|---|
| 1. promising, worth RTL as-is | **no** | 3.4–3.7× more area than the same performance requires; a dead 16 KiB structure; an under-modelled index buffer |
| **2. promising after reducing complexity** | **yes** | build F + delete the directory + add 8 KiB of index line buffers = same speed, 0.101–0.183 mm², no CAM, no comparator network |
| 3. useful only for a narrow workload class | **partly** | the 1.5–3.0× regime needs >L2 target sets with low reuse and has never been observed on a real matrix here; the realistic 1.05–1.24× is not narrow |
| 4. not justified | **no** | the mechanism beats SIMT wherever the window exists, and the cheap half alone (W = 0) loses |

---

## 1. The audit's decisive measurements

**The window is the engine.** Same engine, same traces, window set to zero (an
existing configuration point, no code change):

| workload | regime | 2×8K window | W = 0 (streaming path) |
|---|---|---:|---:|
| w1_uniform | B | **1.159** | 0.975 |
| w3_duphigh | parity | **1.039** | 0.893 |
| w4_duplow | A | **2.994** | 0.994 |

With no window the engine still removes the index stream from the SIMT path and
still has its descriptor/AG pipeline, and it **loses in all three regimes**. The
benefit lives in holding elements, grouping them by line and page, and issuing one
request per line in page order.

**The win is not where the hardware is expensive.** M21's decomposition: regime A's
2.99× is page ordering (2.994 → 1.001 with `page_order=0`); regime B's 1.16× is the
index stream's request count. Dedup — the mechanism the design is named for — is
worth ≈ 0 in the one workload built to expose it (w3: 1 995 : 1 merge ratio, 3.8 %
total, all of which the index stream accounts for).

**Translation, not data movement, is the bottleneck the engine relieves.** 93.7 %
of the engine's active cycles on w4 are dTLB refusals, and the engine *increases*
DRAM traffic by 0.3–11.4 % in every workload. It wins on row-hit rate (0.1 % →
45.5 %) and walk count (99 317 → 32 740), not on bytes.

## 2. Storage, at a glance

| configuration | window | fixed | total | area [E, nominal] | × 1 MiB L2 |
|---|---:|---:|---:|---:|---:|
| U 2×8K (shipped) | 934 128 B | 25 600 B | **937 KiB** | 0.345 mm² | 0.92× |
| U 2×16K | 1 851 632 B | 25 600 B | **1.79 MiB** | 0.676 mm² | 1.79× |
| F 2×8K | 246 000 B | 33 792 B | **273 KiB** | 0.101 mm² | 0.27× |
| F 2×16K | 475 376 B | 33 792 B | **497 KiB** | 0.183 mm² | 0.49× |

Exact arithmetic from declared fields and the repository's `sizeof` probe; the
area column is the M14 density assumption (0.045 µm²/bit nominal, ±3.3× across
nodes). The worst case is reachable: w4_duplow fills every window with 8 192
distinct lines and realizes **933 888 B** [M] against a predicted 934 128 B.

## 3. The brief's structure list, itemised

| structure | costed in | entries × width | total (F 8K / U 8K) | scales with | status |
|---|---|---|---|---:|---|
| two-buffer storage | storage §3 | 2 × Batch | included | — | needed |
| window entries | storage §2–3 | W × 64 b (fused) / × 120 b (U) | 65 536 / 131 072 B | **W** | **load-bearing** |
| line-request tracking | storage §3 | L × 32 b (`fmeta`+`fslot`+`fflag`) / × 256 b (`LineReq`) | 49 152 / 262 144 B | L ≤ W | **load-bearing** |
| index buffers + readiness | storage §6.2 | 256 × 21 B metadata (+ 2×64×64 B data) | 5 396 (+8 192) B | idx_buf_lines | needed, **under-modelled** |
| page directory + masks | storage §6.1 | 512 × 128 b | 8 192 B per buffer | page_dir (fixed) | **dead** |
| permutation + ordering metadata | storage §3 | W × 32 b + L × 32 b | 65 536 B (U only) | W | **removable** |
| cohort scheduling state | storage §3 | 2 × 168 b | 64 B | page_cohorts | **load-bearing (A)** |
| deduplication structures | logic §1 | folded into the grouping pass | 0 extra | — | cheap, ≈ 0 benefit |
| response fanout | storage §4 | ≤ R × 608 b (64 B line image) | ≤ 9 728 B | outstanding fills | **load-bearing** |
| descriptor handling | storage §4 | 1 × 320 b per queued descriptor | 40 B/depth | descriptor rate | needed |
| address-generation logic | logic §1 #10 | 8 lanes × 64-bit MAC | logic only | ag_lanes | needed |
| translation interfaces | logic §5 | **no own TLB** — probes the shared dTLB per line | 0 B, port pressure | lines | the real bottleneck |
| arbitration + issue logic | logic §1 #8, §3 | 128-entry free list + reserve split, 2-way cohort select | 640 B | R | needed |
| synchronization + completion | storage §4 | R × 56 B `Pending` + per-window counters | 7 168 B | R | needed (0.017–0.034 mm² of flops) |

Fourteen structures, four verdicts: **five carry the measured win** (window
entries, line tracking, cohorts, fanout, arbitration/issue), **four are necessary
plumbing** (two buffers, index buffers, descriptors, AG/translation/completion),
**one is cheap and useless** (dedup), and **three cost area with no return** (page
directory, permutation array, `reuse_ent`).

## 4. Why the count is 2 and not 1

The gap between the architecture and a buildable implementation is implementation
debt, not architectural doubt:

1. **The fused representation is already written and measured equivalent** (M15);
   it is simply not the default, and the default is what every performance number
   used. Switching the default is a configuration change, not a redesign.
2. **The page directory is 16 KiB of statistics.** It is written at seal, read by a
   collision counter, direct-mapped with 512 entries against 6 391 pages on the
   workload that matters. Deleting it removes 6.7 % of build F's window and 100 %
   of its function (none).
3. **Two structures are under-costed, not over-costed**, and must be paid: 8 KiB of
   index line buffers (the model reads index operands straight out of the cache
   array), and ~57 k flops of request records (0.017–0.034 mm²), which is 11–26 % of
   build F's total area once the control estimate is added to its 0.101 mm² array.
4. **16K is a mode bit.** It costs +0.082 mm², buys 2.66× on an 8 MiB scattered set
   and 0.5–1.0 % in regime B.

## 5. Why the count is not 3 or 4

* Not 4: the mechanism beats SIMT wherever a window exists, in both regimes, and
  the streaming-only engine demonstrably loses (0.89–0.99).
* Not 3: 1.05–1.24× on four real matrices at 0.101 mm² (0.27× an L2) is not a
  narrow niche. What *is* narrow is the headline 1.5–3.0× regime, which needs a
  target set above L2 with low reuse — measured only on synthetic gathers here, and
  not present in any real matrix this project has run.

The energy caveat belongs to the same honesty: in regime B the engine is faster
**and** less energy-efficient (+11 % DRAM traffic, L1 bypass). A bandwidth-limited
deployment should not enable it there even though it wins on cycles.

## 6. Limitations

1. **No synthesis, no SRAM compiler, no floorplan, no frequency, no power
   measurement.** Area is `bytes × 8 × assumed µm²/bit`; the only thing this audit
   claims exactly is bytes.
2. **The `sizeof` values are the simulator's ABI**, which is why packed-minimum
   widths are reported alongside; the difference is 15–25 % per record.
3. **The 2.99× regime rests on one synthetic family** (8 MiB permuted gather). No
   real application in this tree reaches it.
4. **`page_dir = 512` is a configuration, not a physical constraint**; the finding
   that the directory is dead is about the implementation, not about what a
   directory could do if something read it.
5. **The W = 0 comparison is a configuration point of the same engine**, not a
   separately optimised streaming design; a purpose-built index-only engine might
   do better than 0.975. That does not change the conclusion, because such an
   engine would still lack ordering in regime A.
6. **Nothing here says what a different memory system would do.** The engine's win
   is translation- and order-sensitive; a machine with a bigger TLB or a smarter
   prefetcher would move the boundary.

## 7. What would change the verdict

* **To 1**: evidence that a real workload reaches regime A (an 8 MiB, low-reuse,
  scattered target set). Then 0.101 mm² for 2.99× is an easy yes.
* **To 3**: evidence that all real workloads are regime B, and that a bandwidth
  budget makes the +11 % DRAM traffic unacceptable. Then the engine is a
  latency-only device for cache-resident gathers.
* **To 4**: a cheaper way to get the same ordering — e.g. if the SIMT front end
  could be given a line-ordered request stream for less than 273 KiB, the window
  would be redundant.

## 8. Deliverables

| file | contents |
|---|---|
| `m22-storage-accounting.md` | every structure, entry counts, bits, totals at 2×8K and 2×16K, ports/kinds, measured/derived/assumed tags, scaling law |
| `m22-logic-and-timing-audit.md` | associative/CAM-free inventory, the one sequential block, critical paths, measured cycle attribution |
| `m22-area-power-estimates.md` | exact storage arithmetic, the labelled density assumption, bounded energy direction per regime, explicit non-claims |
| `m22-performance-cost-tradeoff.md` | benefit per regime, marginal value of each mechanism (including W = 0), 8K vs 16K, cost per delivered percent |
| `m22-combined-findings.md` | this file: verdict, structure checklist, limitations |
| `sim/build/wm/audit_sizes.cpp` (built and run) | the `sizeof` probe, reused from M14 |
| `build/wm/m22_win0_*.txt` | the six W = 0 cells behind §1 |

No simulator source, configuration, trace or historical report was modified for
this audit. The regression suite and the M17 anchor were re-run afterwards
(§ below).
