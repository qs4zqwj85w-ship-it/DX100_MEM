# M23-G6G — Bulk physical DRAM row organization

**Question.** M23-G6F let the release inspect the whole ready buffer and pick
requests that matched a row the DRAM already had open; it changed Orkut S1 by
−0.038 %. G6G asks the architecturally different question: forget which rows are
open — *organize* the translated requests by their physical `(bank, row)`
destination and hand each group over as a burst, so an activation that opens a
row serves several of that row's requests.

**Answer.** No. The organization G6G asks for **already exists in the control**,
and it is the reason the control wins. `Batch::ready` is keyed by each run's
position in `ford`, and `ford` is sorted by `(row, bank, line)` from the
controller's own `dram_bank_row()`, so the release has always emitted
physical-cell bursts: the G0 control makes **290 769 cell runs for 823 642
releases (2.83 runs per burst)** on Orkut, and the DRAM already converts them into
**254 249 activations** — 0.874 activations per burst, i.e. it merges bursts
across visits. Re-choosing *which* group goes first changes nothing structural:
G1 opens 287 599 groups of mean size 2.84 (max 16, only 1.3 % full) and reaches
65.1 % same-cell contiguity, the control's 2.83. What changes is locality, and it
gets **worse**: reads per activation 2.5335 → 2.4743 (Orkut), 1.8568 → 1.8562
(LiveJournal), 2.8602 → 2.8549 (ogbn), because density-first abandons the cell the
DRAM is currently sweeping.

| workload | G0 cycles | G1 cycles | G2 cycles |
|---|---:|---:|---:|
| Orkut BFS S1 | 6 130 494 | 6 130 200 (−0.005 %) | **6 113 918 (−0.270 %)** |
| LiveJournal BFS | 14 146 860 | 14 162 344 (**+0.109 %**) | 14 161 076 (+0.100 %) |
| ogbn-products train | 98 563 897 | 98 865 110 (**+0.306 %**) | 98 540 523 (−0.024 %) |

The single gain — G2 on Orkut — is **not** row organization: it is an L2
side-effect (target L2 hit rate 30.94 % → 31.66 %, 5 740 fewer DRAM reads) while
activations *rise* 254 249 → 257 525 and reads per activation *fall*. On the two
workloads where that accident does not occur, the same reordering costs 0.02-0.31 %
of wall.

This is a simulator result throughout. No physical-GPU claim is made.

---

## 1. Architecture, and how it differs from G6F

| | G6F | G6G |
|---|---|---|
| what it reads | which rows the DRAM has **open right now** | which **cell** each buffered request targets |
| selection rule | open-row candidate first | densest cell first |
| relation to the buffer's key order | orthogonal (probes the bank tags) | *identical to it* (the key is the cell) |
| what it can change | the pick inside one decision | the order of decisions across cells |
| measured effect | −0.038 % (Orkut) | −0.005 / −0.270 % (G1/G2, Orkut) |

G6F's negative result had a mechanical cause: the buffer is strictly *ahead* of
the open rows, so it contained 0.011 open-row candidates per decision against the
queue's 4.26. G6G does not depend on the bank state at all — it asks only whether
the buffer can be re-cut into denser bursts. The measurement in §5 says it cannot:
the bursts are already as dense as the buffer's contents allow.

The required pipeline is implemented end to end and unchanged in every other
respect:

```
translation (real dTLB -> L2 TLB -> walker)
  -> ready buffer (4096 translated runs, keyed by (row,bank,line))
  -> (bank,row) group selection            <-- G6G decides here
  -> win_emit_fused: residency gate -> L1 bypass -> L2 lookup -> L2 MSHR
  -> DRAM queue -> FR-FCFS, bank timing, row buffers, command issue
```

## 2. Existing release path, inspected before changing anything

1. **Where translated requests enter.** `win_issue_g6b` drains
   `MemorySystem::take_xlate()` and pushes each completed probe's run position
   (`b.fpos[li]`) into `Batch::ready`. A run exists in the buffer only after its
   translation returned; `b.fpa[li]` is the physical line it was given.
2. **Cache lookup.** Not yet happened. A ready entry has no cache line, no L2
   MSHR and no DRAM queue entry. `win_emit_fused` submits it with
   `pretranslated = true`, so L1 is bypassed and the **L2 lookup is the first
   cache touch**.
3. **Which requests can still be L2 hits.** Measured on the control:
   Orkut 30.94 % of released runs, LiveJournal 15.17 %, ogbn 0.10 %. Only the
   remainder becomes a DRAM transaction, which is why the report separates
   *released* from *DRAM* counts everywhere.
4. **Identity and completion.** A release sets `b.fflag[li]` and allocates one
   residency slot; the slot is freed on completion by `on_target`, and the
   window's `pending`/`issued_n` are the only counters that move. Reordering
   changes *when* a run is released, never *which* slot or *whether* it is
   released once — `cc3_issue == cc3_done` and
   `bulk_tgt_req_gen == issued == done` in all nine cells (§7).
5. **Safe to reorder.** The window is one descriptor's target set: the fused run
   for each distinct line carries the element positions that depend on it, and a
   line is retired when its own fill lands (`on_target`), so no run waits on
   another run's issue order. Bulk-dispatched target reads are independent loads
   from a compiled BFS frontier; the model imposes no ordering between them, and
   the existing P0/P2/P3/P5/G6A/G6C policies already permute this set.
6. **What the release order already is.** `ford` for `bulk_org = 8` is
   `stable_sort` by `(row, bank, line)` of `dram_bank_row(pa_line_of_va_line(...))`
   — the *physical* map, not the virtual page. `ready` is a min-heap over those
   positions, so a pop always takes the smallest `(row, bank, line)` available:
   every pending run of the frontier cell precedes every other pending run.
   **The control is already a `(bank,row)`-grouped release.** Measured: 2.83
   releases per contiguous cell run (Orkut), 1.49 (LiveJournal), 1.68 (ogbn).

That last point is the experiment's pivot. G6G cannot "add" grouping that the key
order already produces; it can only choose a different group to serve next.

## 3. Implementation

Three pieces, all in the release path; nothing else in the model was touched.

### 3.1 Group state (`sim.hpp`, `Batch`)

```
g6g_cell            // G1: cell being drained (0xffffffff = none)
g6g_cell_b[8]       // G2: one per bank
g6g_grp_n[8]        // runs actually released into each open group
g6g_rr              // G2 bank cursor, advanced once per release
```

plus `g6g_sel_cell`/`g6g_last_cell`/`g6g_last_bank` for the contiguity counters.
All initialised in the window-adoption block, so a fresh window starts with no
group. Cell identity is `bank * cfg_.rows + row` — the key the DRAM itself uses;
there is no second address map.

### 3.2 `BulkEngine::pick_group` (`bulk.cpp`)

1. **Continue.** If the slot's open cell still has a pending run in the heap,
   return that cell's smallest key. This is what makes the group persistent: a
   line of the cell that finished translating late rejoins the burst in progress
   instead of opening a new one. The scan skips entries already released
   (`fflag`), exactly as G6F's did.
2. **Close.** When the buffer holds no pending run of the cell, the group ends;
   its realized length is added to `group_drain_sum` and the slot is cleared.
   A group is **never held open waiting for untranslated lines**, so the policy
   cannot stall the release port.
3. **Open.** One metadata pass over the heap counts pending runs per cell
   (generation-stamped arrays of `banks × rows` entries). The **densest** cell
   wins; ties go to the smaller key, and keys are unique, so the choice is a total
   order and does not depend on heap-array order. G2 restricts the choice to the
   bank whose turn it is (`g6g_rr`), falling back to the other bank only when that
   bank has nothing pending — a bank is never starved by the other's backlog.
4. **Order inside a group.** Ascending key, i.e. ascending physical line, because
   a cell's runs occupy a contiguous key range in ascending line order. No second
   sort: the seal-time sort is the intra-group order.

The chosen position is handed to the **unchanged** `win_emit_fused`; the release
gate check (`free_count() <= tgt_reserve()`) still runs *before* any scan, so
cycles where the residency budget is closed cost nothing extra.

### 3.3 Why the whole window is not organized at once

The fused window is 16 214 elements on average (11 130 runs at Orkut); the ready buffer is
4 096 runs and holds **704 cells** on average. Organizing the whole window before
release is exactly M23-G6A's barrier, which G6C beat: translation is streaming, so
at any instant the buffer contains only the runs whose probes have completed.
Grouping is therefore done over what is *available*, which is the only population
the DRAM can be fed from. Nothing is deferred to make a group bigger — that would
trade translation/DRAM overlap for burst length, and §5 shows the bursts cannot
get longer than the buffer's own occupancy anyway.

### 3.4 Knob

`Config::sched_group` (0 = off, 1 = G1, 2 = G2), parsed in `util.cpp` and echoed
in the configuration block **only when non-zero**, so every historical log's
header stays byte-identical.

## 4. Configurations and workloads

`build/wm/m23g6g_run.sh`, nine runs, frozen G6D/G6E/G6F settings:
`configs/m23g6b_base.cfg`, `batch_window=16384`, `win_fused=1`, `bulk_org=8`,
`g6b_pa_capacity=4096`, `sched_vis=0`, `walk_latency_per_level=150`,
`bulk_index_base=0x80000000`, `bulk_desc_line=0x1FFFFC0`, traces
`m23_orkut_bfs.bin` / `m23_livejournal_bfs.bin` / `m23_ogbn_train_100d.bin`
(`bulk_max_elems` 2097152 / 2097152 / 16777216), translation 8/8/8 for Orkut S1
and 4/8/8 for LiveJournal and ogbn.

| Cell | `sched_group` | Policy |
|---|---:|---|
| G0 | 0 | M23-G6C release: smallest `(row,bank,line)` key (the control) |
| G1 | 1 | physical `(bank,row)` groups, one at a time, densest first |
| G2 | 2 | the same groups, one per bank, banks alternating |

No other cell was run; no parameter sweep.

## 5. Results

`python3 tools/m23g6g_extract.py`; logs and `summary.csv` in `build/wm/m23g6g/`.
`acts` = `dram_row_acts_sum`, `a/run` = activations per released run
(`acts / bulk_g6b_runs_released`), `util` = `dram_busy_cycles / (2 × cycles)`,
`qcells`/`qhitC` = distinct cells and open-row candidates in the DRAM queue per
scheduling decision.

### 5.1 Orkut BFS, S1 (8/8/8)

```
cell cycles    dCyc%   reads  hit%  reads/act   acts  a/run  util%  qmean qcells qhitC  svcH  svcM
G0   6130494       --  644142 60.53    2.5335 254249 0.3087  98.43  60.81   27.0  4.26 14.00 26.00
G1   6130200  -0.005%  638100 59.58    2.4743 257892 0.3132  98.11  60.77   28.7  4.13 14.00 26.00
G2   6113918  -0.270%  638402 59.66    2.4790 257525 0.3126  98.37  60.68   29.0  4.20 14.00 26.00
```

### 5.2 LiveJournal BFS (4/8/8)

```
cell  cycles   dCyc%   reads  hit%  reads/act   acts  a/run  util%  qmean qcells qhitC
G0  14146860      --  697141 46.14    1.8568 375452 0.5150  50.42  18.56    7.5  2.81
G1  14162344 +0.109%  698179 46.13    1.8562 376140 0.5159  50.44  18.66    7.6  2.78
G2  14161076 +0.100%  698129 46.14    1.8566 376021 0.5158  50.44  18.67    7.7  2.79
```

### 5.3 ogbn-products train (4/8/8)

```
cell   cycles   dCyc%     reads  hit%  reads/act     acts  a/run  util%  qmean qcells qhitC
G0   98563897      --   9826463 65.04    2.8602  3435646 0.4100  90.70  22.70    7.7  3.01
G1   98865110 +0.306%   9828688 64.97    2.8549  3442694 0.4108  90.48  22.56    7.8  3.00
G2   98540523 -0.024%   9825972 65.02    2.8590  3436846 0.4101  90.73  22.77    7.9  3.01
```

### 5.4 Row groups

```
workload cell groups_opened  size_mean max  full16   drain  contiguity revisit%
orkut    G0        (290 769 cell runs: 2.83 runs each -- the control's own grouping)
         G1          287 599      2.84   16   3 781    2.86    65.1%     0.9%
         G2          287 635      2.83   16   3 793    2.86     0.4%     0.9%
livejour G0        (490 527 cell runs: 1.49)
         G1          461 366      1.44   16   1 442    1.58    36.7%    24.9%
         G2          474 270      1.42   16   1 440    1.54     8.0%    26.9%
ogbn              G0        (4 995 236 cell runs: 1.68)
         G1        4 535 423      1.63   11       0    1.85    45.9%    55.2%
         G2        4 164 682      1.68   11       0    2.01     2.7%    51.3%
```

Size distribution at open, Orkut G1: 112 038 singles, 61 960 twos, 65 050 of 3-4,
37 675 of 5-8, 7 095 of 9-15, 3 781 of 16. LiveJournal G1: 327 584 / 100 806 /
27 920 / 3 149 / 465 / 1 442. ogbn G1: 2 558 328 / 1 281 695 / 691 762 / 3 622 /
16 / 0 — ogbn's cells never fill (its shipped row width is 11 lines here).

**Reading the table.** The mean group is 2.84 runs at Orkut: the buffer simply
does not hold a dense row. Only 1.3 % of groups are a full 16-line cell, and the
revisit rate says a cell almost never comes back (0.9 %). G1's contiguity (65.1 %)
is not an improvement over the control's — the control's 2.83 runs per cell run
*is* 65 % contiguity — and G2 destroys contiguity (0.4 %) by alternating banks.

### 5.5 Activations per burst: the DRAM already merges bursts

The decisive number is `acts` versus the number of release bursts:

| workload | cell runs (bursts) | activations | acts / burst |
|---|---:|---:|---:|
| Orkut | 290 769 | 254 249 | **0.874** |
| LiveJournal | 490 527 | 375 452 | **0.766** |
| ogbn | 4 995 236 | 3 435 646 | **0.688** |

The controller performs **fewer activations than the release stream has cell
bursts**. Rows are therefore already being reused *across* bursts: a cell visited
again after an intervening cell still finds its row open, and index/walk traffic
on the same row is served by the same activation. Grouping the release into
"denser" bursts is re-cutting a unit the DRAM has already stopped caring about.

Under G1 that ratio gets **worse everywhere** (Orkut 0.874 → 0.897, LiveJournal
0.766 → 0.815, ogbn 0.688 → 0.759): the reordering removes the cross-burst
coincidences the controller was exploiting.

### 5.6 Why G1 loses locality

The frontier cell is the cell nearest the DRAM's current position in
`(row, bank)` order, and the address map makes that a sweep: `bank = hash(line)`,
`row = (line / 32) % 4096`, so consecutive key positions walk the DRAM in the
order its own map lays the data out. Choosing the *densest* cell instead detaches
the release order from that sweep. Measured cost: activations 254 249 → 257 892
(+1.4 %) while DRAM reads fell 0.9 %, i.e. **reads per activation 2.5335 → 2.4743
(−2.3 %)**, and the queue became more scattered (`qcells` 27.0 → 28.7).

### 5.7 Where the one gain comes from

G2/Orkut is 16 576 cycles faster and none of it is row organization. Service
cycles decompose exactly (t_hit = 14, t_miss = 26 throughout, so `svcH`/`svcM`
are unchanged): row hits 389 893 → 380 877 (−9 016 × 14 = −126 224 cycles),
conflicts 254 247 → 257 523 (+3 276 × 26 = +85 176), net −41 048 bank-cycles
because total reads fell 5 740. Those reads fell because the target **L2 hit rate
rose 30.94 % → 31.66 %** (hits 254 823 → 260 692): a different release order
changed which lines coalesced in L2. That is a cache effect that happens to help,
not the mechanism the experiment set out to test — and it does not reproduce:
LiveJournal's L2 hit rate *fell* (15.17 % → 15.03 %) and ogbn's is flat at 0.10 %.

### 5.8 The other required measurements

* **Bank utilisation**: Orkut 98.43 / 98.11 / 98.37 %, LiveJournal 50.42 / 50.44 /
  50.44 %, ogbn 90.70 / 90.48 / 90.73 %. The Orkut DRAM is saturated, which is why
  a 1 % change in work moves wall by ~1 %.
* **DRAM queue occupancy** (`qmean`): 60.81 / 60.77 / 60.68 (Orkut), 18.56 / 18.66
  / 18.67 (LJ), 22.70 / 22.56 / 22.77 (ogbn); `dram_max_q` 63 (62 on ogbn G0/G2),
  `dram_q_full = 0` everywhere — no cell pushed the queue harder.
* **Service cycles per read**: 14.00 per hit and 26.00 per conflict in every cell
  of every workload; the timing model never moved.
* **Bulk buffer occupancy**: 2045 → 1774 → 1745 runs (Orkut), 59.0 → 60.1 → 59.2
  (LJ), 23.8 → 21.0 → 21.7 (ogbn).
* **Waiting time in the buffer**: mean 15 224 → 13 204 → 12 949 cycles and **max
  40 746 → 128 302 → 128 154** (Orkut). Density-first releases most runs sooner
  and strands a few for 3.1× longer — a real fairness cost of the policy.
* **Translation/admission stalls**: refusals 30.87 M → 31.13 M → 31.15 M (Orkut,
  +0.9 %), 110.02 M → 109.99 M → 109.98 M (LJ), 770.6 M → 773.0 M → 770.4 M
  (ogbn); full-buffer backpressure cycles 497 292 → 363 117 → 354 360 (Orkut),
  84 373 → 101 479 → 101 538 (LJ), 71 666 → 71 666 → 71 672 (ogbn). Translation
  settings and the dTLB were not touched; these move only because the buffer's
  occupancy profile moves.

### 5.9 Population accounting (Orkut, G0 → G1 → G2)

```
translated runs released        823 642 → 823 475 → 823 759
entering L2 lookup (cc3_l2_reads) 823 643 → 823 476 → 823 760
L1 hits                             0 (bypass, all cells)
L2 hits                         254 823 → 260 714 → 260 692
L2 misses = target DRAM requests 568 820 → 562 762 → 563 068
all-client dram_reads           644 142 → 638 100 → 638 402
DRAM commands (activations)     254 249 → 257 892 → 257 525
```

Only 69 % of translated runs reach DRAM at Orkut and 0.1 % at ogbn, which is why
reorganizing the *translated* population cannot by itself reorganize the DRAM's.

## 6. Correctness validation

| Check | Result |
|---|---|
| Test suite | `./bin/dx100_tests` → **4 767 checks, 0 failures** |
| Control reproduction | G0/Orkut 6 130 494, G0/LiveJournal 14 146 860, G0/ogbn 98 563 897 — and **all 754 pre-existing keys byte-identical** to the M23-G6F F0 logs on all three workloads (`diff` excluding `bulk_g6g_*` → 0 lines) |
| Conservation | `requests_generated == issued == completed` in all nine cells; `bulk_tgt_req_gen == issued == done`; `conservation_ok = 1`, `conservation_violations = 0` |
| No duplicate issue/completion | `cc3_issue == cc3_done` in all nine cells; every release sets `fflag` and its stale heap entry is skipped, never re-issued |
| No lost requests | `bulk_elems_done == bulk_elems_total` on all three workloads in all cells |
| No deadlock / liveness | all nine runs terminated normally; `pick_group` never waits for untranslated lines, and the fallback always returns a pending entry |
| MSHR / outstanding limits | `l2_mshr_max = 64`, `dram_max_q ≤ 63`, `dram_q_full = 0`, `dram_missed_hit = 0`, `bulk_residency_violations = 0`, `bulk_issue_monotonic_violations = 0` in every cell |
| Group selection integrity | `bulk_g6g_sel_mismatch = 0` everywhere — the cell the release believed it was draining is the cell of the run it actually released |
| Determinism | G2/Orkut repeated: **byte-identical log** |
| Existing artifacts untouched | only `build/wm/m23g6g/` written; G6D/G6E/G6F logs read only |

Memory-ordering: the reordered set is one descriptor's target reads — independent
loads from a compiled BFS frontier, each retired by its own fill. No run's
completion depends on another run's issue order, and the policies already shipped
(P0/P2/P3/P5/G6A/G6C) permute the same set, so G6G introduces no ordering that the
existing model does not already permit.

## 7. Hardware cost and feasibility

* **Key width.** `(bank, row)` = 2 banks × 4 096 rows = 8 192 cells, 13 bits. The
  engine does not need an address computation: `b.fpa[li]` is the physical line
  the translation already produced, and `dram_bank_row` is the controller's own
  tag path.
* **What was measured is not a hardware proposal.** The simulator's grouping pass
  is a serial scan of the whole ready buffer per release: 1.90 × 10⁹ cell
  evaluations on Orkut (310 per cycle of a 6.1 M-cycle run). That is an upper
  bound on what any ordering can achieve, not a feasible per-cycle design.
* **A realisable form** is an occupancy table, not a sorter: one
  `{count, head}` entry per mapped cell (8 192 × ~3 B ≈ 24 kB SRAM), updated on
  push/pop in O(1), plus a per-bank arbiter. "Densest first" then needs a
  4 096-entry max-reduction per bank (12-level tree, ~8 k 5-bit comparators per
  bank) — the same wide-reduction problem G6F identified and rejected — or an
  approximation, a per-bank round-robin over non-empty cells, which is O(1) but
  *not* the policy measured here.
* **A full 4 096-entry sort per window is not required and would not help.** The
  buffer is already sorted by cell at seal time; the only dynamic part is which
  cells are *available*, and the group order inside a cell is the seal order.
* **Pipelining** is possible (the grouping decision only has to be ready when a
  residency slot frees, ≥1 cycle ahead), but it buys nothing: the measured payoff
  of the *unconstrained* version is −0.005 % (G1) and −0.270 % (G2, cache
  side-effect), with +0.10 % to +0.31 % losses elsewhere.
* **No synthesis was run.** No area, frequency or power number here is backed by
  one.

## 8. Limitations

1. **Three workloads, one dispatch point.** Orkut S1, LiveJournal and ogbn train
   at their G6D translation winners; no other shapes were run, per the brief.
2. **One window size and one buffer.** 16 384-entry window, 4 096-run ready
   buffer, 2 banks × 4 096 rows × 1 KiB. A wider window would raise per-cell
   occupancy (2.84 is a property of *this* window), so the negative result is
   bounded by the configuration that was measured — but so is every positive one.
3. **The "densest first" rule is one choice among many.** Frontier-first would
   have been identical to G0 by construction (§2.6); largest-first and
   per-bank-alternating are the two that actually change the sequence, and both
   were measured.
4. **Groups are not held for completion.** A policy that waits for a cell's lines
   to finish translating would make longer bursts, at the cost of the
   translation/DRAM overlap G6C won. §5.4 bounds its payoff: cells hold 2.84 runs
   on average and only 0.9 % of them return, so holding mostly waits.

## 9. Recommendation

**Do not adopt physical `(bank,row)` group release, in either form.** The
hypothesis — denser request sequences per row raise reads per activation — is
false for this machine, and the reason is structural rather than an artefact of
the implementation:

1. the release order **is** the `(bank,row)` organization already (`ford` is
   sorted by the controller's own map), delivering 2.83 runs per cell burst at
   Orkut against a 2.84 mean group size under G1 — the policy has nothing to add;
2. the buffer cannot supply denser rows: 1.3 % of cells reach their 16-line
   width, and the DRAM already performs **fewer activations than bursts**
   (0.87 / 0.77 / 0.69), i.e. it merges bursts across visits;
3. re-choosing the group **costs** locality (reads per activation −2.3 % Orkut,
   −0.03 % LiveJournal, −0.2 % ogbn) and 3.1× worst-case buffer wait;
4. the only cycle win (−0.27 %, Orkut G2) is an L2 coalescing accident
   (+0.72 pp hit rate) that does not reproduce and buys nothing where the DRAM is
   less saturated.

**Diagnosis for the next step.** The limiting factor is the *supply of requests
per physical row inside the translated window*, not the release order, not cache
filtering as such, and not cross-bank scheduling (G2 vs G1 bounds interleaving at
≤0.3 % of wall). Three consequences:

* Widening the window is the only lever that raises per-cell occupancy. At Orkut
  the 4 096-run buffer spans 704 cells (2.9 lines each); 16 lines per cell needs a
  buffer roughly 5× deeper, which the residency budget (R − 8 = 120 outstanding)
  would not let the DRAM consume anyway.
* **M23-G7A (row-completion release)** should be re-scoped before it is run: its
  premise is that a cell can be made dense by holding it, and G6G measures the
  material it would hold — 2.84 runs, 0.9 % revisit. Unless the hold policy is
  allowed to *wait across translation*, it has no more to work with than G1 did.
* If locality is still the goal, the surviving lever is the **DRAM controller**,
  not the release: G6E's 7.4 % ceiling is on the controller's side, and the
  measured conflicts are `case_c` (no hit queued at all, 51.5 % of S1 conflicts)
  and `case_b` (hit queued in the busy bank, 48.5 %) — both still open.

## Instrumentation added

`Config::sched_group` (util.cpp parse + conditional echo); `Batch::g6g_cell`,
`g6g_cell_b[8]`, `g6g_grp_n[8]`, `g6g_rr`, `g6g_sel_*`, `g6g_last_*`;
`BulkEngine::pick_group` and `note_group_release` with generation-stamped
per-cell count/min arrays; `BulkStats::g6g_*` counters
(`groups_opened`, `groups_closed`, `group_runs`, `group_size_*`,
`group_drain_*`, `group_hist[6]`, `same_cell`, `switch_cell`, `bank_alt`,
`revisit`, `sel_mismatch`, `scan_calls`, `scan_entries`), exported as
`bulk_g6g_*`. All are release-selection state or diagnostics; none is read by the
DRAM model, the cache hierarchy or any timing decision, and all are zero (and
unallocated) when `sched_group = 0`.

## Reproduce

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests          # 4767 checks, 0 failures
cd .. && ./build/wm/m23g6g_run.sh orkut                    # 3 min
cd .. && ./build/wm/m23g6g_run.sh livejournal               # 1 min
cd .. && ./build/wm/m23g6g_run.sh ogbntrain                 # 13 min
cd .. && python3 tools/m23g6g_extract.py                    # tables + summary.csv
# control identity: diff excluding bulk_g6g_* against build/wm/m23g6f/g6f_F0_<wl>.txt
```
