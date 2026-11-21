# M23-G6H — Full 4K-window physical-row organization: audit and fix

**Question.** Does the Bulk physical-row organizer see and organize the *entire* eligible
population of its 4 096-entry translated-request buffer, and if not, what has to change?

**Answer.** It already does. `pick_group` enumerates the whole ready buffer on every
organization decision — there is no prefix, cursor, candidate cap or early exit anywhere on
that path, and the buffer reaches its full 4 096 entries on all three workloads (measured
`bulk_g6b_ready_peak = 4096`, `bulk_g6h_exam_max = 4096`). The invariant
`organizer_entries_examined == eligible_entries_in_window` holds on every one of the
1.05 × 10⁷ decisions recorded here, with zero excluded entries.

What G6G did *not* do is reproduce **G3 P2's selection rule**: G1 chooses the densest
`(bank,row)` group, G2 alternates banks, while P2 takes the group whose key is smallest in
`(row, bank, line)` order. Adding P2's rule over the same complete population (arm
`sched_group = 3`, called **P2F** below) reproduces the control's row-hit count exactly on
Orkut and to within 2 hits elsewhere, and lands at −0.0009 % / +0.0001 % / +0.0046 % of the
control — i.e. full-window P2 organization *works* and buys nothing. The reason is in §6:
the release order already is the P2 order, and the DRAM controller's own 60-deep queue
extracts the row hits either way.

Everything below is measured with the G6G/G6D configuration; nothing outside
`build/wm/m23g6h/` was written and no historical artifact was touched.

## 0. Verdict in three lines

| Claim | Evidence |
|---|---|
| No visibility limitation exists in this path. | source: two `for (i < b.ready.size())` passes, no cap; measurement: exam mean 1836.8 / 39.3 / 15.8, max 4096, `stale = 0`, `inv_viol = 0` (§1, §4) |
| G6G differed from G3 P2 in its **selection key**, not its window. | G1 = densest cell, G2 = bank rotation, P2 = smallest `(row,bank)` key; P2F restores the key and returns to the control's DRAM behaviour (§2, §3, §5, §6) |
| Full-window P2 organization is not a gain on these traces. | P2F: 6 130 439 / 14 146 872 / 98 568 442 cycles vs control 6 130 494 / 14 146 860 / 98 563 897; row hits identical on Orkut (389 893), +2 of 321 689 on LiveJournal, +2 091 of 6 390 817 on ogbn (§5, §6) |

## 1. Does a visibility limitation exist? (source level)

### 1.1 The structure the organizer reads

`Batch::ready` is a min-heap over a `std::vector<u64>` of *window positions*
(`sim.hpp`, `Batch::ready`). A position is a run's rank in `ford`, and `ford` is
`stable_sort`ed at seal time by

```cpp
if (row[x] != row[y]) return row[x] < row[y];
if (bank[x] != bank[y]) return bank[x] < bank[y];
return x < y;                       // sealed order == ascending line
```

with `(bank, row) = mem_.dram_bank_row(mem_.pa_line_of_va_line(run_line(b, i)))`
(`bulk.cpp`, `win_organize_fused`). So **the heap key is the P2 rank of the run**, and all
runs of one physical cell occupy a contiguous interval of keys.

Entries enter only from a completed translation
(`while (mem_.take_xlate(x)) { … ready_push(b, b.fpos[li]); }`) and leave only through
`ready_erase_val(b, pos)`, which removes the key from the heap in O(log n) the same cycle
the run is released. Capacity is `Config::g6b_pa_capacity = 4096`, enforced before
admission by

```cpp
const u64 pend = tr_next_or_submitted - b.g6b_tr_done;   // probes in flight
const u64 held = static_cast<u64>(b.ready.size()) + pend; // buffer + in flight
if (more && held >= cap) { ++st_.g6b_backpressure; }
```

so `ready` can never exceed 4 096 and nothing is silently dropped.

### 1.2 What `pick_group` actually reads

`BulkEngine::pick_group` has exactly two enumeration sites, and both are full passes:

```cpp
if (open_cell != 0xffffffffu) {                       // (1) keep draining the open group
  for (size_t i = 0; i < b.ready.size(); ++i) { … }
}
g6g_cells_.clear();                                   // (2) count every destination cell
for (size_t i = 0; i < b.ready.size(); ++i) { … }
```

There is no `std::min(size, N)`, no `nth_element` prefix, no per-bank cursor and no
`break` on a populated buffer. The only filter is `if (b.fflag[li] & 1u) continue;` — an
entry already released — and because `ready_erase_val` removes such an entry immediately,
that branch is dead: **measured `bulk_g6h_stale_sum = 0` in all six instrumented cells**.

### 1.3 Where a limit *would* have been — and is not

* `Config::sched_vis` (G6F) is the knob that bounds candidate visibility: `N > 0` compares
  only the `N` smallest positions (`nth_element` prefix in `pick_candidate`). The 61 / 143
  / 1024 values the brief names are `sched_vis` settings. All G6G and G6H cells run
  `sched_vis = 0`, and `pick_candidate` is not called at all when `sched_group != 0`
  (`pos = cfg_.sched_group ? pick_group(b) : (cfg_.sched_vis ? pick_candidate(b) : …)`).
* The release loop's pre-check `if ((sched_vis||sched_group) && free_count() <= tgt_reserve())
  break;` skips the *decision*, not part of the population, and only when `win_emit_fused`
  would refuse every candidate identically (it repeats the same test as its first act).
* `pick_group`'s `throw` on `cfg_.banks > 8` is a DRAM-map width guard, not a candidate cap.

### 1.4 What is legitimately outside the organizer

Runs whose translation has not completed. `bulk_g6h_untr_*` counts
`fmeta.size() - g6b_tr_done` at each decision — in flight or not yet admitted — and it is
large: **mean 3 175.9 runs on Orkut, 4 570.6 on LiveJournal, 7 768.8 on ogbn** (§4). So the
organizer never sees the whole *window*; it sees the whole *eligible buffer*, which is the
architecture the brief specifies (steps 2–3: translate, then make translated requests
visible). No request is ever organized before its physical address exists, and none is
organized twice.

### 1.5 The control does not enumerate — and does not need to

G0 reads `b.ready.front()`, one entry, per release. That is not a narrower view of the
population: the heap invariant makes the front the *global minimum key* of the eligible set,
so every entry constrains the choice without being scanned. This is why G0 already issues in
P2 order (§2.5) and why the fully-enumerating P2F arm lands on top of it.

**Conclusion for §1.** There is no original visibility limitation to explain: the 4K window
is fully visible, the counter `bulk_g6h_exam_max` reaches the configured capacity, and no
entry is excluded from a decision that could have changed it. The G6G gap was in *which*
group the complete population selects.

## 2. G3 P2 versus G6G, dimension by dimension

### 2.1 The two implementations

**G3 P2** (`bulk_org = 2`, `page_sched = 1`; `docs/m23-g3-bulk-locality-organization.md` §3):
the sealed window's runs are ordered by `(row, bank, line)` in `ford`; `win_open_cohort_fused`
opens one cohort per contiguous `(bank,row)` span, in `ford` order; `win_issue_sched_fused`
issues from the first open cohort with a runnable position, so the window is drained
cell-by-cell in ascending key order. A dTLB refusal parks that cohort at `resume_at` and
the issue moves to the next open one — that deferral is the only thing that can reorder P2's
sequence, and it is a translation event, not a policy choice.

**G6G G1** (`bulk_org = 7/8`, `sched_group = 1`): translated runs sit in `ready`; a group is
the set of pending runs of one physical cell; the cell with **the most pending runs** wins
(ties: smaller key), it is drained in ascending key order, and it closes the moment the
buffer holds no pending run of that cell.

**G6G G2** (`sched_group = 2`): the same choice inside the bank whose turn it is, released
alternately, with a fall-back to the other bank.

**G6H P2F** (`sched_group = 3`, new): the cell whose smallest key is smallest wins — P2's own
rule — drained and closed exactly as G1.

### 2.2 Side by side

| | G3 P2 | G6G G1 | G6G G2 | G6H P2F |
|---|---|---|---|---|
| physical key | `dram_bank_row(pa_line_of_va_line(vl))` | same | same | same |
| population | every run of the sealed window (≈11 130 runs) | translated runs in `ready` (≤4 096) | same | same |
| eligibility | run translated at its turn | translated, unreleased | same | same |
| ordering key | ascending `(row, bank, line)` | descending pending count, then smallest key | same per bank, banks alternate | ascending `(row, bank)` cell key |
| ordering through release | cohorts opened in `ford` order; drained in line order | group drained in ascending key | same | same |
| group persistence | per window, until the cohort's span is issued | until the cell leaves the buffer | same | same |
| capacity that bounds it | the window (`batch_window`) | `g6b_pa_capacity` = 4 096 | same | same |
| cross-bank interleaving | none (cell-major) | none | imposed by a bank cursor | none (cell-major) |
| waits for untranslated runs | no (it translates at issue) | no | no | no |

### 2.3 What actually differed

Population and key are **identical** in all four; the ordering rule is not. G1 and G2 are
alternative policies that G6G introduced deliberately (densest-first, bank-rotating), and
the G6G report says so — but the brief's question is whether the *original* P2 behaves
differently once the whole eligible window is visible, and G6G never tested that. P2F is
that test: same key as P2, same population as G6G, no interleaving, no density heuristic.

### 2.4 Why G6G could not have reproduced P2 by construction

G6G's own measurement bounds it: the control leaves 2.83 releases per cell run
(`bulk_g6b_release_pops / bulk_sched_opens`), so the buffer's cells hold 2.84 runs on
average. A densest-first rule given 1.43–2.84 runs per cell can only reshuffle a handful of
runs; a key-ordered rule on the same material reproduces the control (§5). Neither can add
row hits the control does not already have.

### 2.5 And G0 already *is* P2 over the eligible set

Keys of one cell are contiguous in `ford`, so popping the global minimum key visits cells in
ascending key order and drains each one before moving on. G0 therefore issues exactly the P2
order restricted to the runs that have translated — measured: 2.83 releases per cell run on
Orkut, 1.49 on LiveJournal, 1.68 on ogbn. P2F differs from it only when a cell's *late*
run joins after the group closed (2 818 of 287 797 groups on Orkut, 0.98 %) or when a
lower-keyed cell arrives while another group is open; that is why P2F's cycle count lands
within −0.0009 % of G0 while its `sched_opens` fall 1.0 % (290 769 → 287 797).

## 3. Corrected implementation

One branch in `BulkEngine::pick_group`, no new storage beyond the generation-stamped arrays
G6G already keeps:

```cpp
if (cfg_.sched_group == 3) {
  // P2's own rule: the cell holding the smallest rank in `ford` -- the cell's P2 key.
  for (u32 c : g6g_cells_)
    if (best_cell == 0xffffffffu || g6g_min_[c] < g6g_min_[best_cell]) best_cell = c;
  if (best_cell != 0xffffffffu) best_cnt = g6g_cnt_[best_cell];
}
```

* `g6g_min_[c]` is the smallest position of cell `c` among the pending entries, so
  `argmin_c g6g_min_[c]` is the cell that owns the global minimum key — the front of the P2
  order. The choice is a total order on a unique key: deterministic, no tie-break, no
  dependence on heap-array order.
* The group is then drained by the existing step (1), which returns that cell's smallest
  pending position; when the cell has no pending run left the group closes immediately. It
  never waits for an untranslated line, so it cannot stall the release (and G6C's
  translation/DRAM overlap is untouched).
* No bank rotation, no density, no reordering *inside* the cell: within a cell the release
  is ascending line order, exactly as P2's cohort issues it.
* `sched_group = 0/1/2` are byte-identical to before; `pick_candidate` and `sched_vis` are
  unchanged; nothing in the cache hierarchy, the residency budget, the L2 MSHR file, the
  DRAM timing or the FR-FCFS arbitration was touched (`memory.cpp` and `system.cpp` are not
  modified by G6H).

Instrumentation (all `BulkStats`, exported as `bulk_g6h_*`, written only when
`sched_group != 0`, read by no decision):

| Counter | Meaning |
|---|---|
| `decisions` | complete organization decisions (one full enumeration each) |
| `exam_sum` / `exam_max` | entries examined per decision, summed / peak |
| `elig_sum` / `elig_max` | live (translated, unreleased) entries per decision |
| `stale_sum` | released heap entries the scan skipped — must be 0 |
| `untr_sum` / `untr_max` | runs of the same window still in translation at the decision |
| `groups_sum` / `groups_max` | distinct physical cells visible per decision |
| `considered_sum` | cells the choice weighed (equal to `groups_sum`) |
| `deferred_sum` | eligible runs not in the chosen group (left for a later decision) |
| `inv_viol` | decisions where `exam != elig + stale`, or `elig != Σ per-cell counts` |

## 4. Eligible-window visibility measurements

`exam` = `b.ready.size()` at the decision, `elig` = entries with `fflag & 1 == 0`,
`untr` = `fmeta.size() - g6b_tr_done`, `cells` = distinct `(bank,row)` keys seen, `req/cell`
= `elig_sum / groups_sum`.

| Workload | cell | capacity | decisions | exam mean | exam max | elig mean | stale | untr mean | untr max | cells mean | cells max | req/cell | deferred | `inv` |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut | G1 | 4096 | 287 599 | 1 330.7 | **4 096** | 1 330.7 | 0 | 2 612.1 | 16 382 | 928.6 | 2 892 | 1.43 | 99.79 % | 0 |
| Orkut | P2F | 4096 | 287 797 | 1 836.8 | **4 096** | 1 836.8 | 0 | 3 175.9 | 16 382 | 715.7 | 2 070 | 2.57 | 99.85 % | 0 |
| LiveJournal | G1 | 4096 | 461 366 | 33.9 | **4 096** | 33.9 | 0 | 4 554.3 | 16 380 | 18.6 | 1 627 | 1.83 | 95.75 % | 0 |
| LiveJournal | P2F | 4096 | 460 417 | 39.3 | **4 096** | 39.3 | 0 | 4 570.6 | 16 380 | 13.9 | 1 268 | 2.84 | 96.33 % | 0 |
| ogbn train | G1 | 4096 | 4 535 423 | 12.9 | **4 096** | 12.9 | 0 | 7 773.2 | 16 380 | 8.1 | 1 239 | 1.59 | 87.37 % | 0 |
| ogbn train | P2F | 4096 | 4 466 504 | 15.8 | **4 096** | 15.8 | 0 | 7 768.8 | 16 380 | 6.5 | 1 231 | 2.42 | 89.34 % | 0 |

**The required invariant.** `exam_sum == elig_sum` exactly in every cell (Orkut
528 631 108, LiveJournal 18 110 293, ogbn 70 484 531 for P2F), because `stale_sum = 0`; and
`elig` equals the sum of the per-cell counts in every decision (`inv_viol = 0`). The
population examined **reaches the configured capacity** (`exam_max = 4096`) on all six
cells, so the 4K window is not merely addressable but realised.

**Decisions that intentionally operate on a smaller population.** Two, both named:

1. **G0** never enumerates (0 decisions recorded). It pops the heap front, one entry, and
   the heap invariant makes that entry the global minimum of the same population (§1.5).
   Its population is reported instead by buffer occupancy: 2 045.3 / 59.0 / 23.8 runs on
   average, peak 4 096.
2. **The drain-check pass** (step (1)) also reads the whole buffer but only looks for the
   open cell's members; it is counted in `bulk_g6g_scan_*`, not in the decision counters.
   Orkut P2F: `scan_calls = 1 111 361` = 287 797 decisions + 823 564 drain checks.

**What the numbers mean.** Orkut's buffer averages 1 837 eligible runs spread over 716
cells — 36.6 % of the runs in the buffer's window are ready, the rest are in translation.
LiveJournal's buffer is nearly empty at decision time (39 runs, 14 cells): releases consume
translations as fast as they land, so there is almost no population to organize, even though
its peak occupancy is also 4 096. ogbn's 4.47 M decisions each see 15.8 runs in 6.5 cells.
The grouping problem G6G attacked is therefore *material-limited*, not visibility-limited,
on two of the three workloads.

## 5. Workload results

Nine cells: three arms × three workloads (823 642 / 729 066 / 8 380 095 target releases),
all on `configs/m23g6b_base.cfg` with
`batch_window = 16384`, `bulk_org = 8`, `g6b_pa_capacity = 4096`, `sched_vis = 0`, Orkut
S1 = 8 walkers / 8 cohorts / 8 MSHRs and LiveJournal and ogbn train at 4 / 8 / 8. No
parameter sweep.

| Workload | arm | `sched_group` | cycles | vs G0 | DRAM reads | row hit | acts | reads/act | acts per cell run | `sched_opens` | releases/cell run |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut S1 | G0 | 0 | 6 130 494 | — | 644 142 | 60.529 % | 254 249 | 2.5335 | 0.874 | 290 769 | 2.833 |
| Orkut S1 | G1 | 1 | 6 130 200 | −0.005 % | 638 100 | 59.584 % | 257 892 | 2.4743 | 0.897 | 287 599 | 2.863 |
| Orkut S1 | **P2F** | 3 | **6 130 439** | **−0.001 %** | 644 165 | 60.527 % | 254 272 | 2.5334 | 0.884 | 287 797 | 2.862 |
| LiveJournal | G0 | 0 | 14 146 860 | — | 697 141 | 46.144 % | 375 452 | 1.8568 | 0.765 | 490 527 | 1.486 |
| LiveJournal | G1 | 1 | 14 162 344 | +0.109 % | 698 179 | 46.126 % | 376 140 | 1.8562 | 0.815 | 461 366 | 1.580 |
| LiveJournal | **P2F** | 3 | **14 146 872** | **+0.000 %** | 697 142 | 46.144 % | 375 451 | 1.8568 | 0.816 | 460 417 | 1.583 |
| ogbn train | G0 | 0 | 98 563 897 | — | 9 826 463 | 65.037 % | 3 435 646 | 2.8602 | 0.688 | 4 995 236 | 1.678 |
| ogbn train | G1 | 1 | 98 865 110 | +0.306 % | 9 828 688 | 64.973 % | 3 442 694 | 2.8549 | 0.759 | 4 535 423 | 1.848 |
| ogbn train | **P2F** | 3 | **98 568 442** | **+0.005 %** | 9 826 967 | 65.055 % | 3 434 059 | 2.8616 | 0.769 | 4 466 504 | 1.876 |

**Group behaviour** (G6G counters, which G6H reuses; G0 has no groups):

| Workload | arm | groups opened | mean size at open | max | mean drain | same-cell releases | cell revisits | worst-case buffer wait |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Orkut | G1 | 287 599 | 2.84 | 16 | 2.86 | 65.08 % | 0.91 % | 128 302 cyc |
| Orkut | P2F | 287 797 | 2.84 | 16 | 2.86 | 65.06 % | 0.98 % | 40 746 cyc |
| LiveJournal | G1 | 461 366 | 1.44 | 16 | 1.58 | 36.72 % | 24.89 % | 128 549 cyc |
| LiveJournal | P2F | 460 417 | 1.44 | 16 | 1.58 | 36.85 % | 24.74 % | 40 086 cyc |
| ogbn | G1 | 4 535 423 | 1.63 | 11 | 1.85 | 45.88 % | 55.15 % | 279 855 cyc |
| ogbn | P2F | 4 466 504 | 1.68 | 15 | 1.88 | 46.70 % | 54.54 % | 262 329 cyc |

P2F keeps the control's worst-case wait (40 746 cycles on Orkut, identical to G0) because it
never defers a release to hunt for a denser group; G1 pays 3.1× that on the same workload.
Groups closed are `opened − 74` on Orkut and LiveJournal and `opened − 513` on ogbn — exactly
`bulk_batches`, one group still open when each window ends.

## 6. DRAM row hits and reads per activation

Row hits are the quantity the organization is supposed to raise; reads per activation is the
quantity P2's hypothesis predicts it will raise.

| Workload | arm | row hits | row hit | activations | reads/act | conflicts | service hit / miss | bank util |
|---|---|---:|---:|---:|---:|---:|---|---:|
| Orkut | G0 | 389 893 | 60.529 % | 254 249 | 2.5335 | 254 247 | 8.47 / 10.26 | 98.43 % |
| Orkut | G1 | 380 208 | 59.584 % | 257 892 | 2.4743 | 257 890 | 8.34 / 10.51 | 98.11 % |
| Orkut | P2F | **389 893** | 60.527 % | 254 272 | 2.5334 | 254 270 | 8.47 / 10.26 | 98.44 % |
| LiveJournal | G0 | 321 689 | 46.144 % | 375 452 | 1.8568 | 375 450 | 6.46 / 14.00 | 50.42 % |
| LiveJournal | G1 | 322 039 | 46.126 % | 376 140 | 1.8562 | 376 138 | 6.46 / 14.01 | 50.44 % |
| LiveJournal | P2F | **321 691** | 46.144 % | 375 451 | 1.8568 | 375 449 | 6.46 / 14.00 | 50.42 % |
| ogbn | G0 | 6 390 817 | 65.037 % | 3 435 646 | 2.8602 | 3 435 644 | 9.11 / 9.09 | 90.70 % |
| ogbn | G1 | 6 385 994 | 64.973 % | 3 442 694 | 2.8549 | 3 442 692 | 9.10 / 9.11 | 90.48 % |
| ogbn | P2F | **6 392 908** | 65.055 % | 3 434 059 | 2.8616 | 3 434 057 | 9.11 / 9.09 | 90.69 % |

Reading:

1. **P2's organization works and is already there.** On Orkut, P2F produces *exactly* the
   control's row-hit count (389 893 of 644 165 reads) and 23 more activations; on LiveJournal
   it adds 2 hits; on ogbn it adds 2 091 hits and removes 1 587 activations
   (reads/act 2.8602 → 2.8616, +0.05 %). That is the whole payoff of organizing 4 096
   candidates by physical row: **≤ 0.05 %** of reads per activation.
2. **The 4K window is not the constraint; the controller's queue is.** The DRAM already
   performs *fewer* activations than the release produces cell runs (0.874 / 0.688 on
   G0), i.e. FR-FCFS merges bursts across visits inside its own queue (mean 60.8 / 22.7
   queued requests). Reordering 4 096 candidates in front of a 60-deep, hit-first
   reorder buffer cannot create a hit the queue would not have found.
3. **G6G's numbers were its selection rule.** G1 loses 9 685 hits on Orkut (60.53 % →
   59.58 %, reads/act 2.5335 → 2.4743) and 4 823 on ogbn precisely because densest-first
   departs from the key order; P2F restores it and returns to the control.
4. **The anchor says the same thing with no translation at all.** G3's own ideal-translation
   cells put P2 over the *complete sealed window* at 6 076 339 cycles against P0's
   6 077 523 on Orkut (−0.02 %), and 0.27 % / 0.10 % *worse* on ogbn-uniform and Criteo.
   That is the ceiling of this idea under perfect translation, measured before G6G existed,
   and it is ~0.

## 7. Cache and translation effects

| Workload | arm | L2 hit | L2 reads | L2 misses | `walks` | `sched_opens` | ready avg | ready max | ready wait mean / max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| Orkut | G0 | 30.94 % | 823 643 | 568 820 | 107 405 | 290 769 | 2 045.3 | 4 096 | 15 224 / 40 746 |
| Orkut | G1 | 31.66 % | 823 476 | 562 762 | 107 396 | 287 599 | 1 773.7 | 4 096 | 13 204 / 128 302 |
| Orkut | P2F | 30.94 % | 823 639 | 568 843 | 107 405 | 287 797 | 2 045.4 | 4 096 | 15 224 / 40 746 |
| LiveJournal | G0 | 15.17 % | 729 066 | 618 487 | 186 866 | 490 527 | 59.0 | 4 096 | 1 144 / 40 095 |
| LiveJournal | G1 | 15.03 % | 729 086 | 619 511 | 186 859 | 461 366 | 60.1 | 4 096 | 1 168 / 128 549 |
| LiveJournal | P2F | 15.17 % | 729 066 | 618 487 | 186 866 | 460 417 | 59.0 | 4 096 | 1 144 / 40 086 |
| ogbn | G0 | 0.104 % | 8 380 096 | 8 371 346 | 1 311 915 | 4 995 236 | 23.8 | 4 096 | 279 / 262 532 |
| ogbn | G1 | 0.104 % | 8 380 098 | 8 371 348 | 1 315 896 | 4 535 423 | 21.0 | 4 096 | 248 / 279 855 |
| ogbn | P2F | 0.104 % | 8 380 100 | 8 371 347 | 1 311 975 | 4 466 504 | 23.6 | 4 096 | 277 / 262 329 |

* **Cache.** P2F does not touch the hierarchy: Orkut and LiveJournal L2 hit rates and read
  counts are identical to G0 (30.94 % / 15.17 %, 823 643 / 729 066 reads), and ogbn differs
  by 4 reads. G1's +0.72 pp L2 hit-rate change on Orkut — the source of G6G's G2 win — is a
  side effect of its different release order coalescing L2 fills, and it costs more DRAM
  hits than it saves (row hits −9 685, reads/act −2.3 %).
* **Translation.** P2F leaves the dTLB/page stream alone: identical `walks` on Orkut (107 405)
  and LiveJournal (186 866), +60 on ogbn. G1 perturbs it (+3 981 walks on ogbn) because a
  different release sequence back-pressures admission at different moments.
* **Admission and residency.** `g6b_pa_capacity = 4096`, `l2_mshr_max = 64`,
  `dram_max_q ≤ 63`, `dram_q_full = 0`, `dram_missed_hit = 0`, `bulk_residency_violations = 0`
  in all nine cells: no limit was raised, and no run left the buffer unissued
  (`requests_unissued_end = 0`).

## 8. Correctness and resource validation

| Check | Result |
|---|---|
| Test suite | `./bin/dx100_tests` → **4 767 checks, 0 failures** |
| Exact G6G G0 reproduction | rerun on the G6H binary vs frozen `build/wm/m23g6g/g6g_G0_<wl>.txt`: **728 keys, 0 differ**, three workloads |
| Exact G6G G1 reproduction | rerun vs frozen `g6g_G1_<wl>.txt`: **728 keys, 0 differ**, three workloads |
| G3 P2 anchor reproduction | `build/wm/m23g3/g3_orkut_w16384_p2.txt` vs rerun: **584 keys present in both, 0 differ**; cycles 6 076 339 both; the 176 extra keys are the counter families added after G3 |
| Request conservation | `bulk_tgt_req_gen == issued == done` and `cc3_issue == cc3_done` in all nine cells |
| Element conservation | `bulk_elems_done == bulk_elems_total > 0` in all nine cells; `conservation_ok = 1`, `conservation_violations = 0` |
| No duplicate issue / completion | `cc3_issue == cc3_done`; `bulk_g6g_sel_mismatch = 0`; the released run's own `(bank,row)` is re-derived and compared to the chosen cell on every release |
| No lost requests | `requests_unissued_end = 0`; `bulk_addr_mismatch = 0` |
| No deadlock | all nine runs terminated well inside `max_cycles`; `pick_group` never waits for an untranslated line and always returns a pending entry |
| Resource limits | `l2_mshr_max = 64`, `dram_max_q ≤ 63`, `dram_q_full = 0`, `dram_missed_hit = 0`, `bulk_residency_violations = 0`, `ready_peak ≤ 4096` |
| Visibility invariant | `bulk_g6h_inv_viol = 0` and `exam_sum == elig_sum` with `stale_sum = 0` in all six instrumented cells (`tools/m23g6h_check.py` → 9 cells, ALL PASS) |
| Determinism | P2F/Orkut repeated: byte-identical log |
| Historical artifacts untouched | only `build/wm/m23g6h/` was written; `build/wm/m23g3/`, `m23g6e/`, `m23g6f/`, `m23g6g/` read only |

Memory-ordering: the reordered set is one descriptor's independent target loads, each retired
by its own fill; P2F applies an order the shipped P2/G6A already apply.

## 9. Hardware feasibility and limitations

* **What the organizer costs as measured.** P2F's complete enumeration reads 528 631 108
  buffer entries over 6 130 439 cycles on Orkut (86.2 entries/cycle), 287 797 decisions,
  1 836.8 entries each — one decision every 21.3 cycles. LiveJournal: 18 110 293 entries
  (1.3/cycle) over 460 417 decisions. ogbn: 70 484 531 (0.7/cycle). The simulator charges
  **no cycles** for this pass (`bulk_org_cycles` covers the seal only), so the cycle counts
  above are a bound on what the ordering can achieve, not a feasible implementation's time.
* **What a realisable version needs.** Not a 4 096-entry sort — the buffer is already sorted
  by cell at seal time. P2F needs the *minimum cell key among live entries*, which an
  occupancy table answers directly: 8 192 cells × `{count:5, minrank:13}` ≈ 3 B = 24 KiB
  SRAM, updated in O(1) on push/pop, plus a min-heap over the ≤ 2 070 live cells
  (12-level, ~8 k 13-bit comparators shared across banks) or a 13-bit tournament tree.
  The scan measured here is the simulator's stand-in for that structure.
* **P2 (G3) asked for the same structure and for less.** G3's P2 needs the *same* min-key
  selection; G1's densest-first needs a max-reduction, which is the wide-reduction problem
  G6F identified and rejected. So the correct P2 arm is also the cheaper one to build.
* **Pipelining** is possible — the choice only has to be ready when a residency slot frees —
  but it buys nothing: the measured payoff of the unconstrained, full-visibility version is
  −0.001 % / +0.000 % / +0.005 % against the control.
* **No synthesis was run.** No area, frequency, power or timing number in this document is
  backed by one; 24 KiB / 12-level are arithmetic on the configuration, not implementation
  results.

## 10. Limitations

1. **Three workloads, one dispatch point, one window.** Orkut S1, LiveJournal and ogbn train
   at their G6D translation winners; 16 384-element windows, one 4 096-entry ready buffer,
   2 banks × 4 096 rows × 1 KiB. A deeper buffer would raise per-cell occupancy (2.84 here),
   which is the one lever this audit did not sweep, per the brief.
2. **The buffer is full only in bursts.** `exam_max = 4096` proves capacity is reachable, but
   the *mean* eligible population is 1 837 / 39 / 16 runs: on LiveJournal and ogbn the
   organizer usually sees a handful of entries, so "complete window" is a bound, not the
   typical case.
3. **`untr` conflates in-flight and not-yet-admitted runs.** It is
   `fmeta.size() - g6b_tr_done`; splitting it would need a per-run probe-state counter that
   would perturb the admission path, which the brief forbids.
4. **The organizer's scan is uncharged.** All three arms share that convention with G3/G6A/
   G6G, so the comparisons are internally consistent, but no cycle count here includes the
   cost of finding the group.
5. **G3's end-to-end P2 loss is not re-derived.** This document quotes G3's ideal-translation
   cells and reproduces one of them exactly; it does not re-run G3's real-translation P2
   grid, which was measured against a different baseline.

## 11. Conclusion and recommendation

* **The 4K window is fully visible.** `pick_group` enumerates it, no candidate limit exists
  on that path, the examined population reaches 4 096, and the visibility invariant holds on
  1.05 × 10⁷ decisions with zero excluded entries.
* **G6G's gap was the selection rule.** G1/G2 replaced P2's ascending `(row,bank)` key with
  densest-first and bank rotation; P2F restores the key and returns the machine to the
  control's DRAM behaviour, which is the faithful answer to "does the original P2 work over
  the complete eligible population".
* **It works, and it is worth nothing here.** Row hits are unchanged on Orkut (exactly), and
  ≤ +0.05 % of reads per activation at best. The reason is structural: the release order
  already is P2's order, the buffer's cells hold 1.4–2.8 runs, and the DRAM's own 60-deep
  hit-first queue extracts the row locality that exists. G3's ideal-translation anchor says
  the same with translation removed (−0.02 % on Orkut).
* **Do not adopt P2F as a policy change.** Keep `sched_group = 3` as the reference arm that
  proves the claim; the next lever is the DRAM controller (`case_b`/`case_c` conflicts) or
  the buffer depth, not the release order.

## Reproduce

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests          # 4767 checks, 0 failures
cd .. && ./build/wm/m23g6h_run.sh orkut                    # 3 cells, ~4 min
cd .. && ./build/wm/m23g6h_run.sh livejournal              # 3 cells, ~1 min
cd .. && ./build/wm/m23g6h_run.sh g3anchor                 # G3 P2, ideal translation, ~1 min
cd .. && ./build/wm/m23g6h_run.sh ogbntrain                # 3 cells, ~12 min
cd .. && python3 tools/m23g6h_extract.py                   # tables, summary.csv, reproduction diffs
cd .. && python3 tools/m23g6h_check.py                     # 9 cells, ALL PASS
```

Per-cell command (Orkut S1, arm 3):

```bash
cd DX100_MEM/sim && ./bin/dx100sim --config configs/m23g6b_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 --set bulk_org=8 \
  --set walkers=8 --set page_cohorts=8 --set dtlb_mshr=8 \
  --set walk_latency_per_level=150 --set sched_vis=0 --set sched_group=3 --all
```
