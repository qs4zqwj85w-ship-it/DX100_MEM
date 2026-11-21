# M28-C — Storage optimization study: how little on-chip storage does M27 Bulk need?

Investigation and design study. No source, test or configuration file was modified, no simulation was
run, no synthesis result is claimed, nothing was removed and no dispatch, DRAM-scheduling, conservation
or liveness rule was touched. Every number is labelled:

| tag | meaning |
|---|---|
| **[C]** | read directly from the source at the cited line |
| **[D]** | derived arithmetic: a declared entry count × a declared field list |
| **[M]** | measured, quoted from a preserved log or a published report |
| **[E]** | estimated (an [D] figure that assumes maximum occupancy, or a complexity count) |
| **[?]** | unknown, and explicitly unknown |

---

## 1. Executive summary

**1. The inventory is complete and it is larger than the last audit said: 988 832 B (965.7 KiB).**
M28-B's 806 048 B (787 KiB) omitted `fmeta`, `fslot`, `fflag`, `ftr`, the cohort counters and the
engine's small control state (248 320 B) and counted the ready pool twice (65 536 B). The
reconciliation is exact and is shown in §2.3. Of the 988 832 B, the *window* is 475 840 B (48.1 %),
the *release path* (per-run PA, per-run rank, the ready pool) is 458 752 B (46.4 %), and control
state is 54 240 B (5.5 %).

**2. Yes — the page-organization mechanism keeps working at a fraction of the storage, and the first
third costs nothing at all.** Three candidates in §5:

| | bytes | KiB | saved | what it changes |
|---|---:|---:|---:|---|
| current | 988 832 | 965.7 | — | — |
| **A conservative** | **677 536** | **661.7** | **311 296 (31.5 %)** | no behaviour change: one live copy of the release-path arrays, `fpos` as identity |
| **B balanced** | **507 488** | **495.6** | **481 344 (48.7 %)** | field widths narrowed to the bits their ranges need |
| **C aggressive** | **237 760** | **232.2** | **751 072 (76.0 %)** | single window, 512-entry pool that *is* the PA buffer |

**3. Two structural facts make this possible, and neither has been stated before.**

* **`fpa`, `fpos`, `ftr` and `ready` are live in at most one window at a time.** They are members of
  `Batch`, so the source allocates two copies, but they are written and read only inside
  `win_issue_g6b`, which runs for `iss_` — and `iss_` is a single cursor that is cleared only when the
  window retires (`bulk.cpp:2976-2983`). The second copy is never touched: **245 760 B is dead** [C].
* **With `m24_dram_org = 0` — the production setting — `fpos` is provably the identity.** It is built as
  `fpos[ford.empty() ? i : ford[i]] = i` (`bulk.cpp:2324`), and `ford` is empty when the organizer is
  off (`bulk.cpp:1585`), so `fpos[i] == i`. `run_at()` already returns `pos` in that case
  (`sim.hpp:1879-1881`). The 131 072 B array computes a value that is already known: **131 072 B is
  dead** [C].

**4. The per-run arrays are over-provisioned in *depth*, not just width.** A run's physical address
exists only from its translation to its release, and that population is bounded by the pool capacity —
not by the window size. `fpa`/`fpos` are indexed by run id and therefore `W`-deep (16 384) when the
live population is `cap` (4 096, or 512 if the pool is downsized). That is the difference between
`fpa + fpos = 393 216 B` and a pool-indexed form of `512 × 25 b = 1 600 B` [D]. This is Candidate C's
main move and it is the largest single saving in the study.

**5. What must not be shrunk on the current evidence: the window, the cohort table, and the first
sixteen pool entries.** The window is the only capacity that still has demonstrated payoff (M24-G:
32 K is 11–23 % faster, 64 K 19–34 %; 8 K is 12–37 % *slower*); the cohort table is 512 B and worth
1.21–3.86× (M24-E); and a one-entry ready pool costs 5.53–8.02× (M24-E). Everything else in the
engine has either a measured-null contribution (the page directory) or no demonstrated capacity
sensitivity at all.

**6. Best next experiment — the 311 KiB that should be free.** Candidate A is behaviour-preserving by
construction, so its test is falsifiable in the strongest possible way: the cycle counts and every
counter of the Orkut and Criteo 16K cells must be *identical* to the M27 anchors while the reported
storage drops from 988 832 B to 677 536 B. §7 gives the exact parameters and checks.

---

## 2. Complete storage inventory

### 2.1 Method

1. Every structure is taken from a declaration in `sim/src/sim.hpp` (`Batch` at 1550-1810, `IdxRing`,
   `FanJob`, `Cohort`, `Pending`) or from an allocation site in `sim/src/bulk.cpp`
   (`.assign`/`.resize`), so each row names the code that sizes it.
2. Entry counts are the *worst case the structure must hold*, at the production geometry:
   `batch_window = 16 384` elements and therefore at most 16 384 runs (`fmeta` is one entry per
   distinct line; `bulk_win_occ_max = 16 384` on Orkut [M]), `page_dir = 512`,
   `page_cohorts = 8`, `g6b_pa_capacity = 4 096` runs, `R = 128` slots, `org_overlap = 1` and
   `win_policy` = E3 so the window arrays exist twice (`bulk.cpp:4355-4360`).
3. Widths are the **declared C++ types** (`u64` = 8 B, `u32` = 4 B, `u8` = 1 B), i.e. what the
   simulator actually allocates. This is deliberately *not* an RTL figure; §3 gives the packed width
   each field really needs, and §5's Candidate B/C use those.
4. Nothing shared is counted: L1, L2, both TLBs, both MSHR files, walkers, the 128-slot `SlotPool`,
   `MemorySystem::inflight_` and the DRAM arrays are the shared hierarchy of M28-B §3 and are absent
   here. The engine-specific small structures (`fan_`, `info_`, tile SRAM, slot lists, descriptor
   queue) *are* counted, because they are the floor cost of having an engine at all.

### 2.2 The inventory

| # | structure | entries / buffer | width | copies | bytes | class | charged by the model? |
|---|---|---:|---:|---:|---:|---|---|
| 1 | `fent` fused element word | 16 384 | 8 B | 2 | 262 144 | window, data | yes (the `batch_window × 8` term) |
| 2 | `fmeta` run start\|count | 16 384 | 4 B | 2 | 131 072 | window, meta | **no** |
| 3 | `fslot` residency slot per run | 16 384 | 1 B | 2 | 32 768 | window, control | **no** |
| 4 | `fflag` issued bit per run | 16 384 | 1 B | 2 | 32 768 | window, control | **no** |
| 5 | `dir_vpn` page-directory tags | 512 | 8 B | 2 | 8 192 | window, **dead** | yes (`page_dir × 16` covers both) |
| 6 | `dir_mask` per-page line mask | 512 | 8 B | 2 | 8 192 | window, **dead** | yes (same term) |
| 7 | `coh` cohort descriptors | 8 | 32 B | 2 | 512 | window, control | yes (`sched_bytes`) |
| 8 | `g6c_coh_def` refusals per cohort | 8 | 4 B | 2 | 64 | window, control | **no** |
| 9 | `g6c_coh_age` episode start per cohort | 8 | 8 B | 2 | 128 | window, control | **no** |
| 10 | `fpa` physical line per run | 16 384 | 8 B | 2 | 262 144 | release, meta | **no** |
| 11 | `fpos` rank per run | 16 384 | 4 B | 2 | 131 072 | release, control | **no** |
| 12 | `ftr` translated bit per run | 16 384 | 1 B | 2 | 32 768 | release, control | **no** |
| 13 | `ready` translated-run min-heap | 4 096 | 8 B | 2 | 65 536 | release, meta | yes (at 16 B/entry, one pool) |
| 14 | `ford` release order | 16 384 | 4 B | 2 | **0** (organizer off) | release, control | **no** (and 131 072 when on) |
| 15 | `fan_` fill/fanout queue | 128 | 76 B | 1 | 9 728 | engine control | no |
| 16 | `info_` per-slot request record | 128 | 56 B | 1 | 7 168 | engine control | no |
| 17 | index tiles (`idx_buf_lines = 64`) | 2 × 64 | 21 B + 32 B | 1 | 2 720 | engine control | yes (`bulk_idx_bytes`) |
| 18 | `free_`/`unissued_`/`idx_q_` ids | 128 | 12 B | 1 | 1 536 | engine control | no |
| 19 | `queue_` descriptor FIFO | 8 | 40 B | 1 | 320 | engine control | no |
| | **total** | | | | **988 832 B = 965.7 KiB** | | |

Subtotals: **window 475 840 B** (rows 1-9), **release path 458 752 B** (rows 10, 11, 13: `fpa` +
`fpos` + `ready`), **control 54 240 B** (row 12 `ftr` plus rows 15-19). Grouped the way the brief asks:
fpa 262 144; fpos 131 072; ready 65 536; window storage 475 840; other 54 240.

Cross-check against the model's own two published quantities:

* `bulk_win_vec_bytes = 475 136` [M, `build/wm/m27/wl1/m27bulk_orkut.txt:429`] equals exactly
  rows 1-6 (`262 144 + 131 072 + 32 768 + 32 768 + 8 192 + 8 192`) — the fused window vectors only.
  The two cohort-*counter* rows (8-9, 192 B) sit outside both; the cohort table itself (row 7) is
  charged through the `sched_bytes` term.
* `bulk_win_bytes = 279 040` [M, same log line 300] equals `2 × (16 384×8 + 512×16 + 256)`, i.e. rows
  1, 5-7 only. So the charge covers 279 040 B of a 475 840 B window: **196 800 B of window state is
  allocated and uncharged** [D].

### 2.3 Reconciliation with M28-B (787 KiB) and M24-H (as-allocated rows)

M28-B's figure is reproduced exactly by `988 832 = 806 048 + 248 320 − 65 536` [D]:

| term | bytes | why |
|---|---:|---|
| M28-B total | 806 048 | 347 296 charged + 458 752 "allocated but uncharged" |
| + omitted window state | 196 608 | `fmeta` 131 072 + `fslot` 32 768 + `fflag` 32 768 |
| + omitted `ftr` | 32 768 | row 12 |
| + omitted cohort counters | 192 | rows 8-9 |
| + omitted engine control | 18 752 | rows 15, 16, 18, 19 (tiles 2 720 were already charged) |
| − double-counted pool | −65 536 | M28-B charged 4 096 × 16 B *and* listed `Batch::ready` 2 × 4 096 × 8 B; one pool exists |
| **= corrected total** | **988 832** | 965.7 KiB |

The pool row deserves the note: the charge is 16 B/entry, the structure is 8 B/entry, and there are
two vectors — so the *coincidence* that both formulas give 65 536 B is what hid the double count.
The hardware pool is one 4 096-entry heap (32 768 B at the declared width); the model charges 65 536 B.

M24-H's §2.1 row-by-row allocated column reproduces every line of my table (its `fent` 262 144,
`fmeta` 131 072, `fpa` 262 144, `fpos` 131 072, `fslot` 32 768, `ready` 65 536, `dir` 16 384,
`fflag`+`ftr` 65 536, `fan_` 9 728, `info_` 7 168, `coh` 512, lists+queue 1 856, tiles 5 376), and its
row sum is 1 122 560 B. My total is that sum minus exactly two differences:
`1 122 560 − 131 072` (its table includes `ford`, which production leaves empty) `− 2 656` (it counts
the simulator's 5 376 B index *ring* where the design owns the charged 2 720 B tile SRAM) `= 988 832`
[D]. Its own three-accounting headline (707 120 packed / 1 106 176 allocated) is not reused: the
1 106 176 total differs from its own row sum by 16 384 B, a labeling difference in the directory row,
and the packed column applies the ×2 convention differently per row. **988 832 B is the number to
carry forward.**

### 2.4 What is deliberately excluded

| excluded | bytes as allocated | why |
|---|---:|---|
| `ftr_gen` + `ftr_done` timestamps | 524 288 | instrumentation; only latency accounting reads them (`bulk.cpp:2350-2351`, `1939-1940`) — not hardware |
| `win_rec` timeline | 432 | diagnostics (`sim.hpp:1372`) |
| `g6g_*`, `g6f_*`, `g6i_*` scratch | 0 in production | allocated only when `sched_group`, `sched_vis`, `g6i_diag` are set; all are 0 in `configs/m27_bulk.cfg` |
| `reuse_` (M13) | 0 | `reuse_dir = 0` |
| `g6a_pa_capacity` path | 0 | `bulk_org = 8` does not use the G6A buffer; `bulk_g6a_pa_bytes = 0` [M] |
| `row_acts_`/`row_stamp_` (`dram_diag = 1`) | 65 536 | pure diagnostics, identical in both arms |
| `val_`/`addr_` unordered_maps | — | simulator containers for completed elements; a hardware fanout writes into the consumer and keeps nothing |
| `idx_done_`/`idx_pending_`/`tgt_pending_` sets | — | simulator bookkeeping; the hardware bound is the 128-slot budget |

---

## 3. What each structure is for, and what it really needs

### 3.1 `fpa` — the physical address buffer

**What it holds.** `std::vector<u64> fpa`, one entry per run, allocated `b.fpa.assign(n, 0)` at the
start of a window's issue phase (`bulk.cpp:2319`), written once per run when its translation lands
(`b.fpa[li] = x.pa`, `bulk.cpp:2351`), and read once when the run is released
(`r.pa_line = b.fpa[li]` with `r.pretranslated = true`, `bulk.cpp:1938`) and once more to classify
the release into a DRAM cell (`mem_.dram_bank_row(b.fpa[li], bk, rw)`, `bulk.cpp:2456`) [C].

**Producers and consumers.** The only producer is the translation completion path (`take_xlate`,
`memory.cpp:1150` builds `pa_line_of(ppn, r.va_line, page_line_bits())` and pushes it into
`xlate_done_`). The consumers are the release (`win_emit_fused`) and the cell-change counter. Nothing
else reads it. Two buffers exist only because `fpa` is a `Batch` member and the window arrays are
duplicated (§1 finding 3).

**Does the physical address have to be stored in full?** **No** — and the decomposition is exact:

```
pa_line = (ppn << 6) | (va_line & 63)        // memory.cpp:31-33, pa_line_of()
```

with `page_line_bits() = 6` at 4 KiB pages. The low six bits are the line's position inside its page,
which is already in `fent`'s 26-bit line field. So `fpa` carries information the window does not: the
**page number**. In the model the map is even simpler — `pa_page_of_vpn(vpn) = vpn + (PA_OFFSET_BYTES
>> page_shift)` for `m24_map != 3` (`memory.cpp:192-193`), i.e. a constant page offset — but that is a
property of the simulation's map and must not be relied on for hardware. The honest hardware statement
is: **store the PPN, derive the offset** [C/D].

| representation | bits | bytes, one live copy at W = 16 384 |
|---|---:|---:|
| as declared (`u64` line + implied valid) | 64 | 131 072 |
| PA line, packed (26 b line + 1 b valid) | 27 | 55 296 |
| **PPN only** (line offset derived from `fent`), 21 b | 21 | 43 008 |
| PPN only, physical address bounded to the 8 MiB DRAM (2 048 pages), 11 b | 11 | 22 528 |
| **pool-indexed**: PPN held in the `cap`-entry pool entry (C) | 11 | 22 528 → **0 as a separate array** |

**Can entries be reused or overwritten earlier?** No — every run is released exactly once, and the
translation and release orders differ (translation is page-cohort order; release is the sealed/min-key
order), so a run's PA must survive from its translation until its release. But that lifetime is bounded
by the *pool*, not by the window: a run not yet translated has no PA, and a released run's PA is dead.
Hence the required depth is `cap` (4 096 → 512), not `W` (16 384) — the 32 768-entry-worth of storage
in `fpa`+`fpos` is 4–8× deeper than the live population [C].

**Is the full capacity required simultaneously?** Only when every element lands on its own line —
`bulk_win_occ_max = 16 384` on Orkut [M], so yes for the *window* arrays, and dedup only lowers it.
For `fpa` the realised maximum is the pool peak, `bulk_g6b_ready_peak = 4 096` on Orkut [M].

### 3.2 `fpos` — position / rank

**What it is.** Not the original request position and not the sorted position of an element: it is the
**rank of a run in `ford`**, i.e. the key under which the run sits in the ready min-heap. Built once at
issue start as the inverse permutation of `ford` (`b.fpos[b.ford.empty() ? i : b.ford[i]] = i`,
`bulk.cpp:2324`), written never again, and read exactly once per run at translation completion to push
the heap key (`ready_push(b, static_cast<u64>(b.fpos[li]))`, `bulk.cpp:2356`) [C].

**Why its width.** `n ≤ batch_window = 16 384`, so 14 bits. The declared `u32` is 2.3× wider than
needed.

**Does it duplicate information stored elsewhere?** **Yes, in the production configuration it
duplicates nothing at all** — it is the identity mapping (§1 finding 2). When the organizer *is* on it
is the exact inverse of `ford`, so the pair `(ford, fpos)` stores one permutation twice: `ford: rank →
run` is what the release reads, `fpos: run → rank` is what translation pushes. Both are needed only
because the heap key must be produced by a different agent (translation) than the release consumer.

Alternatives that remove one of the two, none of them free:

| scheme | storage | change | risk |
|---|---:|---|---|
| `fpos` as identity when `ford` is empty (Candidate A) | 0 B | one select in the push path | none: same integer |
| store only `ford`, push the *run id* and compare by decoding | 0 B for `fpos` | heap comparator must map run → rank by search, or decode `(row,bank,line)` per comparison | log(cap) decodes per release; order-preserving only if the key is monotone |
| pool-indexed entry holds `(PA, rank)` (Candidate C) | `fpos` 0, PA 8 B/entry | release re-indexed by pool slot | the pool depth becomes the PA depth |
| compress `fpos` to 14 b (Candidate B) | 28 672 B (one copy) | repack | none |

**Are both buffers necessary?** No: only the issuing window has a non-empty `fpos` (same argument as
`fpa`). **Is every entry read after being written?** Only entries whose run is actually translated are
read, and every run of a window is translated, so yes — but the array is `W`-deep for a population
bounded by `cap`.

### 3.3 `ready` — the translated-run pool

**Format.** `std::vector<u64> ready`, a min-heap over run positions (`sim.hpp:1730`), with
`ready_push`/`ready_erase_val` implementing exactly the sift operations of
`std::priority_queue<u64, vector, greater<u64>>` (`sim.hpp:1887-1910`). One entry is one *translated
but unissued run*; the key is its rank, so the release always takes the most local line that exists.
Bounded by `g6b_pa_capacity` (`bulk.cpp:2382`) [C].

**Occupancy on the current production cell** (`build/wm/m27/wl1/m27bulk_orkut.txt` [M]):

| quantity | value |
|---|---:|
| capacity | 4 096 |
| peak (`bulk_g6b_ready_peak`) | **4 096** (full) |
| mean occupancy (`bulk_g6b_ready_occ_sum / cycles` = 12 551 332 265 / 6 131 444) | **2 047.0** |
| mean residence per released run (occ_sum / 823 621 releases) | 15 239 cycles |
| cycles ≥ 2 048 entries (`h12`) | 46.6 % |
| cycles ≥ 512 entries (`h10+h11+h12`) | 86.3 % |
| cycles empty (`h0`) | 3.7 % |
| backpressure / full cycles (`bulk_g6b_backpressure_cycles`) | 116 698 = **1.90 %** |

**Maximum versus typical.** The distribution is not concentrated at the top or the bottom: 46.6 % of
cycles are above half capacity and 3.7 % are empty, so a small pool does not sit idle. This is a
different regime from the one the 4 096 was chosen in: M23-G6B measured mean occupancy **33.8** on
Orkut with 2 walkers, and concluded the buffer was "idle, not a bottleneck" [M]. M24-D/M27 run 8
walkers and the TLB retention fix, so translation now outruns release and the pool sits at 50 % of
capacity. **The historical "4 096 is never the bottleneck" does not describe the current machine.**

**Why 4 096?** No derivation exists in the reports — it is a round number chosen when mean occupancy
was 1.1–33.8 (M23-G6B §3.4). M24-E and M24-G are the first sweeps below it, and both conclude it is
oversized (M24-G: "8–16× oversized").

**What happens when it is full.** Admission stops: `if (ready.size() + pend >= cap) break` in the
translation loop (`bulk.cpp:2388`) and the cycle is counted as backpressure (`2380-2383`). No request
is dropped, no correctness rule is weakened; translation simply waits, which is also what shrinks the
translation→release distance. That is why a smaller pool is *faster* on some cells (M24-G: `criteo_2k`
12 % at 256–512) and slower on others (M24-E: Orkut +3.5 % at 512).

**Can the heap metadata be compact?** The key is a rank ≤ 16 384 (14 b) and the number of live entries
is ≤ `cap`; a 14-bit key with `cap` entries is 7 168 B at 4 096 entries — packed with the PA in the
same entry it is 25 b (Candidate C), so the "heap" costs 1 600 B at 512 entries.

### 3.4 Window storage (rows 1-9)

| structure | function | why its capacity is what it is |
|---|---|---|
| `fent` | the window itself: one 64-bit word per pending element, `va_line 26 \| position 32 \| in-line offset 6` (`bulk.cpp:240-257`), sorted numerically at seal so equal lines are adjacent | `batch_window`, the only capacity with demonstrated payoff; all 64 bits are live (`off` is a descriptor-derived address offset, not a function of `pos` — `bulk.cpp:2700`, so it cannot be dropped) |
| `fmeta` | one `start:16 \| count:16` per distinct line, built by the seal scan (`bulk.cpp:1527-1534`) | one entry per run, ≤ W when every element is its own line |
| `fslot` | the run's residency slot, so the fill can batch its fanout (`bulk.cpp:1949`) | 7 bits would do (`R ≤ 128`); 1 B is declared |
| `fflag` | issued bit, also marking "served from the reuse buffer" (`sim.hpp:1603`) | 1 bit; declared 1 B |
| `dir_vpn` + `dir_mask` | page-directory tags and a per-page line mask | **written and never read by a decision** (`bulk.cpp:1296-1305`); `bulk_dir_occ_max = 1 803` of 512 on Orkut, and M24-G measured 128/256/1024 as identical cycles → **dead state, 16 384 B** |
| `coh` | the page-run descriptors the cohort admission walks (`sim.hpp:1547`) | 8 cohorts; M24-E: width 4 costs +13 % on `criteo_2k`, 8 → 16 buys −0.5 % |
| `g6c_coh_def`, `g6c_coh_age` | per-cohort refusal count and deferral-episode start (`bulk.cpp:2515-2516`) | 8 × 12 B; part of the best-value structure in the design |

Every one of these must exist twice (they are written at the seal of the *filling* window and read
during the *issue* of the same window, so a window waiting in state 2 owns its own copy — verified by
the state machine in `bulk.cpp:2966-2996`). This is the one place where the ×2 is functional.

### 3.5 Other state (rows 12, 14-19)

`ford` is the release order, empty in production. `ftr` is the per-run translated bit, allocated in
`win_issue_g6b` (`bulk.cpp:2320`) — so it is subject to the same one-live-copy argument as `fpa`.
`fan_` (128 × 76 B) holds fill/fanout line images bounded by `R`; `info_` is the memory system's
per-slot record (128 × 56 B, 44 packed bits); the index tiles are the charged 2 720 B; the slot lists
and descriptor queue are 1 856 B. All of these are bounded by `R`, not by `W`, and none has a capacity
knob that the sweeps touched.

---

## 4. Existing evidence

Nothing here was measured for this study; every number is quoted.

### 4.1 Ready-pool capacity (M24-E §5, 16 cells, `g6b_pa_capacity = 1, 16, 64, 512`, single-key overrides
on a 94-field pinned base)

| pool | Orkut | LiveJournal | ogbn | Criteo | mean occ (Orkut) | full % (Orkut) |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 5.530× | 7.642× | 8.021× | 7.463× | 0.0 | 97.5 |
| 16 | 1.077× | 1.108× | 1.000× | 1.004× | 5.9 | 59.5 |
| 64 | 1.068× | 1.102× | 1.000× | 1.005× | 33.7 | 40.8 |
| 512 | 1.035× | 1.063× | 1.000× | 1.005× | 322.6 | 33.2 |
| 4 096 | 1.000× | 1.000× | 1.000× | 1.000× | 2 045.3 | 8.1 |

(cycles: 33 900 681 / 6 605 559 / 6 549 636 / 6 346 277 / **6 130 494** on Orkut.) The `full %`
column is M24-E's own label; the production M27 log's backpressure counter gives 1.90 % for the same
cell — a definition difference, noted in §8, not a contradiction, and the *cycle* column is what the
ranking uses.

**M24-G's sweep agrees and goes further** [M]: on `criteo_2k` the curve is monotone from 4 096 down to
256 (`242 935 / 223 713 / 218 313 / 216 817 / 213 801` cycles) — 12 % *faster* at 256 — and on
`w4`/`n1` the pool is irrelevant at every size. Its recommendation is **512** ("8–16× oversized").

### 4.2 Window capacity (M24-G §5-§6)

| window | SRAM (model charge) | `w4` | `criteo_2k` | `n3` | `n1` (relative to 16 K) |
|---|---:|---:|---:|---:|---:|
| 8 K | 213 760 B | 1.125 | 1.367 | 1.123 | 0.992 |
| **16 K (production)** | 279 040 B | 1.000 | 1.000 | 1.000 | 1.000 |
| 32 K | 606 976 B | 0.887 | 0.769 | 0.798 | 0.972 |
| 64 K | 1 131 264 B | 0.808 | 0.657 | — | — |

The window is the one capacity **still paying**, and *shrinking* it is the one change with a
demonstrated double-digit cost. No candidate here reduces it.

### 4.3 What each release-path structure is worth (M24-H §5.3, `org` = 0/6/7/8 ladder)

Re-expressed as a storage ladder, adding exactly one structure set per step:

| step | structures added | packed B | measured effect |
|---|---|---:|---|
| 0 | fused window + cohorts | 448 640 | — |
| 6 | + physical order + PA buffer (barrier) | 646 864 | 1.4–4.1× **worse**: translating a whole window before releasing serialises the phases |
| 7 | + streaming ready pool | 718 544 | recovers 10–30 %, still worse than 0 on three of four traces |
| 8 | + cohort admission = production | 718 640 | `orkut` 0.921, `livejournal` 0.899, `ogbn` 1.000, `criteo_2k` **1.121** |

Two facts from that ladder matter here. First, **the fused representation is a storage optimisation
only**: `org 0` and the unfused arm produce identical cycles at 3.9× the storage. Second, **the
conditional structures are exactly the ones this study shrinks** (`fpa`, `fpos`, `ford`): their
*existence* buys 8.5–11.2 % on the two long graph traces and costs 12 % on `criteo_2k`, so removing
them is not a storage win — reducing their *representation* is. Nothing in the ladder says 64 bits per
PA or 32 bits per rank is what buys that 8.5 %.

### 4.4 The M27 production cell (the anchor every candidate must reproduce)

`build/wm/m27/wl1/m27bulk_orkut.txt` [M]: `cycles = 6 131 444`, `bulk_batches = 74`,
`bulk_win_occ_max = 16 384`, `bulk_win_total_max = 32 768`, `bulk_g6b_ready_peak = 4 096`,
`bulk_g6b_runs_released = 823 621 = bulk_g6b_tr_requests`, `bulk_dir_occ_max = 1 803`,
`bulk_win_bytes = 279 040`, `bulk_g6b_pa_bytes = 65 536`, `bulk_idx_bytes = 2 720`,
`bulk_sched_bytes = 256`, `bulk_win_vec_bytes = 475 136`. M0-RM is 17 904 629 cycles → 2.9201×.

### 4.5 Invariants and tests that already exist

| check | where | usable as-is for a storage change? |
|---|---|---|
| `bulk_g6b_ready_occ_sum == bulk_g6b_tr_wait_dram` | closed-accounting invariant, quoted by M23-G6B §3.4; **holds exactly on the M27 cell** (12 551 332 265 both) [M] | **yes** — a pool resize must keep it exact |
| `bulk_g6b_ready_peak ≤ bulk_g6b_pa_capacity` | M24-E effect check | yes |
| `win_occ_max ≤ W`, `win_total_max ≤ 2W` | M24-H/M12 §4 invariants; 16 384 / 32 768 on Orkut | yes — and a single window must make `win_total_max ≤ W` |
| request conservation, `residency_violations`, `issue_monotonic_violations` | `bulk.cpp` stats; 0 in production | yes, unchanged |
| element conservation (`instr_sum_ok`, `acc_sum_ok`, `addr_mismatch = 0`, `completion_hash`) | M28-A harness; M27 log | yes |
| test suite | `./bin/dx100_tests`, 84 tests / 4 967 checks / 0 failures; `test_bulk_buffer_capacity` (875), `test_determinism` (231), `test_bulk_residency_invariant` (901), `test_m6_resource_accounting` (1697) | yes, but **no test exercises `g6b_pa_capacity` or `win_fused` storage bounds** — see §8 |

### 4.6 Were the previous experiments clean?

Yes, for capacity: M24-E's pool cells change one key on a 94-field pinned base and prove the effect
with `bulk_g6b_pa_capacity=N` and `ready_peak ≤ N`; M24-G's window cells likewise. The limit is not
methodological but temporal: those cells ran on the M24-D code, before M27-P1/P2/P3, and the pool's
occupancy regime changed by 60× since (33.8 → 2 047 mean). **The ablation is clean; its transfer to
M27 is an assumption that has to be re-tested, not a result.** M24-G's `criteo_2k` cell is the one
place where the pool's *depth* was measured as beneficial to shrink, and it is a 2 K-element trace —
which is exactly the sort of generalization the brief warns against.

---

## 5. Candidate storage configurations

Each candidate is a full configuration of the same structures; the deltas are cumulative left to
right. Every row's derivation is given under the table.

| Structure | Current | Candidate A | Candidate B | Candidate C |
|---|---:|---:|---:|---:|
| `fpa` | 262 144 | 131 072 | 43 008 | **0** (folded into the pool entry) |
| `fpos` | 131 072 | **0** | 0 | 0 |
| `ready` | 65 536 | 32 768 | 7 168 | 1 600 |
| Window storage (rows 1-9) | 475 840 | 475 840 | 442 304 | 221 152 |
| Other (rows 12, 14-19) | 54 240 | 37 856 | 15 008 | 15 008 |
| **Total bytes** | **988 832** | **677 536** | **507 488** | **237 760** |
| **Total KiB** | **965.7** | **661.7** | **495.6** | **232.2** |
| **Saved** | — | **311 296 (31.5 %)** | **481 344 (48.7 %)** | **751 072 (76.0 %)** |
| Engine ÷ shared hierarchy (1 220 224 B) | 0.81× | 0.56× | 0.42× | 0.19× |

**Derivations.**
*Current:* §2.2. `ready` = 2 × 4 096 × 8.
*A:* `fpa` = 1 × 16 384 × 8 (one live copy); `fpos` = 0 (identity when `ford` is empty); `ready` =
1 × 4 096 × 8; window unchanged; other = `ftr` one copy 1 × 16 384 = 16 384 + `fan_` 9 728 + `info_`
7 168 + tiles 2 720 + lists 1 536 + queue 320 = 37 856.
*B:* `fpa` = 1 × 16 384 × 21/8 = 43 008 (PPN 21 b, one live copy); `ready` = 1 × 4 096 × 14/8 = 7 168
(14-bit rank); window = 262 144 + 131 072 + 16 384×7/8×2 (= 28 672 `fslot`) + 16 384×2/8×2 (= 8 192
`fflag`+`ftr` merged into a 2-bit run state) + 512×90/8×2 (= 11 520 directory packed to `vpn 26 +
mask 64`) + 512 + 192 = 442 304; other = 9 728 + 128×44/8 (= 704 `info_` packed) + 2 720 + 1 536 + 320
= 15 008.
*C:* `fpa` = 0 and `ready` = 512 × 25/8 = 1 600, because the pool entry holds `(PPN 11 b, run 14 b)`
and the run's PA dies with its release; window = the same packed form at **one** buffer (16 384 × 8 +
16 384 × 4 + 14 336 + 4 096 + 5 760 + 256 + 96) = 221 152; other unchanged from B.

### Candidate A — conservative (no behaviour change)

Changes: (i) one live copy of `fpa`/`fpos`/`ftr`/`ready`, stored once and re-used by whichever window
holds `iss_`; (ii) the `fpos` push becomes `ford.empty() ? li : fpos[li]`.
Storage 988 832 → 677 536 B. Behaviour: **identical** — same integers, same order, same capacity per
live window, same admission gate. Risk: a lifetime bug if a window could own release-path state
without holding `iss_`; the state machine (`bulk.cpp:2966-2996`) says it cannot, and `bulk_g6b_windows`
versus `bulk_batches` (74 = 74 on Orkut) is the runtime check. Page organization: untouched.

### Candidate B — balanced (representation narrowing)

Adds: packed field widths (`fpa` 21 b, `fpos` 14 b, `ready` 14 b, `fslot` 7 b, `fflag`+`ftr` 2 b,
directory 90 b, `info_` 44 b). Storage 507 488 B. Behaviour: identical by construction — every field's
new width is chosen to cover the range the current code already asserts (`R ≤ 128` for `fslot`,
`n ≤ 16 384` for rank, `page_lines = 64` for the mask). Risk: the widths are *design* choices (a
21-bit PPN presumes a 4 GiB-addressable physical space; 11 b presumes the DRAM bound), and a width
mistake is a silent truncation, so each needs an assertion rather than a comment. No capacity,
admission, ordering or DRAM change. Page organization: untouched.

### Candidate C — aggressive (buffering + metadata)

Adds: a **single** window (`org_overlap = 0`), a **512-entry** pool, and the pool entry carrying the
PA. Storage 237 760 B. Performance risk is real and partly measured: the pool at 512 costs +3.5 % on
Orkut and +6.3 % on LiveJournal (M24-E, pre-M27 code) and is 10–12 % *faster* on `criteo` (M24-G);
the single window serialises seal against issue, whose cost on M27 is **unmeasured** (M11-2 measured
`org_overlap = 0` as neutral on activations in a much older engine). Correctness/liveness risk: with
one buffer the sealer cannot fill while the previous window drains, so the engine's steady state
becomes window-synchronous; `win_total_max` must fall from 32 768 to ≤ 16 384, and the liveness
argument now rests on "a sealed window always drains", which the existing per-window path provides.
Page organization: preserved — same cohorts, same sealed page order, same admission policy, same
release key; only the *population* of translated runs and the number of window buffers change.

---

## 6. Optimization opportunities and risks

Ranked within each class; "saves" is against the 988 832 B inventory.

| # | class | change | current | proposed | saves | arch. change | perf. risk | correctness risk | page org. preserved |
|---|---|---|---:|---:|---:|---|---|---|---|
| 1 | redundant state | `fpos` is the identity when `ford` is empty | 131 072 | 0 | 131 072 | none (a select) | none | none | yes |
| 2 | earlier reuse | one live copy of `fpa`/`fpos`/`ftr`/`ready` | 491 520 | 245 760 | 245 760 | store one, index by `iss_` | none | lifetime (provable) | yes |
| 3 | narrower representation | `fpa` stores PPN, not line | 262 144 | 43 008 | 219 136 | derive offset from `fent` | none | width assertion | yes |
| 4 | narrower representation | rank/`fpos` 14 b, `ready` key 14 b | 131 072+65 536 | 0+7 168 | 189 440 | repack | none | width assertion | yes |
| 5 | narrower representation | `fslot` 7 b, `fflag`+`ftr` 2 b, dir 90 b, `info_` 44 b | 32 768+32 768+32 768+16 384+7 168 | 28 672+8 192+11 520+704 | 72 768 | repack | none | width assertion | yes |
| 6 | shared storage | pool entry carries `(PA, rank)`; `fpa`/`fpos` disappear as arrays | 458 752 | 1 600 (at 512) | 457 152 | release re-indexed by pool slot | pool depth becomes PA depth | slot/PA lifetime binding | yes |
| 7 | smaller capacity | pool 4 096 → 512 (or 1 024) | 65 536 (2 copies) | 8 192 (2 copies at 512) | 57 344 | none | +3.5 % Orkut, −10 % criteo (M24-E/G) | none (backpressure only) | yes |
| 8 | reduced buffering | one window (`org_overlap = 0`) | 475 840 (window arrays) | 237 920 | 237 920 | sealer serialised | **unmeasured** on M27 | liveness argument narrows | yes |
| 9 | smaller capacity | window 16 K → 8 K | 475 840 | 237 920 (declared widths), 213 760 (charge) | 237 920 | none | **+12 to +37 % — rejected** | none | yes |
| 10 | elimination | page directory (measured null) | 16 384 | 0 | 16 384 | remove a structure | none measured | none | yes |

**Rejected or de-prioritised, and why:**

* **#9 (8 K window)** contradicts the brief's own objective. It is the largest single saving
  available and the only one with a *measured double-digit loss*; no candidate uses it.
* **#10 (directory)** is measured null (M24-G: identical cycles at 128/256/1024; M24-H §2.4: "written,
  never read") but the brief says not to remove structures, and it is dead-state removal rather than a
  storage-design question. Kept in the inventory, not in a candidate.
* **`fent` 26+32+6 = 64 bits is exactly full.** `off` is a descriptor-derived address offset
  (`bulk.cpp:2700`), not a function of `pos`, so it cannot be dropped; `pos` is a 32-bit descriptor
  position and could only shrink if window elements were a contiguous range of descriptor positions,
  which nothing guarantees (a window can hold several pages' worth of a strided descriptor and the
  seal reorders them). **Not proposed; see §8.**
* **Sharing `fent`/`fmeta` between the two windows** is impossible: both are live at once by
  construction (one issuing, one filling) — the ×2 there is the design.
* **Reducing `R`** is out of scope: it is the shared residency budget, identical in M0-RM.

**The one non-obvious risk in the strong candidates.** #6 makes the *pool depth* the PA-buffer depth.
The brief's warning applies directly: a low mean occupancy does not prove a smaller capacity is safe,
because capacity is what bounds the *worst case* — and on Orkut the pool runs full 1.9 % of cycles and
above half capacity 46.6 % of them. So #6 must be paired with a measured capacity, not with the mean.

---

## 7. Recommended targeted experiments

Three, in value order. None was run. All use `configs/m27_bulk.cfg` as the baseline and the same
workload files as M27 (`build/wm/m23/m23_orkut_bfs.bin`, `build/wm/m23/m23_criteo_16k.bin`).

### E1 — Candidate A: prove the 311 KiB is free (highest value, lowest risk)

* **Change:** one live copy of `fpa`/`fpos`/`ftr`/`ready` (indexed by `iss_`), and
  `key = ford.empty() ? li : fpos[li]` at `bulk.cpp:2356`. No capacity, width, ordering or dispatch
  change.
* **Workloads:** Orkut BFS and Criteo 16K.
* **Baseline:** `configs/m27_bulk.cfg`, cycles 6 131 444 (Orkut) and the M27 Criteo cell.
* **Expected result:** **byte-identical** cycles and counters; reported storage 988 832 → 677 536 B.
  A non-identical cycle count falsifies the lifetime argument and must be reported as a failure, not
  adjusted for.
* **Checks:** full suite (4 967); `bulk_batches == bulk_g6b_windows`; `ready_peak ≤ cap`;
  `occ_sum == tr_wait_dram`; `win_occ_max ≤ 16 384`; `win_total_max ≤ 32 768`; `addr_mismatch = 0`;
  `completion_hash` unchanged; `residency_violations = issue_monotonic_violations = 0`;
  element/instrument conservation flags; two-run determinism.

### E2 — Ready-pool depth on the M27 machine (the largest capacity question that is not yet answered)

* **Change:** `g6b_pa_capacity ∈ {512, 1 024, 2 048, 4 096}` on top of E1, one key per cell.
* **Workloads:** Orkut (pool full 1.9 %, mean 2 047) and Criteo 16K (the cell where 512 was faster in
  M24-G); `ogbn` optional as the translation-bound control.
* **Baseline:** E1's build at 4 096.
* **Expected result:** 512 costs ≈ +3.5 % on Orkut (M24-E) and is neutral-to-faster on Criteo;
  2 048 should be within noise of 4 096. The deliverable is a *measured* depth/performance curve for
  M27, not a re-quote of M24-E.
* **Checks:** `bulk_g6b_pa_capacity = N`, `ready_peak ≤ N`, `occ_sum == tr_wait_dram` at every N,
  backpressure counter rises monotonically as N falls, and no run ends with `pending != 0`.

### E3 — Release-path representation (the 400 KiB that `fpa`/`fpos` no longer need)

* **Change:** `fpa` stores a 21-bit PPN (offset derived from `fent`), `fpos`/`ready` use 14-bit ranks,
  `ftr` merged into a 2-bit run state with `fflag`; optionally the pool entry form of §6 #6.
* **Workloads:** Orkut and Criteo 16K.
* **Baseline:** E1 at the capacity E2 selects.
* **Expected result:** **identical cycles** to E1/E2 — this is a pure encoding change, so any cycle
  difference is a bug.
* **Checks:** `addr_mismatch = 0`, `completion_hash` identical, `instr_sum_ok = acc_sum_ok = 1`,
  all `bulk_*` counters identical except the storage reports, plus the full suite.

**Not recommended now:** the single-window candidate (#8). It is the second-largest saving but its
M27 cost is unmeasured and it changes the sealer's steady state, so it should follow E1–E3 rather
than lead them.

---

## 8. Explicit unknowns

| item | status |
|---|---|
| absolute area, timing, power | **none claimed.** This study stops at bytes; no synthesis, no gate count, no Fmax, no density model. A packed-bit figure is not an area figure. |
| `fpa`/`fpos` widths in RTL | the PPN is 21 b only if the physical address is what the 4 GiB VA space implies; 11 b only if the physical address is bounded to the 8 MiB DRAM. Which one the controller's bank hash actually needs is **unverified** — `dram_bank_row` hashes the line (`memory.cpp:479-505`) and I did not confirm that only the low 17 bits matter. A design must check it before choosing 11 b. |
| `fent`'s 32-bit position field | not reducible under the current admission rule, but I did not prove that window elements form a contiguous descriptor-position range. If they do, `pos` becomes `base + ordinal` (14 b + one 32-bit base per window, ≈ 73 728 B saved for two buffers). **Unproven; not proposed.** |
| single-window cost on M27 | M11-2 measured `org_overlap = 0` as neutral in an M11 engine (2 149 vs 2 160 activations); M9's E2 uses one live window for a different reason. Nothing measures it on the current pipeline, and it is the second-largest saving in the study. |
| pool depth transfer | M24-E/G ran before M27-P1/P2/P3 and at 2 047-mean occupancy M27 is in a different regime from the 33.8-mean one the 4 096 was chosen in. E2 exists because of this. |
| stampede effects of a smaller pool | a smaller pool shortens the translation→release distance, which is why `criteo` gets *faster*. Whether that also reduces DRAM row locality at 2 banks was not measured; the counters to do it (`bulk_sched_opens`, `dram_row_hits`) exist. |
| `ready`'s 16 B/entry charge | the structure is 8 B/entry. The charge has no line-level explanation in the source (the comment at `bulk.cpp:2343` gives only the product). Treat the model's pool cost as 2× the struct and say so. |
| M24-E's "full %" | 8.1 % on Orkut at 4 096 versus 1.90 % from the M27 log's backpressure counter. Different definitions, unknown which one M24-E used; the cycle columns are unaffected. |
| test coverage of storage knobs | no unit test asserts `g6b_pa_capacity`, `win_fused` storage bounds or the identity property of `fpos`. E1–E3 need new assertions (a check, not a test-suite fix). |
| the fanout backlog | `bulk_fanout_max_backlog` exceeds `R` in some M24 cells because the queue is drained by lanes, not bounded by slots; the hardware bound used here is `R` (128 × 76 B), which may understate a real implementation's buffering. |

---

## 9. Recommendation

**Run E1 first: Candidate A, the 311 296 B that should cost nothing.**

It is the only change in this study whose predicted outcome is *exact identity* of every counter, so
it cannot be argued into existence by a plausible-looking speedup: either the M27 anchors reproduce to
the cycle and the storage report drops by 31.5 %, or the lifetime argument in §1 is wrong and the
architecture has a second live window that `iss_` does not describe. Both outcomes are informative on
the first try, the change touches no capacity, no policy, no ordering and no dispatch, and it converts
"the engine is ~1 MiB" into "the engine is ~660 KiB" without a single performance claim.

Only after that should the pool depth (E2) be measured on the M27 machine, because it is the one
remaining capacity whose *current* value has never been justified on this configuration — the 4 096
was chosen when the pool's mean occupancy was 33.8 and it now sits at 2 047 of 4 096, full 1.9 % of
cycles.

**Stopped here.** No source, test or configuration file was modified; no simulation was run; no
structure was removed; no synthesis result is claimed; no historical artifact was overwritten.

---

## 10. Evidence index

| claim | source |
|---|---|
| inventory entry counts and widths | `sim.hpp:1550-1810` (`Batch`, `Cohort`, `IdxRing`, `FanJob`), `sim.hpp:1463` (`Pending`), `bulk.cpp:2319-2324` (`fpa`/`ftr`/`fpos` sizing), `bulk.cpp:2382` (`ready` bound), `bulk.cpp:3356-3359` (slot pool) |
| `fpa` producer/consumer | `memory.cpp:1150` (PA formed), `bulk.cpp:2351` (written), `bulk.cpp:1938` (read), `bulk.cpp:2456` (cell decode) |
| `pa_line = (ppn << 6) \| (va_line & 63)` | `memory.cpp:31-33`; `memory.cpp:192-196` (page map) |
| `fpos` construction and the identity case | `bulk.cpp:2324`, `bulk.cpp:2356`, `sim.hpp:1879-1881`, `bulk.cpp:1585` |
| one issuing window | `bulk.cpp:2966-2996` (`iss_` adopt / clear), `sim.hpp:1879` (`run_at`) |
| window/ready/tile charges | `bulk.cpp:4355-4360`, `bulk.cpp:2343`, `bulk.cpp:4201-4213`, `bulk.cpp:1263-1266` |
| fused word layout | `bulk.cpp:240-257`; `bulk.cpp:2700` (offset used, not derivable from `pos`) |
| production counters | `build/wm/m27/wl1/m27bulk_orkut.txt`: cycles 22/537, `win_occ_max` 337, `win_total_max` 428, `ready_peak` 98, `ready_hist` 84-96, `occ_sum` 97, `tr_wait_dram` 108, `runs_released` 102, `dir_occ_max` 49, `win_bytes` 300, `pa_bytes` 82, `idx_bytes` 195, `sched_bytes` 265, `win_vec_bytes` 429 |
| pool capacity curve | `docs/m24-e-mechanism-ablation.md` §5 |
| window and pool scaling | `docs/m24-g-resource-scaling.md` §5-§6 |
| structure-by-structure cost ladder | `docs/m24-h-hardware-cost-audit.md` §2.1-§2.4, §5.3-§5.4 |
| prior storage totals | `docs/m28-b-resource-audit.md` §4, §7 |
| `org_overlap` history | `docs/m11-2-dram-scheduler-observability-report.md` §(refuted interleaving), `docs/m12-window-capacity-scaling-report.md` §1.1 |
| pool was "never a bottleneck" (pre-M27) | `docs/m23-g6b-streaming-translation-dram-decoupling.md` §3.4 |
| test inventory | `sim/tests/tests.cpp` (`test_determinism` 231, `test_bulk_buffer_capacity` 875, `test_bulk_residency_invariant` 901, `test_m6_resource_accounting` 1697) |
