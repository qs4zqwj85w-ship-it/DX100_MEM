# M5 — First functional bulk indirect-access engine: implementation report

**Status:** the bulk engine is implemented, functionally validated, and running on the calibrated
`vortex_ref.cfg`. It is **not** evaluated for performance and **no superiority is claimed** (§8.6).
The conventional SIMT baseline is unchanged and its M4a/M4b calibration gates still pass bit-exactly
(§8.5).

**Scope of this milestone:** generation and coordination only — descriptor fetch, index streaming,
address generation, a FIFO request buffer under the `R = 128` residency budget, element identity and
completion. Duplicate detection, cache-line deduplication, bank/row-aware or clairvoyant scheduling,
a new memory hierarchy, area/power, LLVM and Vortex integration are **out of scope and absent** (§10).

Everything below is either `[IMPL]` (exists, tested, reproducible with the commands in §8), `[PLAN]`
(designed for, not built), or `[A]`/`[V]` for modelling assumptions and source-verified parameters,
following the convention of `standalone-simulator-design.md` §4.1.

---

## 1. Architecture implemented

### 1.1 Dataflow

```
                 host: BulkDesc  ──► submit()  (queue, one descriptor executes at a time)
                                        │
   ┌────────────────────────────────────▼─────────────────────────────────────┐
   │ BulkEngine                                                               │
   │                                                                          │
   │  stage A  descriptor fetch   1 line request  ──► decode + validate       │
   │              │                                                           │
   │  stage B  index stream       ceil(count·w_i/64) line requests, FIFO      │
   │              │               idx_window (0 = R) bounds this stream       │
   │  stage C  address generator  addr = data_base + index[e]·w_d             │
   │              │               ≤ ag_lanes (8) addresses per cycle          │
   │              │               element order, only from *returned* indices │
   │  stage D  request buffer     one slot per line request, R = 128 total    │
   │              │               invariant unissued + issued_not_filled ≤ R  │
   │  stage E  issue              FIFO, ≤ eng_issue (1) submissions per cycle  │
   │              │               no dedup · no reorder · no oracle           │
   │  stage F  completion         slot record → element id → value → output    │
   └──────────────────────────────┬───────────────────────────────────────────┘
                                  │  same object, no second backend
                     ┌────────────▼─────────────┐
                     │  MemorySystem (unchanged)│  L1 TLB → L2 TLB → PTW
                     │  translation · caches    │  L2 cache · banked DRAM
                     │  MSHRs · DRAM queues     │  FR-FCFS · latency · bytes
                     └──────────────────────────┘
```

The engine is a **single global client** with **no private L1**: after translation its requests probe
the shared L2 directly, which is what the design requires of the accelerator path
(`standalone-simulator-design.md` §3.2, §4.4; the DXA enters the socket arbiter without an L1,
`dx100-experiment-design-v3.md` §2). Everything below the L2 — MSHRs, bank scheduling, row state,
queue depth, link timing — is byte-for-byte the baseline's code path.

### 1.2 Per-cycle order of operations (`BulkEngine::tick`)

| # | Action | Why this order |
|---|---|---|
| 1 | `MemorySystem::tick` — advance the hierarchy, collect completions | one clock, one memory system |
| 2 | Attribute completions: decode descriptor, publish index lines, reconstruct element values, free slots | slots must be free before anything new is allocated in the same cycle |
| 3 | Start the next queued descriptor if idle | descriptor FIFO |
| 4 | Generate: request index lines (bounded), then compute target addresses from returned index data | dependency: generation reads only completed index lines |
| 5 | Issue: pop the FIFO head, hand to `MemorySystem::submit` (may refuse → retry next cycle) | backpressure is real, never bypassed |
| 6 | Account occupancy and assert `unissued + issued_not_filled ≤ R` | the invariant is checked **every** cycle, not at the end |

### 1.3 Implemented vs planned

| Capability | State |
|---|---|
| Descriptor fetch from memory, decode, validation, clean rejection | `[IMPL]` |
| Index streaming with real line traffic + translation | `[IMPL]` |
| Target address generation, per-element identity, configurable `ag_lanes` | `[IMPL]` |
| Finite request buffer, `R = 128` residency invariant, backpressure counters | `[IMPL]` |
| FIFO issue, out-of-order completion, out-of-order index responses | `[IMPL]` |
| Result reconstruction into `output[element_id]` | `[IMPL]` |
| Duplicate detection / cache-line dedup | `[PLAN]` — M6, hook marked `M6:` in `bulk.cpp` |
| Scheduling policy (row-aware, clairvoyant, batching by bank) | `[PLAN]` — M6 |
| Multiple descriptors in flight (pipelining across tiles) | `[PLAN]` — M6; today descriptors execute one at a time |
| Scatter / store direction, RMW, predicates, multicast | `[PLAN]` — later milestone |
| Output writeback cost (LMEM tile write, copy-out) | `[PLAN]` — see §10.4 |

---

## 2. Descriptor definition

### 2.1 Fields

One descriptor describes one bulk gather, `output[i] = data[index[i]]`, `i = 0 … count-1`.

| Field | Type | Meaning |
|---|---|---|
| `desc_line` | `u64` | VA **line number** where the descriptor itself lives (64 B line) |
| `index_base` | `u64` | VA byte address of `index[0]` — must be 64 B aligned |
| `data_base` | `u64` | VA byte address of `data[0]` — must be `data_bytes` aligned |
| `count` | `u32` | number of elements |
| `elem_base` | `u32` | element id of element 0 (identity survives across descriptors) |
| `index_bytes` | `u8` | index element width: 4 or 8 |
| `data_bytes` | `u8` | data element width: 1, 2, 4 or 8 (must divide 64) |

That is the minimum needed for correct execution in this simulator's address space (Sv32, 4 KiB
pages, 64 B lines, elements ≤ 8 B). `elem_base` is required, not decorative: without it two
descriptors cannot both report identity. No tensor ranks, strides, multicast masks, predicates or
atomics — deliberately (`dx100-vortex-gap-analysis.md` lists those as separate capabilities).

### 2.2 On-memory encoding (64 B, little-endian)

| Offset | Width | Field |
|---|---|---|
| 0 | 4 | magic `0x44583031` |
| 4 | 4 | version `1` |
| 8 | 8 | `desc_line` |
| 16 | 8 | `index_base` |
| 24 | 8 | `data_base` |
| 32 | 4 | `count` |
| 36 | 4 | `elem_base` |
| 40 | 1 | `index_bytes` |
| 41 | 1 | `data_bytes` |
| 42 | 22 | reserved, zero |

The descriptor is **not** passed to the engine as a struct of live fields: the host writes the
encoded line into the memory image, and the engine decodes it from the line it fetched. A corrupted
line (bad magic/version) is rejected rather than executed with garbage — tested (§8.2, `T1`, `T16`).

### 2.3 Validation (`BulkDesc::validate`, `bulk.cpp`)

| Rule | Rejection reason |
|---|---|
| `index_bytes ∈ {4, 8}` | `index_bytes must be 4 or 8` |
| `data_bytes ∈ {1, 2, 4, 8}` and `64 % data_bytes == 0` | `data_bytes must be 1, 2, 4 or 8` |
| `index_base % 64 == 0` | `index_base must be 64 B (line) aligned` — an unaligned array would be decoded at the wrong offsets (§8.4 defect d) |
| `data_base % data_bytes == 0` | `data_base is not data_bytes-aligned` |
| `count ≤ bulk_max_elems` (65 536) | `count exceeds bulk_max_elems` |
| `desc_line < 2^26`, `index_base`, `data_base` < 4 GiB | `… outside the 4 GiB VA space` |
| `count·index_bytes` does not wrap, `index_base + count·index_bytes ≤ 4 GiB` | `index array exceeds the 4 GiB VA space` |
| `elem_base + count ≤ 2^32` | `element ids exceed the 32-bit element-id space` |

`count == 0` is **valid**: an empty descriptor still costs its fetch, then retires (§8.2 `T12`).

Rejection is never silent and never counted as success: `desc_rejected` increments, a reason string is
recorded in `BulkEngine::errors()`, the run does not execute the descriptor, and the identity
`desc_submitted == desc_completed + desc_rejected` holds (§8.4 defect b).

---

## 3. Address-generation behavior

### 3.1 Index stream (stage B)

* Index element `e` lives at `index_base + e·index_bytes`; its line is that address `>> 6`.
* Lines are requested **sequentially and exactly once**, one residency slot each, advancing
  `first_elem += 64/index_bytes`. With line-aligned `index_base` and `index_bytes | 64`, no index
  element ever straddles two lines, so one request serves a whole chunk.
* Realized request count is therefore `ceil(count·index_bytes / 64)` — 16 indices per line at 4 B,
  8 per line at 8 B. Measured: E0 (4 B, 16 384 elements) = 1 024 index lines; E1 (8 B) = 2 048.
* Concurrency is bounded by `idx_window` (0 = the whole `R` budget; default 0 — see §8.3 for the
  measured reason) and by `R` itself.

### 3.2 Generation (stage C)

Per element `e`, in **element order**:

```
line    = index line that covers e                     (must have completed)
value   = load_le(line_bytes[ (first_elem of line) + (e - first_elem)·index_bytes ], index_bytes)
addr    = data_base + value · data_bytes
elem_id = elem_base + e
```

* Only **returned** index lines are read; a line is published into `idx_done_` by the completion
  handler, and generation looks up the chunk containing the next element. If that chunk has not
  arrived, generation stalls and `idx_wait_stall` counts the cycle. There is no path by which a
  target request exists without its index value, which the tests probe from both sides
  (`tgt_first_gen ≥ idx_first_done`, `idx_wait_stall > 0`, `T4`).
* Out-of-order index fills are held in `idx_done_` until their turn; generation still consumes
  elements in order, so the engine's *issue* order is element order even when *fills* are not
  (`T6`, `T7`).
* Throughput: at most `ag_lanes` (default 8, design §3.2) addresses per cycle. `ag_util` is the
  fraction of available AG slots actually used over descriptor-active cycles.

### 3.3 Address arithmetic and faults

`addr = data_base + value·data_bytes` is computed in 64-bit, and `addr + data_bytes > 2^32` is a
reported fault: `addr_faults` increments, the element is **accounted but not requested**, and the
invariant `elems_total == elems_done + addr_faults` holds. No element can vanish silently, and no
wrapped (aliased) address is ever issued. `data_bytes | 64` plus `data_bytes`-aligned `data_base`
guarantee an element never straddles a line, so the in-line offset is exact.

### 3.4 Independent address verification

Every target completion re-derives the element's address from scratch — element index → index byte
address → index value → target address — using the memory image, and compares it with the address the
request was built from. Any disagreement increments `addr_mismatch`, and every run asserts it is 0.
This is a check on extraction offsets, scaling and stale index lines, not a restatement of the
generation code path (it does not read `idx_done_`).

---

## 4. Request-buffer and residency accounting

### 4.1 The pool

There is **one** pool of `R = 128` slot ids, shared by descriptor fetches, index lines and target
requests. A request holds its slot from the cycle it is generated until the cycle its fill returns:

```
residency  =  unissued_(FIFO of allocated slots not yet handed to the memory system)
            + mem.inflight_count()                      (issued, fill not returned)

invariant: residency ≤ R, checked every clock in BulkEngine::tick
```

There is no second window: generation is refused when `residency() == R` (`buf_full_stall`
increments), and issue can only move a request from `unissued_` into the memory system, never out of
the budget. `issued_not_filled` is the memory system's own in-flight list — the same list the
baseline's slots correspond to — so the two front-ends are resident under the same accounting.

### 4.2 Counters and evidence

| Counter | Meaning | E0 (vortex_ref) |
|---|---|---|
| `buf_max` | peak `residency` | **128** (= R, budget saturated) |
| `unissued_max` | peak generated-but-not-issued | 127 |
| `buf_occ` | Σ residency over cycles | 34 221 531 |
| `buf_full_stall` | cycles generation was refused for lack of a slot | 266 202 |
| `issue_stall` | refused `submit()` calls (memory backpressure) | 76 247 |
| `residency_violations` | times the invariant was broken | **0** |

Enforcement is tested at both extremes: `R = 8` binds (`buf_max = 8`, `buf_full_stall > 0`, all 64
elements complete) and `R = 1` still completes with `buf_max = 1` (`T9`, `T10`). The `R = 1` case
matters: it proves the descriptor/index/target stages each release their slot on completion and that
progress does not depend on holding two requests at once.

### 4.3 Issue policy

Strict FIFO in generation order, at most `eng_issue` submissions per cycle. Duplicate detection is
**off**, so `tgt_req_gen == tgt_addr_gen == count` even when several elements share a line — measured
`T2`: 32 elements over 4 distinct lines still produce 32 requests. `issue_monotonic_violations`
counts any target issued out of element order and is 0 in every run (`T15`).

---

## 5. Memory-system integration

### 5.1 One object, no second backend

`BulkEngine` is constructed with a reference to the `MemorySystem` owned by `Sim` and has no other
path to memory. Every request is a `Req` handed to `MemorySystem::submit`, so it takes the same
translation, the same MSHRs, the same caches, the same DRAM banks and queue, the same timings, and
the same byte accounting as a baseline request.

### 5.2 The one structural difference: no private L1

The engine's requests set `Req::bypass_l1`, and `MemorySystem` then probes the **shared L2** directly
instead of the per-core L1 (`memory.cpp`, stage-2 path, plus one propagation line in the L2 MSHR
recycle loop so a bypassing client is handed its fill before the MSHR is reused). This is the design's
architecture (§4.4: the accelerator path has no L1), not a shortcut: it removes the possibility of the
engine harvesting L1 hits the baseline pays for, and it is why every bulk run asserts
`l1_reads == 0` — a counter that would catch a regression here immediately. `Mode::Baseline` and
`Mode::Neutral` never set the flag, so their code path is untouched (§8.5).

### 5.3 Translation and PTE traffic

The engine is a normal translation client of core 0's dTLB (16 entries, 4-way, 4 MSHRs), the 512-entry
L2 TLB and the 2 walkers. PTE fetches are real L2 traffic on the same hierarchy (`pte_fetches =
2 × walks`, `T13`). Measured on E0: `walks = 529` for 3 object classes (descriptor page, 1 MiB index
array, 1 MiB data array across 512 pages) versus the baseline's 512, i.e. the engine's index array
costs 17 extra pages-worth of walks.

### 5.4 Shared counter export

`collect_mem_stats()` and `collect_lat_stats()` are the single definitions of the downstream counters;
both front-ends call them. That is the reason a bulk run's L1/L2/TLB/DRAM block is directly comparable
to a baseline run's, with no second implementation to drift.

**Accounting identity, checked on both front-ends**: `l2_reads − l2_hits − l2_mshr_joins = dram_reads`
— baseline 17 408 − 989 − 2 = 16 417; engine 18 463 − 1 018 − 1 = 17 444. And
`dram_bytes = dram_reads × 64` in both.

---

## 6. Completion and response handling

* Each allocated slot carries its own record (`ReqKind`, VA line, element id, in-line offset, element
  width, first index element). Completion is dispatched on `(core, slot)` from the memory system, so a
  fill is attributed to the request that caused it, never to "the next one".
* **Out-of-order index fills**: published into `idx_done_` keyed by element chunk; `idx_lines_ooo`
  counts fills that arrive after a younger request completed. Measured 256 of 1 024 on E0.
* **Out-of-order target fills**: reconstructed per element id; `tgt_ooo` counts them (8 888 of 16 384
  on E0). Element identity is never positional: `value_of(elem_id)` is indexed by the id carried in
  the slot record.
* **Descriptor completion condition**: all `count` target requests have filled (or faulted), at which
  point the descriptor retires and `desc_cycles_sum/max` record submission → retirement; `count == 0`
  retires right after validation. `desc_fetch_cycles_sum` splits out the fetch portion (335 cycles on
  E0 — the descriptor's page is cold, so this is one walk plus one fill).
* **Errors are reported, not swallowed**: descriptor rejection (§2.3) and address faults (§3.3) are
  counters plus strings; the run's conservation checks then prove that every submitted element either
  completed or is in the fault count (`elems_total == elems_done + addr_faults`).
* Values are read from the memory image at the address the request resolved to
  (`MemorySystem::pa_line_of_va_line`), which is the same mapping `tlb_fill` installs — the model
  translates `ppn = vpn + PPN_OFFSET`, so a PA≠VA aliasing bug cannot hide. `addr_mismatch == 0` in
  every run.

---

## 7. New counters and their precise definitions

All are exported through `Stats` and printed by `--all`. Existing counters keep their meaning
(`requests_generated`, `issued`, `completed` for the engine count descriptor + index + target line
requests, and each component is also available separately).

### 7.1 Descriptors

| Counter | Definition |
|---|---|
| `bulk_desc_submitted` | `submit()` calls that enqueued a descriptor |
| `bulk_desc_completed` | descriptors that fetched, validated, and had every element fill or fault |
| `bulk_desc_rejected` | descriptors refused (at submission, at decode, at validation) |
| `bulk_desc_cycles_sum` / `_mean` / `_max` | **descriptor execution latency**: submission → retirement |
| `bulk_desc_fetch_cycles_mean` | submission → descriptor line decoded |

### 7.2 Index stream

| Counter | Definition |
|---|---|
| `bulk_idx_lines_req` / `_done` | index-line requests generated / fills returned |
| `bulk_idx_lines_ooo` | index fills that arrived while an older index request was outstanding |
| `bulk_idx_out_max` | peak index lines issued-and-not-filled (bounded by `idx_window`) |
| `bulk_idx_window` | effective window (`idx_window`, or `R` when 0) |
| `bulk_idx_elems_req` / `_done` | index elements the accepted descriptors require / elements actually decoded |
| `bulk_idx_first_done` | cycle the first index fill returned |
| `bulk_idx_wait_stall` | cycles the generator had budget and buffer space but no returned index value |
| `bulk_idx_line_bytes` | `idx_lines_req × 64` — line traffic the index stream asked for |

### 7.3 Address generation and target requests

| Counter | Definition |
|---|---|
| `bulk_tgt_addr_gen` | addresses computed (`= elems decoded`; includes faulted ones) |
| `bulk_tgt_req_gen` | target line requests formed (`= addr_gen − faults`; one per element while dedup is off) |
| `bulk_tgt_req_issued` / `_done` | handed to the memory system / fills returned |
| `bulk_tgt_ooo` | target fills that arrived while an older target request was outstanding |
| `bulk_tgt_first_gen` | cycle the first target address was generated |
| `bulk_tgt_line_bytes` | `tgt_req_gen × 64` — line traffic the target stream asked for |
| `bulk_desc_line_bytes` | `desc_submitted × 64` — descriptor traffic |
| `bulk_ag_cycles` | cycles a descriptor was active |
| `bulk_ag_util_x1000` | `1000 · addr_gen / (ag_cycles · ag_lanes)` |
| `bulk_addr_faults` | elements whose address left the 4 GiB VA space (accounted, not requested) |
| `bulk_addr_mismatch` | completed targets whose address disagreed with an independent re-derivation |

### 7.4 Buffer and invariants

| Counter | Definition |
|---|---|
| `bulk_buf_occ` | Σ residency per cycle |
| `bulk_buf_max` | peak `unissued + issued_not_filled` |
| `bulk_unissued_max` | peak `unissued` |
| `bulk_buf_full_stall` | cycles generation was refused because residency was `R` |
| `bulk_issue_stall` | refused `submit()` calls (memory system backpressure) |
| `bulk_residency_violations` | clocks on which `residency > R` (must be 0) |
| `bulk_issue_monotonic_violations` | target requests issued out of element order (must be 0 under FIFO) |
| `bulk_elems_total` / `_done` | elements of accepted descriptors / elements reconstructed |

Index and target traffic are separate by construction (`bulk_idx_line_bytes`,
`bulk_tgt_line_bytes`, `bulk_desc_line_bytes`), while `dram_*` remains the aggregate of what the
hierarchy actually moved.

---

## 8. Tests and exact results

Reproduce: `cd DX100_MEM/sim && make clean && make -j4 && ./bin/dx100_tests`.
Build: **0 warnings, 0 errors** (`-Wall -Wextra`). Suite: **3 683 checks, 0 failures**, 33 groups
(17 pre-existing + 16 M5).

### 8.1 Test groups

| # | Group | Proves |
|---|---|---|
| 1 | `bulk_desc_validation` | field validation (9 cases), encode/decode round-trip, corrupted line rejected, invalid descriptor fetched-then-rejected, garbage line rejected |
| 2 | `bulk_index_addressing` | `ceil(count·w_i/64)` index requests at both widths; 8 B indices with a 1-element tail line; 32 elements over 4 lines → 32 requests |
| 3 | `bulk_target_addressing` | hand-computed `data_base + index·w_d` and byte-exact values for indices {3,0,7} |
| 4 | `bulk_index_dependency` | `tgt_first_gen ≥ idx_first_done`, `idx_wait_stall > 0`, `addr_mismatch = 0` |
| 5 | `bulk_multi_index_outstanding` | 64 index lines, `idx_out_max = 8` with window 8 vs 64 without; target and index slots coexist |
| 6 | `bulk_index_out_of_order` | 16 index lines across a page boundary (second page warmed): `idx_lines_ooo = 7`, every element correct |
| 7 | `bulk_target_out_of_order` | alternating warm/cold data pages: `tgt_ooo = 15`, `issue_monotonic_violations = 0`, every value correct |
| 8 | `bulk_element_ids` | `elem_base = 1000`: ids 1000–1003 present, 999/1004 absent, duplicate index not merged |
| 9 | `bulk_buffer_capacity` | `R = 8`: `buf_max = 8`, `unissued_max ≤ 8`, `buf_full_stall = 515`, all 64 elements done |
| 10 | `bulk_residency_invariant` | `R = 128` and `R = 1` both hold the invariant; `R = 1` completes (1 170 cycles) |
| 11 | `bulk_descriptor_completion` | single and two-descriptor runs; `submitted = completed = 2`, 8 elements, latencies recorded |
| 12 | `bulk_empty_and_small` | `count = 0` retires after the fetch with 0 requests; `count = 1` → 1 index line, 1 target |
| 13 | `bulk_translation_and_cache` | cold run: `walks ≥ 3`, `pte_fetches = 2·walks`, `l1_reads = 0`; second descriptor reuses the TLB (`hits = 12`, same walk count) |
| 14 | `bulk_memory_accounting` | `bulk_wl = E0/E1` via `Sim`: 64/128 index lines, 1 024 targets, every address equal to `data_base + trace.addr`, `l1_reads = 0`, `dram_bytes = 64·dram_reads` |
| 15 | `bulk_fifo_issue` | single-line rig: `tgt_ooo = 0`, monotonic issue, submission rate ≤ `eng_issue`; with `l2_mshr = 2`, `issue_stall = 304` and nothing lost |
| 16 | `bulk_regressions` | line-vs-byte address mapping; rejected ≠ completed; out-of-range `desc_line` refused pre-fetch; unaligned `index_base` rejected |

### 8.2 Small deterministic gathers (hand-checked)

`T3` is the reference example: index array `{3, 0, 7}`, `data_base = 0x200000`, `w_d = 8`. The engine
produces addresses `0x200018 / 0x200000 / 0x200038` and values equal to the byte pattern planted at
those offsets. `T12` shows the fetch cost of an empty descriptor (377 cycles, no requests) against a
single-element one (1 131 cycles, 1 index line + 1 target).

### 8.3 Calibration runs

`configs/vortex_ref.cfg`, `W1`, `N = 16 384`, `Λ = 16 384`, `P = 512`, `seed = 1`. Bulk gather `E0`
uses the *same trace*: `index[i] = trace_addr[i] / elem_bytes`, so both front-ends touch the same
lines at the same in-line offsets, and the engine additionally pays the index stream and the
descriptor fetch.

| Run | cycles | line reqs | DRAM reads | row hits | walks | L1 reads | peak in-flight | mean lat |
|---|---|---|---|---|---|---|---|---|
| SIMT baseline (reference, `lsu_pending = 8`) | 280 432 | 16 384 | 16 417 | 188 | 512 | 16 384 | 11 | 142.6 |
| SIMT baseline, `lsu_pending = 0` (R-bound) | 270 753 | 16 384 | 16 417 | 277 | 512 | 16 384 | 128 | 1 571.5 |
| **Bulk E0** (4 B indices) | 268 592 | 17 409 | 17 444 | 1 871 | 529 | **0** | 128 | 1 451.2 |
| Bulk E1 (8 B indices) | 276 144 | 18 433 | 18 468 | 2 831 | 545 | 0 | 128 | 1 416.9 |
| Bulk E0, `eng_issue = 4` | 268 592 | 17 409 | 17 444 | 1 871 | 529 | 0 | 128 | 1 454.0 |
| Bulk E0, `idx_window = 8` | 271 284 | 17 409 | 17 444 | 1 513 | 578 | 0 | 128 | 1 471.6 |
| Bulk E0, `desc_fetch = 0` (ablation) | 268 257 | 17 409 | 17 442 | 1 871 | 528 | 0 | 128 | 1 451.3 |

Bulk E0 in full: 1 descriptor (fetch 335 cycles, total 268 592), 1 024 index lines / 16 384 elements
decoded, 16 384 target addresses → 16 384 requests → 16 384 fills, `addr_mismatch = 0`,
`residency_violations = 0`, `issue_monotonic_violations = 0`, `addr_faults = 0`,
`buf_max = 128`, `unissued_max = 127`, `buf_full_stall = 266 202`, `issue_stall = 76 247`,
`ag_util = 0.7 %`, index traffic 65 536 B + descriptor 64 B, target traffic 1 048 576 B,
DRAM 17 444 lines = 1 116 416 B, `dram_max_q = 63`, `l2_mshr_max = 64`.

Sanity checks from milestone §6, all satisfied on every run: complete suite green; M4a/M4b green;
index and target traffic verified independently; every target address matched an independent
re-derivation and, for `E0`, the trace it was derived from; `elems_total == elems_done + addr_faults`;
`issued == completed` (nothing lost); `tgt_req_gen == tgt_addr_gen` and one request per element
(nothing merged, nothing counted twice).

### 8.4 Defects found and fixed during the milestone

| # | Defect | Effect | Fix | Regression |
|---|---|---|---|---|
| a | Values/descriptors were fetched by passing a **VA line number** to the byte-address mapping | descriptor "decode failed (magic)", every value read from the wrong physical line | `MemorySystem::pa_line_of_va_line`; all engine lookups use it | `T16` (a) + every functional test |
| b | A rejected descriptor incremented both `desc_rejected` and `desc_completed` | `submitted ≠ completed + rejected`; a rejection looked like a success | `abort_desc()` for non-completing descriptors | `T16` (b), `T1` |
| c | Index chunking assumed a 64 B-aligned `index_base` without checking it | an unaligned index array would be decoded at wrong offsets (silent wrong addresses) | validation rule `index_base % 64 == 0` | `T16` (d) |
| d | No bound on the index stream: it filled all 128 slots with index prefetch, so the dependent target stream could not start until the whole index array had been *requested* (first target at cycle 7 278) | coordination failure between the two stages | `idx_window` knob, `idx_resident_` accounting | `T5` (window binds: 8 vs 64) |

The window default is `0` (= the `R` budget) because measurement disagreed with the intuition: bounding
it to 8 starts the target stream 6 600 cycles earlier but stretches the index tail, costing 1.0 % more
cycles (271 284 vs 268 592). `idx_window` therefore remains a documented capacity knob, not a hidden
optimisation, and §8.3 reports both.

### 8.5 The baseline is unchanged (calibration gates)

| Gate | Expected | Observed |
|---|---|---|
| M4a exact equivalence | identical hash, identical cycles, 0 differing counters | `hash = 5ffe97c9c9b4bf00`, `cycles = 86 308 / 86 308` — **identical to M4** |
| M4b architectural null | walks exact, counts Δ = 0, cycles within 5 % | `walks 512/512`, `cycles 104 332 / 102 947` (1.3 %) — **identical to M4** |
| Full pre-existing suite | 76 checks, 0 failures | 76 checks, 0 failures |
| Whole suite now | — | **3 683 checks, 0 failures** |

The neutral issuer and the completion hash were not touched; `Mode::Baseline`/`Mode::Neutral` never
set `bypass_l1`, and their stage-2 path is the same code it was.

### 8.6 What the comparison does and does not show

**No performance superiority is claimed, and none is established.** The honest reading of §8.3:

* At **equal residency** (`R = 128`, `lsu_pending = 0`), the engine is **0.8 % faster**
  (268 592 vs 270 753 cycles) while moving **6.3 % more DRAM lines** (17 444 vs 16 417). Per line of
  real work it is *not* more efficient.
* Against the **reference** configuration (`lsu_pending = 8`), the engine is 4.2 % faster, but that
  difference is **occupancy capacity** (128 vs 11 outstanding), not generation: exactly the confound
  that M1/M2/M3 were designed to separate. Reporting it as an engine result would be the error the
  experiment design warns about.
* The engine reaches 84 % of the 2-bank row-conflict bound (0.0648 vs 0.077 lines/cycle; baseline
  0.0584, 76 %). Both are DRAM-bound, consistent with M4's finding that the baseline is not limited by
  issue width or MSHRs.
* `eng_issue = 1` is **not** binding: at `eng_issue = 4` the cycle count is identical (268 592). The
  engine was not given issue bandwidth it does not need.
* The one measured structural difference worth carrying forward: the engine's `dram_max_q = 63` vs the
  baseline's 10, and 1 871 vs 188 row hits — a direct consequence of holding a larger coordinated set
  of requests. Whether that can be converted into throughput is an M6 question (it needs dedup and
  ordering to do so), not an M5 result.

---

## 9. Changes to existing files

| File | Change | Why |
|---|---|---|
| `sim/src/bulk.cpp` | **new** (614 lines): `BulkDesc`, `BulkEngine`, `Sim::run_bulk`, `E0/E1` workload construction | the engine |
| `sim/src/sim.hpp` | `Mode::Bulk`, `BulkDesc`, `BulkStats`, `BulkEngine`, `Req::bypass_l1`, `u16`, `VA_LIMIT`, `Config::{ag_lanes, eng_issue, idx_window, desc_fetch, bulk_max_elems, bulk_wl, bulk_index_base, bulk_data_base, bulk_desc_line}`, `MemorySystem::{pa_line_of_va, pa_line_of_va_line, write_va, line_bytes}`, `collect_mem_stats`, `collect_lat_stats`, `Sim::{run_bulk, memory, engine}` | shared interfaces both front-ends need |
| `sim/src/memory.cpp` | L1-bypass probe + fill propagation for a bypassing client; physical memory image (`pa_line_of_va`, `write_va`, `line_bytes`) | engine's architecture (no L1, §4.4) and a testable data image; baseline path untouched |
| `sim/src/system.cpp` | counter export extracted into `collect_mem_stats` / `collect_lat_stats` (no counter changed meaning); `run()` dispatches `Mode::Bulk` | one definition of the downstream counters for both front-ends |
| `sim/src/util.cpp` | config keys for the engine; `mode=bulk`; `to_string` shows the front-end and engine parameters | configuration surface |
| `sim/src/main.cpp` | bulk summary block (descriptor, index/target streams, buffer, AG utilisation, descriptor errors) | the CLI must report the two streams separately |
| `sim/tests/tests.cpp` | +16 groups, +791 lines; no existing check modified or relaxed | §8 |
| `sim/Makefile` | `src/bulk.cpp` added to `SRC` | build |
| `sim/configs/vortex_ref.cfg` | unchanged | — |
| `ref/vortex` | untouched | — |

`docs/standalone-simulator-design.md` was treated as authoritative and **not edited**; the one place
where this milestone had to add a rule (line-aligned `index_base`, §2.3) is recorded here and in the
code, with the reason.

---

## 10. Remaining simplifications and limitations

1. **One descriptor in flight at a time.** Descriptors execute FIFO, so there is no cross-descriptor
   overlap of the index streams. Fields are read when a descriptor *starts*, so each queued descriptor
   must own its line and its arrays must be stable until it is fetched. A descriptor ring with a real
   fetch/prefetch pipeline is M6 work.
2. **No write path.** The engine reads only. Stores, scatter, RMW and predicates are absent, which
   also means the baseline's missing write traffic (M4 §3.4) stays missing on both sides.
3. **Results are modelled, not delivered.** `output[element_id]` is reconstructed in an engine-side
   map; the LMEM tile write and copy-out are **not** charged (design §3.2 row 6 charges them). Any
   future efficiency claim must add that cost, and it is *against* the engine.
4. **Duplicate detection is off by design**, so `tgt_req_gen == count` always. Index lines are
   requested exactly once each (that is structural, not a dedup mechanism).
5. **The engine borrows core 0's dTLB** (16 entries) rather than the design's 8-entry uTLB. This is a
   *favourable* deviation for the engine and must be disclosed in any translation comparison; the
   L2 TLB, walkers and walk cache are shared and unmodified.
6. **`replays` is large for bulk runs** (12.2 M on E0) because up to 128 resident requests poll a
   64-entry L2 MSHR pool; refused probes are decremented from `l2_reads`/`l2_misses`, so this is retry
   bookkeeping, not extra traffic. It does mean `replays` is not comparable across front-ends.
7. **Memory content is a synthetic image.** Only lines the host writes are stored; everything else
   reads a deterministic pattern derived from the physical line. Values are therefore reproducible but
   not derived from an application.
8. **No RTL/SimX cross-check** (unchanged from M4): no RISC-V toolchain, no Docker, and this Vortex
   tree cannot build on macOS. Every `[V]` claim remains a reading of the source.
9. **AG utilisation is 0.7 %** because generation is memory-bound; the number is expected, not a
   defect, but it does mean `ag_lanes` is untested as a constraint at any value ≥ 1 for these
   workloads.
10. **One bulk workload family exists** (`E0`/`E1`, derived from the calibrated trace). The workload
    matrix was deliberately not expanded (milestone §8).

---

## 11. Recommended next milestone

**M6 — measure one optimisation at a time on this engine, starting with cache-line deduplication.**

The engine is a clean A/B baseline for exactly the mechanisms M5 left out: generation and coordination
are in place, FIFO issue is the reference policy, and every counter that a mechanism could move is
already exported. Proposal:

1. **M6a — duplicate detection / line deduplication** (`A4 → A4+D`). Key the request buffer on the
   target line, attach element ids to an existing request. Gate: with `ρ_dup = 0` the dedup arm must
   reproduce M5's E0 numbers **exactly** (same requests, same cycles, same hash) — that is the
   neutral-profile test for the mechanism, analogous to M4a.
2. **M6b — scheduling policy** (`A4 → A5`), one policy per run: bank/row-aware ordering derived only
   from addresses already in hand, then (separately, marked as an upper bound) an oracle policy that
   may read current bank state. Keep the same `R`, the same `ag_lanes`, the same index stream.
3. Keep both gates from this milestone green: the full suite, and M4a/M4b unchanged.
4. Report all comparisons at **equal residency** (`lsu_pending = 0` on the SIMT side), and state the
   index-stream cost of every arm.

Not recommended yet: bulk translation (M4's B-family) — it needs the engine's address visibility to be
worth measuring, and it changes the shared translation path rather than an engine-internal policy.
