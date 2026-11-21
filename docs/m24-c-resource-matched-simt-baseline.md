# M24-C — Resource-matched SIMT baseline

**Answer.** Historical M0 was not a weak baseline because of its caches, its DRAM, or its
global request budget: every one of those already equalled the strongest Bulk
configuration. It was weak in **two translation capacities only** — 2 page walkers and 4
dTLB MSHRs against Bulk's 8 and 8. Giving conventional SIMT exactly those two capacities,
and nothing else, makes it **2.74× faster on Orkut, 3.54× on LiveJournal, 3.95× on ogbn
and 3.93× on Criteo**. That single change is larger than the *entire* historical Bulk
advantage on three of the four workloads (2.94×, 2.38×, 1.27×, 2.32× over M0), and against
the M23-A fused configurations at matched hardware SIMT is now **1.07×, 1.49×, 3.11× and
1.69× faster** than Bulk 2×16K. The historical "Bulk speedup" was therefore mostly a
comparison between a translation-starved baseline and a configuration that had already
been given more walkers.

A Bulk advantage does survive, but only in the G6D family, and it is not the fused-window
organization: **G6D S1 is still 2.92×, 2.52×, 1.27× and 2.22× faster than M0-RM** at
identical hardware capacities. Its advantage comes from *doing less translation work*
(page cohorts coalesce L2 TLB misses into 1.27–2.91× fewer walks) and from *organizing the
physical DRAM requests* it does issue (row-hit rate 60.5 % against SIMT's 29.9 % on Orkut),
not from having more walkers — M0-RM has the same eight.

---

## 1. Question and method

The M23 report line "Bulk is 2.9× faster than SIMT" compared two things at once: an
organization and a resource budget. This milestone separates them by building a *second*
conventional baseline that keeps M0's request path byte-for-byte and changes only the
hardware capacities Bulk was already using, then measuring the same six configurations on
the same four traces.

Three quantities are reported throughout, all in execution cycles:

* **resource benefit** — M0 ÷ M0-RM (hardware scaling at fixed SIMT organization);
* **architecture benefit** — M0-RM ÷ Bulk (organization at fixed hardware);
* **total** — M0 ÷ Bulk, which is exactly the product of the other two.

They are *not* additive in percentage terms and they are not independent (a Bulk
configuration that already contains 8 walkers receives no resource benefit at all), so the
decomposition is reported as ratios with the interaction made explicit rather than as two
summands.

## 2. Source audit

Every value below was read from the configuration files, not from a report summary:
`sim/configs/m23_m0.cfg`, `m23_dx8k.cfg`, `m23_dx16k.cfg`, `m23_dx32k.cfg`,
`m23g6b_base.cfg`, plus the command line of `build/wm/m23g6d_run.sh`.

| Resource | key | M0 | 2×8K | 2×16K | 2×32K | G6D S1 |
|---|---|---|---|---|---|---|
| page walkers | `walkers` | 2 | 2 | 2 | 2 | **8** (CLI) |
| dTLB MSHRs | `dtlb_mshr` | 4 | 4 | 4 | 4 | **8** (CLI) |
| dTLB size / ways | `dtlb_entries`/`dtlb_ways` | 16 / 4 | 16 / 4 | 16 / 4 | 16 / 4 | 16 / 4 |
| dTLB parked | `dtlb_parked` | 2 | 2 | 2 | 2 | 2 |
| L2 TLB size / ways | `l2tlb_entries`/`l2tlb_ways` | 512 / 4 | 512 / 4 | 512 / 4 | 512 / 4 | 512 / 4 |
| L2 TLB MSHRs | `l2tlb_mshr` | 8 | 8 | 8 | 8 | 8 |
| L2 TLB latency | `l2tlb_latency` | 4 | 4 | 4 | 4 | 4 |
| walk cache | `walk_cache` | 16 | 16 | 16 | 16 | 16 |
| levels / walk latency | `levels`/`walk_latency_per_level` | 2 / 150 | 2 / 150 | 2 / 150 | 2 / 150 | 2 / 150 |
| L1 size / ways / MSHRs | `l1_lines`/`l1_ways`/`l1_mshr` | 256 / 4 / 128 | 256 / 4 / 128 | 256 / 4 / 128 | 256 / 4 / 128 | 256 / 4 / 128 |
| L2 size / ways / MSHRs | `l2_lines`/`l2_ways`/`l2_mshr` | 16384 / 8 / 64 | 16384 / 8 / 64 | 16384 / 8 / 64 | 16384 / 8 / 64 | 16384 / 8 / 64 |
| L1 / L2 latency | `l1_latency`/`l2_latency` | 2 / 4 | 2 / 4 | 2 / 4 | 2 / 4 | 2 / 4 |
| global outstanding budget | `R` | 128 | 128 | 128 | 128 | 128 |
| SIMT pending slots | `lsu_pending` | 0 | 0 | 0 | 0 | 0 |
| DRAM banks × rows | `banks`/`rows` | 2 × 4096 | 2 × 4096 | 2 × 4096 | 2 × 4096 | 2 × 4096 |
| DRAM row size, timing | `row_bytes`/`t_hit`/`t_miss` | 1024 / 14 / 26 | 1024 / 14 / 26 | 1024 / 14 / 26 | 1024 / 14 / 26 | 1024 / 14 / 26 |
| DRAM queue, policy | `dram_q`/`frfcfs`/`bank_hash` | 0 / 1 / 1 | 0 / 1 / 1 | 0 / 1 / 1 | 0 / 1 / 1 | 0 / 1 / 1 |
| lane width, warps | `lanes`/`warps_per_core` | 4 / 4 | 4 / 4 | 4 / 4 | 4 / 4 | 4 / 4 |
| engine pipeline | `dispatch`/`win_fused`/`dedup`/`page_order` | 0 / 0 / 0 / 0 | 1 / 1 / 1 / 1 | 1 / 1 / 1 / 1 | 1 / 1 / 1 / 1 | 1 / 1 / 1 / 1 |
| context window | `batch_window` | 0 | 8192 | 16384 | 32768 | 16384 |
| page cohorts | `page_cohorts` | **0** | 2 | 2 | 2 | **8** (CLI) |

The audit's finding is the table itself: **twenty of these rows are identical in all six
configurations**, including every cache, every MSHR except the dTLB's, the DRAM geometry,
the timing parameters, the queue policy, the page size and the 128-entry global budget.
Only four fields move, and two of them (`batch_window`, `page_cohorts`) are Bulk
organizational machinery with no SIMT counterpart. The historical baseline was therefore
*not* under-provisioned in the memory hierarchy at all — it was under-provisioned in
translation concurrency, which is exactly where Bulk had been strengthened.

The G6D S1 cells add `walkers = 8`, `dtlb_mshr = 8`, `page_cohorts = 8` and `bulk_org = 8`
on the command line over `m23g6b_base.cfg`, matching `build/wm/m23g6d_run.sh`'s S1 cell
(`w=8 c=8 m=8 l=150`) and its `bulk_org=8` / `g6b_pa_capacity` settings.

## 3. What cannot be matched, and why

| Bulk resource | SIMT analogue | Decision |
|---|---|---|
| page cohort scheduler (`page_cohorts = 8`) | none — SIMT generates requests warp-by-warp | **not copied**; matching it would be implementing Bulk inside the baseline |
| shared physical-address ready pool (`g6a_pa_capacity`, `g6b_pa_capacity = 4096`) | none — SIMT's L1/L2 MSHRs are its only request buffers | not copied; the MSHRs are already matched |
| index acquisition, index-stream consolidation (`idx_*`, `bulk_index_base`) | none — the trace carries element addresses | not matched; documented as extra Bulk work |
| dedup / reuse directory (`dedup`) | none | not copied |
| physical-row organizer (`bulk_org`, `win_fused`, `page_order`) | FR-FCFS over an open queue | both keep their own scheduler; FR-FCFS is not removed from SIMT |

These are *organization*, not capacity, and the milestone's purpose is to price the
organization separately. The consequence is stated as a limitation in §11: a Bulk
configuration does strictly more memory work (it reads an index array) and strictly less
translation work (it batches), and no SIMT configuration can be made to do either.

## 4. M0-RM

`sim/configs/m24c_m0_rm.cfg` is `m23_m0.cfg` with exactly two lines changed:

```
walkers    = 8      # was 2
dtlb_mshr  = 8      # was 4
```

Everything else — `dispatch = 0`, `batch_window = 0`, `page_cohorts = 0`, `dedup = 0`,
`page_order = 0`, `win_fused = 0`, `R = 128`, the caches, the DRAM — is untouched, so the
model still runs the unmodified SIMT front-end, coalescer, TLB hierarchy, MSHRs and
FR-FCFS controller. The file's header records the delta and the audit that justifies it.
No simulator source was modified in this milestone, so historical M0 and every historical
Bulk configuration remain bit-identical; §12 shows M0 reproducing its published cycles to
the cycle on all four workloads.

## 5. Configuration matrix and invocation

| id | configuration | role |
|---|---|---|
| M0 | `configs/m23_m0.cfg` | historical SIMT baseline, unchanged |
| M0-RM | `configs/m24c_m0_rm.cfg` | SIMT with the two matched capacities |
| 2×8K | `configs/m23_dx8k.cfg` | fused Bulk, 8K window |
| 2×16K | `configs/m23_dx16k.cfg` | fused Bulk, 16K window |
| 2×32K | `configs/m23_dx32k.cfg` | fused Bulk, 32K window |
| G6D S1 | `configs/m23g6b_base.cfg` + 4 CLI fields | strongest translation-throughput configuration |

Workloads, identical for all six: `m23_orkut_bfs.bin` (`bulk_max_elems = 2097152`),
`m23_livejournal_bfs.bin` (16777216), `m23_ogbn.bin` (2097152), `m23_criteo_16k.bin`
(1048576); seed 1, 4 KiB pages, the same physical map, caches, DRAM and timing (they come
from the shared config sections audited above). 24 cells, `build/wm/m24c_run.sh`.

Two invocation details are disclosed rather than hidden:

1. **Index-array placement.** All cells pass `--set bulk_index_base=0x80000000`. The
   `m23_dx*.cfg` files default to `0x00800000`, which collides with the trace at
   `bulk_max_elems = 2M` (the run terminates at `cycles=0`); every published M23 trace run
   used the 2 GiB base. The field is inert for M0 and M0-RM.
2. **G6D S1 is not "2×16K plus walkers".** It also runs `bulk_org = 8` (page cohorts), an
   8-way cohort width and the 4096-entry streaming ready buffer, on `m23g6b_base.cfg`
   rather than `m23_dx16k.cfg`. G6D against 2×16K is therefore *not* a controlled
   comparison and is never used as one; G6D against M0-RM is controlled on hardware, and
   §11 lists what still differs.

## 6. Performance

### 6.1 Execution cycles

| workload | M0 | M0-RM | 2×8K | 2×16K | 2×32K | G6D S1 |
|---|---|---|---|---|---|---|
| Orkut BFS S1 | 49 067 545 | 17 904 629 | 27 034 450 | 16 714 538 | 10 401 464 | 6 130 494 |
| LiveJournal | 67 234 692 | 19 020 756 | 41 651 622 | 28 309 390 | 17 533 888 | 7 558 966 |
| ogbn-products train | 176 184 939 | 44 637 463 | 156 713 716 | 139 088 324 | 113 518 967 | 35 248 957 |
| Criteo embeddings | 28 511 774 | 7 260 692 | 15 778 298 | 12 273 324 | 8 942 139 | 3 267 387 |

### 6.2 The three speedups

| workload | resource (M0/M0-RM) | architecture (M0-RM/2×16K) | architecture (M0-RM/G6D) | total vs 2×16K | total vs G6D |
|---|---|---|---|---|---|
| Orkut | 2.740 | 1.071 | 2.921 | 2.936 | 8.004 |
| LiveJournal | 3.535 | 0.672 | 2.516 | 2.375 | 8.895 |
| ogbn | 3.947 | 0.321 | 1.266 | 1.267 | 4.998 |
| Criteo | 3.927 | 0.592 | 2.222 | 2.323 | 8.726 |

Read the middle column literally: a ratio below 1 means Bulk 2×16K is **slower** than
resource-matched SIMT. It is slower on three of four workloads, and by 3.1× on ogbn.

### 6.3 The historical comparison, restated

The M23-A claim was that fused Bulk beats M0 by 2.94× / 2.38× / 1.27× / 2.32× (the
"total vs 2×16K" column). At matched hardware the same organization *loses* to SIMT. The
historical win and the resource win are the same win counted twice: 2×16K's 8K/16K/32K
window cuts walks (below), which is a way of buying translation headroom, and M0 had been
left with a quarter of the walkers that G6D was later given.

G6D's advantage is real and survives the correction, but it is **not** the fused window:
2×16K and G6D share the same 16K window and the same walk count (107 399 against 107 405 on
Orkut), and differ by 2.7× in cycles. What separates them is the page-cohort organization
(8 cohorts, `bulk_org = 8`, a 4096-entry ready buffer) and the translation capacity that
comes with it — G6D is the configuration the brief calls the strongest *translation
throughput* config, and after resource matching the throughput part is no longer the
difference, so what remains is organization.

### 6.4 Non-additivity

The total is the product of the two ratios by construction, but the two factors are not
independent and their sizes are not transferable:

* Bulk 2×8K/16K/32K already contain M0's resources, so their "resource benefit" is 1.0 —
  the resource benefit is a property of the *pair* being compared, not of the workload.
* The resource benefit is largest exactly where translation is the bottleneck (ogbn 3.95×,
  Criteo 3.93×) and smallest where the request path is (Orkut 2.74×), so it cannot be
  added to an architecture benefit measured on a different bottleneck.
* G6D's total (8.0× on Orkut) is not 2.74 × 2.92 of two independent effects: its
  organization is *why* its walks are 2.9× fewer, and fewer walks is also what makes eight
  walkers sufficient in the first place.

## 7. Translation

`walkers` is average walkers busy; `dMSHR`/`l2MSHR` are mean occupancy of the dTLB and L2
TLB miss registers; `infl` is the maximum number of in-flight translation requests;
`p50`/`p95` are request latencies in cycles.

| workload | config | walks | dTLB hit | L2 hit | walkers | dMSHR | l2MSHR | infl | p50 | p95 |
|---|---|---|---|---|---|---|---|---|---|---|
| Orkut | M0 | 312 381 | 41.6 % | 40.5 % | 1.91 | 3.59 | 3.56 | 128 | 62 | 748 |
| Orkut | M0-RM | 311 592 | 41.6 % | 40.7 % | 5.22 | 5.31 | 5.24 | 128 | 84 | 658 |
| Orkut | 2×16K | 107 399 | 78.6 % | 0.1 % | 1.93 | 2.75 | 2.74 | 122 | 127 | 957 |
| Orkut | G6D S1 | 107 405 | 78.6 % | 0.2 % | 5.26 | 5.31 | 5.27 | 144 | 13 364 | 31 952 |
| LiveJournal | M0 | 443 163 | 18.7 % | 46.4 % | 1.98 | 3.88 | 3.85 | 128 | 86 | 658 |
| LiveJournal | M0-RM | 441 523 | 18.9 % | 46.1 % | 6.96 | 7.09 | 6.99 | 128 | 133 | 425 |
| LiveJournal | 2×16K | 186 923 | 60.5 % | 0.2 % | 1.98 | 3.24 | 3.22 | 121 | 308 | 753 |
| LiveJournal | G6D S1 | 187 108 | 60.5 % | 0.2 % | 7.43 | 7.50 | 7.45 | 137 | 7 315 | 32 939 |
| ogbn | M0 | 1 174 566 | 0.1 % | 1.6 % | 2.00 | 3.99 | 3.98 | 12 | 633 | 659 |
| ogbn | M0-RM | 1 174 411 | 0.1 % | 1.6 % | 7.89 | 7.97 | 7.92 | 18 | 337 | 365 |
| ogbn | 2×16K | 927 157 | 10.4 % | 0.0 % | 2.00 | 3.98 | 3.96 | 73 | 396 | 647 |
| ogbn | G6D S1 | 927 116 | 10.4 % | 0.0 % | 7.89 | 7.97 | 7.92 | 122 | 384 | 658 |
| Criteo | M0 | 190 076 | 3.9 % | 41.0 % | 2.00 | 3.99 | 3.96 | 13 | 315 | 650 |
| Criteo | M0-RM | 189 253 | 3.9 % | 41.2 % | 7.82 | 7.95 | 7.85 | 21 | 306 | 337 |
| Criteo | 2×16K | 81 493 | 41.3 % | 9.1 % | 1.99 | 3.96 | 3.94 | 119 | 306 | 633 |
| Criteo | G6D S1 | 81 616 | 39.8 % | 9.6 % | 7.50 | 7.59 | 7.53 | 128 | 309 | 857 |

Three facts explain the whole milestone.

1. **Historical M0 saturates its translation hardware.** Walkers are 1.91–2.00 of 2 busy
   and the dTLB MSHRs are 3.88–3.99 of 4 on every workload: the baseline was issuing all
   the translation concurrency it had, all the time.
2. **The L2 TLB MSHRs were never the limit.** Occupancy is 3.56–3.98 of 8, and it stays
   below capacity in M0-RM too — the resource the brief suspected was already sufficient.
3. **Bulk's advantage is walk *count*, not walk throughput.** 2×16K cuts walks 2.9× on
   Orkut, 2.4× on LiveJournal and 2.3× on Criteo by coalescing L2 TLB misses inside a
   page cohort before the walker sees them; G6D inherits that reduction unchanged and adds
   nothing to it. Halving the walks is worth more than doubling the walkers, because the
   16K window is a *pre-walk* filter while walkers are a post-miss resource.

The ogbn row is the sharpest: 1.17M walks over 1.2M accesses means essentially every
access misses both TLBs, so the walk rate is fixed by the address stream and eight walkers
convert it into 3.95×; only the cohort filter (1 174 411 → 927 116 walks) can go further,
and that is worth the remaining 1.27×.

`p50` for G6D is 13 364 cycles on Orkut, not a slowdown: the streaming policy parks
translated lines in a 4096-entry ready buffer until the release order reaches them, so the
*measured* latency includes waiting in that buffer while the work still completes early.

## 8. Memory system

| workload | config | reads | row-hit | activations | bank busy | queue depth | DRAM wait | L1 MSHR | L2 MSHR |
|---|---|---|---|---|---|---|---|---|---|
| Orkut | M0 | 519 337 | 29.8 % | 364 489 | 11.9 % | 0.91 | 44 545 085 | 1.24 | 1.18 |
| Orkut | M0-RM | 519 309 | 29.9 % | 363 905 | 32.5 % | 3.06 | 54 765 414 | 3.96 | 3.80 |
| Orkut | 2×16K | 651 309 | 60.4 % | 257 700 | 36.5 % | 7.23 | 120 883 560 | 8.19 | 8.08 |
| Orkut | G6D S1 | 644 142 | 60.5 % | 254 249 | 98.4 % | 60.31 | 369 758 240 | 62.93 | 62.60 |
| LiveJournal | M0 | 600 733 | 16.3 % | 502 763 | 10.7 % | 0.35 | 23 839 217 | 0.63 | 0.60 |
| LiveJournal | M0-RM | 600 717 | 16.4 % | 502 429 | 38.0 % | 2.06 | 39 224 024 | 3.03 | 2.92 |
| LiveJournal | 2×16K | 697 219 | 46.1 % | 375 631 | 25.2 % | 2.51 | 70 940 918 | 3.14 | 3.08 |
| LiveJournal | G6D S1 | 692 695 | 46.2 % | 372 338 | 93.7 % | 53.31 | 402 937 904 | 55.66 | 55.46 |
| ogbn | M0 | 1 194 355 | 0.1 % | 1 192 694 | 8.8 % | 0.06 | 9 835 008 | 0.26 | 0.25 |
| ogbn | M0-RM | 1 194 359 | 0.2 % | 1 192 564 | 34.8 % | 0.20 | 8 821 015 | 1.01 | 0.97 |
| ogbn | 2×16K | 1 369 035 | 10.6 % | 1 223 946 | 12.2 % | 0.24 | 32 597 657 | 0.50 | 0.51 |
| ogbn | G6D S1 | 1 367 453 | 11.0 % | 1 217 711 | 47.9 % | 3.57 | 125 861 472 | 4.41 | 4.65 |
| Criteo | M0 | 86 886 | 6.0 % | 81 696 | 3.9 % | 0.02 | 489 276 | 0.12 | 0.10 |
| Criteo | M0-RM | 86 882 | 5.9 % | 81 721 | 15.1 % | 0.09 | 667 878 | 0.51 | 0.43 |
| Criteo | 2×16K | 103 617 | 31.0 % | 71 497 | 9.4 % | 0.48 | 5 901 231 | 0.72 | 0.69 |
| Criteo | G6D S1 | 104 615 | 32.7 % | 70 438 | 35.3 % | 5.59 | 18 268 221 | 6.45 | 6.39 |

Resource matching changes *when* the DRAM is asked, not *what* it is asked: M0-RM reads
within 0.01 % of M0 on every workload (519 309 against 519 337 on Orkut), so the extra
walkers buy no extra traffic and no extra row locality — the row-hit rate and activation
count are unchanged to three digits. Bulk's row-hit rate is a genuine organizational gain
(29.9 % → 60.4 % on Orkut, 16.4 % → 46.1 % on LiveJournal), but it is bought with 25 % more
reads on Orkut, because Bulk also reads its index array.

Bank busy time is the honest utilization measure across configurations: SIMT under
resource matching raises DRAM utilization fourfold on Orkut (11.9 % → 32.5 %) and Criteo
(3.9 % → 15.1 %) without changing the access pattern, which is what "translation was the
bottleneck" looks like from the memory side. G6D reaches 98.4 % bank busy on Orkut with a
queue 60 requests deep: at that point the DRAM is the limit, and the 6.13M cycles are set
by 644 142 reads at 14/26-cycle timings, not by translation.

## 9. SIMT frontend and the next bottleneck

| workload | config | IPC | requests | slot-pool full | unissued cycles | TLB refusals |
|---|---|---|---|---|---|---|
| Orkut | M0 | 0.0061 | 1 200 003 | 48 759 758 | 5 965 256 383 | 138 924 321 |
| Orkut | M0-RM | 0.0168 | 1 200 003 | 17 601 266 | 2 068 578 086 | 30 607 069 |
| LiveJournal | M0 | 0.0045 | 1 199 981 | 66 926 556 | 8 244 939 956 | 238 271 421 |
| LiveJournal | M0-RM | 0.0159 | 1 199 981 | 18 717 049 | 2 204 315 042 | 56 295 768 |
| ogbn | M0 | 0.0017 | 1 200 003 | 175 863 477 | 21 795 053 464 | 703 491 625 |
| ogbn | M0-RM | 0.0068 | 1 200 003 | 44 329 523 | 5 308 420 458 | 177 324 861 |
| Criteo | M0 | 0.0039 | 360 326 | 28 390 385 | 3 523 152 365 | 110 739 514 |
| Criteo | M0-RM | 0.0152 | 360 326 | 7 147 336 | 863 247 228 | 27 253 723 |

IPC rises 2.8–4.0× with the walkers, requests generated are *identical* (1 200 003 on
Orkut in both), and the peak global occupancy is 132 in every SIMT cell — the 128-entry
budget plus the four lanes' charge latch, the same value every earlier milestone reports.
The slot pool is full for 98.3–99.8 % of cycles in both baselines, so `R` bounds issue at
all times; what the walkers change is how long each slot is held.

**Does translation concurrency simply move the bottleneck?** On Orkut and LiveJournal,
yes. After matching, walkers are 65 % and 87 % busy rather than 100 %, the DRAM moves from
11.9 % to 32.5 % bank busy, and the limiting resource becomes the global 128-request budget
and the SIMT issue path (slot pool full 17.6M of 17.9M cycles). On ogbn and Criteo, no:
walkers are still 7.82–7.89 of 8 busy and the dTLB MSHRs 7.95–7.97 of 8, so the pipeline
remains translation-bound, now by *walk count* rather than by walker count. That asymmetry
is the reason the resource benefit ranges from 2.74× to 3.95× instead of being one number.

## 10. Answers

1. **How much faster is M0-RM than M0?** 2.74× Orkut, 3.54× LiveJournal, 3.95× ogbn,
   3.93× Criteo — from two capacity fields.
2. **How much of the original Bulk advantage disappears?** All of it, on three workloads
   and then some: the historical 2.94×/2.38×/1.27×/2.32× for 2×16K becomes
   1.07×/0.67×/0.32×/0.59× against M0-RM, i.e. 2×16K is now 7 % faster on Orkut and 33–69 %
   *slower* elsewhere.
3. **Does Bulk still provide a substantial advantage?** Only the G6D family:
   2.92× Orkut, 2.52× LiveJournal, 1.27× ogbn, 2.22× Criteo over M0-RM at equal hardware.
4. **What is the remaining advantage?** Translation **work reduction** first — page cohorts
   coalesce pre-walk misses, cutting walks 2.9×/2.4×/1.3×/2.3× — and physical request
   **organization** second (row hit 29.9 % → 60.5 % on Orkut, and 98.4 % bank busy). Not
   translation *throughput*: M0-RM has the same eight walkers and the same eight dTLB
   MSHRs, and its walkers are *less* busy than G6D's.
5. **Does SIMT hit a different bottleneck?** On Orkut and LiveJournal yes — the 128-entry
   global budget and the issue path, with DRAM utilization up 2.7× and 3.6×. On ogbn and
   Criteo no — it stays translation-bound, now limited by the number of walks rather than
   by the number of walkers.
6. **Does it vary between workloads?** Strongly. The resource benefit spans 2.74–3.95× and
   the architecture benefit spans 1.27–2.92×, with the ordering set by how translation-heavy
   the address stream is: ogbn (1.17M walks for 1.2M accesses) is where hardware matters
   most and organization matters least, Orkut is where organization matters most.

## 11. Fairness limitations

* **Bulk does more memory work.** It reads an index array M0 never touches (1 369 035 reads
  against 1 194 359 on ogbn, +14.6 %). Its cycles therefore include work outside the
  comparison, which biases *against* Bulk; against that, its window lets it issue fewer,
  better-ordered requests, which biases for it. Neither effect is removable without
  changing the architecture, and both are visible in the tables.
* **G6D S1 is not a single-variable configuration.** It differs from 2×16K in
  `walkers`, `dtlb_mshr`, `page_cohorts`, `bulk_org`, `g6b_pa_capacity` and its base file.
  It is used only against M0-RM, where the hardware fields match; the organizational
  fields are exactly what is being priced.
* **Some Bulk resources have no SIMT analogue** (§3). They are absent from M0-RM by design,
  so "resource-matched" means "matched in every resource both architectures possess".
* **The index-array base is overridden** to 2 GiB in all cells; at the `m23_dx*.cfg` default
  (8 MiB) the 2M-element index array collides with the trace and the run ends at
  `cycles = 0`, so no published trace cell used the default.
* **Trace substitution.** G6D's own ogbn cell was published on `m23_ogbn_train_100d.bin`;
  all six configurations here use `m23_ogbn.bin` so that the comparison holds one trace
  fixed. G6D's published ogbn number is therefore *not* reproduced here and is not compared.
* **M0's cycle guard.** `m23_m0.cfg` sets `max_cycles = 200000000`; the slowest M0 cell
  (ogbn, 176 184 939) stays below it, so no cell was truncated.
* **Translation latency distributions** are reported as p50/p95 only; the simulator does not
  retain full histograms, so no percentile beyond p95 is available.

## 12. Validation

* **Historical M0 reproduces exactly** on all four workloads against logs written before
  this milestone: 49 067 545 (`build/wm/m23g6c/g6c_orkut_w16384_m0.txt`), 67 234 692
  (`g6c_livejournal_w16384_m0.txt`), 176 184 939 (`build/wm/m23d_train_m0.txt`),
  28 511 774 (`build/wm/m23b_m0.txt`).
* **24/24 cells pass** `tools/m24c_extract.py`: `conservation_ok = 1` and a completion hash
  in every cell; `pte_fetches = 2 × walks`; `walks ≤ l2tlb_misses` (equal in SIMT to within
  the walks still in flight, ≤ 8); `dTLB MSHR peak = 4` for M0 and `= 8` for M0-RM with
  `walker peak = 2` and `= 8` respectively, which is the resource limit actually being
  enforced; `row hits + row conflicts + 2 = dram reads`, `activations = conflicts + 2` and
  `arrivals = reads` in the DRAM model.
* **Determinism.** `m24c_m0rm_livejournal.txt` was re-run and is byte-identical to the
  stored cell.
* **Test suite.** `./bin/dx100_tests` reports 4 924 checks, 0 failures. No simulator source
  was modified in this milestone, so this is the M24-B suite unchanged.
* **Not run:** no new microbenchmark, no refresh or tFAW sensitivity, and no measurement of
  the historical G6D ogbn trace (§11). Nothing in M23 or earlier was modified.

## 13. Conclusions

Resource matching is not a footnote to the Bulk result — it is most of it. The historical
M0 baseline was equal to Bulk in caches, MSHRs, DRAM and the global request budget and
unequal in exactly one place, translation concurrency, and that one place accounted for
more than the entire measured Bulk speedup on three of four workloads. Any future claim
that a Bulk organization beats SIMT has to be made against M0-RM, and against M0-RM the
M23-A fused configurations lose.

What survives is narrower and more interesting: batching translation *before* the walker
(page cohorts) and organizing the resulting physical requests. That combination is worth
1.27–2.92× over a hardware-matched SIMT baseline, and it is strongest exactly where the
address stream is translation-heavy or the DRAM can be made nearly saturated — 2.92× and
98.4 % bank busy on Orkut, only 1.27× on ogbn where the walk rate is fixed by the stream
and the DRAM is idle at 47.9 %. A resource-matched SIMT baseline is therefore the correct
control for every remaining Bulk claim, and the honest summary of M23's headline is that
it measured walker count, cohort width and DRAM ordering together, not one architecture.

## Reproduce

```
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests
cd DX100_MEM && ./build/wm/m24c_run.sh all        # PAR=4 by default; skips finished cells
python3 tools/m24c_extract.py                     # summary.csv, extract.txt, ALL CHECKS PASSED
python3 tools/m24c_verify_doc.py                  # every figure in this file, recomputed
```

Artifacts: `build/wm/m24c/` (24 cell logs, `summary.csv`, `extract.txt`),
`sim/configs/m24c_m0_rm.cfg`, `build/wm/m24c_run.sh`, `tools/m24c_extract.py`,
`tools/m24c_verify_doc.py`.
