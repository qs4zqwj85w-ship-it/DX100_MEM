# M25 — Literature review and prior-art comparison

**Scope.** Nine bodies of work that touch what M24 Bulk does: bulk indirect offload, GPU page-walk
scheduling, TLB prefetch/MSHR compression, index-level coalescing, inter-warp and inter-SM request
merging, physical-page request grouping, and compiler-directed prefetch/offload. For each: execution
model, direction (compiler/hardware), what it reorders, translation concurrency, duplicate handling,
whether SIMT survives, adaptive selection, cost, limitations — then a feature matrix against M24
Bulk and a statement of what is *established* versus what might be *distinctive*.

Nothing here is a claim of novelty. §4 is deliberately deflationary.

Source quality note: values marked † are from an author-hosted PDF or the publisher's full text;
values marked ‡ come from an accessible abstract/secondary page and were not cross-checked against
the paper body. Two attributions in the milestone brief are wrong and are corrected in §5.

---

## 0. The closest prior art, in one paragraph

**DX100 (ISCA 2025)** is the nearest architectural relative by a wide margin: a shared,
memory-mapped accelerator near the memory controllers that executes *bulk indirect* loads/stores on
16 K-element tiles, with a compiler (MLIR) that detects `A[B[i]]` loops and hoists them into packed
operations, a Row Table that groups requests by physical DRAM bank/row/column, a Word Table that
dedups words within a column, and a conventional host that keeps running everything else. M24 Bulk
differs in *how it gets its order* and in *what it does about translation*: it translates through a
real 4 KiB-page dTLB/L2-TLB/walker hierarchy first and makes **admission** the policy, where DX100
assumes huge pages with PTEs pushed once and therefore never faces translation concurrency at all.
Everything else in M24 — the fused window, the dedup, the physical-order sort, the two front-ends —
has a direct precedent, and several of them are cheaper in the prior art.

---

## 1. Works reviewed

### 1.1 DX100 — Programmable Data Access Accelerator for Indirection (ISCA 2025) †

| | |
|---|---|
| execution model | separate accelerator, memory-mapped, shared across cores, placed near the memory controllers; 8-instruction ISA; 16 K-element tiles (1 K–32 K swept); Fill → Request → Response stages; can overlap index-array fill with translation |
| direction | **both**: software/compiler identifies and offloads (MLIR/Polygeist passes: tile, DFS over use-def chains to find indirection, hoist loads, sink stores/RMW), hardware executes and orders |
| reorders | **individual memory requests**, grouped by physical DRAM **row/column** (Row Table per bank: 64 rows via BCAM, 8 columns per row in SRAM; Request Generator interleaves across channels/bank groups); reorder window up to 16 K accesses |
| translation | 256-entry TLB, **huge pages assumed**, PTEs transferred once per application lifetime; no walk concurrency, no walk scheduling, no TLB-miss policy described |
| duplicates | Row Table groups by row and tracks columns; **Word Table** links tile iterations targeting the same column, so one line request serves several words; explicit "coalescing redundant accesses within a tile" |
| keeps SIMT? | not applicable — it augments 4 conventional OoO cores, no SIMT/warp semantics; cores run all non-offloaded work |
| adaptive selection | compiler-side: offload if legal (no cross-iteration dependence, no core stores to the region), else baseline; no runtime switching |
| cost | 2 MB scratchpad (32 × 16 K tiles), 4 ports, 256-entry TLB, 128-entry Request Table, 16 ALU lanes, 64 × 8 Row Table slices; **4.061 mm² / 777.17 mW at 28 nm**, of which scratchpad 3.566 mm² / 577 mW and the Indirect Access unit only 0.323 mm² / 83.7 mW; ~1.5 mm² at 14 nm = **3.7 %** of a four-core processor |
| results | 2.6× geomean over the multicore baseline, 3.9× bandwidth utilisation, 2.0× over the compared indirect prefetcher, 82–85 % of 51.2 GB/s regardless of index order, 7.6 column accesses grouped per row on UME |
| limits | needs bulk tiles (small frontiers fall back); no pointer chasing with random base addresses; requires no inter-iteration dependences and no core stores to the region; RMW limited to associative/commutative ops (ADD/MAX/MIN); scatter not parallelised; coherency relies on a coarse region-based SWMR protocol |

### 1.2 Scheduling Page Table Walks for Irregular GPU Applications (ISCA 2018) †

Authors: Shin, Cox, Oskin, Loh, Solihin, Bhattacharjee, Basu.

| | |
|---|---|
| execution model | unmodified GPU + IOMMU; a **SIMT-aware page-walk scheduler** at the IOMMU's pending-request buffer |
| direction | **hardware** |
| reorders | **page-walk requests**, not PTE addresses: a request carries a 20-bit SIMD-instruction id; when a walker frees, the scheduler prefers a queued request from the same instruction as the last scheduled walk, else the instruction with the least estimated remaining work (1–4 estimated accesses, scored at arrival from the walk-cache state; oldest breaks ties) |
| translation concurrency | this *is* the mechanism: it schedules among pending walks across 8 walkers; adds 2-bit saturating counters to walk-cache entries so queued walks' entries are not evicted |
| duplicates | not addressed; warp-level TLB coalescing assumed (standard) |
| keeps SIMT? | yes — the whole motivation is that a SIMD instruction cannot retire until its translations land |
| adaptive selection | none |
| cost | no area/storage figure reported; the added state is the instruction id per pending request plus 2 bits per walk-cache entry |
| results | 30 % geomean, up to 41 %; stall cycles −23 %; page-walk requests −21 % |
| limits | can starve requests; the work score is an estimate; scanning the queue adds selection latency; gains shrink with a 128-entry buffer and with larger TLB/walker configurations |

### 1.3 LATPC — Locality-Aware TLB Prefetching and MSHR Compression (MICRO 2025) †

| | |
|---|---|
| execution model | GPU MMU front-end: a **Regularity Detector** after the TLB coalescer, **LATP** (stride-based TLB prefetch) and **LATC** (MSHR compression) |
| direction | **hardware** |
| reorders | translation *requests*: detects stride subsequences among the unique VPNs a warp instruction produces; one demand + zero or more exact prefetch targets; requests carry `<VPN, Stride, Index>` |
| translation concurrency | one MSHR entry represents up to **32** in-flight VPNs via a 32-bit valid mask over `Base + Stride·i`; the PTW coalesces requests within a page-walk-buffer entry during L1→L3 traversal, then issues the reserved L4 walks (page-table lines are row-buffer friendly) |
| duplicates | the standard warp-level TLB coalescer removes duplicate VPNs before detection; LATC compresses what remains |
| keeps SIMT? | yes — no compiler pass, no execution-model change |
| adaptive selection | none; three workloads (2 MM, ADD, TPA) simply get no prefetch because their page divergence is low |
| cost | **2.48 KB** on-chip, **0.2581 mm²**, **35.78 mW** (modelled); logic is a 9-bit multiplier, 9-bit adder, two comparators, a 32:1 mux; one extra detector cycle costs 0.45 % of the speedup |
| results | 1.47× geomean over 24 workloads; translation latency 69.46 % of baseline; PTW stalls 71.20 %; L1-TLB MSHR reservation failures 68.84 % → 43.02 %; prefetch coverage 54.78 % |
| limits | only exploits stride regularity *within* a warp instruction; sorting VPNs would add ~3 % but is not implemented (latency/area) |

### 1.4 IRU — Irregular accesses Reorder Unit †

Authors: Segura, Arnau, González (UPC). **Not** Vadivel et al.

| | |
|---|---|
| execution model | unit inside each memory partition, next to the L2 slice; **permutes work items (indices) before the gather address is formed** so 32 lanes of a warp hit one 128 B line |
| direction | hardware, but the *programmer* must guarantee elements are order-independent (host API + ISA extension) |
| reorders | **data elements assigned to threads**, at cache-line granularity — never physical addresses, never DRAM rows |
| translation | not modelled at all; grouping uses `base + index×width`, so no TLB, no walk, no physical mapping is needed |
| duplicates | a 1 024-set direct-mapped hash (4 partitions) packs indices by target line; an optional secondary array **merges** duplicate values (int compare / FP add), removing 48.5 % of elements on average |
| keeps SIMT? | **yes** — same warps, same instructions, only the lane→data binding changes; that relaxation is what makes it cheap and what constrains it to idempotent updates |
| adaptive selection | none; configured per launch |
| cost | 2 KB FIFO + 1.7 KB prefetch buffer (8 outstanding) + 1.2 KB classifier + 2.8 KB ring per partition; **23.9 mm² total on a GTX 980 = 5.6 % of GPU area**, energy overhead 0.5 % (CACTI/GPGPU-Sim models) |
| results | coalescing 1.32× (4 → 3 L1 requests per warp instruction), memory-hierarchy traffic −46 %, SM↔MP traffic to 54 %, **main-memory accesses only −4 %**, 1.33× speedup, −13 % energy |
| limits | order-independence required; hash-capacity bound; adds load latency; merges only duplicates concurrently present; 24-bit indices; main memory barely improves — it fixes *line* coalescing, not row locality |

### 1.5 WarpPool (MICRO 2015) and ICC (IPDPS 2018 / IEEE TC 2019) †

| | WarpPool | ICC |
|---|---|---|
| scope | **inter-warp, within one SM**, after the intra-warp coalescer, before L1 | **across SMs in a cluster**, before the NoC |
| mechanism | requests enter a coalescing window (address-bit-mapped queues, 32 × 2 tags, up to 4 inter-warp merges) | fully-associative merge table (address, SM list, valid); a hit appends the SM id and sends nothing; the reply is **broadcast** |
| direction / SIMT | hardware; SIMT retained | hardware; SIMT retained |
| duplicates | merges inter-warp duplicates | merges intra-cluster duplicates (19.4 % of cluster requests redundant; 14 % of L1 misses coalesced) |
| cost | 5.23 KB/SM, +5.4 mm² = 1.0 % GPU area, 142 mW/SM | **276 B per cluster**, under one cycle |
| results | 1.38× geomean, L1 misses −23 %, up to 2× on unoptimised code | 9.7 % avg / 33 % peak IPC, miss latency −11.7 % |
| limits | loads only (stores bypass); window limited by memory consistency; cache-thrash risk from reordering | entry lifetime shorter than the redundancy window (48 entries captures 12 % vs 14.5 % for 160); cross-cluster duplicates remain |

### 1.6 MARS — Memory Aware Reordered Source †

| | |
|---|---|
| execution model | a request reorderer at the GPU/IP boundary, between all internal arbitration and the memory controller |
| direction | hardware |
| reorders | **requests grouped by 4 KiB physical page** (512-entry RequestQ + 128-entry 2-way PhyPageList + FIFO page order); explicitly does *not* know the DRAM map, because "exposing the memory map is not a viable option" |
| translation | upstream; it consumes physical addresses |
| duplicates | none |
| keeps SIMT? | yes |
| cost | no area/power figure ("negligible compared to GPU area") |
| results | +11 % achieved bandwidth, CAS/ACT +69 %, >2× on two microbenchmarks; 5 synthetic workloads, LPDDR4-3200 |
| limits | preliminary; synthetic only; grouping is page-granular, not row-granular; reorder-buffer latency not quantified |

### 1.7 Compiler-directed indirect prefetching: Prodigy (HPCA 2021), ATP (TACO 2019) †

| | Prodigy | ATP / informed prefetching |
|---|---|---|
| mechanism | compiler captures program semantics into a **Data Indirection Graph** embedded in the binary; a small hardware prefetcher is programmed by it, with an adaptive distance and stale-prefetch dropping | programmer/compiler emits 6-byte **Array Tracking Instructions** outside the loop describing `A[B[i]]`, `A[B[C[i]]]`, `A[func(B[i])]`; hardware tracks arrays and computes addresses, adaptive distance |
| direction | **both**, the clearest compiler/hardware split in this set | both |
| reorders | nothing — it *prefetches*; demand requests are untouched | nothing |
| cost | 0.8 KB storage | not extracted |
| results | 2.6× over no-prefetch, 1.6× energy, 1.5–2.3× over modern prefetchers, 29 applications | 2.17× / 1.85× / 1.41× on 1/4/8 cores; coverage 73 %, timeliness 88 %, accuracy 99 % |
| limit | prefetch only; no ordering, no merging, no translation | degrades with core count because prefetches are dropped |

### 1.8 Near-memory indirect coalescing: AXI-Pack (DATE 2024) ‡

Near-memory indirect stream unit with N parallel index queues and a W-window (64/128/256) request
coalescer (CSHR + hitmap + offset FIFOs): 8.4×/8.6× indirect bandwidth (SpMV SELL/CSR), 3×
end-to-end SpMV, **27 kB on-chip, 0.19–0.34 mm² in 12 nm**. Coalesces only within a W-request window;
sequential coalescing caps bandwidth. Preserves the original request order to the requester.
Closest in spirit to M24's window, but it merges *requests* at the memory side and never reorders by
row.

### 1.9 Hardware gather that deliberately does not reorder: Blackwell TMA `gather4`/`scatter4` ‡

One warp-level instruction moves 4 separately indexed rows between shared memory and global; the
`y_offset` must be 16 B aligned; ≥ 8 row offsets; out-of-bounds zero-filled or ignored. **It gathers
in the caller's order: no row/bank awareness, no dedup, no translation handling.** It is the
baseline that shows how much of M24's benefit is *ordering* rather than *transfer*: TMA moves the
same bytes with none of it.

---

## 2. Feature matrix against M24 Bulk

| | DX100 | ISCA'18 walk sched. | LATPC | IRU | WarpPool | ICC | MARS | Prodigy/ATP | **M24 Bulk** |
|---|---|---|---|---|---|---|---|---|---|
| execution model | separate accel., CPU cores | GPU + IOMMU policy | GPU MMU front-end | in-partition unit | in-SM unit | inter-SM unit | IP→MC reorderer | prefetcher | **second front-end in one GPU** |
| direction | compiler + hw | hw | hw | hw + programmer contract | hw | hw | hw | compiler + hw | **"compiler" metadata (emulated) + hw** |
| reorders | requests by DRAM row/col | walk requests by instruction | translation requests | lane→data mapping | nothing (merges) | nothing (merges) | requests by physical page | nothing (prefetches) | **runs by physical (row,bank,line); cohorts by page** |
| translation concurrency | avoided (huge pages, preloaded PTEs) | **is** the target | MSHR compression + walk batching | not modelled | n/a | n/a | upstream | n/a | **explicit: 4 KiB pages, dTLB/L2TLB/8 walkers, cohorts = admission** |
| duplicate handling | Word Table per column | — | coalescer + compressed MSHR | hash + value merge | inter-warp merge | cluster merge + broadcast | none | — | **dedup to one run per line per 16 K window; no cross-window merge; no L1** |
| keeps SIMT | n/a (CPU) | yes | yes | yes | yes | yes | yes | yes | **yes, concurrently, shared R** |
| adaptive selection | compile-time legality | no | no | no | no | no | no | distance only | **static per-instruction class bit; no fallback** |
| storage | 2 MB scratchpad + tables | pending buffer + 2 bits/entry | 2.48 KB | 23.9 mm² (5.6 %) | 5.23 KB/SM | 276 B/cluster | 512-entry Q + 128-entry list | 0.8 KB | **707 KB packed; 1.10 MB as allocated** |
| published logic area | 0.323 mm² indirect unit | not reported | 0.2581 mm² | included above | 5.4 mm² | included | not reported | not reported | **not synthesised** |

---

## 3. Established versus possibly distinctive

**Established, with a precedent for every piece:**

| mechanism in M24 | precedent |
|---|---|
| bulk/offloaded indirect access on ~16 K-element tiles | DX100 (identical tile scale); AXI-Pack (window coalescing) |
| compiler detects `A[B[i]]` and emits a packed descriptor | DX100 (MLIR), ATP, Prodigy |
| one request per distinct line within a tile; duplicates merged | DX100 Word Table; IRU; WarpPool; ICC; AXI-Pack |
| grouping generated requests by physical DRAM row | **DX100** (the only exact precedent) |
| grouping by *page* when the map is unknown | MARS |
| page-walk scheduling to expose instruction-level progress | ISCA'18 |
| TLB-miss compression and stride prefetch | LATPC |
| "offload only if legal, else run baseline" | DX100 §4.1 |
| a second execution path that keeps conventional SIMT alive | DX100 (CPU cores), and by construction every GPU-side work above |

**Potentially distinctive — and each is weak:**

1. **Admission gated by translation capacity, with the cohort count derived from dTLB MSHRs rather
   than fitted.** ISCA'18 schedules the walk queue; LATPC compresses MSHR demand; M24 instead lets
   up to `page_cohorts` pages be in flight *through the same dTLB/L2TLB path* and parks a refused
   page instead of stopping. No reviewed work states admission as the coupling between a streaming
   consumer and a shared TLB. This is the one mechanism M25 measures as worth its cost (×3.3 on two
   traces from 96 packed bytes).
2. **The negative result.** DX100 asserts row organisation is valuable (it is, for its gather
   patterns: 7.6 columns per row). M24's measurements — M23-G3 ideal translation −0.02 %, G6F full
   visibility −0.04 %, this milestone's mapping factorial 0.0002 % on the production map, and the
   queue-population measurement (7.0 % open-row density in the controller's queue vs 0.0005 % in
   the engine's pool) — say that a separate request organiser in front of a hit-first controller
   buys ~nothing. That is a characterisation result, not a mechanism, and it is the most useful
   thing this programme has produced against the literature.
3. **Two front-ends sharing one residency budget and one hierarchy, with a per-instruction class
   bit.** Close to DX100's "offload or fall back", but finer-grained (per instruction, concurrent,
   interleaved rather than per loop) and with a measured shared-`R` invariant. Whether that is
   *useful* depends entirely on the selector, which does not exist yet (§1 of the efficiency audit).

**What is not claimed:** that combining these mechanisms is novel. Every pair of them appears
somewhere in this set (DX100 pairs offload + row grouping; IRU pairs index coalescing + SIMT;
MARS pairs page grouping + a reorder buffer). M24's combination differs in the *translation
posture* and in the *negative* result, not in its parts.

---

## 4. What the literature says M24 should copy, and what it should not

| M24 trait | verdict from the literature |
|---|---|
| translating before ordering | necessary and expensive: DX100 avoids it with huge pages; every GPU-side design (IRU, WarpPool, ICC, MARS, TMA) sits *after* translation and says nothing about it. M24 is the only one that pays for 4 KiB pages, and the invoice is 8 192–99 347 index/PTE lines per kernel at a 0 % L2 hit rate. |
| dedup by window | cheaper precedents exist: WarpPool merges inter-warp for 5.23 KB/SM; ICC for 276 B/cluster; both preserve SIMT. M24 spends 262 KB (`fent`) + 131 KB (`fmeta`) to dedup 16 K elements at a time and still cannot reuse across windows. |
| physical-order release | DX100's Row Table does the same job with 64 × 8 BCAM/SRAM slices and *at issue*, not over a 16 K permuted array; M24's `ford`+`fpos` is 114 KB. |
| no L1 on the engine path | no precedent: every GPU-side design (IRU, WarpPool, ICC, TMA) keeps the normal cache path. This is M24's clearest self-inflicted handicap (efficiency audit §3). |
| compile-time offload | DX100's legality conditions (no stores to the region, no cross-iteration dependence) are stricter and better justified than M24's single regularity bit. |

---

## 5. Corrections to the milestone brief's citations

| brief | correction |
|---|---|
| "Irregular Accesses Reorder Unit (IRU)" | the paper is **Segura, Arnau, González**, "An efficient GPU-based approach…"/IRU, *J. Supercomputing* 2022 (arXiv 2007.07131); no Vadivel authorship. |
| "Scheduling Page Table Walks for Irregular GPU Applications, ISCA 2018" | correct title and venue; authors **Shin, Cox, Oskin, Loh, Solihin, Bhattacharjee, Basu**. It schedules *walk requests by SIMD instruction*, not by DRAM row — a distinction the brief's framing ("TLB scheduling, DRAM row locality") blurs. |
| "LATPC, MICRO 2025" | verified; note it is a *translation-side* design and does not reorder data requests. |
| "Relevant work on … DRAM row locality" | the only exact precedent for row-keyed request reordering is **DX100**; MARS is page-keyed and deliberately map-blind. |
| "MDM" / "SIGMA" / "Software-Driven Coordinated Miss Handling", if they were intended as gather/coalescing prior art | MDM is a *performance model* (memory divergence), SIGMA is a sparse GEMM accelerator, and "Software-Driven Coordinated Miss Handling" could not be verified as an existing title — the genuine neighbours are **SoftWalker** (software walks, 2.24×) and **neighborhood-aware translation** (1.7×). |

---

## 6. Limits of this review

* **Paywalled detail.** DX100 was read from the authors' arXiv full text; ISCA'18 and LATPC from
  author/publisher full text; IRU from arXiv + the UPC thesis; WarpPool/ICC from author PDFs. MARS
  and AXI-Pack were read from abstracts/accessible text only (‡), and their cost numbers are absent
  or unverified.
* **No re-measurement.** Speedups and costs are the authors' own numbers on their own baselines; no
  attempt was made to normalise them to M0-RM. A 1.33× on a GTX 980 with a broken coalescer is not
  comparable to 2.5× on this model's M0-RM.
* **Publication bias.** Every design above reports a win. The one thing this review can add is the
  M25 negative result (§3.2), which is only meaningful because it was measured on the same
  simulator as the positive ones.
