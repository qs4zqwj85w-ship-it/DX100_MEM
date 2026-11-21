# M28-F — Behaviour-preserving timestamp storage consolidation

**Scope.** Implements the two consolidations approved by the M28-E source audit (`docs/m28-e-ftr-storage-audit.md`):
the per-run translation timestamps `ftr_gen` / `ftr_done` move from per-`Batch` ownership to one
`BulkEngine`-owned copy each, following the pattern M28-D established for `rel_pa_` / `rel_tr_` /
`rel_ready_` / `rel_pos_`.

**Result.** 262 144 B of declared storage removed (256.0 KiB, 21.8 % of the allocated Bulk total), with
**byte-identical output**: the Orkut BFS Bulk log matches the preserved M27 anchor on all 814 lines, the
M0-RM log matches on all 394 lines, and the sorter-enabled (`m24_dram_org = 1`) path matches a
pre-change replica build exactly.

```
allocated Bulk storage incl. timestamps   1 201 824 B  ->  939 680 B   (-262 144 B)
functional Bulk storage (M28-D)             677 536 B  ->  677 536 B   (unchanged, by design)
Orkut Bulk cycles                             6 131 444 ->  6 131 444   (difference 0)
Orkut M0-RM cycles                           17 904 629 -> 17 904 629   (difference 0)
speedup                                          2.9201x -> 2.9201x
```

---

## 1. Stage 1 — verification of the M28-E findings

Before editing, the audited source was re-read and re-grepped. Every line reference in the audit is still
exact at the current HEAD of the tree, and the audited version is the current version: `sim/src/sim.hpp`
and `sim/src/bulk.cpp` had not been modified since M27/M28-D (only the M28-E document was newer).

A single `grep` for the two names found **15 references in 13 code lines** — the same set the audit
tabulated, with no additions and no deletions:

| kind | count | sites (pre-change) |
|---|---:|---|
| declarations in `Batch` | 2 | `sim.hpp:1726-1727` |
| allocations (`assign(n, 0)`) | 4 | `bulk.cpp:2103-2104` (G6A), `2325-2326` (G6B/G6C) |
| writes | 5 | `2123`, `2152`, `2369`, `2420`, `2643` |
| reads | 4 | `1943`, `1944`, `1969`, `2596` |

The three properties the audit's ownership argument rests on were re-confirmed in the current source:

1. Every access site lies inside `win_issue_g6a` / `win_issue_g6b` / `win_admit_g6c` /
   `win_emit_fused`, all reached only through `win_issue` (`bulk.cpp:2746-2750`), which selects
   `bat_[iss_]` — one issuing window at a time.
2. Both `assign(n, 0)` pairs sit in per-adoption init blocks (`b.g6a_phase == 0`, `!b.g6b_init`) that run
   before any submission or drain in the same call.
3. Retirement (`bulk.cpp:2994-2998`, `state == 2 && issued_n >= nlines && pending == 0`) requires every
   run released, and a run reaches `win_emit_fused` only out of the ready buffer, i.e. only after its
   translation landed — so no stamp is still needed when `iss_` clears, and `b = Batch{}` destroys nothing
   that is still live.

`sim/tests/tests.cpp` names neither array, so no test asserts on them directly.

**No discrepancy.** The change proceeded.

### 1.1 One naming deviation from M28-E §5.1

The audit's step 2 proposed the names `tr_gen_` / `tr_done_`. They are implemented as **`rel_gen_` /
`rel_done_`** instead, for two reasons: they join the `rel_*` block that already holds the other five
release-path arrays, and `Batch` already has a `tr_done` member that means something else (a count of
landed translations, `sim.hpp:1728`). A one-underscore difference between "translations completed" and
"per-run completion stamp" is a reading hazard with no benefit. Names only — type, capacity, indexing,
semantics and every access site are unchanged.

---

## 2. Stage 2 — the change

Two hunks in `sim/src/sim.hpp` and eleven statement retargets in `sim/src/bulk.cpp`. No other file was
touched (`memory.cpp`, `system.cpp`, the configs and the tests are untouched — verified by mtime).

### 2.1 `sim/src/sim.hpp`

The two `Batch` members are gone (`1726-1727` before the change) and the M23-G6A comment now records
where they went; the members are declared once in the engine's existing `rel_*` block:

```cpp
  // ---- M28-D: the release path's per-run arrays, owned once ----------------
  ...
  std::vector<u64> rel_pa_;     // physical line per run (was Batch::fpa)
  std::vector<u8> rel_tr_;      // translation landed per run (was Batch::ftr)
  std::vector<u64> rel_ready_;  // translated-but-unissued runs, min-heap of keys
  std::vector<u32> rel_pos_;    // run -> rank in `ford` (was Batch::fpos)
  // M28-F: the two per-run translation timestamps, on the same ownership
  // argument as the four above. ...
  std::vector<u64> rel_gen_;    // probe submission cycle per run (was Batch::ftr_gen)
  std::vector<u64> rel_done_;   // probe completion cycle per run (was Batch::ftr_done)
```
(`sim.hpp:2014-2015`; the `Batch` comment that points here is at `sim.hpp:1718-1726`.)

### 2.2 `sim/src/bulk.cpp` — the eleven retargeted statements

Line numbers are unchanged by the edit (every replacement is one line for one line), so these are the
final positions:

| # | site | before | after |
|---|---|---|---|
| 1 | `2103` | `b.ftr_gen.assign(n, 0);` | `rel_gen_.assign(n, 0);` |
| 2 | `2104` | `b.ftr_done.assign(n, 0);` | `rel_done_.assign(n, 0);` |
| 3 | `2325` | `b.ftr_gen.assign(n, 0);` | `rel_gen_.assign(n, 0);` |
| 4 | `2326` | `b.ftr_done.assign(n, 0);` | `rel_done_.assign(n, 0);` |
| 5 | `2123` | `b.ftr_done[li] = x.cycle;` | `rel_done_[li] = x.cycle;` |
| 6 | `2152` | `b.ftr_gen[li] = cycle;` | `rel_gen_[li] = cycle;` |
| 7 | `2369` | `b.ftr_done[li] = x.cycle;` | `rel_done_[li] = x.cycle;` |
| 8 | `2420` | `b.ftr_gen[li] = cycle;` | `rel_gen_[li] = cycle;` |
| 9 | `2643` | `b.ftr_gen[li] = cycle;` | `rel_gen_[li] = cycle;` |
| 10 | `1943-1944` | `r.gen = b.ftr_gen[li]; r.tlb_at = b.ftr_done[li];` | `r.gen = rel_gen_[li]; r.tlb_at = rel_done_[li];` |
| 11 | `1969` | `const u64 w = cycle - b.ftr_done[li];` | `const u64 w = cycle - rel_done_[li];` |
| 12 | `2596` | `if (b.ftr_gen[li] != 0 \|\| rel_tr_[li])` | `if (rel_gen_[li] != 0 \|\| rel_tr_[li])` |

(Counted as eleven statements: 4 assigns + 5 writes + 2 single-line reads, with `1943-1944` being the
fourth read pair.)

What was deliberately **not** done:

* `assign(n, 0)` stayed at the **same two init sites**, in the same order relative to the surrounding
  statements, so the state a window is adopted with is bit-for-bit what it was.
* No width change (`u64` retained), no capacity change (`n = b.fmeta.size()`, bounded by `batch_window`),
  no indexing change (`li`, the run id — never a rank), no reordering of any statement.
* No signature, policy, configuration or test change. `Batch` is now free of both vectors; the second
  buffer's copy — which the source never touched, because only the issuing window allocates — no longer
  exists.

---

## 3. Stage 3 — correctness verification

```
make -j4                        -> exit 0
   (only pre-existing warnings: memory.cpp:18-19 unused consts VPN_SHIFT / PAGE_LINES)
./bin/dx100_tests               -> exit 0, "4967 checks, 0 failures", "ALL CHECKS PASSED"
```

The suite was not modified in any way; all checks that existed at M28-D still run and pass. The checks the
brief names map onto it as follows:

| brief requirement | covered by |
|---|---|
| complete existing regression suite | all 4 967 checks, 0 failures |
| request-conservation | the suite's conservation checks; the Orkut log also reports `conservation=OK` |
| determinism | `m28f_bulk_orkut.txt` vs `m28f_bulk_orkut_rerun.txt` — identical (§4.2); the paired A/B in §5 |
| liveness | `m26_liveness_map3_s3`, `m26_liveness_cohorts1`, `m27_liveness_stall`, plus every liveness assertion inside the m23/m24/m27 groups |
| timestamp / latency statistics | §4.3 — `lat_sum`, `lat_tlb_sum`, `lat_cache_sum`, `lat_mean`, `lat_tlb_mean` all unchanged |
| sorter-enabled compatibility | §5 (suite: `m24_four_way_organization`, `m27_bulk_no_dram_org` "dram_org_on: ok=1 org_runs=13097"; plus the simulator A/B) |

No test was weakened, skipped or removed, and no invariant or bound was relaxed.

---

## 4. Stage 4 — exact behaviour comparison

One paired Orkut BFS cell, exactly as M28-D: `build/wm/m23_orkut_bfs.bin`, `--all`, same configuration
files, same seed/mapping/measurement boundary. New logs in `build/wm/m28f/`; the M27 anchors in
`build/wm/m27/wl1/` and the M28-D logs in `build/wm/m28d/` were not modified (timestamps unchanged).

### 4.1 Cycles

```
optimized M27 Bulk        6 131 444 cycles   (M28-D anchor 6 131 444, M27 anchor 6 131 444 — difference 0)
M0-RM                    17 904 629 cycles   (M28-D anchor 17 904 629, M27 anchor 17 904 629 — difference 0)
speedup                   2.9201x           (unchanged)
```

### 4.2 Complete-log diff, not a summary comparison

```
diff build/wm/m27/wl1/m27bulk_orkut.txt      build/wm/m28f/m28f_bulk_orkut.txt        -> IDENTICAL (814 lines)
diff build/wm/m28f/m28f_bulk_orkut.txt       build/wm/m28f/m28f_bulk_orkut_rerun.txt   -> IDENTICAL (determinism)
diff build/wm/m27/wl1/m0rm_orkut.txt         build/wm/m28f/m28f_m0rm_orkut.txt         -> IDENTICAL (394 lines)
```

Both 18 682 B Bulk logs and the 8 545 B M0-RM log are byte-for-byte identical to the anchors. Every
printed field therefore agrees: request counts, admission and retirement counts, ready-pool occupancy,
DRAM statistics, TLB/page-walker counters, completion hash, and the `bulk_*_bytes` export block.

### 4.3 The counters the brief names, before = after

| counter | value (identical on both sides) |
|---|---|
| `cycles` | 6 131 444 |
| `bulk_batches` / `bulk_g6b_windows` | 74 / 74 |
| `bulk_g6b_ready_peak` | 4 096 |
| `bulk_g6b_runs_released` | 823 621 |
| `bulk_g6c_dup_submit_blocked` | 0 (the `rel_gen_[li] != 0` guard still never fires) |
| `bulk_g6c_refused` | 5 725 464 |
| `bulk_win_occ_max` / `bulk_win_total_max` | 16 384 / 32 768 |
| `bulk_win_bytes` / `bulk_win_vec_bytes` | 279 040 / 475 136 |
| `bulk_g6b_pa_bytes` | 65 536 |
| `completion_hash` | 1 284 131 032 |
| `conservation` | OK |

### 4.4 Latency statistics

These are the fields the two arrays feed, so they are the sensitive ones; all are unchanged because the
logs are identical:

| field | Bulk, before = after | note |
|---|---:|---|
| `lat_tlb_sum` | 58 715 056 | the translation half of the split — the field the audit predicted would change if the arrays were deleted rather than consolidated |
| `lat_tlb_mean` | 65 | |
| `lat_sum` | 13 263 390 135 | `Req::gen` start-of-life stamps |
| `lat_cache_sum` | 13 204 675 079 | |
| `lat_mean_x100` | 1 475 772 | |
| `bulk_g6b_tr_wait_dram` | 12 551 332 265 | `cycle - rel_done_[li]` at release |
| `bulk_g6b_tr_wait_max` | 39 034 | |

`lat_tlb_sum` matching to the unit — not approximately — is the direct evidence that the consolidated
copy hands `win_emit_fused` exactly the timestamps the per-window copy did: `r.gen` / `r.tlb_at` are set
from the same writes, in the same order, with the same `assign(n, 0)` initial state. M0-RM's own
`lat_tlb_sum` (128 229 448) is unchanged too, as expected — that policy never allocates either array.

---

## 5. Sorter-enabled compatibility

The organizer (`m24_dram_org = 1`) changes release order through `Batch::ford` / `rel_pos_` / `run_key`,
but the timestamps are indexed by **run id**, never by rank, so the audit predicted no interaction. That
was verified exactly rather than argued, because no preserved `org = 1` Bulk log exists to diff against:

**Method.** A scratch copy of `sim/src`, `sim/tests` and the `Makefile` was built with the change
*inverted* — `rel_gen_[li]` → `b.ftr_gen[li]`, `rel_done_[li]` → `b.ftr_done[li]`, both `assign` pairs
reverted, and the two members re-declared in `Batch` — i.e. the pre-change source reconstructed
mechanically. The replica was then checked for fidelity before being trusted:

```
replica, org = 0  vs  build/wm/m27/wl1/m27bulk_orkut.txt   -> IDENTICAL   (replica is faithful)
replica, org = 1  vs  optimized, org = 1                   -> IDENTICAL   (814 lines)
replica, org = 1  cycles = 6 130 567   optimized, org = 1 cycles = 6 130 567
```

Both `org = 1` runs at 6 130 567 cycles — a different number from the `org = 0` anchor, which is the
organizer doing its job, and the same number on both sides of the change. The scratch tree was removed
after the run; its two logs are kept as `build/wm/m28f/prev_bulk_org0.txt` and `prev_bulk_org1.txt`, and
the current build's `org = 1` log as `m28f_bulk_org1.txt`.

**Sorter-enabled compatibility: confirmed identical**, at simulator level and in the unit suite.

---

## 6. Stage 5 — storage accounting

Recalculated from the modified source: declared entry count x declared C++ width at the production
geometry (`batch_window = 16 384`, `page_dir = 512`, `page_cohorts = 8`, `g6b_pa_capacity = 4 096`,
`R = 128`, `org_overlap = 1`). Rows 1-19 are M28-D's inventory, re-checked against the source rather than
copied; rows 20-21 are new.

### 6.1 Allocated functional storage

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
| 10 | `fpa` → `rel_pa_` | 16 384 | 8 B | 1 | 262 144 | 131 072 |
| 11 | `fpos` → `rel_pos_` | 16 384 | 4 B | 0 (organizer off) | 131 072 | 0 |
| 12 | `ftr` → `rel_tr_` | 16 384 | 1 B | 1 | 32 768 | 16 384 |
| 13 | `ready` → `rel_ready_` | 4 096 | 8 B | 1 | 65 536 | 32 768 |
| 14 | `ford` release order | 16 384 | 4 B | 2 | 0 | 0 |
| 15 | `fan_` fill/fanout queue | 128 | 76 B | 1 | 9 728 | 9 728 |
| 16 | `info_` per-slot request record | 128 | 56 B | 1 | 7 168 | 7 168 |
| 17 | index tiles (`idx_buf_lines = 64`) | 2 x 64 | 21 B + 32 B | 1 | 2 720 | 2 720 |
| 18 | `free_`/`unissued_`/`idx_q_` ids | 128 | 12 B | 1 | 1 536 | 1 536 |
| 19 | `queue_` descriptor FIFO | 8 | 40 B | 1 | 320 | 320 |
| | **functional total** | | | | **988 832** | **677 536** |

### 6.2 The consolidated timestamps (formerly per-window)

| # | structure | entries | width | copies | before | after | saved |
|---|---|---:|---:|---:|---:|---:|---:|
| 20 | `ftr_gen` → `rel_gen_` | 16 384 | 8 B | 2 → **1** | 262 144 | **131 072** | 131 072 |
| 21 | `ftr_done` → `rel_done_` | 16 384 | 8 B | 2 → **1** | 262 144 | **131 072** | 131 072 |
| | **subtotal** | | | | **524 288** | **262 144** | **262 144 B (256.0 KiB)** |

`2 x 16 384 x 8 = 262 144` per structure. The factor 2 was the double-buffer convention (`org_overlap`,
the same convention `bulk_win_bytes = 2 x (16 384x8 + 512x16 + 256)` encodes); only the issuing window
ever allocated a copy, and there is one issuing window at a time.

### 6.3 Complete Bulk-specific total

| figure | before M28-F | after M28-F |
|---|---:|---:|
| window state (rows 1-9) | 475 840 | **475 840** |
| release path (rows 10, 12, 13, 20, 21) | 180 224 + 524 288 = 704 512 | **442 368** |
| engine control (rows 15-19) | 21 472 | **21 472** |
| **allocated total, timestamps included** | **1 201 824** | **939 680** |

```
475 840 + 442 368 + 21 472 = 939 680   [source-derived]
677 536 + 262 144          = 939 680   [reconciles with 6.1 + 6.2]
1 201 824 - 262 144        = 939 680
262 144 / 1 201 824        = 21.8 %   (ratio 0.7819x)
```

The source-derived figure is **939 680 B**, exactly the expected value.

### 6.4 Reconciliation with M28-C / M28-D / M28-E

```
M27 baseline, timestamps included   1 513 120 B
  less M28-D consolidation           -311 296 B   (fpa/fpos/ftr/ready -> engine, 1 copy)
  = M28-D position                  1 201 824 B
  less M28-F consolidation           -262 144 B   (ftr_gen/ftr_done -> engine, 1 copy)
  = M28-F position                    939 680 B
total removed across both stages      573 440 B   (37.9 % of the M27 baseline, 1 477.7 KiB -> 917.7 KiB)
```

The functional total is **deliberately not reduced** by M28-F. M28-C and M28-D price only state the design
needs to function, and the timestamps were classified there as timing instrumentation — a classification
this milestone does not revisit (the audit's §2.5/§4.4 finding: they feed no decision except the zero-test
guard). M28-F changes the *cost* of that instrumentation, 2 copies → 1. The functional figure stays
677 536 B, and the ratio to the shared hierarchy (1 220 224 B) therefore stays 0.56×; the
timestamps-included ratio improves 0.98× → 0.77×.

### 6.5 What is excluded, and why (no double counting)

| state | bytes | status |
|---|---:|---|
| window state outside the `bulk_win_bytes` term (`fmeta`, `fslot`, `fflag`, cohort counters) | 196 800 | uncharged before and after; `bulk_win_bytes = 279 040` covers rows 1, 5, 6, 7 only (262 144 + 8 192 + 8 192 + 512) |
| `bulk_g6b_pa_bytes = 65 536` | — | a *charge* of 16 B per pool entry (4 096 x 16), not the heap's storage; `rel_ready_` (row 13, 8 B/entry) is the array it prices. Not added again |
| `bulk_win_vec_bytes = 475 136` | — | counts fused window vectors only, so it was and is unaffected (it stayed 475 136 across M28-D too) |
| `win_rec` timeline | 432 | simulator bookkeeping, one record per window (74 here) |
| `row_acts_` / `row_stamp_` | 65 536 | pure diagnostics; **allocated** because the production config sets `dram_diag = 1`. Diagnostic, uncharged by every `bulk_*` term, and unchanged by M28-F |
| `g6f_*` / `g6g_*` / `g6i_*` scratch, `m24_prev_pos_` | 0 | off in production (`sched_vis = 0`, `sched_group = 0`, `g6i_diag = 0`) |
| `reuse_` (M13) | 0 | `reuse_dir = 0` |
| `g6a_pa_capacity` buffer | 0 | `bulk_org = 8`; `bulk_g6a_pa_bytes = 0` |
| shared hierarchy (L1/L2, dTLB, L2 TLB, MSHRs, DRAM arrays) | 1 220 224 | shared conventional GPU structures, excluded exactly as in M28-C §2.1 / M28-D |

### 6.6 Caveat carried forward

`std::vector` allocates on first use, so the process already held one heap copy per array before the
change: the second `Batch`'s copy was an empty vector object, never an allocation, because a batch is
never issued while another issues. The 262 144 B is therefore a saving in **declared functional
structure** — what M28-C/D/E price, and what a hardware design would instantiate — not in peak heap, and
**not** a physical SRAM/area/power claim. No synthesis was run.

---

## 7. Unresolved discrepancies

**None.** Specifically:

* No behavioural difference of any size: the 814-line Bulk log, the 394-line M0-RM log and the 814-line
  sorter-enabled log are all byte-identical to their comparison points.
* No line-reference drift: every site the audit named still exists at the line it named, and the
  post-change positions are listed in §2.2.
* No anchor was modified or regenerated. `build/wm/m27/`, `build/wm/m24d/` and `build/wm/m28d/` are
  intact; all M28-F output is new, in `build/wm/m28f/`.
* Two pre-existing `memory.cpp` warnings (unused `VPN_SHIFT`, `PAGE_LINES`) remain; they are unrelated to
  this change and were present before it.
* One deviation from the audit's letter, documented in §1.1: the two members are named `rel_gen_` /
  `rel_done_` rather than `tr_gen_` / `tr_done_`. Names only.

---

## 8. Acceptance

| criterion | result |
|---|---|
| both structures consolidated to one engine-owned copy | ✅ `rel_gen_` (`sim.hpp:2014`), `rel_done_` (`sim.hpp:2015`) |
| types, capacities, indexing, init points, timestamp semantics preserved | ✅ `u64`, `n = fmeta.size()`, run id `li`, same two `assign(n, 0)` sites |
| source-verified storage saving | ✅ 131 072 B each, **262 144 B** total |
| complete Bulk-specific total | ✅ **939 680 B** (from 1 201 824 B), matches the expected figure |
| compilation | ✅ exit 0, no new warnings |
| complete regression suite | ✅ 4 967 checks, 0 failures |
| identical cycle counts | ✅ 6 131 444 / 17 904 629, speedup 2.9201x |
| identical request, admission, retirement, ready-pool, DRAM, latency statistics | ✅ all 814 log lines identical |
| sorter-enabled compatibility | ✅ organizer-on A/B identical at 6 130 567 cycles |
| historical files, logs and anchors preserved | ✅ untouched |

**M28-F is complete: 262 144 B of verified storage savings with exact behavioural equivalence.** No
Candidate B/C work, no capacity reduction, no field-width change, no DRAM scheduling change and no new
architectural mechanism was introduced. Work stops here, pending authorization.
