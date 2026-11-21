# M23-D — Real GNN neighbour-feature aggregation (ogbn-products)

Third real-workload evaluation of M23. Architecture and baseline are frozen from
M23-A and are **not** changed here; this document is an evaluation of the frozen
configurations on a new workload.

Configurations, identical data path and cache/TLB/DRAM geometry in all four
cells (see `docs/m23-a-architecture-and-baseline.md`):

| id | window | description |
|---|---:|---|
| M0 | — | strengthened conventional SIMT, no Bulk |
| DX-8K | 8 192 | fused 2×8 K |
| DX-16K | 16 384 | fused 2×16 K |
| DX-32K | 32 768 | fused 2×32 K |

All DX-100 cells run compiler-directed selection (`dispatch=1`, `op_class=gen`);
the per-operation decision is emitted in the `m23_select` dump.

---

## 1. Dataset

| property | value |
|---|---|
| benchmark | OGB `ogbn-products` (node property prediction) |
| source | `https://snap.stanford.edu/ogb/data/nodeproppred/products.zip` (1 480 993 786 B) |
| nodes | 2 449 029 |
| edges in `raw/edge.csv.gz` | 61 859 140 directed `(src,dst)` rows |
| mean out-degree | 25.26 (max 16 037) |
| features | 100 × float32 per node (`raw/node-feat.csv.gz`, 1 158 090 752 B compressed) |
| label space | 47 classes |
| splits shipped in this archive | `split/sales_ranking/{train,valid,test}.csv.gz` |
| train split used | 196 614 node ids |
| graph file SHA-256 | `a012950cc853f8262d5bbc9d93ca393e978e1e8f34bcf639125585dc3b515341` |

Two notes on fidelity, both of which the analysis depends on:

* **The archive does not contain the `time` split.** It ships the secondary
  `sales_ranking` split (the one introduced in the OGB paper for the ranking
  variant). The primary sampler therefore draws from `sales_ranking/train`.
  That split is **popularity-skewed**: its nodes have mean out-degree 141.9 and
  median 90, versus 25.3 for the graph as a whole. A second sampler drawn
  uniformly from all 2 449 029 nodes is evaluated alongside it so the degree
  sensitivity (question 6) is measured rather than assumed.
* Edge rows are used as given: `src → dst`, one direction, no symmetrisation.
  `ogbn-products` connectivity is an undirected co-purchase relation stored as
  one directed row per pair, so per-node out-degree averages 25.3 and the
  in+out neighbourhood would average ~50.5. Nothing is invented; the direction
  is simply stated.

### Selected subset

| sampler | targets | neighbour gathers | distinct lines | distinct pages | working set |
|---|---:|---:|---:|---:|---:|
| `sales_ranking/train` (primary) | 8 455 | 1 200 003 | 689 990 | 38 246 | 42.11 MiB |
| uniform over all nodes | 41 285 | 1 200 001 | 739 041 | 38 234 | 45.11 MiB |

The target cap is `--max-accesses 1 200 000`, the same order as M23-C's
1 200 003 neighbour visits, so the three real workloads are compared at equal
gather volume. The primary run exhausts the cap after 8 455 of the 65 536
requested targets because its drawn targets are high-degree.

## 2. Sampling and workload generation

1. **Target selection.** `rng = default_rng(1)`; `targets = rng.choice(pool,
   65536, replace=False)` where `pool` is either the 196 614 train ids or all
   node ids. Documented, seeded, identical for all four configurations (the
   trace file is generated once and replayed unmodified).
2. **Adjacency resolution.** All 61 859 140 edges are scanned once
   (`np.isin(src, targets)`), the matching rows are stably sorted by `src`, and
   a CSR row range is built for each target. This is **preprocessing**: it does
   not appear in the trace and is charged to nobody.
3. **Sampling order.** Targets are consumed in the sampled order until the
   access cap is reached; each target's neighbours are gathered in the order
   they appear in `edge.csv.gz`, so the access sequence is a deterministic
   function of (dataset, seed, cap).
4. **Kernel.** For every target `t`:
   `agg[t] = Σ_{u ∈ N(t)} feat[u][0:16]`.
5. **Emission.** Each target's neighbour list is packed into 4-lane
   instructions. An instruction is annotated BULK when the target's degree is
   ≥ 8, SIMT otherwise — a compiler rule over static metadata (the target's
   degree from the CSR row range), evaluated before execution, with no runtime
   sampling, no fallback and no mid-operation switching.

## 3. Feature layout and address mapping

```
feat[u][d]  = prop_base + 64·u + 4·d          prop_base = 0, d < 16
row         = 64 B = exactly one cache line
region      = 2 449 029 × 64 B = 156 737 856 B = 149.5 MiB
index array = 0x8000_0000, one u32 element offset per gather, 4 B
descriptor  = line 0x1F_FFC0 (= 0x7FFF_F000 / 64), flag block 0x1FFF_C0
```

Node ids are the dataset's own ids; no renumbering, no permutation. Because the
row is exactly one line, `addr/64` is the touched line and `addr/4096` is the
touched page, so the line/page statistics below are the dataset's real
connectivity applied to the real feature layout.

**Feature width (modelling decision, stated because it changes the numbers).**
The engine's element is one 4 B value and its unit of work is a line, so the
gathered vector is the **first 16 of the 100 features**. The full 100-dim vector
is 400 B = 6.25 lines per neighbour; it is *not* simulated — see §8. The 16 dims
are the dataset's real values in the dataset's real order, and the aggregation
(`Σ` over those 16 dims) is computed from them offline as the reference checksum.

## 4. Measured-kernel scope

Inside the measurement: index-line reads, target-line gather and de-duplication,
address generation, L1/L2/DRAM service, TLB and page walks, the engine's fanout
(aggregation scheduling) stage, and writeback accounting.

Outside the measurement, and named as such everywhere below:

* minibatch/target sampling (step 1),
* adjacency resolution — the CSR scan and sort that turns `colidx` into the
  neighbour id list (step 2); this is the "second level" of indirection the
  engine cannot express,
* the aggregation arithmetic itself: the engine charges fanout *scheduling*
  cycles (`bulk_fanout_cycles`) but no add-cycles, and the SIMT path charges
  none either. With 1.2 M gathers × 16 adds at one add/lane/cycle over 4 lanes,
  omitting accumulation is worth ≈ 4.8 M cycles (2.7 % of M0, 4.2 % of DX‑32K);
  it is omitted from **both** sides, so speedup ratios are affected only by the
  difference in denominator.

No end-to-end GNN claim is made: this is the feature-gather stage on real
connectivity, not training.

## 5. Results

### 5.1 Primary sampler (`sales_ranking/train`)

| | M0 | DX‑8K | DX‑16K | DX‑32K |
|---|---:|---:|---:|---:|
| **cycles** | 176 184 939 | 156 713 716 | 139 088 324 | **113 518 967** |
| **speedup vs M0** | 1.000× | 1.124× | 1.267× | **1.552×** |
| page walks | 1 174 566 | 1 044 628 | 927 157 | 756 473 |
| walk reduction | — | −11.1 % | −21.1 % | −35.6 % |
| PTE fetches | 2 349 132 | 2 089 256 | 1 854 314 | 1 512 946 |
| `cycles / (walks×150)` | 1.0000 | 1.0001 | 1.0001 | 1.0004 |
| DRAM reads | 1 194 355 | 1 294 308 | 1 369 035 | 1 337 483 |
| DRAM row hits | 1 661 (0.14 %) | 109 375 (9.23 %) | 145 089 (11.85 %) | 207 799 (18.39 %) |
| DRAM activations | 1 192 694 | 1 184 933 | 1 223 946 | 1 129 682 |
| DRAM row conflicts | 1 192 692 | 1 184 931 | 1 223 944 | 1 129 682 |
| L2 hit rate | 66.3 % | 61.0 % | 54.5 % | 50.7 % |
| L1 hits | 344 | 48 | 44 | 27 |
| max in-flight requests | 12 | 72 | 73 | 76 |
| DRAM bytes | 76 438 720 | 82 835 712 | 87 618 240 | 85 598 912 |
| `walker_busy` / cycles | 2.00 | 2.00 | 2.00 | 2.00 |

### 5.2 Uniform sampler

| | M0 | DX‑8K | DX‑16K | DX‑32K |
|---|---:|---:|---:|---:|
| **cycles** | 176 800 539 | 160 417 625 | 144 443 603 | **119 371 267** |
| **speedup vs M0** | 1.000× | 1.102× | 1.224× | **1.481×** |
| page walks | 1 178 670 | 1 069 343 | 962 895 | 795 739 |
| DRAM reads | 1 193 961 | 1 287 047 | 1 357 779 | 1 329 040 |
| DRAM row hits | 1 291 (0.11 %) | 97 630 (8.21 %) | 126 664 (10.29 %) | 179 952 (15.66 %) |
| L2 hit rate | 66.4 % | 61.8 % | 55.8 % | 51.9 % |

### 5.3 Gather versus gather+aggregation

| stage | M0 | DX‑8K | DX‑16K | DX‑32K |
|---|---:|---:|---:|---:|
| full measured kernel (cycles) | 176 184 939 | 156 713 716 | 139 088 324 | 113 518 967 |
| …of which engine fanout stage | — | 1 192 474 | 1 185 856 | 1 172 871 |
| fanout share of runtime | — | 0.76 % | 0.85 % | 1.03 % |
| fanout lanes / utilisation | — | 8 / 0.0 % | 8 / 0.1 % | 8 / 0.1 % |
| index traffic | 74 938 lines / 4 796 032 B | same | same | same |
| aggregation arithmetic | not modelled | not modelled | not modelled | not modelled |

Aggregation is 0.8–1.0 % of runtime in every DX cell and is *unmodelled* on the
SIMT side, so "gather+aggregation" is indistinguishable from "gather" at this
workload's arithmetic intensity (16 adds per 64 B line). The engine is not
aggregation-limited; the fanout lanes sit at 0.1 % utilisation because the
window, not the ALU, gates progress.

### 5.4 Engine internals (primary sampler)

| | DX‑8K | DX‑16K | DX‑32K |
|---|---:|---:|---:|
| descriptor / window size | 8 192 | 16 384 | 32 768 |
| window occupancy (max / mean fill) | 8 192 / 8 156 | 16 384 / 16 202 | 32 768 / 32 405 |
| window live cycles (share of runtime) | 156 284 191 (99.7 %) | 138 649 553 (99.7 %) | 111 333 487 (98.1 %) |
| elements via Bulk | 1 199 005 | 1 199 005 | 1 199 005 |
| elements via SIMT | 998 | 998 | 998 |
| Bulk instructions | 302 827 | 302 827 | 302 827 |
| SIMT instructions | 335 | 335 | 335 |
| de-dup merges | 6 430 | 12 645 | 25 520 |
| batches | 147 | 74 | 37 |
| slot stalls (cycles) | 115 298 | 110 500 | 100 900 |
| window-full stalls (cycles) | 81 019 | 66 570 | 65 358 |
| index-buffer stalls (cycles) | 8 646 | 10 952 | 10 429 |

Window occupancy is 98.9–99.5 % of capacity in all three sizes: the context
buffer is full for essentially the whole run, which is why the capacity sweep
still improves monotonically.

## 6. Locality analysis

**Line locality exists, page locality only weakly, row locality not at all.**

* *Line:* 689 990 distinct lines for 1 200 003 gathers — a 42.5 % duplicate
  fraction. Neighbour sets do reuse each other's feature rows.
* *Page:* 38 246 pages are touched, i.e. 18 lines per page against 64 rows per
  page. That is 74.7× the 512-entry L2 TLB, and the walk count confirms it:
  1 174 566 walks for 1.2 M gathers — **0.98 walks per gather at M0**.
* *Row:* 1 661 row hits out of 1 192 694 activations = **0.14 %**. Feature rows
  are 64 B and 64 consecutive node ids share a 4 KiB page, so hitting the same
  DRAM row requires neighbour ids within ±16 of each other. They are not:
  `ogbn-products` node ids follow the original ASIN ordering, which carries no
  relation to the co-purchase graph. This is the clearest confirmation of the
  brief's warning — **graph adjacency does not imply physical locality**.

The runtime is *exactly* walk-bound: `walks × 150` reproduces the cycle count to
within 0.04 % in all four primary cells and all four uniform cells. There is no
residual to attribute to anything else, so every speedup in §5 is a walk-count
reduction and nothing else.

Why the walk reduction is only −11…−36 % here, against −80 % in M23-C: the window
de-duplicates *pages*, and a page spans 64 consecutive node ids. In com-Orkut the
touched pages recur inside a 32 K-access window often; here 38 246 pages are
spread over 1.2 M gathers with a 42.5 % line-dup rate, so the same page recurs at
a distance that frequently exceeds the window. `bulk_dedup_merged` behaves
accordingly: 6 430 merges at 8 K, 25 520 at 32 K — 4× the merges for 4× the
window, but only 2 % of the 1.2 M gathers.

De-duplication is not the only effect, though. At 32 K the DRAM row-hit rate
rises from 0.14 % to 18.4 % and activations fall 5 %, which means the batched
issue order is grouping same-row requests that the SIMT path issued far apart.
That is a *scheduling* benefit, orthogonal to walk count, and it is the reason
DRAM busy cycles stay flat (31.0 M → 32.3 M) while total cycles fall 36 %.

## 7. Compiler selection

| | instructions | BULK | SIMT | elements Bulk / total |
|---|---:|---:|---:|---:|
| primary sampler | 303 162 | 302 827 (99.89 %) | 335 (0.11 %) | 1 199 005 / 1 200 003 |
| uniform sampler | 317 135 | 293 599 (92.6 %) | 23 536 (7.4 %) | 1 140 521 / 1 200 001 |

Rule: degree ≥ 8 → Bulk. Under the popularity-skewed train split almost every
target qualifies; under uniform sampling 7.4 % of instructions (degree < 8) stay
on SIMT and their 59 480 gathers never enter the engine — question 8 is answered
by construction and by the 998 / 59 480 element split.

Question 9: **no cell is slower than M0.** The weakest gain is 1.10× (uniform,
8 K); nothing regresses.

## 8. Answers to the analysis questions

1. **Useful locality across independent targets?** Line reuse yes (42.5 %),
   page reuse only weakly, DRAM row reuse essentially none (0.14 %).
2. **Feature-address relationships from the window?** Yes but weakly: at 32 K
   only 25 520 merges, 2 % of gathers. The window's measurable effect is walk
   count, not merge count.
3. **Page-aware organisation reduce translation?** Yes — walks −11/−21/−36 %,
   and since runtime = walks × 150 exactly, that *is* the speedup.
4. **Larger window improve cache/DRAM?** Mixed and mostly negative: L2 hit rate
   falls 66 % → 51 % (merging before issue removes lookups that would have hit),
   DRAM reads rise 8–15 % (L1 bypass), while DRAM row hits improve 0.14 % →
   18.4 % and activations fall 5 %. DRAM is only 18–28 % of runtime, so the
   worse cache behaviour costs far less than the walks it buys.
5. **Does feature-vector size change the usefulness of Bulk?** Not simulated
   (16 of 100 dims). Analytically the full 400 B row is 6.25 lines per
   neighbour: DRAM bytes ×6.25, page span 156.7 MB → 979.6 MB (≈ 239 k pages,
   467× the TLB), so walks per gather rise and the *absolute* win grows while
   the mechanism is unchanged. Index traffic is per-neighbour and would not
   change. This is the single largest unmeasured effect in M23-D.
6. **Does benefit vary with degree or sampling policy?** Yes, modestly:
   1.552× (mean degree 141.9) vs 1.481× (mean degree 29.1) at 32 K; row hits
   18.4 % vs 15.7 %; SIMT share 0.1 % vs 7.4 %. Direction is the same under
   both samplers, so the conclusion is not an artefact of the skewed split.
7. **Best capacity/trade-off?** 32 K in both samplers (1.552× / 1.481×), with
   occupancy at 98.9 % of capacity and per-doubling gains still rising
   (1.124 → 1.267 → 1.552). The knee is not located; no recommendation is made
   from one workload.
8. **Does selection keep unsuitable work on SIMT?** Yes — 7.4 % of uniform
   instructions, 59 480 gathers, zero engine elements.
9. **Is Bulk ever slower than M0?** No.

## 9. Correctness and accounting

* `conservation = OK`, `conservation_violations = 0` in all eight cells.
* `bulk_addr_mismatch = 0` in all six DX cells: every element the engine fetched
  carries the address the SIMT path would have used.
* Element accounting exact: primary 1 199 005 Bulk + 998 SIMT = 1 200 003
  gathers; uniform 1 140 521 + 59 480 = 1 200 001.
* Index traffic charged, not free: 74 938 index lines = 4 796 032 B (primary),
  71 283 = 4 562 112 B (uniform), matching `ceil(1 199 005 × 4 / 64)`.
  Independence of the index stream is visible in the DRAM client split: 74 938
  index reads with 70 260 row hits (93.7 %), against 1 173 348 target reads with
  137 470 hits (11.7 %).
* Determinism: re-running DX‑8K uniform without `dram_diag` reproduces
  160 417 625 cycles exactly.
* Aggregation reference: `Σ` over the real 16-dim vectors, hashed per target —
  `5bc346b4…` (primary, 8 455 targets) and `f933c02a…` (uniform, 41 285
  targets). The simulator does not recompute this sum (§4), so equality of
  *addresses* (`bulk_addr_mismatch`, conservation) is what is verified across
  configurations; the checksum pins the data the trace was built from.
* Node/adjacency verification: the CSR used for emission is derived from
  `edge.csv.gz` by a stable sort on `src`; neighbours are emitted in file order;
  the 8 455-target subset and its 1 200 003 gathers are reproduced bit-for-bit
  by re-running the builder (`trace_sha256 16bd09ec…`, uniform `b1558e84…`).

## 10. Limitations

1. **16 of 100 feature dims.** One line per neighbour. The full 400 B vector is
   not simulated; §8.5 gives the analytic scaling only. Absolute cycle counts are
   therefore an under-estimate of the real GNN gather by roughly the line-count
   ratio, and the *shape* of the result (walk-bound, weak reuse) would be
   preserved but not measured.
2. **Single indirection.** `colidx → neighbour id → feature row` is collapsed to
   `neighbour id → feature row` before the kernel. The two-hop version a real
   sampler issues (read `colidx`, then gather) is not representable; M23-C's
   report makes the same point for BFS.
3. **Sampling and CSR build are outside the trace** (§4). No claim is made about
   sampler cost.
4. **Aggregation arithmetic unmodelled** on both paths; fanout scheduling only.
5. **The `time` split is absent from the archive**; the `sales_ranking` split is
   used and its popularity skew is reported rather than corrected, with a uniform
   sampler run alongside it.
6. **Directed edge use as stored** — one row per co-purchase pair, not
   symmetrised.

## 11. Reproducibility

```bash
# dataset (1.48 GB), then extract only the four members used
curl -sL -o data/ogb/products.zip https://snap.stanford.edu/ogb/data/nodeproppred/products.zip
cd data/ogb && unzip -o -q products.zip products/raw/edge.csv.gz \
    products/raw/node-feat.csv.gz products/split/sales_ranking/train.csv.gz

# traces
python3 tools/m23_build_ogbn.py --graph data/ogb/products/raw/edge.csv.gz \
    --feat data/ogb/products/raw/node-feat.csv.gz \
    --split data/ogb/products/split/sales_ranking/train.csv.gz \
    --targets 65536 --max-accesses 1200000 --out build/wm/m23_ogbn.bin
python3 tools/m23_build_ogbn.py --graph data/ogb/products/raw/edge.csv.gz \
    --feat data/ogb/products/raw/node-feat.csv.gz \
    --targets 65536 --max-accesses 1200000 --out build/wm/m23_ogbn_uniform.bin

# runs (from sim/)
W="--set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
   --set bulk_max_elems=2097152 --set dram_diag=1"
for c in m0 dx8k dx16k dx32k; do
  ./bin/dx100sim --config configs/m23_$c.cfg $W \
      --set dataset=../build/wm/m23_ogbn.bin --all > ../build/wm/m23d_train_$c.txt
done
```

Logs: `build/wm/m23d_train_{m0,dx8k,dx16k,dx32k}.txt`,
`build/wm/m23d_unif_{m0,dx8k,dx16k,dx32k}.txt`,
`build/wm/m23d_unif_dx8k_nodiag.txt`. Trace metadata including SHAs:
`build/wm/m23_ogbn*.bin.json`.
