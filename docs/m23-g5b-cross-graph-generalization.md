# M23-G5B — Cross-Graph Generalization

M23-G3 characterized the Bulk organization policies on Orkut; G4B/G4C showed what
the DRAM map and page-first ordering do to that result. This milestone asks
whether the **row-aware organization (P2) generalizes to a different graph** — a
different degree distribution — with the same builder, the same resource
configuration and the same translation assumption.

**Short answer.** The *mechanism* generalizes: P2 raises the DRAM row-hit rate
whenever the incoming stream lacks row locality, on LiveJournal by +4.1/+6.4/+8.1
points at 8K/16K/32K. The *benefit* does not generalize uniformly — it is
−2.26/−3.67/−5.09 % of cycles here, against 0.00 % on Orkut and −14.5/−22.2/−28.8 %
on ogbn-products. The determining quantity is how much row locality the trace's
own order already exposes, measured directly as P0's row-hit rate and structurally
as line reuse (accesses per unique line: 11.97 Orkut, 5.86 LiveJournal, 1.62
ogbn). In the deployable (real-translation) configuration the G3 regression
reproduces with the same mechanism and a smaller relative cost: **+13.96 %** at
16K against Orkut's +43.38 %.

---

## 1. Dataset selection

Availability was checked with a HEAD request before any download, for the two
candidates the brief named or implied:

| candidate | HTTP | size | decision |
|---|---|---:|---|
| `soc-LiveJournal1.txt.gz` | 200 | 259 619 239 B | **selected** |
| `soc-pokec-relationships.txt.gz` | 200 | 132 454 730 B | not downloaded (one dataset only, as instructed) |

Selected graph: **SNAP soc-LiveJournal1** (social network, 2008 snapshot),
`data/livejournal/soc-LiveJournal1.txt.gz`, SHA-256 `d7bcd5a8 7b88c896 …`.
4 847 571 vertices, 68 993 773 entries in the file (each unordered pair is stored
once with a direction; the file's own header says so), read as an edge list and
symmetrized exactly as Orkut was.

Why it is the right contrast to Orkut: LiveJournal is a sparse, low-median-degree
social graph, Orkut a dense, high-mean/high-tail one. Same vertex-property gather,
same 64 B record, same access budget — only the structure differs.

## 2. Workload construction

`tools/m23_build_g5b.py` reproduces M23-C's construction (`m23_build_orkut.py`)
rule for rule; only the dataset identity is parameterised, and it is a required
argument so the metadata cannot silently describe another graph.

* subset: **BFS-grown induced subgraph** from the maximum-degree vertex
  (vertex 10 009, degree 22 889), taking vertices in discovery order until
  1 048 576 are selected — levels 0…k−1 plus a prefix of level k;
* every edge with both endpoints selected is kept: **27 976 721** undirected
  induced edges (55 953 442 directed CSR arcs);
* kernel: one level of indirection, `out[j] = prop[adj[j]]`, one **64 B**
  property record per vertex = one cache line per neighbour visit;
* emission: per selected vertex its full adjacency list in 4-lane instruction
  chunks, BULK when the subset degree is ≥ 8 (99.97 % of instructions);
* `--max-accesses 1200000` caps admitted neighbour slots — the same cap as M23-C,
  so this run executes the same 1 200 003 gather accesses as Orkut.

**Excluded** (preprocessing, charged to no configuration): edge-list parse, CSR
construction, BFS level expansion, subset selection, frontier bookkeeping.

Layout (VA): `prop` [0, 67 108 864), `rowptr` at 67 108 864, `colidx` at
71 303 232, `frontier` at 295 117 056, `max_addr` 299 311 360.

## 3. Workload characteristics

| item | LiveJournal | Orkut (M23-C, unchanged) |
|---|---:|---:|
| graph vertices / file entries | 4 847 571 / 68 993 773 | 3 072 441 / 117 185 083 |
| subset vertices | 1 048 576 | 131 072 |
| induced undirected edges | 27 976 721 | 5 866 045 |
| subset mean degree | 53.36 | 89.51 |
| subset median / p90 / p99 degree | 20 / 127 / 496 | 28 / 214 / 809 |
| subset max degree | 22 889 | 33 313 |
| selected vertices (gather sources) | 5 572 | 3 099 |
| selected degree mean / median / max | 215.4 / 93 / 22 889 | 387.3 / 150 / 33 313 |
| property array (one 64 B record/vertex) | **64.00 MiB** | 8.00 MiB |
| DRAM mapping period = banks × rows × row_bytes | 8 MiB | 8 MiB |
| property array / period | **8.00 periods** | 1.00 period |
| touched lines / touched footprint | 204 951 / **12.51 MiB** | 100 282 / 6.12 MiB |
| touched footprint / period | 1.56 periods | 0.77 periods |
| distinct (bank,row) touched / 8192 | **8192 (100 %)** | 8181 (99.9 %) |
| distinct rows touched / 4096 | 4096 (100 %) | 4096 (100 %) |
| address span of the touched set | 14.53 MiB | 8.00 MiB |
| touched 4 KiB pages | 3 720 | 2 048 |
| bank balance over touched lines | 102 472 / 102 479 (0.00 %) | 50 080 / 50 202 (0.12 %) |
| **accesses** | **1 200 003** | 1 200 003 |
| **duplicate-line fraction** | **0.829** | 0.916 |
| line reuse (accesses / unique line) | **5.86** | 11.97 |

LiveJournal's file also contains **duplicate entries**: 22 of the 302 115 emitted
instructions have two lanes gathering the same neighbour, i.e. the same 64 B line
(Orkut and ogbn have none). They are kept — the workload retains every selected
edge, duplicates included — and the simulator treats them as hardware does: the
window dedup merges them in the Bulk path, the L1 coalescer merges the two lanes
into one request in the SIMT path. That is why the SIMT baseline reports
1 199 981 completed operations for 1 200 003 lane accesses, and why Orkut's SIMT
run was exact.

Mapping-period coverage is therefore complete in both graphs — every one of the
4096 rows in both banks is touched — so the difference between them is not period
aliasing but *how much of the period a window of local work spans*. Orkut's whole
touched set fits inside 0.77 of a period; LiveJournal's spans 1.56 periods with
all 8192 (bank,row) pairs live.

## 4. Configurations and measurement identity

`sim/configs/m23g5b_base.cfg` is `m23g3_base.cfg` with a new header comment:
`diff` of the two files with comments and blank lines stripped is **empty**, so
R = 128, L1 256×4-way/128 MSHR, L2 16384×8-way/64 MSHR, dTLB 16×4-way, l2TLB
512×4-way, 2 walkers × 2 levels × 150 cycles, 2 banks × 4096 rows × 1024 B,
t_hit 14 / t_miss 26, FR-FCFS, unbounded queue, page_dir 512, fused two-buffer
window with dedup/page scheduling, and `ideal_translation = 1` are identical to
G3's. Only `batch_window` and `bulk_org` move, on the command line:

* **SIMT** `dispatch = 0`;
* **P0** `bulk_org = 0` (published order, ascending VA line);
* **P2** `bulk_org = 2` (DRAM (row,bank) first);
* **P3** `bulk_org = 3` (combined) — included because its implementation is stable
  on this workload (all cells clean, no DNF);
* windows 8 192 / 16 384 / 32 768 elements; `bulk_index_base = 0x80000000`,
  `bulk_desc_line = 0x1FFFFC0`, `bulk_max_elems = 2097152`.

The second arm repeats P0/P2/P3 at 16K under `configs/m23_dx16k.cfg` (real
translation: dTLB, walks, page directory), the deployable configuration, exactly
as G3's `g3p_*` arm did.

## 5. Results — ideal translation (M23-G2D reference setting)

SIMT baseline: **7 185 363** cycles — 596 984 DRAM reads, 17.17 % row hits,
L2 hit 34.4 % (312 533 / 909 517), 38.21 MB, 5.317 B/cycle.

| W | P | cycles | ×SIMT | row hit | conflicts | DRAM cmds | mean svc | B/cyc | bank busy | L2 hit | dedup | org cost | win util |
|---:|:--|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 8K | P0 | 7 453 804 | 0.964 | 37.45 % | 429 302 | 686 373 | 21.51 | 5.893 | 99.02 % | 19.38 % | 35.30 % | 8.05 % | 99.62 % |
| 8K | P2 | 7 285 677 | 0.986 | **41.53 %** | 401 441 | 686 576 | 21.02 | 6.031 | 99.03 % | 19.27 % | 35.38 % | 10.89 % | 99.62 % |
| 8K | P3 | 7 286 821 | 0.986 | 41.53 % | 401 432 | 686 583 | 21.02 | 6.030 | 99.01 % | 19.29 % | 35.36 % | 10.89 % | 99.62 % |
| 16K | P0 | 7 110 176 | 1.011 | 46.21 % | 371 514 | 690 668 | 20.46 | 6.217 | 99.35 % | 14.26 % | 39.12 % | 8.44 % | 98.96 % |
| 16K | P2 | 6 848 866 | 1.049 | **52.63 %** | 327 392 | 691 164 | 19.68 | 6.459 | 99.32 % | 14.22 % | 39.10 % | 11.43 % | 98.96 % |
| 16K | P3 | 6 848 048 | 1.049 | 52.63 % | 327 346 | 691 109 | 19.68 | 6.459 | 99.33 % | 14.22 % | 39.11 % | 11.43 % | 98.96 % |
| 32K | P0 | 6 782 849 | 1.059 | 58.81 % | 293 465 | 712 472 | 18.94 | 6.723 | 99.49 % | 4.81 % | 43.87 % | 8.84 % | 98.96 % |
| 32K | P2 | 6 437 400 | 1.116 | **66.88 %** | 236 116 | 712 887 | 17.98 | 7.088 | 99.53 % | 4.73 % | 43.89 % | 11.93 % | 98.96 % |
| 32K | P3 | 6 437 232 | 1.116 | 66.87 % | 236 129 | 712 822 | 17.98 | 7.087 | 99.52 % | 4.74 % | 43.89 % | 11.93 % | 98.96 % |

Translation is free in this arm (`walks = 0`, `tlb0_misses = 0`), by construction:
that is what makes the cycle delta attributable to the request order.

"There is no reuse left to harvest": P3 is identical to P2 to within 0.01 %, the
same finding as G3 — the cache-line key is inert once the (bank,row) cell is open.

Both Bulk policies beat SIMT only at 32K; at 8K they are *slower* than SIMT
(0.96–0.99×), a pre-existing property of the fused engine already recorded in
M23-F1/G3, not a result of this milestone.

## 6. Results — published configuration (real translation, 16K)

SIMT baseline: 67 234 692 cycles, 600 733 reads, 16.31 % row hits, L2 hit 67.6 %
(1 254 996 / 1 855 906), 443 163 walks, 975 461 dTLB misses (18.7 % hit rate).

| P | cycles | ×SIMT | row hit | conflicts | DRAM cmds | mean svc | bank busy | L2 hit | walks | dTLB misses | sched opens | sched idle | starved |
|:--|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| P0 | 28 309 390 | 2.375 | 46.12 % | 375 629 | 697 219 | 20.47 | 25.20 % | 40.75 % | 186 923 | 317 832 | 185 913 | 27 829 747 | 78.10 % |
| P2 | 32 260 832 | 2.084 | 50.84 % | 342 940 | 697 595 | 19.90 | 21.51 % | 40.73 % | 186 899 | 316 016 | 346 481 | 31 823 410 | 79.37 % |
| P3 | 32 229 545 | 2.086 | 51.05 % | 341 452 | 697 569 | 19.87 | 21.51 % | 40.73 % | 186 917 | 315 727 | 346 515 | 31 791 926 | 79.37 % |

P2 improves the row-hit rate by +4.7 points and reduces conflicts by 32 689, and
still costs **+13.96 %** cycles. The signature is G3's, not a new one: `walks`
(186 923 → 186 899) and `dTLB misses` (317 832 → 316 016) are unchanged, so the
translation stream is identical; what changes is `sched_opens` (+160 568) and
`sched_idle` (+3.99 M cycles), and bank busy falls 25.20 % → 21.51 %. Forming
(bank,row) cohorts fragments the issue stream faster than the reduced conflict
count pays for it.

## 7. Attribution — where the cycles go

Every avoided row conflict shortens that command's service by
`t_hit − t_miss` = 12 cycles. The DRAM runs both banks at ~99 % occupancy, so a
service-time reduction shows up as that reduction divided by `banks × busy`, and
the organizer's own cycles are overlapped inside the same window
(`org_overlap = 1`).

| W | conflicts avoided | Δ mean service (cyc/cmd) | service time saved (cmds × Δsvc) | banks × busy | predicted Δcycles | measured Δcycles |
|---:|---:|---:|---:|---:|---:|---:|
| 8K | 27 861 | −0.490 | 336 323 | 1.981 | −169 776 | **−168 127** (−1.0 %) |
| 16K | 44 122 | −0.771 | 532 505 | 1.987 | −268 031 | **−261 310** (−2.5 %) |
| 32K | 57 349 | −0.968 | 689 673 | 1.990 | −346 570 | **−345 449** (−0.3 %) |

Conflicts avoided, mean service and bank busy are measured; the predicted column
is arithmetic on them. The organizer does cost real work — `bulk_org_cycles` goes
from 599 882 (P0, the same at every window) to 793 762 / 782 576 / 768 192, i.e.
the new ordering costs **+168 k to +194 k cycles** — but those cycles hide in the
DRAM service window instead of adding to the run, which is why the cycle delta
tracks the service saving almost exactly.

The conclusion that matters: on LiveJournal the row grouping removes real DRAM
service time and the system realizes essentially all of it (50 % of the
per-command saving, which is 100 % of what a two-bank DRAM can turn into
wall-clock time). It is no longer a wash as on Orkut, and it is still short of
the 97–100 % window-ceiling capture G3 measured on ogbn.

## 8. Cross-graph comparison

The Orkut column is read from `build/wm/m23g3/summary.csv` (this milestone did
not re-run or modify any Orkut cell).

### 8.1 Structure → benefit

| graph | accesses | unique lines | reuse | dup frac | footprint | periods | P0 row hit (16K) | P2 row hit | **P2 Δcycles** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut | 1 200 003 | 100 282 | 11.97 | 0.916 | 6.12 MiB | 0.77 | 60.52 % | 60.53 % | **−0.02 %** |
| LiveJournal | 1 200 003 | 204 951 | 5.86 | 0.829 | 12.51 MiB | 1.56 | 46.21 % | 52.63 % | **−3.67 %** |
| ogbn-products (G3) | 1 200 001 | 739 041 | 1.62 | 0.384 | 45.11 MiB | 5.64 | 10.07 % | 55.60 % | **−22.21 %** |

### 8.2 The relation holds at every window (ideal translation)

| window | Orkut P0 → P2 | LiveJournal P0 → P2 | ogbn P0 → P2 |
|:--|---:|---:|---:|
| 8K | 49.63 % → 49.64 %, +0.00 % (+167 cyc) | 37.45 % → 41.53 %, −2.26 % | 7.78 % → 37.75 %, −14.49 % |
| 16K | 60.52 % → 60.53 %, −0.02 % | 46.21 % → 52.63 %, −3.67 % | 10.07 % → 55.60 %, −22.21 % |
| 32K | 73.68 % → 73.70 %, −0.02 % | 58.81 % → 66.88 %, −5.09 % | 14.43 % → 72.47 %, −28.82 % |

### 8.3 The deployable regression also transfers

| graph | published 16K P0 | P2 | Δ |
|---|---:|---:|---:|
| Orkut | 16 714 538 | 23 965 633 | **+43.38 %** |
| LiveJournal | 28 309 390 | 32 260 832 | **+13.96 %** |
| ogbn-products | 144 443 603 | 144 828 967 | +0.27 % |

The added organizer work is roughly graph-independent (Orkut +178 755 extra
`sched_opens`, LiveJournal +160 568); what differs is the baseline length the same
absolute cost is charged against, which is why the same mechanism reads +43 %,
+14 % or +0.3 %.

## 9. What generalizes, and what predicts it

**Generalizes.**
1. The mechanism. Grouping a window by DRAM (row,bank) raises the row-hit rate
   monotonically with window size on every graph tested: +0.02/+0.01/+0.02 pt
   (Orkut), +4.1/+6.4/+8.1 pt (LiveJournal), +30.0/+45.5/+58.0 pt (ogbn).
2. P3 ≡ P2 to within 0.05 % — the reuse key is inert in all three graphs.
3. Window utilization is 99 % and organization overhead stays single-digit
   (8–12 % ideal, 2–2.4 % published) — the cost of the policy is not what breaks
   it.
4. Under real translation, P2 loses to P0 through the same route on both graphs:
   unchanged walks and dTLB misses, more scheduler opens, lower bank busy.

**Does not generalize.** The size of the benefit. It is not a property of "row
awareness" but of the *headroom*: a graph whose access stream already exposes row
locality has nothing for the organizer to recover.

**Predictor.** Two equivalent quantities, both measurable from the trace before
any simulation:

* **line reuse** `accesses / unique lines` — Orkut 11.97 → −0.02 %, LiveJournal
  5.86 → −3.67 %, ogbn 1.62 → −22.21 %;
* equivalently **P0's own row-hit rate at the operating window** — 60.5 % → 0.0 %,
  46.2 % → −3.7 %, 10.1 % → −22.2 %.

A workable rule from these three points: P2 pays when the untransformed order
leaves more than roughly half the window's row hits on the table (P0 < ~30 %),
is a wash when P0 is already above ~60 %, and sits between the two on
LiveJournal. The structural drivers are the duplicate-line fraction (0.92 / 0.83 /
0.38) and the footprint relative to the 8 MiB period (0.77 / 1.56 / 5.64) — a
footprint inside one period with high reuse makes ascending-address order
approximately row order, which is exactly the case that has nothing to gain.

**LiveJournal sits where the rule says it should**: −2.3 % to −5.1 % ideal, and a
+14 % regression (not Orkut's +43 %) in the deployable configuration.

## 10. Accounting and correctness

`build/wm/m23g5b/checks.txt`, one line per cell; 12 cells, **0 failures, 0 missing
fields**. Per cell:

* `conservation_ok = 1`, `conservation_violations = 0`;
* every logical element accounted for exactly once:
  `bulk_elems_done = bulk_elems_total = 1 199 762`, and
  `1 199 762 = 730 475 target requests + 469 287 merged duplicates` at 16K —
  the remaining 241 accesses of the 1 200 003-access trace are the SIMT tail
  (`simt_instr = 77`), so all 1 200 003 accesses are issued across the two paths;
* `bulk_tgt_req_done = bulk_tgt_req_gen`, `bulk_addr_faults = 0`,
  `bulk_addr_mismatch = 0` (the engine re-derives every address from the index
  image and compares), `bulk_residency_violations = 0`;
* `dram_bytes = 64 × dram_reads` in every cell, and
  `|dram_wait_sum − dram_q_occ_sum| ≤ 0.5 %` of the queue-occupancy sum;
* `bulk_win_window` equals the configured window in every cell.

The two SIMT baselines are the only cells outside this table, because they have no
window or bulk counters. Their completion is accounted for separately:
1 200 003 lane accesses → 1 199 981 completed memory operations, the difference
being the 22 same-line lane pairs of the duplicate entries above, merged by the
L1 coalescer. Zero accesses are unserved.

## 11. Reproduction

```
# dataset (verified before use: 259 619 239 B, sha256 d7bcd5a8…)
curl -sSL -o data/livejournal/soc-LiveJournal1.txt.gz \
  https://snap.stanford.edu/data/soc-LiveJournal1.txt.gz

# trace (deterministic; 5 572 sources, 1 200 003 accesses, sha256 7363867a…)
python3 tools/m23_build_g5b.py --edges data/livejournal/soc-LiveJournal1.txt.gz \
  --cache /tmp/g5b_edges.npy --dataset "SNAP soc-LiveJournal1 (social network, 2008 snapshot)" \
  --url https://snap.stanford.edu/data/soc-LiveJournal1.txt.gz \
  --graph-vertices 4847571 --graph-edges-file 68993773 \
  --vertices 1048576 --max-accesses 1200000 --policy bfs \
  --out build/wm/m23_livejournal_bfs.bin

./build/wm/m23g5b_run.sh all 6      # 10 cells, ideal translation
./build/wm/m23g5b_run.sh puball 4   #  4 cells, published configuration
python3 tools/m23g5b_extract.py     # checks, summary.csv, attribution.csv, crossgraph.csv
```

One cell takes ~145 s wall on this machine (ideal translation). Artefact hashes:
`build/wm/m23g5b/hashes.txt`; trace and config hashes also in
`build/wm/m23_livejournal_bfs.bin.json` and the table below.

| artefact | sha256 |
|---|---|
| `build/wm/m23_livejournal_bfs.bin` | `7363867a364c7a9b09dc3b73f5b62ca7272d957b6abd283a85d01b8dc0923ad1` |
| `data/livejournal/soc-LiveJournal1.txt.gz` | `d7bcd5a87b88c896c35fdb9611e804c3f4033c39b58c4c9ea3ba53c680d516d8` |
| `sim/configs/m23g5b_base.cfg` | `5db9dd13e78b215ec63bf5137a4fff2f875de8b6809838c0d1b0f5626a9dfe26` |
| `tools/m23_build_g5b.py` | `2bae4f125d1a7837f36554d48537aca1c7176b3521c513eb0dcfcaa67ebbd576` |
| `tools/m23g5b_extract.py` | `9ad9ad0625ea05143970a99c0f4e7f614a3c4f0b3b5c6482d8e8dad190721ea0` |
| `build/wm/m23g5b/summary.csv` | `9da5cfb823895470d309ca66b13c331a3dc836c8a84718db80242459e45c999e` |

## 12. Preservation of the Orkut results

`build/wm/m23_orkut_bfs.bin` (sha256 `11cc8da4…`) and every `build/wm/m23g3/*`
cell were read only. The Orkut column of §8 comes from G3's own `summary.csv`,
so the comparison cannot drift from the published run. `configs/m23g3_base.cfg`
is untouched (its hash is in `hashes.txt` alongside the new config).

## 13. Limitations

* One graph, one subset size, one seed. The three-point relation of §9 is
  consistent and monotone but it is still three graphs, not a fitted law;
  treat the "P0 < ~30 %" threshold as a description of these points.
* The gather is one level of indirection, as the engine supports; the second-level
  `dist[adj[j]]` is excluded and charged to nobody, so this is not a full BFS.
* Ideal-translation cells have no dTLB/walk cost by construction: they isolate the
  DRAM ordering mechanism, and are not a performance claim. The real-translation
  arm is one window size (16K) per policy.
* Property records are 64 B; a wider feature vector would change the footprint and
  is M23-G5A's subject, not this one.
* These are simulator cycles for a modelled DX-100 memory system. No physical GPU
  was measured and no physical-GPU performance is claimed.
