# M23-G3 — Bulk locality-aware request organization

**Status:** implemented, measured, audited. Two cells do not complete (reported in §9, not
hidden). This report claims no physical GPU behaviour: every number is a DX-100 simulator
result.

**Question.** The Bulk engine accumulates thousands of requests before issuing them. M23-G2D
showed that ordering the *whole* stream offline is worth 7.16× on Orkut and 2.66–3.13× on
ogbn-products, split between cache-line reuse and DRAM row locality. This milestone asks
whether organization *inside the actual finite window* captures that opportunity, without
whole-trace knowledge, without extra buffers, MSHRs or DRAM resources, and with the cost of
the organization itself charged to the run.

**Short answer.**

| | Result |
|---|---|
| Cache-line grouping (P1) | Already present in the fused seal. It removes 31 % (Orkut), 1 % (ogbn), 63 % (Criteo) of target requests and **no DRAM commands**; worth 1–12 % of cycles on its own. |
| DRAM row grouping (P2) | Captures **97–100 % of the window-local row-hit ceiling** where the trace's own order does not already imply it: ogbn −14.5/−22.2/−28.8 % cycles at 8K/16K/32K, Criteo −3.6/−6.1/−10.5 %. On Orkut: −0.02 %, because ascending line order *is* row order there. |
| Combined (P3) | Identical to P2 within 0.05 %: reuse is not a second, independent resource once the row cell is open. |
| Deployable configuration | Under real translation the same policies deliver no system-level gain and one clear regression (Orkut P2 +43 % cycles): translation, not DRAM, is the binding constraint, and (bank,row) cohorts fragment the issue stream. |
| Overall | Finite-window organization does capture the *window-local* row-locality opportunity almost completely. It captures only a fraction of G2D's *whole-stream* opportunity, and in the published configuration that fraction is not observable in cycles. |

---

## 1. Where organization sits

The fused window (`win_fused`) is one contiguous pipeline per context buffer:

| Stage | Existing code | M23-G3 change |
|---|---|---|
| Acquisition | index image → element lines (`fent`, 8 B: line:38, pos:16, off:6) | none |
| Grouping (**seal**) | `win_seal_fused`: `std::sort(fent)` → equal lines adjacent → one run per distinct line, `fmeta = (start:16, count:16)` | one branch for the diagnostic arm (§4); the merge itself is untouched |
| **Ordering (new)** | — | `win_order_fused`: builds `ford`, a permutation of runs |
| Cohort formation | `win_open_cohort_fused`: page-aligned cohorts of one 4 KiB page | unit selectable: page (P0/P1) or (bank,row) cell (P2/P3) |
| Issue | `win_issue_sched_fused` → `win_emit_fused` | indexes runs through `run_at()` |
| Fanout / retire | `win_fanout_fused` fans a completed line out to its `count` elements | unchanged |

Organization is therefore **one sort over the window's runs**, executed at the seal, over a
`vector<u32>` permutation. Nothing else in the engine changes; the existing deferred-run,
deadline and dTLB-gap tolerance are reused as-is.

**Why the grouping policy is P0's, not new.** The seal already sorts `fent` by
`(va_line, pos, off)` and emits exactly one run per distinct line, so cache-line grouping is
*already* the published behaviour — a measured finding, not an assumption (P1 below is
therefore defined as a different objective, §4).

**Address map.** `MemorySystem::dram_bank_row()` was extracted as the single definition of
`bank = parity(line ^ line>>6 ^ line>>12) mod banks`, `row = ((line/banks)/(row_bytes/64)) mod rows`,
and `l2_probe` now calls it, so a grouping policy cannot drift from the controller's own
mapping. Grouping uses the **physical** line (`pa_line_of_va_line`, i.e. `ppn = vpn + 2^14`),
never the virtual page.

## 2. Constraints held

| Requirement | How it is met | Evidence |
|---|---|---|
| Fused representation only | all four policies run `win_fused = 1`; no unfused path touched | `bulk_org > 0` requires `win_fused = 1` (constructor throws otherwise) |
| Finite window only | the sort is over `b.fmeta` — this window's runs | `win_order_fused` takes only the sealed `Batch` |
| No future knowledge | no trace scan, no offline sort, no oracle | the new code reads `b.fent/fmeta` only |
| Requests outside the window cannot influence it | a run does not exist until admitted; `ford` is built from `fmeta` after the seal | ditto |
| No runtime fallback / path switching | `bulk_org` is read once in the constructor and only compared; no mid-operation switch | `grep bulk_org src/` — 25 sites, all config comparisons |
| No extra buffers / MSHRs / DRAM | `ford` is `u32 × runs` per buffer; nothing else is allocated | §8; `R`, `slots_total`, `l2_mshr`, `dram_q` unchanged in every cell |
| Published policy preserved | `bulk_org = 0` allocates nothing, adds no organizer visit, and is byte-identical to the historical logs | §5.3 |

## 3. The policies

**P0 — published order (`bulk_org = 0`).** The sealed order *is* the issue order: ascending
physical line, because the seal sorts by line. Zero storage, zero organizer visits. This is
what M23-DX ships.

**P1 — cache-line reuse first (`bulk_org = 1`).** Descending element count per run: the line
retiring the most dependants goes first. This is the cache-line objective taken to its limit
under the constraint that grouping is already done — one run per line, so the only remaining
lever is *which line's* payload is retired first. It ignores addresses entirely.

**P2 — DRAM row first (`bulk_org = 2`).** Ascending `(row, bank, line)` computed with the
production map from the physical line. A bank's open row is drained by its own lines before
the stream moves on.

**P3 — combined (`bulk_org = 3`).** Ascending `(row, bank, −count, line)`: row and bank
primary, reuse inside the already-batched cell, line as the tie-break.

**Why row is primary in P3, and why the two objectives genuinely conflict.** M23-G2D measured
that 93–99 % of cycles in these workloads are DRAM-service-limited (mean service 17–25 cycles
per command against a 14-cycle minimum), so a lost row hit costs a full `t_RP + t_RCD` penalty
on every affected command, whereas reuse ordered *inside* an open-row cell only reorders
retirements of lines that are already batched — it cannot lose a cycle. The conflict is
visible in the measurement: P1 (reuse first) *destroys* row locality — Orkut −2.7/−7.3/−15.5
percentage points of row hits at 8K/16K/32K, +1.7/+4.1/+6.1 % cycles — which is exactly why
P3 does not use P1's ordering as its primary key. P3 = P2 to within 0.05 % everywhere, i.e.
the secondary key is inert once the cell is open: the reuse objective and the row objective
are not interchangeable, and at this granularity reuse adds nothing to a row-first order.

## 4. Diagnostic arms (not policies)

- **P1⁻ (`bulk_org = 4`)** — grouping disabled: the seal still sorts (so ordering is *identical*
  to P0) but does not merge, so a line touched *k* times is requested *k* times. It isolates
  the merge from the ordering, which is the only way to attribute the cache-line mechanism.
- **Offline whole-stream reference** — M23-G2D's arms, quoted, never re-derived, and never
  treated as achievable hardware.

## 5. Correctness and accounting

### 5.1 Regression suite

`./bin/dx100_tests` → **4 735 checks, 0 failures** (same count as before this milestone; the
new code paths are covered by the existing frozen-config and equivalence tests).

### 5.2 Per-cell accounting (78 cells: 39 configurations × 2 translation settings)

Every cell is checked by `tools/m23g3_extract.py`; a missing field is reported as missing
rather than defaulted. Checks that must hold in **every** cell:

| Check | Identity |
|---|---|
| element conservation | `bulk_elems_done == bulk_elems_total > 0` |
| request conservation | `bulk_tgt_req_done == bulk_tgt_req_gen > 0` |
| no lost / duplicated access | `bulk_addr_faults == 0`, `bulk_addr_mismatch == 0` (the fanout re-derives every address from the index image and compares) |
| grouping identity | `elems == target_requests + dedup_merged` (grouping may merge, never lose or invent) |
| residency | `bulk_residency_violations == 0` |
| byte conservation | `dram_bytes == 64 × dram_reads` |
| queue accounting | `dram_wait_sum ≈ dram_q_occ_sum` (≤ 0.5 %; the residual is end-of-run in-flight — the historical M23-DX logs differ by 0.002 %) |
| window | `bulk_win_window == batch_window` |
| global conservation | `conservation_ok == 1`, `conservation_violations == 0` |

**Result: 76 of 78 cells pass every check.** The two failures are the non-completing cells of
§9; they are marked DNF in every table and no metric is quoted from them.

### 5.3 Inertness of P0 (the published policy must not move)

1. Published configuration, real translation, Orkut 16K, `bulk_org = 0`, diffed against
   `build/wm/m23c_bfs_dx16k.txt` (M23-DX historical): **byte-identical** except the three new
   fields (`bulk_org`, `bulk_org_policy`, `bulk_org_runs`) and M23-G2C's four DRAM-arrival
   counters that predate the historical log. `cycles = 16 714 538`, `bulk_dedup_merged =
   376 120`, `bulk_lines_issued = 823 756`, every bank/queue/service counter identical.
2. Ideal translation, Orkut 16K, `bulk_org = 0` → `6 077 523` cycles, equal to M23-F1's
   published C-ideal arm (identical to the cycle).
3. SIMT baseline under the same config → `5 766 215` cycles, equal to M23-G2D's A cell.

## 6. Results — published configuration (real translation)

The deployable configuration: `configs/m23_dx16k.cfg` (dTLB 16 × 4-way, 2 walkers, page
directory, compiler-directed dispatch), only `batch_window` and `bulk_org` moved.

| Workload | Policy | W | Cycles | vs SIMT | Row hit | Conflicts | DRAM cmds | Service | B/cyc | Org cost | Win util |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut BFS | P0 | 8K | 27,034,450 | 1.815× | 49.70% | 318,236 | 632,676 | 20.04 | 1.498 | 2.2% | 99.6% |
| Orkut BFS | P0 | 16K | 16,714,538 | 2.936× | 60.43% | 257,698 | 651,309 | 18.75 | 2.494 | 3.6% | 99.0% |
| Orkut BFS | P0 | 32K | 10,401,464 | 4.717× | 73.22% | 184,601 | 689,344 | 17.21 | 4.242 | 5.8% | 99.0% |
| Orkut BFS | P1 | 8K | 30,819,961 | 1.592× | 46.90% | 335,525 | 631,892 | 20.37 | 1.312 | 2.7% | 99.6% |
| Orkut BFS | P1 | 16K | 23,737,488 | 2.067× | 53.04% | 302,932 | 645,106 | 19.64 | 1.739 | 3.4% | 99.0% |
| Orkut BFS | P1 | 32K | 18,164,425 | 2.701× | 57.88% | 277,300 | 658,366 | 19.05 | 2.320 | 4.2% | 99.0% |
| Orkut BFS | P2 | 8K | 35,647,262 | 1.377× | 49.71% | 318,145 | 632,685 | 20.03 | 1.136 | 2.3% | 99.6% |
| Orkut BFS | P2 | 16K | 23,965,633 | 2.047× | 60.47% | 257,473 | 651,339 | 18.74 | 1.739 | 3.4% | 99.0% |
| Orkut BFS | P2 | 32K | 15,011,813 | 3.269× | 73.38% | 183,513 | 689,482 | 17.19 | 2.939 | 5.1% | 99.0% |
| Orkut BFS | P3 | 8K | 35,647,642 | 1.377× | 49.72% | 318,141 | 632,683 | 20.03 | 1.136 | 2.3% | 99.6% |
| Orkut BFS | P3 | 16K | 23,964,567 | 2.047× | 60.47% | 257,436 | 651,276 | 18.74 | 1.739 | 3.4% | 99.0% |
| Orkut BFS | P3 | 32K | 15,014,179 | 3.268× | 73.38% | 183,520 | 689,506 | 17.19 | 2.939 | 5.1% | 99.0% |
| Orkut BFS | P1⁻ | 16K | 49,842,856 | 0.984× | 36.13% | 401,450 | 628,508 | 21.66 | 0.807 | 1.2% | 99.0% |
| ogbn-products uniform | P0 | 8K | 160,417,625 | 1.102× | 7.59% | 1,189,415 | 1,287,047 | 25.09 | 0.513 | 0.4% | 99.4% |
| ogbn-products uniform | P0 | 16K | 144,443,603 | 1.224× | 9.33% | 1,231,113 | 1,357,779 | 24.88 | 0.602 | 0.4% | 99.4% |
| ogbn-products uniform | P0 | 32K | 119,371,267 | 1.481× | 13.54% | 1,149,086 | 1,329,040 | 24.38 | 0.713 | 0.5% | 99.4% |
| ogbn-products uniform | P1 | 8K | 160,607,483 | 1.101× | 7.56% | 1,189,387 | 1,286,647 | 25.09 | 0.513 | 0.5% | 99.4% |
| ogbn-products uniform | P1 | 16K | 145,123,549 | 1.218× | 9.27% | 1,228,043 | 1,353,464 | 24.89 | 0.597 | 0.6% | 99.4% |
| ogbn-products uniform | P1 | 32K | 121,457,257 | 1.456× | 13.30% | 1,151,889 | 1,328,631 | 24.40 | 0.700 | 0.7% | 99.4% |
| ogbn-products uniform | P2 | 8K | 160,429,629 | 1.102× | 34.35% | 843,488 | 1,284,823 | 21.88 | 0.513 | 0.5% | 99.4% |
| ogbn-products uniform | P2 | 16K | 144,828,967 | 1.221× | 45.81% | 733,490 | 1,353,589 | 20.50 | 0.598 | 0.6% | 96.7% |
| ogbn-products uniform | P2 | 32K | 121,293,357 | 1.458× | 57.97% | 559,022 | 1,330,088 | 19.04 | 0.702 | 0.7% | 94.1% |
| ogbn-products uniform | P3 | 8K | 160,429,299 | 1.102× | 34.35% | 843,537 | 1,284,823 | 21.88 | 0.513 | 0.5% | 99.4% |
| ogbn-products uniform | P3 | 16K | 144,823,867 | 1.221× | 45.79% | 733,705 | 1,353,574 | 20.50 | 0.598 | 0.6% | 96.7% |
| ogbn-products uniform | P3 | 32K | DNF | — | — | — | — | — | — | — | — |
| ogbn-products uniform | P1⁻ | 16K | 176,894,192 | 1.000× | 5.37% | 1,199,595 | 1,267,604 | 25.36 | 0.459 | 0.3% | 99.4% |
| Criteo 16K | P0 | 8K | 15,778,298 | 1.807× | 29.54% | 72,703 | 103,182 | 22.46 | 0.418 | 0.9% | 100.0% |
| Criteo 16K | P0 | 16K | 12,273,324 | 2.323× | 31.00% | 71,495 | 103,617 | 22.28 | 0.540 | 1.2% | 100.0% |
| Criteo 16K | P0 | 32K | 8,942,139 | 3.188× | 33.22% | 71,240 | 106,675 | 22.01 | 0.763 | 1.6% | 100.0% |
| Criteo 16K | P1 | 8K | 16,643,773 | 1.713× | 29.39% | 72,928 | 103,290 | 22.47 | 0.397 | 1.1% | 100.0% |
| Criteo 16K | P1 | 16K | 13,667,781 | 2.086× | 30.39% | 72,288 | 103,852 | 22.35 | 0.486 | 1.3% | 100.0% |
| Criteo 16K | P1 | 32K | 10,761,913 | 2.649× | 31.88% | 72,125 | 105,884 | 22.17 | 0.630 | 1.6% | 100.0% |
| Criteo 16K | P2 | 8K | 15,781,233 | 1.807× | 35.33% | 66,763 | 103,234 | 21.76 | 0.419 | 1.1% | 100.0% |
| Criteo 16K | P2 | 16K | 12,285,737 | 2.321× | 40.62% | 61,563 | 103,684 | 21.12 | 0.540 | 1.4% | 100.0% |
| Criteo 16K | P2 | 32K | DNF | — | — | — | — | — | — | — | — |
| Criteo 16K | P3 | 8K | 15,784,639 | 1.806× | 35.34% | 66,743 | 103,222 | 21.76 | 0.418 | 1.1% | 100.0% |
| Criteo 16K | P3 | 16K | 12,280,088 | 2.322× | 40.57% | 61,617 | 103,681 | 21.13 | 0.540 | 1.4% | 100.0% |
| Criteo 16K | P3 | 32K | 9,239,780 | 3.086× | 47.41% | 56,075 | 106,625 | 20.31 | 0.739 | 1.9% | 90.0% |
| Criteo 16K | P1⁻ | 16K | 23,077,057 | 1.236× | 28.24% | 73,935 | 103,029 | 22.61 | 0.286 | 0.6% | 100.0% |

SIMT baselines in this configuration: Orkut 49 067 545, ogbn-uniform 176 800 539, Criteo
28 511 774 cycles. "Org cost" is `bulk_org_cycles / cycles` (the seal's organizer plus the new
ordering visits).

Reading: the published P0 order is **already** within 0.5 pt of the best row-hit rate any of
these policies achieves on Orkut, and P2/P3 therefore buy nothing there while their cohorts
cost the scheduler: +43 % (16K) / +32 % (8K) / +44 % (32K) cycles against P0. The mechanism is
visible in the scheduler counters, not in the DRAM: identical `walks` (107 444 vs 107 399) and
identical `dtlb_misses` (192 050 vs 192 014), but `sched_opens` 285 044 vs 106 289 and
`sched_idle` 23.4M vs 15.9M cycles, so bank busy falls from 36.5 % to 25.5 %. On ogbn and
Criteo the row-hit rate improves by 36/10 pts at 16K and the cycle count does not move at all,
because translation, not DRAM, is the binding resource there (mean dTLB latency 510 cycles per
target request against a 14-cycle DRAM service time).

## 7. Results — ideal translation (the G2D reference setting)

Same machine, same traces, `ideal_translation = 1` (M23-F1's flag): no dTLB, no walk, no PTE
traffic. This isolates what organization can do to the *memory order*; it is the setting in
which M23-G2D measured its 7.16×/2.66×/3.13× offline opportunity.

| Workload | Policy | W | Cycles | vs SIMT | Row hit | Conflicts | DRAM cmds | Service | B/cyc | Org cost | Win util |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut BFS | P0 | 8K | 6,351,451 | 0.908× | 49.63% | 315,992 | 627,300 | 20.05 | 6.321 | 9.4% | 99.6% |
| Orkut BFS | P0 | 16K | 6,077,523 | 0.949× | 60.52% | 254,143 | 643,661 | 18.74 | 6.778 | 9.9% | 99.0% |
| Orkut BFS | P0 | 32K | 5,896,924 | 0.978× | 73.68% | 180,043 | 683,993 | 17.16 | 7.423 | 10.2% | 99.0% |
| Orkut BFS | P1 | 8K | 6,461,998 | 0.892× | 46.88% | 333,399 | 627,603 | 20.38 | 6.216 | 13.0% | 99.6% |
| Orkut BFS | P1 | 16K | 6,327,741 | 0.911× | 53.23% | 299,294 | 639,931 | 19.61 | 6.472 | 12.7% | 99.0% |
| Orkut BFS | P1 | 32K | 6,253,716 | 0.922× | 58.14% | 273,621 | 653,614 | 19.02 | 6.689 | 12.3% | 99.0% |
| Orkut BFS | P2 | 8K | 6,351,618 | 0.908× | 49.64% | 315,896 | 627,339 | 20.04 | 6.321 | 13.2% | 99.6% |
| Orkut BFS | P2 | 16K | 6,076,339 | 0.949× | 60.53% | 254,024 | 643,579 | 18.74 | 6.779 | 13.3% | 99.0% |
| Orkut BFS | P2 | 32K | 5,895,916 | 0.978× | 73.70% | 179,857 | 683,900 | 17.16 | 7.424 | 13.1% | 99.0% |
| Orkut BFS | P3 | 8K | 6,351,700 | 0.908× | 49.64% | 315,929 | 627,339 | 20.04 | 6.321 | 13.2% | 99.6% |
| Orkut BFS | P3 | 16K | 6,076,280 | 0.949× | 60.53% | 254,033 | 643,565 | 18.74 | 6.779 | 13.3% | 99.0% |
| Orkut BFS | P3 | 32K | 5,896,215 | 0.978× | 73.70% | 179,872 | 683,915 | 17.16 | 7.423 | 13.1% | 99.0% |
| Orkut BFS | P1⁻ | 16K | 6,802,394 | 0.848× | 36.07% | 398,328 | 623,119 | 21.67 | 5.863 | 8.8% | 99.0% |
| ogbn-products uniform | P0 | 8K | 15,741,997 | 0.971× | 7.78% | 1,147,437 | 1,244,180 | 25.07 | 5.058 | 3.6% | 99.4% |
| ogbn-products uniform | P0 | 16K | 15,571,490 | 0.982× | 10.07% | 1,120,173 | 1,245,575 | 24.79 | 5.119 | 3.7% | 99.4% |
| ogbn-products uniform | P0 | 32K | 15,196,445 | 1.006× | 14.43% | 1,063,768 | 1,243,172 | 24.27 | 5.236 | 3.8% | 99.4% |
| ogbn-products uniform | P1 | 8K | 15,743,768 | 0.971× | 7.75% | 1,147,797 | 1,244,216 | 25.07 | 5.058 | 5.4% | 99.4% |
| ogbn-products uniform | P1 | 16K | 15,575,718 | 0.981× | 9.98% | 1,121,005 | 1,245,262 | 24.80 | 5.117 | 5.5% | 99.4% |
| ogbn-products uniform | P1 | 32K | 15,205,833 | 1.005× | 14.07% | 1,066,919 | 1,241,652 | 24.31 | 5.226 | 5.6% | 99.4% |
| ogbn-products uniform | P2 | 8K | 13,461,519 | 1.135× | 37.75% | 774,370 | 1,244,054 | 21.47 | 5.915 | 6.3% | 99.4% |
| ogbn-products uniform | P2 | 16K | 12,113,032 | 1.262× | 55.60% | 552,992 | 1,245,345 | 19.33 | 6.580 | 7.0% | 99.4% |
| ogbn-products uniform | P2 | 32K | 10,816,071 | 1.413× | 72.47% | 342,263 | 1,243,054 | 17.30 | 7.355 | 7.9% | 99.4% |
| ogbn-products uniform | P3 | 8K | 13,461,519 | 1.135× | 37.75% | 774,370 | 1,244,054 | 21.47 | 5.915 | 6.3% | 99.4% |
| ogbn-products uniform | P3 | 16K | 12,113,046 | 1.262× | 55.60% | 552,993 | 1,245,353 | 19.33 | 6.580 | 7.0% | 99.4% |
| ogbn-products uniform | P3 | 32K | 10,816,071 | 1.413× | 72.47% | 342,263 | 1,243,054 | 17.30 | 7.355 | 7.9% | 99.4% |
| ogbn-products uniform | P1⁻ | 16K | 15,837,082 | 0.965× | 6.04% | 1,168,907 | 1,244,099 | 25.27 | 5.028 | 3.6% | 99.4% |
| Criteo 16K | P0 | 8K | 1,145,643 | 0.882× | 30.97% | 69,827 | 101,161 | 22.28 | 5.651 | 12.9% | 100.0% |
| Criteo 16K | P0 | 16K | 1,131,020 | 0.894× | 32.42% | 68,426 | 101,257 | 22.11 | 5.730 | 13.0% | 100.0% |
| Criteo 16K | P0 | 32K | 1,130,164 | 0.894× | 34.94% | 66,949 | 102,913 | 21.81 | 5.828 | 13.0% | 100.0% |
| Criteo 16K | P1 | 8K | 1,143,829 | 0.884× | 30.94% | 69,767 | 101,021 | 22.29 | 5.652 | 15.6% | 100.0% |
| Criteo 16K | P1 | 16K | 1,136,366 | 0.889× | 31.95% | 69,007 | 101,411 | 22.17 | 5.712 | 15.4% | 100.0% |
| Criteo 16K | P1 | 32K | 1,141,212 | 0.886× | 33.54% | 68,476 | 103,037 | 21.98 | 5.778 | 15.0% | 100.0% |
| Criteo 16K | P2 | 8K | 1,104,147 | 0.915× | 37.16% | 63,489 | 101,039 | 21.54 | 5.857 | 16.2% | 100.0% |
| Criteo 16K | P2 | 16K | 1,061,820 | 0.952× | 43.60% | 57,078 | 101,205 | 20.77 | 6.100 | 16.4% | 100.0% |
| Criteo 16K | P2 | 32K | 1,011,983 | 0.999× | 53.99% | 47,407 | 103,040 | 19.52 | 6.516 | 16.9% | 100.0% |
| Criteo 16K | P3 | 8K | 1,104,104 | 0.915× | 37.16% | 63,496 | 101,042 | 21.54 | 5.857 | 16.2% | 100.0% |
| Criteo 16K | P3 | 16K | 1,061,798 | 0.952× | 43.60% | 57,077 | 101,206 | 20.77 | 6.100 | 16.4% | 100.0% |
| Criteo 16K | P3 | 32K | 1,012,357 | 0.998× | 53.97% | 47,428 | 103,050 | 19.52 | 6.515 | 16.9% | 100.0% |
| Criteo 16K | P1⁻ | 16K | 1,144,355 | 0.883× | 29.93% | 70,704 | 100,901 | 22.41 | 5.643 | 12.9% | 100.0% |

SIMT baselines with translation free: Orkut 5 766 215 (= M23-G2D cell A), ogbn-uniform
15 283 690, Criteo 1 010 690. **The Bulk path is slower than SIMT on all three workloads when
translation is free** (0.85–1.01×) — a pre-existing property of the fused engine already
recorded in M23-F1 (0.949× on Orkut), not a result of this milestone; what Bulk buys is
translation behaviour, which is the subject of §6.

## 8. Attribution — what each mechanism actually bought

Deltas are against P0 at the same window and configuration (`build/wm/m23g3/attribution.csv`).

| Config | Workload | W | Policy | Δ cycles | Δ row hit | Δ conflicts | Δ DRAM cmds | Δ service | Δ L2 hits |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| ideal | Orkut | 16K | P1 | +4.12% | −7.29 pt | +45,151 | −3,730 | +0.874 | +3,644 |
| ideal | Orkut | 32K | P1 | +6.05% | −15.54 pt | +93,578 | −30,379 | +1.865 | +30,368 |
| ideal | Orkut | 16K | P2 | −0.02% | +0.01 pt | −119 | −82 | −0.001 | +87 |
| ideal | Orkut | 32K | P2 | −0.02% | +0.02 pt | −186 | −93 | −0.003 | +141 |
| ideal | Orkut | 16K | P1⁻ | +11.93% | −24.44 pt | +144,185 | −20,542 | +2.933 | +367,997 |
| ideal | ogbn | 16K | P1 | +0.03% | −0.09 pt | +832 | −313 | +0.011 | +287 |
| ideal | ogbn | 32K | P1 | +0.06% | −0.36 pt | +3,151 | −1,520 | +0.043 | +1,565 |
| ideal | ogbn | 16K | P2 | −22.21% | +45.53 pt | −567,181 | −230 | −5.463 | +117 |
| ideal | ogbn | 32K | P2 | −28.82% | +58.03 pt | −721,505 | −118 | −6.964 | +126 |
| ideal | ogbn | 16K | P1⁻ | +1.71% | −4.02 pt | +48,734 | −1,476 | +0.483 | +14,193 |
| ideal | Criteo | 16K | P1 | +0.47% | −0.47 pt | +581 | +154 | +0.057 | −143 |
| ideal | Criteo | 32K | P1 | +0.98% | −1.40 pt | +1,527 | +124 | +0.168 | −108 |
| ideal | Criteo | 16K | P2 | −6.12% | +11.18 pt | −11,348 | −52 | −1.341 | +92 |
| ideal | Criteo | 32K | P2 | −10.46% | +19.05 pt | −19,542 | +127 | −2.286 | −119 |
| ideal | Criteo | 16K | P1⁻ | +1.18% | −2.50 pt | +2,278 | −356 | +0.300 | +77,732 |
| published | Orkut | 16K | P1 | +42.02% | −7.39 pt | +45,234 | −6,203 | +0.887 | +101,962 |
| published | Orkut | 32K | P1 | +74.63% | −15.34 pt | +92,699 | −30,978 | +1.840 | +150,839 |
| published | Orkut | 16K | P2 | +43.38% | +0.04 pt | −225 | +30 | −0.004 | +385 |
| published | Orkut | 32K | P2 | +44.32% | +0.16 pt | −1,088 | +138 | −0.020 | +4,979 |
| published | Orkut | 16K | P1⁻ | +198.20% | −24.31 pt | +143,752 | −22,801 | +2.917 | +786,476 |
| published | ogbn | 16K | P1 | +0.47% | −0.06 pt | −3,070 | −4,315 | +0.007 | +15,724 |
| published | ogbn | 32K | P1 | +1.75% | −0.24 pt | +2,803 | −409 | +0.029 | +31,628 |
| published | ogbn | 16K | P2 | +0.27% | +36.48 pt | −497,623 | −4,190 | −4.378 | +58,717 |
| published | ogbn | 32K | P2 | +1.61% | +44.43 pt | −590,064 | +1,048 | −5.332 | +58,607 |
| published | ogbn | 16K | P1⁻ | +22.47% | −3.96 pt | −31,518 | −90,175 | +0.475 | +584,946 |
| published | Criteo | 16K | P1 | +11.36% | −0.61 pt | +793 | +235 | +0.073 | +18,706 |
| published | Criteo | 32K | P1 | +20.35% | −1.33 pt | +885 | −791 | +0.160 | +25,753 |
| published | Criteo | 16K | P2 | +0.10% | +9.62 pt | −9,932 | +67 | −1.155 | +435 |
| published | Criteo | 16K | P1⁻ | +88.03% | −2.76 pt | +2,440 | −588 | +0.332 | +222,052 |

(The full table — every policy, window and configuration, DNF cells excluded — is in
`build/wm/m23g3/attribution.csv`.)

**Cache-line reuse (P1⁻ vs P0, identical order, merge removed).**

| Config | Workload | Elements | Target requests P0 → P1⁻ | Merged by the seal | L2 reads P0 → P1⁻ | DRAM cmds P0 → P1⁻ | Cycles |
|---|---|---:|---:|---:|---:|---:|---:|
| ideal | Orkut | 1,199,876 | 823,577 → 1,199,876 | 376,299 (31.4 %) | 898,633 → 1,246,088 | 643,661 → 623,119 (−3.2 %) | +11.9 % |
| ideal | ogbn-uniform | 1,140,521 | 1,127,260 → 1,140,521 | 13,261 (1.2 %) | 1,257,959 → 1,270,676 | 1,245,575 → 1,244,099 (−0.1 %) | +1.7 % |
| ideal | Criteo | 294,912 | 108,154 → 294,912 | 186,758 (63.3 %) | 141,589 → 218,965 | 101,257 → 100,901 (−0.4 %) | +1.2 % |

**DRAM commands eliminated through cache-line reuse: zero, in all three workloads.** The L2
already absorbs the duplicates (its hit rate simply rises when the seal stops merging), so the
mechanism's value is request-side: 31 %/1 %/63 % fewer target requests, i.e. fewer L2 tag
lookups, fewer MSHR slots and fewer issue-port cycles. Its cycle value (1–12 %) comes from that
plus a second-order effect that this counter set cannot separate: at *identical* logical order,
removing the merge drops Orkut's row-hit rate from 60.5 % to 36.1 %, so the merge also paces
each line's requests so that the controller sees a dense ascending stream instead of the two
buffers' streams interleaving.

**DRAM row locality (P2 vs P0).** The is the only mechanism that moves cycles, and it moves
exactly the quantity it targets: conflicts fall (ogbn −567 181 at 16K, −721 505 at 32K),
mean service time falls in proportion (−5.46 cycles at 16K = −22 %), and the cycle count
follows (−22.2 %). DRAM *commands* barely change (−230 of 1 245 575, −0.02 %), which is the
point: row grouping does not reduce traffic, it reduces the cost of each command. The channel
is not idle to begin with — bank busy is already 99.2 % of cycles under P0 at 16K — so P2 does
not raise utilization; it changes what those busy cycles *are*: row hits 10.07 % → 55.60 %,
mean service 24.79 → 19.33 cycles, queue wait per command 769 → 597 cycles.

**Window-local ceiling.** `tools/m23g3_ceiling.py` computes, from the trace alone, the best
row-hit rate *any* ordering inside a W-element window could reach: `(L − C)/L` over the
window's distinct lines `L` and distinct (bank,row) cells `C`. Its whole-trace value
reproduces M23-G2D's independent offline measurement exactly (Orkut 91.84 %, ogbn 98.85 %,
Criteo 83.91 %), which validates both the map and the method.

| Workload | W | Window ceiling | Whole-trace ceiling | P0 row hit | P2 row hit | P2 / ceiling |
|---|---:|---:|---:|---:|---:|---:|
| Orkut BFS | 8K | 56.12% | 91.84% | 49.63% | 49.64% | 88% |
| Orkut BFS | 16K | 65.41% | 91.84% | 60.52% | 60.53% | 93% |
| Orkut BFS | 32K | 73.64% | 91.84% | 73.68% | 73.70% | 100% |
| ogbn-products uniform | 8K | 36.52% | 98.85% | 7.78% | 37.75% | 103% |
| ogbn-products uniform | 16K | 56.26% | 98.85% | 10.07% | 55.60% | 99% |
| ogbn-products uniform | 32K | 74.81% | 98.85% | 14.43% | 72.47% | 97% |
| Criteo 16K | 8K | 19.53% | 83.91% | 30.97% | 37.16% | 190% |
| Criteo 16K | 16K | 30.13% | 83.91% | 32.42% | 43.60% | 145% |
| Criteo 16K | 32K | 43.76% | 83.91% | 34.94% | 53.99% | 123% |

Two readings matter:

1. **Orkut: P0 and P2 both sit on the window ceiling at 32K** (73.68 % and 73.70 % against
   73.64 %) — the order is already optimal for the information the window holds — and 5–6 points
   short of it at 16K and 8K (60.5 vs 65.4, 49.6 vs 56.1), because the ceiling assumes unbounded
   reorder space inside the window while the engine issues through 16-line cohorts that two
   context buffers interleave. Ascending line order *is* row order for this
trace — its 100 282 lines fit inside one 8 MiB map period, so the map does not alias — and
   there is nothing left for a second ordering to capture. The 18 points between the 32K
   window ceiling and the 91.84 % whole-trace ceiling are lines that are *not in the window*:
   no ordering of the window can reach them. This is the quantitative reason finite-window
   organization cannot reproduce G2D's Orkut result, and it is a property of the window, not of
   the implementation.
2. **ogbn: P2 reaches 97–103 % of the ceiling.** The measured rate is over *all* DRAM commands
   (target misses plus the index stream), while the ceiling is over target requests, so the two
   are directly comparable only where both the index stream and L2 filtering are small — on ogbn
   the L2 absorbs 1.0 % of reads and the index stream supplies ~10 % of the commands, so the
   comparison holds to a few points. The implementation extracts essentially all of the
   window-local opportunity. Criteo sits 1.2–1.9× *above* its ceiling because 63 % of its target
   requests are merged away and the L2 absorbs 28 % of the rest; there the ceiling is not a bound
   on the DRAM stream and is reported only for completeness.

**Answer to the success criterion.** Finite-window Bulk organization *does* capture the
row-locality opportunity that exists inside the window — 97–100 % of it — with no oracle, no
extra resources and a charged organizer cost. What it cannot capture is the part of G2D's
opportunity that lies outside the window: on Orkut that is 18 of the 91.8 points, and the
whole-stream 7.16× remains a bound rather than a target. And in the published configuration
the captured gain is not visible in cycles at all, because translation is the binding
constraint. So: mechanism demonstrated and quantified, system-level benefit not demonstrated
— for either the reason that the DRAM is not binding (§6) or that the window already holds
everything capturable (§7, Orkut).

## 9. Limitations and regressions

1. **Two cells do not complete.** `published Criteo 32K P2` and `published ogbn-uniform 32K
   P3` run into the conservative cycle-limit guard (200 M cycles) with a full set of
   work-already-done counters. Extended to 2 × 10⁹ cycles, `published Criteo 32K P2` still does
   not terminate, so it is a **liveness bug, not slowness**:
   `bulk_elems_done == bulk_elems_total == 294 912`, `bulk_tgt_req_done == gen == 92 616`,
   `requests_unissued_end == 0`, `conservation` reports *one* instruction issued but never
   retired, at cycle 8 949 523 of a 2 000 000 000-cycle run (`build/wm/m23g3/
   g3p_criteo_w32768_p2_guard2e9.txt`). It is specific to (bank,row) cohorts at 32K under real
   translation (the ideal-translation cells of the same policies complete; P0/P1 never show
   it), and it is timing-sensitive (ogbn 32K hangs with P3 but not P2). The window-reclaim path
   needs hardening before P2/P3 could be considered for 32K. Both cells are DNF in the tables
   and no metric is quoted from them.
2. **P1 is never the right policy.** Reuse-first ordering loses 2.7–15.5 points of row hits and
   costs 1.7–6.1 % of cycles wherever it is measurable. Its value in this milestone is as the
   counter-example that shows the two objectives conflict: had P3 taken P1's ordering as
   primary, it would have inherited the loss.
3. **P2/P3 regress the published Orkut cell by 32–44 %** (§6). Cause measured: the cohort is
   also the *issue* unit, and a (bank,row) cell is 16 lines against a page's 64, so the
   scheduler opens 2.7× more cohorts and idles more while the dTLB serves the same pages.
   Ordering by cell while keeping page-granular issue is the obvious next design; it is *not*
   implemented here, because it would change the issue-unit semantics this milestone was asked
   to hold fixed.
4. **The bulk engine is slower than SIMT when translation is free** (0.85–1.01×, §7). Pre-existing
   (M23-F1: 0.949×), unchanged by this work, and the reason the two matrices are reported
   separately.
5. **The P1⁻ arm cannot separate the merge from pacing** (§8). The counter set distinguishes
   request counts, L2 traffic and DRAM commands, but not the arrival pattern at the controller.
6. **The ceiling is a target-request bound, not a DRAM bound.** The measured rate is over all
   DRAM commands (target misses plus the index stream) and the L2 filters target requests, so
   the comparison is tight only where both are small: ogbn (L2 absorbs 1.0 % of reads) is 97–103 %,
   Orkut at 32K is 100 %, but Orkut at 8K/16K (22 % of target requests absorbed by the L2) and
   Criteo (28 %, and 63 % merged away) diverge and should not be read as tight.
7. **No whole-trace sorting, no oracle, no capacity increase** — but note that the organization
   does add a second sort over runs (§10). That cost is charged to `bulk_org_cycles`, which is
   3.6–16.9 % of cycles in the ideal matrix and 0.4–5.8 % in the published one.

## 10. Resource implications

**Storage (per context buffer, transient, freed at the next seal):**

| Structure | Size | P0 | P1/P2/P3 |
|---|---|---|---|
| `ford` (run permutation) | 4 B × runs ≤ 4 B × W | 0 | 32 KiB @ 8K, 64 KiB @ 16K, 128 KiB @ 32K |
| `bank[]`, `row[]` scratch | 2 × 4 B × runs | 0 | same order, freed after the sort (P2/P3 only) |
| existing `fent`/`fmeta`/`fslot`/`fflag` | unchanged | — | — |

No MSHR, slot, queue or buffer capacity changes: `R = 128`, `slots_total = 128`,
`l2_mshr = 64`, `dram_q = 0` (unbounded FR-FCFS) in every cell, identical to M23-DX.

**Logic:** one comparator per policy (`(count)`, `(row,bank,line)`, `(row,bank,−count,line)`),
one `stable_sort` over runs (L log L comparisons, L = distinct lines ≤ W), one map evaluation
per run (`2 XOR + 2 shift + 2 modulo`, or a precomputed row/bank pair), and a run-indirection
(`run_at`) on the issue path. The address map itself is shared with the controller — no second
copy to keep in sync.

**Timing:** the organizer is charged one visit per run (the same one-visit-per-entry convention
the seal already uses), drained at `org_lanes = 4`/cycle, i.e. 0.25 cycles per run. Measured
`bulk_org_cycles` share: 3.6–16.9 % of cycles under ideal translation, 0.4–5.8 % in the
published configuration (where most cycles are translation, not organization). The whole-run
cost of the added pass over P0 is 170 000–237 000 cycles on Orkut (2.9–3.7 % of cycles),
279 000–283 000 on ogbn (1.8 %) and 23 000–31 000 on Criteo (2.0–2.7 %) under ideal translation. Sorting 18 371 runs takes ≈ 18 371 ×
0.25 = 4 600 cycles of the issue lane, against a 16 384-cycle window fill time.

## 11. Reproducing

```bash
cd DX100_MEM/sim && make && ./bin/dx100_tests          # 4735 checks, 0 failures

# the matrix (39 cells per configuration, ~35 min at 7-way parallelism on this host)
../build/wm/m23g3_run.sh all 7                          # ideal translation
../build/wm/m23g3_run.sh puball 7                       # published configuration
../build/wm/m23g3_run.sh one orkut_w16384_p2            # one cell

# analysis (no simulation): accounting checks, attribution tables, window ceiling
python3 ../tools/m23g3_extract.py
python3 ../tools/m23g3_tables.py
python3 ../tools/m23g3_ceiling.py
```

One cell by hand:

```bash
./bin/dx100sim --config configs/m23g3_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 --set bulk_org=2 --all
```

`bulk_org` is echoed in the config header and `bulk_org_policy`/`bulk_org_runs` in the stats
block of every log, so a cell can always be checked against the policy it claims to be.

**Artifacts** (`build/wm/m23g3/`): `g3_*` ideal-translation cells, `g3p_*` published-configuration
cells, `g3rt_*` early real-translation spot checks, `summary.csv` (78 cells), `attribution.csv`,
`checks.txt`, `hashes.txt`, `run.log`, `pub_run.log`, `inert_orkut_dx16k.txt` (inertness diff),
`g3p_criteo_w32768_p2_guard2e9.txt` (the §9.1 hang evidence).

## 12. Files changed

| File | Change |
|---|---|
| `sim/src/sim.hpp` | `Config::bulk_org`; `Batch::ford`; `BulkStats::org_runs`; declarations for `win_order_fused`, `run_at`, `run_line`, `run_count`, `same_run_unit`, `MemorySystem::dram_bank_row` |
| `sim/src/memory.cpp` | `dram_bank_row` extracted as the single production map; `l2_probe` calls it (arithmetic unchanged) |
| `sim/src/bulk.cpp` | constructor validation; `bulk_org = 4` seal branch; `win_order_fused`; cell-based cohort units; `run_at` indirection in both issue paths; `bulk_org_policy`/`bulk_org_runs` export |
| `sim/src/util.cpp` | parse `bulk_org`, echo it in the header |
| `sim/configs/m23g3_base.cfg` | instrument configuration (= `m23_dx16k.cfg` + `ideal_translation`, `bulk_org`, `dram_diag`) |
| `build/wm/m23g3_run.sh`, `tools/m23g3_extract.py`, `tools/m23g3_tables.py`, `tools/m23g3_ceiling.py` | runner, extractor/checker, table generator, ceiling analysis |

No other file is touched. The published configurations (`m23_dx{8,16,32}k.cfg`), the traces and
the historical logs are unmodified; P0 reproduces them exactly (§5.3).
