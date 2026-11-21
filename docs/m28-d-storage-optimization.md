# M28-D — Behavior-preserving Bulk storage optimization (Candidate A)

Implementation and verification report. Candidate A of `docs/m28-c-storage-optimization-study.md`,
and nothing else: one live copy of the release path's per-run arrays, and the identity `fpos` when the
custom DRAM sorter is off. No capacity, width, ordering, admission, translation or DRAM change.

**Headline.** Functional storage **988 832 B → 677 536 B** (−311 296 B, −31.5 %, 965.7 → 661.7 KiB),
exactly the Candidate A target. Behaviour is not merely equivalent but **byte-identical**: the Orkut BFS
Bulk log is the same 814 lines as the preserved M27 anchor, `cycles = 6 131 444`, and the M0-RM log is
the same 394 lines, `cycles = 17 904 629`. The suite is 4 967 checks, 0 failures. Two source files were
edited (`sim/src/sim.hpp`, `sim/src/bulk.cpp`); no test, configuration, workload or historical result
was touched.

Every number below is labelled as in M28-C: **[C]** read from the source at the cited line, **[D]**
derived arithmetic (declared entry count × declared field width), **[M]** measured in a log produced by
this milestone, **[?]** unknown.

---

## 1. Summary of the change

| # | Candidate A item | before | after | saved |
|---|---|---:|---:|---:|
| 1 | one live copy of `fpa` (physical line per run) | 262 144 | 131 072 | 131 072 |
| 2 | `fpos` as the identity when `ford` is empty | 131 072 | 0 | 131 072 |
| 3 | one live copy of `ready` (translated-run heap) | 65 536 | 32 768 | 32 768 |
| 4 | one live copy of `ftr` (translated bit per run) | 32 768 | 16 384 | 16 384 |
| | **total** | **491 520** | **180 224** | **311 296** |

The four arrays now live in `BulkEngine` (`rel_pa_`, `rel_tr_`, `rel_ready_`, `rel_pos_`), not in
`Batch`, because they are only ever touched by the window that holds the single issue cursor `iss_`.
`rel_pos_` is allocated only when `Batch::ford` is non-empty; with `m24_dram_org = 0` it stays empty
and `run_key(li)` returns `li`, which is exactly what the array would have held.

Nothing else changed: window arrays, cohort table, pool capacity, residency budget `R`, field widths,
seal order, admission policy, release order, dispatch, the shared hierarchy and the DRAM controller are
untouched (§8).

---

## 2. Stage 1 — verification of every proposed saving against the source

The claim M28-C makes is a **lifetime** claim: `fpa`, `fpos`, `ftr` and `ready` are written and read
only by the window selected by `iss_`, so the second `Batch`'s copies can never hold or influence
anything. Each row below was re-derived from the current source before any edit.

### 2.1 The only producer of `iss_`

`BulkEngine::win_issue` (`bulk.cpp:2730-2731`, pre-edit numbering), the sole entry point of the whole
issue path, begins with

```cpp
if (iss_ < 0) return;
Batch& b = bat_[static_cast<size_t>(iss_)];
```

so every function it calls receives `bat_[iss_]` and nothing else. `iss_` is written in exactly three
places [C]:

| site | effect |
|---|---|
| `win_tick` step (2) (`bulk.cpp:2966-2983`) | `if (iss_ == i) iss_ = -1; ... b = Batch{};` — retirement clears the window in the same statement |
| `win_tick` step (3) (`bulk.cpp:2986-2996`) | adopts a sealed window: `iss_ = i` (only when `iss_ < 0`) |
| `win_reset` (`bulk.cpp:640`) | `fill_ = iss_ = -1` |

Step (2) is the load-bearing one. A window retires only when
`state == 2 && issued_n >= nlines && pending == 0 && retired >= n_ent`; `issued_n` counts lines actually
submitted, so `issued_n >= nlines` means every run of the window has been released, and a run leaves
`ready` only by being released or erased as stale. The heap is therefore empty whenever `iss_` is
cleared — which the run confirms (`bulk_g6b_ready_occ_sum` before the last window's retirement equals
its `runs_released`) [M], and the window is destroyed two lines later in any case.

### 2.2 The four arrays, one by one

| array | declared | allocation site | read/write sites | conclusion |
|---|---|---|---|---|
| `fpa` (`vector<u64>`) | `sim.hpp:1723` | `win_issue_g6a` (`bulk.cpp:2097`), `win_issue_g6b` (`bulk.cpp:2319`) | write `2118`, `2351`; read `1686`, `1766`, `1801`, `1938`, `2268`, `2449` | every site is inside a function reached only from `win_issue`, i.e. only for `iss_` |
| `ftr` (`vector<u8>`) | `sim.hpp:1726` | `2098`, `2320` | write `2120`, `2352`; read `2117`, `2350`, and the G6C duplicate-submit guard | same |
| `ready` (`vector<u64>` heap) | `sim.hpp:1742` | first `push_back` in `win_issue_g6b` (`2356`) | `ready_push` `2356`; `ready_pop` `2419`; `ready_erase_val` `2436`; scans `1666-1700`, `1761-1771`, `1795`, `2260`, `2293`; capacity rule `2382-2395`, `2560`, `2586`; occupancy `2470-2480`, `2511-2518` | same; and the heap is empty at retirement (§2.1) |
| `fpos` (`vector<u32>`) | `sim.hpp:1743` | `2323-2324` | write `2324`; read `2356` only | `fpos[ford.empty() ? i : ford[i]] = i`; when `ford` is empty this is `fpos[i] = i` |

No other translation unit mentions these members: a search of `sim/src` outside `bulk.cpp` finds them
only in the `sim.hpp` declarations, and the test suite does not name them.

**Nothing can affect behaviour.** There is no path in which a window owns these arrays without holding
`iss_`:

* the *filling* window (`fill_`) never allocates them — its only allocation path is `win_seal_fused`,
  which touches `fent`/`fmeta`/`fslot`/`fflag` [C];
* two windows are never issuing at once: `win_tick` step (3) requires `iss_ < 0` [C];
* `win_issue_g6a` states the same property for the two-phase path — "the engine adopts a window for
  issue only when none is issuing (`iss_ < 0`), so no other window can be translating while this one
  drains" (`bulk.cpp:2085-2091`) [C];
* the two discard paths (`win_reset`, `win_seal` of an empty window) operate on `fill_`, never on an
  issuing window [C].

**Expected savings, recorded before editing** [D]: `fpa` 16 384 × 8 × 2 = 262 144 → 131 072;
`fpos` 16 384 × 4 × 2 = 131 072 → 0 (production); `ready` 4 096 × 8 × 2 = 65 536 → 32 768;
`ftr` 16 384 × 1 × 2 = 32 768 → 16 384. **Total 311 296 B**, which reconciles exactly with Candidate A.

### 2.3 The `fpos` identity, stated precisely

`win_order_fused` (`bulk.cpp:1578-1632`) returns with `ford` cleared whenever `m24_dram_org = 0`, and
also for `bulk_org ∈ {0, 4}` or fewer than two runs [C]. In each of those cases the loop
`fpos[ford.empty() ? i : ford[i]] = i` writes `fpos[i] = i`. `run_at()` already encodes the same
identity for `ford` (`sim.hpp:1879-1881`), so the release key is known without an array. When the
organizer *does* build an order, `fpos` is the inverse permutation and is still materialized — the
sorter's own code, its configuration and its output are unchanged.

---

## 3. Stage 2 — the exact source changes

Two files, no others. `sim/src/memory.cpp` (13:11:12) and `configs/m27_bulk.cfg` (13:16:20) have their
pre-milestone modification times; the M27 anchor log is untouched (13:34:48).

### 3.1 `sim/src/sim.hpp`

**(a) `Batch` loses the four declarations.** `fpa` and `ftr` are removed from the M23-G6A block, which
now records why, and keeps `ftr_gen`/`ftr_done`:

```cpp
    // M28-D: the physical line phase 1 produced (`fpa`) and the translated bit
    // (`ftr`) are per-run arrays owned by the *engine*, not by a window: ...
    std::vector<u64> ftr_gen;
    std::vector<u64> ftr_done;
```

`ready` and `fpos` are removed from the M23-G6B block, which now points at the engine's copies.
`Batch` is otherwise unchanged: `fent`, `fmeta`, `fslot`, `fflag`, `ford`, `coh`, the cohort counters,
the G6A/G6B/G6G control words and `ftr_gen`/`ftr_done` all stay per window.

**(b) One engine-owned set of release-path buffers** (`sim.hpp:1990-2006`), next to `bat_`:

```cpp
  std::vector<u64> rel_pa_;     // physical line per run (was Batch::fpa)
  std::vector<u8> rel_tr_;      // translation landed per run (was Batch::ftr)
  std::vector<u64> rel_ready_;  // translated-but-unissued runs, min-heap of keys
  std::vector<u32> rel_pos_;    // run -> rank in `ford` (was Batch::fpos)
```

with the lifetime argument written down beside them.

**(c) `run_key`** (`sim.hpp:1891-1893`) replaces the `fpos` lookup:

```cpp
  u64 run_key(u32 li) const {
    return rel_pos_.empty() ? static_cast<u64>(li) : static_cast<u64>(rel_pos_[li]);
  }
```

**(d) The heap helpers take the heap, not a window** (`sim.hpp:1897-1935`): `ready_push`,
`ready_pop` and `ready_erase_val` now take `std::vector<u64>& ready`. The sift-up/sift-down bodies are
character-identical to the `std::priority_queue`-equivalent code they replaced; only the container
argument changed, so the release sequence cannot move.

### 3.2 `sim/src/bulk.cpp`

Mechanical, one identifier per array: `b.fpa` → `rel_pa_`, `b.ftr[…]` → `rel_tr_[…]`,
`b.ready` → `rel_ready_`, at every site listed in §2.2 (61 lines of `bulk.cpp` now name one of the
four arrays, comments included).

The two semantic edits are the allocation/adoption blocks and the key:

* `win_issue_g6a` init (`bulk.cpp:2101-2102`): `rel_pa_.assign(n, 0); rel_tr_.assign(n, 0);`
  (`ftr_gen`/`ftr_done` still assigned per window).
* `win_issue_g6b` init (`bulk.cpp:2323-2341`): same two assignments, then

```cpp
    rel_ready_.clear();
    if (b.ford.empty()) {
      rel_pos_.clear();
    } else {
      rel_pos_.assign(n, 0);
      for (u32 i = 0; i < n; ++i) rel_pos_[b.ford[i]] = i;
    }
```

  The `rel_ready_.clear()` is a no-op in the state a fresh window was always in (a window is adopted
  only with `state == 0`, i.e. straight after `Batch{}`); it states the invariant instead of relying on
  it. Nothing else about the init changed — same `n`, same contents, same order.
* `win_issue_g6b` translation completion (`bulk.cpp:2367-2373`):

```cpp
    if (li >= n || rel_tr_[li]) continue;
    rel_pa_[li] = x.pa;
    b.ftr_done[li] = x.cycle;
    rel_tr_[li] = 1;
    b.g6b_tr_done++;
    st_.g6b_tr_done++;
    ready_push(rel_ready_, run_key(li));
```

  `run_key` returns the same integer `fpos[li]` held in production, and the identical `fpos[li]` when
  the organizer built an order.
* `win_reset` (`bulk.cpp:642-643`) clears `rel_ready_`/`rel_pos_` with the windows.

No capacity, constant, branch condition, ordering comparison or counter was altered. `git diff`-style
equivalence was checked by rebuilding and comparing full logs (§5).

---

## 4. Stage 3 — correctness verification

| check | result |
|---|---|
| build (`make -j4`, clang++, `-O2 -Wall -Wextra`) | **exit 0**; the only warnings are the two pre-existing `unused variable VPN_SHIFT/PAGE_LINES` in `memory.cpp`, unrelated and untouched |
| test suite `./bin/dx100_tests` | **exit 0, 4 967 checks, 0 failures, "ALL CHECKS PASSED"** — the same count as the M27 anchor |
| request conservation | `conservation=OK`, 1 199 876 elements / 1 200 003 accesses, exit 0 [M] |
| `bulk_residency_violations` | 0 [M] |
| `bulk_issue_monotonic_violations` | 0 [M] |
| `bulk_addr_mismatch` | 0 [M] |
| `bulk_g6b_ready_occ_sum == bulk_g6b_tr_wait_dram` | 12 551 332 265 both [M] |
| `bulk_g6b_ready_peak ≤ g6b_pa_capacity` | 4 096 ≤ 4 096 [M] |
| `win_occ_max ≤ 16 384`, `win_total_max ≤ 32 768` | 16 384, 32 768 [M] |
| `bulk_batches == bulk_g6b_windows` | 74 = 74 [M] |
| determinism (two independent runs, same binary/config/input) | byte-identical logs (§5) |
| sorter-enabled compatibility | the suite's `dram_org_on` check passes (`ok=1 org_runs=13097`), and `m27_bulk_no_dram_org`/`m27_eng_core`/`m27_*` checks pass. **Limitation:** no M27 anchor exists for the organizer-on path, so it is covered by the suite and by code inspection of `rel_pos_` only, not by a paired workload run (§7) |

No check was weakened, removed or bypassed; no assertion was relaxed.

---

## 5. Stage 4 — exact behaviour comparison

One paired comparison, exactly as the brief specifies: M0-RM vs optimized M27 Bulk, Orkut BFS,
`build/wm/m23_orkut_bfs.bin`, `--all`, same configuration files, same measurement boundary. New logs in
`build/wm/m28d/`; the M27 anchors in `build/wm/m27/wl1/` were not modified.

### 5.1 Cycles

```
optimized M27 Bulk        6 131 444 cycles   (preserved M27 anchor: 6 131 444 — difference 0)
M0-RM                    17 904 629 cycles   (preserved M27 anchor: 17 904 629 — difference 0)
speedup                   2.9201×           (unchanged)
```

### 5.2 Diff against the preserved anchors

```
diff build/wm/m27/wl1/m27bulk_orkut.txt    build/wm/m28d/m28d_bulk_orkut.txt      -> IDENTICAL (814 lines)
diff build/wm/m28d/m28d_bulk_orkut.txt     build/wm/m28d/m28d_bulk_orkut_rerun.txt -> IDENTICAL (determinism)
diff build/wm/m27/wl1/m0rm_orkut.txt       build/wm/m28d/m28d_m0rm_orkut.txt      -> IDENTICAL (394 lines)
```

Every printed field is identical — not "within noise", not "the counters that matter": the logs are
byte-for-byte the same. Consequently there is no difference to investigate in admission, retirement,
translation, ordering, pool behaviour, DRAM scheduling or completion.

### 5.3 The counters the brief names, before = after

| counter | value (identical both sides) |
|---|---|
| `cycles` | 6 131 444 |
| `bulk_batches` / `bulk_g6b_windows` | 74 / 74 |
| `bulk_g6b_ready_peak` | 4 096 |
| `bulk_g6b_ready_occ_sum` | 12 551 332 265 |
| `bulk_g6b_tr_wait_dram` | 12 551 332 265 |
| `bulk_g6b_runs_released` | 823 621 |
| `bulk_win_occ_max` / `bulk_win_total_max` | 16 384 / 32 768 |
| `bulk_win_bytes` / `bulk_win_vec_bytes` | 279 040 / 475 136 |
| `bulk_g6b_pa_bytes` | 65 536 |
| `completion_hash` | 1 284 131 032 |
| `conservation` | OK |

The M27 anchors therefore stand unmodified, and the optimized build reproduces them exactly.

---

## 6. Stage 5 — storage accounting after the change

Recalculated from the modified source: declared entry count × declared C++ width, at the production
geometry (`batch_window = 16 384`, `page_dir = 512`, `page_cohorts = 8`, `g6b_pa_capacity = 4 096`,
`R = 128`, `org_overlap = 1`). Shared hierarchy structures are excluded exactly as in M28-C §2.1.

| # | structure | entries | width | copies | before | after |
|---|---|---:|---:|---:|---:|---:|
| 1 | `fent` fused element word | 16 384 | 8 B | 2 | 262 144 | 262 144 |
| 2 | `fmeta` run start\|count | 16 384 | 4 B | 2 | 131 072 | 131 072 |
| 3 | `fslot` residency slot per run | 16 384 | 1 B | 2 | 32 768 | 32 768 |
| 4 | `fflag` issued bit per run | 16 384 | 1 B | 2 | 32 768 | 32 768 |
| 5 | `dir_vpn` page-directory tags | 512 | 8 B | 2 | 8 192 | 8 192 |
| 6 | `dir_mask` per-page line mask | 512 | 8 B | 2 | 8 192 | 8 192 |
| 7 | `coh` cohort descriptors | 8 | 32 B | 2 | 512 | 512 |
| 8 | `g6c_coh_def` refusals per cohort | 8 | 4 B | 2 | 64 | 64 |
| 9 | `g6c_coh_age` episode start per cohort | 8 | 8 B | 2 | 128 | 128 |
| 10 | `fpa` → engine `rel_pa_` | 16 384 | 8 B | **1** | 262 144 | **131 072** |
| 11 | `fpos` → engine `rel_pos_` | 16 384 | 4 B | **0** (off) | 131 072 | **0** |
| 12 | `ftr` → engine `rel_tr_` | 16 384 | 1 B | **1** | 32 768 | **16 384** |
| 13 | `ready` → engine `rel_ready_` | 4 096 | 8 B | **1** | 65 536 | **32 768** |
| 14 | `ford` release order | 16 384 | 4 B | 2 | 0 (organizer off) | 0 |
| 15 | `fan_` fill/fanout queue | 128 | 76 B | 1 | 9 728 | 9 728 |
| 16 | `info_` per-slot request record | 128 | 56 B | 1 | 7 168 | 7 168 |
| 17 | index tiles (`idx_buf_lines = 64`) | 2 × 64 | 21 B + 32 B | 1 | 2 720 | 2 720 |
| 18 | `free_`/`unissued_`/`idx_q_` ids | 128 | 12 B | 1 | 1 536 | 1 536 |
| 19 | `queue_` descriptor FIFO | 8 | 40 B | 1 | 320 | 320 |
| | **total** | | | | **988 832 B** | **677 536 B** |

```
before   988 832 B = 965.7 KiB
after    677 536 B = 661.7 KiB
saved    311 296 B = 304.0 KiB = 31.5 %
```

Subtotals after the change: **window 475 840 B** (rows 1-9, unchanged), **release path 180 224 B**
(rows 10, 12, 13: 131 072 + 16 384 + 32 768 — was 458 752), **engine control 21 472 B** (rows 15-19).
475 840 + 180 224 + 21 472 = 677 536 [D].

Ratios: engine ÷ shared hierarchy (1 220 224 B) goes **0.81× → 0.56×**, the Candidate A figure.

### 6.1 Remaining uncharged allocations

Unchanged by this milestone, restated so the balance is explicit:

| allocation | bytes | charged by the model? |
|---|---:|---|
| window state beyond the `bulk_win_bytes` term (`fmeta`, `fslot`, `fflag`, cohort counters) | 196 800 | **no** — `bulk_win_bytes = 279 040` covers rows 1, 5, 6, 7 only |
| release path (`rel_pa_` + `rel_tr_` + `rel_ready_`) | 180 224 | **no** — no `bulk_*` term covers it, before or after |
| `bulk_g6b_pa_bytes = 65 536` | — | a *charge* at 16 B per pool entry, not this inventory; unchanged (4 096 × 16) |
| engine control (`fan_`, `info_`, tiles, lists, queue) | 21 472 | tiles only (`bulk_idx_bytes = 2 720`) |

### 6.2 Excluded instrumentation and simulator state

Stated explicitly because M28-C's table did not name them:

| state | bytes | why excluded |
|---|---:|---|
| `ftr_gen`, `ftr_done` (per window) | 2 × 16 384 × 8 B × 2 arrays = 524 288 | read only into `Req::gen`/`Req::tlb_at`, which feed the stale-lifecycle event (`memory.cpp:1139`) and the issued-to-ready latency sums (`memory.cpp:1205-1209`); **no decision reads them**. Left per window, untouched by this milestone (§7) |
| `g6f_*`, `g6g_*`, `g6i_*`, `m24_prev_pos_` scratch | — | empty unless `sched_vis`/`sched_group`/`g6i_diag` is set; diagnostic |
| `val_`/`addr_`, `idx_done_`, `idx_pending_`, `tgt_pending_`, `win_recs`, `err_` | — | simulator bookkeeping (`win_recs` grows one record per window: 74 here) |
| token/queue/ref plumbing | — | unchanged, outside the engine's functional storage |

**No physical SRAM area, cell count or silicon area is claimed.** These are declared functional
storage widths in the model's own terms; a packed-RTL figure is Candidate B's subject and was not
implemented.

### 6.3 One honest caveat about what changed in the binary

Before the change the four arrays were members of `Batch`, but `std::vector` allocates on first use,
and the only `.assign` calls are inside the issue path. The simulator therefore already held **one
heap allocation** per array at run time; the second copy existed as an empty vector object, not as
storage. What this milestone changes is the **declared functional structure** — the thing M28-C's
inventory prices, and the thing a hardware design would instantiate — from two buffers per array to
one. Behaviourally it is a no-op, which is exactly why the logs are identical. Stated plainly so the
saving is not mistaken for a change in the simulation's peak heap use.

---

## 7. Differences, omissions and unresolved issues

1. **M28-C's inventory omitted `ftr_gen`/`ftr_done`** (2 × 8 B × 16 384 × 2 windows = 524 288 B). They
   are the only two `Batch` arrays with the same single-window lifetime as the four Candidate A names.
   They were **left untouched**, for two reasons: they are latency instrumentation rather than
   functional state (§6.2), and Candidate A's accounting — and therefore the 677 536 B target — does
   not include them. They are now recorded as an excluded row so the next study does not inherit the
   omission. A future authorized step could consolidate them for a further 262 144 B of declared
   storage with the same proof, but that is **not** this milestone.
2. **The organizer-on path has no M27 workload anchor.** `rel_pos_` is exercised by the suite
   (`dram_org_on`, `org_runs = 13 097`) and by inspection, but not by a paired Orkut run, because no
   post-M27 organizer-on anchor exists to compare against. Reported as a limitation, not a pass.
3. **No test exercises `g6b_pa_capacity` or the release-path storage bounds** (carried over from
   M28-C §8). Nothing in this change alters pool capacity, so this is unchanged risk rather than new.
4. **`bulk_win_vec_bytes` and `bulk_win_bytes` are unchanged** (475 136 / 279 040), because the charge
   never covered the release path. The storage a reader can verify from the log is therefore unchanged;
   the 311 296 B is a *declared-structure* figure verified against source, as in M28-C.
5. Nothing was found that "could affect behaviour" in the sense the brief's §1 warns about, so no
   stopping condition was triggered.

---

## 8. Confirmation of scope

Not changed, verified by inspection of the edited regions and by the byte-identical logs:

* dispatch/compiler selection, `op_class`, `dispatch_order` — untouched;
* page-cohort admission (`win_admit_g6c`, `g6c_open_cohort`), cohort count, cohort table;
* ready-pool capacity `g6b_pa_capacity = 4 096` and its backpressure rule;
* window capacity 16 384 and the two-buffer structure (`org_overlap = 1`, `win_policy` E3);
* request ordering and release timing: `run_at`, `run_key`, the heap comparisons and the release loop
  are the same integers and the same sequence;
* translation: probes, dTLB/L2-TLB behaviour, `bulk_org` paths G6A/G6B/G6C;
* L1/L2, TLB, walkers, MSHRs, `R = 128`, `MemorySystem::inflight_`;
* DRAM controller, FR-FCFS, address map, timing constants, `m24_dram_org = 0` (the sorter stays
  disabled by default and its implementation is untouched);
* request conservation, liveness/deadlock checks, residency and monotonicity invariants — no check
  weakened or removed;
* field widths (nothing narrowed), buffer sharing beyond the four verified copies, no new optimization.

Historical artifacts preserved: `build/wm/m24d/*`, `build/wm/m27/*`, `docs/m24*`–`docs/m28-c*`, and
`configs/m27_bulk.cfg` (13:16:20). The new logs are additive, in `build/wm/m28d/`.

---

## 9. Reproduction

```bash
cd DX100_MEM/sim
make -j4                        # build
./bin/dx100_tests               # 4 967 checks, 0 failures

./bin/dx100sim --config ../configs/m27_bulk.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin --all \
  > ../build/wm/m28d/m28d_bulk_orkut.txt          # 6 131 444 cycles

./bin/dx100sim --config ../build/wm/m24d/m24d_simt.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin --all \
  > ../build/wm/m28d/m28d_m0rm_orkut.txt          # 17 904 629 cycles

diff ../build/wm/m27/wl1/m27bulk_orkut.txt ../build/wm/m28d/m28d_bulk_orkut.txt   # identical
diff ../build/wm/m27/wl1/m0rm_orkut.txt    ../build/wm/m28d/m28d_m0rm_orkut.txt   # identical
```

---

## 10. Where this stops

Candidate A only. Candidate B (packed widths) and Candidate C (single window, 512-entry pool carrying
the PA) are **not** implemented and were not started. No further optimization was performed, and none
should be inferred from this report: Candidate A's result is the 311 296 B that cost nothing, and it
says nothing about what B or C would cost in cycles.
