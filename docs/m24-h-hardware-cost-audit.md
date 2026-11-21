# M24-H — M24 Bulk hardware resource and implementation cost audit

**Scope.** A cost audit of the *existing* production M24 Bulk architecture (M23-G6D S1),
weighed against the resource-matched M0-RM baseline of M24-C/M24-D. No architecture was
changed, no configuration was edited, and no prior artifact was modified: every historical
number is read-only input, and this milestone's own measurements are 18 cells under
`build/wm/m24h/` (a storage ladder plus the M23 panel, §5.3).

**What this audit is not.** There is no synthesis, no SRAM compiler, no floorplan, no
timing closure and no power measurement for this design, and none was attempted. Every
byte below is [C] exact arithmetic on declared fields; every mm² is [E] a labelled density
assumption; every "large" or "expensive" is comparative, never absolute. No frequency and
no absolute power are claimed anywhere.

**Tags.** [M] measured · [C] calculated from source · [E] technology/energy assumption ·
[?] would need RTL.

## 0. Answer

| question | answer |
|---|---|
| **1. What additional hardware does it need?** | **690.5 KiB of engine SRAM** (packed RTL minimum, 16 K window, two buffers, worst case every element on its own line), **0.67× the 1 MiB L2 it feeds**, plus ~16 KiB of engine control and an 8 KiB index-operand buffer. As allocated by the simulator: 1 080 KiB, because 47 % of that is per-run timestamp state that is instrumentation, not hardware. |
| **2. Which structures carry the advantage?** | **The window's order, not the window's machinery.** The dedup/fold and the sealed page order are what convert an uncoalesced gather into run-local traffic (62 % of `criteo_2k`'s and 71 % of `n3_clu4m`'s elements fold onto a shared line). The fused representation delivers that order in 1/3.9 of the unfused storage and, measured here, at **identical cycles**. The streaming stack on top adds 8.5–11.2 % on two M23 traces and **~0 % (one trace −12 %) on the M24 panel**. |
| **3. What is expensive relative to its contribution?** | `fpa`+`fpos`+`ford` (**240 KiB, 34 %** of the engine) buy 0.4–1.6 % on three M24 traces and *cost* 12 % on `criteo_2k`; the `ready` pool (28.7 KiB) is never the binding resource (mean occupancy 28–87 %, peak 392 of 4096 when cohorts are off); the page directory (**11.5 KiB**) is dead, measured null; and `ftr_gen`/`ftr_done` (512 KiB) are simulator timestamps that must never be mistaken for hardware. |
| **4. Main implementation risks** | The **two sorts** (16 384 × 64-bit element sort at seal, 16 384 × 32-bit physical-order sort) are the largest logic blocks in the design and the simulator charges them *zero* cycles; the fanout's 512 b/cycle datapath; the 4096-entry heap's 12-level sift and address decode; `fpa`'s scattered write. Plus the liveness edge M24-G found (`l2_mshr > 64` and `cohorts = 1` deadlock on `criteo_2k`). |
| **5. Is the advantage compelling?** | **On the traces where Bulk wins, yes — but the price is a cache-sized block.** 2.50× / 3.28× on the two order-bound and translation-bound traces for 0.67× an L2; 0.98× / 0.82× on the two it loses, where the same silicon buys nothing. The *marginal* G6D stack (+264 KiB of the 690) is not paid for by the M24 panel at all. Nothing here is hardware-optimal: M24-G measured the 32 K and 64 K windows faster on every trace at 2× and 4× the storage. |

## 1. Audit of prior estimates: what survives, what is stale

| prior work | its claim | status here |
|---|---|---|
| **M22 storage accounting** | entry widths (16 B `WinEnt`, 32 B `LineReq`, 32 B `Cohort`, 56 B `Pending`, 76 B `FanJob`) and build U/F totals (2×16K: 1 851 632 / 475 376 B) | **widths valid [M]** — re-used unchanged. **build F stale**: it predates M23-G6A–G6D, so it has no `ford`, `fpos`, `fpa`, `ftr`, `ready` or `g6c_*`. Production is 707 120 B packed, **+231 744 B (+48.7 %)** over M22's build F. |
| **M22 §1 "the published numbers are unfused"** | the fused representation existed but was not what M18–M21 measured | **superseded**: since M23-G6D the production config *is* `win_fused = 1` (`build/wm/m24d/m24d_bulk.cfg` §7). This audit prices the fused build, and §5.3 shows the fused/unfused cycle counts are **identical** on all three M23 traces (storage-only change). |
| **M22 area model E1** | 0.045 µm²/bit nominal (0.030–0.100 spread), 0.3–0.6 µm²/DFF | **re-used unchanged** as an [E] input. One conversion only; ±3.3× across nodes, so ratios are the trustworthy part. |
| **M22 logic audit** | "no CAM anywhere"; dedup folded into a streaming grouping pass; seal is a W-visit pass, not `W log W` | **partly stale.** No CAM is still true of the *shipped* release path (heap pop + front compare). But the implementation sorts: `std::sort(b.fent)` at seal and `std::stable_sort(b.ford)` in the organizer. §3 re-costs these; they are now the largest logic blocks, and the simulator's `org_backlog` charge still gives them one visit per entry. |
| **M22 power estimate E2** | 1 × 64 B SRAM access = 1 E1; 1 DRAM read incl. activate ≈ 100–200 E1 | **re-used**; the *traffic* claim it was applied to ("the engine never reduces DRAM traffic") does **not** hold on the M24 panel — see §4. |
| **M23-G6A/B/C source audits** | the PA buffer, ready buffer and cohort table, and the 4096-entry capacity | **valid as architecture**, but their *charge* (`g6b_pa_capacity × 16 B` = 65 536 B) is 2× smaller than the array the code allocates (`fpa` is indexed by run, so it is L × 8 B = 131 072 B per buffer in ABI, 65 536 B packed). §2.3 states both. |
| **M24-A / M24-B** | page/DRAM factor (55.7–73.7 %), HBM hierarchy | **not re-used as cost**; M24-B's HBM is not instantiated in this pair (`dram_kind = ddr`), so no HBM cost is claimed. |
| **M24-C / M24-D** | the fair pair: 94 identical keys, 8 differing architecture fields | **valid and re-used unchanged**: the comparison in §5.1 is that pair, unmodified. |
| **M24-E ablation** | cohorts off 1.21–3.86×; pool 1 vs 4096 = 5.53–8.02×; unfused 8.5–11.2 %; index 0–2 %; DRAM order ≈ 0 % | **valid** and used as the benefit column of §5.4. New here: the same ablations re-expressed as a *storage ladder* (§5.3), which is what turns "a mechanism is worth X" into "X per KiB". |
| **M24-G scaling** | cohorts knee 4–8; pool 8–16× oversized; window 32K/64K faster; page directory dead; `l2_mshr > 64` deadlock | **valid**; it is the reason two structures here are priced at zero benefit and one is flagged as a liveness risk. |

Nothing was re-measured that already answered its question. The 18 new cells exist because
no prior milestone measured the G6A→G6D structures one at a time *in the fused
representation*: M24-E's "unfused" arm changes representation **and** pipeline together.

## 2. Storage audit

Full generated tables: `build/wm/m24h/storage.md` (`storage.csv`, `totals.csv`). Counts are
the worst case the structure must hold: `W = L = 16 384` (one run per element, which
`w4_duplow` realizes exactly), `R = 128`, `D = 512` pages, `C = 8` cohorts, ready pool
4 096, two buffers (`org_overlap = 1`, `win_policy` = E3).

### 2.1 Window and engine structures (production, org 8)

| structure | entries | bits/entry | B packed | B as allocated | × | class | shares |
|---|---:|---:|---:|---:|---:|---|---:|
| `fent` element table | 16 384 | 64 | 262 144 | 262 144 | 2 | data | 36.5 % |
| `fmeta` run start\|count | 16 384 | 32 | 131 072 | 131 072 | 2 | meta | 18.2 % |
| `fpa` physical address | 16 384 | 32 | 131 072 | 262 144 | 2 | meta | 18.2 % |
| `ford` physical order | 16 384 | 14 | 57 344 | 131 072 | 2 | control | 8.0 % |
| `fpos` run → rank | 16 384 | 14 | 57 344 | 131 072 | 2 | control | 8.0 % |
| `fslot` residency slot | 16 384 | 7 | 28 672 | 32 768 | 2 | control | 4.0 % |
| `ready` min-heap ranks | 4 096 | 14 | 14 336 | 65 536 | 2 | meta | 2.0 % |
| `fan_` fill/fanout queue | 128 | 608 | 9 728 | 9 728 | 1 | data | 1.4 % |
| `dir_vpn` + `dir_mask` | 512 | 90 | 11 520 | 16 384 | 2 | **dead** | 1.6 % |
| `idx ring` descriptors | 256 | 168 | 5 376 | 5 376 | 1 | meta | 0.7 % |
| `fflag`, `ftr` | 16 384 | 1 each | 8 192 | 65 536 | 2 | control | 1.1 % |
| `coh` + `g6c_coh_def/age` | 8 | 168/16/32 | 432 | 704 | 2 | control | 0.1 % |
| `info_` request records | 128 | 44 | 704 | 7 168 | 1 | control | 0.1 % |
| slot lists, `queue_` | 128 / 8 | 24 / 320 | 704 | 1 856 | 1 | control | 0.1 % |
| **total, hardware** | | | **707 120** | **1 106 176** | | | 100 % |
| `ftr_gen` + `ftr_done` timestamps | 16 384 | — | 0 | 524 288 | 2 | **instr** | — |
| `win_rec` timeline | 3 | — | 0 | 432 | 2 | instr | — |
| index operand buffers | 128 lines | 512 | 8 192 | 0 | 1 | **req** | — |

**Three accountings, and the one to use.**

| accounting | bytes | vs 1 MiB L2 | mm² @ 0.045 µm²/bit [E] | use it for |
|---|---:|---:|---:|---|
| simulator `charge` (`bulk_win_bytes` + idx + pool) | 347 296 | 0.33× | 0.125 | continuity with M24 reports only |
| **packed RTL, dead state excluded** | **707 120** | **0.67×** | **0.255** | the hardware cost estimate |
| packed RTL, dead state included | 718 640 | 0.69× | 0.259 | as above, + the directory |
| as allocated by the simulator, hardware only | 1 106 176 | 1.05× | 0.398 | upper bound (C++ widths) |
| as allocated, instrumentation included | 1 122 560 | 1.07× | 0.404 | **not** a hardware number |

**Reconciliation.** The simulator's `bulk_win_vec_bytes` covers the fused subset only
(`bulk.cpp:3062–3072` sums `fent/fmeta/fslot/fflag/dir`). At `L = W` that subset is
**475 136 B**, which is exactly what the log reports on `w4_duplow` (475 136) and `orkut`
(475 136) [M]. The production engine also allocates **622 592 B** in `ford`/`fpos`/`fpa`/
`ftr`/`ready` that no published storage figure has ever counted, plus 24 128 B of engine
state and 704 B of cohort table. 475 136 + 622 592 + 704 + 24 128 = **1 122 560 B** = the
total as allocated, to the byte. That is the audit's main accounting finding: **every
M24 storage number so far is the first row of the table, i.e. 42 % of what the engine
actually allocates.**

### 2.2 Replication, sharing, and what is double counted

* Window arrays are **×2**: two buffers, because `org_overlap = 1` and the E3 policy keeps
  filling while the other drains. Only one window *issues* at a time (`iss_`), so the ×2 is
  a throughput choice for the sealer, not a requirement: a single live window would halve
  `fent`/`fmeta`/`fpa`/`ford`/`fpos`/`fslot`/`fflag`/`ftr` to 337 920 B packed (the report
  does **not** take that credit).
* The ready pool is per `Batch` in the source but only the issuing buffer uses it, so the
  hardware need is **one** 4 096-entry pool (7 168 B packed), not two.
* `fslot` and `info_` are **not** duplicates: `fslot` is the window's view of a run's slot
  (used to batch the fill fanout), `info_` is the memory system's per-slot record. They
  hold different fields of the same transaction.
* Caches, MSHRs, TLBs, walkers, the DRAM model and `R` are **shared** and identical in both
  arms (M24-D); they are not part of this table and are not double counted.
* `val_`/`addr_` (`std::unordered_map`, one node per completed element) are simulator-only
  and excluded: a hardware fanout writes the element into the consumer and keeps nothing.

### 2.3 The two capacities where charge and implementation disagree

* **Ready pool.** The charge is `g6b_pa_capacity × 16 B` = 65 536 B [M]. The structure that
  implements it is `fpa`, indexed **by run id** (`b.fpa[li]`), so it is `L × 32 b` =
  131 072 B packed per buffer, and the same for `fpos`'s key. A re-indexed circular pool of
  `capacity` entries × (26 b PA + 14 b run id) = 20 480 B would serve the same purpose, but
  that is a redesign and is **not** taken; §5.4 prices what the current form buys.
* **Element table.** 64 b per element is right for the fused word (line 26 | position 32 |
  offset 6), and it is 36.5 % of the engine on its own. It is the only structure whose size
  is set by the *window* rather than by `R` or the page size.

### 2.4 Is each structure active in the representative configuration?

| structure | active | evidence [M] |
|---|---|---|
| `fent`, `fmeta`, `fslot`, `fflag`, `coh` | yes | `bulk_win_entries = 131 072`, `bulk_sched_opens = 57 926` (`w4`) |
| `ford`, `fpa`, `ftr`, `fpos`, `ready` | yes | `bulk_org_policy = 8`, `bulk_org_runs = 131 072`, `bulk_g6b_pa_capacity = 4096` |
| `g6c_coh_def`, `g6c_coh_age` | yes | `bulk_g6c_admit_attempts = 3 439 896` (`w4`), 1 344 926 (`criteo_2k`) |
| `dir_vpn`, `dir_mask` | written, never read | `bulk_dir_occ_max = 6 391` at `D = 512`; identical cycles at 128/256/1024 (M24-G §5) |
| `ftr_gen`, `ftr_done`, `win_rec` | yes, instrumentation | latency accounting only; no decision reads them |
| `reuse_` (M13) | **no** | `bulk_reuse_on = 0`; the production config has `reuse_dir = 0` |

Measured peaks against each capacity: `bulk_win_occ_max = 16 384` (window full on all four
traces), `bulk_g6b_ready_peak = 4 096` (pool full on `criteo_2k`, 3 574 mean on
`n1_seq8m`), `bulk_fanout_max_backlog = 5 772` on `n3_clu4m` against a queue bounded by
`R = 128`, `bulk_idx_out_max = 64`, `bulk_unissued_max = 64`. The fanout backlog exceeding
`R` is a simulator artifact (the queue is drained by lanes, not bounded by slots); the
hardware bound is `R` and is what §2.1 costs.

## 3. Logic and timing complexity

No gate counts, no FO4, no frequency: none is derivable from a C++ model. What *is*
derivable is the operation count per event and the structure of each decision, and this
section reports exactly that, with the measured event counts from the four production
cells.

### 3.1 Inventory (source-level operation counts) [C] from `sim/src/bulk.cpp`

| # | operation | sites | per event | events on `w4` / `criteo` [M] | class |
|---|---|---|---|---|---|
| 1 | window admission | `win_admit_one` `:1067`, `win_admit` `:1093` | 1 element/visit, `org_lanes = 4`/cycle | 131 072 / 34 816 | sequential, 1 admission port |
| 2 | **seal sort** | `win_seal_fused` `:1507` `std::sort(b.fent)` | sort 16 384 × 64 b keys | 8 / 3 windows | **largest sequential block** |
| 3 | run detection (dedup) | `:1508–1515` | 1 compare + 1 `fmeta` write per distinct line | 131 072 / 13 097 runs | post-sort run detection |
| 4 | **physical-order sort** | `win_order_fused` `:1590` `std::stable_sort(b.ford)` | sort 16 384 × 32 b indices by (row 12 b, bank 1 b, line) | 8 / 3 windows | **largest scatter** |
| 5 | cohort select / admission | `win_issue_sched_fused` `:2013`, `win_admit_g6c` `:2529` | scan ≤ 8 cohorts (open ∧ pos<end ∧ resume≤now), 1 install cycle | `sched_opens` 57 926 / 6 569; `admit_attempts` 3 439 896 / 1 344 926 | 8-way priority, 1/cycle |
| 6 | page-run open | `g6c_open_cohort` `:2624` | pointer walk until the VPN changes; 1 cycle install | 106 292 cohorts (M24-E) | sequential, pageable |
| 7 | **ready heap** | `ready_push/pop` `sim.hpp:1740` | push: sift up ≤ 12 levels; pop: sift down ≤ 12 | 131 072 push + 131 072 pop / 13 097 + 13 097 | 1 comparator/level |
| 8 | slot arbitration | `alloc_slot` `:397`, `tgt_reserve` `:1314` | free-list head + reserved-share compare | 131 072 / 13 097 | trivial |
| 9 | address generation | `:1290` `base + idx × b` | `ag_lanes = 8` × 64-bit MAC/cycle | 131 072 / 34 816 | 8 MACs |
| 10 | fanout | `win_fanout_fused` `:2889`, `on_line` | 64 B copy + ≤ 64 element writes, `fanout_lanes = 4` | 131 072 lines / 131 072 elems | 512 b/cycle datapath |
| 11 | response match | `info_[slot]` | direct index, `seq` for OOO | — | **no CAM** |
| 12 | page directory | `:1272` `dir_mask \|= 1 << (vl & 63)` | 64-bit OR + 1 tag compare | — | trivial, dead |
| 13 | descriptor fetch | `BulkDesc::decode` `:276` | 1 line, 1 decode | 8 / 3 descs | trivial |

There is still **no content-addressable memory and no tag-compare-everything** in the
shipped path, and that part of M22's reading survives. Two things M22 did not see are now
the dominant blocks:

* **Two sorts.** The element sort is over 16 384 keys of 58 significant bits; the ordering
  sort is over 16 384 indices with a 13-bit (row, bank) key. Neither is a comparator
  network in any sane build: both are counting/radix scatters (58 bits → 2 × 29-bit passes,
  or a 13-bit bucket pass with 8 192 counters). That costs **8 192 × 14 b ≈ 14 KiB of
  bucket counters plus one 16 384-entry scatter port per sort** — logic and wiring this
  audit cannot size, and the simulator charges it **zero cycles** (`st_.org_backlog` is a
  visit count, not a pipeline). The seal overlap measurement (M22 §2: 292 671 cycles of
  shadow) is the only evidence that this is affordable, and it is evidence about *time*,
  not about area.
* **A 4 096-entry heap.** `ready` is a `std::priority_queue`-equivalent min-heap over
  14-bit keys: an insert and a delete-min per released line, each a 12-level sift
  (12 SRAM reads + 12 compares in the same cycle unless pipelined). Measured: 131 072
  push/pop pairs on `w4`, 13 097 on `criteo_2k`. This is the one structure where a naive
  build is a real timing risk — the alternative (`sched_vis`, `sched_group`) scans the
  whole pool and is correctly labelled diagnostic.

### 3.2 Critical paths, ordered by what would set the clock [?]

| path | chain | pipelinable | risk |
|---|---|---|---|
| **fanout** | 64 B line read → 64 B write mux → dependant walk, 4 elements/cycle | yes, but 512 b/cycle | **highest** wire/port risk |
| **seal scatter** | sort key → bucket → random write into 16 384 × 64 b | no (it is the scatter) | **high**: 14 KiB of counters + a 16 K random-write port |
| **order scatter** | PA → (row, bank) → bucket → random write into 16 384 × 14 b | no | high, same shape |
| heap sift | 12 dependent SRAM reads + compares | pipelined across releases | medium |
| `fpa` write | translation completion → scattered write by run id | yes | medium (banking) |
| address generation | 8 × 64-bit MAC | 2–3 stages | low–medium |
| cohort select | 8-way compare + 1 install cycle | 1 cycle | low |

**High-fanout control signals:** the heap's 4 096-way address decode, the 8 192-way bucket
counters of each sort, and the 8-way cohort select. None of them is deep; all of them are
wide.

### 3.3 What the engine is actually waiting for [M]

The release port is blocked by the shared residency budget on **98.1 %** of `w4`'s cycles
(`bulk_g6b_release_slot_stall = 1 483 241` of 1 511 665), 98.2 % of `n1_seq8m`'s, 59.8 % of
`n3_clu4m`'s and 52.9 % of `criteo_2k`'s. The engine is not logic-bound and not
translation-bound on this panel: it is **`R`-bound**, with a pool holding 1 132–3 574
translated runs it cannot issue. That is the quantitative reason §5 finds the deep pool
cheap to remove.

## 4. Power and bandwidth implications

No power model is validated for this design, so **no absolute power, energy or leakage
number is claimed**. What is measured is traffic and access count, and what is stated is a
direction with an explicit break-even.

### 4.1 Measured traffic, Bulk against the resource-matched SIMT [M]

| trace | DRAM reads (Δ) | DRAM bytes (Δ) | row-hit rate | row activations (Δ) | L1 reads (Δ) | L2 reads (Δ) | walks (Δ) |
|---|---:|---:|---:|---:|---:|---:|---:|
| `w4_duplow` | 162 269 (+3.8 %) | 10.4 MB (+3.8 %) | **62.5 %** (0.2 %) | 60 875 (**−61.0 %**) | 213 505 (+4.0 %) | 182 404 (−48.5 %) | 16 895 (−82.9 %) |
| `criteo_2k` | 14 983 (**−22.9 %**) | 0.96 MB (−22.9 %) | **63.7 %** (6.3 %) | 5 442 (**−70.1 %**) | 28 852 (−35.7 %) | 27 687 (−60.7 %) | 5 114 (−75.3 %) |
| `n1_seq8m` | 164 398 (+5.3 %) | 10.5 MB (+5.3 %) | 92.9 % (89.5 %) | 11 701 (−28.5 %) | 213 505 (+4.0 %) | 167 531 (+5.6 %) | 2 570 (+5.2 %) |
| `n3_clu4m` | 47 620 (+18.6 %) | 3.05 MB (+18.6 %) | 72.3 % (53.5 %) | 13 170 (−29.4 %) | 120 599 (−41.2 %) | 83 878 (−48.6 %) | 6 988 (−4.7 %) |

Bracketed values on row-hit are the SIMT arm's rate. This **corrects M22 §3.1**, which
found the engine always increasing DRAM reads by 0.3–11 %: on this pair it *reduces* them on
`criteo_2k` (−22.9 %) because the coalesced SIMT path touches a 16 KiB table twice per
element and the window folds the duplicates, while it still adds 3.8–18.6 % where the window
fetches whole lines the SIMT path would have hit in L1/L2.

**Index-array traffic** is the engine-only addition: 8 192 index lines (524 288 B) on `w4`
and `n1`, 2 176 lines (139 264 B) on `criteo_2k` — 6.3 %, 16.6 %, 6.3 % and 21.5 % of
`w4`/`criteo_2k`/`n1`/`n3` target-line traffic, plus 64 B per descriptor (8, 3, 8 and 8
descriptors). It is proportionally largest exactly where the fold is strongest: `criteo_2k`
fetches 0.38 target lines per element but still reads one full index line per 16 elements. The target traffic itself is
**more** than the elements' worth on `w4` (131 072 lines for 131 072 elements = 1 line per
element) and **less** on `criteo_2k` (13 097 lines for 34 816 elements = 0.38 per element).

### 4.2 On-chip access accounting [C]

Per released run the engine touches `fmeta`, `fslot`, `fflag`, `fpa`, `fpos` and `ford`
(6 accesses) plus a heap push and pop (2 more, each up to 12 SRAM reads). Per element it
writes `fent` once and reads it once at seal.

| trace | runs | per-run accesses | per-element | total on-chip accesses |
|---|---:|---:|---:|---:|
| `w4_duplow` | 131 072 | 8 | 2 | ≈ 1.31 M |
| `criteo_2k` | 13 097 | 8 | 2 | ≈ 0.17 M |
| `n1_seq8m` | 131 072 | 8 | 2 | ≈ 1.31 M |
| `n3_clu4m` | 38 166 | 8 | 2 | ≈ 0.57 M |

At M22's E2 (one 64 B SRAM access = 1 E1) that is 0.17–1.31 M E1 of added on-chip work, of
which the heap sift is the majority (≤ 24 accesses per run).

### 4.3 Direction, with its break-even [D]

Against it stand the removed row activations: 95 125 on `w4`, 12 768 on `criteo_2k`, 4 662
on `n1_seq8m`, 5 493 on `n3_clu4m`. In E1 units the two sides are

```
added on-chip      = 0.17–1.31 M E1                  (above)
removed activations = A × c_act,  c_act ∈ [0, 200] E1  (E2 bounds a DRAM read at 100–200 E1)
```

so each trace has its own break-even `c_act = added / A`:

| trace | added E1 | activations removed | break-even `c_act` | verdict at E2's 100–200 E1 |
|---|---:|---:|---:|---|
| `w4_duplow` | 1.31 M | 95 125 | **13.8** | favourable |
| `criteo_2k` | 0.17 M | 12 768 | **13.7** | favourable |
| `n3_clu4m` | 0.57 M | 5 493 | **103** | borderline (inside E2's range) |
| `n1_seq8m` | 1.31 M | 4 662 | **281** | **unfavourable**: above E2's upper bound |

The direction is therefore **not uniform**, and it tracks the performance result: the two
traces the engine wins on are the two where removing activations clearly pays for the added
metadata traffic, `n3_clu4m` is a wash, and on `n1_seq8m` — the sequential trace it *loses*
— the engine adds metadata accesses to save almost no activations. That is a directional
statement on an assumed number, not an energy measurement; switching activity rises with
the access counts above and with 4-lane fanout, and leakage scales with §2's area and is
not estimated separately.

## 5. Cost/performance

### 5.1 The fair comparison (M24-D pair, unchanged)

| trace | Bulk cycles | M0-RM cycles | ratio | what makes the difference |
|---|---:|---:|---:|---|
| `w4_duplow` | 1 511 665 | 3 785 634 | **2.504** | row locality (62.5 % vs 0.2 %) at +3.8 % reads |
| `criteo_2k` | 242 935 | 796 800 | **3.280** | fold (62 % of elements) and −22.9 % reads |
| `n1_seq8m` | 1 222 342 | 1 191 908 | 0.975 | sequential: nothing to reorder |
| `n3_clu4m` | 484 783 | 399 150 | 0.823 | clustered reuse: the SIMT path's L2 hits are cheaper |

### 5.2 Additional hardware, stated against that ratio

| item | quantity | confidence |
|---|---|---|
| engine SRAM (packed RTL) | 707 120 B = 690.5 KiB = 0.67× L2 | [C] bytes, [E] area |
| of which Bulk-specific vs a bare cache hierarchy | **all of it** — M0-RM has no engine | [C] |
| metadata share (`fmeta`+`fpa`+`fpos`+`ford`+`ready`+`dir`) | 402 688 B = 57 % | [C] |
| engine control (`info_`, slot lists, idx ring, queues) | 16 512 B = 2.3 % | [C] |
| dead state | 11 520 B (directory) | [C] |
| logic | 2 sorts (16 K elements each), 4 096-entry heap, 8-way cohort select, 8-lane AG, 4-lane fanout | [?] no gate count |
| timing risk | fanout 512 b/cycle; two scatter ports; heap sift | [?] |
| memory traffic | −22.9 % to +18.6 % DRAM reads; +0.4–4.0 % index lines | [M] |
| unquantified | wire area of fanout/scatter, clock impact, leakage, verification cost of the liveness edge | — |

### 5.3 The storage ladder: what each G6A–G6D step is worth

Four configurations of the same binary on the same traces. `org 0` is the fused window with
the P0 cohort scheduler and **no** PA buffer, order permutation or ready pool; each later
step adds exactly one structure set (deltas from `build/wm/m24h/storage.md`) [M] cycles,
[C] bytes.

| org | structures added | packed B | Δ B | `w4` | `criteo_2k` | `n1_seq8m` | `n3_clu4m` |
|---|---|---:|---:|---:|---:|---:|---:|
| 0 | fused window + cohort scheduler | 448 640 | — | 1 517 177 | **216 804** | 1 242 398 | 489 674 |
| 6 | + physical order + PA buffer (G6A) | 646 864 | +198 224 | 6 232 136 | 829 897 | 1 747 915 | 936 180 |
| 7 | + streaming ready pool (G6B) | 718 544 | +71 680 | 4 957 094 | 743 616 | 1 222 169 | 802 812 |
| 8 | + cohort admission (G6C/G6D) = **production** | 718 640 | +96 | **1 511 665** | 242 935 | **1 222 342** | **484 783** |

| step | `w4` | `criteo_2k` | `n1_seq8m` | `n3_clu4m` | reading |
|---|---:|---:|---:|---:|---|
| 0 → 6 (order + PA buffer, barrier) | ×4.11 | ×3.83 | ×1.41 | ×1.91 | translating a whole window before releasing any of it serialises translation and DRAM |
| 6 → 7 (streaming release) | ×0.795 | ×0.896 | ×0.699 | ×0.858 | recovers 10–30 % of that, still worse than org 0 |
| 7 → 8 (cohort admission) | ×0.305 | ×0.327 | ×1.000 | ×0.604 | the cohort pipeline is what makes streaming pay |
| **net, 0 → 8** | **×0.996** | **×1.121** | **×0.984** | **×0.990** | **the whole G6D stack costs 270 000 B (264 KiB) and buys ≈ 0 %** |

On the three traces the stack was *designed* on (M23 panel, re-run here; the production
cells reproduce M24-E's `base` to the cycle, and `org 0` reproduces its `unfused` arm to the
cycle — a determinism cross-check, §7):

| trace | org 0 | production | production/org 0 | verdict on the 264 KiB |
|---|---:|---:|---:|---|
| `orkut` | 6 653 100 | 6 130 494 | **0.921** | +8.5 % |
| `livejournal` | 8 404 988 | 7 558 966 | **0.899** | +11.2 % |
| `ogbn` | 35 248 112 | 35 248 957 | 1.000 | 0.0 % |

Two facts fall out, and they are the audit's core cost result:

1. **The fused representation is a storage optimisation only.** `org 0` (fused) and M24-E's
   `unfused` arm produce *identical* cycle counts on all three M23 traces — 6 653 100,
   8 404 988, 35 248 112 — while their storage differs by 3.9× (475 136 vs 1 851 392 B as
   allocated). The 3.9× is real; it buys no cycles, and it must not be presented as a
   performance win.
2. **The streaming stack is workload-conditional.** It pays 8.5–11.2 % on two long-running
   graph traces, ties on a third and on three of the M24 panel's four, and *loses 12 %* on
   `criteo_2k`, where the pool fills (peak 4 096) and its backpressure costs more than the
   translation overlap gains.

### 5.4 Which structures are expensive relative to their contribution

| structure set | packed B | share | measured contribution | verdict |
|---|---:|---:|---|---|
| `fent` element table | 262 144 | 37 % | the window itself: without it there is no engine | **indispensable**; cheaper is impossible in this format |
| dedup + sealed page order | 0 (in `fent`/`fmeta`) | — | 62 % / 71 % of elements folded on `criteo`/`n3`; 62.5 % vs 0.2 % row hits on `w4` | **the win** |
| `fmeta` run table | 131 072 | 18 % | one record per run; required by every path | **necessary** |
| cohort table + `g6c` counters | 432 | 0.1 % | removing it costs **1.21–3.86×** (M24-E) | **best value in the design** |
| `fpa` + `fpos` + `ford` | 245 760 | 34 % | `org 0` (none of them) is within 0.4–1.6 % on three M24 traces and **12 % faster** on `criteo_2k`; +8.5/+11.2 % on orkut/livejournal | **conditional**: paid for only by long graph traces |
| `ready` pool | 28 672 | 2 % | pool depth 1 → 4096 is worth 5.53–8.02× (M24-E), but 16 entries already captures 90–100 % of it; mean occupancy is 1 132 (`n3`), 1 451 (`criteo_2k`), 3 253 (`w4`), 3 574 (`n1`) of 4 096 | **oversized** (M24-G agrees); the first 16 entries are essential, the last 4 080 are not |
| index ring + tiling | 5 376 + 8 192 | 1.9 % | idx_mode 0 vs 2: 0–2.0 % (M24-E) | **fair value** |
| `dir_vpn`/`dir_mask` | 11 520 | 1.6 % | **zero**, measured null at 128/256/1024 | **delete** |
| `ftr_gen`/`ftr_done` | 524 288 as allocated | (47 % of ABI) | none — simulator timestamps | **not hardware** |
| engine control | 16 512 | 2.3 % | `R` slots, index ring, queues: the floor cost of any engine | **unavoidable** |

### 5.5 What this audit does not price

* **Wire and scatter area.** The two sorts, the fanout and the heap's decode are wide
  structures whose area is mostly routing; byte-counted SRAM understates them, so a 3.9×
  *storage* reduction is **not** a 3.9× *silicon-area* reduction.
* **Frequency.** The fanout's 512 b/cycle and the heap's 12-level sift are the two paths
  that could cost cycles; without RTL nothing is claimed.
* **Leakage**, which scales with area, and **verification**, which the liveness edge
  (`criteo_2k` at `l2_mshr > 64`, at one cohort) makes non-trivial.
* **The 32 K/64 K window.** M24-G measured 32 K 11–23 % and 64 K 19–34 % faster on the same
  panel for 2×/4× this storage. On the performance axis the production point is therefore
  **not** on the frontier; on cost-per-cycle it is.

## 6. Conclusions

1. **The additional hardware is a cache-sized SRAM block plus wide sorting logic.** 690.5 KiB
   packed (0.67× the L2), 37 % of it one element table, 34 % per-run order/PA state, 1.6 %
   dead, and a further 512 KiB of simulator timestamps that no hardware needs (47 % of the
   as-allocated total).
2. **The performance advantage comes from order, not from machinery.** Dedup and the sealed
   page order turn an uncoalesced gather into 0.38–1.0 line requests per element with 62–93 %
   row hits. The fused representation delivers that order at 1/3.9 of the storage and no
   change in cycles.
3. **Three things are expensive relative to their contribution**: the order/PA arrays
   (240 KiB, ~0 % on four of seven traces and −12 % on one), the pool depth beyond ~16
   entries (28 KiB for ≤ 11 %), and the page directory (11.5 KiB for nothing).
4. **The main risks are the two scatter sorts and the fanout datapath**, neither of which the
   simulator charges, plus a real liveness edge on one workload.
5. **The advantage is compelling where Bulk wins and not where it does not.** 2.50× and
   3.28× for 0.67× an L2 on the two traces that reward reordering is a defensible trade for
   an accelerator whose job is exactly those traces; 0.98× and 0.82× on the other two is
   not, and no part of the 690 KiB helps there. The marginal 264 KiB of the G6D stack is
   justified by two of seven traces, and the honest summary is that the *fused window with
   a cohort scheduler* is the cost-effective core, while the streaming pipeline is a
   workload-conditional extension that the M24 panel does not pay for.

## 7. Confidence

| claim | tag | basis |
|---|---|---|
| per-entry bits and bytes | **[C]** | declared fields in `sim.hpp` / `bulk.cpp`; ranges recomputed |
| totals 707 120 / 1 106 176 / 347 296 B | **[C]** | `tools/m24h_structures.py`, reconciled to the byte against `bulk_win_vec_bytes` |
| 0.045 µm²/bit and the mm² columns | **[E]** | M22 E1, unchanged; ±3.3× across nodes |
| cycles, ratios, ladder, traffic | **[M]** | 18 cells, deterministic; production reproduces M24-E base and `org 0` reproduces its unfused arm to the cycle |
| ablation contributions (§5.4) | **[M]** | M24-E, quoted; not re-run |
| operation counts (§3) | **[C]** | source lines + [M] event counts |
| sort/heap area, frequency, power | **[?]** | not estimable without RTL; not claimed |
| energy direction (§4.3) | **[D]** | arithmetic on [M] activations under [E] E2 |

## 8. Limitations

1. **No synthesis of any kind.** The audit's arithmetic stops at bytes; one density
   assumption converts them to mm². A 3.9× storage reduction is not a 3.9× area reduction.
2. **Worst-case sizing.** Every per-run array is costed at `L = W` (one run per element,
   which `w4_duplow` realizes). On `criteo_2k` only 38 % of elements are distinct, so the
   realized storage is smaller; the sizing point is the honest RTL budget.
3. **Instrumentation is separated, not priced.** 512 KiB of timestamps are reported and
   excluded; a reader who takes the as-allocated total at face value overstates the engine
   by 47 %.
4. **One seed, M0 map, DDR only.** The panel and pair are M24-D's; M24-B's HBM hierarchy is
   not instantiated and no HBM cost is claimed.
5. **The ladder removes structures by configuration, not by editing source.** `org 0` is a
   supported configuration of the same binary; the comparison is therefore exact in the
   memory system and only approximate as a *hardware* alternative (the source still
   allocates nothing for the absent arrays, which is exactly right).
6. **The M23-panel cells are three.** They are the traces G6D was designed on; a fourth
   (`criteo` at 16 K) exists in M24-E but at a different footprint than the M24 panel's
   `criteo_2k`, so it is quoted from M24-E rather than mixed into the ladder.
7. **No power model is validated.** §4 gives a direction and its break-even, nothing more.

## Reproduce

```
cd DX100_MEM
python3 tools/m24h_plan.py                     # cells.tsv + cmds.txt (18 cells)
PAR=4 ./build/wm/m24h_run.sh all               # ~25 min, skips finished cells
python3 tools/m24h_structures.py               # storage.md, storage.csv, totals.csv
python3 tools/m24h_extract.py                  # cells.csv, ladder.csv, traffic.csv, checks.txt, report.txt
(cd sim && ./bin/dx100_tests)                  # 4 924 checks, 0 failures
```

Artifacts: `build/wm/m24h/{cells.tsv,cmds.txt,log/×18,storage.md,storage.csv,totals.csv,
cells.csv,ladder.csv,traffic.csv,checks.txt,report.txt,run.log}`. Scripts:
`build/wm/m24h_run.sh`, `tools/m24h_{plan,structures,extract}.py`. Production pair and
historical artifacts unmodified.
