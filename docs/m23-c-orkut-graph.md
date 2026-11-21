# M23-C — Real Graph Adjacency and Neighbour Gather (com-Orkut)

Second real-workload experiment under M23. Architecture, M0 and the memory
system are those frozen by M23-A; nothing was retuned.

---

## 1. Dataset

| item | value |
|---|---|
| dataset | SNAP **com-Orkut** undirected social network |
| file | `data/orkut/com-orkut.ungraph.txt.gz` (447 MB) |
| SHA-256 | `f73e33fb685f411a…` |
| vertices / undirected edges | **3 072 441 / 117 185 083** |
| mean / max degree | 76.3 / 33 313 |
| read | file's own edge list, undirected, no sampling or generation |

## 2. CSR and the subset

The full graph is read once and turned into a symmetric CSR (`rowptr`, `colidx`)
in the builder (`tools/m23_build_orkut.py`); this is **preprocessing** and is not
charged to any configuration.

Measured kernel input = a **BFS-grown induced subgraph**, not a truncated edge
list:

* seed = vertex 43 608, the highest-degree vertex (degree 33 313) — deterministic;
* grow by BFS levels, taking vertices in discovery order until 131 072 are
  selected (levels 0…k−1 plus a prefix of level k);
* keep **every** edge whose two endpoints are both selected: **5 866 045**
  undirected induced edges.

| subset statistic | value | full graph |
|---|---:|---:|
| vertices | 131 072 | 3 072 441 (4.3 %) |
| induced edges | 5 866 045 | 117 185 083 (5.0 %) |
| mean degree | 89.5 | 76.3 |
| median / p90 / p99 degree | 28 / 214 / 809 | — |
| max degree | 33 313 | 33 313 |

The degree distribution is preserved in shape (the mean is 17 % above the
graph's — a BFS ball around the largest hub is slightly denser than the graph
average, which is a property of Orkut's clustering, not of the selection).

## 3. The measured kernel and what it excludes

One level of indirection, exactly what the engine supports:

```
for each selected vertex v, for each u in adj[v]:   out[j] = prop[u]
```

`prop` is a 64 B per-vertex property record (16 × float32, one cache line), so
each neighbour visit is one line read — the same line granularity a GNN feature
gather has.

**Excluded (preprocessing, reported separately, charged to nobody):** edge-list
parse, CSR construction, BFS level expansion, subset selection.

**Excluded (engine limitation):** the second-level dependent gather `dist[u]`
and the frontier/worklist bookkeeping. A full level-synchronous BFS iteration
issues three dependent memory streams per edge — `colidx[u]` (4 B), `dist[u]`
(8 B) and the property gather. This kernel models **two of the three** (the
`colidx` read, as the engine's index array, and the property gather), i.e. about
**two-thirds of a BFS level's memory operations**. It is a neighbour gather, not
a BFS, and no whole-graph BFS speedup is claimed.

## 4. Frontier / vertex-selection policies

All three use the same subset, the same CSR and the same layout; only which
vertices are expanded differs.

| policy | selection | vertices expanded | accesses | instructions bulk / SIMT |
|---|---|---:|---:|---:|
| **bfs** (primary) | subset vertices in BFS discovery order | 3 099 | 1 200 003 | 301 148 / 43 |
| **hub** | 316 highest-degree subset vertices (mean degree 3 799) | 316 | 1 200 002 | 300 118 / 0 |
| **tail** | 4 096 lowest-degree vertices with ≥ 2 neighbours (mean 2.4) | 4 096 | 9 736 | **0 / 4 096** |

The compiler rule is the same in all three: an instruction is annotated **Bulk**
when its vertex's adjacency list is ≥ 8 neighbours (a full warp of independent,
irregular requests); shorter lists stay **SIMT**. The `tail` frontier yields no
bulk work at all — deliberately, as the "unsuitable operation" control.

## 5. Results

### 5.1 bfs frontier (primary)

| | M0 | 2×8K | 2×16K | 2×32K |
|---|---:|---:|---:|---:|
| **cycles** | **49 067 545** | **27 034 450** | **16 714 538** | **10 401 464** |
| **speedup** | 1.000 | **1.815×** | **2.936×** | **4.717×** |
| walks | 312 381 | 176 599 | 107 399 | 62 059 |
| pte fetches | 624 762 | 353 198 | 214 798 | 124 118 |
| L2-TLB reads / hits | 525 358 / 212 977 | 177 084 / 485 | 107 552 / 153 | 62 244 / 185 |
| walker busy (2 walkers) | 93 714 300 | 52 979 700 | 32 219 700 | 18 617 700 |
| L1 reads / hits | 1 200 003 / 25 082 | 1 023 309 / 54 | 898 877 / 50 | 754 800 / 50 |
| L2 reads / hits | 1 799 004 / 1 279 578 | 1 375 911 / 743 080 | 1 112 436 / 460 365 | 871 652 / 180 594 |
| DRAM reads | 519 337 | 632 676 | 651 309 | 689 344 |
| DRAM row hits | 154 848 | 314 438 | 393 609 | 504 741 |
| DRAM row conflicts | 364 487 | 318 236 | 257 698 | 184 601 |
| DRAM activations | 364 489 | 318 238 | 257 700 | 184 603 |
| DRAM busy cycles | 11 644 586 | 12 676 320 | 12 210 726 | 11 866 052 |
| peak slots of 128 | 128 | 64 | 64 | 64 |

### 5.2 hub frontier

| | M0 | 2×8K | 2×16K | 2×32K |
|---|---:|---:|---:|---:|
| **cycles** | **41 463 846** | **26 903 940** | **17 209 462** | **11 163 527** |
| **speedup** | 1.000 | **1.541×** | **2.409×** | **3.714×** |
| walks | 246 807 | 174 930 | 109 109 | 63 499 |
| DRAM reads | 680 163 | 794 673 | 819 949 | 848 971 |
| DRAM row hits | 272 230 | 437 231 | 535 236 | 652 793 |
| DRAM activations | 407 933 | 357 442 | 284 713 | 196 178 |
| L2 hits | 1 010 579 | 735 866 | 444 108 | 143 460 |

### 5.3 tail frontier — the control

| | M0 | 2×8K | 2×16K | 2×32K |
|---|---:|---:|---:|---:|
| cycles | 553 838 | **553 838** | **553 838** | **553 838** |
| bulk elements | — | 0 | 0 | 0 |
| SIMT elements | 9 736 | 9 736 | 9 736 | 9 736 |
| every memory counter | identical in all four | | | |

### 5.4 DX-100 internal accounting (bfs)

| counter | 8K | 16K | 32K |
|---|---:|---:|---:|
| elements admitted | 1 199 876 | 1 199 876 | 1 199 876 |
| distinct lines issued | 948 188 | 823 756 | 679 679 |
| elements merged | 251 688 (21.0 %) | 376 120 (31.3 %) | 520 197 (43.4 %) |
| window batches | 147 | 74 | 37 |
| index lines requested | 74 993 | 74 993 | 74 993 |
| index-stream dTLB refusals | 511 862 | 462 877 | 423 993 |
| target-issue dTLB refusals | 0 | 0 | 0 |
| organizer / fanout cycles | 599 938 / 811 695 | 599 938 / 766 348 | 599 938 / 716 621 |
| request-buffer peak (of 128) | 121 | 128 | 128 |
| fused window bytes | 245 760 | 475 136 | 924 672 |
| mean window occupancy / capacity | 16 199 / 16 384 (**98.9 %**) | 31 932 / 32 768 (**97.4 %**) | 62 514 / 65 536 (**95.4 %**) |

## 6. Analysis

### Q1. How much locality exists among neighbour-derived target accesses?

Three different localities, measured separately:

| locality | measurement | value |
|---|---|---|
| **page** | pages touched of 2 048 in the 8 MiB property region | **all 2 048** (L2-TLB holds 512 → 4× oversubscribed) |
| **cache line** | distinct lines of 131 072 possible | 100 282 (**76.5 %** of the array), duplicate-line fraction **0.916** |
| **DRAM row** | row hits / reads, M0 | 154 848 / 519 337 = **29.8 %** (row = 1 KiB = 16 lines) |

So the graph has strong **line** reuse (a neighbour set recurs heavily across
adjacent vertices — Orkut's clustering) but weak **page** locality in program
order and only moderate **row** locality. These three move in different
directions under the window, which is why they are not interchangeable.

### Q2. Does a larger window expose locality SIMT order does not?

**Yes.** Distinct lines issued fall 948 188 → 823 756 → 679 679 (−28 %) and
merged elements rise 21.0 % → 31.3 % → 43.4 % of all bulk elements. Batches fall
147 → 74 → 37. The gain is monotone in capacity with no saturation: mean window
occupancy is still 95 % at 32K.

### Q3. Does page-aware organisation reduce translation overhead?

**Yes — it is the dominant effect again.** Walks 312 381 → 176 599 → 107 399 →
62 059 (**−80 %**), L2-TLB reads 525 358 → 62 244 (−88 %), walker busy
93.7 M → 18.6 M cycles.

Walk-service time predicts the cycle counts: `walks × 150` gives
46.86 M / 26.49 M / 16.11 M / 9.31 M against measured
49.07 M / 27.03 M / 16.71 M / 10.40 M — **95.5 %, 98.0 %, 96.4 %, 89.5 %** of
the runtime. The residual grows at 32K because DRAM (~12 M busy cycles) becomes
a comparable constraint.

### Q4. Cache behaviour and DRAM scheduling?

**Cache hit rates get worse; row locality gets better; traffic gets worse.**

* L2 hit rate falls 71 % → 54 % → 41 % → 21 %: a bigger window merges duplicate
  neighbours *before* issue, so fewer lookups survive to be L2 hits. The engine
  wins by issuing fewer requests, not by hitting more.
* Row hits rise 29.8 % → 49.7 % → 60.4 % → **73.2 %** and activations fall
  364 489 → 184 603 (−49 %). This is genuine row-locality gain: page-ordered
  issue groups lines that share a 1 KiB row.
* DRAM **reads** rise 519 337 → 689 344 (+33 %) because target requests bypass
  L1, so every merged-away request still costs one line read where SIMT might
  have hit. DRAM busy cycles stay ~12 M in all four: DRAM is saturated
  throughout, so its *efficiency* improves while its *utilisation* does not.

### Q5. Sensitivity to degree distribution and frontier composition

| frontier | mean degree | 8K | 16K | 32K |
|---|---:|---:|---:|---:|
| bfs order | 387 | 1.815× | 2.936× | **4.717×** |
| hubs only | 3 799 | 1.541× | 2.409× | 3.714× |
| low-degree tail | 2.4 | 1.000× | 1.000× | 1.000× |

Sensitivity is real and in the expected direction: **hub-dominated frontiers
gain least** (1.54× vs 1.82× at 8K) because their neighbour sets are broad and
less repeated — the same 1.2 M accesses touch 94 482 lines at 92 % duplication
but with 26 % more DRAM reads than the BFS frontier. Low-degree frontiers gain
nothing at all and cost nothing (§Q7). BFS order — which mixes hubs and leaves —
is the most favourable, and it is also the most natural execution order, so it is
reported as primary with the other two alongside rather than instead.

### Q6. Which context capacity is most effective?

32K, in every policy and every counter, and the marginal gain has **not**
decayed:

| step | bfs cycles | gain | storage added | gain per doubling |
|---|---:|---:|---:|---:|
| M0 → 8K | 49.07 → 27.03 M | 1.82× | 245 760 B | — |
| 8K → 16K | 27.03 → 16.71 M | 1.62× | +229 376 B | 1.62× |
| 16K → 32K | 16.71 → 10.40 M | 1.61× | +449 536 B | 1.61× |

Unlike M23-B (Criteo), where the second doubling bought only 1.37× and the
per-KiB return halved, here each doubling returns almost the same factor. The
reason is visible in occupancy: the buffer is still 95 % full at 32K, i.e. the
workload is capacity-bound rather than ordering-saturated. **This does not
establish 32K as the preferred capacity** — it establishes that this graph
workload, like the Criteo one, has not yet found the knee; M23-D must agree.

### Q7. Does compiler-directed selection identify unsuitable graph operations?

**Yes, and the tail frontier is the proof.** Its 4 096 vertices have mean degree
2.4, every instruction is annotated SIMT, **zero** elements reach the engine, and
all four configurations produce **identical** cycle counts and identical memory
counters (553 838 cycles, 3 691 walks, 5 739 DRAM reads). The accelerator is not
merely neutral here: it is *absent*, with no descriptor, no index traffic and no
fanout charge.

At the other extreme, 43 of 301 191 instructions in the bfs frontier are
SIMT-class (isolated vertices, degree < 8) and they execute on the SIMT path
while 1 199 876 neighbour visits run on the engine.

### Q8. Which graph access patterns still perform poorly?

1. **Low-degree / tail frontiers**: no benefit (and no loss). A graph phase that
   is mostly low-degree vertices cannot be helped by a windowed gather.
2. **Hub-only frontiers**: the weakest positive result (1.54×) and the highest
   DRAM read count. Broad, weakly-repeated neighbour sets are exactly the case
   where merging does least.
3. **Everything DRAM-bound**: DRAM busy cycles do not fall (11.6 M → 11.9 M). The
   engine reduces *requests and walks*, not bytes; when DRAM is already
   saturated (as here at ~24 % of runtime) that caps the benefit.
4. **Index acquisition is the blocked port**: 511 862 index-stream dTLB refusals
   and 0 target-issue refusals. The engine's own index reads are what the
   translation system refuses, so index traffic scales the cost with count.

## 7. Correctness and accounting

| check | result |
|---|---|
| CSR construction | symmetric, `nnz = 2 × 5 866 045`; degree vector matches the induced edge multiset |
| vertex IDs / target addresses | engine index value = `16 · u`, so `addr = prop_base + u · 64` exactly; `bulk_addr_mismatch = 0` in all nine DX runs |
| partition completeness | bfs 127 + 1 199 876 = 1 200 003 = trace accesses; hub 0 + 1 200 002; tail 9 736 + 0 |
| conservation | `conservation = OK` in all twelve runs |
| index traffic charged | 74 993 index lines = ⌈1 199 876 × 4 B / 64 B⌉ in every DX run |
| descriptor charged | 1 descriptor fetch per run |
| preprocessing separated | CSR build, BFS and subset selection are outside the trace; §3 |
| identical inputs | one trace per policy, same SHA-256 in all four configurations |
| M0 unchanged | `l1_mshr = 128`, no engine, `dispatch = 0` |
| determinism | the `dram_diag` pass reproduced the non-diagnostic cycle counts exactly |

Completion hashes are not compared across configurations (they accumulate in
completion order, which scheduling changes); element-level equivalence rests on
`bulk_addr_mismatch = 0` plus conservation.

## 8. Limitations

1. **No second-level indirection.** The `dist[u]` probe and frontier bookkeeping
   are excluded; the kernel is ~⅔ of a BFS level's memory streams and is not a
   BFS. No whole-graph speedup is claimed.
2. **One descriptor per run**: 1.2 M neighbour visits execute as one gather over
   147/74/37 window batches. Per-vertex timing is not observable.
3. **Per-vertex/per-degree-class timing is not observable**; cache, TLB and DRAM
   counters are aggregate. Policy comparisons are therefore whole-kernel.
4. **DRAM capacity is not modelled** (M23-A): the row index wraps at 8 MiB while
   the property region is exactly 8 MiB, so the region maps onto the full row
   space once. Row-hit numbers are therefore *not* aliased for this workload —
   unlike M23-B's 64 MiB table region.
5. **Walk cost is calibrated** (150 cycles/level, 2 walkers). Absolute speedups
   scale with it; the walk *reductions* are structural.
6. **L2-TLB reach 2 MiB vs 2 048 pages (8 MiB)**: the property region is 4× the
   reach, which is why walks dominate. A larger TLB would compress these numbers.
7. **`bulk_max_elems` raised to 2 097 152** for these runs (descriptor capacity,
   not a memory-system resource), identical in all three DX-100 configurations.
8. **The subset is 4.3 % of Orkut's vertices**; degrees and clustering match, but
   no claim is made about whole-graph behaviour.

## 9. Reproducibility

```bash
cd DX100_MEM

# 1. dataset (447 MB)
curl -sL -o data/orkut/com-orkut.ungraph.txt.gz \
  "https://snap.stanford.edu/data/bigdata/communities/com-orkut.ungraph.txt.gz"

# 2. three frontier policies over one induced subset (131 072 vertices)
for p in bfs hub tail; do
  python3 tools/m23_build_orkut.py --edges data/orkut/com-orkut.ungraph.txt.gz \
    --vertices 131072 --max-accesses 1200000 --policy $p \
    --out build/wm/m23_orkut_$p.bin
done

# 3. four configurations per policy
cd sim
for p in bfs hub tail; do for c in m0 dx8k dx16k dx32k; do
  ./bin/dx100sim --config configs/m23_$c.cfg \
    --set dataset=../build/wm/m23_orkut_$p.bin \
    --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
    --set bulk_max_elems=2097152 --set dram_diag=1 --all \
    > ../build/wm/m23c_${p}_$c.txt
done; done
```

Note `bulk_desc_line` is a **line number** (`0x1FFFFC0` = `0x7FFFF000 / 64`).

## 10. What this establishes

**Establishes:** on a real social graph with its own degree distribution, a
BFS-frontier neighbour gather runs **1.82× / 2.94× / 4.72×** faster at 2×8K /
2×16K / 2×32K than conventional SIMT, and the mechanism is the same as in
M23-B — 80 % fewer page walks — plus, here, a real row-locality gain (29.8 % →
73.2 % row hits, activations halved). Compiler selection cleanly separates the
unhelpful case: a low-degree frontier is routed entirely to SIMT and costs
exactly nothing.

**Does not establish:** that any BFS is accelerated (only the gather is
measured); that 32K is the right capacity (marginal gain has not decayed, so the
knee is still unlocated); that the result survives a larger TLB or a
bandwidth-limited DRAM; or any per-vertex effect. Those remain M23-D's and
M23-E's questions.
