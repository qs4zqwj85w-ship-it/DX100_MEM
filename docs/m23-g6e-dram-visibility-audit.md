# M23-G6E — DRAM scheduling visibility audit

**Question.** M23-G6D (8 walkers / 8 cohorts / 8 dTLB MSHRs) raised translation
throughput 2.69× and made the ready buffer 22× deeper (93 → 2045 runs) with
identical DRAM service time (18.74 cycles/read), yet Orkut's DRAM row-hit rate
moved 60.49 % → 60.53 %. Where in the path from translation completion to
command issue is the scheduling visibility lost, and why does a deeper buffer not
buy row locality?

**Answer in one paragraph.** The DRAM controller never sees the ready buffer at
all: it sees at most `l2_mshr_max` = 63 requests, i.e. 3.0 % of the 2045
translated runs at S1, and 83.3 % of cycles it receives nothing new because the
release port is refused by the residency budget. Inside the queue there is no
visibility defect — FR-FCFS scans the whole deque every cycle and
`dram_missed_hit` is **0** in all four cells, so a selectable row hit was never
passed over. What reaches the controller is already a *sparse* sample of each
row: the release order is the DRAM-optimal `(row, bank, line)` order, but the
release stream changes cell 0.353 times per run at S1 (2.83 runs per cell) and
the DRAM realises 0.395 activations per read — 2.53 reads per activation, the
*same* 2.53 as A0. G6D changed the rate of the stream, not its shape, so the hit
rate (which is exactly `1 − activations/reads`) did not move.

Nothing here implements an optimisation: two diagnostic counters were added, and
all four cells reproduce their G6C/G6D logs byte for byte.

---

## 1. Source-level dataflow

Cycle-level path, with the exact sites touched.

| # | Stage | Where |
|---|---|---|
| 1 | Walk completes, translation handed to the engine | `MemorySystem::take_xlate` (`memory.cpp`), drained in `bulk.cpp:1913-1921` |
| 2 | **Ready-buffer insertion** | `bulk.cpp:1920` — `b.ready.push(b.fpos[li])` |
| 3 | Buffer organization | `sim.hpp:1407` — `std::priority_queue<u64, vector<u64>, greater<u64>> ready` (a **min-heap on the run's position in the window's P8 order**) plus `sim.hpp:1409 fpos` (inverse permutation, `bulk.cpp:1903-1904`) |
| 4 | **Release** (`bulk_org=8`) | `bulk.cpp:1969-1995` — budget `max(1, eng_issue)` = 4/cycle; `top()` → `run_at` → `win_emit_fused` → `pop` |
| 5 | Residency gate | `bulk.cpp:1611 win_emit_fused` returns **1** when `free_count() <= reserve` (`tgt_reserve()` = 8 of R = 128 slots ⇒ ≤ 120 outstanding target requests); the loop `break`s and the run stays parked (`bulk.cpp:1984-1988`) |
| 6 | L1 → L2 | `bulk.cpp:791 mem_.tick` → `memory.cpp` L1 MSHR → L2 with **64 MSHRs** (`l2_m_.assign(cfg.l2_mshr, ...)`, `memory.cpp:51`) |
| 7 | **DRAM enqueue** | miss allocated an L2 MSHR → `dram_wait_.push_back` (unbounded; `dram_q_full = 0` in every cell) |
| 8 | **Candidate selection** | `memory.cpp:572 dram_tick` — a full linear scan of `dram_wait_` from index 0, FR-FCFS (row hit first, then oldest), no cursor, no FIFO head, no limited window |
| 9 | Issue / timing | `bank_ready_[bank] = cycle + (open ? t_hit : t_miss) + link` |

**Order of the release stream.** For `org == 8` the window is sorted by
`(row, bank, line)` (`bulk.cpp:1583-1586`, the `else` branch: `row[x] < row[y]`,
then `bank`, then sealed order = ascending line). The release therefore pops the
globally smallest `(row, bank, line)` among *translated* runs. This is already
the DRAM-optimal order; the audit's finding is that the order is not the problem.

**The one behaviour that matters at step 4.** Because the key is a global
minimum, a line of a row whose visit has already passed gets a *key smaller than
the frontier*: it jumps to the head and is popped immediately, alone. A row is
visited as many times as its lines arrive late, which is why the observed runs
per cell (2.83 at S1) is far below the 16 lines a row holds.

---

## 2. Ready-buffer occupancy vs. scheduler visibility

Two different populations, never to be conflated:

* **Ready-buffer occupancy** (2045 avg at S1): translated, not yet issued, one
  entry per run, resident in the min-heap. No DRAM-side counter can see it.
  `bulk_g6b_ready_occ_sum / cycles`.
* **DRAM scheduler visibility** (`dram_q_occ_sum / dram_diag_decisions`, 60.81 at
  S1): requests that passed steps 4-7 and are in `dram_wait_` when `dram_tick`
  arbitrates. Only these can be chosen.

Counters, defined exactly (all new ones are written only when `dram_diag` is on,
and **none is read by any decision**):

| Counter | Definition |
|---|---|
| `dram_diag_decisions` | cycles in which `dram_wait_` was non-empty at `dram_tick` |
| `dram_elig_sum` | Σ over decisions of the number of queued requests whose bank is free (`!bank_open_ \|\| bank_ready_ <= cycle`) |
| `dram_vis_rows_sum` | Σ over decisions of the number of **distinct `(bank,row)` cells** in the queue (per-cycle generation stamp array `row_stamp_`, allocated only for diagnostics) |
| `dram_vis_banks_sum` | Σ over decisions of the number of distinct **banks** represented |
| `dram_vis_rows_max` | largest single-decision distinct-cell count |
| `dram_hit_cand_h{k}` | decisions whose queue held exactly `k` row-hit candidates (last bucket = ≥16); Σ = `dram_diag_decisions` |
| `bulk_g6b_release_pops` | runs handed to `win_emit_fused` and accepted (accepted = `rc == 0`) |
| `bulk_g6b_release_blocked_cycles` | cycles with a non-empty ready buffer in which **no** run was released |
| `bulk_g6b_release_slot_stall` | cycles in which the release stopped on `win_emit_fused == 1`, i.e. the residency budget |

Deliberately not used: "available requests". Two requests to the same open row
are one *cell* and one issue opportunity; `dram_elig_sum` (bank free) and
`dram_vis_rows_sum` (cells) answer different questions and are reported
separately.

`dram_blocked_cycles` is **not** a wasted-cycle or missed-opportunity metric: it
counts cycles in which every queued request's bank is still busy servicing an
earlier command, i.e. a legitimately occupied bank.

---

## 3. Measurements

`build/wm/m23g6e/g6e_{A0,S1,DLJ,DOGBN}.txt`, `summary.csv`,
`python3 tools/m23g6e_extract.py`. A0 = G6C baseline 2/2/4; S1 = G6D winner
8/8/8 (Orkut); DLJ and DOGBN = 4/8/8 on LiveJournal and ogbn-products;
`bulk_org=8`, L=150, window 16 384 throughout.

```
cell   cycles      reads    hit%  dec%  qMean elig cells banks hitC e/cell hitSh%  pops/cyc blocked%  ready  resid  vis%  wMed  w95  tgt%  idx%
A0      16505468   651193  60.49  34.7  22.96 1.47  5.38  1.65  4.65  4.27   20.3   0.050     6.1     93   1870  24.6   127  1023  56.5  93.6
S1       6130494   644142  60.53  99.2  60.81 3.29 27.00  1.97  4.26  2.25    7.0   0.134    83.3   2045  15224   3.0  1023  2047  56.2  93.4
DLJ     14146860   697141  46.14  52.1  18.56 1.07  7.45  1.71  2.81  2.49   15.1   0.052     4.2     59   1144  31.5   127  1023  40.6  93.7
DOGBN   98563897  9826463  65.04  92.1  22.70 1.30  7.74  1.90  3.01  2.93   13.3   0.085    12.3     24    279  95.6   255  1023  67.0  93.8
```

`dec%` = share of cycles with a non-empty DRAM queue; `qMean` = queue occupancy
at a decision; `elig` = bank-free requests; `cells`/`banks` = distinct cells and
banks in the queue; `hitC` = row-hit candidates; `e/cell` = `qMean / cells`;
`hitSh%` = hit candidates / queue occupancy; `blocked%` = release-port blocked
cycles; `resid` = `ready / (pops/cycle)`; `vis%` = `qMean / ready`;
`wMed`/`w95` = per-request DRAM wait, log2-bucket upper edge; `tgt%`/`idx%` =
row-hit rate of the target (client 3) and index (client 1) streams.

### A. Ready-buffer visibility — the buffer is invisible, and the port is the gate

At S1 the controller sees 60.81 of 2045.3 runs = **2.97 %**. The release port is
refused by the residency budget in **83.3 %** of cycles (5.11 M cycles released
nothing while 2045 runs waited; `g6b_release_slot_stall` fired 5.88 M times), and
the pop rate is 0.134/cycle against a 4/cycle budget — the budget is never the
limit. Little's law gives a run a **15 224-cycle** residence in the buffer at S1
vs 1 870 at A0.

There is no FIFO, cursor, or limited scan inside the buffer: `b.ready.top()` is
the global minimum, so the released prefix is always the smallest remaining
`(row, bank, line)` key, but only among runs translated so far.

### B. DRAM queue visibility — 63 is a cache cap, not a DRAM cap

`dram_max_q = 63` and `l2_mshr_max = 64` in every cell; `dram_q_full = 0`
everywhere (the deque never refuses an arrival). The visible population is capped
by the 64 L2 MSHRs, and in practice by the residency pool (R − reserve = 120
outstanding) that must be entered *before* the L2 MSHR is allocated. Per-request
queueing latency at S1: median bucket 512-1023, p95 bucket 1024-2047, max 1239
(A0: median 64-127, p95 512-1023). The queue is at 96 % of its cap on average
(60.81 / 63) — it is full, not merely deep.

### C. Candidate diversity — diversity up 5×, density down 2×

| | A0 | S1 | change |
|---|---|---|---|
| distinct cells per decision | 5.38 | 27.00 | ×5.0 |
| distinct banks per decision | 1.65 | 1.97 | ×1.2 |
| requests per cell (`e/cell`) | 4.27 | 2.25 | ×0.53 |
| row-hit candidates per decision | 4.65 | 4.26 | ×0.92 |
| hit share of queue | 20.3 % | 7.0 % | ×0.35 |

This is the core result. Deepening the buffer by 22× raises the *number of rows*
the controller can see by 5× and *halves* the number of requests it has of each
one. Hit candidates per decision are flat, because at most two rows can be open
and each open row now offers ~2.2 queued requests instead of ~4.3. The
distribution shows the same thing: at S1 the queue holds ≥ 16 distinct cells in
  75 % of decisions (2-3 cells in 9.4 %), while at A0 it holds ≤ 7 cells in
  91.8 % of decisions and ≥ 16 in only 2.6 %.

The reason density *falls* as the buffer deepens is the late-arrival mechanism in
§1: the frontier at S1 is 15 224 cycles ahead of translation completion (1 870 at
A0), so many lines of an already-passed row arrive late, get a smaller key, and
are popped as singletons ahead of the frontier — spending a cell visit on one
line. Measured at the release port itself: `bulk_sched_opens / release_pops` =
0.525 at A0 (1.90 runs per cell visit) but only 1.0/2.83 = 0.353 at S1.

### D. Scheduling effectiveness — no missed opportunity, but 48.5 % unavoidable policy cost

`dram_missed_hit = 0` in all four cells: FR-FCFS never left a selectable row hit
unissued. Conflict decomposition (`case_b` = a hit was queued but its bank was
busy; `case_c` = no hit existed in the queue at all):

| cell | conflicts | case_b | case_c | reads/activation | 1 − acts/reads |
|---|---|---|---|---|---|
| A0 | 257 280 | 35.2 % | 64.8 % | 2.531 | 60.49 % |
| S1 | 254 247 | 48.5 % | 51.5 % | 2.534 | 60.53 % |
| DLJ | 375 450 | 29.0 % | 71.0 % | 1.857 | 46.14 % |
| DOGBN | 3 435 644 | 61.4 % | 38.6 % | 2.860 | 65.04 % |

The hit rate is an identity, `1 − activations/reads` (`dram_row_acts_sum`), and
**reads per activation is 2.53 in both A0 and S1** — a 2.7× throughput change and
a 22× buffer change leave it untouched. Activation rate must therefore be set
upstream of the controller. Consistent with this, the *index* stream (client 1),
whose locality G6D never touched, hits **93.4-93.8 %** in every cell: the
controller and FR-FCFS demonstrably deliver 94 % when the arriving stream is
dense, so the policy is not the defect.

### E. Coverage and saturation

Orkut (A0, S1), LiveJournal (DLJ), ogbn-products train (DOGBN). Orkut S1 is at
**98.1 % / 98.7 % bank occupancy** (bank-busy-blocked 88.7 % of cycles, issuing
10.5 %, starved 0.8 %) and 0.105 reads/cycle against a structural peak of
2/18.74 = 0.107. The DRAM is saturated; only *service time per read* remains.

Target-stream detail (client 3 only): A0 323 688/573 381 hits = 56.45 %;
S1 319 817/568 820 = 56.22 %; DLJ 251 359/618 487 = 40.64 %; DOGBN
5 608 684/8 371 341 = 67.00 %. The aggregate 60.5 % is a blend with a 5-12 %
index stream (11.6 % of Orkut S1 reads) that already hits 94 %.

The target stream is also a *sample* of each row: 30.4 % of Orkut target lines
(A0) and 30.9 % (S1) are L2 hits and never reach DRAM; LiveJournal 15.2 %;
ogbn 0.1 %.

---

## 4. Identified bottleneck

Supported by the measurements above:

1. **Visibility into translated work is the release/residency boundary, not the
   DRAM queue.** 2.97 % of the buffer is visible; 83.3 % of cycles release
   nothing; the visible population is capped at 63 by the 64 L2 MSHRs, and
   `dram_q_full = 0` proves the DRAM queue itself is never the limiter.
2. **The controller has no visibility defect.** Full scan every cycle,
   `missed_hit = 0`, and 94 % hits on the dense index stream.
3. **The row-hit rate is set by the density of the arriving stream, and G6D did
   not change it.** Reads per activation is 2.53 in both cells; the release
   stream visits 2.83 runs per cell at S1 (1.90 at A0) and the DRAM realises
   2.53, i.e. it already extracts ~90 % of the cell density offered at S1.
4. **Therefore the row-hit rate could not have improved**: opportunities per
   decision (4.65 → 4.26) and activations per read (0.395 → 0.395) are invariant
   under the G6D changes, which is exactly what 60.49 % → 60.53 % reports.

Not supported / not claimed: that row hits are irrelevant (they are not — a miss
costs 10.26 vs 8.47 cycles at S1), that a deeper queue would help (it is already
at 96 % of its cap), or that the release policy is optimal (it is measured, not
judged).

**Ceiling.** With Orkut S1 at 98.1-98.7 % bank occupancy, converting *every* conflict
to a hit saves 254 247 × (10.26 − 8.47) ≈ 455 k cycles = **7.4 % of the 6.13 M
wall**. Of that, ≤ 3.6 % needs only the 123 222 `case_b` events (a hit was
already visible in the busy bank), and ≤ 3.8 % requires a denser arriving stream
(131 027 `case_c` events). For comparison, G6D already took 62.9 %.

---

## 5. Remaining uncertainties

* **Why density falls with depth** is inferred from the late-arrival mechanism
  plus `sched_opens/pops`; no counter attributes individual pops to "frontier
  advance" vs "late-arrival jump". A per-pop classification would settle it.
* **`dram_svc_*` intervals are not clean latencies** under saturation: DOGBN shows
  9.11 hit / 9.09 miss, i.e. the interval is set by bank recirculation, not by
  `t_hit`/`t_miss`.
* **The 7.4 % ceiling assumes the conflict mix is held fixed.** Removing
  conflicts changes the bank-occupancy balance and the release timing, so the
  realized gain from any change must be measured, not derived.
* Only `bulk_org=8`, L=150, the four cells above. C1 (L=75) and the B1/B2 cells
  were not re-run; their G6D cycles are unchanged by this instrumentation, but
  their visibility counters are not measured.
* Per-cycle histograms (`dram_q_h*`, `dram_vis_rows_h*`) count ticks, and bucket
  0 covers {0, 1} (`log2_bucket`), not "empty" alone; per-decision means use
  `dram_diag_decisions` and are exact.
* ogbn shoulders the usual G6D caveat: the window seal is deadline-driven, so
  windows are 513 vs 514 baseline and runs vary ≤ 0.03 %. Cycles here match the
  stored logs exactly.

---

## 6. Recommended next experiment

**M23-G7A — row-completion release (measure only, one knob).** Do not touch
FR-FCFS, the DRAM timing, or the release *order*; change only the *admission* of
the release: pop the min-key run only when either (a) its cell's translated
members form a run of ≥ k, or (b) the head has waited > W cycles (deadline, as
the window seal already does). Predicted from measurements: this raises
runs-per-cell-visit from 2.83 toward the row's translated-line count and attacks
the 51.5 % `case_c` conflicts, whose ceiling is 3.8 % of wall at S1 — but it also
delays issue, so the net must be measured, with `bulk_sched_opens/pops`,
`dram_row_acts_sum`, `dram_stall_case_b/c` as the deciding counters.

Cheaper alternative with a *known* 3.6 % bound: **conflict deferral** — when a
row hit is queued in a busy bank (`case_b`), do not issue a conflict to the other
bank if the hit's bank frees within the conflict's extra cost. That exploits
locality already visible (48.5 % of S1 conflicts) and needs no change upstream.

Neither is implemented here; both are policy changes outside this audit's scope.

---

## Instrumentation and reproduction

Added (3 files, diagnostic only, never read by a decision):
`sim.hpp` — `DramStats::vis_rows_sum/vis_rows_max/vis_banks_sum/vis_rows_hist[16]/hit_cand_hist[17]`,
`MemorySystem::row_stamp_/stamp_`, `BulkStats::g6b_rel_pops/g6b_rel_blocked/g6b_rel_slot_stall`;
`memory.cpp` — stamp allocation at construction, distinct-cell loop and hit-candidate
histogram in the existing `if (diag)` block of `dram_tick`;
`bulk.cpp` — three release-loop counters;
`system.cpp` — exports `dram_vis_*`, `dram_vis_rows_h0..15`, `dram_hit_cand_h0..16`,
`bulk_g6b_release_*` (36 new keys).

* `make -j4` clean; `./bin/dx100_tests` → **4 767 checks, 0 failures**.
* `./build/wm/m23g6e_run.sh A0 S1 DLJ DOGBN` → `build/wm/m23g6e/`.
* Reproduction: all four cells equal their stored G6D cycles exactly
  (16 505 468 / 6 130 494 / 14 146 860 / 98 563 897) and **all 708 pre-existing
  keys are byte-identical** in every cell (`diff` excluding the 36 new keys → 0
  lines). G6C A0 is therefore reproduced exactly with instrumentation in place.
* Not run: C1, B1/B2, any sweep, any timing or policy change, any physical-GPU
  claim.
