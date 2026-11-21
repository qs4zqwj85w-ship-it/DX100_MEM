# M12 — Bulk window capacity and double-buffer scaling

**Question.** Would two 16 K-entry buffers (rather than two 8 K-entry buffers) provide meaningful
additional performance, and does the benefit continue at 32 K?

**Answer, in one paragraph.** No, not as a capacity argument. Doubling the capacity of each buffer
does not make the organizer better at organizing: within one window the issue order is already at
its activation floor, and the number of outstanding target requests is capped at **120 =
`R − idx_reserve`**, a value reached in every W1 cell and never exceeded in any of the 90 cells
(§1.2, §4). What a larger `batch_window` actually does is
change the *number* of windows (`ceil(N/W)`) and, once a window is larger than the line set's
reuse distance, let deduplication fold repeated lines. Both are step functions of `W`, not a
smooth capacity curve. Measured: `W = 16 K` is **−15.0 %** at `N = 16 384` (where the stream fits
in one window instead of two), **−3.4 %** at `N = 32 768`, and **+4.1 %** (a regression) at
`N = 65 536`. `W = 32 K` is **−30.9 %** and **−29.2 %** at the two larger sizes, but the dominant
term there is not row locality: it is that the window now spans the reuse distance, so
deduplication folds a second pass over the line set and the target-request count falls 33.6 %.
Cycles track that count (≈ 8.4–10.2 cycles per request in every W1 cell, §4.3). No default
changes; M10 keeps `batch_window = 8 192`.

---

## 0. Scope, controls and reproducibility

All cells are the published M9/M10 driver, `experiment=m9diag`, on the controlled A/B configuration.
Exactly one parameter varies per comparison: `batch_window` (capacity per buffer), or the workload
geometry where the workload is the variable. Everything else is the published M10 setting:

```
--config configs/ab_controlled.cfg --set rho_dup=0 --set elem_bytes=4 --set bulk_wl=E0 \
  --set bulk_max_elems=262144 --set max_cycles=200000000 --set bulk_index_base=8388608 \
  --set bulk_desc_line=98304 --set page_sched=1 --set idx_mode=2 --set experiment=m9diag \
  --set pool_slots=128 --set dram_diag=1
```

Unchanged in every cell: E3 window policy (the M9 default), `batch_deadline = 4096`,
two-buffer architecture (`org_overlap = 1`), `R = 128`, `page_dir = 512`, `org_lanes = 4`,
`fanout_lanes = 4`, `eng_issue = 4`, `ag_lanes = 8`, index pipeline (`idx_mode = 2`,
`idx_buf_lines = 64`, `idx_mshr = 64`, `idx_reserve = 8`), L1 path (`mem_path = l1`), 16/64-entry
MSHRs, dTLB/PTLBs, 2 banks with the hashed map, 14/26 timings, unbounded DRAM queue, FR-FCFS.
No `pool_slots`, MSHR or other independent capacity was raised anywhere.

**Harness.** `build/wm/m12_matrix.sh` (90 cells, ~2 min) writes `build/wm/m12_*.txt`;
`build/wm/m12_extract.py` parses them (`perf`, `seedstats`, `pipeline`, `mem`, `e2`, `check`).

**Published anchors re-verified with the M12 build (§6):** W1 `220 123 / 138 924 / 125 581`,
W0 `11 052 / 17 267 / 18 142`, E0-policy control `B = 153 475`, `./bin/dx100tests` 4 032 checks,
0 failures. The M11.2 §9 control reproduces exactly: `batch_window = 16 384` → **106 456**.

**Code changes in this milestone are display-only** (no decision reads any of them; the anchors
above were re-measured after them):

| Change | File | Why |
| --- | --- | --- |
| Window-size histogram buckets scale with `W/8` (was hard-coded 1 024) | `bulk.cpp` (`export_win_records`), `ab.cpp` (`[16]` labels) | at `W = 8 192` the step is 1 024, so every earlier log keeps its numbers and labels byte for byte; at 16 K/32 K the old labels would have been false |
| `bulk_win_vec_bytes`: peak realized storage of the two batch buffers | `sim.hpp`, `bulk.cpp` (occupancy block), `ab.cpp` (`[16]`) | the brief asks for actual allocated storage, not only the design's 8 B/entry charge |
| L1/L2 MSHR peak occupancy, MSHR allocations, L1 stall cycles in `[15]` | `ab.cpp` | the brief asks for MSHR occupancy and refusals; the counters already existed, only the print was missing |

The value/order hashes are discussed in §6 — they are *not* an invariant across capacities, and the
reason is a property of the hash, not of the output.

---

## 1. What the implementation actually is (source-level)

### 1.1 Buffers, cursors, capacity

* **Two physical buffers.** `std::array<Batch, 2> bat_` (`sim.hpp`), each with its own entry
  vector, sealed order, issue order and page directory. There is no third buffer and no pool of
  windows.
* **`batch_window` is capacity *per buffer*.** `win_admit_one` rejects at `b.n_ent >= cfg_.batch_window`
  (`bulk.cpp:950`); `win_admit` sets `seal_pending_` at the same bound (`bulk.cpp:973`); `win_tick`
  seals on `b.n_ent >= cfg_.batch_window` (`bulk.cpp:1496`). Two buffers therefore hold at most
  `2W` entries, and `win_total_max` is checked against `bufs * W` in the invariants.
* **Two independent cursors.** `fill_` names the buffer accepting admissions, `iss_` the buffer
  being issued; they are separate indices, and step (4) of `win_tick` skips `iss_` when choosing a
  buffer to fill, so the next window *is* filled while the previous one is consumed
  (`org_overlap = 1`). They are independent *buffers*, not independent *ports*: one
  admission path (`ag_lanes = 8` addresses/cycle) and one issue path (`eng_issue = 4` lines/cycle)
  serve whichever buffer is current.
* **At most one unsealed window.** A single `fill_` cursor means no cycle can admit into two
  buffers; the invariant `win_incomplete_max <= 1` is checked every run and measured 1 everywhere.
  The remaining state machine is `empty → filling → issuing → draining → empty`.
* **Double buffering is not "two windows in flight" in the general sense.** At most one window
  issues and at most one fills; the second buffer is *either* the sealed window waiting to issue,
  *or* the buffer filling behind it. It is a two-slot pipeline, not two parallel consumers.

### 1.2 What limits what

| Quantity | Bound | Value here | Measured evidence |
| --- | --- | --- | --- |
| Elements pending in one window | `batch_window` | 8 192 / 16 384 / 32 768 | `win_occ_max` equals `W` when a window fills |
| Elements pending in both buffers | `2 × batch_window` | 16 K / 32 K / 64 K | `win_total_max` |
| Sealed-but-not-released windows | 1 structural + 1 issuing | ≤ 2 | `win_drain_max = 2` at `W = 8 192` |
| **Outstanding target requests** | `R` minus the index share | **≤ 120** | `peak targets in flight` reaches exactly 120 in every W1 cell (including the E2 control) and never exceeds it anywhere; PSCR peaks at 20–54 because its whole target set is small |
| Distinct pages per window | `page_dir` | 512 | `dir_collisions = 0` |
| Index lines in the acquisition ring | `idx_buf_lines` | 64 × 2 halves = 256 descriptors | unchanged by `W` |
| Indirect accesses *visible* to the organizer | elements admitted to the current window | ≤ `W` | `window entries` |

The last two rows are the point of the brief's warning: **`batch_window` does not raise the number
of outstanding target requests, the residency budget, the index window, or the DRAM queue.** It
raises only how many *element addresses* the organizer may see at once (§4.1 quantifies what that
is worth: the mean occupancy of the same 120 slots rises, the peak does not).

### 1.3 Structures that scale with `W`, and those that do not

Scales (per buffer): `ent` (`WinEnt`, one per element), `sorted` (entry ids), `lines` (`LineReq`,
one per distinct line), `order` (one per line), plus the modelled charge
`W·8 + page_dir·16 + page_cohorts·32` bytes per buffer.

Does **not** scale: `dir_vpn`/`dir_mask` (fixed by `page_dir` = 512), `coh` (fixed by
`page_cohorts` = 2), the index ring (256 descriptors), `FanJob` queue entries (fixed 64 B each),
the value/address stores (`val_`, `addr_` are per *output element*, `O(N)`, not per window), the
residency pool, all caches/TLBs/MSHRs, and the DRAM queue.

### 1.4 Actual storage and peak occupancy

Modelled window SRAM (`bulk_win_bytes`, two buffers, 8 B/entry + 8 KiB directory per buffer):

| `W` | modelled, 2 buffers |
| --- | --- |
| 8 192 | 147 584 B |
| 16 384 | 278 656 B |
| 32 768 | 540 800 B |

Measured realized peak of the buffers' own vectors (`bulk_win_vec_bytes`, new in M12; an upper
bound, because `std::vector` capacities never shrink):

| workload / `N` | `W = 8 192` | `W = 16 384` | `W = 32 768` |
| --- | --- | --- | --- |
| W1 / 16 384 | 901 824 | 893 632 | 893 632 |
| W1 / 32 768 | 933 856 | 1 786 112 | 1 220 784 |
| W1 / 65 536 | 933 888 | 1 851 328 | 2 441 760 |

The realized footprint is 6–9× the design's 8 B/entry charge, because the simulator stores
`WinEnt` (16 B) plus the line-ordered id vector, the `LineReq` vector and the issue order
(~24–32 B/entry more). A hardware window would use the packed 8 B/entry layout; the discrepancy is
an implementation artifact of how the model organizes, and it is reported rather than folded into
the design cost.

Peak occupancy is reported by `win_occ_max` (one window) and `win_total_max` (both); see §3.3.

---

## 2. Experiment matrix

| Group | Geometry | `W` | `N` | Seeds | Cells |
| --- | --- | --- | --- | --- | --- |
| **W1** (primary) | Λ 16 384, P 512, κ 0.25, permuted | 8 192 / 16 384 / 32 768 | 16 384 / 32 768 / 65 536 | 1–5 | 45 |
| **W1/E2** control | same, `win_policy = 1` (one live window) | same | same | 1 | 9 |
| **W0** | Λ 16 384, P 512, κ 0.25, `bulk_stream = 1` | same | same | 1 | 9 |
| **PSCR** | Λ 16 384, P 2 048, κ 0 (engine runs every op) | same | same | 1 | 9 |
| **STRID** (published) | Λ 1 024, P 64, κ 1 | same | same | 1 | 9 |
| **STRIDmix** | Λ 1 024, P 64, κ 0.25 (engine gets 75 %) | same | same | 1 | 9 |

Total **90 cells**, all `M9 GATES PASSED`, all return code 0. No cell was omitted.

Two matrix notes:

* At `N = 16 384` the descriptor has 16 384 elements, so `W = 32 768` cannot differ from
  `W = 16 384` — it is listed to show that (every number is identical, byte for byte), not as an
  independent point.
* **STRID at its published κ = 1 is all-SIMT**, so in arm C the engine receives no work at all
  (`target requests = 0`, C = A = 22 664 / 26 760 / 34 952). It is kept as the published cell, and
  `STRIDmix` (same geometry, κ = 0.25) is added so the window is actually exercised at that
  footprint. The bulk-only arm B of STRID is reported alongside, since it runs the whole stream
  through the engine regardless of κ.

---

## 3. Results

### 3.1 Primary W1 comparison (5 paired seeds)

Total execution cycles, arm C (dispatched). `±` is the stdev over seeds; the percentage is the mean
of the *paired* per-seed differences against `W = 8 192`.

| `W` | `N = 16 384` | `N = 32 768` | `N = 65 536` |
| --- | --- | --- | --- |
| 8 192 | 124 406 ± 1 134 | 214 114 ± 1 686 | 421 827 ± 1 912 |
| 16 384 | **105 716 ± 917** (−15.0 %) | **206 826 ± 1 825** (−3.4 %) | **439 337 ± 4 391** (+4.1 %) |
| 32 768 | 105 716 ± 917 (−15.0 %) | **148 047 ± 46** (−30.9 %) | **298 580 ± 4 596** (−29.2 %) |

Cycles per element: 7.59 / 6.45 / 6.45 (N = 16 K), 6.53 / 6.31 / 4.52 (N = 32 K),
6.44 / 6.70 / 4.56 (N = 65 K).

Per-seed deltas, so the sign is visible rather than averaged away:

| `N` | `W` | seed 1 | 2 | 3 | 4 | 5 | sign |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 384 | 16 384 | −15.23 | −15.31 | −14.89 | −14.49 | −15.19 | 5/5 faster |
| 32 768 | 16 384 | −3.56 | −3.73 | −3.05 | −3.29 | −3.39 | 5/5 faster |
| 32 768 | 32 768 | −31.27 | −30.46 | −31.24 | −30.11 | −31.19 | 5/5 faster |
| 65 536 | 16 384 | +4.40 | +4.72 | +4.46 | +3.18 | +3.99 | 5/5 **slower** |
| 65 536 | 32 768 | −28.75 | −28.72 | −28.66 | −30.49 | −29.47 | 5/5 faster |

The spread across seeds (≤ 1 % of the mean in every cell except `N = 65 536, W = 32 768`, where it
is 1.5 %) is much smaller than every effect except the two "no change" pairs, so the signs above
are not seed artefacts.

### 3.2 The other geometries

Cycles in arm C; B (bulk-only) in brackets where it differs in direction.

| Workload | `N` | `W = 8 192` | `W = 16 384` | `W = 32 768` |
| --- | --- | --- | --- | --- |
| W0 (streaming) | 16 384 | 18 142 | 18 093 (−0.3 %) | 18 093 (−0.3 %) |
| | 32 768 | 34 148 | 36 532 (+7.0 %) | 36 056 (+5.6 %) |
| | 65 536 | 69 428 | 69 097 (−0.5 %) | 72 941 (+5.1 %) |
| PSCR (page-scattered, all-bulk) | 16 384 | 623 096 | 322 401 (−48.3 %) | 322 401 (−48.3 %) |
| | 32 768 | 1 242 945 | 637 167 (−48.7 %) | 329 956 (−73.5 %) |
| | 65 536 | 2 485 036 | 1 266 791 (−49.0 %) | 647 042 (−74.0 %) |
| STRID (κ = 1, all-SIMT) | all | C unchanged | C unchanged | C unchanged |
| STRIDmix (κ = 0.25) | 16 384 | 21 302 | 21 290 (−0.1 %) | 21 290 (−0.1 %) |
| | 32 768 | 27 356 | 27 549 (+0.7 %) | 26 244 (−4.1 %) |
| | 65 536 | 42 967 | 41 206 (−4.1 %) | 43 000 (+0.1 %) |

Bulk-only arm B, for the same cells (`W = 8 192 → 16 384 → 32 768`): W0 `17 267 / 16 448 / 16 448`
(16 K), `34 084 / 33 553 / 32 401` (32 K), `68 204 / 66 216 / 65 389` (65 K); W1
`138 924 / 107 146 / 107 146`, `229 461 / 198 351 / 149 114`, `430 527 / 389 666 / 304 113`;
STRID `18 596 / 18 102 / 18 102`, `27 707 / 28 187 / 25 680`, `48 555 / 50 224 / 48 016`.

Three qualitative results follow, and they govern the interpretation:

1. **PSCR scales like a pure function of the window count.** 623 096 → 322 401 at `N = 16 384`
   (2 windows → 1), 1 242 945 → 637 167 → 329 956 at `N = 32 768` (4 → 2 → 1), 2 485 036 →
   1 266 791 → 647 042 at `N = 65 536` (8 → 4 → 2). Each halving of the window count halves the
   cycles, and `activations per activated row` follows exactly: 7.39 → 4.02 → 2.07 at `N = 65 536`.
2. **W0 gains nothing and can lose 5–7 %.** It is already coalescible and L2-resident; there is no
   locality left to recover at larger `W`, and the extra fill time before the first seal is pure
   loss (§4.4).
3. **STRID is capacity-insensitive in both arms**; its `±4 %` are the same order as the seed spread
   measured on W1 at this footprint, and `STRIDmix` has no consistent sign (+, −, +).

### 3.3 Window and pipeline behaviour (seed 1)

`windows` = adopted/sealed; `live` = cycles with two live windows; `size` = mean sealed window;
`iws` = mean sealed→processing-start wait; `fillW` = cycles with generator work and no buffer
accepting it; `wfull` = cycles the organizer was saturated.

| Workload | `N` | `W` | windows | size | occ max | live cycles (% of run) | iws | fillW | wfull |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| W1 | 16 384 | 8 192 | 2 / 2 | 6 188 | 8 192 | 79 856 (63.6 %) | 38 230 | 0 | 2 564 |
| W1 | 16 384 | 16 384 | 1 / 1 | 12 376 | 12 376 | 0 (0 %) | 1 | 0 | 510 |
| W1 | 32 768 | 8 192 | 4 / 4 | 6 155 | 8 192 | 204 530 (95.0 %) | 47 960 | 0 | 3 328 |
| W1 | 32 768 | 16 384 | 2 / 2 | 12 310 | 16 384 | 127 062 (61.2 %) | 59 907 | 0 | 4 981 |
| W1 | 32 768 | 32 768 | 1 / 1 | 24 620 | 24 620 | 0 (0 %) | 1 | 0 | 1 549 |
| W1 | 65 536 | 8 192 | 7 / 7 | 7 043 | 8 192 | 412 555 (97.3 %) | 54 514 | 316 171 | 4 796 |
| W1 | 65 536 | 16 384 | 4 / 4 | 12 326 | 16 384 | 420 484 (95.0 %) | 97 457 | 250 575 | 5 867 |
| W1 | 65 536 | 32 768 | 2 / 2 | 24 652 | 32 768 | 120 829 (40.0 %) | 52 528 | 0 | 10 238 |
| PSCR | 65 536 | 8 192 | 8 / 8 | 8 192 | 8 192 | 2 169 027 (87.3 %) | 267 466 | 1 833 515 | 4 615 |
| PSCR | 65 536 | 16 384 | 4 / 4 | 16 384 | 16 384 | 940 290 (74.2 %) | 226 783 | 603 698 | 6 028 |
| PSCR | 65 536 | 32 768 | 2 / 2 | 32 768 | 32 768 | 308 871 (47.7 %) | 142 661 | 0 | 10 686 |

There are **no buffer-full stalls in this path**, and that is structural, not an omission: a window
that fills is sealed immediately (`full → win_seal`), so "full" is an event (`seals full`), never a
stall. The producer stalls only when *both* buffers are occupied, which is `fillW`; the consumer
stalls when a sealed window is present and none is adopted, which is `cycles sealed but nothing
adopting` = **1 in every cell that runs a window and never more than 7 anywhere** (0 in STRID,
where the engine executes nothing) — a cold-start cost, never a hand-off delay.

**Is the second buffer filled before the first finishes?** Yes, and precisely: at `N = 65 536`,
`W = 32 768` the second buffer is adopted at cycle 33 926 (the cycle the first window starts
issuing), sealed at 57 892, while the first window is released at 162 947 — the second buffer is
ready **105 055 cycles before** the first finishes, and then waits sealed for that whole time
(`iws` = 52 528 mean). The same pattern at 16 K: sealed at 36 545, first release at 156 371.

**Are both buffers productively used?** Only as a hand-off. `live cycles` is 95–97 % of the run at
8 K and 16 K (one window issuing, one filling/waiting), but falls to **40 %** at `W = 32 768`,
because a 32 K window takes ~24 K cycles to fill and ~130 K to consume: the producer is finished
long before the consumer, and the second buffer spends most of its life sealed and waiting. The
pipeline is therefore *consumer-bound* at every capacity, and increasingly so as `W` grows.

### 3.4 What the larger window does to requests, activations and DRAM

Arm C, W1, seed 1. `reqs` = target requests generated (after dedup), `dedup` = fraction of
admitted elements folded onto an already-requested line, `busy` = sum of bank service cycles,
`util` = busy / (2 × cycles).

| `N` | `W` | elems | reqs | dedup | cyc/req | acts | rows | act/row | hit % | busy | util |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 16 384 | 8 192 | 12 376 | 12 376 | 0.0 % | 10.15 | 2 206 | 1 050 | 2.10 | 83.8 | 216 718 | 86.3 % |
| 16 384 | 16 384 | 12 376 | 12 376 | 0.0 % | 8.60 | 1 192 | 1 050 | 1.14 | 91.1 | 202 548 | 95.1 % |
| 32 768 | 8 192 | 24 620 | 24 606 | 0.1 % | 8.75 | 3 392 | 1 074 | 3.16 | 86.3 | 388 450 | 90.2 % |
| 32 768 | 16 384 | 24 620 | 24 596 | 0.1 % | 8.45 | 2 362 | 1 074 | 2.20 | 90.7 | 382 992 | 92.2 % |
| 32 768 | 32 768 | 24 620 | 16 384 | **33.5 %** | 9.04 | 1 339 | 1 074 | 1.25 | 92.5 | 267 508 | 90.3 % |
| 65 536 | 8 192 | 49 304 | 49 290 | 0.0 % | 8.60 | 6 709 | 1 122 | 5.98 | 86.7 | 785 716 | 92.7 % |
| 65 536 | 16 384 | 49 304 | 49 272 | 0.1 % | 8.98 | 3 698 | 1 122 | 3.30 | 92.9 | 776 996 | 87.8 % |
| 65 536 | 32 768 | 49 304 | 32 752 | **33.6 %** | 9.22 | 2 626 | 1 122 | 2.34 | 92.7 | 533 734 | 88.4 % |

Three readings, each of which the report will use:

* **Activations per activated row track the number of windows, not their size.** Two windows give
  2.10 (`N = 16 K`), 2.20 (`N = 32 K`) and 2.34 (`N = 65 K`) — nearly identical despite capacities
  of 8 K, 16 K and 32 K. One window gives 1.14 / 1.25. And 5.98 at seven windows. This is the
  controlled comparison the brief asks for ("is the 16 K result simply a one-window-versus-two-window
  effect"): **yes — at equal window count, capacity buys nothing**.
* **Cycles per target request are constant** at 8.5–10.2 across every W1 cell. The cycle change is
  the *request* change plus a boundary-activation term, never a per-request efficiency gain.
* **The 16 K regression at `N = 65 536` is not extra DRAM work.** Requests are unchanged
  (49 272 vs 49 290) and DRAM service per request is unchanged (15.8 vs 15.9 cycles), but the run
  is 4.1 % longer because the DRAM is *less* continuously busy (87.8 % vs 92.7 % of two banks): the
  long monotone sweep issues in bursts. `[14]` classifies the added time as "neither pipeline
  moved" 17 418 cycles vs 8 967 (index-starved 13 823 vs 6 394) and residency refusal 80.9 % of the
  run vs 68.6 %. In bulk-only (arm B) the same cell improves by 9.5 %, so the loss is created by
  the shared pool, not by the window itself. §8 records this as an unexplained residual.

### 3.5 Translation and the caches

| Workload / `N` | `W` | page walks | dTLB MSHR stall cycles | L1 req | L1 MSHR peak | L2 MSHR peak |
| --- | --- | --- | --- | --- | --- | --- |
| W1 / 16 384 | 8 192 → 16 384 | 526 → 526 | 178 437 → 135 236 | 14 153 → 14 153 | 16 → 16 | 17 → 17 |
| W1 / 32 768 | 8 192 → 16 384 → 32 768 | 571 → 538 → 538 | 213 365 → 103 713 → 104 092 | 28 183 → 28 173 → 19 961 | 16 | 18 → 17 → 17 |
| W1 / 65 536 | 8 192 → 16 384 → 32 768 | 691 → 622 → 562 | 300 075 → 176 962 → 156 705 | 56 431 → 56 413 → 39 893 | 16 | 18 → 17 → 17 |
| PSCR / 65 536 | 8 192 → 16 384 → 32 768 | 16 374 → 8 257 → 4 161 | 4 948 321 → 2 501 398 → 1 263 373 | 69 633 → 69 633 → 36 865 | 16 | 17 |
| W0 / 65 536 | 8 192 → 16 384 → 32 768 | 177 → 177 → 177 | 153 350 → 136 980 → 121 358 | 10 226 → 10 226 → 10 224 | 16 | 18 → 17 → 17 |

`L1 MSHR peak` is **16 — the configured maximum — in every one of the 90 cells**, at every capacity.
L2 MSHR peak is 17–18 of 64. So a larger window does not relieve MSHR pressure anywhere; it does
not even change it. What it does change is translation: walks fall with the window count
(PSCR −75 %, W1 −19 % at `N = 65 536`), because each window re-touches every page once.

PSCR is the clearest case that capacity's benefit is not a DRAM benefit: at `N = 65 536` its DRAM
utilisation is 27.2 % at `W = 8 192` (nothing is waiting on DRAM — 4.9 M cycles of dTLB MSHR stalls
and 16 374 walks dominate), and the capacity gain is the disappearance of those walks as the window
count falls 8 → 4 → 2.

### 3.6 Single buffer versus two (E2 control, seed 1)

`win_policy = 1` keeps one live window: a new buffer is adopted only when no other buffer is live,
and the E3-style deadline is anchored at allocation. `ag-double` is the difference of the
double-buffered cell against this control.

| `N` | `W` | double | single | single vs double | acts double / single | window size double / single |
| --- | --- | --- | --- | --- | --- | --- |
| 16 384 | 8 192 | 125 581 | 147 616 | +17.6 % | 2 206 / 2 124 | 6 188 / 6 188 |
| 16 384 | 16 384 | 106 456 | 147 616 | **+38.7 %** | 1 192 / 2 124 | 12 376 / 6 188 |
| 16 384 | 32 768 | 106 456 | 147 616 | +38.7 % | 1 192 / 2 124 | 12 376 / 6 188 |
| 32 768 | 8 192 | 215 415 | 234 171 | +8.7 % | 3 392 / 4 183 | 6 155 / 6 155 |
| 32 768 | 16 384 | 207 737 | 235 754 | +13.5 % | 2 362 / 3 210 | 12 310 / 8 206 |
| 32 768 | 32 768 | 148 053 | 235 754 | **+59.2 %** | 1 339 / 3 210 | 24 620 / 8 206 |
| 65 536 | 8 192 | 423 792 | 425 366 | +0.4 % | 6 709 / 7 449 | 7 043 / 7 043 |
| 65 536 | 16 384 | 442 419 | 434 079 | **−1.9 %** (single wins) | 3 698 / 6 448 | 12 326 / 8 217 |
| 65 536 | 32 768 | 301 937 | 434 079 | **+43.8 %** | 2 626 / 6 448 | 24 652 / 8 217 |

Two things this isolates, and one it cannot:

* With one buffer, **capacity above ~8 K is largely unusable**: at `N = 16 384` a single buffer
  seals at 6 188 elements whatever `W` is, because it cannot be filled while it is consumed and the
  deadline fires during the alternation. So "two 16 K buffers" is not "16 K of visibility with
  redundant buffering" — without the second buffer the 16 K capacity does not exist.
* Where the capacity is usable (32 K), the second buffer is worth **44–59 %**. Double buffering is
  the *enabler* of large windows, not a beneficiary of them.
* It cannot cleanly separate "overlap" from "usable capacity", because a single buffer caps the
  fillable size. The honest statement is: E2 measures the value of the second buffer *including*
  the capacity it unlocks, and that combined value is the +8.7 % to +59 % above.

---

## 4. Attribution

### A. Visibility benefit — none beyond deduplication

The organizer's within-window ordering is unchanged by `W` (one `sorted` pass by `va_line`, one
ascending-page issue order). The measured consequence: at equal window count, `act/row` is 2.10 /
2.20 / 2.34 for capacities of 8 K / 16 K / 32 K (§3.4). If more simultaneous visibility helped the
organizer, these would separate; they do not.

The one genuine visibility effect is that a window can only fold a repeated line if both copies are
*admitted together*: dedup is 0.0–0.1 % for every cell where `W` is below the reuse distance, and
jumps to **33.5 %** exactly when the window is wider than the reuse distance
(`W > Λ` = 16 384 elements, §4.3). This is visibility of *repeats*, not better ordering.

A secondary, measurable visibility effect is that the engine keeps the residency pool fuller on
average: mean targets in flight rises 45.6 → 97.7 at `N = 16 384` (W1) and 86.4 → 102.2 at
`N = 65 536` — but the **peak is 120 in every W1 cell and never above 120 in any cell**, i.e.
`R − idx_reserve`. More visibility smooths the use of the same slots; it does not add one.

### B. Double-buffering benefit — real, and it grows with capacity, but it is a hand-off, not overlap

§3.6: the second buffer is worth +0.4 % to +17.6 % at `W = 8 192` and +43.8 % to +59.2 % at
`W = 32 768`. Yet §3.3 shows the consumer is the bottleneck at every capacity: the second buffer is
filled and sealed 50–105 K cycles before the first is released, and `live cycles` falls from 97 %
(at 8 K) to 40 % (at 32 K). So the benefit is not "producer and consumer running concurrently at
full rate"; it is "the hand-off never waits for a fill". At 8 K the hand-off is short (§3.3: `iws`
38–54 K of a 125–424 K run) and the second buffer matters less; at 32 K a fill is long enough that
without the second buffer the consumer would idle for the whole fill.

### C. DRAM locality benefit — quantified, and it is a small part of the total

| cell | Δ activations | Δ activation service cycles | Δ DRAM busy | Δ cycles | locality share of the gain |
| --- | --- | --- | --- | --- | --- |
| W1, 16 K, 8 K→16 K | 2 206 → 1 192 (−46.0 %) | 57 356 → 30 992 (−26 364) | −14 170 (−6.5 %) | −19 125 (−15.2 %) | activation saving exceeds the cycle saving (utilisation rose 86.3 → 95.1 %), so the whole gain is activation time |
| W1, 65 K, 8 K→16 K | 6 709 → 3 698 (−44.9 %) | 174 434 → 96 148 (−78 286) | −8 720 (−1.1 %) | **+18 627 (+4.4 %)** | the DRAM work fell and the run got slower — locality is not the binding constraint |
| W1, 65 K, 8 K→32 K | 6 709 → 2 626 (−60.9 %) | 174 434 → 68 276 (−106 158) | −251 982 (−32.1 %) | −121 855 (−28.7 %) | about half the busy reduction is fewer *requests* (49 290 → 32 752), the other half activations |
| PSCR, 65 K, 8 K→32 K | 31 195 → 8 724 (−72.0 %) | 811 070 → 226 824 | −729 748 (−54.0 %) | −1 838 014 (−74.0 %) | neither: 12 213 fewer page walks and 32 768 fewer requests dominate |

So: **at `N = 16 384` the entire 15 % gain is fewer row activations** (the workload fits one
window; nothing is deduplicated). At `N = 65 536` activation savings are largely hidden — the
DRAM has two banks and is already ~90 % busy, so an activation on one bank overlaps a row hit on
the other — and the cycle win at 32 K comes instead from requesting 33.6 % fewer lines. No cell's
gain should be attributed to row activations alone.

### D. Other effects

* **Translation pressure falls monotonically** with `W` (walks −19 % on W1/65 K, −75 % on PSCR),
  because a window re-walks every page it touches and larger windows are fewer. On PSCR this is the
  dominant effect.
* **Request admission and MSHR pressure are unchanged**: L1 MSHR peak pinned at 16 (its limit) in
  all 90 cells; L2 MSHR peak 17–18 of 64; index lines claimed unchanged (1 539 / 3 082 / 4 096 per
  W1 cell group).
* **Index acquisition starts later and finishes earlier** as `W` grows (first target issue
  46 114 → 53 536 → 59 962 at `N = 65 536`), and index starvation in `[14]` doubles at 16 K
  (6 394 → 13 823 cycles). Larger windows trade a later, longer start for a shorter tail.
* **No extra buffering pressure anywhere else**: DRAM queue peak is 16 in every cell (the depth
  the published M10 configuration reaches, unchanged by `W`); the fan-out queue and the residency
  pool are untouched.
* **W0 loses 5–7 %** where W1 wins: it is already coalescible and fully deduplicated (93.7 %
  dedup at *every* capacity), so the only thing a larger window adds is a longer fill before the
  first seal — first target issue moves 46 137 → 59 975 at `N = 65 536`, and activations per row get
  *worse* (1.64 → 2.23) because a 32 K window covers several passes of the same rows within one
  window's sweep.

### 4.3 The step, not the slope

W1 with `rho_dup = 0` draws each element from a *distinct* line, so one pass over the line set is
`Λ = 16 384` elements, and the reuse distance of a line is `Λ` elements. A window can fold a repeat
only if the repeat falls inside it, i.e. `W > Λ`. Measured per-element cost, grouped by that
condition:

| window vs reuse distance | cells | cycles per element | dedup |
| --- | --- | --- | --- |
| `W ≤ Λ` (no fold possible) | every `W = 8 192` cell; `W = 16 384` at `N ≤ 32 768` | 6.31 – 7.67 | 0.0 – 0.1 % |
| `W = 2Λ` (one fold per line) | `W = 32 768` at `N = 32 768` and `N = 65 536` | **4.52, 4.61** | **33.5, 33.6 %** |

The boundary is a step, not a slope: **once the window exceeds the reuse distance, each line is
requested once for two element visits and per-element cost drops from ~6.5 to ~4.5 (−30 %)**. For
`W ≤ Λ`, capacity changes nothing but the window count, which is why 16 K is a regression at
`N = 65 536` (bigger window, still exactly one pass wide) and why `W = 32 768` at `N = 16 384` is
identical to `W = 16 384` (the whole stream is already one window).

### 4.4 Sanity check on the published M11.2 observation

M11.2 §9 reported `batch_window = 16 384` → 106 456 and `262 144` → 106 456 at `N = 16 384` and
called the effect "the window boundary". M12 confirms it exactly (106 456 at 16 K/32 K, five seeds,
and the same 1 192 activations) and adds the missing qualifier: the boundary effect is real, but it
is *bounded by the activation cost the two banks can hide* — worth 15 % here because the run was
short and the DRAM 86 % busy, and worth −4 % at `N = 65 536` where the same saving cannot be
converted into time.

---

## 5. Resource cost

| `W` per buffer | modelled SRAM (2 buffers) | realized peak (W1/65 K) | extra entries tracked | peak outstanding | mean outstanding |
| --- | --- | --- | --- | --- | --- |
| 8 192 | 147 584 B | 933 888 B | 16 384 | 120 | 86.4 |
| 16 384 | 278 656 B | 1 851 328 B | 32 768 | 120 | 102.2 |
| 32 768 | 540 800 B | 2 441 760 B | 65 536 | 120 | 94.8 |

Doubling `W` doubles the modelled window SRAM (8 B/entry: +131 072 B per step for two buffers), the
realized vector footprint (≈ +0.9 MB per step in this implementation), the page-directory charge
(unchanged at 8 KiB per buffer) and the number of elements the organizer tracks. It does **not**
increase request-tracking capacity: outstanding target requests stay at 120 and the DRAM queue peak
at 16. The cost is therefore paid entirely in SRAM for *visible elements*, and it buys either
nothing (no repeats, no boundary change) or a 30 % step (repeats folded).

---

## 6. Correctness and regression validation

| Check | Result |
| --- | --- |
| 90 cells, harness exit codes | all 0 |
| `M9 GATES PASSED` per cell | 90 / 90 |
| Completion **values** vs the expectation computed from the index image, arm C | 72 / 72 cells identical, **0 mismatched addresses** (the 72 are the mixed-class cells where the driver prints the check; the all-bulk cells — PSCR — are covered by the engine's own `addr_mismatch` and conservation gates, which also pass) |
| `window entries == elements in the descriptor` (nothing lost, nothing duplicated) | every cell |
| `lines issued + dedup merges == window entries` (element→line accounting closes) | inside the gates, every cell |
| `window occupancy <= W`, `buffered entries <= 2W`, `at most one unsealed window` | inside the gates, every cell |
| residency invariant `unissued + issued_not_filled <= R` (`bulk_residency_violations = 0`) | every cell |
| shared residency accounting (`slot take/give` balance, pool attribution) | inside the gates, every cell |
| `./bin/dx100tests` | **4 032 checks, 0 failures** |
| Published anchors, W1 | 220 123 / 138 924 / **125 581** — exact |
| Published anchors, W0 | 11 052 / 17 267 / **18 142** — exact |
| E0-policy control | **153 475** — exact |
| M11.2 §9 control (`W = 16 384`, N = 16 384) | **106 456** — exact |
| M10 defaults / published configs | untouched (`ab_controlled.cfg` unchanged, `batch_window = 0` in the file, M9/M10 drivers unchanged) |

**On the completion hashes (`check` reports 24 cells "hash differs").** `BulkStats::completion_hash`
is `fnv(hash, element_id)` folded in *retirement order*; it is order-sensitive by construction. A
window boundary changes the order in which lines are issued and fanned out, so the hash changes
across `W` for every workload whose completion order changes — including PSCR and W0, which are
otherwise identical. It is **not** an output check: the output check is the value hash against the
index-derived expectation plus `addr_mismatch`, both of which pass in all 72 arm-C cells, and the
element/line accounting closes exactly. The hash is still a valid inertness check for a *diagnostic
flag* at fixed `W` (which is how M11.2/M11.3 used it: 2645928636 / 1417164640 identical with
`dram_diag` off and on), and it is unchanged across capacities for STRID, where the engine executes
nothing in arm C. §8 records this as a limitation of the counter, not of the implementation.

Out-of-order completion and buffer ownership are covered by the same gates: `p.win_batch/window
line` name the owning buffer and line for every fill, a fill for a stale buffer is rejected
(`b.state != 2` or `p.win_batch >= bat_.size()`), and `issue_monotonic_violations = 0` in every
cell.

---

## 7. Decision

1. **Is 16 K per buffer a meaningful improvement over 8 K?** Only in the one case where it removes
   a window boundary, and that case is exactly the size the brief warned about. At `N = 16 384` it
   is −15.0 % (5/5 seeds) but it changes 2 windows into 1; at `N = 32 768` it is −3.4 % (5/5); at
   `N = 65 536` it is **+4.1 %, a regression on every seed** because 16 384 entries is exactly one
   pass over the line set there. So: **not a general improvement, and not a safe default.**
2. **Does 32 K provide additional benefit or diminishing returns?** It provides a *step*, not a
   slope: −30.9 % and −29.2 % at `N = 32 768` and `65 536`, 0 % relative to 16 K at `N = 16 384`.
   The step exists only when `W` exceeds the workload's reuse distance (`Λ` elements), i.e. it is a
   property of the workload, not of the capacity, and it is worth up to ~30 % — far above the whole
   measured DRAM-locality ceiling from M11.3 (6.7 %), so it is not noise, but it is conditional.
3. **Does double buffering actually improve throughput at larger capacities?** Yes, and it is the
   precondition rather than the mechanism: +43.8 % to +59.2 % at 32 K, +0.4 % to +17.6 % at 8 K.
   But the second buffer is *filled and then waits* 50–105 K cycles; it hides the hand-off, not a
   concurrent fill/consume race, and `live cycles` falls to 40 % at 32 K.
4. **Does the benefit justify the storage and metadata?** Modelled: 540 800 B for two 32 K buffers
   against 147 584 B at 8 K, i.e. +262 144 B for the last doubling (0.54 MB total), against a
   conditional ~30 % cycle win — attractive *if* the dispatch can predict that a window will exceed
   the reuse distance. Realized: ~2.4 MB in this implementation, which does not justify itself; a
   hardware window would need the packed 8 B/entry layout the model assumes.
5. **Should a larger window be considered for a future architecture revision?** Yes, but as a
   *workload-aware* decision, not as a default increase: the parameter that predicts the win is
   `W / reuse distance`, and the two things that actually move are the number of windows and the
   deduplication ratio. A 32 K window with no knowledge of the reuse distance is as likely to cost
   4 % as to win 30 %.
6. **M10 defaults are unchanged.** `batch_window = 8 192`, `batch_deadline = 4096`, E3 policy,
   two buffers, `R = 128`. No published configuration, milestone name or baseline was modified.

**Recommended next experiment** (§8): a *reuse-aware window* — keep `W = 8 192` as the default and
let the dispatch raise it, capped at 32 K, only when the descriptor's element count and line-set
size show that a window would exceed the reuse distance (both are derivable before cycle 0 from the
index array the dispatch already compacts). That is the only lever this study shows to be worth
30 %, and it is a property of the workload the dispatcher can see.

---

## 8. Limitations

1. **`W = 32 768` at `N = 16 384` is not an independent cell** (one window holds the whole
   stream); it is reported to demonstrate that, and excluded from every conclusion.
2. **The E2 control conflates overlap with usable capacity** (§3.6). A single buffer cannot be
   filled while consumed, so its window is deadline-truncated at ~8 K; the comparison therefore
   measures "one buffer" as a configuration, not "no overlap" as a mechanism.
3. **The `N = 65 536`, `W = 16 384` regression is measured but not fully explained.** Requests and
   DRAM service per request are unchanged; DRAM utilisation falls from 92.7 % to 87.8 %; `[14]`
   attributes the added cycles to index starvation (6 394 → 13 823) and residency refusal
   (68.6 % → 80.9 % of the run); arm B improves 9.5 % in the same cell, so the shared pool is
   involved. Isolating it would require changing the arbitration or the pool, which this
   investigation is not allowed to touch; it is documented rather than chased.
4. **Only W1 has five seeds.** W0, PSCR, STRID and STRIDmix are single-seed. Their effects are
   either far outside the W1 seed spread (PSCR −48 %, −74 %) or inside it (STRIDmix ±4 %), so the
   conclusions are not affected, but the per-cell means are not seed means.
5. **STRID at its published κ = 1 gives the engine no work in arm C.** Its C numbers are therefore
   SIMT-only and capacity-invariant by construction; STRIDmix exists to cover the footprint with
   the engine active.
6. **No DRAM writes are reported** because the trace is a pure gather: `write_va` preloads memory
   before cycle 0 and issues no DRAM traffic, so writes are structurally zero in every cell.
7. **Per-bank busy cycles are not instrumented**; `DRAM bank busy (cycles)` is the sum over both
   banks, so bank *utilisation* here is aggregate (busy / 2 × cycles), not per bank.
8. **The completion hash is order-sensitive** (§6) and therefore not usable as a cross-capacity
   output check; the value hash plus `addr_mismatch` are.
9. **`pool_slots`, MSHRs and every other independent capacity were held fixed**, as required; no
   claim here says anything about what a larger residency budget would do in combination with a
   larger window. M11.3 measured the one nearby lever (`l1_mshr = 32`, −2.8 %), which is a
   different mechanism and is not additive with anything in this report.

---

## 9. Files

| Path | Contents |
| --- | --- |
| `build/wm/m12_matrix.sh` | the 90-cell harness (grouped, each cell printed with its cycles and gate count) |
| `build/wm/m12_extract.py` | section-aware parser: `perf`, `seedstats`, `pipeline`, `mem`, `e2`, `check` |
| `build/wm/m12_*.txt` | 90 run logs (`W1`, `W1_e2`, `W0`, `PSCR`, `STRID`, `STRIDmix` × `N` × `W` × seed) |
| `build/wm/m12_matrix.log` | harness transcript: one line per cell, all `gates=1` |
| `sim/src/bulk.cpp`, `sim/src/sim.hpp`, `sim/src/ab.cpp` | the three display-only additions of §0 |
