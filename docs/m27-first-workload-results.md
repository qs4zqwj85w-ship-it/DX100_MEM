# M27 — First workload performance comparison: Orkut BFS, M0-RM vs M27 Bulk

All numbers below are **M27 measurements** produced by the corrected simulator (`sim/bin/dx100sim`,
built after the M27 fixes). No source file, configuration file, timing parameter or historical artifact
was modified to produce them. Logs: `build/wm/m27/wl1/{m0rm_orkut,m27bulk_orkut}.txt`.

**Headline: 2.9201× speedup of M27 Bulk over M0-RM** on the paired Orkut BFS cell (17 904 629 →
6 131 444 cycles), against the historical M24-D reference of 2.921× (−0.016 % relative). Both runs
complete with request conservation OK and the liveness check armed and untripped.

---

## 1. Effective configuration verification

The two cells differ in **exactly nine configuration keys**, and the simulator prints the same
identification line for both (`dispatch=N op_class=gen dispatch_order=0 pool_slots=R`).

| key | M0-RM | M27 Bulk | why |
|---|---|---|---|
| `dispatch` | 0 | 1 | SIMT only vs dispatch scheduler active |
| `batch_window` | 0 | 16 384 | window organiser |
| `win_fused` | 0 | 1 | the sealed-window pipeline |
| `bulk_org` | 0 | 8 | M24/G6D-S1 page organization |
| `dedup` | 0 | 1 | intra-line dedup |
| `page_sched` | 0 | 1 | page-cohort scheduler |
| `page_cohorts` | 0 | 8 | cohort width |
| `page_order` | 0 | 1 | sealed page order |
| `m24_dram_org` | 1 | **0** | the M27 default: DRAM sorter off |

Everything else is identical and resource-matched: `l1 = 256`, `l2 = 16 384`, `l2tlb = 512`,
`l1_mshr = 128`, `l2_mshr = 64`, `dtlb_mshr = 8`, `walkers = 8`, `walk_latency_per_level = 150`,
`slots_total = 128`, `mem_path = l1`, `cores = 1`, `dram = 2` banks, `t_hit = 14`, `t_miss = 26`,
`lat = 4`, `page_bytes = 4 096`, `elem_bytes = 4`, `dram_map = 0`.

**Note on `m24_dram_org = 1` in the baseline.** This is the historical M24-D baseline file, preserved
byte-for-byte. The key is read in exactly one place — `win_order_fused`, the window release-order
organiser — which is only reached when `win_fused = 1`. With `dispatch = 0` and `win_fused = 0` the
SIMT path never calls it, so its value is **inert in the M0-RM arm**. Evidence: the M0-RM run
reproduces the historical M0-RM log exactly (17 904 629 cycles), and the run emits none of the
`bulk_*`/`win_*` counters.

**M27 architecture is present as intended:** `bulk_org_runs = 0` (sorter never ran),
`bulk_win_entries = 1 199 876` with `win_fused = 1` and `bulk_win_page_order = 1` (page organization
did run), `bulk_sched_on = 1` with `bulk_sched_cohorts = 8` (dispatch scheduler retained), `l1`/`l2`/
`l2tlb`/DRAM unchanged from the baseline values listed above.

### The selection mechanism (verified, not assumed)

`op_class = gen` in both configs, and the mechanism is `build_op_classes(cfg, tr)`
(`sim/src/workload.cpp:291`), called once before the first request or descriptor:

```cpp
if (cfg.op_class == "simt")    → every instruction SIMT        (gate G1)
if (cfg.op_class == "bulk")    → every instruction Bulk        (gate G2)
if (cfg.op_class == "pattern") → contiguity rule kappa_real applied to the addresses
if (cfg.op_class == "gen")     → tr.instr_class, recorded by the trace generator as metadata
```

**M27 retains the static, per-instruction, trace-annotated selector exactly as M25 described.** It is
not a compiler-driven selector, and nothing is classified at run time — `Sim::run_hybrid` reads
`Config::dispatch_order` and the map, never an address. The `pattern` variant exists only to show the
annotation is recoverable by a rule a compiler could implement; `gen` (the compiled-in annotation) is
what these runs use. On this trace the annotation sends **43 instructions / 127 elements to SIMT and
301 148 instructions / 1 199 876 elements to Bulk** (`simt_share_x1000 = 0`).

---

## 2. Workload and input identity

| item | value |
|---|---|
| dataset | `build/wm/m23_orkut_bfs.bin` (the established M24-D Orkut BFS input) |
| instructions / lines / pages | 301 191 / 100 282 / 2 048 |
| elements (accesses) | 1 200 003 |
| `elem_bytes` / `page_bytes` | 4 / 4 096 |
| address map | `dram_map = 0` (M0, production) |
| seed | fixed in the trace file; no run-time RNG (`idx_oracle = 0`, `bulk_idx_oracle = 0`) |
| measurement boundary | full run, `--all`, `begin → last retirement` |
| invocation | `--config … --set dataset=…/m23_orkut_bfs.bin --all` |

The only override on the command line is the dataset path; every parameter comes from the config file.
M0-RM wall time 16.5 s, M27 Bulk 60.6 s — no unexpected duration, nothing aborted.

---

## 3. M0-RM results

| | value |
|---|---|
| total cycles | **17 904 629** |
| requests issued / completed | 1 200 003 / 1 200 003 |
| completion | `conservation=OK`, `conservation_violations=0`, exit 0, liveness never tripped |
| lines/cycle · DRAM GB/s | 0.006 · 0.74 |
| identity check | **identical** to the preserved `m24d_simt_orkut.txt` (17 904 629 cycles) |

## 4. M27 Bulk results

| | value |
|---|---|
| total cycles | **6 131 444** |
| descriptor submitted / completed / rejected | 1 / 1 / 0 |
| elements done / total | 1 199 876 / 1 199 876 |
| completion | `conservation=OK`, `conservation_violations=0`, `bulk_issue_monotonic_violations=0`, `bulk_residency_violations=0`, exit 0, liveness never tripped |
| bulk address faults / mismatches | 0 / 0 |

## 5. Speedup

```
M0-RM       17 904 629 cycles
M27 Bulk     6 131 444 cycles
speedup      17 904 629 / 6 131 444 = 2.9201×
```

Historical M24-D reference on the same input: 17 904 629 / 6 130 494 = **2.9206×**.
Difference: **−0.016 %** (M27 Bulk is 950 cycles, +0.0155 %, slower than the historical Bulk run).
§8 decomposes that difference.

---

## 6. Resource and translation statistics

### 6.1 Translation

| counter | M0-RM | M27 Bulk |
|---|---|---|
| dTLB reads | 1 200 003 | 898 742 |
| dTLB hits | 499 100 (41.6 %) | 706 677 (78.6 %) |
| dTLB misses | 700 903 (58.4 %) | 192 065 (21.4 %) |
| page walks | 311 592 | 107 419 |
| walker busy (walker-cycles) | 93 477 600 | 32 225 700 |
| dTLB MSHR stall cycles | **69 725 269** | 6 121 523 |
| dTLB `refuse_full` / `refuse_parked` | 30 607 069 / 39 118 200 | 1 924 646 / 4 196 877 |
| L2 TLB reads / hits / misses | 525 367 / 213 775 / 311 592 | 107 579 / 160 / 107 419 |
| **max translation concurrency** | 8 dTLB MSHRs (saturated), 8 walkers, 8 L2-TLB outstanding | same caps; loads far below them |

Translation is the dominant cost in M0: 58 % of its requests miss the dTLB, it takes 311 k walks, and
it loses **69.7 M** issue-cycles to MSHR/refusal back-pressure. In M27 Bulk the engine's 823 621
translations are dense enough that 78.6 % hit the dTLB (the window's page locality), cutting walks by
65.5 % and translation stalls by 91.2 %.

### 6.2 Cache

| counter | M0-RM | M27 Bulk |
|---|---|---|
| L1 reads | 1 200 003 | 898 742 |
| L1 hits / misses | 25 355 (2.1 %) / 1 174 648 | 49 (0.005 %) / 898 693 |
| L1 evictions | 1 174 354 | 898 421 |
| L1 MSHR max / occupancy (req-cycles) | 78 / 128 · 70 922 296 | 77 / 128 · 385 700 007 |
| L1 stall cycles | 22 634 737 | 265 916 374 |
| L2 reads | 1 797 063 | 1 011 948 |
| L2 hits / misses | 1 277 389 (71.1 %) / 519 674 | 366 929 (36.3 %) / 645 019 |
| L2 evictions | 502 925 | 627 904 |
| L2 MSHR max / occupancy | 64 / 64 · 67 960 840 | 64 / 64 · 383 695 708 |

The Bulk stream has **no cache reuse to speak of** (L1 hit 0.005 %, L2 hit 36.3 % against 2.1 %/71.1 %
for M0). That is the cost side of batching: requests are released in page-cohort order over the whole
run, so temporal locality at the L1/L2 is largely destroyed. It is not a defect in the engine; it is
the trade the architecture makes, and §6.3 shows what it buys.

### 6.3 Memory system

| counter | M0-RM | M27 Bulk |
|---|---|---|
| DRAM reads | 519 309 | 644 288 |
| DRAM writes | 0 (read-only model) | 0 |
| row hits | 155 404 (29.9 %) | **390 024 (60.5 %)** |
| row conflicts / activations | 363 903 / 363 905 | 254 262 / 254 264 |
| rows used (of 8 192 cells) | 8 181 | 8 185 |
| bank busy (bank-cycles) | 11 637 186 | 12 071 200 |
| **bank utilization (2 banks)** | **32.5 %** | **98.4 %** |
| bytes moved | 33 235 776 (0.74 GB/s) | 41 234 432 (2.69 GB/s, computed) |
| max queue depth | 63 | 63 |
| mean DRAM wait | 105.5 cycles | 573.8 cycles |
| DRAM-starved cycles | 12 299 097 (68.7 %) | 50 408 (0.8 %) |

Per-client DRAM attribution confirms the intended routing of the bulk path:

| client | M0-RM reads | M27 Bulk reads |
|---|---|---|
| 0 (SIMT) | 518 996 | 62 |
| 1 (index stream) | – | 74 993 |
| 2 (page walks / PTE) | 313 | 278 |
| 3 (bulk targets) | – | 568 955 |

**M0-RM is not memory-bound; it is supply-bound.** The DRAM sits idle 68.7 % of the time, the 128-slot
residency pool is full 17 601 266 cycles (98.3 % of the run, `simt_slot_full_cycles`), and translation
back-pressure alone accounts for 69.7 M stall cycles. **M27 Bulk is memory-bound**: both banks are busy
98.4 % of the time and the DRAM is starved in only 0.8 % of cycles. The 2.92× is therefore a
*utilization* conversion, not a bytes reduction — the Bulk arm actually moves **24 % more bytes**
(41.2 MB vs 33.2 MB) and still finishes 2.92× sooner, because it raises effective bandwidth from
1.86 B/cycle to 6.72 B/cycle.

### 6.4 Residency budget

| counter | M0-RM | M27 Bulk |
|---|---|---|
| slots | 128 | 128 |
| peak slot occupancy | 128 | 128 |
| `inflight_max` | 128 | 128 |
| admission refusals | `simt_issue_stalls = 69 725 269`, pool full 98.3 % of cycles | `bulk_slot_stall = 5 893 832` (= `refuse_reserve` 5 893 348 + `refuse_noslot` 484) |

Both arms reach the shared ceiling of 128, which is the point of the M27 fairness fix: the engine now
competes for the same slots as everyone else, and in this cell it still reaches 128 while the SIMT
side keeps its own requests admitted (127 SIMT elements, all served). `bulk_slot_take = bulk_slot_give
= 7 447 700` — every acquisition is released.

---

## 7. Dispatch behaviour (Bulk path)

| counter | value |
|---|---|
| SIMT instructions / elements | 43 / **127** (0.01 % of elements) |
| Bulk instructions / elements | 301 148 / **1 199 876** |
| windows / batches | 74 / 74 (`win_size_mean` 16 214, median 16 384, min 3 844 — last window partial) |
| window entries | 1 199 876 |
| intra-line dedup merges | 376 255 (31.4 % of elements) |
| target line requests generated / issued / completed | 823 621 / 823 621 / 823 621 (1.46 elements per line) |
| index stream | 74 993 lines, 2 720 bytes, resident max 64, `idx_stall` 709 372 cycles |
| page cohorts opened | 106 301 over 2 048 pages (51.9 per page) |
| scheduler cohorts / opens / switches / scans | 8 / 585 776 / 585 776 / 823 621 |
| cohort admissions accepted / refused | 823 621 / 5 725 464 (`refuse_parked` 3 840 103, `refuse_full` 1 885 361) |
| deferrals readmitted / max defer age | 137 123 / 15 713 cycles |
| ready pool peak / mean occupancy | **4 096** (at capacity) / 2 047 |
| ready-pool back-pressure / no-ready cycles | 116 698 / 103 423 |
| translations requested / completed | 823 621 / 823 621 |
| release inversions vs physical order | 519 of 823 547 pairs (0.06 % — sorter off, page order on) |
| DRAM-sorter runs | **0** |
| descriptor | 1 submitted, 1 completed, 0 rejected |

**Dispatch is essentially 100 % Bulk on this trace** (127 of 1 200 003 elements go to SIMT), because
the annotation is per instruction and Orkut BFS is irregular by construction. So this cell measures
"the Bulk architecture on a bulk-eligible workload", not a realistic mixed dispatch. That is the
intended meaning of the M24-D comparison, and it must be stated whenever the 2.92× is quoted.

Window dedup removes 31.4 % of element requests (1 199 876 → 823 621 lines), and page cohorts turn
those into 60.5 % row hits. The engine is throttled more by its own ready-pool capacity (4 096, at the
ceiling, 116 698 back-pressure cycles) than by the memory system (0.8 % starved).

---

## 8. Comparison against historical M24-D results

| run | cycles | issued | DRAM reads | row hits | conflicts | `bulk_org_runs` |
|---|---|---|---|---|---|---|
| M24-D Bulk (historical, preserved) | 6 130 494 | 898 763 | 644 142 | 389 893 | 254 247 | 823 604 |
| **M27, same historical config** (sorter ON) | 6 130 567 | 898 725 | 644 230 | 390 074 | 254 154 | 823 604 |
| **M27 production** (`configs/m27_bulk.cfg`) | **6 131 444** | **898 742** | **644 288** | **390 024** | **254 262** | **0** |
| M24-D M0-RM (historical) | 17 904 629 | 1 200 003 | 519 309 | 155 404 | 363 903 | – |
| **M27 M0-RM** | **17 904 629** | 1 200 003 | 519 309 | 155 404 | 363 903 | – |

| | speedup |
|---|---|
| historical M0-RM vs G6D-S1 | 2.9206× |
| **M27 M0-RM vs M27 Bulk** | **2.9201×** |

**Decomposition of the −0.016 % (950 cycles).**

1. **+73 cycles (+0.0012 %)** — the M27 correctness and accounting fixes with the sorter still on
   (historical config today vs the historical log). Cause: translation probes now hold a residency
   slot for their lifetime, so target admissions are occasionally refused or deferred where they were
   previously waved through. This is the fix working: those probes were consuming the same dTLB, L2
   TLB, walkers and DRAM before; they simply no longer also occupy the slot pool for free.
2. **+877 cycles (+0.014 %)** — removing the DRAM sorter (`org_runs` 823 604 → 0). The sorter was
   mildly *helpful* on this cell: without it, row hits fall 389 893 → 390 024 (noise) but conflicts
   rise 254 154 → 254 262 and reads rise 644 230 → 644 288. The effect is 0.014 % — noise-level. This
   matches the M26/M27 conclusion that under M0 with an 8 MiB working set at 8 MiB of capacity there
   is almost no row sharing left to exploit, and it is why the sorter is no longer in the production
   path.
3. **Request counts moved slightly** (issued 898 763 → 898 742, −21, i.e. −0.002 %). See §9.2: `issued`
   in the hybrid arm is the sum of four streams, and the slot-accounting change perturbs the
   interleaving by a handful of admissions. It is not a workload difference: `trace_instr`,
   `trace_lines`, `elements`, `simt_elems`/`bulk_elems` and `dispatch_order` are identical.

**Conclusion of §8: the M27 corrections cost 0.016 % on this cell and change no conclusion.** The
2.92× stands, now measured on a simulator whose translation path cannot strand a request, whose
liveness is checked on every run, and whose engine pays for the resources it uses.

---

## 9. Unexpected behaviour and anomalies

1. **`lines_per_cycle=0.000` and `dram_gbs=0.00` are printed for the hybrid arm.** Reporting only: the
   printed rate fields are not filled by `run_hybrid` (they read `RunResult` fields the SIMT path
   sets). Computed from the raw counters, the Bulk arm moves 41 234 432 bytes in 6 131 444 cycles =
   **2.69 GB/s** at the model's 400 MHz. No effect on cycles or counters; pre-existing, not introduced
   by M27. Worth fixing in the reporting layer before these numbers are quoted anywhere.
2. **`issued` is not the same quantity in the two arms.** M0-RM: 1 200 003 SIMT requests. M27 Bulk:
   898 742 = 1 descriptor + 74 993 index lines + 823 621 target lines + 127 SIMT requests. Comparing
   the two raw numbers as "requests" is wrong; the meaningful figure is requests per element,
   1.000 (M0) vs 0.749 (Bulk). This also **resolves the open item from the M27 implementation report**:
   the ±5/±21 differences against the preserved logs are stream-composition and interleaving, not a
   workload difference.
3. **`lat_mean` = 14 758 (Bulk) vs 186 (M0).** Not a latency regression. The Bulk request clock starts
   at phase-1 submission (`r.gen`), so it includes the deliberate residence in the ready pool (mean
   occupancy 2 047 runs) before release. The Bulk architecture trades per-request latency for
   throughput by design; end-to-end latency is not comparable between the arms and should not be
   quoted as one.
4. **The ready pool is at its ceiling** (peak 4 096 of 4 096, 116 698 back-pressure cycles). The engine
   is limited here by its own translation/release rate, not by the DRAM. This is a candidate for the
   *next* study (is 4 096 the right capacity for this window size?), not something to change now.
5. **The M0-RM baseline is itself resource-starved** (slot pool full 98.3 % of cycles, 69.7 M
   translation stall cycles). That is a property of conventional SIMT on this trace at 128 slots, not
   a misconfiguration — but it means the 2.92× measures "starved conventional" vs "saturated engine".
   A reviewer may reasonably ask whether 128 slots is the right resource match for a client that must
   cover a 150-cycle walk latency; **I did not change it**, and the question is recorded here for
   review.
6. **Dispatch is 99.99 % Bulk** (127 of 1.2 M elements to SIMT). The static annotation is doing what
   M25 said it does; the number describes a bulk-eligible workload, not a mixed one.

---

## 10. Conclusion and recommended next step

**What was established.** On the established Orkut BFS cell, with everything else held fixed:

- M0-RM completes in **17 904 629** cycles; M27 Bulk in **6 131 444**; speedup **2.9201×**.
- Both runs are **valid**: conservation OK, zero residency/issue/monotonic violations, liveness armed
  and untripped, descriptor completed, all 1 199 876 elements done.
- The M27 corrections (TLB retention, permanent liveness, probe slot charging, sorter removal,
  single-core guard) **change the historical figure by −0.016 %** — within noise, and fully explained.
- The mechanism of the win is **utilization, not traffic**: bank utilization 32.5 % → 98.4 %,
  effective bandwidth 1.86 → 6.72 B/cycle, despite moving 24 % more bytes. Window dedup (−31.4 %
  requests) and page cohorts (29.9 % → 60.5 % row hits) are what the engine converts the idle
  bandwidth into; the loss of L1/L2 reuse (71.1 % → 36.3 % L2 hits) is the price it pays.

**What this does not establish.** One cell, one workload, one repeat-free measurement; a baseline that
is translation-starved; a dispatch mix that is 99.99 % Bulk; and no accounting for run-to-run noise.

**Recommended next step, in order, on your authorization only:**

1. **Repeat this pair once** and compare, to establish the run-to-run noise floor before any number is
   quoted with a decimal place. (The historical set has `__repeat` logs for exactly this reason.)
2. **Fix the reporting fields** (`lines_per_cycle`, `dram_gbs`, and `issued` decomposition) so future
   comparison tables cannot be misread — a reporting change only, no model change.
3. **Then** the second cell (Criteo 16K) as a paired comparison, to see whether the effect is
   workload-specific.

No further workload has been run. Stopping here for review.
