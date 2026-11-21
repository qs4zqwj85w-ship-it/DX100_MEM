# M28-E — `ftr_gen` / `ftr_done` storage lifetime audit

**Scope.** Read-only. No source, configuration, build, test or simulation change accompanies this
document. It answers one question: can the two per-run timestamp arrays that M28-D left in `Batch` be
consolidated into a single active copy without changing any observable behaviour?

**Answer.** Yes — **both** (decision 1, §7). Each is written and read only inside the window that holds
the single issue cursor `iss_`, in the same functions and the same basic blocks as `fpa`/`ftr`/`ready`,
which M28-D already consolidated. Consolidation saves **262 144 B** of declared structure and touches
neither a decision nor an exported counter.

**What this audit does not claim.** No physical SRAM, area, timing or power figure. The numbers below are
the simulator's declared structure ("what the design owns"), the same currency as M28-C and M28-D, not a
synthesis result.

---

## 1. What the two arrays are

Both are produced by the translation-first organizations (`bulk_org ∈ {6, 7, 8}`) and hold one 64-bit
cycle stamp per **run** of the sealed window:

| array | type | meaning | written | read |
|---|---|---|---|---|
| `ftr_gen` | `std::vector<u64>` | cycle the run's translation probe was **submitted** | at probe submission | at release (`Req::gen`), and as an "already submitted" test |
| `ftr_done` | `std::vector<u64>` | cycle that probe's translation **completed** | at translation completion | at release (`Req::tlb_at`, and the translation→DRAM wait) |

`n = b.fmeta.size()` — the number of deduplicated runs, bounded by `batch_window` (16 384 in production).
They exist because a pretranslated release enters the hierarchy at stage 2: the memory system installs the
buffered physical line immediately (`memory.cpp:1008-1016`) and does **not** overwrite `Req::gen` /
`Req::tlb_at`, so the engine has to hand it the two earlier stamps to keep the latency split comparable to
the non-pretranslated policies. The comment at `memory.cpp:1012-1014` states exactly that contract.

Neither array exists for `bulk_org ∈ {0..5}`: those arms never enter `win_issue_g6a`/`win_issue_g6b`, so
the vectors are never assigned and stay empty (0 B). Production is `bulk_org = 8` (`configs/m27_bulk.cfg`),
the G6C arm, which runs G6B's translation driver with P0's admission policy (`sim.hpp:1872`), so both
arrays are allocated and fully used there.

---

## 2. Stage 1 — source tracing

All references live in two files, `sim/src/sim.hpp` and `sim/src/bulk.cpp`. A search of `sim/src` outside
`bulk.cpp` finds only the `Batch` declaration; the test suite (`sim/tests/tests.cpp`) does not name either
array, so no check asserts on them directly.

### 2.1 Declaration and allocation

| item | site |
|---|---|
| declaration | `sim.hpp:1726-1727`, inside `Batch`, in the M23-G6A block (with the M28-D note at `1719-1725`) |
| instance count | `std::array<Batch, 2> bat_{}` (`sim.hpp:1989`) — two buffers, the double-buffering convention every window array is charged twice for |
| allocation (G6A) | `bulk.cpp:2103-2104`, `b.ftr_gen.assign(n, 0); b.ftr_done.assign(n, 0);` in the `g6a_phase == 0` init block (`2100-2108`) |
| allocation (G6B / G6C) | `bulk.cpp:2325-2326`, same two statements, in the `!b.g6b_init` block (`2322-2341`); G6C has no init of its own — it runs inside `win_issue_g6b` and reuses this one |
| deallocation | implicit: retirement does `b = Batch{}` (`bulk.cpp:2998`), which destroys both vectors |
| reset (engine-wide) | `win_reset` (`bulk.cpp:640-645`) clears the engine's arrays and `iss_`; `Batch` vectors are destroyed by the `Batch` default constructor |

`assign(n, 0)` is the **only** reset: there is no other zero-fill, no reuse of stale content across
windows, and `n` is always the window's own run count.

### 2.2 Write sites — five, all inside a translation path

| # | site | statement | context |
|---|---|---|---|
| W1 | `bulk.cpp:2152` | `b.ftr_gen[li] = cycle;` | G6A phase 1, after `mem_.submit` accepted probe `li`; `li = b.tr_next` |
| W2 | `bulk.cpp:2123` | `b.ftr_done[li] = x.cycle;` | G6A phase 1, draining `mem_.take_xlate`; `x.cycle` is the completion cycle |
| W3 | `bulk.cpp:2420` | `b.ftr_gen[li] = cycle;` | G6B/G6C single-cursor admission, after an accepted probe |
| W4 | `bulk.cpp:2369` | `b.ftr_done[li] = x.cycle;` | G6B/G6C completion drain; the same loop that pushes the run into the ready buffer (`2373`) |
| W5 | `bulk.cpp:2643` | `b.ftr_gen[li] = cycle;` | G6C page-cohort admission (`win_admit_g6c`), after an accepted probe |

Each write is one slot, exactly once per run: `rel_tr_[li]` gates the completion writes (`2121`, `2367`),
and the admission cursors (`b.tr_next`, `b.g6b_tr_next`, cohort `c.pos`) advance past an index only when
its probe was accepted, so no run is stamped twice and no run is left unstamped — the feasibility of
release depends on it (§3.3).

### 2.3 Read sites — four

| # | site | statement | what it feeds |
|---|---|---|---|
| R1 | `bulk.cpp:1943` | `r.gen = b.ftr_gen[li];` | the pretranslated request's start-of-life stamp |
| R2 | `bulk.cpp:1944` | `r.tlb_at = b.ftr_done[li];` | the pretranslated request's translation-complete stamp |
| R3 | `bulk.cpp:1969` | `const u64 w = cycle - b.ftr_done[li];` | `g6b_tr_wait_dram` / `g6b_tr_wait_max` / `g6b_runs_released` (and the G6A equivalents at `1975-1977`) |
| R4 | `bulk.cpp:2596` | `if (b.ftr_gen[li] != 0 \|\| rel_tr_[li])` | the G6C duplicate-submission guard |

R1 and R2 sit in one block (`1936-1945`) that also reads `rel_pa_[li]` (`1942`) — an array M28-D already
proved engine-owned. R3 sits two lines after the same block. R4 is the only read that is not in that
block, and it is still inside `win_admit_g6c`, called from `win_issue_g6b` (`2394`) for the issuing window.

### 2.4 Every function that consumes the values

| consumer | site | kind |
|---|---|---|
| `MemorySystem::submit`, pretranslated branch | `memory.cpp:1008-1016` | leaves `gen`/`tlb_at` as the caller set them; no decision |
| retirement accounting | `memory.cpp:1205-1209`: `lat = cycle - r.gen`, `tlb_lat_sum += r.tlb_at - r.gen`, `cache_lat_sum += cycle - r.tlb_at`, `lat_all.push_back(lat)` | statistics |
| stale-lifecycle event | `memory.cpp:1131-1140` (`life_ev(..., LE_S1_STALE, cycle, r.gen, r.vpn)`) | diagnostics only; `life_` is off in production and the branch is unreachable for a pretranslated request, which never enters stage 1 |
| exported latency block | `system.cpp:361-376` → `lat_sum`, `lat_tlb_sum`, `lat_cache_sum`, `lat_mean`, `lat_tlb_mean`, `lat_p50/p95` | statistics, part of every published result file |
| `BulkStats` release-wait counters | set at `bulk.cpp:1966-1978` (G6A branch `1975-1977`) | statistics (`bulk_g6b_tr_wait_dram = 12 551 332 265`, `bulk_g6b_tr_wait_max = 39 034` in `build/wm/m27/wl1/m27bulk_orkut.txt:108-109`) |

### 2.5 Functional weight — answered per the brief's list

| question | `ftr_gen` | `ftr_done` |
|---|---|---|
| affects functional scheduling? | **no** — no admission, release-order, cohort or budget decision reads it | **no** |
| affects translation? | **no** — the probe is submitted with `r.seq = li` only; the stamp is written *after* `submit` returned true | **no** |
| affects request completion / retirement? | **no** — `r.gen` is subtracted in `lat_sum`; `r.ready` and `r.stage` are set by `submit` | **no** — `r.tlb_at` likewise |
| affects liveness? | **no** — `last_progress` is set by `submit`/`tick`, not by a timestamp | **no** |
| affects statistics / output? | **yes** — `lat_sum`, `lat_mean`, `lat_p95`, per-client diag | **yes** — `lat_tlb_sum`, `lat_cache_sum`, `lat_tlb_mean`, `g6b_tr_wait_*` |
| one decision-grade use | **yes** — R4's `!= 0` in-flight test (§4.2) | no |

So the honest classification is: **not decision state, but not removable output state either** — exactly
the distinction that makes consolidation the right answer and elimination the wrong one (§5.3).

### 2.6 Do references escape the Batch?

Yes, but **by value only**: `r.gen` and `r.tlb_at` copy the numbers into a `Req` that outlives the window
inside `inflight_`. No pointer, reference or index into either vector escapes `win_emit_fused`. The arrays
themselves are never touched after the release that copied them.

### 2.7 Concurrency and survival

Two `Batch` objects exist, but the arrays are read or written only by the function chain rooted at

```cpp
void BulkEngine::win_issue(u64 cycle) {
  if (iss_ < 0) return;
  Batch& b = bat_[static_cast<size_t>(iss_)];
  if (cfg_.bulk_org == 6) { win_issue_g6a(cycle, b); return; }
  if (cfg_.bulk_org == 7 || cfg_.bulk_org == 8) { win_issue_g6b(cycle, b); return; }
```
(`bulk.cpp:2746-2750`) — one batch, chosen by `iss_`. The second buffer at that moment holds either a
*filling* window (whose only state writes are `fent`/`fmeta`/`fslot`/`fflag`, via `win_seal_fused`) or
nothing at all. The whole engine is single-threaded and `take_xlate` is drained only from these two
functions.

`iss_` has exactly three writers: retirement (`bulk.cpp:2995`, `iss_ = -1`, immediately before
`b = Batch{}` at `2998`), adoption (`bulk.cpp:3006`, guarded by `iss_ < 0` at `3003`), and `win_reset`
(`bulk.cpp:644`). Adoption therefore cannot overlap a previous window's issue phase.

**Must the data survive window retirement?** No. Retirement requires
`b.state == 2 && b.issued_n >= nlines && b.pending == 0 && b.retired >= b.n_ent` (`bulk.cpp:2994`).
`issued_n` counts lines actually submitted through `win_emit_fused`, and every released run was first
translated (§3.3), so by the time `iss_` clears, every run has already consumed both stamps at R1-R3.
Nothing reads either array afterwards.

---

## 3. Stage 2 — lifetime and ownership

### 3.1 The access shape is the M28-D proof, unchanged

M28-D established that `fpa` (`rel_pa_`), `ftr` (`rel_tr_`) and the ready heap are touched only by the
window holding `iss_`, because every access site lies in `win_issue_g6a` / `win_issue_g6b` /
`win_emit_fused` / the release-ordering helpers, all reached from `win_issue`. The two timestamps have the
**same access shape at the same sites**:

| site | engine array already consolidated (M28-D) | timestamp | relation |
|---|---|---|---|
| `bulk.cpp:2368-2369` | `rel_pa_[li] = x.pa;` | `b.ftr_done[li] = x.cycle;` | adjacent, same loop, same `li` |
| `bulk.cpp:2122-2123` | `rel_pa_[li] = x.pa;` | `b.ftr_done[li] = x.cycle;` | adjacent, same loop, same `li` |
| `bulk.cpp:1942-1944` | `r.pa_line = rel_pa_[li];` | `r.gen`/`r.tlb_at` from both arrays | one basic block |
| `bulk.cpp:2368-2373` | `rel_tr_[li] = 1`, `ready_push(rel_ready_, ...)` | `b.ftr_done[li]` | same loop body |
| `bulk.cpp:2596-2597` | `rel_tr_[li]` | `b.ftr_gen[li] != 0` | one condition |

There is no site where a timestamp is read without the same window's already-consolidated state being
read in the same expression or basic block. A lifetime proof for `rel_pa_`/`rel_tr_` therefore transports
to `ftr_gen`/`ftr_done` by construction, not by analogy.

### 3.2 Sorter-enabled configurations

`ftr_gen`/`ftr_done` are indexed by **run id** (`li`), never by rank, so `Batch::ford`, `rel_pos_` and the
organizer play no part in their addressing. With `m24_dram_org = 1` the ready heap is keyed by
`run_key(li)` instead of `li`, but R1-R4 still index by `li`. Turning the organizer on changes release
order and therefore *which* run is released next — not the lifetime of either array, and not which batch
owns it. The same is true when `ford` is empty (production, `m24_dram_org = 0`).

### 3.3 The one hazard worth stating explicitly

Because the arrays would become engine state, a *stale* entry could in principle be read by the next
window. Three properties rule it out, and they are the same three that protect M28-D's arrays:

1. **A released run was always translated.** A run reaches `win_emit_fused` only from the ready buffer,
   and it enters the ready buffer only at a completion drain (`bulk.cpp:2366-2373`); G6A's phase 2 starts
   only after `b.tr_done == n` (`bulk.cpp:2158`). So `issued_n >= nlines` implies every run has both
   stamps set.
2. **Retirement therefore implies no probe of this window is still in flight**, so `take_xlate` cannot
   deliver a record for a retired window into the next window's arrays. (A record whose `li >= n` would be
   dropped by the existing guard at `2121`/`2367` in any case.)
3. **The next window re-establishes the initial state** with `assign(n, 0)` at its own init block, before
   any submission or drain, exactly as it does today for `rel_pa_`/`rel_tr_`.

The residual shared-queue property — a stale `Xlate` record with an in-range `seq` would be
indistinguishable from a fresh one, because the queue carries no window tag — is **pre-existing** and
identical for `rel_pa_`/`rel_tr_`. It is not created, widened or narrowed by consolidation; it can only be
reached through a liveness violation of property 1, which the suite's liveness checks cover.

### 3.4 Reuse after retirement

Both vectors are `assign`ed on adoption, so content from the previous window is cleared structurally
rather than by convention. A consolidated engine copy is therefore *strictly* safer than the current
per-`Batch` copy in one respect: the second buffer's copy is currently empty-but-declared (never assigned,
because the filling window is not the issuing one), so the declared 2× charge prices a copy that never
holds data. That is the same observation M28-D recorded for `fpa`/`ready`.

---

## 4. Stage 3 — minimum required representation

### 4.1 Are all entries needed?

Yes, at the current capacity. In `bulk_org ∈ {6,7,8}` every run of the window receives exactly one probe,
so every index in `[0, n)` is written and later read. There is no sparsity to exploit: a run that is never
translated would block its window's release (§3.3), so indices `>= n` are the only unused slots, and
`assign(n, 0)` already sizes the array to `n` rather than to `batch_window`.

The one place a *value* rather than a stamp is consumed is R4 (`!= 0`). That could in principle be a
separate one-bit "submitted" vector — but it would be an additional structure, not a smaller one, and it
would split a bit that currently shares a word with the timestamp the same run needs anyway.

### 4.2 Is the full data type required?

In principle a 32-bit stamp would cover every published run (`6 131 444` and `17 904 629` cycles,
`build/wm/m28d/`), and halving both arrays would save another 131 072 B. **This is not proposed and not
available under the brief:** it narrows a field, silently truncates any run beyond 2³² cycles, and changes
the `u64` arithmetic the memory-side accounting is written in (`memory.cpp:1205-1209`). Recorded as
measured-then-rejected, not as a candidate.

### 4.3 Can the values be reconstructed?

No, from anything that survives. `ftr_gen` is the cycle a specific probe for a specific run was accepted;
`ftr_done` is the cycle its translation landed. `MemorySystem` holds both transiently in the probe's own
`Req` (`memory.cpp:1055` sets `gen`, `1151` sets `tlb_at`), but that request retires in the same tick for
an `xlate_only` probe (`memory.cpp:1154-1173`), and the `Xlate` record it emits carries only
`{seq, pa, cycle}` (`memory.cpp:1159`). Neither can be recomputed from the physical line, the run id or
the window's shape — translation latency depends on the walker/PTW state at that moment.

A related option was examined and rejected on scope grounds: fold `ftr_done` into the ready-heap entry
(each run passes through the heap exactly once), which would remove the per-run `ftr_done` array at the
cost of widening the heap element from 8 B to 16 B. That is a **pipeline redesign** — it changes the
ready pool's element type, which M28-D has just fixed, and `win_emit_fused`'s signature — and its net
declared saving (≈98 KiB) does not justify breaking the brief's prohibition on redesign. It is not part
of this recommendation.

### 4.4 Is the storage diagnostic or functionally required?

Diagnostic in the strict sense (no decision depends on it except R4's in-flight test), but **observable**:
`lat_sum`/`lat_tlb_sum`/`lat_cache_sum`/`lat_mean`/`lat_tlb_mean` and `bulk_g6b_tr_wait_*` are exported and
appear in published result files. Removing the arrays would change those outputs; consolidating them
changes nothing. That is precisely why the answer is "one copy", not "no copy".

---

## 5. Stage 4 — candidate optimization

### 5.1 What would change

Move both vectors from `Batch` to `BulkEngine`, beside `rel_pa_`/`rel_tr_` (`sim.hpp:1989-2006`), keeping
name, type, capacity, indexing, timestamp semantics and every consumer:

| step | site | edit |
|---|---|---|
| 1 | `sim.hpp:1726-1727` | remove the two `Batch` members; the M28-D comment at `1719-1725` loses its last clause and points at the engine block |
| 2 | `sim.hpp` engine block (~`2005`) | add `std::vector<u64> tr_gen_, tr_done_;  // (was Batch::ftr_gen/ftr_done)` |
| 3 | `bulk.cpp:2103-2104` | `tr_gen_.assign(n, 0); tr_done_.assign(n, 0);` |
| 4 | `bulk.cpp:2325-2326` | same two statements |
| 5 | `bulk.cpp:2152, 2420, 2643` | `b.ftr_gen[li] = cycle;` → `tr_gen_[li] = cycle;` |
| 6 | `bulk.cpp:2123, 2369` | `b.ftr_done[li] = x.cycle;` → `tr_done_[li] = x.cycle;` |
| 7 | `bulk.cpp:1943-1944, 1969, 2596` | read `tr_gen_[li]` / `tr_done_[li]` |
| 8 | `sim.hpp` (comment) | record that the release path owns one copy, as for `rel_pa_` |

Nine statement edits and one declaration move; no signature, capacity, width, order or policy change. The
`assign(n, 0)` stays at the *same two init sites*, so the state a window is adopted with is bit-for-bit
what it is today.

### 5.2 Why it is safe

* Every write and read is inside the `iss_`-selected chain (§2.7, §3.1).
* Both init blocks are per-adoption (`b.g6a_phase == 0`, `!b.g6b_init`) and run before any submission or
  drain in the same call, so the arrays are sized and cleared for the current window only.
* Retirement implies every stamp has been consumed (§3.3), so no stale value can be read.
* `ftr_gen`/`ftr_done` are not `Batch`-identity-bearing: no code compares the arrays, takes their address
  or sizes them from anything but `n`.
* The values that *do* escape (`r.gen`, `r.tlb_at`) escape by value at release, so the copies in
  `inflight_` remain valid regardless of ownership.

### 5.3 Why consolidation is preferable to deletion

Deleting them would zero the translation half of the latency split for every pretranslated request:
`tlb_lat_sum` would lose its contribution, `lat_tlb_mean` (65 in the M27 Orkut log) would change, and
`bulk_g6b_tr_wait_dram` would have no `ftr_done` to subtract. That is a change in observable simulator
output, which the brief forbids. One model copy preserves every number exactly.

### 5.4 Effect on the measured diagnostics

`bulk_win_vec_bytes` counts only the fused *window* vectors (`fent`, `fmeta`, `fslot`, `fflag` …), not the
release path: M28-D moved four arrays out of `Batch` and the metric stayed at `475 136`
(`build/wm/m28d/m28d_bulk_orkut.txt` byte-identical to the M27 anchor). Moving these two out will
therefore leave `bulk_win_vec_bytes` at `475 136` as well. Nothing in `bulk_win_bytes` (the design charge,
`279 040`) references them either, so no export changes.

---

## 6. Stage 5 — storage accounting

### 6.1 The two structures

| structure | current (2 declared copies) | proposed (1 copy) | saved |
|---|---:|---:|---:|
| `ftr_gen` — 16 384 runs × 8 B × 2 | 262 144 | 131 072 | **131 072** |
| `ftr_done` — 16 384 runs × 8 B × 2 | 262 144 | 131 072 | **131 072** |
| **total** | **524 288** | **262 144** | **262 144 B (256.0 KiB)** |

`2 × 16 384 × 8 = 262 144` per structure. The factor 2 is the double-buffer convention
(`org_overlap` / two buffers, the same convention `bulk_win_bytes = 2 × (16 384×8 + 512×16 + 256)`
encodes); only one copy can ever hold data, because only the issuing window allocates it.

### 6.2 Where that leaves the Bulk footprint

| figure | before M28-E | after M28-E |
|---|---:|---:|
| functional structure (M28-D total) | 677 536 | **677 536** (unchanged) |
| timestamp block, excluded from that total (M28-C §2.4, M28-D §6) | 524 288 | **262 144** |
| **allocated total, timestamps included** | **1 201 824** | **939 680** |
| ratio | — | **0.7819×** |

`988 832 − 311 296 = 677 536` (M28-D); `677 536 + 524 288 = 1 201 824`;
`1 201 824 − 262 144 = 939 680`; `262 144 / 1 201 824 = 21.8 %`.

The functional total is deliberately **not** reduced: M28-C and M28-D price only state the design needs
to function, and this audit does not reclassify instrumentation as hardware. What M28-E changes is the
cost of the instrumentation, at 2 copies → 1.

### 6.3 Remaining uncharged / excluded allocations (carried forward)

| state | bytes | status after M28-E |
|---|---:|---|
| `fmeta` + `fslot` + `fflag` + cohort counters (window state outside `bulk_win_bytes`) | 196 800 | unchanged (M28-D §5) |
| `ftr_gen` + `ftr_done` | 262 144 | still outside the functional total, now one copy each |
| `win_rec` timeline | 432 | diagnostics, unchanged |
| `row_acts_`/`row_stamp_` (`dram_diag = 1`) | 65 536 | pure diagnostics, unchanged |
| `g6g_*` / `g6f_*` / `g6i_*` scratch | 0 | off in production, unchanged |
| `reuse_` (M13) | 0 | `reuse_dir = 0`, unchanged |
| `g6a_pa_capacity` buffer | 0 | `bulk_org = 8` (`bulk_g6b_pa_bytes = 65 536` is the ready-pool charge, not this) |

### 6.4 Honest caveat on what is being saved

Exactly as recorded in M28-D §7: `std::vector` allocates lazily, so the current process already holds at
most one heap copy per array (the second `Batch`'s copy is empty until it is issued, and a batch is never
issued while another issues). The 262 144 B is therefore a saving in **declared functional structure** —
the thing M28-C and M28-D price — not in peak heap, and not in physical area.

---

## 7. Decision

**1. Both structures can be consolidated.**

The evidence:

* Every one of the five writes and four reads is inside the `iss_`-selected chain
  (`win_issue_g6a`, `win_issue_g6b`, `win_admit_g6c`, `win_emit_fused`) — the same chain M28-D verified for
  `fpa`/`ftr`/`ready`.
* Both allocation sites are per-adoption init blocks that run before any submission or drain, and there is
  exactly one issuing window at a time (`bulk.cpp:3003-3009`).
* Retirement requires every run released, hence every translation landed, hence no stamp is still needed
  and no in-flight probe can target the next window (`bulk.cpp:2994`).
* The sorter/`ford` path does not change the addressing (`li`, never a rank), so organizer-on and
  organizer-off configurations are equally unaffected.
* Neither array carries decision state. R4's `ftr_gen[li] != 0` is the one decision-grade read; it is a
  zero-test that `assign(n, 0)` reproduces exactly, and its guard reports zero firings in production
  (`bulk_g6c_dup_submit_blocked = 0`, `build/wm/m27/wl1/m27bulk_orkut.txt:123`).
* Deleting rather than consolidating would change exported latency statistics, which the brief forbids;
  one copy preserves every consumer bit-for-bit.

**Smallest safe implementation:** steps 1-8 of §5.1 — two `Batch` members moved to `BulkEngine` beside
`rel_pa_`/`rel_tr_`, the two existing `assign(n, 0)` pairs retargeted, and nine statement renames. Types,
capacities, indexing, timestamp semantics, init/reset points, scheduling, completion, liveness and every
exported number stay as they are.

**Savings:** 131 072 B per structure, **262 144 B** total; functional footprint stays 677 536 B, allocated
footprint including the timestamps falls 1 201 824 → 939 680 B.

No source, configuration, build, test or simulation was run for this audit. If the change is authorized,
the verification bar should be M28-D's: rebuild, the full unit suite, and one paired Bulk-vs-M0-RM Orkut
cell that must reproduce `6 131 444` cycles and `lat_tlb_sum = 58 715 056` exactly.
