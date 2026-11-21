# M23-G6F — Full-buffer candidate visibility

**Question.** M23-G6E showed that the DRAM controller sees 60.8 of 2045
translated runs (2.97 %) and that 83.3 % of cycles release nothing because the
residency budget is closed. If FR-FCFS could inspect the whole translated
population — 4096 entries, or the 2045 actually resident — would it find more row
hits and raise DRAM locality?

**Answer.** No. The visibility is now really delivered — 2060 candidates compared
per release decision instead of 1, 705 distinct `(bank,row)` cells, 34× the
queued population — and Orkut S1 moves 6 130 494 → 6 128 185 cycles (**−0.038 %**),
row-hit rate 60.5290 % → 60.5288 %, reads per activation 2.5335 → 2.5335. The
reason is structural and measured: the ready buffer holds only cells *ahead* of
the DRAM's open rows, so the entire 2060-entry candidate population contains
**0.011** open-row candidates per decision while the 60.8-entry DRAM queue
contains **4.26**. Per candidate entry the buffer is 13 000× less likely to hit
than the queue. Full-buffer visibility is looking for locality in the wrong
population: the requests that would hit are the ones already released, not the
ones waiting.

This is a simulator result throughout. No physical-GPU claim is made.

---

## 1. Bottleneck and hypothesis

From M23-G6E (`docs/m23-g6e-dram-visibility-audit.md`), at Orkut S1:

* ready buffer 2045 translated runs, DRAM queue 60.81 → **2.97 % visible**;
* release port refused by the residency budget (R−8 = 120 outstanding) in
  **83.3 %** of cycles; pop rate 0.134/cycle against a 4/cycle budget;
* the visible population capped at 63 by the **64 L2 MSHRs** (`dram_max_q = 63`,
  `l2_mshr_max = 64`, `dram_q_full = 0`);
* FR-FCFS itself flawless: full scan, `dram_missed_hit = 0`;
* reads per activation **2.53**, identical to the A0 baseline, i.e. the row-hit
  rate is set upstream of the controller.

**Pipeline semantics, established before changing anything.** Ready-buffer
entries have *not* passed L1 or L2. A run enters `b.ready` at translation
completion, carrying only its physical address (`b.fpa[li]`); it has no cache
line, no L2 MSHR and no DRAM queue entry until `win_emit_fused` submits it
(`r.pretranslated = true`, L1 bypassed, L2 looked up). The ~61-request figure is
not a buffer property at all: it is `l2_mshr = 64` minus whatever the index and
walk streams hold, and the residency pool (R − reserve = 120) gates entry into
it, refusing in 83 % of cycles. So a run may be inspected for free (its PA is
metadata the engine already owns) but *issuing* it always costs one slot and one
MSHR — which is exactly the distinction this experiment has to keep.

Hypothesis (G6F): the controller is starved of *candidates*, not of issue slots.
Let it compare every translated request in the buffer — metadata reads only, no
new in-flight state — and pick the row hits, and `1 − activations/reads` should
rise.

---

## 2. Implementation

Three source changes, all in the release path. The DRAM model, its timing,
FR-FCFS, translation and the cache hierarchy are untouched.

### 2.1 The buffer becomes readable (`sim.hpp`)

`Batch::ready` was a `std::priority_queue<u64, std::vector<u64>, std::greater<u64>>`,
which hides its container, so no code could read the candidate population. It is
now `std::vector<u64>` with the *same* heap operations exposed as
`ready_push` / `ready_pop` / `ready_erase_val` (`std::push_heap` /
`std::pop_heap` / sift-up-or-down). The comparison and the tie-breaking are
unchanged, so the control's pop sequence is bit-identical — proved in §5.

### 2.2 A read-only open-row probe (`sim.hpp`, `memory.cpp`)

```cpp
bool dram_row_open(u64 line, u32& bank, u32& row) const {
  dram_bank_row(line, bank, row);
  return bank < bank_open_.size() && bank_open_[bank] && bank_row_[bank] == row;
}
```

This is the model's own hit test (`memory.cpp:632,663`: `bank_open_[b] &&
bank_row_[b] == row`). It creates no request, allocates nothing and mutates
nothing. Bank *readiness* is deliberately excluded: a released run reaches the
queue ~L cycles later, so the open row — not the instantaneous ready time —
predicts a hit on arrival.

### 2.3 Candidate selection (`bulk.cpp pick_candidate`)

Ranking is the existing FR-FCFS priority applied to candidates instead of to
queued requests:

1. a candidate whose `(bank,row)` is **currently open** outranks one that would
   activate;
2. inside a class, the smallest window position wins — plain FCFS on the P8
   order, which is what the release already did.

The compared set is `min(|ready|, sched_vis)`. When `sched_vis` is below the
buffer size the set is the `sched_vis` **smallest positions** (`nth_element` on a
scratch copy): a bounded reorder window over the P8 order, not an arbitrary
subset of the heap array. The scan also counts, per decision, the candidates
inspected, the distinct `(bank,row)` cells among them (generation-stamp array)
and how many sat on an open row.

### 2.4 The release loop (`bulk.cpp`)

```cpp
if (cfg_.sched_vis && free_count() <= tgt_reserve()) { ++st_.g6b_rel_slot_stall; break; }
const u64 pos = cfg_.sched_vis ? pick_candidate(b) : b.ready.front();
const u32 li  = run_at(b, static_cast<u32>(pos));
const int rc  = win_emit_fused(b, li, cycle);   // unchanged admission path
if (rc != 0) { ...; break; }
ready_erase_val(b, pos);
```

Every property the brief asks to preserve:

| Property | Status |
|---|---|
| L1/L2 lookup and admission | unchanged — the chosen run enters `win_emit_fused`, which submits a `Req` through the hierarchy (`pretranslated`, L1 bypass → L2 lookup, `r.client = 3`) |
| L2 MSHR limit | unchanged, 64; `l2_mshr_max = 64` in every cell |
| Outstanding/residency limit | unchanged, R = 128 with reserve 8; the gate is the first test, before any scan |
| Requests issued twice | impossible — a released run is marked in `fflag` and its stale heap entry is discarded when it surfaces; `cc3_issue == cc3_done` everywhere |
| DRAM command limits, bank timing, row buffers | untouched |
| Translation policy, page walker timing | untouched |
| General FR-FCFS policy in the controller | untouched — the controller still scans its whole queue as before |

**Candidate visibility is not concurrency.** The scan reads metadata for up to
4096 runs and admits **one** request per release slot, through the same gate.
Nothing about in-flight capacity changed; the scan adds no in-flight state.

`Config::sched_vis` (default 0) selects the mode and is echoed in the
configuration block **only when non-zero**, so every historical log's header
stays byte-identical.

---

## 3. Configuration and method

`build/wm/m23g6f_run.sh`, nine runs, frozen G6D/G6E settings throughout:
`configs/m23g6b_base.cfg`, `bulk_org=8`, `batch_window=16384`,
`walk_latency_per_level=150`, `bulk_index_base=0x80000000`,
`bulk_desc_line=0x1FFFFC0`, traces `m23_orkut_bfs.bin` /
`m23_livejournal_bfs.bin` / `m23_ogbn_train_100d.bin` (`bulk_max_elems`
2097152 / 2097152 / 16777216), translation 8/8/8 for Orkut S1 and 4/8/8 for
LiveJournal and ogbn — the G6D winners.

| Cell | `sched_vis` | Maximum candidate visibility |
|---|---:|---|
| F0 | 0 | G6E behaviour: the release reads only the heap minimum (1 candidate) |
| F1 | 1 024 | 1 024-candidate reorder window |
| F2 | 4 096 | full ready buffer |

Per the revised matrix only these three cells were run: 128, 256, 512 and 2048
were **not** run.

---

## 4. Results

`python3 tools/m23g6f_extract.py`; logs and `summary.csv` in `build/wm/m23g6f/`.
`qhitC` = open-row candidates in the DRAM queue per decision, `b_hitC` = the same
count in the candidate set the release compared. `vis` = candidates compared per
decision (F0 compares 1 by construction).

### 4.1 Orkut BFS, S1 (8/8/8)

```
cell vis    cycles    dCyc%   reads   hit%  reads/act  qmean qhitC | b_ent b_cells b_hitC  pick%  dens
F0     1  6130494        --  644142  60.5290   2.5335   60.81 4.264 |     1      --      --      --    --
F1  1024  6129599  -0.0146%  644049  60.5322   2.5337   60.81 4.263 |   893   316.3  0.0037  0.162%  2.82
F2  4096  6128185  -0.0377%  643973  60.5288   2.5335   60.82 4.269 |  2060   704.7  0.0106  0.324%  2.92
```

### 4.2 LiveJournal BFS (4/8/8)

```
cell vis    cycles    dCyc%   reads   hit%  reads/act  qmean qhitC | b_ent b_cells b_hitC  pick%  dens
F0     1 14146860        --  697141  46.1440   1.8568   18.56 2.806 |     1      --      --      --    --
F1  1024 14145557  -0.0092%  697108  46.1488   1.8570   18.60 2.805 |    61    16.0  0.0200  1.214%  3.85
F2  4096 14145557  -0.0092%  697108  46.1488   1.8570   18.60 2.805 |   143    28.3  0.0200  1.214%  5.06
```

LiveJournal's F1 and F2 are **behaviourally identical** — same cycles, same picks
(`pick_hit = 8 849`, `pick_frontier = 720 232`), same `b_hitC` — while F2
compared 2.3× more candidates (143 vs 61 per decision). The buffer exceeds 1 024
only in brief bursts, and the extra candidates found exactly **zero** additional
open-row candidates. LiveJournal also shows the highest pick rate of the three
workloads (1.21 %), and the smallest effect anyway.

### 4.3 ogbn-products train (4/8/8)

```
cell vis    cycles    dCyc%   reads   hit%  reads/act  qmean qhitC | b_ent b_cells b_hitC  pick%  dens  acts
F0     1 98563897        --  9826463  65.0368   2.8602   22.70 3.009 |     1      --      --      --    --  3435646
F1  1024 98578564  +0.0149%  9826597  65.0328   2.8598   22.70 3.009 |    22     8.4  0.0195  1.647%  2.60  3436083
F2  4096 98578550  +0.0149%  9826597  65.0328   2.8598   22.70 3.009 |    24     9.0  0.0195  1.647%  2.66  3436081
```

ogbn's ready buffer averages 24 runs; it only spikes past 1 024 in short bursts,
which is why F1 and F2 differ at all. Out of 8 380 106 release decisions they
differ in 14 (`pick_hit` 137 982 vs 137 996), which is exactly the 14-cycle gap
in a 98.6 M-cycle run. There is essentially no visibility to add on this
workload.

ogbn is also the one cell that got *slower* (+0.0149 %), with activations rising
3 435 646 → 3 436 083 even though 1.65 % of picks were open-row candidates. That
is the second mechanism at work: a candidate that is a hit *at release time*
arrives at the DRAM ~L cycles later, by which point the row may have closed, so
the request lands as a conflict — and it displaced the frontier run that would
have been served in order. Release-side hit-chasing optimistically reorders on
stale bank state.

Nothing else about the memory system moved either. DRAM service per read is
identical in all three Orkut cells (8.47 cycles on a hit, 10.26 on a conflict —
`svcH`/`svcM`), issue rate stays 0.105 reads/cycle, `dram_max_q` 63,
`l2_mshr_max` 64, residency stalls 5 883 207 → 5 882 622, and starved/blocked
fractions are unchanged at 0.8 % / 88.7 %. The only counters that move
materially are the new `bulk_g6f_*` ones and the read count itself.

**Net effect of full visibility: −0.038 % (Orkut), −0.009 % (LiveJournal),
+0.015 % (ogbn).** Against the 62.9 % G6D gained on Orkut, and against G6E's
computed 7.4 % ceiling for a row-hit-perfect controller, F2 captures about
**0.5 % of the available prize**, and nothing at all on the other two workloads.

---

## 5. Why nothing changed — the two populations are disjoint

### 5.1 The visibility is real

At Orkut S1 F2 compares 2060 candidates per decision — 705 distinct `(bank,row)`
cells, `vis_cells_max = 2074` — against F0's single heap minimum. Nothing else in
the run moved: `qmean` 60.81 → 60.82, `dram_max_q` 63, `l2_mshr_max` 64,
`bulk_g6b_release_slot_stall` 5 883 207 → 5 882 622 (the gate still closes in
83 % of cycles), pop rate 0.134/cycle.

### 5.2 The buffer does not contain the row that is open

| search space | size | open-row candidates | per entry |
|---|---:|---:|---:|
| DRAM queue (what FR-FCFS chooses from) | 60.8 | **4.26** | 7.0 % |
| ready buffer (what the new scan chose from) | 2 060 | **0.011** | 0.0005 % |

A 34× larger population contains 400× fewer hits; per candidate entry the buffer
is ~13 000× less likely to be a row hit than the queue. Only **0.32 %** of
release decisions in F2 found an open-row candidate at all, and picking one
changed nothing measurable (6 130 494 → 6 128 185 cycles).

**The mechanism.** The release frontier is the smallest key in the buffer, and
`dram_row_open` compares against the row the bank is *currently* serving —
opened by a request released ~450 cycles (the queue's residence) earlier. A
cell's lines are therefore popped, queued and served in order, and by the time a
row becomes the DRAM's open row its remaining lines have already left the buffer.
The buffer is strictly a lookahead of the frontier; the open row is strictly
behind it. Full-buffer visibility compares future cells with a past row.

The uniformity test quantifies it: the buffer spans 704.7 of the 8 185 mapped
rows (8.6 %), so if open rows were distributed at random, 2 banks × 8.6 % × 2.92
lines ≈ 0.5 open-row candidates per scan would be expected. Observed 0.0106 — 47×
*below* uniform. The buffer is not a random sample of the address space with
respect to the open rows; it is anti-correlated with them.

### 5.3 What the buffer does have is not enough either

The buffer's own row density is 2.92 requests per cell at Orkut (5.06 at
LiveJournal, with 1.21 % of picks being hits — four times Orkut's rate) against
the queue's 2.25. Both are far from the 16 lines a row holds, and only ~69 % of
Orkut target lines reach DRAM at all (30.9 % are L2 hits). Making a *deeper*
window dense is not the same as making the *served* window dense.

### 5.4 Where the amount came from

`−0.0377 %` is consistent with G6E's identity: reads per activation stayed
2.5335, the hit rate stayed 60.53 %, and only the read count moved by 169
(−0.026 %, fewer L2 misses because a slightly different release order changed
which lines coalesced in L2: `cc3_l2_hits` 254 823 → 254 876). The DRAM did not
become more local; the workload did fractionally less work.

---

## 6. Resource accounting and correctness

All nine cells, checked against the control of the same workload:

| Check | Result |
|---|---|
| Control reproduces G6E exactly | **yes** — F0/orkut 6 130 494, F0/livejournal 14 146 860, F0/ogbntrain 98 563 897, and all 708 pre-existing keys byte-identical in each (`diff` excluding the new keys → 0 lines) |
| Request conservation | `bulk_g6b_tr_requests == tr_completed == runs_released`; `cc3_issue == cc3_done` in every cell (Orkut F2: 823 526 == 823 526 == 823 526; 823 527 issue == 823 527 done) |
| No request issued or completed twice | `cc3_issue == cc3_done`; every release marks `fflag` and its stale heap entry is discarded when it surfaces |
| No requests lost | windows unchanged (Orkut 74), `dram_reads = hits + conflicts` with the same +2 in-flight offset as the control |
| No deadlock / max-cycle termination | all nine runs completed normally; `starved + blocked + issuing` still partitions the cycles |
| MSHR and outstanding limits respected | `l2_mshr_max = 64`, `dram_max_q ≤ 63`, `dram_q_full = 0`, residency gate unchanged (`slot_stall` 5 883 207 → 5 882 622) |
| Determinism | F2/Orkut repeated → **byte-identical** log; F1 vs F2 on LiveJournal identical except the cap counters |
| Existing artifacts untouched | G6E/G6D logs only read; new cells live in `build/wm/m23g6f/` |
| Test suite | `./bin/dx100_tests` → **4 767 checks, 0 failures** |

One accounting note: the `ready` occupancy metric (`bulk_g6b_ready_occ_sum`)
includes entries already released but not yet surfaced to the top of the heap
only transiently — the implementation erases by key, so `|ready|` remains the
exact count of pending runs (Orkut F2 12 530 790 945 vs control 12 538 861 068,
−0.06 % because 116 fewer runs were released).

---

## 7. Hardware feasibility

* **Naive full scan.** A 4096-entry compare per cycle is a 4096:1 min-reduction:
  12 levels of 16-bit magnitude compare-and-select, plus a 12-bit `(bank,row)`
  equality against two open-row tags, plus a 4096:1 priority encode back to the
  release slot. The logic depth (~12 × 7 FO4 ≈ 85 FO4 for the key reduction) is
  borderline for a 1 GHz control path, but the routing is not: ~4096 × (16-bit
  key + 12-bit cell) ≈ 115 k wires converge per bank, and the switching cost
  (~4096 tag compares every cycle ≈ hundreds of pJ at 1 GHz) would exceed the
  entire DRAM controller's budget. As a *simulator upper bound* it is exactly
  what the F2 cell measures. **No synthesis was run; no area, timing or power
  number here is backed by one.**
* **The scan is also unnecessary.** The buffer is *already* keyed by
  `(row, bank, line)`, so every candidate of a given row occupies a contiguous
  key range. A per-`(bank,row)` occupancy counter plus a head pointer answers
  "how many candidates does this row have, and where is the first" in O(1). Two
  banks ⇒ 3 candidates to arbitrate (the two open rows and the frontier row), not
  4096. That is the practical form of "full visibility": a small indexed table,
  not a wide comparator tree.
* **But it would buy nothing.** The measured upper bound for any visibility
  scheme is −0.038 %. A free implementation is not worth an indexed table for
  0.04 %; the experiment's value is that it *rules out* the hypothesis.

---

## 8. Recommendation

**Do not adopt full-buffer visibility, in any of the three forms.** The
candidate population behind the frontier is structurally the wrong population to
search for row hits.

The one dimension this experiment did **not** test — and the only one the
measurements still leave open — is *making the served window dense* rather than
making the looked-at window wider. Concretely, **M23-G7A (row-completion
release)**: hold the frontier until the head cell has `k` translated lines
(`k` ≈ the queue's 60 slots / one cell) or a deadline expires, then release that
cell's lines as a burst. That attacks the measured 51.5 % `case_c` conflicts
("no hit queued at all") and needs the per-`(bank,row)` counter from §7 to be
O(1). G6E bounded its payoff at 7.4 % of Orkut S1 wall; the deciding counters are
`bulk_sched_opens/pops`, `dram_row_acts_sum`, `dram_stall_case_b/c` and total
cycles.

A cheaper alternative remains available and untested: **conflict deferral** — when
a hit is queued in a busy bank (`case_b`, 48.5 % of S1 conflicts), do not issue a
conflict to the other bank if the hit's bank frees within the conflict's extra
cost. Bound: 3.6 % of wall. It exploits locality already visible and needs no
change upstream.

Neither is implemented here: both change release/issue policy, and this brief's
job was to establish what visibility alone achieves.

---

## Instrumentation added

`Config::sched_vis` (util.cpp parse + conditional echo); `Batch::ready` as a
vector min-heap with `ready_push`/`ready_pop`/`ready_erase_val`;
`MemorySystem::dram_row_open`; `BulkEngine::pick_candidate` with generation-stamp
cell counting; `BulkStats::g6f_scan_calls / scan_entries / vis_cells_sum /
vis_cells_max / vis_hit_sum / pick_hit / pick_frontier`, exported as
`bulk_g6f_*`. All are diagnostic or release-selection state; none is read by the
DRAM model, the cache hierarchy or any timing decision.
