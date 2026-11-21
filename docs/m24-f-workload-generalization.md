# M24-F — M24 Bulk workload generalization

**Scope.** An evaluation of where the production M24 Bulk architecture (M23-G6D S1) beats
conventional SIMT on matched resources, where it loses, and which workload properties
actually predict the outcome. No architecture was changed, no policy was tuned, and no
historical trace or result was modified.

**Answer.** The advantage is not a property of "indirect access" and it is not driven by
duplication. On 31 traces spanning 4 KiB to 45 MiB it is driven by one thing: **whether the
gather stream arrives in an order the page table and the DRAM cannot already exploit.**

* When the target set spans ≥ 1× L2 and the stream is *not* sequential, Bulk wins
  **1.22–3.28×**, because cohort admission cuts page walks by 1.2–7.6× and physical order
  lifts the row-hit rate from 0.1–18.6 % to 9.8–82.1 %.
* When the target set is L1/L2-resident, Bulk gains **1.01–1.05×** — real but, as M24-E
  already showed, not what the architecture is for.
* When the stream is *already sequential* at scale, Bulk **loses 2.5–4.4 %**: SIMT's
  coalescer already gets 90 % row hits, and the engine's index reads are pure added traffic.
* When the hot set is L2-resident but the tail is not, Bulk **loses 18 %**: sealing into page
  order re-fetches lines that SIMT was hitting in L2.
* Real SpMV with high intra-window reuse loses **8–16 %** for the same reason plus a larger
  index share.

The two most important cells are `w4_duplow` and `n1_seq8m`: **identical working set
(8 MiB), identical page count (2 048), identical line-duplicate rate (0.000)** — one random,
one sequential. The random one runs **2.504× faster** on Bulk; the sequential one **0.975×**.
Everything else in this report is a variation on that comparison.

## 1. What was already covered, and what was not

### 1.1 Evidence audited

| source | what it contributes | reused how |
|---|---|---|
| **M18 / M19** | eight real application traces (four SpMV, four BFS) with per-trace JSON records | the four SpMV traces are run here; the BFS traces carry no single-level work (their gathers depend on the frontier) and are excluded — see §12 |
| **M21** | ten synthetic single-level patterns × 3 seeds, 131 072 gathers each, plus their offline structure table | all ten at seeds 1 and 2 are run here; the offline statistics are **recomputed** by `tools/m24f_stats.py` and reproduce M21's published numbers exactly (w1 `dup(word)` 0.8750, `dup(line)` 0.9844) |
| **M23** | the four real workloads of M24-D, plus `orkut_hub`, `orkut_tail`, `ogbn_uniform`, `ogbn_train_100d`, `ogbn_uniform_100d`, `criteo_2k` | four are run here; the four M24-D primaries and their map-M3 cells are **reused unchanged** (§10) |
| **M24-A** | the page/DRAM factor decomposition and four address maps | the map dimension is quoted, not re-run (M24-D already ran both arms on map M3) |
| **M24-C / M24-D** | the resource-matched pair and its four speedups (2.921 / 2.516 / 1.266 / 2.222) | the comparison configuration for every cell here; the four primaries are quoted as the anchor |

**Nothing already measured was re-measured.** The 31 new cells exist because the existing
matrix has a structural hole, not because the results were in doubt.

### 1.2 The hole

The existing matrix covers the *working-set* axis well (4 KiB to 8 MiB) and the *pattern*
axis thoroughly at small footprint (ten synthetic families), but every structured pattern
(sequential, clustered, zipf) was generated at ≤ 2 MiB. Two gaps follow:

1. **Structured access at a footprint beyond L2 and beyond TLB reach.** There was no trace
   that is simultaneously large *and* ordered, so the interaction between "the stream is
   already good" and "the footprint is too big to cache" was unmeasured. That is the corner
   where the architecture is most at risk, and it is where it loses.
2. **A controlled random-vs-sequential pair at fixed footprint.** `w4_duplow` (8 MiB, random,
   131 072 distinct lines) had no sequential counterpart, so the 2.5× it shows could not be
   attributed to order rather than to size.

Five traces were built to close both, all at the M21 control (131 072 gathers, 8 192 rows of
16, 4 lanes, seed 1, same instruction stream — only the index values differ):

| id | pattern | parameters | why |
|---|---|---|---|
| `n1_seq8m` | sequential | 8 388 608 cols, stride 8 | 8 MiB, 2 048 pages, **dup 0.000, seq 1.000** — the counterpart of `w4_duplow` |
| `n5_seq8m_s4` | sequential | 8 388 608 cols, stride 4 | 4 MiB, seq 0.500 — the counterpart of `n4_uni4m` |
| `n4_uni4m` | uniform | 524 288 cols | 3.5 MiB, dup 0.567 — beyond L2, still random |
| `n2_zipf4m` | zipf (a = 1.0) | 524 288 cols | 1.5 MiB, dup 0.822 — skew beyond L2 |
| `n3_clu4m` | clustered (90 % in 5 %) | 524 288 cols | 0.89 MiB touched over 1 024 pages (4 MiB address space) |

## 2. Workload taxonomy

Nine families, defined by measured structure rather than by name. "WS" is the target
working set; "pages" counts 4 KiB target pages; `dup` is the line duplicate rate; `seq` is
the fraction of consecutive target accesses whose line is the previous line + 1.

| family | traces | WS | pages | dup | seq | what it stresses |
|---|---|---|---|---|---|---|
| **S — L1-resident, page-dense** | w3, w7, w9, w10, qc2534 | 4–20 KiB | 1–5 | 0.998–1.000 | 0.004–0.124 | nothing: both arms hit L1 |
| **M — L2-resident, page-dense** | w1, w2, w6, bcsstk25, lnsp3937, dubcova3 | 31–352 KiB | 8–88 | 0.981–0.994 | 0.000–0.202 | L2 hit rate |
| **V — L2-sized, page-spread** | w5, criteo_2k, n2 | 0.76–1.42 MiB* | 256–1 945 | 0.822–0.875 | 0.013–0.125 | L2 boundary + many pages |
| **B — beyond L2, unordered** | w4, n4, w8, orkut_hub | 1.97–8.00 MiB | 512–2 048 | 0.000–0.921 | 0.000–0.181 | page walks and DRAM order |
| **H — beyond TLB reach** | ogbn_uniform | 45.1 MiB | 38 234 | 0.384 | 0.000 | 74.7× the 512-page TLB reach |
| **Q — already sequential** | n1, n5 | 4.00–8.00 MiB | 1 024–2 048 | 0.000–0.500 | 0.500–1.000 | nothing for the engine to fix |
| **C — clustered hot/cold** | n3 | 0.89 MiB over 4 MiB of address space | 1 024 | 0.889 | 0.000 | L2-resident hot set, scattered tail |
| **G — graph frontiers** | orkut_hub, orkut_tail (+ 3 M24-D primaries) | 0.34–5.77 MiB | 1 742–2 048 | 0.424–0.921 | 0.001–0.181 | degree distribution |
| **E — embedding tables** | criteo_2k (+ Criteo 16k primary) | 0.76 MiB (16k: 4 MiB table) | 1 945 | 0.844 | 0.021 | index-driven reuse |

\* criteo_2k's 777 KiB footprint sits below L2 but touches 1 945 pages, 3.8× the TLB
reach — the one trace where the two axes disagree, and it is the largest win in the matrix.

## 3. Method — the fair comparison

**The pair.** `build/wm/m24d/m24d_bulk.cfg` (production G6D S1) against
`build/wm/m24d/m24d_simt.cfg` (M0-RM). M24-D audited these two files: 94 declared keys, the
**same key set and order**, and exactly **8 differing values**, every one of them a Bulk
organizational mechanism (`dispatch`, `batch_window`, `win_fused`, `page_sched`,
`page_cohorts`, `dedup`, `page_order`, `bulk_org`). Every shared resource — walkers, dTLB and
L2-TLB MSHRs, both cache hierarchies and their MSHRs, the walk cache, the global outstanding
budget, DRAM geometry, timing, mapping and queue — is byte-identical. This is the only pair
M24-D endorses and it is the only pair used here.

**Per-trace workload fields.** Three fields describe the *workload*, not the architecture,
and both arms receive the same value in every cell:

| field | source | example |
|---|---|---|
| `dataset` | the trace file | `build/wm/m21_w4_duplow_s1_full.bin` |
| `elem_bytes` | the M18TRC1 header (8 B for SpMV/synthetic doubles, 4 B for graph properties) | 8 |
| `bulk_index_base`, `bulk_desc_line`, `bulk_max_elems` | the trace's own JSON record | 8 421 440 / 131 584 / 409 600 |

An M23 graph trace records no descriptor block, so those three keep the configuration's
values — exactly what M24-D ran them with.

**Matrix.** 31 traces × 2 arms = **62 new cells**, plus 4 reused M24-D primary pairs and 24
reused map-M3 cells. `build/wm/m24f_run.sh` (plan / cmds / all / group), manifest in
`build/wm/m24f/traces.tsv`, exact command lines in `build/wm/m24f/commands.txt`.

**Validation** (`tools/m24f_extract.py`, all 31 pairs): both arms report the same
`trace_accesses` and `accesses`; both pass the model's own `conservation=OK`; the dispatch
fields are 1/0 as required; `bulk_elems_done = bulk_elems_total`; the DRAM row identity
`hits + conflicts + 2 = reads` holds in both arms; no cell reached the 500 M cycle guard.
Three cells were re-run (`w4_duplow` bulk, `criteo_2k` simt, `n3_clu4m` bulk) and reproduce
their cycles and completion hashes exactly.

## 4. The matrix

Speedup = M0-RM cycles / Bulk cycles. `row%` is the DRAM row-hit rate,
`eng%` the share of trace accesses dispatched to the engine.

| trace | family | M0-RM | Bulk | speedup | walks b/s | DRAM reads b/s | row% b/s | eng% |
|---|---|---:|---:|---:|---|---|---|---:|
| criteo_2k | V/E | 796 800 | 242 935 | **3.280** | 5 114 / 20 703 | 14 983 / 19 440 | 63.7 / 6.3 | 43.6 |
| orkut_hub | G | 21 802 760 | 7 431 551 | **2.934** | 109 137 / 246 768 | 811 352 / 680 115 | 65.5 / 40.2 | 100.0 |
| w4_duplow_s2 | B | 3 790 091 | 1 509 813 | **2.510** | 16 890 / 99 144 | 162 277 / 156 322 | 62.5 / 0.2 | 32.0 |
| w4_duplow | B | 3 785 634 | 1 511 665 | **2.504** | 16 895 / 99 024 | 162 269 / 156 320 | 62.5 / 0.2 | 32.0 |
| n4_uni4m | B | 2 541 068 | 1 162 203 | **2.186** | 8 693 / 65 839 | 135 054 / 131 419 | 74.2 / 0.6 | 32.0 |
| w8_wslarge | B | 1 278 793 | 876 879 | **1.458** | 1 639 / 2 289 | 108 106 / 107 132 | 82.1 / 18.6 | 32.0 |
| w8_wslarge_s2 | B | 1 277 653 | 878 609 | **1.454** | 1 641 / 2 248 | 108 215 / 106 946 | 82.1 / 18.4 | 32.0 |
| ogbn_uniform | H | 44 793 202 | 36 602 832 | **1.224** | 962 821 / 1 178 543 | 1 356 186 / 1 193 958 | 9.8 / 0.1 | 95.0 |
| lnsp3937 | M | 55 546 | 46 602 | **1.192** | 88 / 88 | 5 512 / 5 511 | 90.0 / 79.7 | 30.2 |
| w2_clustered | M | 218 216 | 207 260 | 1.053 | 426 / 425 | 27 179 / 27 228 | 89.8 / 83.9 | 32.0 |
| w5_sequential | V | 332 877 | 316 231 | 1.053 | 650 / 649 | 41 682 / 41 515 | 91.0 / 89.7 | 32.0 |
| w5_sequential_s2 | V | 332 877 | 316 231 | 1.053 | 650 / 649 | 41 682 / 41 515 | 91.0 / 89.7 | 32.0 |
| w2_clustered_s2 | M | 217 949 | 207 198 | 1.052 | 426 / 425 | 27 175 / 27 217 | 89.8 / 84.0 | 32.0 |
| w1_uniform | M | 215 444 | 204 847 | 1.052 | 426 / 425 | 27 166 / 27 165 | 91.2 / 85.1 | 32.0 |
| w1_uniform_s2 | M | 215 393 | 204 895 | 1.051 | 426 / 425 | 27 166 / 27 165 | 91.1 / 85.2 | 32.0 |
| w6_zipf_s2 | M | 216 460 | 205 951 | 1.051 | 426 / 425 | 27 166 / 27 181 | 90.5 / 84.6 | 32.0 |
| w6_zipf | M | 216 336 | 205 879 | 1.051 | 426 / 425 | 27 166 / 27 181 | 90.5 / 84.6 | 32.0 |
| n2_zipf4m | V | 571 425 | 543 908 | 1.051 | 7 734 / 10 599 | 59 190 / 53 751 | 68.1 / 40.3 | 32.0 |
| w9_seqsmall | S | 193 888 | 191 621 | 1.012 | 398 / 397 | 25 386 / 25 372 | 91.1 / 89.7 | 32.0 |
| w7_wssmall_s2 | S | 193 650 | 191 621 | 1.011 | 398 / 397 | 25 386 / 25 372 | 91.1 / 90.0 | 32.0 |
| w10_runresid | S | 192 451 | 190 545 | 1.010 | 396 / 395 | 25 258 / 25 256 | 91.1 / 90.1 | 32.0 |
| w7_wssmall | S | 193 486 | 191 621 | 1.010 | 398 / 397 | 25 386 / 25 372 | 91.1 / 90.0 | 32.0 |
| w3_duphigh | S | 191 768 | 190 109 | 1.009 | 395 / 394 | 25 194 / 25 181 | 91.1 / 90.2 | 32.0 |
| w3_duphigh_s2 | S | 191 755 | 190 109 | 1.009 | 395 / 394 | 25 194 / 25 181 | 91.1 / 90.2 | 32.0 |
| orkut_tail | G | 141 163 | 141 163 | **1.000** | 3 673 / 3 673 | 5 739 / 5 739 | 1.6 / 1.6 | 0.0 |
| n1_seq8m | Q | 1 191 908 | 1 222 342 | **0.975** | 2 570 / 2 443 | 164 398 / 156 164 | 92.9 / 89.5 | 32.0 |
| n5_seq8m_s4 | Q | 688 926 | 720 970 | **0.956** | 1 463 / 1 419 | 96 684 / 90 692 | 92.5 / 90.2 | 32.0 |
| bcsstk25 | M | 455 621 | 493 290 | **0.924** | 787 / 788 | 62 445 / 50 245 | 90.1 / 86.1 | 32.0 |
| qc2534 | S | 742 628 | 881 339 | **0.843** | 1 627 / 1 368 | 111 686 / 87 447 | 90.6 / 89.5 | 33.2 |
| dubcova3 | M | 3 295 852 | 3 937 942 | **0.837** | 7 597 / 5 962 | 480 270 / 382 406 | 89.3 / 80.3 | 32.7 |
| n3_clu4m | C | 399 150 | 484 783 | **0.823** | 6 988 / 7 335 | 47 620 / 40 147 | 72.3 / 53.5 | 32.0 |

Group medians: graph 2.079 (n = 4), new 0.975 (n = 5), SpMV 0.883 (n = 4), synthetic seed 1
1.051 (n = 10), synthetic seed 2 1.052 (n = 8).

## 5. Which properties predict the outcome

Correlations over all 31 traces (Pearson on the raw speedup, Spearman on ranks, and Pearson
on log₂ speedup because the outcome is multiplicative).

| property | Pearson r | Spearman | r (log₂ S) |
|---|---:|---:|---:|
| index lines / all lines | **−0.587** | −0.541 | −0.607 |
| reuse within one 16K window | **−0.510** | −0.534 | −0.563 |
| line duplicate rate | −0.381 | −0.466 | −0.416 |
| target working set / 1 MiB L2 | 0.149 | 0.450 | 0.185 |
| pages / 512-page TLB reach | 0.055 | 0.416 | 0.083 |
| DRAM rows touched | 0.051 | 0.438 | 0.080 |
| sequential-run fraction | −0.143 | −0.365 | −0.171 |
| target accesses per page | −0.261 | −0.432 | −0.285 |
| lines per page | −0.247 | −0.150 | −0.198 |
| mean accesses per row run | −0.193 | −0.340 | −0.200 |

**Two of these are traps and the brief anticipated both.**

*High duplication does not predict a win.* The coefficient is **negative** (−0.466 rank). The
largest win in the matrix, `w4_duplow` at 2.504×, has a duplicate rate of **exactly 0.000**;
the highest-duplication traces (`w3_duphigh` at 1.000) gain **1.009×**. Duplication matters to
the fold, but the fold is not what the architecture is winning with on these workloads.

*Small working sets do not predict a win either.* Every trace that fits in L2 *and* is
page-dense sits at 1.01–1.05×, but three L2-resident SpMV traces **lose** 8–16 % and one
clustered trace loses 18 % (§8) — a small footprint is not protection.

**Caution on the coefficients.** Working-set size and page count are the same variable in
this matrix (pages = WS / 4 KiB, r = 0.974), and duplication is collinear with footprint
(r = −0.488 against WS/L2). With n = 31 and that collinearity, these coefficients describe the
matrix; they do not identify a cause. The causal statements in §6–§8 rest on the designed
contrasts, where a single property is changed and the rest are held.

## 6. The controlled contrasts

### 6.1 Same footprint, same page count, same duplicate rate — only the order differs

| trace | seq | dup | WS | pages | speedup | walks b/s | DRAM reads b/s | row% b/s |
|---|---:|---:|---:|---:|---:|---|---|---|
| `w4_duplow` | 0.000 | 0.000 | 8 192 KiB | 2 048 | **2.504** | 16 895 / 99 024 | 162 269 / 156 320 | 62.5 / 0.2 |
| `n1_seq8m` | 1.000 | 0.000 | 8 192 KiB | 2 048 | **0.975** | 2 570 / 2 443 | 164 398 / 156 164 | 92.9 / 89.5 |

This is the whole result in two rows. The two traces touch the same number of distinct lines
(131 072), the same number of pages (2 048) and the same number of DRAM rows (8 192); neither
has a single duplicated line. The only difference is the order the elements arrive in, and it
is worth **2.57×** of relative position.

*Why.* On `w4` the SIMT arm walks a page for nearly every 16-line group (99 024 walks) and
its random order leaves the DRAM at a **0.2 %** row-hit rate; the engine's cohort admission
walks **5.9× fewer** pages (16 895) and its page-ordered release hits **62.5 %**. On `n1` the
SIMT coalescer already streams line after line: 2 443 walks, **89.5 %** row hits, and no
index array to read. The engine can only add its 8 192 index lines (+5.3 % DRAM reads) and
its own window overhead, and it does (−2.5 %).

### 6.2 The same effect at 4 MiB

| trace | seq | dup | WS | speedup | walks b/s | row% b/s |
|---|---:|---:|---:|---:|---|---|
| `n4_uni4m` | 0.000 | 0.567 | 3 546 KiB | **2.186** | 8 693 / 65 839 | 74.2 / 0.6 |
| `n5_seq8m_s4` | 0.500 | 0.500 | 4 096 KiB | **0.956** | 1 463 / 1 419 | 92.5 / 90.2 |

Half-sequential is already enough to erase the advantage: at `seq` 0.5 the SIMT arm reaches
90.2 % row hits and 1 419 walks, and the engine has nothing left to recover.

## 7. Where Bulk wins, and what it is winning with

Nine cells gain ≥ 1.10× — seven distinct traces, since `w4_duplow` and `w8_wslarge` appear at
both seeds. Six of the seven are at or beyond 2 MiB, or touch ≥ 512 pages; the seventh,
`lnsp3937`, is a 31 KiB trace that still gains 1.192× on row hits and L2 traffic alone.

| trace | speedup | walks S/B | reads S/B | row% B−S | index share | bank busy b/s | walker util |
|---|---:|---:|---:|---:|---:|---|---:|
| criteo_2k | 3.280 | 4.05 | 1.30 | +57.4 | 14.5 % | 56.6 / 30.8 | 79.0 % |
| orkut_hub | 2.934 | 2.26 | 0.84 | +25.3 | 9.2 % | 99.0 / 33.0 | 55.1 % |
| w4_duplow | 2.504 | 5.86 | 0.96 | +62.3 | 5.0 % | 99.3 / 53.6 | 41.9 % |
| n4_uni4m | 2.186 | 7.57 | 0.97 | +73.6 | 6.1 % | 99.3 / 67.0 | 28.0 % |
| w8_wslarge | 1.458 | 1.40 | 0.99 | +63.5 | 7.6 % | 99.6 / 99.6 | 7.0 % |
| ogbn_uniform | 1.224 | 1.22 | 0.88 | +9.7 | 5.3 % | 46.0 / 34.6 | 98.6 % |
| lnsp3937 | 1.192 | 1.00 | 1.00 | +10.2 | 28.8 % | 89.9 / 81.5 | 7.1 % |

Three mechanisms appear, in this order of importance:

1. **Page-walk reduction** (1.2–7.6× fewer walks), from cohort admission holding several runs
   open. Every win above 1.22× has it and no tie does (every tie's walk ratio is 1.00); the one
   winning trace without it, `lnsp3937` at 1.00, gains on the other two mechanisms.
2. **DRAM order** (row hits +9.7 to +73.6 points), from sealing pages ascending and releasing
   physically. On the four traces where the engine changes the order most (`w4`, `n4`,
   `ogbn_uniform`, `criteo_2k`) the SIMT arm sits at **0.1–6.3 %** row hits; on `w8` and
   `orkut_hub`, where SIMT already finds some locality, it sits at 18.6–40.2 % and the row
   gain is smaller (+63.5 and +25.3 points respectively).
3. **Fewer lines requested** (L2 reads −49 % on `w4`, −61 % on `criteo_2k`), from the fold —
   present, but it is *not* the discriminator: `w4` has zero duplication and `criteo_2k` wins
   partly because its fold removes 23 % of the DRAM reads (14 983 against 19 440).

`criteo_2k` is the family where the two axes disagree and it is worth stating plainly: a
**777 KiB** working set — *smaller* than L2 — gains **3.280×**, because it touches **1 945
pages**, 3.8× the TLB reach. Translation pressure, not capacity, is the binding constraint.
The M24-D Criteo 16k primary (2.222×) is the same workload with a larger table.

## 8. Where Bulk loses, and what it is paying

| trace | speedup | walks S/B | reads S/B | L2 reads b/s | index share | org occupancy | diagnosis |
|---|---:|---:|---:|---|---:|---:|---|
| n1_seq8m | 0.975 | 0.95 | 0.95 | 158 638 / 167 531 | 5.0 % | 8.0 % | bandwidth-bound, index traffic is pure cost |
| n5_seq8m_s4 | 0.956 | 0.97 | 0.94 | 93 093 / 100 782 | 8.5 % | 11.4 % | same, at seq 0.5 |
| bcsstk25 | 0.924 | 1.00 | 0.80 | 55 413 / 67 970 | 25.2 % | 25.7 % | L2-resident, engine reads 24 % more lines |
| qc2534 | 0.843 | 0.84 | 0.78 | 90 637 / 120 334 | 25.9 % | 26.3 % | L1/L2-resident, index wait-stall 22 % of cycles |
| dubcova3 | 0.837 | 0.78 | 0.80 | 442 611 / 516 228 | 20.7 % | 20.8 % | dense rows, all reuse already in cache |
| n3_clu4m | 0.823 | 1.05 | 0.84 | 163 223 / 83 878 | 17.2 % | 15.5 % | L2 reuse destroyed by the streamed order |

Two distinct losses, and neither is a bug:

**(a) Nothing to fix, but a cost to pay** (`n1`, `n5`). At `seq` 0.5–1.0 the SIMT arm already
achieves 89.5–90.2 % row hits with 1 419–2 443 walks. The engine can still fold and order, but
there is nothing to fold and the order is already right, so its index reads (+5–8 % of DRAM
traffic) and window residency are pure overhead. Both arms are DRAM-saturated (bank busy
99.9–100 %), so the extra reads convert directly into cycles. This is the honest negative:
**on an already-streaming gather, M24 Bulk is slower than the hardware coalescer.**

**(b) The engine's own ordering destroys L2 reuse** (`n3`, and to a lesser degree `qc2534`,
`dubcova3`, `bcsstk25`). On `n3` the hot set is 0.9 MiB — inside L2 — but the tail is not. The
SIMT arm reads L2 163 223 times and misses to DRAM on only 40 147 of them (**75 % L2 hit**).
The engine reads L2 49 % less often (83 878) but misses on 47 620 (**43 % hit**) — **19 % more
DRAM reads than SIMT** — because it drains whole pages in sealed order, so a line reused by a
later window is fetched again after L2 has evicted it. Its row-hit rate is 18.8 points *better*
and it still loses 18 %. Similar mechanism on the three SpMV losses, where the index share is
also 21–26 %.

This is a mechanism interaction worth naming: **the window's streaming order is what wins on
`w4` and what loses on `n3`.** Same mechanism, opposite sign, decided by whether the reuse
distance of the hot set is shorter than the window drain.

## 9. Operating envelope

| regime | condition (measured) | traces | outcome | what decides it |
|---|---|---|---|---|
| **W1 — translation-bound** | pages ≥ ~2× TLB reach, hot set ≪ L2 or unordered | criteo_2k | **3.28×** | 4.05× fewer walks, +57 row points |
| **W2 — order-bound** | WS ≥ 1× L2, `seq` ≤ 0.18, pages ≥ 512 | w4, n4, w8, orkut_hub | **1.46–2.93×** | walks 1.4–7.6×, row hits +25 to +74 |
| **W3 — latency-bound at scale** | WS ≫ L2 (45 MiB), DRAM at ~46 % bank busy | ogbn_uniform | **1.22×** | 1.22× fewer walks, +9.7 row points |
| **T — nothing at stake** | WS ≤ 1× L2, pages ≤ 256 | w1, w2, w3, w5, w6, w7, w9, w10, n2, lnsp3937 | **1.01–1.19×** | both arms already at L2/L1; the engine's gain is a constant few per cent |
| **N0 — no single-level work** | engine share 0 % | orkut_tail | **1.000×** | identical by construction |
| **L1 — already sequential** | `seq` ≥ 0.5 at WS ≥ 1× L2 | n1, n5 | **0.956–0.975×** | index stream is added traffic |
| **L2 — reuse destroyed** | hot set < L2 with a cold tail, `reuse16K` ≥ 0.87 | n3 | **0.823×** | streamed order re-fetches L2-resident lines |
| **L3 — dense real gathers** | WS ≪ L2, `dup` ≥ 0.99, index share ≥ 20 % | bcsstk25, qc2534, dubcova3 | **0.837–0.924×** | index traffic + L2 reuse loss |

**The single sentence:** M24 Bulk pays when the address stream is already good and wins when
it is not, and "already good" is measured by `seq` and the row-hit rate the SIMT arm achieves
on its own — not by duplication, and not by working-set size alone.

## 10. Robustness

**Seeds.** Eight of the ten synthetic families were run at two seeds; the speedup is stable to
±0.006 (`w1` 1.052/1.051, `w4` 2.504/2.510, `w8` 1.458/1.454, `w3` 1.009/1.009, `w5`
1.053/1.053, `w2` 1.053/1.052, `w6` 1.051/1.051, `w7` 1.010/1.011). `w9_seqsmall` has seed 1
only; `w10_runresid` seed 1 only.

**Address maps (reused from M24-D, both arms).** The four primaries on map M0 against map M3
at three placement seeds:

| workload | map M0 | map M3 s1 | s2 | s3 | spread |
|---|---:|---:|---:|---:|---:|
| Orkut BFS S1 | 2.921 | 2.967 | 2.954 | 2.971 | 0.6 % |
| LiveJournal | 2.516 | 2.529 | 2.530 | 2.530 | 0.0 % |
| ogbn-products | 1.266 | 1.266 | 1.266 | 1.266 | 0.0 % |
| Criteo 16k | 2.222 | 2.206 | 2.225 | — | 0.9 % |

The Criteo map-M3 seed-3 cell is the non-terminating defect M24-D documented (it serves every
element by cycle 3 292 976 and then runs to the 500 M guard); it is excluded here and not
counted as a speedup. The advantage is map-invariant to within 0.9 % on the three workloads
that complete.

## 11. Resource utilisation in the winning cells

| trace | pool occupancy | pool full | backpressure | engine share | walker busy | IPC b/s |
|---|---:|---:|---:|---:|---:|---|
| criteo_2k | 1 451.2 | 12.9 % | 12.9 % | 43.6 % | 79.0 % | 0.329 / 0.100 |
| orkut_hub | 2 743.7 | 14.7 % | 14.7 % | 100.0 % | 55.1 % | 0.161 / 0.055 |
| w4_duplow | 3 253.4 | 45.9 % | 45.9 % | 32.0 % | 41.9 % | 0.271 / 0.108 |
| n4_uni4m | 3 333.9 | 51.2 % | 51.2 % | 32.0 % | 28.0 % | 0.352 / 0.161 |
| w8_wslarge | 3 461.9 | 63.3 % | 63.3 % | 32.0 % | 7.0 % | 0.467 / 0.320 |
| ogbn_uniform | 253.4 | 5.9 % | 5.9 % | 95.0 % | 98.6 % | 0.033 / 0.027 |
| lnsp3937 | 38.3 | 0.0 % | 0.0 % | 30.2 % | 7.1 % | 1.805 / 1.514 |

The 4096-entry pool is under pressure (46–63 % of cycles full) exactly in the cells where the
engine wins biggest, which is the M24-E result reproducing on new workloads: the pool is the
mechanism that lets cohort admission run ahead of the DRAM.

## 12. Limitations

1. **The traces are read-only.** Every workload here is a single-level gather; DRAM writes are
   zero by construction, so this milestone says nothing about write-back behaviour, dirty-line
   eviction or read/write turnaround. The brief asks for "DRAM reads and writes"; writes are
   reported as structurally absent, not as measured zero.
2. **`elem_bytes` differs across families** (8 B for SpMV and synthetic doubles, 4 B for graph
   properties). This is a workload property applied identically to both arms, so the pairs are
   fair, but the families are not width-matched to each other and cross-family comparisons of
   absolute cycles carry that caveat.
3. **The four M18 BFS traces were excluded.** They carry no single-level work (their gathers
   depend on the previous frontier), so the engine's share is 0 and the pair is degenerate by
   construction — `orkut_tail` is included precisely to show what that looks like (1.000×).
   `ogbn_train_100d` and `ogbn_uniform_100d` were excluded on cost grounds: at ~7× the
   accesses of their base traces they would dominate the matrix without testing a new
   property (feature width is M23-G5A's axis, not this one).
4. **One seed per new trace** and one simulator seed. The seed spread on the eight
   two-seed families is ≤ 0.006, which bounds but does not eliminate the concern.
5. **Correlations are descriptive, n = 31, and collinear** (§5). The causal claims rest on the
   two designed contrasts in §6 and on the counter evidence in §7–§8.
6. **Map invariance is inherited, not re-derived for the new traces.** Only the four M24-D
   primaries were re-checked on map M3 (§10); whether `n3`'s L2-reuse loss or `n1`'s
   bandwidth loss is equally map-invariant is not measured.
7. **The Criteo map-M3 s3 cell cannot be used** (M24-D's known non-termination). It is
   excluded from §10 rather than reported as a 0.015× speedup.

## 13. Conclusion

M24 Bulk's operating envelope, measured rather than assumed:

* **Wins (1.22–3.28×)** when the gather stream is *unordered and page-spread*: at or beyond the
  L2 boundary with `seq` ≤ 0.18, or with a page count several times the TLB reach even when the
  data fits in L2. Cohort admission cuts walks 1.2–7.6× and physical order lifts row hits from
  0.1–18.6 % to 9.8–82.1 %.
* **Ties (1.01–1.19×)** when the target set is L2-resident and page-dense: both arms hit L2 and
  the engine's advantage is a constant few per cent.
* **Loses (0.82–0.98×)** in two regimes that must be designed around, not tuned away: an
  already-sequential stream at scale, where the index read is pure cost, and an L2-resident
  hot set with a cold tail, where the sealed order re-fetches lines SIMT was hitting.
* **Duplication is not the predictor.** The 2.504× cell has a 0.000 duplicate rate; the
  1.000-duplicate cell gains 1.009×. What the engine exploits is *disorder*, and duplication is
  only one of the things that produces it.

The architecture is therefore not universally better, and the M24-D headline (1.27–2.92× on
four production workloads) is an *upper* part of its range: those four workloads happen to sit
in the win regime. Across the 31-trace matrix the median is 1.05×, nine traces win, nine tie
and thirteen gain nothing or lose.

## Reproduce

```
cd DX100_MEM
python3 tools/m24f_manifest.py                       # traces.tsv: 31 traces, workload fields
python3 tools/m24f_stats.py                          # structure.tsv: offline properties
(cd sim && ./bin/dx100_tests)                        # 4 924 checks, 0 failures
PAR=5 ./build/wm/m24f_run.sh all                     # 62 cells; skips finished ones
python3 tools/m24f_extract.py                        # pairs.csv, summary.csv, report.txt
python3 tools/m24f_verify_doc.py                     # every figure in this file, recomputed
```

Artifacts: `build/wm/m24f/` — `traces.tsv`, `structure.tsv`, `commands.txt`, 62 cell logs +
3 repeat logs, `pairs.csv`, `summary.csv`, `report.txt`, and the five new traces
(`n1_seq8m`, `n2_zipf4m`, `n3_clu4m`, `n4_uni4m`, `n5_seq8m_s4`) with their builder JSON.
Scripts: `build/wm/m24f_run.sh`, `tools/m24f_manifest.py`, `tools/m24f_stats.py`,
`tools/m24f_extract.py`, `tools/m24f_verify_doc.py`. Historical traces and results are
untouched: every M18/M21/M23 trace file and every M24-D log is read-only input here.
