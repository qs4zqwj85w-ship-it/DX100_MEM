# M13 — Cross-window reuse tracking and deduplication

**Question.** Can a bounded mechanism capture target-cache-line reuse *across*
consecutive 8K windows and approach the performance of the 2 × 32K
configuration, without 32K buffers?

**Answer.** Yes, but only at more storage than the window it replaces. The
mechanism that can do it — retaining completed 64 B target lines — needs
`K ≥ Λ` entries, where `Λ` is the target footprint, because the reuse distance
of W1 is exactly one pass over the whole footprint (measured: median 16 372
line-lookups at Λ = 16 384 lines). At `K = Λ` it beats 2 × 32K by 16.2 % *and*
cuts DRAM traffic below it, but it costs 1.59 MB of SRAM against the window's
541 KB. Below `K = Λ` the capture collapses: a 508 KB directory (slightly
*less* SRAM than the 2 × 32K window pair) buys 5.8 % where the windows buy
29.2 %. The 32K window is the cheaper way to see this reuse because it holds
*references* (8 B each) rather than *lines* (64 B each).

M13 is therefore **not retained as a default**. The official M10 configuration
remains two 8K windows and no reuse buffer; the mechanism stays in the tree,
off by default, as the measurement that settles the question.

---

## 0. What was done

* Read the M12 window implementation and answered the seven feasibility
  questions with code references (§1).
* Added an exact, offline trace analysis: the engine dumps each sealed window's
  distinct target lines, and a script replays that stream through a fully
  associative LRU to obtain the exact capture curve of any directory size (§2).
* Implemented a bounded set-associative **completed-line reuse buffer** with an
  in-flight merge option, in the existing fanout path, behind `reuse_dir`
  (default 0), plus one display-only dump (§3, §4).
* Ran 158 cells: W1 × 3 sizes × 5 seeds × 7 configurations, four control
  workloads, five ablations. All 158 pass the M9 gates, the value hash, the
  address re-derivation check and the new M13 accounting identities (§6–§8).
* Added 10 correctness tests (306 new checks); the suite is **4338 checks, 0
  failures**, and the four published anchors reproduce exactly (§8).
* Measured the storage/timing cost and the crossover (§5, §7), and recommend
  against adopting the mechanism (§10).

---

## 1. Findings in the existing M12 implementation

All of this is `sim/src/bulk.cpp` as published at the end of M12; nothing below
was changed to make M13 easier.

### 1.1 How target-line identity is represented

A target address is generated from an index value as
`addr = data_base + index[i] * data_bytes`, and the identity used for grouping
is `addr >> 6` — the **VA line number** (`WinEnt::va_line`, `BulkEngine::win_admit_one`).
Grouping on the VA line is exact rather than approximate for a structural
reason the M6 report already states: a 64 B line never straddles a 4 KiB page,
and the model's VA→PA map (`ppn = vpn + PPN_OFFSET`) is injective, so two
elements with the same VA line are necessarily on the same physical line. M13
reuses exactly this identity, so the two mechanisms can never disagree about
what "the same line" means.

### 1.2 How duplicates are recognised within a window

At seal, `win_seal` builds `b.sorted` (entry ids ordered by `(va_line, id)`) and
folds equal lines into one `LineReq` with `count` dependants
(`st_.dedup_merged += (j - i) - 1`). There is **no** hash table and no
associativity: the sort *is* the deduplication, and its result is a per-window
line list built once.

### 1.3 When the metadata is discarded

`win_tick` releases a buffer (`b = Batch{}`) as soon as
`issued_n >= order.size() && pending == 0 && retired >= n_ent`. At that moment
the window's line list, its 64 B copies and its dependant list are all gone.
Between windows there is no state at all that names a target line.

### 1.4 Whether consecutive windows actually contain repeated lines

Measured, exactly, from the dumps (§2): at W1, N = 65 536, `batch_window =
8 192`, arm C's windows contain **8 192 distinct lines each and 0 % internal
duplicates** — a window is a sample of a permutation, so it has no repeats
inside itself. `st_.dedup_merged = 14` over 49 304 elements confirms it from
the other direction: essentially every fold in this workload is across a window
boundary, not inside one. The repeats are between a line's reference in pass
`p` and its reference in pass `p+1`, i.e. **one full pass apart**.

### 1.5 Whether the previous window's data is still available

Yes, but only in the wrong place. `on_line` copies the fill into
`FanJob::bytes` (a 64 B array in a deque), and `win_fanout` distributes it to
the dependent elements. That copy is destroyed when the job is popped, and the
`FanJob` is not addressable by line. The only other copy is in the cache
hierarchy — see 1.6.

### 1.6 Whether the shared L1 already captures the benefit

Under the published path (`mem_path = l1`) the engine's target requests **do**
probe the shared L1, and the answer is essentially no:

| W1 cell | target requests | L1 requests (all clients) | DRAM reads | L1 hit rate |
|---|---|---|---|---|
| N=16 384, 8K | 12 376 | 14 153 | 13 589 | 0.0 % |
| N=65 536, 8K | 49 290 | 56 431 | 50 372 | 0.0 % |
| N=65 536, 32K | 32 752 | 39 893 | 35 873 | 0.0 % |

The L1 is 256 lines against a 16 384-line footprint, so a repeat one pass old
never survives in it. The L2 (16 384 lines, 8-way) is exactly footprint-sized
and is shared with the index stream, the descriptor and the SIMT arm; it hits
12.8 %, so **almost every target request reaches DRAM** even though the data was
touched a pass earlier. This is the single most important finding for M13: the
reuse is real, it is large (66.8 % of lookups repeat a line), and the existing
hierarchy does not serve it.

The one control where the hierarchy *does* serve it is STRIDmix (Λ = 1 024
lines): its L1/L2 hit rates are high enough that eliminating 82 % of the target
requests changes DRAM reads by **zero** (4 104 → 4 104) while costing 7 % in
cycles (§7.4). That is the "already captured" case, and it is a loss.

### 1.7 Whether the two buffers overlap enough to merge in flight

Structurally yes: `fill_` and `iss_` are independent cursors, so a sealed
window can still be draining while the next fills (`win_live_cycles`,
`win_drain_max`). Measured at W1 N=65 536: 342 305 cycles with two live windows
in arm C. In practice the seam is narrow: of 49 290 lookups, **3 762 (7.6 %)**
found their line still outstanding and were merged, versus 18 170 (36.9 %)
served from retained data. Merging is therefore worth having but is not the
main effect (ablation: switching it off costs +4.8 %, §7.5).

---

## 2. Quantitative cross-window reuse analysis

### 2.1 Method

`--set m13_dump=<path>` appends one record per seal —
`W <index> <n_ent> <va_line> …` with the window's distinct lines — after every
decision for that window has been made. `build/wm/m13_analyze.py` replays those
records: it counts repeats, measures each repeat's distance in line-lookups and
in windows, and runs a fully associative LRU over the seal stream with one batch
per window. That LRU is the **exact upper bound** on what a data-retaining
directory of `K` entries can serve, because it assumes unlimited probing
bandwidth, perfect hashing and no timing constraint. The file contains both
arms; the tables below use arm C, the dispatched configuration the report is
about.

### 2.2 The W1 opportunity, measured

W1, N = 65 536, `batch_window` = 8 192, arm C:

| quantity | value |
|---|---|
| elements in the descriptor | 49 304 |
| line lookups (distinct lines, summed over windows) | 49 290 |
| distinct target lines | 16 384 |
| **lookups that repeat an earlier line** | **32 906 (66.8 %)** |
| — repeat of a line seen in the immediately preceding window | 8 248 |
| — two windows back | 16 484 |
| — three or more windows back | 8 174 |
| mean gap, in windows | 2.00 |
| reuse distance, median / p90 (line-lookups) | 16 372 / 24 585 |
| same-window repeats | 0 (a seal emits distinct lines) |

The footprint is `Λ = P × λ = 512 × 32 = 16 384` lines and one pass of the
permuted draw is 16 384 bulk elements, so the reuse distance of a line is
exactly one pass — one pass is two 8K windows, and the median gap is 16 372
lookups. There is no short-distance reuse to exploit: the distribution is a
single narrow mode at `Λ`, not a decaying histogram.

### 2.3 Capture curve (exact bound) versus what was built

Percentage of the 49 290 lookups a directory of `K` entries can serve:

| K | 256 | 512 | 1 024 | 2 048 | 4 096 | 8 192 | **16 384** | 32 768 |
|---|---|---|---|---|---|---|---|---|
| W1 N=65 536 | 0.53 | 1.04 | 2.09 | 4.20 | 8.26 | 16.75 | **66.76** | 66.76 |
| W1 N=32 768 | 0.50 | 0.99 | 2.05 | 4.17 | 8.34 | 16.73 | **33.41** | 33.41 |
| W1 N=16 384 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | **0.00** | 0.00 |
| STRIDmix N=65 536 | 20.83 | 41.67 | **83.33** | 83.33 | 83.33 | 83.33 | 83.33 | 83.33 |
| W0 N=65 536 | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 |
| PSCR N=65 536 | 0.57 | 1.13 | 2.29 | 4.61 | 9.44 | 18.83 | **75.00** | 75.00 |

Two facts decide the milestone:

1. **The curve is a cliff at `K = Λ`, not a curve.** Below `Λ` capture is
   roughly `K/Λ`; at `Λ` it jumps to everything there is. A directory sized to
   "substantially less storage" is on the wrong side of the cliff by
   construction.
2. **A 32K window is not the same mechanism.** When `W = 32 768` the window
   itself sees only 49.98 % of the repeats (2 windows × 24 652 elements over a
   3-pass trace), which is exactly the `dedup merges` M12 measured and the
   29.2 % cycle win. A directory of 16 384 entries sees **all** 66.76 %, which
   is why it can outperform the window — at 3× the storage.

N = 16 384 has **no** cross-window reuse at all (0.00 % at every `K`): the whole
trace is one pass. The −15 % that M12 measured for a 16K/32K window at that
size is *page grouping* (row activations 2 206 → 1 192 at equal request count),
not deduplication, and no reuse mechanism can reproduce it. M13 correctly
measures 0.0 % hits there.

---

## 3. Chosen architecture and rationale

### 3.1 The three candidates, and why two were rejected

**(a) Metadata only — a tag directory with a "completed" bit.** Rejected on
correctness, and the brief is right to insist on it: a later access can only
avoid a memory request if the data is actually available. An entry holding
`va_line` and a bit cannot produce `output[i] = data[index[i]]`; the value would
have to be fetched, which is the request the mechanism is supposed to remove.
Such a directory can only *merge into an outstanding request*, which is
capability (b), not a substitute for it.

**(b) In-flight merging only.** Cheap (≤ R entries, no data) and correct, but it
addresses only the seam: measured 7.6 % of lookups, worth +4.8 % cycles at
K=16 384 (§7.5). It cannot touch the 59 % of repeats whose earlier request
completed long ago, and it is subsumed by (c) at negligible cost. Implemented
as an option, not as the mechanism.

**(c) Retain completed target lines.** The only mechanism that avoids a request
outright, and the one implemented. A hit at seal retires that line's elements
from bytes the engine already holds: no request, no residency slot, no
translation, no L1/L2/DRAM traffic.

### 3.2 Structure

A set-associative store of **completed 64 B lines**, tag = VA line, index =
mixing hash of the tag, LRU within a set:

* **Probe at seal**, once per distinct line of the window — after `b.lines`,
  `b.order` and the cohorts are final, and after `issued_n` has been reset.
  Each lookup ends in exactly one of three ways:
  1. **data held** → push the window's fanout job from the retained bytes, mark
     the line issued, count `reuse_hit_ready`;
  2. **still outstanding** → attach this window's `(batch, line)` to the entry
     so the eventual fill fans out to both, mark issued, count
     `reuse_hit_inflight`. One request, one slot, two windows;
  3. **miss** → issue exactly as M10 would, and allocate an entry so the next
     window can reuse it.
* **Publish at fill**: `on_line` copies the 64 B into the entry (state
  *completed*) and fans out to every attached window line.
* **Evict** on allocation pressure: invalid first, else the least recently used
  *completed* entry. An **outstanding entry is never evicted** — it is a promise
  to a window that has not retired. If every way in a set is outstanding the
  line simply issues without a record (`reuse_full`), so directory pressure can
  never block or lose a request.
* **Reset on abort**: `abort_desc` drops outstanding records (their fills will
  never be attributed) and counts `reuse_dropped`. Completed entries survive a
  descriptor boundary — the tags are absolute VA lines and the data is the
  image's — which the `m13_descriptor_boundary` test checks.

### 3.3 Why this is the smallest correct mechanism here

Everything a hit needs is in the entry, so a hit is *literally the fill path*:
copy 64 B, run `win_fanout`. There is no second extractor to get the word offset
wrong, and the existing independent address re-derivation in `win_fanout`
(`addr_mismatch`) therefore covers directory hits for free. No prefetch, no
prediction, no knowledge of unissued elements: the directory only answers
questions the trace has already asked.

---

## 4. Implementation details

### 4.1 Configuration (all defaults preserve M10)

| key | default | meaning |
|---|---|---|
| `reuse_dir` | **0** | enable the buffer; with 0 none of the M13 code runs |
| `reuse_sets` | 256 | sets; entries = `reuse_sets × reuse_ways` |
| `reuse_ways` | 4 | ways per set, LRU inside a set |
| `reuse_hash` | 1 | mixing hash for the set index |
| `reuse_merge` | 1 | merge into requests still outstanding |
| `reuse_charge` | 1 | charge one organizer visit per lookup and per insertion |
| `m13_dump` | empty | diagnostic dump path; empty writes nothing |

### 4.2 Set indexing needs a real hash

The first implementation used `va_line % sets`, the same index the model's L2
uses. On the footprints this simulator generates it aliases: W1's lines are
`p*64 + 2j`, so with 4 096 sets only 2 048 residues are reachable and a
16 384-entry array fills to 8 192. A weak XOR fold (`line ^ line>>11 ^
line>>22`) does not help, because the stride structure lives entirely below bit
11. The shipped form is a two-multiply mixing hash with the high half folded
down — the same argument as `VX_CFG_L2_BANK_HASH = 1`, which this model already
sets. Measured effect of *not* hashing, at `K = 16 384`: peak valid entries
8 192 instead of 13 226, hit rate 10.8 % instead of 36.9 %, cycles 355 189
instead of 252 210 (+40.8 %). Correctness never depends on the choice: the tag
comparison is exact, the index only decides where a line may live.
`reuse_hash = 0` remains available as that ablation.

### 4.3 Accounting identities (checked on every run)

```
reuse_probes                        == reuse_lines_planned
reuse_hit_ready + reuse_hit_inflight + reuse_miss == reuse_lines_planned
lines_issued                        == reuse_miss
lines_issued + dedup_merged + reuse_hit_ready + reuse_hit_inflight == win_entries
tgt_req_gen == tgt_req_issued == tgt_req_done
unissued + issued_not_filled <= R      (unchanged; no new residency slot is created)
```

`dedup_merged` keeps its M6 definition (elements folded onto an
already-requested line) and is now computed over *all* groups, served or
issued, which is why the fourth identity closes. A directory hit forms **no**
request, so it can never add outstanding-request capacity; the residency peak
is unchanged or lower (`§7.3`).

### 4.4 Reporting counters

`bulk_reuse_{lines_planned,probes,hit_ready,hit_inflight,miss,full,sub_full,
evict,inserts,dropped,elems_ready,elems_merged,req_avoided,sub_max,occ_max,
occ_sum,bytes,vec_bytes}`, printed in section `[18]`. The distinctions the brief
requires are separate counters, not one "eliminated" number: avoided before
submission (`hit_ready`), merged while outstanding (`hit_inflight`), served from
retained data (`elems_ready`), served by the hierarchy (L1/L2 hits in `[7]`,
`[15]`), and still reaching DRAM (`dram_reads`).

---

## 5. Storage and timing costs

### 5.1 Per-entry storage

| bytes | field |
|---|---|
| 64 | the line's data |
| 4 | tag (VA line; 26 bits suffice for the 4 GiB space) |
| 4 | LRU stamp |
| 1 | state (invalid / outstanding / complete) |
| 1 | subscriber count |
| 12 | 3 subscribers × (batch, line) |
| **88** | modelled total per entry (86 used, 2 pad) |

`bulk_reuse_bytes = sets × ways × 88` — the charge for a packed layout. The
simulator's own object is 100 B (`ReuseEnt` pads to 100 with that field order),
so the realized `bulk_reuse_vec_bytes` is exactly `K × 100`; both are reported,
the same methodology M12 used for `bulk_win_bytes` vs `bulk_win_vec_bytes`.

### 5.2 Total SRAM, like for like

M12's modelled window charge is `2 × (W × 8 + page_dir × 16 + cohorts)`, i.e.
147 584 B at W=8 192 and 540 800 B at W=32 768. Adding the M13 directory:

| configuration | window SRAM | directory | total | ×M10 |
|---|---|---|---|---|
| M10: 2 × 8K | 147 584 | — | **147 584** | 1.00 |
| M12: 2 × 16K | 278 656 | — | **278 656** | 1.89 |
| M12: 2 × 32K | 540 800 | — | **540 800** | 3.66 |
| M13: 2 × 8K + K=1 024 | 147 584 | 90 112 | **237 696** | 1.61 |
| M13: 2 × 8K + K=4 096 | 147 584 | 360 448 | **508 032** | 3.44 |
| M13: 2 × 8K + K=8 192 | 147 584 | 720 896 | **868 480** | 5.88 |
| M13: 2 × 8K + K=16 384 | 147 584 | 1 441 792 | **1 589 376** | 10.77 |

`K = 4 096` is the interesting point: it costs 6 % **less** SRAM than the
2 × 32K window pair and delivers 5.8 % where the windows deliver 29.2 %.

### 5.3 Timing cost

Each lookup is charged one organizer visit, and each insertion one more; the
organizer drains `org_lanes = 4` visits per cycle and admission yields while it
is busy, so the charge is a real, back-pressure-forming cost rather than a
formality. At `W = 8 192` that is 8 192 extra visits per sealed window
(≈ 2 048 cycles if it were serial), doubling the seal pass.

Measured: at W1 N=65 536, K=16 384, removing the charge changes the run from
252 210 to 252 457 cycles — **+0.1 %**. The mechanism's control cost is hidden
by the drain at this geometry, because the organizer has slack while the
previous window's fills are outstanding. On a workload with no drain to hide in
(STRIDmix) the same charge is *not* free (§7.4).

### 5.4 What a hit is worth, and what it is not

A hit removes a request, its residency slot, its translation and its L1/L2/DRAM
traffic. At K=16 384, W1 N=65 536: 21 932 requests avoided (44.5 % of lookups),
DRAM reads 50 372 → 27 649 (−45 %), row activations 6 709 → 5 559. It does not
remove the *fanout* work — every element is still reconstructed — so the saving
is bounded by the memory path, which is why the faster the memory path the less
a hit is worth (§7.4).

---

## 6. Experimental methodology

* **Base configuration**: `configs/ab_controlled.cfg`, the published controlled
  environment (`R = 128`, L1 256×4/2cy/16 MSHR, L2 16 384×8/4cy/64 MSHR, 2 banks,
  14/26/0 DRAM, FR-FCFS, bank hash, E3 window policy, `idx_mode = 2`,
  `page_sched = 1`, `pool_slots = 128`).
* **Driver**: `experiment=m9diag`, i.e. the published three-arm M9 diagnostic
  (A = SIMT only, B = bulk only, C = dispatched). **C is the configuration under
  test**; A supplies the SIMT reference for speedup; B is a diagnostic.
* **Identical logical traces**: all configurations in a row share `N`, `Λ`, `P`,
  `κ`, `ρ_dup` and `seed`, and a fresh `MemorySystem` per arm. Only
  `batch_window`, `reuse_*` differ.
* **Seeds**: 5 paired seeds for every W1 cell (105 primary runs). Controls and
  ablations use seed 1.
* `dram_diag = 1` everywhere, which M11.2/M11.3 established is inert.
* **Harness**: `build/wm/m13_matrix.sh` (158 cells, `build/wm/m13_matrix.log`),
  extractors `build/wm/m13_extract.py` (`perf|reuse|mem|check|abl`) and
  `build/wm/m13_analyze.py` (reuse distances). Representative command:

```
./bin/dx100sim --config configs/ab_controlled.cfg --set rho_dup=0 \
  --set elem_bytes=4 --set bulk_wl=E0 --set bulk_max_elems=262144 \
  --set max_cycles=200000000 --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
  --set page_sched=1 --set idx_mode=2 --set experiment=m9diag --set pool_slots=128 \
  --set dram_diag=1 --set Lambda=16384 --set P=512 --set bulk_stream=0 \
  --set N=65536 --set kappa=0.25 --set seed=1 --set batch_window=8192 \
  --set reuse_dir=1 --set reuse_sets=4096 --set reuse_ways=4
```

Configurations in the primary table: M10 `w8192`; M12 `w16384`, `w32768`; M13
`k1024`, `k4096`, `k8192`, `k16384` (= `reuse_sets` 256/1 024/2 048/4 096 × 4
ways, hashed, merge on, charge on).

---

## 7. Results

### 7.1 W1 — the primary comparison (C arm, mean of 5 paired seeds)

Cycles; `Δ` columns are the mean of the per-seed paired differences.

| N | configuration | SRAM (B) | cycles | ± sd | speedup vs SIMT | Δ vs M10 | Δ vs 2×32K |
|---|---|---|---|---|---|---|---|
| 16 384 | M10 2×8K | 147 584 | 124 405.6 | 1 134.2 | 1.769 | — | +17.68 % |
| 16 384 | M12 2×16K | 278 656 | 105 716.4 | 916.9 | 2.081 | −15.02 % | +0.00 % |
| 16 384 | M12 2×32K | 540 800 | 105 716.4 | 916.9 | 2.081 | −15.02 % | 0.00 % |
| 16 384 | M13 K=1 024 | 237 696 | 124 196.0 | 950.6 | 1.772 | −0.17 % | +17.48 % |
| 16 384 | M13 K=4 096 | 508 032 | 124 196.0 | 950.6 | 1.772 | −0.17 % | +17.48 % |
| 16 384 | M13 K=8 192 | 868 480 | 124 196.0 | 950.6 | 1.772 | −0.17 % | +17.48 % |
| 16 384 | M13 K=16 384 | 1 589 376 | 124 196.0 | 950.6 | 1.772 | −0.17 % | +17.48 % |
| 32 768 | M10 2×8K | 147 584 | 214 114.2 | 1 686.3 | 1.638 | — | +44.63 % |
| 32 768 | M12 2×16K | 278 656 | 206 826.0 | 1 824.9 | 1.695 | −3.40 % | +39.70 % |
| 32 768 | M12 2×32K | 540 800 | 148 046.8 | 45.8 | 2.368 | −30.85 % | 0.00 % |
| 32 768 | M13 K=1 024 | 237 696 | 209 786.2 | 1 747.0 | 1.671 | −2.02 % | +41.70 % |
| 32 768 | M13 K=4 096 | 508 032 | 199 083.6 | 1 382.0 | 1.761 | −7.02 % | +34.47 % |
| 32 768 | M13 K=8 192 | 868 480 | 184 356.4 | 789.1 | 1.902 | −13.90 % | +24.53 % |
| 32 768 | M13 K=16 384 | 1 589 376 | 170 150.0 | 830.3 | 2.061 | −20.53 % | +14.93 % |
| 65 536 | M10 2×8K | 147 584 | 421 827.4 | 1 911.8 | 1.521 | — | +41.30 % |
| 65 536 | M12 2×16K | 278 656 | 439 337.2 | 4 390.7 | 1.461 | +4.15 % | +47.15 % |
| 65 536 | M12 2×32K | 540 800 | 298 579.8 | 4 595.7 | 2.149 | −29.22 % | 0.00 % |
| 65 536 | M13 K=1 024 | 237 696 | 414 869.6 | 1 635.3 | 1.547 | −1.65 % | +38.97 % |
| 65 536 | M13 K=4 096 | 508 032 | 397 514.2 | 1 469.1 | 1.614 | −5.76 % | +33.16 % |
| 65 536 | M13 K=8 192 | 868 480 | 351 748.2 | 1 795.5 | 1.824 | −16.61 % | +17.82 % |
| 65 536 | M13 K=16 384 | 1 589 376 | 250 318.2 | 1 132.0 | 2.563 | **−40.66 %** | **−16.15 %** |

Sign counts on the paired seeds: **every** M13 configuration is faster than M10
in 5/5 seeds at every size, but at N = 16 384 the difference is 17–895 cycles
(−0.17 %) with **0 of 12 376 lookups hitting** — a phase shift from the
organizer charge, identical for all four K, not a benefit. Against 2 × 32K,
only `K = 16 384` at N = 65 536 is faster (5/5, by 41 680–51 785 cycles); every
other M13 cell is slower in 5/5 seeds.

### 7.2 What eliminated what (W1, N = 65 536, K = 16 384)

| quantity | M10 2×8K | M12 2×32K | M13 K=16 384 |
|---|---|---|---|
| elements in the descriptor (C) | 49 304 | 49 304 | 49 304 |
| line lookups (distinct lines per window) | 49 290 | 32 752 | 49 290 |
| folded by the window's own dedup | 14 | 16 552 | 14 |
| served from retained data | — | — | 18 170 (36.9 %) |
| merged into an outstanding request | — | — | 3 762 (7.6 %) |
| requests still issued | 49 290 | 32 752 | 27 358 (55.5 %) |
| **requests avoided** | — | 16 552 (by deferral) | **21 932 (by retention)** |
| L1 requests (all clients) | 56 431 | 39 893 | 34 499 |
| DRAM reads | 50 372 | 35 873 | **27 649** |
| row activations | 6 709 | 2 626 | 5 559 |
| DRAM row-hit rate | 86.7 % | 92.7 % | 79.9 % |
| peak bulk residency | 128 | 120 | 128 |
| peak total outstanding | 128 | 128 | 128 |
| index lines requested | 3 082 | 3 082 | 3 082 |

M13 removes *more* requests than the 32K window and less DRAM traffic than it,
but it loses the row locality the window gains (a 32K window issues a page's
lines together; an 8K window sees a third of the pages per window and re-opens
rows, 5 559 vs 2 626 activations). That is the second half of why the 32K window
is efficient: it is a *locality* mechanism as much as a dedup mechanism.

### 7.3 Control workloads (seed 1, C arm, cycles)

| workload | N | M10 2×8K | M12 2×32K | M13 K=4 096 | M13 K=16 384 |
|---|---|---|---|---|---|
| W0 (streaming) | 16 384 | 18 142 | 18 093 | 18 150 (+0.04 %) | 18 150 (+0.04 %) |
| W0 | 32 768 | 34 148 | 36 056 | 34 227 (+0.23 %) | 34 227 (+0.23 %) |
| W0 | 65 536 | 69 428 | 72 941 | 69 896 (+0.67 %) | 69 896 (+0.67 %) |
| PSCR (κ=0) | 16 384 | 623 096 | 322 401 | 623 087 (−0.00 %) | 623 087 (−0.00 %) |
| PSCR | 32 768 | 1 242 945 | 329 956 | 1 237 466 (−0.44 %) | 1 073 582 (−13.63 %) |
| PSCR | 65 536 | 2 485 036 | 647 042 | 2 471 943 (−0.53 %) | 2 004 630 (**−19.34 %**) |
| STRID (κ=1) | 65 536 | 34 952 | 34 952 | 34 952 | 34 952 |
| STRIDmix (κ=0.25) | 16 384 | 21 302 | 21 290 | 21 296 (−0.03 %) | 21 290 (−0.06 %) |
| STRIDmix | 32 768 | 27 356 | 26 244 | 29 640 (**+8.35 %**) | 29 649 (+8.37 %) |
| STRIDmix | 65 536 | 42 967 | 43 000 | 46 020 (**+7.10 %**) | 46 029 (+7.13 %) |

* **W0**: no cross-window reuse exists (0.06 % of lookups repeat), so the
  mechanism is pure overhead — 0.04–0.67 %, i.e. within the noise of the
  organizer charge. It never helps and never hurts materially.
* **PSCR**: at κ=0 every operation is bulk and the trace is page-dispersed. The
  reuse is the same size as W1's (75 %), but translation walks dominate, so
  eliminating requests also eliminates their walks: −19.3 % at N=65 536. Still
  far short of the window's −74 %, because the window removes the *pages* per
  window and the directory cannot.
* **STRID** (κ=1): the engine gets no work at all, so every row is identical by
  construction — a useful control that the mechanism is inert when unused.
* **STRIDmix**: 82.3 % of lookups hit (83.3 % is all there is), and the run is
  **7 % slower**, because those requests were L1/L2 hits: DRAM reads are
  unchanged (4 104 → 4 104) while the lookups cost organizer visits. This is the
  brief's "existing L1 caching already captures it" case, measured.

### 7.4 Why STRIDmix regresses

| N=65 536 | target requests | L1 requests | DRAM reads | DRAM row hits | cycles |
|---|---|---|---|---|---|
| M10 2×8K | 6 144 | 13 320 | 4 104 | 91.1 % | 42 967 |
| M13 K=4 096 | 1 089 | 8 265 | 4 104 | 91.0 % | 46 020 |
| M13 K=16 384 | 1 024 | 8 200 | 4 104 | 91.0 % | 46 029 |

5 055 requests removed (82 % of lookups), zero DRAM reads removed. The requests
were being served by the hierarchy already, in parallel with the engine's other
stalls; removing them frees nothing on the critical path, while the 6 144
lookups and the re-timed fanout cost 3 053 cycles. The lesson generalizes: a
reuse directory is worth its cost only where the request it removes would have
reached DRAM.

### 7.5 Ablations (W1, N = 65 536, seed 1)

| cell | cycles | hit (ready) | merged | requests avoided | peak valid | DRAM reads |
|---|---|---|---|---|---|---|
| M13 K=16 384 | 252 210 | 18 170 | 3 762 | 21 932 | 13 226 | 27 649 |
| — merge off | 264 354 (+4.8 %) | 18 209 | 0 | 18 209 | 13 226 | 28 856 |
| — charge off | 252 457 (+0.1 %) | 18 167 | 3 762 | 21 929 | 13 226 | 27 637 |
| — unhashed index | 355 189 (+40.8 %) | 5 305 | 2 321 | 7 626 | 8 192 | 41 052 |
| — 8-way (2 048 sets) | 244 776 (−3.0 %) | 19 504 | 4 036 | 23 540 | 14 082 | 26 836 |
| M13 K=4 096 | 399 759 | 1 894 | 313 | 2 207 | 4 096 | 47 152 |
| — merge off | 402 115 (+0.6 %) | 1 893 | 0 | 1 893 | 4 096 | 47 471 |

* In-flight merging is worth **4.8 %** at K=16 384 and 0.6 % at K=4 096: real,
  but an order of magnitude smaller than data retention.
* The organizer charge is worth **0.1 %**: the drain hides it on W1.
* Hashing is worth **40.8 %**: without it the array is half unusable.
* 8 ways instead of 4 buys **3.0 %** (peak 14 082 vs 13 226 entries). A fully
  associative directory would presumably approach the 66.8 % bound instead of
  the measured 44.5 %, i.e. a little more headroom — but not a change of sign.

---

## 8. Correctness and regression results

### 8.1 Tests

`./bin/dx100_tests` → **4338 checks, 0 failures** (4032 before M13; 306 new).
The ten new tests are all constructed so the expectation comes from the bytes
the test planted (`word b of data line k = 1000k + b`), never from the engine:

| test | what it pins down |
|---|---|
| `m13_within_window` | intra-window duplicates still fold by dedup (6 merges for 8 elements on 2 lines); the directory is probed once per *distinct* line, not per element |
| `m13_cross_window` | a line repeated in the next window costs exactly one request for the whole run, whether the fill had landed or not; 4 elements retired with no request |
| `m13_word_offsets` | one line at offsets 0/4/8/60 across a boundary: each dependant gets its own word from the 64 B copy, and `addr_mismatch == 0` |
| `m13_multi_dependent` | one element per window on one line: 3 dependants attach to 1 request, all values correct |
| `m13_eviction` | a completed entry evicted by another line; re-accessing the evicted line re-issues rather than serving a stale entry |
| `m13_full` | the only entry is outstanding: the line issues normally instead of evicting a promise; nothing lost |
| `m13_no_reuse` | 8 distinct lines: zero hits, `reuse_miss == reuse_probes == planned` |
| `m13_adversarial` | two lines alternating across every window boundary in a 2-entry directory: thrash, correct values, residency bound held |
| `m13_accounting` | the five identities of §4.3 on a mixed hit/merge/miss pattern |
| `m13_descriptor_boundary` | a second descriptor reuses what the first retained, and its values are still the image's |

### 8.2 Regression anchors (all with the mechanism off)

| anchor | expected | measured |
|---|---|---|
| M10 W1 8K (A/B/C) | 220 123 / 138 924 / 125 581 | 220 123 / 138 924 / 125 581 |
| M10 W0 8K | 11 052 / 17 267 / 18 142 | 11 052 / 17 267 / 18 142 |
| M11.2 control `batch_window=16 384` | 106 456 | 106 456 |
| E0 policy anchor | 153 475 | 153 475 |

The matrix reproduces them cell for cell (the `w8192`, `w16384`, `w32768` rows of
§7.1 are the M12 numbers to the cycle), so no published result moved.

### 8.3 Run-level gates, all 158 cells

* `M9 GATES PASSED` — 158/158, `rc = 0`.
* `value hash` == expectation, `mismatched addresses` = 0 — 158/158 (the row is
  not printed for the all-bulk / all-SIMT controls, which have no mixed arm; the
  gates still ran).
* §4.3 identities — 158/158.
* `unissued + issued_not_filled <= R` — 158/158; peak total outstanding is 128
  in every W1 cell, i.e. exactly `R`, never above it. M13 adds no residency
  capacity: at K=16 384 the peak bulk residency is 128 (M10) vs 128 (M13) and
  the *mean* target population falls because 44.5 % of lines never become
  requests.

---

## 9. Limitations and unresolved issues

1. **Set associativity leaves headroom on the table.** At K=16 384 the array has
   16 384 entries but only 13 226 are valid at peak, and 36.9 % of lookups hit
   where the fully associative bound is 66.8 %. 8 ways recovers 3 % of cycles.
   A fully associative or higher-way directory would be faster; it would not be
   cheaper, so the storage conclusion does not depend on it.
2. **`reuse_sub_max` is small (≤ 3).** Only three dependant windows can attach
   to one outstanding line; the fourth falls back to a normal request (counted
   `reuse_sub_full`, measured 0 in every cell). With two buffers live this is
   not reachable, but a design with more buffers would need a deeper group.
3. **Only the C arm's reuse is exercised.** The buffer is engine-private; arm A
   is a different front-end and arm B is a diagnostic, so the tables report C.
   A shared reuse buffer visible to the SIMT arm is out of scope and would
   change the controlled comparison.
4. **The dump is per seal, so intra-window order is not represented.** Real
   order inside a window is unresolvable by construction (a seal looks the whole
   window up as a batch), which is exactly how the bound is computed; a
   directory that probed incrementally during admission could do marginally
   better and is not modelled.
5. **`Λ` is a property of the trace generator, not of a program.** W1's reuse
   distance is *exactly* one pass because the generator draws permutations.
   Real irregular gathers have a distribution; the mechanism's cost model (a
   probe per distinct line) does not change, but the capture curve would.
6. **M13 does not reproduce the window's page locality.** Row activations remain
   at the 8K level (5 559 vs the 32K window's 2 626). Combining retention with a
   larger page-cohort window is conceivable, but it is a *different* milestone
   and would change the visibility resource this one is required to hold fixed.
7. **PSCR shows the mechanism is walk-limited there too.** −19 % comes from
   removing requests (and their walks); the 32K window's −74 % comes from
   touching fewer pages per window. No retention scheme can substitute.

---

## 10. Recommendation

**Do not retain M13 as a configuration. Keep it as an off-by-default
instrument.**

Against the brief's decision criteria:

* *"Can M13 approach 32K performance with substantially less storage?"*
  **No.** At 508 032 B — 6 % *less* SRAM than the 2 × 32K pair — M13 delivers
  −5.8 % where the windows deliver −29.2 %. To beat the window it needs
  1 589 376 B, 2.9× the window's SRAM, and it still loses the row locality the
  window provides. The crossover is at `K = Λ`, and `K = Λ` costs `64 B × Λ`
  against the window's `8 B × 2W`, i.e. 8× more expensive per retained element
  of reuse distance.
* *"A smaller improvement may be worthwhile if storage and complexity are much
  lower."* The storage is not much lower, and the mechanism is not lower
  complexity: it adds a tag array, an LRU, an in-flight merge group and a
  second retirement path. On the one workload where it would be cheap
  (STRIDmix, Λ = 1 024 lines = 90 KB) it is a 7 % regression, because the L1
  already serves those requests.
* *"A negative result is valid if the L1 already captures most of it, or if
  retention costs too much."* Both hold, in different regimes, and both are
  measured above (§7.4, §5.2).
* *"Do not change the official M10 baseline."* Unchanged: `reuse_dir` defaults
  to 0, `batch_window` stays 8 192 in the M10 configuration, and all four
  anchors reproduce exactly.

What *is* worth keeping:

* The **reuse-distance analysis** (`m13_dump` + `m13_analyze.py`) as the tool
  that answers "would a bigger window help this trace?" in seconds, without a
  sweep. It is what showed that 16K/32K's win at N=16 384 is page grouping, not
  reuse.
* The **mechanism**, off by default, because it is the only way to *prove* the
  storage argument rather than argue it, and because a future design that
  already holds the footprint (a private L1 slice, a partitioned L2) would make
  it nearly free: at that point the marginal cost of the tags is small and the
  measured benefit is real (−16 % versus the best window configuration, with
  DRAM traffic 45 % below M10).

If a follow-up is wanted, the measurement points at **why the L2 misses at all**
rather than at another engine-side directory: the 1 MiB L2 is exactly
footprint-sized and shared with the index stream, and it hits 12.8 % on W1. A
capacity or partitioning study of the *shared* L2 would address the same 66.8 %
of repeats with hardware the machine already has, and is the only direction in
which the storage economics improve rather than worsen.

---

### Appendix A — files

| path | contents |
|---|---|
| `DX100_MEM/sim/src/bulk.cpp` | `win_reuse_probe`, `reuse_publish`, `reuse_abort`, `push_fan`, the seal hook, the fill hook, the gates |
| `DX100_MEM/sim/src/sim.hpp` | `Config::reuse_*`, `BulkStats::reuse_*`, `ReuseEnt`/`Sub`, `LineReq::reuse_ent` |
| `DX100_MEM/sim/src/ab.cpp` | section `[18]`, environment fields |
| `DX100_MEM/sim/tests/tests.cpp` | ten `m13_*` tests |
| `DX100_MEM/sim/build/wm/m13_matrix.sh` | 158-cell harness |
| `DX100_MEM/sim/build/wm/m13_extract.py` | `perf`, `reuse`, `mem`, `check`, `abl` |
| `DX100_MEM/sim/build/wm/m13_analyze.py` | reuse distances and the LRU capture curve |
| `DX100_MEM/sim/configs/ab_controlled.cfg` | documented M13 keys, all defaulting to off |
| `DX100_MEM/sim/build/wm/m13_*.txt` | 158 logs, `m13_matrix.log`, six dumps |

### Appendix B — reproduction

```
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests        # 4338 checks, 0 failures
./build/wm/m13_matrix.sh > build/wm/m13_matrix.log       # ~4 minutes, 158 cells
python3 build/wm/m13_extract.py perf                     # §7.1
python3 build/wm/m13_extract.py reuse                    # §7.2
python3 build/wm/m13_extract.py check                    # §8.3
python3 build/wm/m13_analyze.py                          # §2.2, §2.3
```
