# M23-G4B — Page-first, row-second Bulk organization (P4)

**Status:** implemented, measured, audited. 36 of 36 cells complete and clean; no cell was
guard-limited or non-terminating. No physical GPU behaviour is claimed: every number is a
DX-100 simulator result.

**Question.** M23-G3 showed that ordering a Bulk window by DRAM `(row, bank)` (P2) captured
97–100 % of the window-local row-locality opportunity under ideal translation, but under
published translation it delivered nothing on ogbn/Criteo and cost Orkut **+43 %** cycles.
The measured cause was structural: P2 also changed the *issue unit*, replacing a 64-line
page cohort with a 16-line `(bank,row)` cell, which multiplied cohort openings by 2.4–2.7×
and idled the issue port. P4 asks whether the same window can be organized hierarchically — page first,
row second — so that the row objective is served **without** changing the issue unit.

**Short answer.**

| | Result |
|---|---|
| Does P4 avoid the Orkut regression? | **Yes, completely.** Cohort openings are `106 289` at 16K against P0's `106 289` and P2's `285 044`; cycles `16 715 405` vs P0 `16 714 538` (**+0.005 %**) against P2's `+43.4 %`; dTLB misses and walks equal P0's. |
| Does P4 retain a DRAM gain? | **No — it retains nothing, on either workload.** Row hits move by **+0.01…+0.16 pt** (Orkut) and **±0.00 pt** (ogbn) against P0, at every window, in both translation arms. |
| Why | A page is 64 lines and a row is 16, so one page covers exactly two rows per bank. Inside a page, ascending line order *already* ascends row and alternates banks; the only order P4 can still change is the bank interleaving, and changing it moves nothing. Every point of row locality P2 harvests on ogbn lives **between** pages, and page-major ordering forbids exactly those pairs (§7). |
| Overall | P4 answers its question cleanly and negatively: the regression and the gain have the same cause — the issue unit — so a policy that keeps P0's issue unit keeps P0's behaviour *including* its lack of row locality. No further heuristic is added. |

---

## 1. The policy

`bulk_org = 5`. One `stable_sort` over the sealed window's runs, key = ascending
`(virtual page, row, bank, line)`:

- **page key** — the virtual page the access was issued against (`va_line >> 6`), i.e. exactly
  the 64-line unit the cohort and the dTLB use;
- **row/bank keys** — `MemorySystem::dram_bank_row(pa_line_of_va_line(line))`, the controller's
  own production map applied to the **physical** line, exactly as P2. Nothing assumes a
  virtual page is a physical row; a page whose PPN scatters it across rows is handled by the
  real map.
- **cohort unit — page.** `win_open_cohort_fused` selects the cell unit for `bulk_org ∈ {2,3}`
  only; P4 falls through to the published page run. This is the single line that distinguishes
  the experiment from P2, and it is the variable the milestone is about.

Ordering is over `b.ford`, the same run permutation P2 uses, so the fused representation, the
`run_at` indirection and the request accounting are untouched. Only `win_order_fused` gained a
branch, the cohort-unit predicate was narrowed from `>= 2` to `== 2 || == 3`, and the
constructor's range check widened to `0..5`.

**Constraints held.**

| Requirement | Evidence |
|---|---|
| P0 and P2 unmodified | both reproduce M23-G3's published cells to the cycle (§4) |
| No whole-trace sorting | the sort is over `b.fmeta`, this window's runs only; no trace is read |
| No extra hardware | `ford` + two scratch vectors per buffer, as P2; `R`, `slots`, `l2_mshr`, `dram_q` unchanged |
| No runtime fallback | `bulk_org` is read once in the constructor; no mid-operation path switch |
| Historical results preserved | G3's published and ideal cells reproduce exactly (§4) |

## 2. Correctness, accounting and liveness

`./bin/dx100_tests` → **4 735 checks, 0 failures** (unchanged count; the new branch is covered by
the existing frozen-config and equivalence tests).

All 36 cells pass `tools/m23g4b_extract.py`: element conservation
(`bulk_elems_done == bulk_elems_total`), request conservation
(`bulk_tgt_req_done == bulk_tgt_req_gen`), no address fault or mismatch, grouping identity
(`elems == target_requests + dedup_merged`), residency bound, byte conservation
(`dram_bytes == 64 × dram_reads`), queue identity within 0.5 %, `bulk_win_window == batch_window`,
`conservation_ok == 1`, and `bulk_org_policy == 5` in every P4 log.

**No DNF cells.** Unlike G3's cell-cohort path at 32K (two cells non-terminating), P4 completes
at 8K/16K/32K on both workloads in both translation arms.

**Inertness.** `pub Orkut 8K/16K/32K P0` = 27 034 450 / 16 714 538 / 10 401 464 and `P2` =
35 647 262 / 23 965 633 / 15 011 813, identical to M23-G3's published table; the ideal arm
reproduces G3's ideal cells to the cycle (e.g. Orkut 16K P0 6 077 523, ogbn 16K P2 12 113 032).

## 3. Results — published translation (deployable)

Window 16K is the published `m23_dx16k.cfg`; 8K/32K move only `batch_window`.

| Workload | W | Pol | Cycles | vs P0 | Row hit % | Conflicts | Service | Cmds | Opens | Idle % | Org % |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut BFS | 8K | P0 | 27,034,450 | 1.000 | 49.70 | 318,236 | 20.04 | 632,676 | 175,826 | 97.2 | 2.2 |
| Orkut BFS | 8K | P2 | 35,647,262 | 0.758 | 49.71 | 318,145 | 20.03 | 632,685 | 416,121 | 98.2 | 2.3 |
| Orkut BFS | 8K | **P4** | **27,033,828** | **1.000** | 49.71 | 318,146 | 20.03 | 632,668 | **175,826** | 97.2 | 3.1 |
| Orkut BFS | 16K | P0 | 16,714,538 | 1.000 | 60.43 | 257,698 | 18.75 | 651,309 | 106,289 | 94.9 | 3.6 |
| Orkut BFS | 16K | P2 | 23,965,633 | 0.697 | 60.47 | 257,473 | 18.74 | 651,339 | 285,044 | 97.6 | 3.4 |
| Orkut BFS | 16K | **P4** | **16,715,405** | **1.000** | 60.46 | 257,493 | 18.74 | 651,306 | **106,289** | 94.9 | 4.8 |
| Orkut BFS | 32K | P0 | 10,401,464 | 1.000 | 73.22 | 184,601 | 17.21 | 689,344 | 60,987 | 84.3 | 5.8 |
| Orkut BFS | 32K | P2 | 15,011,813 | 0.693 | 73.38 | 183,513 | 17.19 | 689,482 | 179,195 | 95.0 | 5.1 |
| Orkut BFS | 32K | **P4** | **10,396,360** | **1.000** | 73.32 | 183,884 | 17.20 | 689,229 | **60,988** | 84.4 | 7.4 |
| ogbn-uniform | 8K | P0 | 160,417,625 | 1.000 | 7.59 | 1,189,415 | 25.09 | 1,287,047 | 1,010,657 | 94.1 | 0.4 |
| ogbn-uniform | 8K | P2 | 160,429,629 | 1.000 | 34.35 | 843,488 | 21.88 | 1,284,823 | 719,434 | 94.2 | 0.5 |
| ogbn-uniform | 8K | **P4** | **160,417,628** | **1.000** | 7.59 | 1,189,416 | 25.09 | 1,287,047 | **1,010,657** | 94.1 | 0.5 |
| ogbn-uniform | 16K | P0 | 144,443,603 | 1.000 | 9.33 | 1,231,113 | 24.88 | 1,357,779 | 904,209 | 93.4 | 0.4 |
| ogbn-uniform | 16K | P2 | 144,828,967 | 0.997 | 45.81 | 733,490 | 20.50 | 1,353,589 | 496,912 | 93.7 | 0.6 |
| ogbn-uniform | 16K | **P4** | **144,443,902** | **1.000** | 9.33 | 1,231,116 | 24.88 | 1,357,779 | **904,209** | 93.4 | 0.6 |
| ogbn-uniform | 32K | P0 | 119,371,267 | 1.000 | 13.54 | 1,149,086 | 24.38 | 1,329,040 | 737,053 | 92.2 | 0.5 |
| ogbn-uniform | 32K | P2 | 121,293,357 | 0.984 | 57.97 | 559,022 | 19.04 | 1,330,088 | 288,546 | 92.6 | 0.7 |
| ogbn-uniform | 32K | **P4** | **119,371,266** | **1.000** | 13.54 | 1,149,086 | 24.38 | 1,329,041 | **737,053** | 92.2 | 0.7 |

"Idle %" is `bulk_sched_idle / cycles` (both workloads are translation-bound, so the port is
idle most of the time in every policy); "Org %" is `bulk_org_cycles / cycles`, which for P4
includes the seal's organizer plus the new ordering pass.

**Deltas against P0 (same arm, workload and window).**

| Workload | W | Pol | Δcycles | Δrow hit | Δconflicts | Δopens | Δidle | ΔdTLB miss | Δwalks |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| Orkut | 8K | P2 | **+31.86 %** | +0.02 pt | −91 | +240,295 | +8,728,760 | +106 | +17 |
| Orkut | 8K | P4 | −0.002 % | +0.01 pt | −90 | **0** | −375 | +2 | 0 |
| Orkut | 16K | P2 | **+43.38 %** | +0.04 pt | −225 | +178,755 | +7,533,906 | +36 | +45 |
| Orkut | 16K | P4 | +0.005 % | +0.03 pt | −205 | **0** | +3,816 | −12 | +15 |
| Orkut | 32K | P2 | **+44.32 %** | +0.16 pt | −1,088 | +118,208 | +5,489,864 | +48 | +29 |
| Orkut | 32K | P4 | **−0.049 %** | +0.10 pt | −717 | +1 | +731 | +21 | +32 |
| ogbn | 8K | P2 | +0.007 % | +26.76 pt | −345,927 | −291,223 | +94,498 | −11,343 | +63 |
| ogbn | 8K | P4 | +0.000 % | −0.00 pt | +1 | 0 | 0 | 0 | 0 |
| ogbn | 16K | P2 | +0.267 % | +36.48 pt | −497,623 | −407,297 | +767,142 | −46,480 | +2,557 |
| ogbn | 16K | P4 | +0.000 % | −0.00 pt | +3 | 0 | 0 | −2 | +2 |
| ogbn | 32K | P2 | +1.610 % | +44.43 pt | −590,064 | −448,507 | +2,298,270 | −59,900 | +12,429 |
| ogbn | 32K | P4 | −0.000 % | +0.00 pt | 0 | 0 | 0 | 0 | 0 |

P4's cycle deltas are all within **0.05 %**, its cohort openings are P0's to the count, and its
translation counters are P0's to within 2 misses in 10⁵–10⁶. On ogbn the DRAM counters are
*identical* to P0's — not merely close.

A note on the cohort arithmetic: P2's cells hold 2.28 / 2.89 / 3.79 lines on Orkut (2.4–2.7×
more openings than P0's 5.39 / 7.75 / 11.14) but **1.58 / 2.27 / 3.87 against P0's 1.12 / 1.25 /
1.51** on ogbn, where it *reduces* openings by 29–61 %. A cell is a bad cohort unit for a dense
sequential walk and a good one for a sparse gather; that asymmetry is why P2 hurts exactly one
of the two workloads.

## 4. Results — ideal translation (diagnostic reference)

`ideal_translation = 1` removes the dTLB, the walker and PTE traffic. This is a reference arm,
not a deployable configuration: it deletes the resource whose interaction the milestone is
about.

| Workload | W | P0 | P2 | P4 | P2 vs P0 | P4 vs P0 |
|---|---:|---:|---:|---:|---:|---:|
| Orkut | 8K | 6,351,451 | 6,351,618 | 6,351,330 | +0.003 % | −0.002 % |
| Orkut | 16K | 6,077,523 | 6,076,339 | 6,076,791 | −0.019 % | −0.012 % |
| Orkut | 32K | 5,896,924 | 5,895,916 | 5,895,958 | −0.017 % | −0.016 % |
| ogbn | 8K | 15,741,997 | 13,461,519 | 15,741,450 | **−14.49 %** | −0.003 % |
| ogbn | 16K | 15,571,490 | 12,113,032 | 15,571,959 | **−22.21 %** | +0.003 % |
| ogbn | 32K | 15,196,445 | 10,816,071 | 15,196,913 | **−28.82 %** | +0.003 % |

With translation free there is no admission limit to starve, so P2's Orkut penalty disappears
entirely (−0.02 %) — evidence that the published P2 penalty is an *interaction* with
translation admission, not a DRAM effect. P4 remains at P0 to within 0.02 % in every cell, i.e.
its ordering is memory-equivalent to P0 even when the whole memory system is free to respond.
Row hits confirm this directly: P4 vs P0 is +0.00…+0.02 pt on both workloads in this arm, while
P2 gains +30.0/+45.5/+58.0 pt on ogbn.

## 5. Why P4 is neutral — the page-restricted ceiling

`tools/m23g4b_page_ceiling.py` computes, from the trace and the production address map, the best
row-hit rate any ordering can reach **if it must stay page-major**:
`(L − Σ_p C_p)/L`, where `C_p` is the number of distinct `(bank,row)` cells page `p`'s lines
touch. The unrestricted ceiling `(L − C)/L` is M23-G3's, and its whole-trace column reproduces
G2D's 91.84 % / 98.85 % exactly.

| Workload | W | lines L | cells C | page cells ΣC_p | ceiling (any order) | ceiling (page-major) | gap | lines/page |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut | 8K | 948,230 | 416,086 | 416,086 | 56.12 % | **56.12 %** | **0.00 pt** | 5.39 |
| Orkut | 16K | 823,828 | 284,999 | 284,999 | 65.41 % | **65.41 %** | **0.00 pt** | 7.75 |
| Orkut | 32K | 679,765 | 179,212 | 179,212 | 73.64 % | **73.64 %** | **0.00 pt** | 11.15 |
| ogbn | 8K | 1,133,238 | 719,434 | 1,102,189 | 36.52 % | **2.74 %** | **33.78 pt** | 1.12 |
| ogbn | 16K | 1,127,130 | 493,053 | 1,067,205 | 56.26 % | **5.32 %** | **50.94 pt** | 1.25 |
| ogbn | 32K | 1,115,301 | 280,922 | 1,002,310 | 74.81 % | **10.13 %** | **64.68 pt** | 1.51 |

Two independent facts fall out, and together they are the whole explanation:

1. **On Orkut there is no inter-page row locality to lose.** `ΣC_p == C` exactly at every
   window: no `(bank,row)` cell in an Orkut window has lines in two different pages. Page-major
   ordering is therefore *not a constraint at all* for Orkut's row objective — P2's unrestricted
   freedom buys it the same ceiling P0 already reaches (G3: P0 at 32K = 73.68 % against a
   73.64 % ceiling). P2's +43 % on Orkut therefore cannot be a DRAM story: it changes the issue
   unit and nothing else, which is exactly what the counters show (§3).
2. **On ogbn the row locality is almost entirely inter-page.** A window's pages contribute only
   1.12–1.51 lines each, so page-major grouping can consolidate at most 2.7–10.1 % of commands
   into hits, against 36.5–74.8 % available to an order that may interleave pages. The physical
   row map is 8 MiB (4096 rows × 2 banks × 16 lines), far smaller than ogbn's footprint, so many
   *distinct* virtual pages alias onto the *same* physical row — and P2 harvests precisely those.
   P4 is forbidden from expressing them.

This is the geometric statement the milestone was probing, made quantitative: a row is a quarter
of a page, so "row locality inside a page" is arithmetically bounded at two rows per bank, while
the locality that matters on a sparse gather is a property of the *row map's aliasing*, which is
by construction an inter-page phenomenon.

The residual within-page freedom is the bank interleaving: ascending line order alternates banks
every line, whereas `(row, bank, line)` groups each bank's half-row. That is the only thing P4
changes, and it is worth −0.05…+0.005 % of cycles and ≤0.16 pt of row hits — including a small,
consistent win at Orkut 32K (−5,104 cycles, −717 conflicts) that is far below anything worth a
policy.

## 6. Translation behaviour

Published arm, 16K (8K/32K in `build/wm/m23g4b/summary.csv`):

| Workload | Pol | dTLB miss | Walks | MSHR max | MSHR avg | Walker max | Admission refusals | TLB latency (mean) |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Orkut | P0 | 192,014 | 107,399 | 4 | 2.75 | 2 | 32,318,673 | 88.63 |
| Orkut | P2 | 192,050 | 107,444 | 4 | 1.81 | 2 | 47,194,543 | 83.74 |
| Orkut | **P4** | **192,002** | **107,414** | 4 | 2.75 | 2 | **32,323,324** | **88.63** |
| ogbn | P0 | 1,155,258 | 962,895 | 4 | 3.99 | 2 | 305,373,427 | 550.08 |
| ogbn | P2 | 1,108,778 | 965,452 | 4 | 3.99 | 2 | 318,216,345 | 518.19 |
| ogbn | **P4** | **1,155,256** | **962,897** | 4 | 3.99 | 2 | **305,374,558** | **550.08** |

"Admission refusals" is `tlb0_mshr_stalls`, the number of probe attempts the dTLB refused
because the MSHR slot or the parked-waiter limit was full (the issue port retries next cycle);
"TLB latency" is `lat_tlb_sum / tlb0_reads`, issue→translated.

P4 reproduces P0's translation profile exactly: the same misses, the same walks, the same MSHR
occupancy and the same refusal count to within 0.02 %. That is structural — the page key is
ascending, so the page stream is P0's. P2 does **not** create more walks either (+45 of 107 399 on
Orkut), but it changes how the same pages are presented to the dTLB: mean MSHR occupancy *falls*
(2.75 → 1.81) while admission refusals rise 46 % (32.3 M → 47.2 M). In other words the same
translation work is fetched in a more fragmented order, and more probe attempts are refused for
it. The counter set localizes the effect to the issue order; it does not decompose it further.

### Cross-check: more translation capacity does not fix P2 (M23-G4A spot data)

Orkut BFS, 16K, published configuration, sweeping only `dtlb_mshr` and `walkers` (8/8 Orkut
cells of the M23-G4A matrix complete; the G4A matrix as a whole is *not* complete and is not
reported here beyond this table):

| MSHR / walkers | P0 cycles | P2 cycles | P2 vs P0 | P0 opens | P2 opens |
|---|---:|---:|---:|---:|---:|
| 4 / 2 (published) | 16,714,538 | 23,965,633 | **+43.4 %** | 106,289 | 285,044 |
| 8 / 2 | 16,704,951 | 23,815,066 | +42.6 % | 106,289 | 285,044 |
| 4 / 4 | 13,139,921 | 20,674,260 | **+57.3 %** | 106,307 | 285,078 |
| 8 / 4 | 13,069,606 | 20,613,300 | **+57.7 %** | 106,306 | 285,049 |

Quadrupling walkers makes *both* policies 22 % faster but leaves P2's penalty at 7.1–7.5 M
cycles, and it grows in relative terms because the baseline improves. Cohort openings do not
move at all. So the Orkut regression is **not** a translation-capacity shortage: it is the issue
unit, and it only becomes visible when translation admission is real (ideal arm: −0.02 %).

## 7. Resource cost of P4

Same storage as P2: `ford` (4 B × runs) plus three scratch vectors over the window
(`bank`, `row`, `vpage`, 4 B each, freed at the next seal) — 32/64/128 KiB at 8K/16K/32K, no
MSHR, slot, queue or buffer growth. Organization is charged one visit per run
(`bulk_org_runs` = 823 734 on Orkut 16K, 1 127 130 on ogbn 16K).

Measured cost against P0's seal, which already pays for its own sort:

| Workload | W | P0 `bulk_org_cycles` | P4 | extra | extra as % of cycles |
|---|---:|---:|---:|---:|---:|
| Orkut | 8K | 599,938 | 837,037 | +237,099 | +0.88 % |
| Orkut | 16K | 599,938 | 805,894 | +205,956 | +1.23 % |
| Orkut | 32K | 599,938 | 769,874 | +169,936 | +1.63 % |
| ogbn | 8K | 570,261 | 853,621 | +283,360 | +0.18 % |
| ogbn | 16K | 570,261 | 852,067 | +281,806 | +0.20 % |
| ogbn | 32K | 570,261 | 849,099 | +278,838 | +0.23 % |

## 8. Limitations

1. **P4 is a negative result, and it is reported as one.** It removes the regression and buys
   nothing; the report does not add heuristics to rescue it.
2. **The page ceiling is trace-and-map dependent.** `ΣC_p == C` for Orkut holds for this trace,
   this 8 MiB row map and these windows; a workload whose footprint spans more than the row
   space can differ, as ogbn does. The ogbn page ceiling (2.7–10.1 %) is a bound on the target
   stream, while the measured row-hit rate also contains the index stream and is filtered by
   the L2, so the two should be compared as an ordering bound, not as an efficiency figure.
3. **Two workloads only, as the brief specifies.** Criteo is not run here; G3's Criteo cells
   showed the same pattern as ogbn (P2 −6.1 % ideal, +0.1 % published).
4. **The G4A translation sweep is quoted only for its complete Orkut cells** (§6); its
   ogbn-uniform cells are not part of this report.
5. **Idle time is high in every policy** (84–98 % of cycles) because these workloads are
   translation-bound; P4 does not reduce it below P0's, it merely avoids P2's increase.
6. **No physical-GPU claim.** Everything here is simulator output at fixed latencies
   (`t_RP + t_RCD` = 26, `t_CAS` = 14, walk 150 cycles/level).

## 9. Reproducing

```bash
cd DX100_MEM/sim && make && ./bin/dx100_tests        # 4735 checks, 0 failures

../build/wm/m23g4b_run.sh all 6                      # 18 published cells (~20 min at 6-way)
../build/wm/m23g4b_run.sh idealall 6                 # 18 ideal-reference cells
python3 ../tools/m23g4b_extract.py                   # checks, summary.csv, tables.md
python3 ../tools/m23g4b_page_ceiling.py              # page-restricted row ceiling
```

One cell:

```bash
./bin/dx100sim --config configs/m23g4b_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 --set bulk_org=5 --all
```

`bulk_org` is echoed in the config header and `bulk_org_policy`/`bulk_org_runs` in the stats
block, so a cell can always be checked against the policy it claims to run.

## 10. Files changed

| File | Change |
|---|---|
| `sim/src/bulk.cpp` | `win_order_fused`: P4 branch (`vpage` key + page-major `(row,bank,line)`); cohort-unit predicate narrowed to `bulk_org == 2 \|\| == 3`; constructor range check `0..5`; comments |
| `sim/configs/m23g4b_base.cfg` | instrument configuration (= published `m23_dx16k.cfg` + `bulk_org`, `dram_diag`), policies 0/2/5 documented |
| `build/wm/m23g4b_run.sh` | 36-cell runner (published + ideal arms) |
| `tools/m23g4b_extract.py` | accounting checks, `summary.csv`, `tables.md` |
| `tools/m23g4b_page_ceiling.py` | page-restricted vs unrestricted row ceiling |

No other file is touched: `win_order_fused`'s P0/P1/P2/P3 branches, the seal, the issue paths,
the address map, the traces and every historical log are unmodified, and P0/P2 reproduce them
to the cycle (§2).

**Artifacts** (`build/wm/m23g4b/`): `g4b_*` published cells, `g4bi_*` ideal-reference cells,
`summary.csv` (36 rows), `checks.txt`, `tables.md`, `run.log`, `hashes.txt`.
