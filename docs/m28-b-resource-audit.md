# M28-B — Resource audit: M0-RM vs M27 Bulk, custom DRAM organizer off and on

Read-only audit. No source, test or configuration file was modified, no simulation was run, no
synthesis result is claimed. Every number below is either **verified** (read from the effective
configuration or a preserved log), **derived** (computed from a declared array size and a struct field
list), or **estimated** (a derived figure that assumes a maximum occupancy). Each is labelled.

---

## Executive summary

**M0-RM is genuinely resource-matched to M27 for every conventional shared resource.** The two
configurations differ in exactly **nine** keys; eight are the Bulk pipeline (`dispatch`,
`batch_window`, `win_fused`, `bulk_org`, `dedup`, `page_sched`, `page_cohorts`, `page_order`) and the
ninth, `m24_dram_org`, is read only inside the window path and is therefore **inert in M0-RM**.
L1, L2, both TLBs, both MSHR files, walkers, page geometry, the 128-slot residency budget and the
DRAM controller are identical by value *and* by code path (§4, §7).

**M27 adds 347,296 B (≈339 KiB) of modelled engine state plus 458,752 B (≈448 KiB) of per-run arrays
the model allocates but does not charge**, for a derived implementation requirement of ≈738 KiB of
engine SRAM (window + ready pool), sorter off. Page organization alone accounts for essentially all of
it; the DRAM organizer adds only **65,536 B** of persistent state (the `ford` rank array) plus a
3-key stable sort over up to 16,384 runs and 196,608 B of transient sort working set.

**The custom DRAM organizer is instantiated, supported, and switchable**, but it is **not** part of the
production M27 architecture and the existing experimental evidence does not justify carrying its state
always-on: it measured ±0.16 % on the production map and is valuable only on scrambled/mapped layouts,
where M24-A measured 1.5–3.7 %.

---

## 1. Methodology

1. Read every configuration field that can influence resources (`sim.hpp:44-470`), the parser
   (`util.cpp:120-160`) and the effective production file (`configs/m27_bulk.cfg`, 163 lines).
2. Diffed the effective M0-RM file (`build/wm/m24d/m24d_simt.cfg`) against it, comments stripped, to
   enumerate every difference rather than assume one.
3. Read the constructors that allocate state: `MemorySystem::MemorySystem` (`memory.cpp:55-160`) and
   `BulkEngine::BulkEngine` (`bulk.cpp:~330-400`), plus every `assign`/`resize` in `bulk.cpp`.
4. Read the accounting expressions the model itself publishes (`bulk.cpp:4364-4370` for window SRAM,
   `4201-4213` for index tiles, `2110` and `2343` for the PA buffers, `3085-3112` for realized
   vectors), so a storage claim is either the model's own charge or explicitly separated from it.
5. Read the organizer (`bulk.cpp:1578-1632`), its guard (`363-376`), its call site and its charging
   (`1310`, `1631`, `2964-2968`).
6. Consulted existing reports for measured effects only (M24-A, M24-D, M24-E, M25, M26-A, M27): no
   claim of a measured number is made from source inspection alone.

Nothing was executed. The only numbers that are not read directly are simple products of a declared
count and a declared field list; those are marked **derived**.

---

## 2. Configurations compared

| | A: M0-RM | B: M27 sorter off | C: M27 sorter on |
|---|---|---|---|
| file | `build/wm/m24d/m24d_simt.cfg` (preserved) | `configs/m27_bulk.cfg` (production) | B + `--set m24_dram_org=1` (or `build/wm/m24d/m24d_bulk.cfg`) |
| `dispatch` | 0 | 1 | 1 |
| `batch_window` / `win_fused` | 0 / 0 | 16 384 / 1 | same as B |
| `bulk_org` | 0 | 8 | 8 |
| `dedup` / `page_order` / `page_sched` / `page_cohorts` | 0 / 0 / 0 / 0 | 1 / 1 / 1 / 8 | same as B |
| `m24_dram_org` | **1** (inert) | **0** | **1** |

**Everything else is byte-identical between A and B** (verified by `diff` of the comment-stripped
files: nine differing lines, listed above). All values below are the effective ones and are confirmed
by the run echo, e.g. `dispatch=1 op_class=gen dispatch_order=0 pool_slots=R` and
`m24_map=0 m24_dram_org=0 m24_perm_seed=1 page_bytes=4096`.

**Why `m24_dram_org = 1` in A is inert:** the flag is read in exactly one function,
`BulkEngine::win_order_fused` (`bulk.cpp:1585`), which is reached only from the window seal path
(`win_seal_fused`), which requires `win_fused = 1`, which requires `batch_window > 0`. A has
`dispatch = 0`, `batch_window = 0` and never constructs a `BulkEngine`. The M27 measurement confirms it:
A reproduces the historical M0-RM log exactly (17 904 629 cycles) and emits no `bulk_*` counters.

**Is C still supported?** Yes, and it runs. The constructor refuses `m24_dram_org = 0` only for
`bulk_org ∈ {1,2,3,5}` whose definition *is* the physical organizer (`bulk.cpp:371-375`); it refuses
`m24_dram_org = 1` for nothing. With `bulk_org = 8` the organizer executes normally: the M27 anchor
`build/wm/m27/bulk_orkut_hist.txt` shows `bulk_org_runs = 823 604` and 6 130 567 cycles, against
`bulk_org_runs = 0` and 6 131 444 cycles for B. So C is a supported, measured configuration, not a
vestigial flag.

---

## 3. Shared conventional resources

Verified from `configs/m27_bulk.cfg` and the allocating code. **Identical in A, B and C.**

| resource | effective value | allocated at | storage (derived) | status |
|---|---|---|---|---|
| lanes | 4 | `sim.hpp:49`, file | – | **verified** |
| warps / cores | 4 / 1 | file | – | verified |
| L1 | 256 lines, 4-way (64 sets), 16 KiB, latency 2 | `memory.cpp:66` | data 16 384 B; tags 256 × 8 B = 2 048 B | verified + derived |
| L1 MSHRs | 128 entries | `memory.cpp:69` | 128 × 24 B = 3 072 B | verified + derived |
| L2 | 16 384 lines, 8-way (2 048 sets), 1 MiB, latency 4 | `memory.cpp:67` | data 1 048 576 B; tags 16 384 × 8 B = 131 072 B | verified + derived |
| L2 MSHRs | 64 entries | `memory.cpp:70` | 64 × 24 B = 1 536 B | verified + derived |
| dTLB | 16 entries/core × 1 core, 4-way | `memory.cpp:58` | 16 × 32 B = 512 B | verified + derived |
| dTLB MSHRs | 8, each parking ≤ 2 (`dtlb_parked`) | `memory.cpp:59` | 8 × 24 B = 192 B | verified + derived |
| L2 TLB | 512 entries, 4-way, latency 4 | `memory.cpp:60` | 512 × 32 B = 16 384 B | verified + derived |
| L2 TLB MSHRs | 8 | `memory.cpp:61` | 8 × 32 B = 256 B | verified + derived |
| page walkers | 8 | `memory.cpp:62` | 8 × 8 B busy stamps = 64 B | verified + derived |
| walk cache | 16 entries | file | 16 × 8 B = 128 B | verified + derived |
| page geometry | 4 KiB pages, 2 levels, 150 cycles/level | file | – | verified |
| residency budget `R` | 128 line requests | file; `bulk.cpp:3356-3359` | one `SlotPool` (128 × 5 B) + 128 `Req` records in `MemorySystem::inflight_` | verified |
| DRAM geometry | 2 banks × 4 096 rows × 1 024 B = **8 MiB** | `memory.cpp:73-76` | per-bank: ready 8 B, row 8 B, open 1 B | verified |
| DRAM queue | `dram_q = 0` → **unbounded** deque (`memory.cpp:556` tests `cfg_.dram_q &&`) | `memory.cpp:71` | grows to the L2 MSHR limit (~64) | verified |
| DRAM scheduler | FR-FCFS (`frfcfs = 1`), XOR bank hash, `t_hit 14`, `t_miss 26`, `link 0` | file | – | verified |
| HBM hierarchy | not instantiated (`dram_kind = ddr`) | `memory.cpp:126` | 0 | verified |

**The single per-request record is shared.** Both front-ends submit `Req` objects into the same
`MemorySystem::inflight_` and the same pipeline; there is no second request path, no second probe, no
engine-private cache or TLB. The engine's requests are ordinary `Req`s with `r.core = ecore()`
(`bulk.cpp:773, 1040, 1362, 1927, 2135, 2390, 2595`), and `mem_path = l1` makes
`path_bypasses_l1()` return false for them (`sim.hpp:1017-1019`), so they enter **core 0's L1 and
dTLB** — the same structures M0-RM uses.

---

## 4. Bulk-specific resources

### 4.1 What the model charges (verified)

The simulator publishes its own SRAM charge, and it is the number to quote first
(`bulk.cpp:4364-4370`):

```
bulk_win_bytes = bufs × ( batch_window × 8  +  page_dir × 16  +  sched_bytes )
               = 2 × (16 384 × 8 + 512 × 16 + 256)
               = 2 × (131 072 + 8 192 + 256) = 279 040 B = 272.5 KiB
```

(`bufs = 2` because `org_overlap = 1` and `win_policy = 255 → E3 ≠ 1`; `sched_bytes = page_cohorts ×
32 = 256 B` at `bulk.cpp:1263-1266`.) Confirmed at runtime: `bulk_win_bytes = 279 040`,
`bulk_sched_bytes = 256`.

| structure | charge | bytes | basis |
|---|---|---|---|
| fused window, 2 buffers, 16 384 elements each, one 64-bit fused word per element | `bulk_win_bytes` | 262 144 | verified (`bulk.cpp:4368`) |
| page directory, 512 entries × 16 B per buffer | `bulk_win_bytes` | 16 384 | verified |
| page-cohort descriptors, 8 × 32 B per buffer | `bulk_sched_bytes` | 512 | verified |
| index tiles, 2 × 64 lines × 21 B + 32 B | `bulk_idx_bytes` | **2 720** | verified (`bulk.cpp:4201-4213`) |
| translated ready pool, 4 096 entries × 16 B | `bulk_g6b_pa_bytes` | **65 536** | verified (`bulk.cpp:2343`) |

**Modelled engine SRAM total: 279 040 + 2 720 + 65 536 = 347 296 B ≈ 339.2 KiB.**

Note the ready pool is reported **separately** and is *not* inside `bulk_win_bytes`. An earlier reading
of `bulk_win_bytes` alone (272.5 KiB) understates the engine by 67 KiB.

### 4.2 What the model allocates but does not charge (derived — the audit's main new finding)

`bulk_win_bytes` and `bulk_win_vec_bytes` do not include four per-run arrays that the code allocates and
uses functionally. `n` is the number of distinct lines in a window, ≤ `batch_window` = 16 384.

| array | declared | used for | bytes (n = 16 384, 2 buffers) |
|---|---|---|---|
| `Batch::fpa` | `sim.hpp:1712`, allocated `bulk.cpp:2319` | the run's translated physical line, written per translation (`2351`), read by the release (`2449`) and by the request (`1938`) | 8 × n × 2 = **262 144** |
| `Batch::fpos` | `sim.hpp:1731`, allocated `bulk.cpp:2323` | the run's rank in `ford`, i.e. the ready-heap key (`2356`) | 4 × n × 2 = **131 072** |
| `Batch::ready` | `sim.hpp:1730`, bounded by `g6b_pa_capacity` (`bulk.cpp:2382`) | the min-heap of translated-but-unissued runs | 8 × 4 096 × 2 = **65 536** |
| `Batch::ford` | `sim.hpp:1707`, built `bulk.cpp:1592` | the release order — **only when the organizer is on** | 4 × n × 2 = **131 072** |

Total uncharged, organizer off: **458 752 B ≈ 448.0 KiB**. With the organizer on: **589 824 B ≈
576.0 KiB**.

The model's own measurement of realized simulator storage agrees with the charged part and confirms the
omission: `bulk_win_vec_bytes = 475 136` B for the Orkut cell, which is exactly
`2 × (16 384×8 + 16 384×4 + 16 384 + 16 384 + 512×8 + 512×8)` — the fused and directory vectors only
(`bulk.cpp:3094-3105`), with no `fpa`/`fpos`/`ready`/`ford` term.

**So the honest implementation figure for the engine, sorter off, is ≈ 347 KiB charged and ≈ 806 KiB
modelled-plus-allocated (347 296 + 458 752 = 806 048 B ≈ 787 KiB).** I label the second figure an
**estimate**, because it assumes every window reaches 16 384 distinct lines (dedup only lowers it) and
uses the simulator's field widths, which are wider than a real design needs in several places.

### 4.3 Separation required by the brief

| category | structures |
|---|---|
| **Hardware state required for functionality** | fused window (2 × 16 384 × 8 B), page directory (2 × 512 × 16 B), cohort descriptors (2 × 8 × 32 B), ready pool (4 096 × 16 B), index tiles (2 × 64 × 21 B), PA per run (`fpa`), rank per run (`fpos`), ready heap, `SlotPool` (shared) |
| **Simulator-only state** | `std::vector`/`std::deque`/`std::map`/`std::set` containers used as implementation containers (`idx_done_`, `idx_pending_`, `tgt_pending_`, `val_`, `addr_`, `info_`, `unissued_`, `idx_q_`, `free_`, `fan_`); their *capacity* is reported as `bulk_win_vec_bytes` and is not a design quantity |
| **Instrumentation / debugging only** | `g6g_*` scratch (5 vectors over banks×rows when `sched_group` set — off), `g6f_stamp_`/`g6f_scratch_` (off), `g6i_*` (off), `BulkStats` counters, `win_recs`, `life_*` in `MemorySystem` (off), `row_acts_`/`row_stamp_` (allocated only when `dram_diag = 1`, which the production file *does* set — 2 × 8 192 × 4 B = 65 536 B of pure diagnostic state), `ClientCacheStats cc[4]` |
| **Dead / unused in this configuration** | `g6a_pa_capacity = 65 536` and `fpa/ftr/ftr_gen/ftr_done` sizing path for `bulk_org = 6` (not used; `bulk_g6a_pa_bytes = 0` in the production log), `ReuseEnt` array (`reuse_dir = 0` → 0 B), `idx_mode = 0` path, `Batch::ent`/`sorted`/`lines`/`order`/`coh` in fused mode (stay empty — this *is* the storage argument for the fused layout, `sim.hpp:1660-1668`) |
| **Allocated but uncharged (see 4.2)** | `fpa`, `fpos`, `ready`, `ford` |

---

## 5. Custom DRAM organizer: what it actually adds

### 5.1 The implementation

`BulkEngine::win_order_fused` (`bulk.cpp:1578-1632`), called once per seal from `win_seal_fused`:

```cpp
if (!cfg_.m24_dram_org) return;                 // OFF: ford stays empty, fpos is identity
if (org == 0 || org == 4) return;               // P0/P4: no order to compute
b.ford.resize(n); for (i) b.ford[i] = i;
std::vector<u32> bank(n), row(n), vpage(n);     // transient, per seal
for (i) mem_.dram_bank_row(mem_.pa_line_of_va_line(run_line(b, i)), bank[i], row[i]);
std::stable_sort(b.ford.begin(), b.ford.end(), [&](x, y) {
    if (row[x] != row[y]) return row[x] < row[y];   // key 1
    if (bank[x] != bank[y]) return bank[x] < bank[y]; // key 2
    return x < y;                                     // key 3 = sealed line order
});
st_.org_runs += n; st_.org_backlog += n;
```

**It is genuinely instantiated**, not a configuration ghost: it allocates, it decodes every run's
physical address through the controller's own mapping function, it sorts, and its output is consumed —
`fpos` becomes the ready-pool heap key (`bulk.cpp:2324, 2356`), so the release stream is the physical
order. When off, `ford` is empty, `run_at()` returns the identity (`sim.hpp:1800-1803`), and `fpos` is
the stored position — the sealed ascending-line order.

### 5.2 Incremental resources, C over B

| item | cost | label |
|---|---|---|
| persistent state: `ford`, 4 B × n per buffer | **131 072 B** (2 buffers, n = 16 384) | derived |
| transient sort working set: `bank`, `row`, `vpage` (3 × 4 B × n) | **196 608 B** during one seal | derived |
| comparator | 3-key lexicographic, stable → the sort must be stable, so a real implementation needs the run index as the final key | verified (code) |
| sorting network / merge sorter | `n log₂ n` ≈ 16 384 × 14 ≈ **229 k comparisons per seal**, × 74 seals = 17 M comparisons per Orkut run | estimated |
| physical-address decode per run | one `dram_bank_row` (XOR fold + mod) per run per seal: 823 604 decodes on Orkut | verified (measured `org_runs`) |
| extra ports | none: the sort reads `fpa`, which phase 1 already wrote, and writes `ford`; the ready path reads `fpos` exactly as it does when the organizer is off | verified |
| extra pipeline stage | **none** — the sort runs inside the existing seal step, not as a new stage | verified |
| change to release ordering | the release order becomes ascending `(row, bank, line)` instead of ascending VA line; the *population* and its timing budget are unchanged | verified |
| state added to `bulk_win_bytes` | **0 B** — the charge formula has no `ford` term | verified |
| modelled time cost | `org_backlog += n` (`bulk.cpp:1631`), drained at `org_lanes = 4` per cycle (`2964-2968`); `bulk_org_runs` counts the runs sorted | verified |

The organizer's work is charged to the organizer's own port as a backlog, not as latency on the
critical path. Measured consequence: enabling it on Orkut moved the run from 6 131 444 to 6 130 567
cycles (**−877 cycles, −0.014 %**) while adding 823 604 organizer runs. The charge is real but not
binding in this cell, which is why the difference is noise rather than hours.

---

## 6. Resource fairness

### 6.1 Equivalent (verified)

| dimension | finding |
|---|---|
| residency budget | one `SlotPool` of `R − latch_charge = 128` slots in the hybrid (`bulk.cpp:3356-3360`); M0-RM's own free list is also 128. Both logs report `slots_total = 128`, `inflight_max = 128`. |
| arbitration | plain first-come-first-served: `pool.take(1)` for the engine (`bulk.cpp:412`), `pool.take(0)` for SIMT (`bulk.cpp:3479`), `pool.give()` on completion (`3539`). No priority, no quota, no reservation. `dispatch_order` only breaks ties within a cycle. |
| caches | both enter L1 (`mem_path = l1`, `path_mode_ = 1` → `path_bypasses_l1()` false for every requester). Same L1, same L2, same MSHR files. |
| translation | same dTLB, same L2 TLB, same 8 walkers, same walk latency. Since M27-P3, an engine translation probe holds a residency slot for its lifetime, so the engine pays for the translation concurrency it uses. |
| DRAM | same controller, same FR-FCFS, same timing, same unbounded queue. |
| per-request record | the same `Req` type and the same `inflight_` vector in both arms. |

### 6.2 Resources M27 can use that M0-RM cannot

- **Internal staging**: 2 windows × 16 384 elements + 4 096 translated runs + 128 index lines. M0-RM
  has no equivalent: its work-in-progress is bounded by the 128 slots plus a coalescer latch.
  This is the architecture, not an accounting asymmetry — but it means M27 can hold ~20 000 pieces of
  work before forming a request where M0 holds ~128, and that is the reason the M27 numbers look the
  way they do. It must be declared whenever the two are compared.
- **A second requester identity**: engine requests are attributed as client 1 in the diagnostics and
  enter core 0's L1 as an ordinary client. M0-RM has only client 0. No capacity advantage, only
  attribution.

### 6.3 Resources M0-RM has that M27 does not

- **The whole budget without a reserve**: M27 must leave `idx_reserve = 8` slots free for the index
  stream while tiled acquisition is active (`tgt_reserve()`, `bulk.cpp:1972`), and it refuses target
  admissions when `free <= 8` (measured: 5 893 348 such refusals on Orkut). M0-RM has no such
  constraint. This is a **disadvantage to M27**, and it is deliberate: without it the index stream
  stalls and the run loses ~4.8 % (`sim.hpp:436-441`).
- **Nothing else.** M0-RM has no cache, TLB, MSHR, walker or DRAM capacity that M27 lacks.

### 6.4 Translation concurrency and outstanding accounting

Identical configuration, and the measured maxima agree: `tlb0_mshr_max = 8`, `l2tlb_max_occ = 8`,
`walker_max = 8`, `inflight_max = 128` in both arms. The one behavioural difference is *who* occupies
the MSHRs: M0-RM holds up to 2 parkers per dTLB MSHR on a miss and occupies a slot while waiting; M27's
engine probes are translation-only requests that retire the cycle their translation lands, so they
occupy a slot for a shorter time but pay for it (M27-P3).

### 6.5 Is the comparison fair?

For the *shared* resources, yes — verified by value, by code path and by the measured maxima. The two
asymmetries are both declared and both work **against** M27 in the accounting sense (a reserve it must
respect, and a slot it must spend on every translation), while its genuine advantage is the internal
staging that the architecture exists to provide. No hidden capacity advantage was found in the shared
structures.

---

## 7. Consolidated resource table

Bytes are for the production geometry. "charged" = inside a modelled SRAM quantity the model publishes;
"alloc" = allocated by the implementation but not charged; "est" = derived with a maximum-occupancy
assumption.

| Resource | A: M0-RM | B: M27 sorter off | C: M27 sorter on | Classification |
|---|---|---|---|---|
| L1 (16 KiB, 4-way) + 128 MSHRs | 21 504 B | 21 504 B | 21 504 B | Shared |
| L2 (1 MiB, 8-way) + 64 MSHRs | 1 181 184 B | 1 181 184 B | 1 181 184 B | Shared |
| dTLB (16, 4-way) + 8 MSHRs | 704 B | 704 B | 704 B | Shared |
| L2 TLB (512, 4-way) + 8 MSHRs | 16 640 B | 16 640 B | 16 640 B | Shared |
| walkers (8) + walk cache (16) | 192 B | 192 B | 192 B | Shared |
| Residency: 128 slots + 128 `Req` | 128 | 128 | 128 | Shared |
| DRAM: 2 banks, 4 096 rows, 8 MiB, unbounded queue | present | present | present | Shared |
| Fused window, 2 × 16 384 × 8 B | – | 262 144 B | 262 144 B | Bulk-specific |
| Page directory, 2 × 512 × 16 B | – | 16 384 B | 16 384 B | Bulk-specific |
| Cohort descriptors, 2 × 8 × 32 B | – | 512 B | 512 B | Bulk-specific |
| Ready pool, 4 096 × 16 B | – | 65 536 B | 65 536 B | Bulk-specific |
| Index tiles, 2 × 64 × 21 B + 32 B | – | 2 720 B | 2 720 B | Bulk-specific |
| PA per run (`fpa`), 2 × 8 B × n | – | 262 144 B (alloc) | 262 144 B (alloc) | Bulk-specific, uncharged |
| Rank per run (`fpos`), 2 × 4 B × n | – | 131 072 B (alloc) | 131 072 B (alloc) | Bulk-specific, uncharged |
| Ready heap, 2 × 8 B × 4 096 | – | 65 536 B (alloc) | 65 536 B (alloc) | Bulk-specific, uncharged |
| Release order (`ford`), 2 × 4 B × n | – | **0** | **131 072 B (alloc)** | Optional Bulk-specific |
| Sort working set (3 × 4 B × n, transient) | – | 0 | 196 608 B (est) | Optional Bulk-specific |
| Diagnostic state (`dram_diag`, `cache_diag`) | ~65 536 B extra | ~65 536 B extra | ~65 536 B extra | Simulator-only |
| **Engine total, as charged by the model** | **0** | **347 296 B (339.2 KiB)** | **347 296 B (339.2 KiB)** | |
| **Engine total, charged + allocated** | **0** | **806 048 B (787.2 KiB)** (est) | **937 120 B (915.2 KiB)** (est) | |

For scale: the shared hierarchy is 1 220 224 B (≈ 1.16 MiB) in all three configurations, so the engine
adds **28.5 %** of the shared cache+TLB storage when charged, and **66.1 %** when every per-run array is
counted. The organizer's 131 072 B of persistent state is **37.7 %** of B's charged engine figure and
**16.3 %** of B's allocated figure.

---

## 8. Required conclusions

**1. Is M0-RM genuinely resource-matched to current M27 for all conventional shared resources?**
**Yes.** Nine keys differ, eight are the Bulk pipeline and the ninth (`m24_dram_org`) is unreachable in
M0-RM. L1, L2, dTLB, L2 TLB, every MSHR count, the 8 walkers, 4 KiB/2-level walks, the 128-slot budget
and the DRAM controller/timing are identical by value and by code path, and the measured maxima
(`slots_total`, `inflight_max`, `tlb0_mshr_max`, `l2tlb_max_occ`, `walker_max`) agree. The single caveat
is that M0-RM's file carries `m24_dram_org = 1`: it is inert, and I would rather say so than edit a
preserved artifact.

**2. What additional hardware resources does M27 require?** 347 296 B of charged engine SRAM
(339 KiB: window 262 KiB, ready pool 64 KiB, directory 16 KiB, tiles 2.7 KiB, cohorts 0.5 KiB) plus
458 752 B of per-run arrays the model allocates but does not charge (≈ 787 KiB total, est), plus the
organizer port (`org_lanes = 4` entries/cycle) and the admission/issue control. It requires **no** new
cache, TLB, MSHR, walker or DRAM capacity.

**3. What is the incremental cost of page organization alone?** Essentially all of it: the window, the
page directory, the cohorts, the ready pool, the index tiles and the per-run arrays exist for page
organization. Page organization is the whole 787 KiB (est); it adds no translation or memory-system
capacity, and it *spends* residency slots on its translation probes instead of getting them free.

**4. What additional cost does custom DRAM sorting introduce?** 131 072 B of persistent state (`ford`)
plus 196 608 B of transient sort working set, a 3-key stable sort over up to 16 384 runs (≈ 229 k
comparisons per seal, ≈ 17 M per Orkut run), one physical-address decode per run per seal, and one
`fpos` indirection in the ready path. No extra ports, no extra pipeline stage, no change to the request
population or its timing budget. It is **not** included in the model's `bulk_win_bytes` charge.

**5. Does the organizer justify its cost based on the existing evidence?** **Not on the production map,
and the evidence is consistent about it.** M27 measured −877 cycles (−0.014 %) on Orkut and −0.156 % on
Criteo 16K when it is enabled; M24-A found it valuable (1.5–3.7 %) only on scrambled/mapped layouts
(M1–M3) and on a 4 KiB-row geometry, precisely the cases the production map avoids. Its persistent state is 38 %
of the engine's charged SRAM (131 072 B against 347 296 B) and its ordering can only matter when the
controller cannot already see the locality. Keeping it as an off-by-default configuration (as M27 does) is the position the evidence
supports; making it always-on is not.

**6. Is M27 a clean page-organization-only architecture relative to M0-RM, apart from dispatch?**
**Yes, with two caveats to state honestly.** The release path performs no physical sort, allocates no
`ford`, and `bulk_org_runs = 0` proves the organizer never runs; what remains is window + cohorts +
ready pool + sealed page order. Caveats: (a) the model's SRAM charge omits `fpa`/`fpos`/`ready`
(≈ 448 KiB, est), so "page organization only" is ≈ 787 KiB, not 339 KiB; (b) `sched_vis`/`sched_group`
(`0` in production) are separate knobs that also read DRAM state when enabled — they are off, and the
production claim depends on them staying off.

**7. Any hidden fairness advantages or disadvantages?** One genuine advantage, one genuine
disadvantage, both now declared. Advantage: M27's internal staging (2 × 16 384 elements + 4 096
translated runs) has no analogue in M0-RM, so M27 can hold far more work in progress before forming a
request — it is the architecture, but it must be disclosed whenever the speedup is quoted.
Disadvantage: M27 must reserve 8 of its 128 slots for the index stream and must spend a slot on every
translation probe, so it can use *less* of the shared budget than M0-RM can at any instant. Nothing in
the caches, TLBs, MSHRs, walkers or DRAM gives M27 capacity that M0-RM lacks.

---

## 9. Uncertainties and limits

| item | status |
|---|---|
| storage figures | **derived** from declared counts and simulator struct field lists. The simulator's ABI is wider than a real design in several places (e.g. 64-bit LRU stamps, `Req` fields that a hardware request would not carry), so the byte figures are an upper bound on a plausible implementation, not a synthesis result. |
| the ≈ 229 k comparisons / 17 M comparisons | **estimated** (n log n), not measured; no synthesis, no sort-network model. |
| 8 MiB DRAM at `dram_q = 0` | the queue is unbounded in the model; its effective depth is whatever the 64 L2 MSHRs admit. There is no fixed DRAM-queue SRAM to size. |
| `Req` / `Pending` record sizes | not itemized; both arms share them, so they cancel in the comparison but they are not negligible in absolute terms (128 records). |
| organizer timing | charged as a 4/cycle backlog on the organizer's own port (`bulk.cpp:2964-2968`), not as critical-path latency; the measured 877-cycle effect confirms it is not binding on Orkut. A cell where the organizer becomes the bottleneck would change that, and none was run here. |
| M27 diagnostic overhead | the production file sets `cache_diag = 1` and `dram_diag = 1`, which allocate ~65 KiB of counters and a per-(bank,row) array. This is instrumentation, not architecture, and it is *identical in A and B*, but it means neither run is a "clean" no-diagnostics build. |
| not measured here | no simulation was run for this audit; all measured numbers are quoted from the preserved M24-D/M27 logs named below. |

---

## 10. Evidence index

| claim | source |
|---|---|
| shared resource values | `configs/m27_bulk.cfg`; run echo `build/wm/m27/wl1/m27bulk_orkut.txt:14` |
| nine-key diff | `diff` of comment-stripped `build/wm/m24d/m24d_simt.cfg` vs `configs/m27_bulk.cfg` |
| allocation sites | `memory.cpp:58-76`, `memory.cpp:71`, `bulk.cpp:3356-3360` |
| window/ready/tile charges | `bulk.cpp:4364-4370`, `bulk.cpp:2343`, `bulk.cpp:4201-4213`, `bulk.cpp:1263-1266` |
| uncharged per-run arrays | `sim.hpp:1712, 1730, 1731, 1800`; `bulk.cpp:2319, 2323, 2351, 2382, 2449, 1938` |
| organizer implementation | `bulk.cpp:1578-1632`; guard `bulk.cpp:363-376`; charging `bulk.cpp:1310, 1631, 2964-2968` |
| fair slot pool | `bulk.cpp:412, 3479, 3539`; `sim.hpp:747-787` |
| L1 path equality | `sim.hpp:1017-1019`; `bulk.cpp:4469` |
| measured organizer effect | `build/wm/m27/wl1/bulk_orkut_{hist,prod}.txt`; `docs/m27-first-workload-results.md` §8 |
| historical M24-A value of the organizer | `docs/m24-a-four-way-organization-evaluation.md`; `docs/m24-e-mechanism-ablation.md` §6 |

**Stopped here.** No experiment launched, no source or configuration modified, no artifact overwritten.
