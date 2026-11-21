# M6 — Bulk memory organization: architecture investigation

**Scope:** design investigation only. No source file was modified and no experiment result was
re-run for this report; every number quoted is either a measurement already published in
`m5.1-controlled-ab-report.md` / `m5-bulk-engine-implementation-report.md`, or arithmetic derived from
the simulator's own parameters (`configs/ab_controlled.cfg`) and marked *(derived)*.

**Answer to the research question, up front.**

> Can a dedicated bulk indirect-memory engine use visibility across a large window of independent
> accesses to organize memory transactions more efficiently than conventional SIMT execution?

**Yes, but not through deduplication — and not at the window sizes M5.1 tested.** The source and the
M5.1 counters say three things clearly:

1. **Line deduplication is the wrong first mechanism.** On W1 (the only workload with bulk-vision
   upside) there is nothing to deduplicate: the trace is a permutation, 16 384 elements on 16 384
   distinct lines. Measured L1 MSHR joins: **0** on W1 vs **12 288** on W0 — and on W0 the SIMT
   coalescer (`system.cpp:176`) is already perfect (4 096 requests for 16 384 elements), so dedup only
   repairs a deficit the engine itself creates. Dedup is a *cost-avoidance* mechanism, not a win.
2. **The mechanism with real upside is page-granular organization**, because in this system a 4 KiB
   page is exactly four DRAM rows, so issuing a page's lines in ascending order converts row conflicts
   into row hits *and* collapses dTLB misses at the same time. W1's measured profile is the ideal
   target: **98.3 % of DRAM accesses are row conflicts** (16 138 / 16 417) and **96.6 % of dTLB probes
   miss** (15 835 / 16 384), with the 4-entry dTLB MSHR pool (`memory.cpp:129`) accounting for the
   entire 290 936-cycle issue-stall count.
3. **The window has to be in the thousands, not 128, and the win is a curve, not a switch.** Reordering
   by page turns W1's 98.3 % row-conflict stream into 93.8 % row hits, but the hit rate is
   `1 − 1 024/W` (1 024 = the footprint's occupied `(bank,row)` slots): **1.03× at W = 128** (M5.1's
   regime, i.e. nothing), **1.35× at W = 2 048**, **1.68× at W = 8 192**. A page's 32 accesses are
   scattered over a mean span of 15 546 of the 16 384 index positions, so full page grouping needs a
   window the size of the whole workload. *Bulk* must mean bulk — and the honest target is 1.35×, not
   the 1.76× ceiling.

A projection from the simulator's own DRAM rules puts a page-organized engine at **1.35×** SIMT on W1 at
W = 2 048 and 1.68–1.76× only at W ≥ 8 192, against **≈0.8×** (25 % slower) on W0 — §3.3, with the method
and its limits stated there. Both are falsifiable, and the W0 number is the one that keeps the whole
programme honest.

---

## 1. Current M5 architectural limitations, with exact source references

The M5 engine is a correct **request generator**: it fetches a descriptor, streams the index array,
derives one target address per element, holds R residency slots, and collects fills. It has no
mechanism that looks at more than one address at a time, and no structure that could hold more than R.

| # | Limitation | Evidence |
|---|---|---|
| L1 | **One request per element, unconditionally.** There is no grouping step between address generation and submission. | `bulk.cpp:367` `alloc_and_enqueue(ReqKind::Target, addr >> 6, elem, chunk)` — one `Pending` per element |
| L2 | **FIFO, single address at a time.** Domination is `unissued_`, a plain deque appended in generation order. | `sim.hpp:455`, `bulk.cpp:167` |
| L3 | **The only storage is the R slot array.** `info_` is `assign(R, Pending{})` and `free_` holds exactly R ids; there is no window, queue or batch above it. Measured `bulk_buf_max = 128` = R. | `bulk.cpp:135-139`, `bulk.cpp:161` |
| L4 | **No element→line dependency tracking.** Each element is an independent request, so a line that 16 elements share is fetched 16 times and 15 of those are absorbed downstream. Measured: 12 288 L1 MSHR joins on W0. | `bulk.cpp:367`, `memory.cpp:391` (`l1.mshr_joins++`) |
| L5 | **Order is never chosen.** `issue()` pops the FIFO and counts a violation if target elements are not monotonic. | `bulk.cpp:388-405`, `bulk_st_issue_monotonic_violations` |
| L6 | **Issue is blind to translation state.** `submit()` fuses the TLB probe with admission, so the front-end cannot express "this page is already resident". | `memory.cpp:468` `if (!tlb_probe(req.core, req.vpn, cycle, ready)) return false;` |
| L7 | **Result distribution is per element, not per line.** Each fill re-reads the 64 B line and extracts one element. | `bulk.cpp:262-268` (`on_target`: `val_[p.elem] = load_le(...)`) |
| L8 | **Physical identity is unobtainable without probing.** `pa_line_of_va()` is a pure function, but nothing in the engine's control flow ties a PA decision to a completed translation. | `sim.hpp:250-256`, `memory.cpp:100-105` |
| L9 | **The engine is one TLB client** (`r.core = 0`) using the SIMT core's 16-entry dTLB, i.e. 96.6 % miss on W1 — every probe allocates an MSHR in a 4-entry pool that parks at 2. | `bulk.cpp:392-394`, `memory.cpp:117-145` |
| L10 | **No masking.** Neither `Access` nor `BulkDesc` has a lane mask, so "preserve masks" is unimplemented and untestable. | `sim.hpp:196-203`, `sim.hpp:352-368` |

M5.1 then measured what that generator does when the environment is fair: **W1 270 753 → 278 034 cy
(−2.7 %), W0 30 573 → 44 773 cy (−46.4 %)**, 17 409 requests against A's 4 096 on W0 (4.25×), with
12 288 of the excess absorbed by L1 MSHR joins and the DRAM traffic ratio 1.063 / 1.250.

**What is actually scarce, from the same run (W1, l1 path, R = 128):**

| Resource | A (SIMT) | B (M5 FIFO) | Reading |
|---|---|---|---|
| DRAM row conflicts / reads | 16 138 / 16 417 (98.3 %) | 16 212 / 17 444 (92.9 %) | the dominant cost is *row misses*, not bandwidth |
| dTLB misses / probes | 15 835 / 16 384 (96.6 %) | 15 860 / 17 409 (91.1 %) | 512 pages vs 16 entries |
| issue stalls = dTLB MSHR stalls | 290 936 | 77 818 | the 4-entry pool is the serialisation point |
| page walks | 512 | 529 | the L2 TLB (512 entries) holds the whole page set |
| L1 MSHR joins | 0 | 0 | nothing to dedup on W1 |
| peak resident | 132 (128 + 4 latch) | 128 | M5.1 §4 |

So the bottleneck order on W1 is: **DRAM row misses → dTLB MSHR pool → everything else.** Neither is
addressed by deduplication. Both are addressed by *page order*.

---

## 2. Feasible architecture alternatives

Three concrete designs, assessed against the interfaces that actually exist. The decisive geometric
fact, used by all three, is derived in §5.1: **a 64 B line never straddles a 4 KiB page**
(`LINE_BYTES = 64`, `PAGE_LINES = 64`, `memory.cpp:11-12`), and the engine's `vpn` is
`va_line >> 6` (`bulk.cpp:392`). Therefore *two elements on the same VA line are necessarily on the
same physical line*, and line-level dedup on virtual addresses is exact without any translation.
Aliasing can only affect page-granular identity, which a gather never needs — a line is the unit.

### Option A — Batch window + counting sort by page, then line bitmap per page

*Window → sort by page → 64-bit line bitmap per page → issue pages in order, bits in order.*

| Aspect | Design |
|---|---|
| Data structures | Ring of `W` element entries `{elem u32, off u8, width u8, pad u16}` (8 B); per-page `{u64 line_mask, u32 first_chain, u32 n_elems}` (16 B); chain array of element indices sorted by `(page, line)`. |
| Organization | When the window is sealed: counting sort of the W entries by page id (2 passes over W), then a pass per page bucketing by `va_line & 63`. |
| Cost | sort 2W entry-visits at `org_lanes`/cycle → `2W/org_lanes` cycles per batch; at W = 2 048, `org_lanes` = 4 → 1 024 cycles per batch, 8 batches → 8 192 cycles (**3.0 %** of A's 270 753). Peak SRAM ≈ 8W B + 16D B; at W = 2 048, D = 512 → 16 KiB + 8 KiB = 24 KiB. |
| Translation | Unchanged: one probe per issued line, but issued page-ordered, so the dTLB sees each page's cohort consecutively. |
| Dedup | Falls out of the bitmap (bit test/set). |
| Fanout | Chain segment per (page, line); one fill retires `n_elems` elements. |
| Interface change | **None.** |
| Weakness | The sort is pure overhead (3 %), and the batch is sealed on a *count*, not on a page's cohort being worth issuing. |

### Option B — Page-directory window (recommended)

*The window **is** the page directory: each address is inserted into its page's slot in O(1); issue walks
the directory in page order.*

| Aspect | Design |
|---|---|
| Data structures | `PageSlot { u64 line_mask; u32 chain_head; u16 n_lines; u16 n_elems; }` (16 B), `D` slots, addressed by `vpn mod D` (direct-mapped, with a `vpn` tag for validation); element entries `{elem,off,width,pad}` (8 B) in `W`-entry pool, chained per `(page,line)`; `chain_next[u32]` (4 B/entry). |
| Organization | `O(1)` per address: probe the slot, set the bit, push the element onto its chain. **No sort, no associative search, no batch.** |
| Cost | 1 directory access per address, `org_lanes` = 4/cycle → 16 384 addresses = **4 096 cycles (1.5 %)**. Issue pass: one scan of `D` slots, `D` cycles, overlapped with issuing. Peak SRAM ≈ 8W + 16D B (the entry already carries its chain link); W = 4 096, D = 512 → 32 KiB + 8 KiB = 40 KiB. |
| Translation | One probe per issued line; pages issued in ascending order → the dTLB sees each page's cohort consecutively. |
| Dedup | Free (bit test before setting). |
| Fanout | Per-slot, per-line chain segments. |
| Interface change | **None** for grouping/issue/fanout; one *optional* accessor (§9.3) to derive (bank,row) from the hierarchy's own map instead of duplicating the hash in the engine. |
| Completeness | A 4 KiB page has exactly 64 lines, so a 64-bit mask per page is an **exact** — not approximate — set of the lines touched in that page. Nothing is lost, nothing is merged that should not be. |
| Weakness | Direct-mapped directory: a page collision evicts an unissued cohort, which must then be flushed (issued immediately, in effect as a partial batch). Bounded by `D`; W1's whole page set is 512 pages, so `D = 512` holds everything. |

### Option C — Translation-first bulk window (translate, then group by PA)

*Submit every address immediately to obtain its translation, hold the translated-but-unissued requests,
group by physical line/row, then issue.*

| Aspect | Design |
|---|---|
| Data structures | A second residency class: `T` translated-pending entries holding `{pa_line, elem, off, width}` plus the line bitmap. |
| Cost | Requires splitting translation from admission in `MemorySystem` (new interface), a second budget `T`, and re-deriving the residency rule (a translated request holds two resources). |
| Benefit over B | Only observable if VA-order and PA-order differ: **they cannot differ here** (§5.2: `pa_line = va_line + 1 048 576` exactly, and the (bank,row) derivation is invariant to that offset). |
| Verdict | **Rejected.** It buys nothing this model can show, and it costs the one thing the model gets right — a single, simple residency rule. It is the right design for a system with a real page colouring / alias map; it is not the right design to *build* now. |

### Comparison

| | A (sort) | **B (page directory)** | C (translate-first) |
|---|---|---|---|
| Organization cost per address | 2 visits (sort) | **1 visit** | 2 (probe + insert) |
| Total organization cycles (W1) | ~8 192 (3.0 %) | **~4 096 (1.5 %)** | ≥ 16 384 (6.1 %) |
| Dedup | bitmap | **bitmap** | bitmap (PA) |
| Page ordering | after sort | **intrinsic** | after sort |
| Interface change | none | **none** | new translate/issue split |
| New residency rule | no | **no** | yes (two classes) |
| Alias-safe | yes (VA lines) | **yes (VA lines)** | yes (PA) but unobservable here |
| Risk | throughput of the sort | directory collisions (`D`) | residency-rule complexity |

---

## 3. Recommended architecture and justification

### 3.1 The architecture

**Option B: a page-directory bulk window, double-buffered, cohort-triggered issue, wide fanout.**
Six stages, matching the intended block diagram, with the *window* and the *organizer* fused into one
structure because a page-indexed bitmap is simultaneously the buffer and the index:

```
index stream ──► address generation ──► page-directory window ──► issue (page order,
 (bulk.cpp:318)     (bulk.cpp:355-372)     (NEW: 64-bit line mask      line order within page)
                                            per page + element chain)        │
                                                                             ▼
                                              fanout ◄──── fills ◄──── shared MemorySystem
                                        (NEW: 1 fill → k elements)      (l1 path, unchanged)
```

The three mechanisms the milestone names map onto this as follows:

* **A. Cache-line coalescing / dedup** — the bitmap's bit test. Exact, free, and *not* the source of the
  win (§3.3).
* **B. Translation-aware organization** — issuing pages in ascending order. The dTLB is 16 entries and
  the L2 TLB holds all 512 pages, so page order converts ~15 835 dTLB misses into (at best) one per
  page per pass. This is the largest single effect available.
* **C. Memory-aware scheduling** — **subsumed, not separate.** A 4 KiB page spans exactly two DRAM row
  numbers (`row_bytes` = 1 024 B = 16 lines → 64 lines = 2 rows of 16), so ascending line order within a
  page is already ascending row order: **2 row misses + 30 row hits per page**, with no row state at all.
  Cross-page row reuse is *zero* in this map (§3.2), so a separate row scheduler has nothing to schedule.

### 3.2 Why page order is row order here — the arithmetic

From `ab_controlled.cfg` and `memory.cpp:412-425`:

```
lines_per_row = row_bytes / LINE_BYTES      = 1024 / 64      = 16
row(line)     = ((line / banks) / 16) % rows = (line / 32) % 4096
bank(line)    = (line ^ (line>>6) ^ (line>>12)) % 2
```

A 4 KiB page is 64 consecutive lines, `va_line = 64·vpn + j`, `j ∈ [0,64)`. Therefore
`line/32 = 2·vpn + j/16`, so **page `vpn` occupies exactly row numbers `2·vpn` and `2·vpn+1`**, 16 of its
lines in each, while page `vpn+1` occupies `2·vpn+2, 2·vpn+3` — **no cross-page row sharing exists**.

Verified directly against the map for W1's actual line set (512 pages × 32 lines, stride 2):

| Property | Value |
|---|---|
| footprint lines | 16 384 distinct, one access each |
| row numbers touched | **1 024** — exactly 2 per page |
| `(bank,row)` slots touched | 1 024, holding **16 lines each** (a page's 32 lines split 16/16 across its two rows) |
| row numbers shared between pages | **0** |

Consequences:

* Ascending `j` within a page is ascending row order: issuing a page's 32 lines in address order yields
  **2 row misses + 30 row hits per page** ⇒ 1 024 misses + 15 360 hits over the workload, with no row
  state whatsoever.
* A separate row scheduler adds nothing: a row's 16 lines belong to exactly one page, so sorting by row
  and sorting by page produce the same runs. **M8's mechanism is a property of M6's page order**, and
  the row experiment collapses to "did we sort within the page" — one bit in the issue walk.

This is a property of this address map (linear, `bank_hash` folds only the bank index, `row_bytes` 1 024,
4 KiB pages). It must be re-derived if any of those change; §11 records that as a live dependency.

### 3.3 Predicted performance (projections, not measurements)

**Method.** Take the W1 line sequence (a uniform permutation of the 16 384-line footprint), reorder it by
the policy under test, and walk it through the model's own bank/row/timing rules (`memory.cpp:412-425`,
`t_hit = 14`, `t_miss = 26`, 2 banks, one command/cycle). That yields the DRAM **makespan**, a lower bound
on memory time. It is then divided by the DRAM busy fraction A actually achieved
(212 728 / 270 753 = **78.6 %**) to project total cycles. The FIFO row reproduces A's regime (0.3 % row
hits vs A's measured 1.7 % — A's extra hits are PTE reads and instruction interleaving; both are "no row
locality"), which is the check that the machinery is faithful.

| Policy | W | row hits | row misses | hit % | DRAM makespan | projected total | **vs A** |
|---|---|---|---|---|---|---|---|
| FIFO (M5.1 regime) | — | 44 | 16 340 | 0.3 % | 212 728 | 270 700 | 1.00 |
| dedup only, no reorder | any | 44 | 16 340 | 0.3 % | 212 728 | 270 700 | **1.00** — nothing to dedup |
| page order | 128 | 953 | 15 431 | 5.8 % | 207 400 | 263 900 | 1.03 |
| page order | 512 | 3 311 | 13 073 | 20.2 % | 193 252 | 245 900 | 1.10 |
| page order | 2 048 | 9 148 | 7 236 | 55.8 % | 158 152 | 201 200 | **1.35** |
| page order | 4 096 | 12 331 | 4 053 | 75.3 % | 139 024 | 176 900 | 1.53 |
| page order | 8 192 | 14 336 | 2 048 | 87.5 % | 126 976 | 161 500 | 1.68 |
| page order | 16 384 (whole) | 15 360 | 1 024 | 93.8 % | 120 832 | 153 700 | 1.76 |

The curve is exactly `hit% ≈ 1 − 1 024/W`: the mechanism is a function of how many of the footprint's
1 024 rows a window can bring together, and nothing else. The index stream adds 1 024 sequential lines
(≈7 200 cycles of bank time, ≈+6 %) which these totals **exclude**; add it before quoting.

Two limits matter more than the upside:

* **W = 128 buys nothing (1.03×).** M5.1's residency budget cannot exploit bulk visibility at all.
* **Full page grouping needs a window the size of the workload.** A page's 32 accesses are scattered
  uniformly through the permutation — measured mean span **15 546 of 16 384** positions. So 16 384 is not
  a tuning knob, it is the *requirement* for the 1.76× row. At the implementable W = 2 048 the honest
  expectation is **1.35×**.

**W0 is the counter-case and must be reported as a loss.** A already coalesces 4:1 and already achieves
93.7 % row hits (3 842 / 4 101), so no reordering can improve its DRAM side, while the engine still reads
1 024 index lines + 1 descriptor that the SIMT arm never touches (+25 % DRAM traffic). Projected
**≈0.8×**, unchanged by any mechanism in M6.

---

## 4. Data structures and state machine

### 4.1 Structures

| Structure | Field | Bits | Purpose |
|---|---|---|---|
| `WinEntry` | `elem` | 32 | element id (descriptor `elem_base + i`) |
| | `off` | 6 | byte offset in line (element never straddles: §5.3) |
| | `width` | 2 | 1/2/4/8 B, encoded |
| | `next` | 24 | chain link into `WinEntry` pool (`NONE` = end) |
| `PageSlot` (×`D`) | `tag` | 19 | `vpn` (33-bit VA space → 2²⁰ pages; 19 bits covers `D`-windowed aliasing check) |
| | `line_mask` | 64 | **exact** set of lines touched in this page |
| | `n_elems` | 16 | elements accumulated for this page (backpressure/cohort trigger) |
| | `head[8]` | 8×? | *alternative to chains*: one head per 8-line group (8 B/group, 64 B/slot) bounds lookup to 8 chains — see §11.2 |
| `Batch` | `state`, `pageno_cursor`, `bit_cursor`, `issued`, `retired`, `sealed_at` | — | issue walk state |

**Storage (per window).** `WinEntry` 8 B × W (the 32/6/2/24-bit fields pack into one 64-bit word),
`PageSlot` 16 B × D, plus a `batch_done` bitmap of W bits for retirement. At W = 4 096, D = 512:
**32 KiB + 8 KiB + 0.5 KiB ≈ 41 KiB**. At W = 2 048, D = 512: ≈ 24 KiB. Both are plausible L2-adjacent
SRAM blocks and both are **reported as a resource cost** in
every comparison (§8.3), because the SIMT arm has nothing comparable.

**Chain vs per-line-group heads (§11.2, open):** a chain through the entry pool is cheapest in area but
makes fanout a pointer chase (worst case 64 hops per line, and a line can hold up to 16 same-line
elements on a 4 B gather). A 64-entry head array per page (one per line) is 256 B/page — too much for
`D = 512` (128 KiB). The compromise worth implementing first is **8 heads per page** (one per 8-line
group, 32 B/page), which bounds the chase to 8 and is exact for the *common* case (an instruction hits
one or two lines). This is deferred to §11.2, not decided.

### 4.2 State machine (per window/batch)

```
          fill (index stream)                seal                  drain
IDLE ──────────────────► FILLING ──────────────────► ISSUING ──────────────────► IDLE
                            │  ▲                        │
              cohort/deadline│  │refill (double-buffered)│
                            ▼  │                        ▼
                          SEALED ──────────────────► DRAINING
```

| State | Entry condition | Actions | Exit |
|---|---|---|---|
| `FILLING` | window empty or refill started | AG consumes index values, inserts into `PageSlot` (1/cycle), counts `W` | `W` reached → `SEALED`; descriptor end → `SEALED`; `age > batch_deadline` → `SEALED` |
| `SEALED` | window closed | one directory scan computes the issue order (page ascending); no data movement | immediately → `ISSUING` |
| `ISSUING` | order known | walk pages ascending, bits ascending; `submit()` one line per set bit; a line holds **one** R slot from issue to fill | all lines of the batch issued |
| `DRAINING` | last line issued | fanout on fills; retire elements | `retired == elements in batch` |
| `IDLE` | — | accept next descriptor | — |

`FILLING` of batch *k+1* starts as soon as batch *k* is `SEALED` (double buffer). Backpressure: if the
fill side is full while the issue side is not draining, the AG stalls (`idx_wait_stall` analogue); if the
issue side cannot get a slot, `buf_full_stall` counts, exactly as M5 does today.

**Anti-starvation:** the deadline is mandatory, not optional. Without it a descriptor whose remaining
elements never fill a window would never issue — the same class of bug that `idx_window = 0` avoids in
M5. Deadline = `batch_deadline` × A's mean latency (initial value 4 096 cycles ≈ 3 p95 latencies).

---

## 5. Address translation and cache-line grouping strategy

### 5.1 When to group by cache line — and why VA is sufficient

**Group at address-generation time, on the virtual line number.** Proof that this is exact:

1. `LINE_BYTES = 64`, `PAGE_LINES = 64` (`memory.cpp:11-12`), so `4 096 % 64 == 0`: no line crosses a
   page boundary.
2. The engine's `vpn = va_line >> 6` (`bulk.cpp:392`) is exactly the page number of that line.
3. `pa_line_of(ppn, line)` = `(ppn << 6) | (line & 63)` (`memory.cpp:19-23`), i.e. the line's offset inside
   the page is preserved and the page is replaced wholesale.
4. Therefore `va_line₁ == va_line₂ ⟹ vpn₁ == vpn₂ ∧ pa_line₁ == pa_line₂`.

So grouping on VA lines produces exactly the PA grouping, with **zero aliasing hazard**, before any
translation. This is not an approximation and not a modelling artefact — it is a property of any system
whose page size is a multiple of its line size. The consequence is architectural: *the engine does not
need translation to dedup lines*, and therefore does not need a new interface to do it (contrast
Option C).

**Where PA is genuinely required — and where it is not.** PA is required only for:
(a) *page-granular* aliases (two VAs → one PA page), which a gather never needs because a line is the
unit and one line lives in one page; and (b) row/bank scheduling, because `row` is derived from the
*physical* line number. Case (b) is where §5.2 bites.

### 5.2 Virtual aliases, and the one place this model is too kind

`ppn = vpn + PPN_OFFSET` (`memory.cpp:19`, `memory.cpp:160`), so **`pa_line = va_line + 1 048 576`**
exactly, and the mapping is injective: **the simulator has no virtual aliases at all.** Two consequences
the report must state plainly:

* Alias handling is **unimplemented and untestable** here. A design that claims alias support cannot be
  validated in this simulator; §11.4 records it as an evidence gap, and the interface (§9) must carry
  the PA any future alias logic would need.
* Worse for *row* scheduling: because the offset is `2²⁰`, and `2²⁰` is a multiple of the full row period
  (`4 096 rows × 32 lines = 131 072` lines; `1 048 576 = 8 × 131 072`), and because the bank hash folds
  only bits ≥ 6 of a value whose low bit is unchanged by `+2²⁰`, **`bank(PA) = bank(VA)` and
  `row(PA) = row(VA)`**. A VA-derived row schedule would therefore score identically to a PA-derived one
  *in this simulator*, and the difference would only appear on a real page colouring.

This is a genuine limitation of the evidence, and the reason the recommended mechanism is **page order**
(which needs no row at all, §3.2) rather than row order (which needs a PA the model cannot honestly
distinguish). If M8 implements row scheduling, it must (i) derive `(bank,row)` from the hierarchy's own
map via the new accessor of §9.3, and (ii) be validated on a configuration where `PPN_OFFSET` is not
row-period-aligned, otherwise the experiment is vacuous.

### 5.3 Element widths and partial lines

* `BulkDesc::validate()` (`bulk.cpp:99-101`) requires `64 % data_bytes == 0`, so an element never
  straddles a line: widths 1, 2, 4, 8 all divide 64. `off` therefore identifies the element uniquely
  within its line.
* `(off, width)` is stored **per element**, never per line — the same line may serve elements of
  different widths at different offsets (the descriptor has one width for all elements, but the
  structure must not bake that in; aliasing two different descriptors into one window would otherwise
  be silent corruption).
* Partial-line accesses exist only as *sectors* in this model: a fill always delivers 64 B
  (`line_bytes()`), and there is no write path, so a partially valid line cannot be observed. The
  fanout only ever reads the bytes it needs.
* **Masks are absent** (L10). A masked-off lane would have to be represented as an element with
  `off`/`width` and a valid bit; `WinEntry` has 6 spare bits (§4.1) — reserve one now, so adding masks
  later does not change the layout.

---

## 6. Request scheduling and bounded-window refill policy

### 6.1 Issue order inside a batch

1. **Pages ascending** (`vpn`), which for a direct-mapped directory means ascending slot scan with the
   `tag` check.
2. **Lines ascending within the page** (bit 0 → 63 of `line_mask`), which is ascending address and
   therefore ascending row (§3.2).
3. **No reordering across batches**; batches are issued in creation order, so an element's latency is
   bounded by one batch.

This order is chosen for three simultaneous effects: dTLB hits (a page's cohort is consecutive), DRAM
row hits (ascending lines within four rows), and deterministic replay (no timing-dependent order).

### 6.2 Refill policy

| Parameter | Meaning | Initial value | Rationale |
|---|---|---|---|
| `batch_window` (W) | element entries per batch | **2 048**, swept {128, 512, 2 048, 4 096, 8 192} | the sweep *is* the experiment: row/dTLB benefit appears at `W ≥ ~2 048` (§3.3) |
| `page_dir` (D) | directory slots, direct-mapped | **512** | W1's entire page set; at `D < P` collisions force early flush |
| `org_lanes` | insertions per cycle | 4 | matches `eng_issue = 4`, A's formation rate |
| `fanout_lanes` | elements extracted per fill | 4 (min) / 8 | see §7.2 — 1/cycle would be the bottleneck at 16 elements/line |
| `batch_deadline` | max fill age before sealing | 4 096 cy | anti-starvation |
| `org_overlap` | 1 = fill k+1 during issue of k | 1 | 0 = serialized, for cost attribution |

**Refill arithmetic (W1).** Fill rate is limited by the index stream, not the AG: one 64 B index line
carries 16 indices and costs ≈13 cycles of DRAM (2 banks, almost all conflicts) → ≈1.2 addresses/cycle,
so `ag_lanes = 8` is only ≈15 % utilised and the fill of W entries takes ≈0.83·W cycles:

| W | batches | fill cycles | issue cycles (128 × 7) | exposed per batch | total exposure |
|---|---|---|---|---|---|
| 2 048 | 8 | ≈1 700 | ≈900 | ≈800 | ≈6 400 (3.2 %) |
| 8 192 | 2 | ≈6 800 | ≈3 600 | ≈3 200 | ≈6 400 (4.0 %) |

Double buffering makes the exposure a per-batch tail rather than a full stall, and it is why the §3.3
projections saturate below the 1.76× ceiling. The sweep must report it, not absorb it.

### 6.3 Backpressure

* Directory full for a page (`D` collisions) or window full → seal early. Counted as
  `win_early_seal`.
* No free R slot at issue → `buf_full_stall` (existing counter, unchanged semantics).
* Both fill side full and issue side stalled → AG stalls, `idx_wait_stall` (existing).

---

## 7. Response fanout and retirement

### 7.1 Metadata and mechanism

One line request carries: `slot` (as today) plus, in the engine's own table,
`{ page_slot, line_bit, chain_head, n_elems_of_line }`. The R slot array stays exactly R entries; the
fanout table is the window, not a second residency class.

On fill (`attribute()` → `on_line()`), for the chain segment of that `(page,bit)`:

```
const u8* b = mem_.line_bytes(mem_.pa_line_of_va_line(va_line));   // one 64 B read
for (e in chain_segment)  val_[elem] = load_le(b + off[e], width[e]);   // fanout_lanes per cycle
retired += n_elems_of_line
```

* **One 64 B read per line**, not per element (M5 does one per element, `bulk.cpp:264`).
* Element→value mapping is unchanged in *semantics*; only the multiplicity of the memory read changes.
* Out-of-order retirement is inherent: fills arrive in DRAM order, chains are independent. Retirement
  accounting is per element (`retired`, plus a per-batch `done` bitmap to assert exactly-once).

### 7.2 The fanout bandwidth trap

At W0's 16 elements/line, a fill retires 16 elements. Line completion rate at DRAM saturation is one
line per `t_hit/2 = 7` cycles. Therefore:

| `fanout_lanes` | cycles to retire one line | vs 7-cycle arrival | verdict |
|---|---|---|---|
| 1 | 16 | 2.3× too slow | **fanout becomes the bottleneck** |
| 4 | 4 | fits | minimum acceptable |
| 8 | 2 | margin | matched to `ag_lanes = 8` |

So fanout is a *first-class cost*, not a detail. It is counted (`fanout_cycles`, `fanout_util_x1000`) and
charged: with `fanout_lanes = 1` the M6 engine would be *slower* than M5's per-element path on W0, which
would be a self-inflicted regression. This is exactly the "do not assume zero-cycle organization"
discipline the milestone asks for, and it is measurable on the first run.

### 7.3 Correctness with out-of-order responses

* Chains are immutable once the batch is sealed; no fill can add an element to a segment being drained.
* A line's slot returns to `free_` on fill, before fanout (fanout reads the image, not the slot) — so
  fanout latency does not hold residency. **This must be justified:** it means fanout's 4 cycles/line do
  not consume R, but they do consume issue bandwidth when the AG competes for the same ports. Counted,
  not hidden.
* Double-retire and lost-retire are both caught by the per-batch bitmap equality check.

---

## 8. Resource accounting and fair comparison methodology

### 8.1 The M5.1 latch asymmetry — resolution

Measured: A's peak outstanding is 132 = 128 slots + 4 coalescer latches, B's is 128. The latch is
`cur_lines.size() - cur_pos` (`system.cpp:229`), i.e. the line addresses of the instruction currently
being expanded, bounded by `lanes` (`system.cpp:155` caps attempts at `warps_per_core`, and the latch is
asserted `≤ lanes`).

**Resolution: charge the latch against A's budget.** In the controlled profile, A's slot pool becomes

```
per_core = R/cores − lanes          // system.cpp:80, 124 at cores = 1, R = 128, lanes = 4
```

so `peak_outstanding = 128` exactly, matching B. Implementation is one line plus a config flag
`ab_charge_latch = 0|1` (default 1 for controlled runs, 0 to reproduce M5.1's published numbers
bit-exactly). The asymmetry matters for the *claim*, not for the result direction: A's cycles are
identical at R = 64 and R = 128 (270 753 both), so shaving 4 slots cannot change the sign.

**B's new resource is disclosed, not hidden.** The window is an additional structure with no SIMT
counterpart: `W×8 + D×16` bytes (§4.1). It is reported in every table (`win_bytes`, `win_occ_max`,
`page_dir_occ_max`) and swept as an independent variable, so a reader can see what was bought and with
what. What is **not** done — and will not be — is pretending the window is free, or counting window
entries as residency to make B look capacity-bound.

### 8.2 The comparison, stated precisely

| Resource | A (SIMT) | B (M6) | Rule |
|---|---|---|---|
| Line requests in flight | `R − lanes` slots + ≤4 latch = **R** | **R** slots | identical, enforced every cycle |
| Front-end storage above R | none (latch only) | window `W·16 + D·16` B | B's extra, disclosed and swept |
| Cache path | `mem_path = l1` | `mem_path = l1` | one switch; `submit()` is the only entry point |
| Translation | core 0 dTLB 16×4w, 4 MSHR, 2 parked | same | unchanged; page order is the only lever |
| Workload | one `shared_ptr<const Trace>`, one image hash | same | M5.1 gates retained |
| Instruction supply | 4 warps × 1 instr/cycle | index stream + AG at `eng_issue = 4` | formation rate matched |

### 8.3 Accounting rules for organization cost

Every cycle the organizer spends is either **exposed** (`org_overlap = 0`, charged to `cycles`) or
**overlapped** (`org_overlap = 1`, charged only when it exceeds the issue phase). Both are reported; the
headline number is the overlapped one, with the serialized one beside it. Counters: `org_cycles`,
`fanout_cycles`, `win_occ_sum/max`, `win_early_seal`, `batch_count`, `batch_fill_cycles`,
`batch_issue_cycles`, `deadline_seals`, `dedup_merged` (elements folded into an existing bit),
`lines_issued` (unique lines) vs `elements` (fanout width).

**The invariant that keeps the report honest:** `elements == Σ lines_issued × fanout_width`. If
`lines_issued × 16 ≠ elements` on a 4 B gather, something was merged that should not have been.

---

## 9. Required simulator interface changes

### 9.1 Required for M6: **none** in `MemorySystem`

The whole of Option B — window, page directory, dedup, page-ordered issue, fanout — operates above
`submit()`. `submit()` already (a) probes the dTLB and charges the miss, (b) charges the L1 (under
`mem_path = l1`) and the L2/MSHR, (c) returns fills by `slot` via the `done` vector, and (d)
`line_bytes(pa_line_of_va_line(v))` already delivers the 64 B. No cache, TLB, walker or DRAM interface
changes.

The one thing the engine must *not* do is bypass translation for dedup — and §5.1 shows it does not have
to: VA-line identity is PA-line identity.

### 9.2 Engine-side changes (new code, existing patterns)

| Change | Where | Note |
|---|---|---|
| `WinEntry` / `PageSlot` pools | new `src/win.{hpp,cpp}` or `bulk.cpp` | engine-internal |
| `BulkEngine::organize()` insert path | replaces the per-element `alloc_and_enqueue` at `bulk.cpp:367` | keeps `alloc_and_enqueue` for index/desc |
| `issue()` walks pages/bits instead of `unissued_` FIFO | `bulk.cpp:388` | keeps the monotonic-violation counter, now *by design* |
| `on_line()` fanout | replaces `on_target()` (`bulk.cpp:262`) | one image read per line |
| `BulkStats` additions | `sim.hpp:395-412` | `org_cycles`, `fanout_cycles`, `dedup_merged`, `win_*`, `batch_*` |
| Config additions | `sim.hpp:100-120` | `batch_window`, `page_dir`, `org_lanes`, `fanout_lanes`, `batch_deadline`, `org_overlap`, `dedup`, `page_order`, `batch_window=0` → legacy path |

`batch_window = 0` must select the **existing M5 code path untouched**, so the M5.1 A/B numbers and the
M4a/M4b gates stay reproducible bit-exactly.

### 9.3 Optional, and what each would enable

| Interface | Signature | Enables | Cost |
|---|---|---|---|
| Address-map accessor | `void address_map(u64 pa_line, u32& bank, u32& row) const` | M8 row scheduling without duplicating `memory.cpp:412-425` in the engine (a duplicated hash is a correctness hazard if the map changes) | trivial, no behaviour change |
| Split translation | `bool translate(u32 core, u64 vpn, u64& ready, u64& ppn)` | Option C, and any PA-level cohorting | rewrites the residency rule (§2 C) — **not recommended now** |
| Per-client residency | `size_t inflight_count(u32 client) const` | co-residency of A and B, or a second engine client | engine currently assumes `mem_.inflight_count()` is its own (`bulk.cpp:152`) — a latent limitation if A and B ever share a system; the A/B harness never does |
| Line-fill payload | `const u8* fill_bytes(u64 slot)` | removes the engine's need to know `pa_line_of_va_line` | replaces a pure-function call with state; slightly *less* honest, since it hides the fetch the model does not model anyway |

---

## 10. M6 implementation plan and acceptance tests

### 10.1 Order of work

1. **Fair-comparison fix first** (`ab_charge_latch`) — it is 3 lines, it must be in the baseline of every
   M6 measurement, and it is the milestone's explicit requirement.
2. **Window + page directory + insert path**, with `dedup = 0`, `page_order = 0`: the engine must still
   emit one request per element, in generation order. Gate: bit-identical to M5.1's B arm.
3. **Line bitmap + fanout** (`dedup = 1`): on W0, 17 409 → 5 121 requests expected (4 096 unique lines +
   1 024 index + 1 descriptor), L1 MSHR joins → 0.
4. **Page-ordered issue** (`page_order = 1`): the translation and DRAM effect.
5. **Counters + report**: `org_cycles`, `fanout_cycles`, `win_bytes`, batch timeline.
6. **Sweep `batch_window` × `page_dir`**, with `dedup`/`page_order` as the 2×2 ablation.

### 10.2 Acceptance tests (appended to `tests/tests.cpp`, same style)

| # | Test | Pass condition |
|---|---|---|
| 1 | `m6_legacy_identity` | `batch_window = 0` reproduces M5.1 B exactly: W1 278 034 cy, 17 409 requests, DRAM 17 444, same completion hash |
| 2 | `m6_neutral_identity` | `batch_window = 2048, dedup = 0, page_order = 0` reproduces the same numbers as (1) — the window alone changes nothing |
| 3 | `m6_dedup_soundness` | W0 (κ=1): `lines_issued = 4 096`, `elements = 16 384`, `fanout_width = 4`, `Σ = elements`; `l1_mshr_joins = 0` |
| 4 | `m6_value_equivalence` | for every element, `value_of(elem)` and `addr_of(elem)` equal the M5 FIFO arm's, bitwise, for both W1 and W0 (order free) |
| 5 | `m6_bitmap_exact` | per page, `line_mask` equals an independent `std::set` of the batch's VA lines; group sizes sum to `W` |
| 6 | `m6_residency` | `unissued + inflight ≤ R` every cycle; one slot per **line** from issue to fill; `win_occ ≤ W` separately |
| 7 | `m6_no_starvation` | descriptor of 17 elements with `W = 2048`: every element issues within `batch_deadline`; `win_early_seal > 0` |
| 8 | `m6_organization_charged` | `org_cycles == 2W/org_lanes` per batch when `org_overlap = 0`, and `cycles` includes it; with `org_overlap = 1`, `org_cycles` is reported but not added |
| 9 | `m6_fanout_bandwidth` | `fanout_lanes = 1` is measurably slower than `= 4` on W0 (the trap of §7.2 is reproduced, then fixed) |
| 10 | `m6_baselines_unchanged` | M4a hash `5ffe97c9c9b4bf00` / 86 308, M4b 104 332 / 102 947, `vortex_ref` 280 432 / 16 417, and M5.1's A-arm numbers all unchanged |
| 11 | `m6_latch_charged` | with `ab_charge_latch = 1`, A's `peak_outstanding == R == 128` and B's `== 128` |
| 12 | `m6_dtlB_effect` | `P = 4096` (more pages than L2 TLB entries): `page_order = 1` yields strictly fewer dTLB misses than `page_order = 0` |

### 10.3 Milestone sequence (revised)

The requested M6→M9 sequence is right in spirit; two changes follow from the source:

* **M6** — window, page directory, dedup, page-ordered issue, fanout, fair-comparison fix. **This is the
  whole architecture experiment.** Page order gives the translation effect and the row effect
  simultaneously (§3.2), so M8's mechanism is largely *delivered* by M6.
* **M7** — translation-aware organization *as a measured ablation*, not a new mechanism: sweep `D` and
  `page_order`, and go to `P = 4 096` so the L2 TLB (512) thrashes and page grouping has something to
  fix. If the effect is zero at `P = 512`, the report must say so.
* **M8** — row-aware scheduling, **gated**: first instrument only (resident `(bank,row)` duplicate count
  per cycle; predicted ≈8/128 on W1 — the 5.8 % row of the §3.3 table — and ≈3 900/4 096 on W0). Implement only if the gate shows
  headroom, and only on a map where PA ≠ VA row (otherwise vacuous, §5.2).
* **M9** — integrated evaluation and the 2×2×2 ablation. Not before M6 reports.

---

## 11. Known limitations and unresolved design decisions

1. **Evidence gap: no aliases, and a row-invariant address map** (§5.2). The model cannot distinguish
   VA-derived from PA-derived row grouping, and cannot test alias handling. Any M8 claim must be
   qualified accordingly, and any alias claim is unsupported.
2. **Unresolved: chain vs per-line-group heads** (§4.1). Chains are cheap in area, slow to drain; a
   full 64-head page costs 128 KiB at `D = 512`. The 8-head compromise is a guess that needs a
   measurement (`fanout_cycles` at W0's 16 elements/line).
3. **The window's area cost is not modelled as a constraint.** 24–41 KiB of SRAM beside a 16 KiB L1 is a
   real design statement, and no counter reflects its cost in the comparison beyond disclosure.
4. **No masks, no write path, no LMEM writeback** — unchanged from M5/M5.1. A masked gather (soundness)
   and a copy-out (where the accelerator would actually pay) remain unmodelled; B's cost stays
   understated, as in M5.1 §10.
5. **Single descriptor, single client.** Multi-descriptor pipelining and a second TLB client are out of
   scope; `residency()` assumes the shared `inflight_count()` is the engine's own
   (`bulk.cpp:152`), which is true only while A and B never co-reside.
6. **The DRAM model is one command/cycle with no bank groups or bus turnaround.** Row-hit conversion is
   therefore *overstated* in benefit and *understated* in cost: a real device pays an activate/precharge
   penalty that this model folds into `t_miss`.
7. **Organization cost is a parameter, not a synthesis result.** `org_lanes = 4` and `fanout_lanes = 4`
   are design choices; the report must present the sweep, not a single point.
8. **The 1.76× row of §3.3 requires a window the size of the whole workload** (16 384 entries ≈ 128 KiB
   of window SRAM at 8 B/entry), which is not a plausible hardware structure. The implementable regime is
   W = 2 048–8 192, i.e. **1.35–1.68×**, and the report must not quote the ceiling as if it were reachable.
9. **Address generation is index-DRAM-bound, not AG-bound.** At ≈1.2 addresses/cycle the 8-wide AG is
    ≈15 % utilised, so the window fills at the index stream's pace and `ag_lanes` is nearly irrelevant to
    results — a finding to state, not to tune around.
10. **The 78.6 % DRAM busy fraction is A's**, not the organized engine's. If page cohorts serialise issue
    (fewer independent banks in flight at once, since a cohort targets 2 rows of 1 bank), the engine can
    become latency-bound and land below the projection. M6 must report the measured busy fraction
    alongside the projection, not instead of it.
