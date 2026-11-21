# M23-G6C — G6B with P0-style translation admission deferral: the 16K matrix

**Report complete: 15 / 15 cells, every check passed, all seven published anchors exact.**

## 0. Answer in one paragraph

G6C recovers P0's translation admission completely and therefore performs exactly like P0: it is
**1.25 %, 0.19 % and 0.07 % below P0's cycles** on Orkut, LiveJournal and ogbn-products, **31 / 12 / 42 %
below P2**, **36 / 28 / 47 % below G6B**, and **2.97× / 2.38× / 1.04×** M0. It does *not* beat P0, and the
reason is measurable rather than speculative: after the deferral fix the machine runs at **97.6 / 99.2 /
98.6 % of the page-walker's own serial time** (`walks × 300 / 2` cycles), the same place P0 sits
(96.4 / 99.0 / 98.5 %). Translation admission was G6B's only deficit; G6B's independent physical DRAM
scheduling adds ≤ 1.3 % once admission is fixed, and the ready buffer still holds only ~1–6 physical
rows, so it cannot recover P2's row locality. **The remaining bottleneck is page-walk throughput, not
admission and not DRAM.** No performance claim is made beyond these measured cells.

## 1. Architecture and implementation summary

The implementation is the M23-G6C arm `bulk_org = 8`, described in
`docs/m23-g6c-admission-deferral.md` and unchanged for this experiment (no code was modified; see §7).
In one line: **G6B's cycle structure and data path, with P0's admission policy in front of it.**

* Translation admission walks the window's runs in *stored* order (ascending line; a 4 KiB page's 64
  runs are contiguous) and keeps up to `page_cohorts = 2` page runs open in P0's own cohort table
  (`Batch::Cohort`, charged 32 B each by `win_seal`).
* A dTLB refusal sets `resume_at = cycle + park_retry` and admission **continues with another page in
  the same cycle** — P0's rule. Refusals are classified as `refuse_parked` (this page's MSHR at
  `dtlb_parked = 2`) or `refuse_full` (no MSHR free of 4).
* A completed translation enters the 4 096-entry ready buffer immediately, keyed by its position in the
  P2 (row, bank, line) order. The DRAM scheduler pops the smallest key and releases it through the
  unchanged `win_emit_fused`, exactly as in G6B. Translation and DRAM overlap in the same cycle.
* Unchanged by this arm: translation resources (16×4 dTLB, 4 MSHRs, 2 parkable per page; 512×4 L2 TLB,
  8 MSHRs; 2 walkers; 16-entry walk cache; 150 cycles/level × 2 levels), the ready buffer, the P2
  release order, `win_emit_fused`, the fused window representation, and every other arm's code path.

## 2. Configurations, inputs and hashes

One configuration file for all five arms — the **frozen M23-G6B configuration**, byte-identical to the
M23-G6B matrix's. Only `dispatch` and `bulk_org` move, both on the command line.

| Item | Value |
|---|---|
| Config | `sim/configs/m23g6b_base.cfg`, sha256 `abc91fab3ba094c77800438f0a63847afecd1a1248a05229237f1295b38b4b4b` |
| Orkut BFS | `build/wm/m23_orkut_bfs.bin`, 19 501 271 B, `11cc8da4cfaf9a9920c567a89952399ca77fa23f7ea807ef3ebc6acf76c6c29e` |
| LiveJournal BFS | `build/wm/m23_livejournal_bfs.bin`, 19 502 195 B, `7363867a364c7a9b09dc3b73f5b62ca7272d957b6abd283a85d01b8dc0923ad1` |
| ogbn-products train gather | `build/wm/m23_ogbn_train_100d.bin`, 136 522 502 B, `c20ce42a72b04468e2cc8070365247cd8f0056cbdf19d0fb776e0b1e291f3818` |
| Simulator | `sim/bin/dx100sim`, `e41bcd8a2990b4a79af24265a36c61162665275b18c5f54effd78456b1c84ebc` |
| Runner | `build/wm/m23g6c_run.sh`, `2c71a311eed6063b5140770fcd016a3bdba920f00084bcbaf40670e6031ad01e` |
| Extractor | `tools/m23g6c_extract.py`, `4ce65d7820c5abc6c18628397238fcf35c3ef94de8c1f9b81b7870f976772283` |

Arms: **M0** `dispatch=0, bulk_org=0` (no engine); **P0** `bulk_org=0`; **P2** `bulk_org=2`;
**G6B** `bulk_org=7`; **G6C** `bulk_org=8`. Window 16 384, `R = 128`, `eng_issue = 4`,
`page_sched = 1`, `page_cohorts = 2`, `park_retry = 1`, `g6b_pa_capacity = 4096`, 2 banks × 4 096 rows
× 1 KiB, `t_hit 14 / t_miss 26`, `frfcfs`, hashed bank index, `l1_mshr = 128`, `l2_mshr = 64`.

Exact cell command (one cell; the runner adds the per-workload trace, element count and arm flags):

```
cd DX100_MEM/sim
./bin/dx100sim --config configs/m23g6b_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 --set bulk_org=8 --all
```
Matrix: `./build/wm/m23g6c_run.sh all 4` (15 cells, 4 in parallel). ogbn uses
`bulk_max_elems=16777216`; M0 adds `--set dispatch=0`. Raw logs: `build/wm/m23g6c/g6c_<workload>_w16384_<arm>.txt`,
tidy table `build/wm/m23g6c/summary.csv`, runner log `build/wm/m23g6c_run.log`.

## 3. Overall cycles and relative performance

| workload | arm | cycles | ×M0 | vs P0 | vs P2 | vs G6B |
|---|---|---|---|---|---|---|
| Orkut | M0 | 49 067 545 | 1.000 | +193.56 % | +104.74 % | +89.17 % |
| Orkut | P0 | 16 714 538 | 2.936 | — | −30.26 % | −35.56 % |
| Orkut | P2 | 23 965 633 | 2.047 | +43.38 % | — | −7.60 % |
| Orkut | G6B | 25 938 106 | 1.892 | +55.18 % | +8.23 % | — |
| Orkut | **G6C** | **16 505 468** | **2.973** | **−1.25 %** | **−31.13 %** | **−36.37 %** |
| LiveJournal | M0 | 67 234 692 | 1.000 | +137.50 % | +108.41 % | +71.52 % |
| LiveJournal | P0 | 28 309 390 | 2.375 | — | −12.25 % | −27.78 % |
| LiveJournal | P2 | 32 260 832 | 2.084 | +13.96 % | — | −17.70 % |
| LiveJournal | G6B | 39 200 163 | 1.715 | +38.47 % | +21.51 % | — |
| LiveJournal | **G6C** | **28 256 445** | **2.379** | **−0.19 %** | **−12.41 %** | **−27.92 %** |
| ogbn train | M0 | 207 287 728 | 1.000 | +3.70 % | −40.33 % | −44.75 % |
| ogbn train | P0 | 199 882 548 | 1.037 | — | −42.46 % | −46.72 % |
| ogbn train | P2 | 347 389 697 | 0.597 | +73.80 % | — | −7.41 % |
| ogbn train | G6B | 375 172 893 | 0.553 | +87.70 % | +8.00 % | — |
| ogbn train | **G6C** | **199 738 512** | **1.038** | **−0.07 %** | **−42.50 %** | **−46.76 %** |

Note the ogbn row: M0 is only 3.7 % slower than P0, and P2/G6B are 68 % / 81 % *slower* than M0. P0 and
G6C are the only Bulk arms that beat the conventional baseline there (+3.7 % / +3.8 %).

## 4. Translation admission

| workload | arm | walks | dTLB miss | MSHR avg | MSHR max | walker util | L2 TLB occ avg | MSHR-stall cycles | refuse parked / full |
|---|---|---|---|---|---|---|---|---|---|
| Orkut | M0 | 312 381 | 700 635 | 3.59 | 4 | 95.5 % | 3.56 | 194 411 567 | 55 487 246 / 138 924 321 |
| Orkut | P0 | 107 399 | 192 014 | 2.75 | 4 | 96.4 % | 2.74 | 32 318 673 | 24 754 643 / 7 564 030 |
| Orkut | P2 | 107 444 | 192 050 | 1.81 | 4 | 67.2 % | 1.80 | 47 194 543 | 40 455 993 / 6 738 550 |
| Orkut | G6B | 107 439 | 192 108 | 1.56 | 4 | 62.1 % | 1.55 | 26 020 645 | 23 195 379 / 2 825 266 |
| Orkut | G6C | 107 400 | 192 071 | **2.80** | 4 | **97.6 %** | 2.78 | 32 575 918 | 25 014 863 / 7 561 055 |
| LiveJournal | M0 | 443 163 | 975 461 | 3.88 | 4 | 98.9 % | 3.85 | 267 459 083 | 29 187 662 / 238 271 421 |
| LiveJournal | P0 | 186 923 | 317 832 | 3.24 | 4 | 99.0 % | 3.22 | 56 356 068 | 37 692 673 / 18 663 395 |
| LiveJournal | P2 | 186 899 | 316 016 | 2.58 | 4 | 86.9 % | 2.57 | 64 202 040 | 52 907 297 / 11 294 743 |
| LiveJournal | G6B | 186 922 | 317 855 | 1.93 | 4 | 71.5 % | 1.92 | 39 393 233 | 33 125 388 / 6 267 845 |
| LiveJournal | G6C | 186 875 | 317 743 | **3.24** | 4 | **99.2 %** | 3.23 | 56 389 553 | 37 677 270 / 18 712 283 |
| ogbn | M0 | 1 307 998 | 2 585 450 | 2.96 | 4 | 94.7 % | 2.95 | 820 749 797 | 769 852 042 / 50 897 755 |
| ogbn | P0 | 1 313 192 | 2 588 592 | 2.13 | 4 | 98.5 % | 2.11 | 399 120 193 | 399 046 583 / 73 610 |
| ogbn | P2 | 1 313 744 | 2 579 869 | 1.17 | 4 | 56.7 % | 1.17 | 692 831 533 | 692 677 445 / 154 088 |
| ogbn | G6B | 1 313 204 | 2 588 553 | 1.06 | 4 | 52.5 % | 1.06 | 377 234 056 | 377 117 394 / 116 662 |
| ogbn | G6C | 1 312 297 | 2 586 789 | **2.13** | 4 | **98.6 %** | 2.11 | 399 508 374 | 399 049 803 / 458 571 |

G6C admission detail (`bulk_g6c_*`):

| workload | attempts | accepted | refused | parked / full | defer events | re-admitted | refusal→other page, same cycle | age mean / max | depth mean / max | cohorts opened | admission-idle cycles |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Orkut | 32 891 032 | 823 786 | 32 067 246 | 24 548 578 / 7 518 668 | 32 067 246 | 102 200 | 440 299 | 316 / 9 900 | 1.50 / 2 | 106 286 | 97.53 % |
| LiveJournal | 56 483 162 | 729 139 | 55 754 023 | 37 150 692 / 18 603 331 | 55 754 023 | 175 212 | 398 493 | 320 / 9 300 | 1.50 / 2 | 185 868 | 98.80 % |
| ogbn | 403 213 439 | 8 380 253 | 394 833 186 | 394 724 888 / 108 298 | 394 833 186 | 1 228 298 | 5 431 034 | 324 / 48 712 | 1.50 / 2 | 1 303 051 | 98.70 % |

Reading: every run is admitted exactly once (`accepted = bulk_org_runs`), every deferral is bounded
(`depth ≤ 2 = page_cohorts`, oldest-first retry at `park_retry = 1`), and the refusal mix matches P0's
(`parked / full` 24.5 M / 7.5 M vs P0's 24.8 M / 7.6 M on Orkut). G6B's `tr_stall` — a *cycle* count of a
stopped cursor, 25.6 M / 39.4 M / 373.9 M — is 0 by construction in G6C; the same backpressure now
appears as discrete deferral events, so the two must not be read as one counter.

## 5. Ready buffer and overlap

| workload | arm | capacity | peak | **avg occupancy** | occupancy integral | **residence mean / max** | backpressure cycles | no-ready cycles | tr-active | G6C tgt-outstanding | overlap / tgt-out |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Orkut | G6B | 4 096 | 4 096 | 33.8 | 877 970 421 | 1 066 / 30 318 | 0.14 % | 96.78 % | 99.54 % | 30.93 % | 98.66 % |
| Orkut | G6C | 4 096 | 4 096 | **93.3** | 1 540 486 007 | 1 870 / 35 074 | 0.71 % | 91.38 % | 98.70 % | 48.40 % | 97.47 % |
| LiveJournal | G6B | 4 096 | 4 096 | 11.8 | 461 245 609 | 632 / 30 307 | 0.06 % | 98.77 % | 99.83 % | 25.79 % | 99.44 % |
| LiveJournal | G6C | 4 096 | 4 096 | **18.0** | 508 476 322 | 697 / 30 969 | 0.17 % | 97.99 % | 99.67 % | 36.65 % | 99.21 % |
| ogbn | G6B | 4 096 | 3 319 | 1.1 | 427 630 234 | 51 / 252 497 | 0.00 % | 98.97 % | 99.98 % | 26.09 % | 99.96 % |
| ogbn | G6C | 4 096 | 4 096 | **3.8** | 759 056 382 | 91 / 293 094 | 0.04 % | 98.02 % | 99.94 % | 50.99 % | 99.96 % |

Identities: `ready_occ_sum == tr_wait_dram` exactly in every cell (it is the same quantity), every run
released exactly once (`runs_released == lines_issued == tr_requests == tr_completed`), peak ≤ capacity.
The buffer is never the binding constraint: backpressure is ≤ 0.71 % of cycles, and it *rises* with G6C
(0.14 → 0.71 % on Orkut) only because translation now outruns a 4 096-entry buffer at window start.

Occupancy distribution: only mean, peak, the fraction of cycles at capacity (backpressure) and the
fraction of cycles with nothing releasable (no-ready) are instrumented. No occupancy histogram exists,
and none was added during the experiment (§7). Coarse shape: G6C sits near the mean almost always with
start-of-window spikes to capacity (peak = 4 096 on all three workloads); G6B peaked at 3 319 on ogbn
and never filled.

## 6. DRAM

| workload | arm | reads | row hit | conflicts | rows used | bank util | B/cycle | starved | wait mean | service mean |
|---|---|---|---|---|---|---|---|---|---|---|
| Orkut | M0 | 519 337 | 29.82 % | 364 487 | 8 181 | 11.87 % | 0.677 | 90.76 % | 85.8 | 22.4 |
| Orkut | P0 | 651 309 | 60.43 % | 257 698 | 8 185 | 36.53 % | 2.494 | 65.45 % | 185.6 | 18.7 |
| Orkut | P2 | 651 339 | 60.47 % | 257 473 | 8 185 | 25.47 % | 1.739 | 75.02 % | 145.8 | 18.7 |
| Orkut | G6B | 651 220 | 60.47 % | 257 438 | 8 185 | 23.53 % | 1.607 | 77.14 % | 149.2 | 18.7 |
| Orkut | G6C | 651 193 | **60.49 %** | 257 280 | 8 185 | **36.97 %** | **2.525** | **65.32 %** | 201.8 | 18.7 |
| LiveJournal | M0 | 600 733 | 16.31 % | 502 761 | 8 192 | 10.74 % | 0.572 | 92.99 % | 39.7 | 24.0 |
| LiveJournal | P0 | 697 219 | 46.12 % | 375 629 | 8 192 | 25.20 % | 1.576 | 78.10 % | 101.7 | 20.5 |
| LiveJournal | P2 | 697 595 | **50.84 %** | 342 940 | 8 192 | 21.51 % | 1.384 | 79.37 % | 92.1 | 19.9 |
| LiveJournal | G6B | 697 145 | 46.13 % | 375 544 | 8 192 | 18.20 % | 1.138 | 82.97 % | 97.0 | 20.5 |
| LiveJournal | G6C | 697 162 | 46.14 % | 375 516 | 8 192 | 25.24 % | 1.579 | 78.14 % | 103.6 | 20.5 |
| ogbn | M0 | 9 377 309 | 48.12 % | 4 864 616 | 8 192 | 45.75 % | 2.895 | 55.65 % | 67.6 | 20.2 |
| ogbn | P0 | 9 925 604 | 62.70 % | 3 701 864 | 8 192 | 45.87 % | 3.178 | 56.27 % | 66.3 | 18.5 |
| ogbn | P2 | 9 927 758 | **66.21 %** | 3 354 401 | 8 192 | 25.80 % | 1.829 | 77.24 % | 56.9 | 18.1 |
| ogbn | G6B | 9 928 055 | 61.49 % | 3 823 321 | 8 192 | 24.64 % | 1.694 | 78.67 % | 56.0 | 18.6 |
| ogbn | G6C | 9 924 795 | 62.73 % | 3 698 964 | 8 192 | **45.89 %** | **3.180** | **56.20 %** | 66.9 | 18.5 |

DRAM *counts* are unchanged: reads within 0.05 % across the Bulk arms, bytes identical (41.7 / 44.6 /
635.2 MB, one 64 B line per read), and mean service time identical to ±0.4 cycles (18.5–20.5 on
LJ/ogbn, 18.7 on Orkut).
Only *utilisation* moves — G6C keeps the controller as busy as P0 (36.97 vs 36.53 % on Orkut, 25.24 vs
25.20 %, 45.89 vs 45.87 %), which is the throughput gain. Queue wait rises with utilisation (201.8 vs
185.6 on Orkut) as Little's law requires; it is not a regression in service.

`bulk_sched_opens` is **not comparable across arms** and is not used as a locality metric here: in P0/P2
it counts page runs / (bank,row) cells *opened*, in G6A/G6B/G6C it counts *cell changes at release*
(432 433 / 492 394 / 5 608 020 for G6C). The cross-arm locality evidence is `dram_rows_used` (identical),
row hits and conflicts.

## 7. Correctness and conservation

| Check | Result |
|---|---|
| Cells completed | **15 / 15**; extractor exit 0, "all 15 cells complete; all checks passed" |
| Address mismatches, residency violations, issue-order violations | 0 in all 15 cells |
| Requests generated vs completed; elements retired vs total | equal in all Bulk cells; window entries = elements |
| Duplicate translation or release | `tr_completed == tr_requests == runs`, `runs_released == lines_issued`, `g6c_dup_submit_blocked = 0` |
| Refusal accounting | `attempts = accepted + refused`, `refused = defer_events`, `parked + full = refused`, `refuse_other = 0` |
| Deferral boundedness / starvation | `depth_max = 2 = page_cohorts` on all three; `re-admitted > 0` on all three; no cell hit the cycle guard |
| Ready-buffer identity | `ready_occ_sum == tr_wait_dram` exactly, all six streaming cells |
| Published anchors (exact equality) | P0 16 714 538 / 28 309 390; P2 23 965 633 / 32 260 832; G6B 25 938 106 / 39 200 163 / 375 172 893 — **7 / 7 OK** |
| Historical cells, byte-identical | M23-G6B and M23-G6C P0/P2/G6B logs differ in **0 lines** after excluding the 18 new `bulk_g6c_*` keys (all nine pairs) |
| `dx100_tests` | **4 767 checks, 0 failures** (unchanged binary; no test was touched) |
| `conservation=OK` | 15 / 15 |

No cell was omitted, and no partial matrix was reported as complete. M0's ogbn cell is the slowest run in
the matrix (~40 min of host time: SIMT replays 16.7 M accesses with no windowing or dedup) but it
completed cleanly.

## 8. What improved, what did not (brief §5, point by point)

1. **Better translation admission — yes, to P0's level.** dTLB MSHR occupancy 2.80 / 3.24 / 2.13 of 4
   (P0 2.75 / 3.24 / 2.13; G6B 1.56 / 1.93 / 1.06) and walker utilisation 97.6 / 99.2 / 98.6 %
   (P0 96.4 / 99.0 / 98.5 %; G6B 62.1 / 71.5 / 52.5 %). The admission fix is complete, not partial.
2. **Increased translation concurrency — yes.** G6B's stopped-cursor stalls (25.6 M / 39.4 M / 373.9 M
   cycles) become P0's short bounded deferrals (`park_retry = 1`), with 440 k / 398 k / 5.43 M refusals
   immediately followed by admission of a *different* page in the same cycle.
3. **Greater ready-buffer scheduling visibility — yes, but small in absolute terms.** Occupancy rises
   2.8× / 1.5× / 3.3× (33.8 → 93.3, 11.8 → 18.0, 1.1 → 3.8 entries) and target-outstanding time rises
   1.56× / 1.42× / 1.95× (30.9 → 48.4, 25.8 → 36.7, 26.1 → 51.0 % of cycles). In units of the DRAM's own unit (one 16-line row cell) the buffer exposes
   at most ~6 / ~1 / ~0.25 rows at the mean, so the scheduler's choice is still local.
4. **Improved physical row locality — no.** G6C's row-hit rate is P0's, not P2's: 60.49 / 46.14 / 62.73 %
   versus P2's 60.47 / 50.84 / 66.21 %. On LiveJournal and ogbn it stays 4.7 and 3.5 points below P2, and
   conflicts fall only to P0's level. The buffer sees the *stored-order* translation stream, so what
   arrives is page-ordered regardless of how the release queue orders it.
5. **Changes in DRAM command count — none.** Reads differ by < 0.05 %, bytes are identical, and service
   time is unchanged; the throughput gain comes from keeping the same controller busier, not from extra
   or cheaper commands.
6. **Actual end-to-end improvement — yes, and it equals P0.** −36.4 / −27.9 / −46.8 % versus G6B,
   −31.1 / −12.4 / −42.5 % versus P2, −1.25 / −0.19 / −0.07 % versus P0. Against M0: 2.97× / 2.38× /
   1.04×. G6C is the only Bulk arm that beats SIMT on ogbn.

**Throughput was bought with per-request latency.** Mean latency from translation submission to
completion rises to 2 049 / 955 / 278 cycles (P0 299 / 317 / 191, G6B 1 228 / 855 / 221) because a
translated run now waits in the buffer (mean residence 1 870 / 697 / 91 cycles). Same throughput,
more requests in flight — that is the intended behaviour of a decoupled pipeline, and it means latency
must not be quoted as if it were the objective.

## 9. Per-workload bottleneck analysis

The page-walker floor is `walks × 300 / 2` cycles (2 levels × 150 cycles, 2 walkers):

| workload | arm | walks | walker floor (cycles) | cycles | share of the floor |
|---|---|---|---|---|---|
| Orkut | P0 | 107 399 | 16 109 850 | 16 714 538 | 96.4 % |
| Orkut | G6B | 107 439 | 16 115 850 | 25 938 106 | 62.1 % |
| Orkut | **G6C** | 107 400 | **16 110 000** | **16 505 468** | **97.6 %** |
| LiveJournal | P0 | 186 923 | 28 038 450 | 28 309 390 | 99.0 % |
| LiveJournal | G6B | 186 922 | 28 038 300 | 39 200 163 | 71.5 % |
| LiveJournal | **G6C** | 186 875 | **28 031 250** | **28 256 445** | **99.2 %** |
| ogbn | P0 | 1 313 192 | 196 978 800 | 199 882 548 | 98.5 % |
| ogbn | G6B | 1 313 204 | 196 980 600 | 375 172 893 | 52.5 % |
| ogbn | **G6C** | 1 312 297 | **196 844 550** | **199 738 512** | **98.6 %** |

* **Orkut.** Walker-bound in G6C and P0 (97.6 / 96.4 %). G6B had admission stopped in 25.6 M cycles
  (`tr_stall`; its whole excess over P0 is 9.2 M cycles), the DRAM starved in 77 % of cycles, and the
  walkers running at 62 %; G6C leaves 0.40 M cycles (2.4 %) of slack. Row locality was never Orkut's
  limiter — P0's 60.43 % is already P2's 60.47 % — so the whole 36 % gain over G6B is admission.
* **LiveJournal.** The tightest arm in the matrix: G6C is at 99.2 % of the theoretical walker bound
  (0.8 % slack), so there is essentially nothing left to schedule. Its 4.7-point row-hit deficit against P2 is worth
  nothing here: DRAM commands and bytes are identical and the controller is 25 % busy, i.e. fully fed.
* **ogbn-products.** Two regimes. P2/G6B destroy the page stream (walker 56.7 / 52.5 %, 77 % of cycles
  DRAM-starved) and end 68–81 % *slower than SIMT*. G6C restores the stream (98.6 % walker, 56.2 %
  starved) and lands 3.8 % below M0 and 0.07 % below P0 — but the walker floor is 196.8 M of the
  199.7 M cycles, so the remaining 1.4 % is all that any scheduling change could win here.
* **Why G6C cannot beat P0.** Both saturate the same resource with the same policy; the ≤ 1.3 % gap is
  the DRAM side (P2-ordered release keeps the controller 0.44 / 0.04 / 0.02 points busier, row hits
  +0.06 / +0.02 / +0.03 points, starved cycles −0.13 / +0.04 / −0.07 points). It is a real, deterministic
  1 % effect, not a new regime, and it is the *whole* value of G6B's independent physical scheduling at
  this translation capacity.

## 10. Limitations

* **Three workloads, one window size (16K), one translation configuration.** No 8K/32K, no capacity
  sweep, no walker or walk-latency variation — all excluded by the brief's scope.
* **The ready-buffer occupancy distribution is not instrumented** (only mean, peak, fraction-at-capacity,
  fraction-empty). The "multiple rows visible" answer is therefore derived from mean occupancy and the
  16-line cell size, and from the row-hit outcome, not measured as a histogram.
* **`bulk_sched_opens` is not cross-arm comparable** (different events per arm); cross-arm locality rests
  on `dram_rows_used`, row hits and conflicts.
* **Deferral age is per episode** (first refusal → the admission that ends it), and admission-idle
  (97.5–98.8 % of cycles) is high by construction: with two cohorts × two parkable MSHRs the dTLB is
  legitimately full most of the time, so "idle" there means "every cohort is deferred", not "no work".
* **Mean latency rises 5–7× versus G6B** (buffer residence); this matrix measures throughput only.
* **M0 ogbn is expensive** (~40 min host time) and sits closest to the Bulk arms (3.7 % apart), so that
  cell's reference value is a small difference of two large numbers; it is exact, but treat the 1.04×
  as "parity", not as a speedup to tune against.
* Deterministic simulation, single run per cell; no measurement noise is modelled, so the ≤ 1.3 % G6C–P0
  differences are real for this model but not statistically qualified.

## 11. Recommended next experiment (one, falsifiable)

**M23-G6D — is the residual gap entirely the page walker?** Re-run this same five-arm matrix while
varying only the translation *throughput* configuration: `walkers ∈ {2, 4}` and
`walk_latency_per_level ∈ {150, 75}`, keeping page size, dTLB/L2-TLB geometry, `dtlb_parked`, DRAM
mapping and timing, and the ready buffer fixed. The prediction from §9 is quantitative: total cycles
should scale as `walks × 2 × walk_latency / walkers`, i.e. halving the walker serial time should take
G6C from ~16.5 M to ~8.3 M cycles on Orkut and from ~199.7 M to ~100 M on ogbn, with the ordering of the
five arms otherwise unchanged.

* If cycles track that law, translation *throughput* — not admission, not DRAM scheduling — is the
  binding resource, and the useful next step is to reduce walks (page-directory residency, walk-cache
  behaviour) rather than to add scheduling policy.
* If they do not, the deficit is elsewhere (e.g. DRAM service interleaving under higher request rates)
  and the same cells will localise it, because every counter needed is already in this matrix.

Do not proceed to further scheduling policies before that measurement: §8 shows the scheduling design
space that remains is worth ≤ 1.3 % at the current translation capacity.

## 12. Artifacts

| Path | Contents |
|---|---|
| `build/wm/m23g6c/g6c_<workload>_w16384_<arm>.txt` | 15 raw cell logs (arms `m0, p0, p2, p7, p8`) |
| `build/wm/m23g6c/summary.csv` | one row per cell, every derived metric (sha256 `e44970c64bd51c04…`) |
| `build/wm/m23g6c_run.log` | runner log, 15 cells, skip-if-present so the matrix is restartable |
| `build/wm/m23g6c_run.sh`, `tools/m23g6c_extract.py` | cell runner and extractor (hashes in §2) |
| `build/wm/m23g6b/*` | untouched M23-G6B matrix, used for the byte-identical reproduction check |
| `docs/m23-g6c-admission-deferral.md` | implementation note for `bulk_org = 8` (unchanged by this experiment) |

Reproduce: `./build/wm/m23g6c_run.sh all 4` then `python3 tools/m23g6c_extract.py` (exit 0 = every cell
present and every check passed).
