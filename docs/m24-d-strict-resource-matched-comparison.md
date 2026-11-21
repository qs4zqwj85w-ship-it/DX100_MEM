# M24-D — Strict resource-matched comparison: conventional SIMT vs. M24 Bulk

**Scope.** This milestone evaluates two architectures that already exist. It does not
introduce a mechanism, does not tune a policy and does not change a single simulator source
line. It asks one question and tries to make the answer unarguable:

> With every hardware resource the two architectures share set to the same value, how much
> of M24 Bulk's advantage over conventional SIMT survives?

**Answer.** Between 1.27× and 2.92× of it, depending on the workload. None of that
remainder is capacity: the two configurations differ in eight fields and every one of them
is a Bulk organizational mechanism. After matching 33 resource rows / 44 resource fields —
page walkers, dTLB and L2 TLB MSHRs, both cache hierarchies and their MSHR files, the walk
cache, page-walk latency, the global outstanding budget, DRAM geometry, timing, mapping and
queue — **zero common-resource mismatches remain**, and the residual advantage is explained
by two measured effects: 1.27–2.90× fewer page walks, and a DRAM stream that hits an open
row 11.0–60.5 % of the time where SIMT manages 0.2–29.9 %.

## 1. Result

Speedups are execution-cycle ratios, so larger is better.

| comparison | Orkut BFS S1 | LiveJournal | ogbn-products | Criteo |
|---|---|---|---|---|
| historical M0 vs. Bulk G6D S1 | 8.004 | 8.895 | 4.998 | 8.726 |
| **resource-matched M0-RM vs. Bulk G6D S1** | **2.921** | **2.516** | **1.266** | **2.222** |

The first row is the historical claim and it is not a like-for-like comparison: M0 ran with
2 page walkers and 4 dTLB MSHRs while Bulk ran with 8 and 8. The second row is the only
comparison this milestone endorses. Resource matching removes between two-thirds and
three-quarters of the historical gap (2.921 / 8.004 = 0.365 on Orkut, 0.253 on
ogbn-products), which is the headline: **most of what M23 measured was hardware, not
architecture.**

What is left is real but narrower, and §6 explains why it varies by 2.3× across workloads.
It is not free — Bulk reads 14.5–24.0 % more memory lines because it walks an index array
SIMT never touches — and it is not universal: on these same matched resources the fused
2×8K, 2×16K and 2×32K configurations **mostly lose** to M0-RM, and only G6D S1 is faster on
all four workloads (§9, quoted from M24-C, not re-measured). The advantage belongs to the
G6C/G6D organization specifically.

## 2. The configuration-difference audit

`tools/m24d_config_audit.py` produces `build/wm/m24d/config_audit.txt`. It reads the
configuration files and the simulator's own effective-configuration echo, never a previous
report's summary, and it fails on any undeclared difference.

### 2.1 What was audited

Three independent checks over 98 declared keys:

| check | source | result |
|---|---|---|
| A. the two new configs, comment-stripped | `m24d_simt.cfg` vs `m24d_bulk.cfg` | same key set, same order, **8 differing values** |
| B. each new config vs. its historical counterpart | `m24c_m0_rm.cfg`; `m23g6b_base.cfg` + the G6D S1 command line | **every resource row unchanged**; 5 declared setting deltas |
| C. the effective configuration the model ran | `== configuration ==` echo of the 8 primary logs | 86 echoed keys, **7 differ** |

The key budget is 98 = 44 resource keys in 33 rows + 46 settings + 8 architecture fields.
The echo is a subset (86 keys) because it prints capacities rather than every knob: it is
the check that the file was believed by the model, not just written down.

### 2.2 The eight fields that differ — all architecture

| field | SIMT | Bulk | what it is |
|---|---|---|---|
| `dispatch` | 0 | 1 | no engine / the M5 indirect-access engine |
| `batch_window` | 0 | 16384 | no context window / 16K-entry fused window |
| `win_fused` | 0 | 1 | no fused representation / one entry per line, elements packed behind it |
| `page_sched` | 0 | 1 | no page scheduler / translation-aware cohort scheduler |
| `page_cohorts` | 0 | 8 | no cohorts / 8 page runs open at once |
| `dedup` | 0 | 1 | no fold / elements sharing a line served by one request |
| `page_order` | 0 | 1 | no sealing order / pages ascending, lines ascending within page |
| `bulk_org` | 0 | 8 | no organization / M23-G6D streaming release over M23-G6C cohorts |

Nothing else differs. In particular `win_policy` is `255` (unset) in both files, so the
effective policy is *derived* from `dispatch` — E0 for SIMT, E3 for Bulk — rather than being
a ninth independent difference.

### 2.3 The resource rows — identical, and equal to the historical values

Every row below is read from the two new files and from both historical files, and must
equal the audited value or the tool fails. The `M0-RM` and `G6D S1` columns prove the new
files did not silently re-provision anything.

| resource | value | SIMT | Bulk | M0-RM | G6D S1 |
|---|---|---|---|---|---|
| cores / warps per core | 1 / 4 | 1/4 | 1/4 | 1/4 | 1/4 |
| lane width / element bytes | 4 / 4 | 4/4 | 4/4 | 4/4 | 4/4 |
| global outstanding budget `R` | 128 | 128 | 128 | 128 | 128 |
| SIMT pending slots (LSU) | 0 | 0 | 0 | 0 | 0 |
| memory path | l1 | l1 | l1 | l1 | l1 |
| L1 lines / ways / latency | 256 / 4 / 2 | 256/4/2 | 256/4/2 | 256/4/2 | 256/4/2 |
| L1 MSHRs | 128 | 128 | 128 | 128 | 128 |
| L2 lines / ways / latency | 16384 / 8 / 4 | 16384/8/4 | 16384/8/4 | 16384/8/4 | 16384/8/4 |
| L2 MSHRs | 64 | 64 | 64 | 64 | 64 |
| dTLB entries / ways | 16 / 4 | 16/4 | 16/4 | 16/4 | 16/4 |
| dTLB MSHRs | 8 | 8 | 8 | 8 | 8 |
| dTLB parked per entry | 2 | 2 | 2 | 2 | 2 |
| L2 TLB entries / ways | 512 / 4 | 512/4 | 512/4 | 512/4 | 512/4 |
| L2 TLB MSHRs | 8 | 8 | 8 | 8 | 8 |
| L2 TLB latency | 4 | 4 | 4 | 4 | 4 |
| page walkers | 8 | 8 | 8 | 8 | 8 |
| walk cache entries | 16 | 16 | 16 | 16 | 16 |
| page-table levels | 2 | 2 | 2 | 2 | 2 |
| walk latency per level | 150 | 150 | 150 | 150 | 150 |
| ideal translation | 0 | 0 | 0 | 0 | 0 |
| DRAM banks / rows | 2 / 4096 | 2/4096 | 2/4096 | 2/4096 | 2/4096 |
| DRAM row bytes | 1024 | 1024 | 1024 | 1024 | 1024 |
| DRAM interleave granule | 1 | 1 | 1 | 1 | 1 |
| DRAM `t_hit` / `t_miss` | 14 / 26 | 14/26 | 14/26 | 14/26 | 14/26 |
| DRAM link width | 0 | 0 | 0 | 0 | 0 |
| DRAM queue depth | 0 | 0 | 0 | 0 | 0 |
| FR-FCFS | 1 | 1 | 1 | 1 | 1 |
| XOR bank hash | 1 | 1 | 1 | 1 | 1 |
| DRAM address map | 0 | 0 | 0 | 0 | 0 |
| page size | 4096 | 4096 | 4096 | 4096 | 4096 |
| memory hierarchy kind | ddr | ddr | ddr | ddr | ddr |
| physical map / permutation seed | 0 / 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| engine release organization | 1 | 1 | 1 | 1 | 1 |

Two rows deserve a note. `dram_kind = ddr` in both, so the M24-B HBM hierarchy is not
instantiated and neither file sets an `hbm_*` key; the two runs therefore take the same
defaults for all 23 HBM parameters, which the echo shows on its `hbm=` line. And
`m24_dram_org = 1` (physical-order release) is set in both because the engine reads it only
when an engine exists; setting it identically removes it from the diff.

### 2.4 The 46 settings that are identical in both files

`core_map=round_robin`, `experiment=none`, `op_class=gen`, `dispatch_order=0`,
`ag_lanes=8`, `eng_issue=4`, `idx_window=0`, `desc_fetch=1`, `idx_oracle=0`, `idx_mode=2`,
`idx_buf_lines=64`, `idx_mshr=64`, `idx_reserve=8`, `page_dir=512`, `org_lanes=4`,
`fanout_lanes=4`, `batch_deadline=4096`, `org_overlap=1`, `park_retry=1`,
`bulk_stream=0`, `win_policy=255`, `win_abs_cap=4`, `g6a_pa_capacity=65536`,
`g6b_pa_capacity=4096`, `sched_vis=0`, `sched_group=0`, `g6i_diag=0`, `cache_diag=1`,
`dram_diag=1`, `pool_slots=0`, `ab_charge_latch=0`, `m23_select=`, `wl=W1`, `N=16384`,
`Lambda=16384`, `P=512`, `kappa=0`, `rho_dup=0`, `seed=1`, `bulk_wl=E0`,
`bulk_index_base=0x80000000`, `bulk_data_base=0`, `bulk_desc_line=0x1FFFFC0`,
`bulk_max_elems=16777216`, `max_cycles=500000000`, `skip_b=0`.

The engine's geometry (`ag_lanes`, `eng_issue`, `org_lanes`, `fanout_lanes`, `page_dir`,
`idx_*`) is stated identically in both files because it is inert at `dispatch = 0`, not
because SIMT is being given an engine: with no engine constructed, those keys allocate
nothing and drive nothing.

### 2.5 Resources with no SIMT analogue — present on one side only

These are not mismatches to be corrected; matching them would be implementing Bulk inside
the baseline, which the brief forbids.

| Bulk resource | SIMT analogue | decision |
|---|---|---|
| 4096-entry streaming ready pool (`g6b_pa_capacity`) | none — SIMT's L1/L2 MSHRs are its only request buffers | declared identically (4096) in both files but only the engine allocates it; not given to SIMT |
| 8 open page cohorts (+32 B descriptor each) | none — SIMT generates requests warp by warp | not copied |
| index acquisition unit (`idx_mode`, `idx_buf_lines`, `idx_mshr`, `idx_reserve`) | none — the trace carries element addresses | not copied; the engine's index traffic is counted against it in §5.5 |
| dedup / reuse directory | none | not copied |
| physical-row organizer (`bulk_org`) | FR-FCFS over an open queue | both keep their own scheduler; FR-FCFS is not removed from SIMT |

"Resource-matched" therefore means **matched in every resource both architectures possess**.
That definition is the honest one, and it is why §9 lists what it cannot equalise.

### 2.6 M24-C's "six fields", re-audited

M24-C §11 listed six fields in which G6D S1 differed — but that list was written for
**G6D S1 against 2×16K**, not for G6D S1 against M0-RM. Re-auditing from the files:

| M24-C claim (G6D vs. 2×16K) | state against M0-RM, audited here |
|---|---|
| `walkers` | **matched** (8 = 8) and still matched |
| `dtlb_mshr` | **matched** (8 = 8) and still matched |
| `page_cohorts` | differs 0 vs. 8 — architecture, remains |
| `bulk_org` | differs 0 vs. 8 — architecture, remains |
| `g6b_pa_capacity` | **equal** (4096 = 4096); it was a difference against 2×16K, which never allocates the buffer, not against M0-RM |
| different base file (`m23_dx16k` vs. `m23g6b_base`) | a provenance difference, resolved: the two M24-D files are one template, 8 lines apart |

So the count against M0-RM is not six but **eight**, every one of them a Bulk mechanism, and
the two that M24-C had already matched stay matched. No common-resource mismatch was
outstanding, which is why this milestone corrects nothing in §3.

### 2.7 Differences against the historical files, classified

The brief asks for four classes. Against `m24c_m0_rm.cfg` the SIMT file differs in five
declared non-resource fields; against `m23g6b_base.cfg` + the G6D S1 command line the Bulk
file differs in one.

| class | field | pinned SIMT vs. M0-RM | pinned Bulk vs. G6D S1 | why it is not a confounder |
|---|---|---|---|---|
| common resource mismatch | — | **none** | **none** | 33 rows in §2.3 |
| architecture-specific mechanism | 8 fields | identical to M0-RM | identical to G6D S1 | that is the comparison |
| workload / simulator setting | `max_cycles` | 200 000 000 → 500 000 000 | 3 000 000 000 → 500 000 000 | one guard for both arms; the slowest cell that completes is 176 184 939 |
| workload / simulator setting | `bulk_index_base` | 0x00800000 → 0x80000000 | same | inert at `dispatch = 0`; the 2 GiB base is what every published trace cell used and what keeps the 2M-element index array clear of the trace |
| workload / simulator setting | `bulk_desc_line` | 98304 → 0x1FFFFC0 | same | inert at `dispatch = 0` |
| workload / simulator setting | `bulk_max_elems` | 65536 → 16777216 | unchanged | a descriptor validation limit, larger than every trace; caps nothing |
| workload / simulator setting | `dram_diag` | 0 → 1 | unchanged (1) | diagnostics only; no decision reads it |
| non-performance metadata | comments, headers | rewritten | rewritten | no key changes; the audit compares parsed keys |

The classification is not asserted, it is demonstrated: the pinned files reproduce the
published M24-C cells **to the cycle and to the completion hash** on all four workloads
(§8.4), so none of these deltas is performance-relevant.

## 3. Why no configuration value was changed

The brief says to correct genuine common-resource mismatches in new configurations only. The
audit found none to correct: M24-C's two-field change (`walkers` 2→8, `dtlb_mshr` 4→8)
really did close every shared capacity, and the historical M0 had never been short of cache,
MSHR, DRAM or request budget.

What this milestone adds is not a correction but a **reproducible proof**. Two files are
generated from one template and differ in exactly the eight architecture lines:

```
$ diff <(grep -v '^#' m24d_simt.cfg | grep -v '^$') \
       <(grep -v '^#' m24d_bulk.cfg | grep -v '^$')
46,53c46,53
< dispatch = 0
< batch_window = 0
< win_fused = 0
< page_sched = 0
< page_cohorts = 0
< dedup = 0
< page_order = 0
< bulk_org = 0
---
> dispatch = 1
> batch_window = 16384
> win_fused = 1
> page_sched = 1
> page_cohorts = 8
> dedup = 1
> page_order = 1
> bulk_org = 8
```

Same binary, same defaults, same 90 other lines ⇒ the effective configurations can differ
in at most those eight keys, and the model's own echo confirms exactly which of them move.
Because every field is pinned, no value depends on a compiled default that could drift
between the two arms.

The historical files are untouched: `sim/configs/m24c_m0_rm.cfg` and
`sim/configs/m23g6b_base.cfg` are byte-identical to what M24-C and M23 published, and the
G6D S1 cell still runs through `build/wm/m23g6d_run.sh`'s command line.

## 4. Matrix and invocation

| stage | cells | what it is for |
|---|---|---|
| `primary` | 8 | 2 arms × 4 workloads, the comparison |
| `repeat` | 8 | the same cells re-run, determinism |
| `map` | 24 | `m24_map = 3` (seeded physical-page permutation) with seeds 1, 2, 3 — 3 mappings × 4 workloads × 2 arms |
| `seedgen` | 6 | synthetic W1 generator, seeds 1, 7, 13 × 2 arms |

46 cells, `build/wm/m24d_run.sh all`, five at a time. The command line carries exactly one
argument that could differ between the arms, and it is the trace:

```
./bin/dx100sim --config ../build/wm/m24d/m24d_<arm>.cfg \
  --set dataset=../build/wm/m23_<trace>.bin --all
```

Traces, identical for both arms: `m23_orkut_bfs.bin`, `m23_livejournal_bfs.bin`,
`m23_ogbn.bin`, `m23_criteo_16k.bin`; 4 KiB pages, seed 1, the same physical map, the same
caches, DRAM and timing — all of which come from the identical shared lines of the two
files. `bulk_max_elems` is pinned at 16 777 216 in both, above every trace's element count,
so the index-array size is never a hidden difference.

## 5. Results

### 5.1 Execution cycles

| workload | M0 (historical) | SIMT (M0-RM) | Bulk G6D S1 | SIMT / Bulk |
|---|---|---|---|---|
| Orkut BFS S1 | 49 067 545 | 17 904 629 | 6 130 494 | 2.921 |
| LiveJournal | 67 234 692 | 19 020 756 | 7 558 966 | 2.516 |
| ogbn-products train | 176 184 939 | 44 637 463 | 35 248 957 | 1.266 |
| Criteo embeddings | 28 511 774 | 7 260 692 | 3 267 387 | 2.222 |

The M0 column is quoted from the published M24-C cells (which reproduced the pre-M24 logs
exactly); M0 is not re-measured here because nothing about it changed.

### 5.2 The three comparisons the brief asks to keep apart

| workload | historical M0 / Bulk | **M0-RM / Bulk** | M0 / M0-RM (resource) | fraction of the historical gap left |
|---|---|---|---|---|
| Orkut | 8.004 | **2.921** | 2.740 | 0.365 |
| LiveJournal | 8.895 | **2.516** | 3.535 | 0.283 |
| ogbn-products | 4.998 | **1.266** | 3.947 | 0.253 |
| Criteo | 8.726 | **2.222** | 3.927 | 0.255 |

On Orkut, LiveJournal and Criteo the resource benefit alone (2.740–3.947×) is **larger than
the entire Bulk speedup**, which is the strongest statement in this report: the historical
comparison was measuring walker count and dTLB MSHR count more than it was measuring Bulk.

### 5.3 Where the remaining advantage comes from

Page walks, the same traces and the same eight walkers in both arms:

| workload | walks SIMT | walks Bulk | walk ratio | cycle ratio | DRAM row hit SIMT → Bulk |
|---|---|---|---|---|---|
| Orkut BFS S1 | 311 592 | 107 405 | 2.901 | 2.921 | 29.9 % → 60.5 % |
| LiveJournal | 441 523 | 187 108 | 2.360 | 2.516 | 16.4 % → 46.2 % |
| ogbn-products train | 1 174 411 | 927 116 | 1.267 | 1.266 | 0.2 % → 11.0 % |
| Criteo embeddings | 189 253 | 81 616 | 2.319 | 2.222 | 5.9 % → 32.7 % |

On ogbn-products the walk ratio and the cycle ratio agree to 0.03 % (1.267 vs. 1.266): that
workload is almost purely a translation-work problem, and the whole Benefit of Bulk there is
translating each page once per cohort instead of once per warp-level miss. Elsewhere the
cycle advantage is smaller than the walk advantage (Criteo) or slightly larger (LiveJournal,
Orkut), because the two arms pay different costs per walk and get different DRAM service —
§5.4 and §5.5.

### 5.4 Translation

| workload | arm | walks | dTLB hit | L2 TLB hit | walker busy | latency p50 / p95 |
|---|---|---|---|---|---|---|
| Orkut | simt | 311 592 | 41.6 % | 40.7 % | 5.22 | 84 / 658 |
| Orkut | bulk | 107 405 | 78.6 % | 0.2 % | 5.26 | 13 364 / 31 952 |
| LiveJournal | simt | 441 523 | 18.9 % | 46.1 % | 6.96 | 133 / 425 |
| LiveJournal | bulk | 187 108 | 60.5 % | 0.2 % | 7.43 | 7 315 / 32 939 |
| ogbn-products | simt | 1 174 411 | 0.1 % | 1.6 % | 7.89 | 337 / 365 |
| ogbn-products | bulk | 927 116 | 10.4 % | 0.0 % | 7.89 | 384 / 658 |
| Criteo | simt | 189 253 | 3.9 % | 41.2 % | 7.82 | 306 / 337 |
| Criteo | bulk | 81 616 | 39.8 % | 9.6 % | 7.50 | 309 / 857 |

The mechanism is visible in the dTLB column: cohorts keep 8 page runs open and issue them
page-major, so consecutive requests hit the same dTLB entry (78.6 % against 41.6 % on
Orkut). The same walkers are then asked for fewer walks. The L2 TLB hit rate falls to
0.2–9.6 % precisely because there are almost no L2 TLB misses left to hit.

Bulk's p50 translation latency is far larger (13 364 vs. 84 cycles on Orkut) because the
engine queues a whole window's worth of page runs and measures from admission, while SIMT's
counter starts at the miss. These are not the same distribution and are not compared as
such; the only quantity comparable across arms is the number of walks.

### 5.5 Memory system

| workload | arm | reads | row hit | activations | bank busy | queue depth | L2 reads | L2 hit | GB/s |
|---|---|---|---|---|---|---|---|---|---|
| Orkut | simt | 519 309 | 29.9 % | 363 905 | 32.5 % | 3.06 | 1 797 063 | 71.1 % | 1.856 |
| Orkut | bulk | 644 142 | 60.5 % | 254 249 | 98.4 % | 60.31 | 1 011 218 | 36.2 % | 6.725 |
| LiveJournal | simt | 600 717 | 16.4 % | 502 429 | 38.0 % | 2.06 | 1 847 407 | 67.4 % | 2.021 |
| LiveJournal | bulk | 692 695 | 46.2 % | 372 338 | 93.7 % | 53.31 | 1 042 562 | 33.3 % | 5.865 |
| ogbn-products | simt | 1 194 359 | 0.2 % | 1 192 564 | 34.8 % | 0.20 | 3 548 478 | 66.3 % | 1.712 |
| ogbn-products | bulk | 1 367 453 | 11.0 % | 1 217 711 | 47.9 % | 3.57 | 3 104 956 | 47.2 % | 2.483 |
| Criteo | simt | 86 882 | 5.9 % | 81 721 | 15.1 % | 0.09 | 612 763 | 85.8 % | 0.766 |
| Criteo | bulk | 104 615 | 32.7 % | 70 438 | 35.3 % | 5.59 | 304 815 | 64.6 % | 2.049 |

Three readings:

1. **Bulk reads more** — +24.0 %, +15.3 %, +14.5 %, +20.4 % — because the index array is
   real traffic. This biases the comparison *against* Bulk and is not removable.
2. **Bulk activates fewer rows** — −30.1 %, −25.9 %, +2.1 %, −13.8 % — because the sealed
   window is released in physical (row, bank, line) order, so requests arrive at the
   controller already grouped. Orkut's activations fall by 30.1 % on 24.0 % *more* reads.
3. **Bulk fills the memory system.** Bank busy rises from 32.5 % to 98.4 % on Orkut and the
   queue from 3.06 to 60.31 entries — on ogbn-products, where nothing can be grouped
   (0.2 % row hit even with organization), bank busy only reaches 47.9 %.

The L2 hit rate falls in every Bulk cell because the engine's folded request stream asks for
far fewer lines (612 763 vs. 1 797 063 L2 reads on Orkut) and reuses each one less: the fold
removes the intra-line repeats that gave SIMT its hits, and the DRAM absorbs the rest.

### 5.6 Issue path

| workload | arm | lines / cycle | peak outstanding | tlb_refuse_full | replays |
|---|---|---|---|---|---|
| Orkut | simt | 0.0056 | 132 | 30 607 069 | 22 634 737 |
| Orkut | bulk | 0.0164 | — | 11 029 115 | 321 678 553 |
| LiveJournal | simt | 0.0108 | 132 | 56 295 768 | 9 056 286 |
| LiveJournal | bulk | 0.0271 | — | 28 013 679 | 343 953 367 |
| ogbn-products | simt | 0.0155 | 132 | 177 324 861 | 0 |
| ogbn-products | bulk | 0.0196 | — | 269 732 606 | 10 134 984 |
| Criteo | simt | 0.0088 | 132 | 27 253 723 | 0 |
| Criteo | bulk | 0.0197 | — | 22 499 895 | 4 319 811 |

`peak outstanding` is `simt_peak_outstanding`, the SIMT arm's own counter (128 slots plus a
4-deep latch); the engine's equivalent is `bulk_peak_slot`, which reaches at most 128 with
zero residency violations in every cell (§8.2). `replays` counts retries of a request the
translation layer refused; Bulk's values are up to an order of magnitude larger (321 678 553
against 22 634 737 on Orkut) because its release path re-probes a page run that is still
translating instead of stalling, which is the same trade the cohort scheduler makes in the
first place.

## 6. Why each workload lands where it does

**ogbn-products train — 1.266×, the floor.** 1.17 M walks for 1.20 M accesses: nearly every
access needs a translation, and both arms saturate the 8 walkers (7.89 busy). Bulk's only
lever is translating a page once per cohort instead of once per miss, which cuts walks to
927 116 — exactly the 1.267 ratio it then achieves. The memory side is untouched: 0.2 % row
hit for SIMT and 11.0 % for Bulk, bank busy 34.8 % → 47.9 %. There is nothing here for
organization to win, because the stream has no reuse to organize.

**Orkut BFS S1 — 2.921×, the ceiling.** 2.90× fewer walks from cohorts, and the ordering
turns a stream that hit an open row 29.9 % of the time into one that hits 60.5 %, at 98.4 %
bank busy and a queue depth of 60. The DRAM is essentially saturated while SIMT leaves it
two-thirds idle: this is the workload where organization has the most to work with, and it
gets both effects at once.

**LiveJournal — 2.516×.** Between the two: 2.36× fewer walks, row hit 16.4 % → 46.2 %,
bank busy 38.0 % → 93.7 %. Its cycle gain (2.516) exceeds its walk gain (2.360), so here the
DRAM side contributes positively rather than eating into the translation win.

**Criteo embeddings — 2.222×, the only cell where organization pays a visible price.** The
walk gain is 2.319× but the cycle gain is 2.222×: Bulk spends part of its translation win on
20.4 % more memory reads (index traffic is a large fraction of a 86 882-read stream) and on
latency p95 of 857 cycles. It still wins comfortably, because 32.7 % row hit against 5.9 %
and 35.3 % bank busy against 15.1 % more than repays the extra reads.

## 7. Repetitions

The four trace workloads are fixed files and take no seed; where repetition is possible it
is done both ways — three physical mappings, and three generator seeds.

### 7.1 Determinism

All eight primary cells were re-run and are identical in cycles **and** completion hash:
17 904 629 / 6 130 494 (Orkut), 19 020 756 / 7 558 966 (LiveJournal), 44 637 463 /
35 248 957 (ogbn), 7 260 692 / 3 267 387 (Criteo) — SIMT / Bulk, each twice.

### 7.2 Physical mapping (24 cells)

`m24_map = 3` permutes physical page frames by seed, so each repetition is a different
memory map — the same map in both arms, which is what the comparison requires.

| workload | seed 1 | seed 2 | seed 3 | min | median | max | map 0 |
|---|---|---|---|---|---|---|---|
| Orkut | 2.967 | 2.954 | 2.971 | 2.954 | 2.967 | 2.971 | 2.921 |
| LiveJournal | 2.529 | 2.530 | 2.530 | 2.529 | 2.530 | 2.530 | 2.516 |
| ogbn-products | 1.266 | 1.266 | 1.266 | 1.266 | 1.266 | 1.266 | 1.266 |
| Criteo | 2.206 | 2.225 | *excluded* | 2.206 | 2.225 | 2.225 | 2.222 |

The conclusion is map-invariant: every workload's speedup moves by at most 1.7 % against its
map-0 result (Orkut 2.921 → 2.954–2.971), and the ordering of the four workloads never
changes.
The one excluded cell is the subject of §8.3.

Two cells are worth flagging as *insensitive* rather than stable: the SIMT arm's cycles are
identical to the digit across all three mappings on ogbn-products (44 637 463) and within 11
cycles on Criteo (7 260 703 in all three, 7 260 692 at map 0). That is expected — those runs
are bounded by translation and L2 misses, not by which physical row a line lands in — and it
is a consistency check on the harness: a physical-page permutation must not change a run
that never reuses a row.

### 7.3 Generator seeds (6 cells)

The synthetic W1 generator does take a seed, so it is swept:

| seed | SIMT cycles | Bulk cycles | SIMT / Bulk |
|---|---|---|---|
| 1 | 213 419 | 129 037 | 1.654 |
| 7 | 214 449 | 129 037 | 1.662 |
| 13 | 214 150 | 129 037 | 1.660 |

SIMT varies by 0.5 % across seeds. Bulk does not vary **at all** — and that is a property of
this workload, not a harness artefact: 16 384 elements fill exactly one 16 384-entry window
(`bulk_batches = 1`), and the sealed window is released in physical order, so Bulk's cost is
a function of the *set* of lines, which is identical across seeds, while its arrival order is
not. The completion hashes differ (2 361 496 344 / 662 117 756 / …), which proves the three
traces really were different. The 1.65–1.66× ratio is therefore the order-insensitive part
of Bulk's advantage on a fully-fitting window; it is quoted as such and not as a fifth
workload result.

## 8. Validation

### 8.1 Conservation and request accounting

Every one of the 45 completing cells is checked against 28 identity families, and the tool
exits non-zero if any of them fails.

Common to both arms (10): `accesses = trace_accesses`; `pte_fetches = 2 × walks`;
`walks ≤ l2tlb_misses`; dTLB `hits + misses = reads`; L2 TLB `hits + misses = reads`;
L1 `hits + misses = reads`; L2 `hits + misses = reads`; DRAM `row hits + row conflicts + 2 =
reads`; `activations = row conflicts + 2`; `dram_reads = dram_arrivals`.

Capacity (5): dTLB MSHR peak ≤ 8, walker peak ≤ 8, L2 TLB MSHR peak ≤ 8, L1 MSHR peak ≤
128, L2 MSHR peak ≤ 64 — i.e. the populated structure never exceeds the capacity the config
declares.

SIMT only (4): `walks` equals `l2tlb_misses` to within the walks still in flight (≤ 8);
`simt_peak_outstanding ≤ 132`; `cc0_issue = cc0_done = requests_accepted =
requests_generated`; `requests_accepted ≤ accesses` (Criteo's repeated indices coalesce onto
requests already in flight, 360 326 of 638 976).

Engine only (9): `bulk_elems_done = bulk_elems_total`; `bulk_idx_elems_done =
bulk_idx_elems_req`; `bulk_idx_lines_done = bulk_idx_lines_req = bulk_idx_sub`;
`bulk_idx_consumed = bulk_elems_total`; `bulk_tgt_req_gen = ..._done = ..._issued`;
`bulk_lines_issued = bulk_tgt_req_gen`; `bulk_win_sealed = bulk_batches`;
`bulk_peak_slot ≤ 128`; `bulk_residency_violations = 0` — the last two are the proof that the
engine lives inside the same `R = 128` budget as SIMT rather than beside it.

### 8.2 Deadlock and guard

`max_cycles = 500 000 000` for both arms, and every cell that completes stays far below it:
the slowest is 176 184 939 (35 % of the guard). One cell does not complete and is reported
rather than dropped — see §8.3.

### 8.3 The one non-terminating cell

`bulk_criteo_map3_s3` (Bulk, Criteo, physical map seed 3) reaches the 500 M-cycle guard with
`conservation_ok = 0` and the simulator's own diagnosis
`VIOLATED [cycle limit exceeded (deadlock guard)] [SIMT: issued != completed (work left in
flight)] [slots still held when the run ended] [the hierarchy still had requests in flight]`.

It is **not a slowdown**. The workload is finished:

| quantity | value |
|---|---|
| elements served / total | 294 912 / 294 912 |
| windows sealed / batches | 19 / 19 |
| last window sealed at | cycle 2 981 048 |
| last element completed at | cycle 3 292 976 |
| DRAM reads | 104 169 |
| streaming ready pool peak | 4 096 (its declared capacity) |
| cycles reported | 500 000 000 |

So the target stream completes at 0.7 % of the guard and the model then ticks forever with
the ready pool pinned at its capacity — a termination defect in the M24 Bulk release path
under this one mapping. M24-D does not modify the Bulk architecture, so this is recorded, not
fixed: it is a known limitation of the arm under test, it is excluded from the statistics of
§7.2 (Criteo's distribution is over two mappings rather than three), and it does not affect
any primary cell.

### 8.4 The pinned configs are the published architectures

Each primary cell is compared against the M24-C log for the same arm and trace, requiring
equal cycles, equal completion hash and equal walks:

| workload | M0-RM published = pinned | G6D S1 published = pinned |
|---|---|---|
| Orkut | 17 904 629 | 6 130 494 |
| LiveJournal | 19 020 756 | 7 558 966 |
| ogbn-products | 44 637 463 | 35 248 957 |
| Criteo | 7 260 692 | 3 267 387 |

Eight of eight identical. This is what licenses §2.7: the five setting deltas of the SIMT
file and the one of the Bulk file provably change nothing.

### 8.5 The simulator was not modified

`sim/src/*.cpp` and `*.hpp` mtimes all precede this milestone (22:45–22:48 on 20 November;
M24-C's report is 23:15), and `./bin/dx100_tests` reports **4 924 checks, 0 failures** — the
M24-B suite unchanged. No M23 or M24-A/B/C report or configuration was edited.

### 8.6 Not run

No new microbenchmark; no refresh or tFAW sensitivity; the M24-B HBM hierarchy is not
exercised (`dram_kind = ddr` in both arms, as in every M23 number); the fused 2×8K/2×16K/
2×32K Bulk configurations are not re-measured (their M24-C cells are already on this same
hardware); and no alternative Bulk organization (G6A/G6H/G6I) is included. §7.3's synthetic
sweep is the only generated traffic; the four workloads are single traces.

## 9. Fairness of the comparison

**What is fair.** Every resource both architectures possess is identical, in the files and in
the model's own echo, and each arm is provably the published architecture it claims to be.
The traces, the address map, the cache and DRAM configuration, the timing parameters, the
guard and the instrumentation are one shared block of text.

**What cannot be made fair, and is therefore stated:**

1. **Bulk does more memory work.** It reads an index array SIMT never touches: +14.5 % to
   +24.0 % DRAM reads. Its cycles include that work. This biases *against* Bulk.
2. **Bulk does less translation work.** Cohorts translate a page once per run of lines
   rather than once per miss: 1.27–2.90× fewer walks. This is the mechanism under test, not a
   confounder, but it means "the same instruction stream" is not what the two arms execute.
3. **Latency distributions are not comparable.** Bulk's per-request translation latency
   (p50 13 364 cycles on Orkut) and SIMT's (84) start from different events. Only walk counts
   are compared across arms.
4. **Some Bulk resources have no SIMT analogue** (§2.5): the 4 096-entry ready pool, the
   cohort descriptors, the index machinery, the dedup directory. They are absent from the
   SIMT arm by design; giving them to it would be implementing Bulk in the baseline. So
   "resource-matched" means matched in every *shared* resource, and anyone claiming SIMT was
   denied something must name which of those five is a capacity rather than an organization.
5. **The result is specific to the G6C/G6D organization.** On these same matched resources,
   the fused Bulk configurations mostly *lose* to M0-RM, so "Bulk wins" is not a statement
   about the Bulk idea — it is a statement about cohorts plus streaming physical-order
   release. Quoted from M24-C §6.2 and not re-measured here, M0-RM over each configuration
   in Orkut / LiveJournal / ogbn / Criteo order:

   | Bulk configuration | Orkut | LiveJournal | ogbn | Criteo | wins vs. M0-RM |
   |---|---|---|---|---|---|
   | 2×8K | 0.662 | 0.457 | 0.285 | 0.460 | 0 of 4 |
   | 2×16K | 1.071 | 0.672 | 0.321 | 0.592 | 1 of 4 |
   | 2×32K | 1.721 | 1.085 | 0.393 | 0.812 | 2 of 4 |
   | G6D S1 | 2.921 | 2.516 | 1.266 | 2.222 | 4 of 4 |

   A value above 1 means the Bulk configuration is faster. Only G6D S1 is faster on every
   workload, and by a margin no fused window approaches.
6. **One arm has a known liveness defect** under one mapping (§8.3). It affects no primary
   cell, but it means the Bulk configuration is not fully robust across address maps while
   the SIMT arm is (all 12 of its mapping cells terminate).
7. **One trace per workload.** The repetitions cover mappings and generator seeds, not graph
   instances: Orkut is one BFS level set, LiveJournal one graph's gather, ogbn-products one
   training epoch's sample, Criteo one embedding table. Cross-instance variance is not
   measured here.

Given 1 and 2 point in opposite directions and neither can be removed without changing an
architecture, the honest reading is that **2.92× / 2.52× / 1.27× / 2.22× is a lower bound on
the organization's benefit if measured net of the index traffic, and an upper bound if
measured gross of it** — and this report does not attempt to price the two against each
other, because doing so would require giving SIMT an index array it does not have.

## 10. Conclusions

1. **The hardware was the confounder.** Matching the shared hardware — the audit covers 44
   resource fields, and only page walkers and dTLB MSHRs had ever differed — collapses
   Bulk's historical advantage from 4.998–8.895× to 1.266–2.921×. On three of four workloads the
   resource benefit alone (2.740–3.947×) exceeds the entire historical Bulk speedup.
2. **No common-resource mismatch remained to correct.** The audit found zero, both for
   M0-RM and for G6D S1, from the files and from the model's echo. M24-C's "six fields" was a
   G6D-vs-2×16K list; against M0-RM the difference is eight fields and all eight are the
   Bulk feature set.
3. **What survives is organizational and workload-dependent, by 2.3×.** 2.921× on Orkut
   (cohort walk reduction 2.90× *and* row hit 29.9 % → 60.5 %, bank busy 98.4 %), 1.266× on
   ogbn-products (output equals the 1.267× walk reduction to 0.03 %, DRAM still 47.9 %
   busy). The ordering is set by how translation-heavy the address stream is and how much
   row locality the physical map can be made to expose.
4. **It is not free and not universal.** Bulk pays 14.5–24.0 % more DRAM reads for its index
   array, and the fused 2×8K/16K/32K Bulk configurations mostly lose to this same M0-RM on
   these same resources (M24-C). The advantage reported here belongs to the G6D organization.
5. **The M0-RM control is now the baseline for any Bulk claim.** No future comparison should
   be run against historical M0, whose 2 walkers and 4 dTLB MSHRs accounted for two-thirds to
   three-quarters of every published speedup.

## Reproduce

```
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests      # 4 924 checks, 0 failures
cd DX100_MEM && PAR=5 ./build/wm/m24d_run.sh all       # 46 cells, skips finished ones
python3 tools/m24d_config_audit.py                     # config_audit.txt, AUDIT PASSED
python3 tools/m24d_extract.py                          # summary.csv, extract.txt, ALL CHECKS PASSED
python3 tools/m24d_verify_doc.py                       # every figure in this file, recomputed
```

Artifacts: `build/wm/m24d/` (46 cell logs, `summary.csv`, `extract.txt`,
`config_audit.txt`), `build/wm/m24d/m24d_simt.cfg`, `build/wm/m24d/m24d_bulk.cfg`,
`build/wm/m24d_run.sh`, `tools/m24d_config_audit.py`, `tools/m24d_extract.py`,
`tools/m24d_verify_doc.py`.
