# M6 — Bulk visibility window: implementation and measured results

**Status:** implemented in the standalone C++17 simulator, validated, and measured. The controlled A/B
methodology of M5.1 is preserved and every published M5.1 number reproduces exactly. Nine new test
groups; **3 846 checks, 0 failures**.

**Result in one line:** on the permutation workload (W1) the window with page ordering is **1.160×
faster than the controlled SIMT arm** (270 753 → 233 475 cycles) at W = 8 192, driven by DRAM row hits
(277 → 14 304) and dTLB misses (15 835 → 2 256) — with **more** traffic, not less. On the coalescible
stream (W0) it is **0.778×** (28 % slower): deduplication perfectly recovers SIMT's coalescing
(17 409 → 5 121 requests) but cannot pay for the index array SIMT never reads. Deduplication contributes
**exactly zero** on W1 and page ordering contributes **exactly zero** on W0, measured separately.

---

## 1. What was built

| File | Change |
|---|---|
| `src/sim.hpp` | `Config`: `batch_window`, `page_dir`, `org_lanes`, `fanout_lanes`, `batch_deadline`, `org_overlap`, `dedup`, `page_order`, `ab_charge_latch`. `BulkStats`: 20 window counters. `BulkEngine`: `WinEnt`/`LineReq`/`Batch`/`FanJob`, two window buffers, `stream_index`, `win_*` methods. `AbArm`: window columns. |
| `src/bulk.cpp` | The windowed organization path (below). The M5 FIFO path is **untouched** and selected by `batch_window = 0`. Index streaming extracted into `stream_index()` and shared by both paths so both charge identical index traffic. |
| `src/system.cpp` | `ab_charge_latch`: `per_core = R/cores - lanes`, plus `peak outstanding <= R` and `slot_pool_per_core`/`latch_charge` reporting. |
| `src/ab.cpp` | Window columns in the A/B table, window-aware target-request accounting, `ab_charge_latch` in the environment signature, `run_m6_sweep` / `print_m6_sweep`. |
| `src/main.cpp`, `src/util.cpp` | `experiment = m6 | m6_sweep`; all nine new keys parsed (unknown keys still throw). |
| `configs/ab_controlled.cfg` | Documented M6 block, defaults chosen to reproduce M5.1 (`batch_window = 0`, `ab_charge_latch = 0`). |
| `tests/tests.cpp` | 9 new groups (3 846 checks total). |

### 1.1 The architecture as implemented

```
index stream ──► address generation ──► window (W entries, D page slots)
   (shared)         (shared)                    │
                                                │  seal: line-ordered pass
                                                ▼
                            page directory + line mask + element chains
                                                │
                            issue: pages ascending, lines ascending
                                                ▼
                     shared MemorySystem (L1 → L2 → DRAM, same TLB, same R)
                                                │
                                                ▼
                            fanout: one fill → k elements (fanout_lanes/cycle)
```

A window holds **pending elements**; a slot holds an **issued line**. They are different resources and
are counted separately (§1.2). One line request is formed per *distinct line*; its fill retires every
element that depends on it.

### 1.2 The seven requested clarifications, as implemented

1. **Page grouping is the mechanism; row locality is a consequence of the map.** Nothing in the code
   mentions rows. `win_seal` builds `dir_mask` from `va_line & 63` and orders requests by `(vpn, line)`;
   it never assumes a page spans a fixed number of DRAM rows. The row effect appears only because
   `memory.cpp` derives `row = (line/32) % rows`. Changing `row_bytes` or `banks` changes the row
   outcome and cannot break the engine — verified by the `dram_locality`/`bank_hash` groups, which run
   on the same code. The report's row columns are therefore *observations*, not assumptions.
2. **W, R and D are independent.** `W` bounds window entries (`win_occ_max ≤ W`, checked), `R` bounds
   outstanding line requests (`peak_slot ≤ R`, checked), `D` bounds pages resident in one window
   (`dir_occ_max`, `dir_collisions`). Measured: at W = 256 the window holds 256 entries while only 128
   line requests are ever in flight; at W = 8 192, 8 192 entries are held with the same 128 slots. They
   are reported as three separate columns, never conflated.
3. **Dedup and page ordering switch independently.** `dedup` controls whether equal lines collapse into
   one `LineReq`; `page_order` controls only the issue order. The 2×2 ablation is in every table.
4. **Shared hierarchy preserved.** All requests go through `MemorySystem::submit()`; there is no private
   L1 and no bypass introduced. `mem_path = l1` still routes both arms through the same L1, and
   `bulk.cpp`'s existing check ("`mem_path = l1` but the engine produced no L1 traffic") still holds.
5. **Instrumentation.** Translation: `TLB misses`, `L2 TLB reads`, `page walks`, `PTE fetches`,
   `tlb0_mshr_stalls`, `replays`. DRAM: `row hits`, `row conflicts`, `dram_busy_cycles`,
   `dram_blocked_cycles`, `dram_starved_cycles`. Issue: `issue stalls`, `buffer-full stalls`. Latency:
   mean/p50/p95. Fanout: `fanout_cycles`, `fanout_elems`, `fanout_max_backlog`. Organization:
   `org_cycles`. Occupancy: `win_occ_max`, `win_total_max`, `dir_occ_max`, `peak_slot`, `win_bytes`.
6. **Exact reconstruction.** Every element is written to `val_`/`addr_` exactly once, from a copy of the
   64 B line taken at fill time (`line_bytes` returns scratch memory for unwritten lines, so the copy is
   mandatory). `addr_mismatch` re-derives every address from the index image. `test_m6_value_equivalence`
   compares all 2 048 elements' value **and** address against the per-element path: **0 missing, 0 bad**.
7. **Latch charge.** `ab_charge_latch = 1` gives A `per_core = R - lanes = 124`, so A's peak outstanding
   is **128**, exactly B's (measured: A 124 slots + 4 latch = 128; B 128). Default 0 preserves the
   published M5.1 configuration; with the flag on, A's cycles are unchanged (270 753) because A saturates
   at R ≈ 64 (M5.1 §7), so the correction changes the *claim*, not the result.

---

## 2. Window semantics and state machine

**Admission** (1 visit/entry): consume the next index value that has arrived, compute
`data_base + index * data_bytes`, append `{va_line, elem, off, width}` to the filling window. Bounded by
`W` and by the organizer's rate (`org_lanes`). Admission stalls (`win_full_stall`) when the organizer has
more than 2·`org_lanes` entries of work queued.

**Seal** (1 more visit/entry) on: window full, descriptor exhausted, or `batch_deadline` exceeded
(anti-starvation; measured `seals_full = 8`, `seals_end = 0`, `seals_deadline = 0` at W = 2 048, so the
deadline never fired — it exists for the 17-element descriptor case in the tests). Seal does three
things: sort entries by `va_line` (this *is* the deduplication), build one `LineReq` per distinct line
(or per entry when `dedup = 0`), and build the page directory with an exact 64-bit line mask per page.

**Issue**: walk `order` (pages ascending when `page_order = 1`, i.e. first-admission order otherwise),
take one slot per line, `submit()`. A refused submit leaves the line unissued and counts `issue_stall`.

**Completion**: fill → copy 64 B → fanout job → up to `fanout_lanes` elements per cycle → `val_`/`addr_`,
`retired++`. A window becomes reusable when `issue_pos == order.size() ∧ pending == 0 ∧ retired == n_ent`.

**Two buffers** (`org_overlap = 1`): the next window fills while the current one issues. With
`org_overlap = 0` nothing fills until the previous window has fully drained.

---

## 3. Acceptance criteria — all met

| Requirement | Evidence | Status |
|---|---|---|
| `batch_window = 0` reproduces M5.1's **278 034** cycles and **17 409** requests | `m6_legacy_identity`: A = 270 753, B = 278 034, B requests = 17 409, DRAM = 17 444, walks = 529 | **exact** |
| Disabling both mechanisms reproduces legacy behaviour | `m6_switch_off`: requests 2 177/2 177, DRAM 2 184/2 184, one request per element, `dedup_merged = 0`; cycles 33 718 → 34 500 (+2.3 %, the batching tail, stated in §7) | **met** |
| Mechanisms tested independently before combined | `m6_mechanism_independence`: dedup on W1 changes *nothing* (cycles identical to the digit), page order changes cycles and triples row hits, combined reproduces page order exactly | **met** |
| Sweep W = 128 … 16 384 at fixed R and shared parameters | `experiment=m6_sweep`, 7 × 4 cells × 2 arms, every cell `ok` | **met** |
| At least W0 and W1 | both, both latch settings | **met** |
| No lost/duplicated/misrouted results | `m6_value_equivalence`: 2 048 elements, 0 missing, 0 bad value, 0 bad address | **met** |
| Latch charge implemented, M5.1 reproducible | `m6_latch_charge` + default-off flag | **met** |
| Measured vs analytical clearly separated | §5–§6 | **met** |

Full suite: **3 846 checks, 0 failures**, 49 groups (40 pre-existing + 9 new), ~6.7 s, clean build with
`-Wall -Wextra` (0 warnings).

---

## 4. Measured results (all numbers from `./bin/dx100sim`, all `status: OK`)

Environment: `configs/ab_controlled.cfg`, `R = 128`, `mem_path = l1`, one cores, `lanes = 4`,
`elem_bytes = 4`, 2 banks / 4 096 rows, latches free unless stated. **A is the same controlled SIMT
baseline in every row** (270 753 on W1, 30 573 on W0).

### 4.1 W1 — permutation, 16 384 elements on 16 384 distinct lines (`x` = A/B speedup)

| W | none | x | dedup only | x | page order | x | **both** | **x** |
|---|---|---|---|---|---|---|---|---|
| 128 | 285 971 | 0.947 | 285 971 | 0.947 | 281 421 | 0.962 | 281 421 | 0.962 |
| 512 | 276 705 | 0.978 | 276 705 | 0.978 | 261 873 | 1.034 | 261 873 | 1.034 |
| 1 024 | 275 577 | 0.982 | 275 577 | 0.982 | 257 693 | **1.051** | 257 693 | 1.051 |
| 2 048 | 275 076 | 0.984 | 275 076 | 0.984 | 272 510 | 0.994 | 272 510 | 0.994 |
| 4 096 | 275 411 | 0.983 | 275 411 | 0.983 | 266 046 | 1.018 | 266 046 | 1.018 |
| 8 192 | 276 381 | 0.980 | 276 381 | 0.980 | **233 475** | **1.160** | **233 475** | **1.160** |
| 16 384 | 276 384 | 0.980 | 276 384 | 0.980 | 233 478 | 1.160 | 233 478 | 1.160 |

Read this table as three separate statements:

* **Deduplication is worthless here** and the measurement says so to the digit: no column differs from
  its neighbour, because `lines_issued = 16 384 = elements` and `dedup_merged = 0` at every W.
* **Page ordering is the mechanism**, and its value grows with W (0.962 → 1.160) because a window has to
  span more of the footprint before a page's accesses arrive together.
* **The curve is not monotone**: W = 2 048 (0.994) is worse than W = 1 024 (1.051) despite better row
  behaviour. This is a batch-granularity effect, not a mechanism failure — see §7.

### 4.2 W1 — what the window bought (both mechanisms on)

| W | lines | row hits | row conf | dTLB misses | walks | org cy | fanout cy | mean lat | p95 | issue stalls | window B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 128 | 16 384 | 1 931 | 15 511 | 16 267 | 530 | 8 192 | 16 376 | 722 | 1 477 | 95 390 | 18 432 |
| 512 | 16 384 | 4 332 | 13 110 | 12 364 | 534 | 8 192 | 16 359 | 1 194 | 1 715 | 220 011 | 24 576 |
| 1 024 | 16 384 | 6 802 | 10 640 | 9 062 | 534 | 8 192 | 16 362 | 1 192 | 1 581 | 240 204 | 32 768 |
| 2 048 | 16 384 | 10 078 | 7 364 | 5 323 | 539 | 8 192 | 16 361 | 1 053 | 1 402 | 266 374 | 49 152 |
| 4 096 | 16 384 | 13 263 | 4 179 | 2 812 | 535 | 8 192 | 16 370 | 838 | 1 215 | 263 775 | 81 920 |
| 8 192 | 16 384 | **14 304** | **3 138** | **2 256** | 529 | 8 192 | 16 379 | **643** | **1 259** | 228 995 | 147 456 |
| 16 384 | 16 384 | 14 303 | 3 139 | 2 256 | 529 | 8 192 | 16 379 | 643 | 1 259 | 228 995 | 278 528 |

Arm A for the same workload: **277 row hits / 16 417 reads**, **15 835 dTLB misses**, 512 walks,
mean latency 1 571, p95 1 920, 290 936 issue stalls.

Against A at W = 8 192: **row hits ×51.6** (277 → 14 304), **row conflicts ÷5.1** (16 138 → 3 138),
**dTLB misses ÷7.0** (15 835 → 2 256), **L2 TLB reads ÷10.2** (15 807 → 1 552), **mean latency ÷2.4**
(1 571 → 643), **issue stalls ÷1.27**, while traffic **rises** 6.3 % (16 417 → 17 444 DRAM reads) and
walks rise by exactly 17 (512 → 529: 16 index pages + 1 descriptor page).

### 4.3 Detail at W = 8 192 (W1) — full A/B

| metric | A | B | B/A |
|---|---|---|---|
| cycles | 270 753 | **233 475** | **86.2 %** |
| line requests | 16 384 | 17 409 | 106.3 % |
| DRAM reads | 16 417 | 17 444 | 106.3 % |
| DRAM row hits / conflicts | 277 / 16 138 | 14 304 / 3 138 | 51.6× / 19.4 % |
| dTLB misses | 15 835 | 2 256 | 14.3 % |
| page walks | 512 | 529 | 103.3 % |
| mean / p50 / p95 latency | 1 571 / 1 665 / 1 920 | 643 / 524 / 1 259 | 40.9 / 31.5 / 65.6 % |
| L1 stall cycles | 22 056 296 | 8 429 163 | 38.2 % |
| TLB MSHR stalls | 290 936 | 159 874 | 55.0 % |
| peak unresolved / slots | 132 (128+4) | 128 | — |
| windows sealed / pages resident | — | 3 / 512 | — |

### 4.4 W0 — coalescible stream (κ = 1, 16 384 elements, 4 096 lines)

| W | none | x | dedup only | x | page order | x | both | x |
|---|---|---|---|---|---|---|---|---|
| 128 | 57 326 | 0.533 | 56 558 | 0.541 | 57 326 | 0.533 | 56 558 | 0.541 |
| 512 | 49 726 | 0.615 | 46 128 | 0.663 | 49 726 | 0.615 | 46 128 | 0.663 |
| 1 024 | 46 984 | 0.651 | 42 000 | 0.728 | 46 984 | 0.651 | 42 000 | 0.728 |
| 2 048 | 45 766 | 0.668 | 40 302 | 0.759 | 45 766 | 0.668 | 40 302 | 0.759 |
| 4 096 | 45 370 | 0.674 | 39 601 | 0.772 | 45 370 | 0.674 | 39 601 | 0.772 |
| 8 192 | 45 106 | 0.678 | **39 322** | **0.778** | 45 106 | 0.678 | **39 322** | **0.778** |
| 16 384 | 45 106 | 0.678 | 39 322 | 0.778 | 45 106 | 0.678 | 39 322 | 0.778 |

The mirror image of W1: **page ordering changes nothing** (identical to the digit at every W, because a
sequential stream is already in page order) and **dedup is the whole effect** (57 326 → 39 322).
At W = 8 192 dedup issues **4 096 target requests for 16 384 elements** — exactly SIMT's coalescing,
`dedup_merged = 12 288`, `L1 MSHR joins` 12 288 → **0**. Requests 17 409 → **5 121**
(4 096 targets + 1 024 index + 1 descriptor).

B still loses (0.778) because DRAM reads rise 25 % (4 101 → 5 128) — the index array plus descriptor are
traffic the SIMT arm never generates — and the measured ratio is almost exactly the traffic ratio
(1.25) with a slight latency gain (826 → 775). **No organization mechanism can remove that cost.**

---

## 5. Measured versus projected

The investigation (§3.3) projected speedups from the model's DRAM rules. They were wrong in a specific
and explainable way:

| W | projected | measured | Δ |
|---|---|---|---|
| 128 | 1.03 | 0.962 | −0.07 |
| 512 | 1.10 | 1.034 | −0.07 |
| 2 048 | 1.35 | 0.994 | −0.36 |
| 4 096 | 1.53 | 1.018 | −0.51 |
| 8 192 | 1.68 | 1.160 | −0.52 |
| 16 384 | 1.76 | 1.160 | −0.60 |

**The row behaviour itself was predicted correctly** (87.5 % row hits projected at W = 8 192, 87.5 %
measured). What the projection got wrong was the *conversion into cycles*: it assumed the run would become
DRAM-makespan-bound at A's 78.6 % busy fraction. It does not. At W = 8 192 the engine sustains only
≈48 requests in flight on average (17 444 lines / 233 475 cycles × 643 cycles mean latency), against 128
slots, with 228 995 refused submissions — the hierarchy, not the slot budget, is the limit. Reducing DRAM
service time therefore removes less than the DRAM share of the run.

The lesson for the remaining milestones is the one the milestone asked to establish: **a row-hit metric
is not a performance result.** The 1.160× is real and reproducible; the 1.68× was arithmetic.

---

## 6. Fair-comparison statement

* Both arms: same `Config`, same materialised `Trace`, same memory image (hash compared), `R = 128`,
  `mem_path = l1`, same 16-entry dTLB with 4 MSHRs / 2 parked, same L2 TLB, walkers, L1, L2 and DRAM.
* With `ab_charge_latch = 1`, A's pool is 124 slots + ≤4 latch = **128 outstanding**, and B's is 128.
  Measured: `peak outstanding` A = 128, B = 128, and the speedup is unchanged (1.1597 vs 1.1600), because
  A saturates below R = 124 anyway. The correction removes the objection without moving the result.
* B's **extra** resource is the window, disclosed in every table: `W·8 + D·16` bytes per buffer
  (18 KiB at W = 128 … 272 KiB at W = 16 384 with two buffers), peak occupancy and page residency
  reported as separate columns. It is never counted as residency and never claimed to be free.

---

## 7. What the data says, and what it does not

1. **Translation, not DRAM, is the first-order effect on W1.** Page ordering cuts dTLB misses 7×
   (15 835 → 2 256) and L2 TLB reads 10×; DRAM row hits improve 51×. Latency falls 2.4×. The cycle gain
   (1.16×) is smaller than any of those individually, which is the honest summary: **page grouping
   removes two bottlenecks at once and still lands at 1.16×** because the shared hierarchy's MSHRs and
   the batch pipeline bound what converts.
2. **Deduplication and page ordering are genuinely orthogonal**, demonstrated by two exact zeros: dedup
   on a permutation (W1) and page order on a stream (W0) each change the cycle count by nothing at all.
3. **The W = 2 048 dip is a batch-granularity artefact, not a mechanism failure.** Row hits are monotone
   in W while cycles are not, so the loss must come from the seal/issue/refill pipeline: `issue stalls`
   peaks at that W (266 374) and mean latency rises with occupancy. The current design seals on a global
   element count; sealing on a *page cohort* instead would decouple the two and is the obvious next
   change (§9). Reported as measured, not smoothed.
4. **W0 is a structural loss.** Even with perfect dedup the engine reads 1 025 lines SIMT does not need,
   and 0.778 × 1.25 ≈ 0.97 says the rest of the loss is that traffic. A bulk engine cannot win a
   workload where the SIMT coalescer is already optimal; any claim otherwise would be measuring the
   wrong thing.
5. **The window size is the architecture.** 1.03× at W = 1 024 vs 1.16× at W = 8 192 on the same
   mechanism: 8 KiB versus 128 KiB of window SRAM decides whether this works.

---

## 8. Limitations

1. **One workload family, one descriptor.** No multi-descriptor pipelining; a descriptor's `count` bounds
   a window, so W = 16 384 is only reachable because W1 has 16 384 elements.
2. **No masks, no write path, no LMEM writeback** — unchanged from M5/M5.1; B's cost stays understated.
3. **No virtual aliases exist in the model** (`ppn = vpn + 0x4000`), so alias handling is untested. The
   design does not need it: a 64 B line cannot straddle a page, so VA-line identity is PA-line identity.
4. **The address map is offset-invariant** here, so VA-derived and PA-derived grouping are
   indistinguishable. Nothing in the implementation depends on that (§1.2 item 1), but the *evidence*
   cannot separate them.
5. **Organization cost is a rate model** (`org_lanes = 4`, two visits per entry, measured exactly:
   8 192 cycles for 16 384 entries). It is a stated cost, not a synthesis result.
6. **The window's area is disclosed, not optimised**: 18 KiB → 272 KiB depending on W.
7. **`org_overlap = 0` was not swept.** The serialised variant exists and is implemented, but the
   fill/issue interaction is only characterised at `org_overlap = 1`.
8. **W0's loss is configuration-specific** to `P = 256, κ = 1`; a stream whose index array is already
   resident in cache would cost less, and that was not measured.

---

## 9. Suggested next step (not done — M7/M8 were explicitly out of scope)

The measurement points at one change: **seal on a page cohort rather than on a global element count**,
i.e. issue a page as soon as it has k lines buffered instead of waiting for W entries. That directly
attacks the W = 2 048 dip and the 48-of-128 occupancy, and it is a change to `win_seal`'s trigger only —
no new mechanisms, no new resources.

---

## 10. Reproduction

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests          # 3 846 checks, 0 failures

# paired A/B, legacy path (must print 270753 / 278034)
./bin/dx100sim --config configs/ab_controlled.cfg

# paired A/B with the window
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=m6 --set batch_window=8192

# full sweep: W x {dedup, page_order}, both workloads
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=m6_sweep
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=m6_sweep \
    --set wl=W0 --set kappa=1 --set P=256

# latch-charged (A pool = R - lanes)
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=m6_sweep --set ab_charge_latch=1

# ablations on a single cell
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=m6 --set batch_window=8192 \
    --set dedup=0 --set page_order=0
```
