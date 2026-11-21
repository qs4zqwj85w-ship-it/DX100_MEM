# Hardware resource and cost audit — DX100 Bulk accelerator (M5–M13)

**Question.** How much hardware does each mechanism require, and are the measured
speedups worth the area, power, timing risk and complexity?

**Answer in one paragraph.** The core architecture is cheap: a 2 × 8K windowed
Bulk engine costs **~0.17 mm² of SRAM at a nominal 7 nm density** (≈ 3.6× a
16 KiB L1's array, half a 1 MiB L2) and buys 1.5–1.8× over SIMT on irregular
work. Growing the windows to 2 × 32K costs **~0.67 mm²** (1.8× the L2) and is the
best area-per-performance trade in the whole design — up to **93 % of cycles
saved per mm²** — but only for irregular-heavy and mid mixtures at N ≥ 32 768;
in regular-heavy mixes it is 7.5 % *slower* for 4× the area. The M13 reuse
directory is the one mechanism that does not justify its silicon: 0.30–0.69 mm²
of flat SRAM for 5.8–40.7 % in a single workload family, and its 16 % edge over
2 × 32K evaporates the moment the window is built efficiently. Two structures
should never reach RTL: the separate per-line record array (56 % of the
window's realized storage) and the simulator's `val_`/`addr_` element maps
(1.6–2.0 MB of diagnostic state that has no hardware counterpart).

How to read the numbers below — every value carries one of four tags:

| tag | meaning |
|---|---|
| **[M]** | **measured** — produced by the simulator in this repo, reproducible from the logs |
| **[C]** | **calculated** — arithmetic on measured counts and the entry widths in §2 |
| **[E]** | **estimated** — a labelled modelling assumption (SRAM density, energy per access); never a measurement |
| **[?]** | **speculative** — plausible but unvalidated (frequency, RTL gate cost) |

No physical area, power or frequency measurement was made. Everything in §4 and
§7 is a model with visible inputs.

---

## 1. Executive summary

| mechanism | storage [M] | area [E] | benefit [M] | verdict |
|---|---|---|---|---|
| 2 × 8K window + dedup (M10) | 147 584 B charged / 933 888 B realized | 0.053 / 0.171 mm² | 1.52–1.81× vs SIMT | **essential** |
| M7 page-cohort scheduling | 8 256 B per buffer | 0.003 mm² | 1 192 vs 2 206 row activations at N=16 K | **essential** |
| M8 index pipeline | 2 720 B | 0.001 mm² | removes index serialization | **essential** |
| M9 shared R = 128 pool | 7 168 B of flops | ~0.03 mm² | enables dispatch | **essential** |
| configurable W → 2 × 32K | 540 800 B charged / 2.4 MB realized | 0.195 / 0.667 mm² | **−46.2 %** cycles (κ=0.5, N=65 K) | **worthwhile, conditional** |
| 2 × 16K | 278 656 B | 0.100 / 0.336 mm² | never best by > 0.13 % | **not recommended** |
| M13 reuse directory, K = 4 096 | 508 032 B | 0.183 / 0.301 mm² | −5.8 % (κ=0.25) | **experimental** |
| M13 reuse directory, K = 16 384 | 1 589 376 B | 0.572 / 0.690 mm² | −40.7 % (κ=0.25) | **not recommended** |
| `val_`/`addr_` element maps | ≈ 1.6–2.0 MB [C] | n/a | none (diagnostic) | **discard** |

Three findings that change earlier conclusions:

1. **The 8 B/entry charge is a lower bound, not the cost.** Measured entry records
   are 16 B (`WinEnt`), 32 B (`LineReq`), 56 B (`Pending`) and 100 B
   (`ReuseEnt`). A window as currently specified needs the element table **and**
   a per-line record **and** a permutation, i.e. **~3.2× the modelled charge**
   even in a packed layout. The M12 storage comparison understated the window by
   that factor ([C], §2.3).
2. **Because of (1), 2 × 32K and M13-at-K=Λ are the same size in hardware**
   (1.85 MB vs 1.92 MB in the nominal packed layout), and M13 is 16 % faster
   there. M13's verdict in the M13 report — "2.9× the storage for a 16 % win" —
   holds only if the window is built at its 8 B/entry ideal. **The right
   response is to build that ideal window, not to buy the directory.**
3. **The 6–9× realized/modelled ratio is structural, not a vector artifact.**
   At the measured peaks the vector capacities equal their sizes (933 888 B
   measured vs 934 016 B computed from `capacity()==size`), so `std::vector`
   over-allocation contributes **nothing**; the ratio is the field widths plus
   the `LineReq` array that the model does not charge at all (§2.3).

---

## 2. Hardware structure inventory

### 2.1 Entry records, measured (`build/wm/audit_sizes`, g++ **sizeof** probe)

| record | size [M] | bits | fields (from `sim.hpp`) | used for |
|---|---|---|---|---|
| `WinEnt` | **16 B** | 128 | `va_line` 8, `elem` 4, `off` 2, `width` 1, pad 1 | one admitted element |
| `LineReq` | **32 B** | 256 | `va_line` 8, `first` 4, `count` 4, `seq` 4, `slot` 4, `issued` 1, `reuse_ent` 4, pad 3 | one distinct target line |
| `Batch::Cohort` | **32 B** | 256 | `pos` 4, `end` 4, `vpn` 8, `resume_at` 8, `open` 1, pad 7 | one page run (M7) |
| `FanJob` | **76 B** | 608 | `batch` 4, `line` 4, `pos` 4, `bytes[64]` | one in-flight fill/fanout |
| `Pending` | **56 B** | 448 | kind, va_line 8, elem 4, off 2, width 1, seq 8, first_elem 4, win_batch/line 8, reuse_ent 4, … | one R-slot request |
| `ReuseEnt` | **100 B** | 800 | tag 4, lru 4, state 1, nsub 1, `subs[3]` 24, `data[64]` | M13 directory entry |
| `ReuseEnt::Sub` | 8 B | 64 | batch 4, line 4 | M13 in-flight merge |
| `BulkDesc` | **40 B** | 320 | base/count/stride fields | queued descriptor |
| `WinRec` | 72 B | 576 | window timeline record | **diagnostic only** |

`LINE_BYTES = 64`. Two of these are optimisation-relevant and two are pure
instrumentation (`WinRec`, and the maps of §2.4).

### 2.2 Structures, by function

| # | structure | entries | bits/entry | total | kind | replication | scales with | provenance |
|---|---|---|---|---|---|---|---|---|
| 1 | window element table (`Batch::ent`) | W = 8 192 | 128 [M] (49 packed [C]) | 131 072 B | data+meta | per buffer ×2 | **W** | new (M6) |
| 2 | seal permutation (`Batch::sorted`) | W | 32 | 32 768 B | grouping meta | per buffer ×2 | W | new (M6) |
| 3 | line records (`Batch::lines`) | ≤ W distinct | 256 [M] (≈112 packed [C]) | 262 144 B | tags+meta | per buffer ×2 | distinct lines ≤ W | new (M6) |
| 4 | issue order (`Batch::order`) | ≤ W | 32 | 32 768 B | control | per buffer ×2 | lines | new (M6) |
| 5 | page directory (`dir_vpn`, `dir_mask`) | D = 512 | 128 (2 × u64) | 8 192 B | tags+mask | per buffer ×2 | page_dir (not W) | new (M7) |
| 6 | page runs (`Batch::coh`) | 2 | 256 [M] | 64 B | control | per buffer ×2 | page_cohorts | new (M7) |
| 7 | index tiles (`idx_mode`) | 2 × 64 lines | 168 (21 B [M]) | 2 720 B | data+ready bits | per engine | idx_buf_lines | new (M8) |
| 8 | request records (`info_`) | R = 128 | 448 [M] | 7 168 B | control | per engine | **R** | new (M5) |
| 9 | slot FIFOs (`unissued_`, `idx_q_`, `free_`) | ≤ 128 each | 32 | ≤ 1 536 B | control | per engine | R | new (M5) |
| 10 | fanout queue (`fan_`) | peak backlog | 608 [M] | 76 B each | data (64 B) + refs | per engine | outstanding fills | new (M6) |
| 11 | descriptor queue (`queue_`) | queue depth | 320 | 40 B each | control | per engine | descriptor rate | new (M5) |
| 12 | **M13 directory (`reuse_`)** | K = sets × ways | 800 [M] / 704 charged [C] | 102 400–1 638 400 B | tags+**retained data** | per engine, off by default | **K** | new (M13), optional |
| 13 | counters, hashes, timeline | ~120 scalars | 64 | ≈ 1 KB | control | per engine | — | instrumentation |
| 14 | `val_`, `addr_` maps | N elements | ≈ 32–40 B/elem [E] | ≈ 1.6–2.0 MB at N=49 k | diagnostic | per engine | **N** | **simulator only** |

Rows 1–12 are the hardware the design owns. Row 13 is reporting. Row 14 exists
only because the simulator must answer `value_of(elem)` for tests; a hardware
engine never stores all N results (§2.4).

### 2.3 Reconciliation with M12's 6–9× finding

At `W = 8 192`, per buffer [C]:

| component | bytes | share |
|---|---|---|
| element table (16 B × 8 192) | 131 072 | 28.1 % |
| seal permutation (4 B × 8 192) | 32 768 | 7.0 % |
| **line records (32 B × 8 192)** | **262 144** | **56.1 %** |
| issue order (4 B × 8 192) | 32 768 | 7.0 % |
| page directory (16 B × 512) | 8 192 | 1.8 % |
| page runs (2 × 32 B) | 64 | — |
| **sum per buffer** | **467 008** | |
| **simulator's modelled charge** | **73 792** | |
| **ratio** | **6.33×** | |

Measured `bulk_win_vec_bytes` at W1/N=65 536 [M]: **933 888 B** = 466 944 per
buffer, i.e. within 64 B of the table above. The capacities *equal* the sizes at
the measured peak, so:

* **vector over-allocation contributes nothing** to the ratio — M12's
  "capacities never shrink" explanation is not the cause at these cells;
* the ratio is **2× from `WinEnt` being 16 B rather than 8** (padding of a 49-bit
  record), **plus 4 B/element of permutation**, **plus a 32 B line record the
  charge ignores entirely**, **plus 4 B/line of order**.

Ratio by cell [M]: 6.33× (8K), 6.64× (16K), 4.51× (32K — the realized peak
occurs at a partly filled 32K buffer, 15 479 distinct lines of 32 768).

**Conclusion:** the 6–9× is a *lower bound* on how wrong an 8 B/entry estimate
is, not an artifact to be dismissed. The audit therefore reports three storage
figures for every configuration (§3).

### 2.4 Structures that must not reach RTL

* `std::unordered_map<u32,u64> val_, addr_` — written on **every** fill in both
  the windowed and M5 paths (`bulk.cpp:502-504`, `1572-1573`), one node per
  completed element. At N = 49 304 (κ=0.25/65 K) that is ≈ 1.6–2.0 MB [E, node
  layout dependent] of state whose only readers are `value_of()`/`has_value()`
  in tests and diagnostics. A hardware fanout writes the element's word into the
  consumer's register and keeps nothing.
* `WinRec` (72 B/window) and the `m13_dump` writer — timeline and trace
  instrumentation.
* `std::set<u64> idx_pending_, tgt_pending_` — ordered sets used only to count
  out-of-order completions; a hardware scoreboard needs a bitmap of R bits.

---

## 3. Storage calculations

Three layouts, all at `D = 512`, `page_cohorts = 2`, `R = 128`:

* **L1 — model charge.** `2 × (W×8 + D×16 + sched)`. Assumes the element table
  is the *only* per-element storage (fused 64-bit entry: line 26, elem 14,
  offset 6, width 3, next 14, issued 1).
* **L2 — packed nominal.** The algorithm as specified: 8 B element + 2 B
  permutation + 16 B packed line record + 2 B order + directory + runs. This is
  the honest cost of *today's* structure with narrow fields.
* **L3 — simulator realized.** Measured `bulk_win_vec_bytes` [M].

| configuration | L1 [M] | L2 [C] | L3 [M] | L2/L1 |
|---|---|---|---|---|
| 2 × 8K | 147 584 B | 475 264 B | 933 888 B | 3.22× |
| 2 × 16K | 278 656 B | 934 016 B | 1 851 328 B | 3.35× |
| 2 × 32K | 540 800 B | 1 851 520 B | 2 441 760 B | 3.42× |
| M13 K=1 024 over 2×8K | 237 696 B | 565 376 B | 1 036 288 B | 2.38× |
| M13 K=4 096 over 2×8K | 508 032 B | 835 712 B | 1 343 488 B | 1.64× |
| M13 K=8 192 over 2×8K | 868 480 B | 1 196 160 B | 1 753 088 B | 1.38× |
| M13 K=16 384 over 2×8K | 1 589 376 B | 1 917 056 B | 2 572 288 B | 1.21× |

M13's directory is a single flat array of fixed-size entries, so its three
layouts converge (704 B charged, 800 B object); the *window's* inflation is what
makes M13 look expensive in L1 and competitive in L2.

Non-window structures, constant across configurations [M]/[C]: request records
7 168 B, slot FIFOs ≤ 1 536 B, index tiles 2 720 B, fanout queue 76 B × peak
backlog (measured peak `bulk_fanout_max_backlog`), descriptor queue 40 B ×
depth, counters ≈ 1 KB. Total ≈ **12–14 KB**, i.e. **8 % of the 8K window** and
independent of W.

---

## 4. Area estimation methodology

**Assumption (E1) — SRAM bitcell and array efficiency.** Published 6T SRAM
bitcell densities are ≈ 0.021 µm² (5 nm), ≈ 0.027 µm² (7 nm), ≈ 0.05 µm²
(14/16 nm). Peripherals, sense amps, redundancy and wiring add 40–100 % for the
array sizes here. Three points are used, all labelled illustrative:

| point | bitcell | array efficiency | effective µm²/bit |
|---|---|---|---|
| low | 0.021 | 0.70 | **0.030** |
| nominal | 0.027 | 0.60 | **0.045** |
| high | 0.050 | 0.50 | **0.100** |

`area_mm² = bytes × 8 × µm²_per_bit / 10^6`. No vendor compiler was available;
these are order-of-magnitude numbers, not a synthesis result. **Minimum macro
size** (≈ 0.005 mm²) binds only the page directory and index tiles, which are
therefore costed as flop-based macros, not as SRAM.

| configuration | L1 area (low/nom/high) | L2 area (low/nom/high) | L3 area (nominal) |
|---|---|---|---|
| 2 × 8K | 0.035 / **0.053** / 0.118 mm² | 0.114 / **0.171** / 0.380 mm² | 0.336 mm² |
| 2 × 16K | 0.067 / **0.100** / 0.223 | 0.224 / **0.336** / 0.747 | 0.666 |
| 2 × 32K | 0.130 / **0.195** / 0.433 | 0.444 / **0.667** / 1.481 | 0.879 |
| M13 K=4 096 | 0.122 / **0.183** / 0.406 | 0.201 / **0.301** / 0.669 | 0.484 |
| M13 K=16 384 | 0.381 / **0.572** / 1.272 | 0.460 / **0.690** / 1.534 | 0.926 |

Scale references for the same assumptions: a 16 KiB L1 → **0.047 mm²**, a 1 MiB
L2 → **0.377 mm²** (nominal). So the 2 × 8K window is **1.1× the 16 KiB L1's own
array** at its L1 charge and **3.6×** at the specified (L2) packing; the 2 × 32K
window at L2 packing is **1.8× a 1 MiB L2**. This is the sobering reading of the
area numbers: relative to an on-chip cache the window is *not* small, and the
packing choice (§5) is worth 2.9 MB.

Control state is not SRAM and must not be costed as if it were. The request
records alone are 7 168 B of flops (row 8 of §2.2) ≈ 57 k DFFs at ≈ 0.3–0.6 µm²
each [E] → **0.02–0.03 mm²**, i.e. *about as much as the whole 8K window's
SRAM*. Request tracking, not data buffering, is the larger of the two.

---

## 5. Logic complexity assessment

No gate counts are inferred from C++. The classification is structural: what the
hardware must compare, select or arbitrate per cycle. **[?]** = would need RTL.

| logic block | what it does | relative size [?] | source | RTL-ready |
|---|---|---|---|---|
| address generation | `base + index × bytes`, per lane, 64-bit | **large** (1 multiplier + adder per AG lane) | `bulk.cpp` `win_issue`/`stream_index` | yes |
| tag compare | 26-bit equality × 4 ways + hit mux | medium | `win_reuse_probe` | yes |
| set index hash | 2 × 64-bit multiply + 2 shifts/XOR | medium (2 DSP blocks) | `reuse_set_of` | yes, pipelined |
| LRU / replacement | 4-way age update per hit/insert | small | `reuse_publish` | yes |
| dedup grouping at seal | compare-and-group W elements by line | **largest sequential block** | `win_seal` (sort) | needs redesign (§9) |
| page-run open/switch | vpn compare, span/cursor counters, retry deadline | small | `win_issue` | yes |
| slot arbitration | 128-entry free list + index reserve split | medium (priority select) | `tgt_reserve`, `win_emit` | yes |
| fanout | 64 B copy + dependant list walk | medium (64 B/cycle mux) | `win_fanout` | yes, restructure |
| completion scoreboard | seq tracking, OOO detection | small | `idx_pending_`/`tgt_pending_` | simplify to bitmap |
| control FSMs | descriptor fetch, seal, issue, drain | medium | `win_tick` | yes |

**The one block with no clean RTL path is the seal.** The model sorts W entries
(`std::stable_sort` on `(va_line, id)`) and charges W/4 cycles for the pass. A
hardware implementation would not sort: it would stream the element table
through a hash/grouping stage that writes `(head, next, count)` into the line
record array, one element per cycle, W cycles per window, with no comparator
network. That redesign also **removes the `sorted` array** (row 2, 7 % of
realized storage) at the cost of ~14 bits of `next` pointer per element, which is
the fusion assumed in layout L1. This is the single highest-leverage
simplification in the whole architecture.

---

## 6. Timing analysis

### 6.1 What the simulator charges, versus what hardware costs

| operation | simulator | plausible hardware | risk |
|---|---|---|---|
| window admission | 1 visit, 4 visits/cycle shared (`org_lanes`) | 1 element/cycle into SRAM (1 port) → **4× slower** than the model | **medium**: model is optimistic by 4× on this phase |
| seal / dedup | W visits → W/4 cycles | W cycles single-port; 2W if a hash write follows | medium |
| fanout | 4 lanes/cycle (`fanout_lanes`) | 1 element/cycle, or 4 with 4 banked macros | low (not on the critical path, §6.2) |
| tag lookup (M13) | 1 visit | 1 cycle (26-bit compare × 4 + mux ≈ 12–15 FO4 at 1 GHz) | low |
| page-directory lookup | 1 visit | 1–2 cycles (SRAM read + mask extract) | low |
| address generation | 1 target/cycle (`eng_issue = 1`) | 1 per cycle at 1 GHz is comfortable for a 26×4 B multiply | low |
| L1 access | 1/cycle | 1/cycle (published configuration) | none |

**How much does the 4× admission optimism matter?** The simulator classifies its
own cycles: at κ=0.25/N=65 536/W=8K, **1 806 of 423 791 cycles (0.4 %) are
attributed to window/organizer stalls** [M]. A 4× error on that phase is
therefore ≤ 1.7 % of total cycles — it cannot change any conclusion in this
audit or in M12–M14. This is the right way to bound a simulator's phase
assumption: not by trusting it, but by measuring how much of the run it owns.

### 6.2 Critical paths and their real cost

* **Nothing in the Bulk data path is a frequency limiter.** The longest
  combinational chain is 26-bit tag compare → 4-way hit mux → 64 B read mux,
  which pipelines in 1 cycle at 1 GHz. The 64-bit hash multiply is 2–3 cycles and
  belongs in a pipeline stage, not the lookup path.
* **The seal is a throughput problem, not a timing problem.** It is inherently
  W cycles (one element per cycle through a grouping stage) regardless of
  frequency; at W = 32 768 and 1 GHz that is 33 µs per window, but it overlaps
  the previous window's drain (measured: `consumer starve = 0` in every cell of
  the M14 sweep [M]), so it is hidden.
* **Buffer switching** is a register-file write plus a state transition: 1–2
  cycles. The model's `seal → issue` of 1 cycle is realistic.
* **Fanout** is the only path that can back-pressure: one 64 B line per cycle at
  4 elements/cycle consumption requires a 4-wide 64 B mux (≈ 2 048 bits/cycle).
  Feasible but it is the largest wire-dominated structure in the design.

**Do not read simulator cycles as silicon timing.** The model's 4 visits/cycle
organization and unlimited-width fanout are abstractions; §6.1 bounds their
effect on the *conclusions*, not on achievable frequency. No synthesis, placement
or clock-tree work exists for this design, so any frequency claim would be
fabricated.

---

## 7. Power and energy analysis

### 7.1 Model

**Assumption (E2).** Energy per event, normalized to one 64 B SRAM access
(`E1`), using published memory-hierarchy energy tables: a 64 B SRAM read is
≈ 5 pJ at 45 nm-class, a 64 B DRAM read including activate/precharge is
≈ 0.6–1.3 nJ, i.e. **E_DRAM ≈ 100–200 E1**. A tag compare and LRU update are
≈ 0.02 E1 a piece and are folded into the access count. These are ratios, not
measurements of this design. No leakage or clock power is estimated (it scales
with §4 area and would be pure invention at this stage).

### 7.2 Activity, measured [M] (κ = 0.25 and 0.50, N = 65 536, seed 1, arm C)

| event | 2×8K (κ=.25) | 2×16K | 2×32K | 2×8K (κ=.5) | 2×32K (κ=.5) |
|---|---|---|---|---|---|
| buffer writes (elements admitted) | 49 304 | 49 304 | 49 304 | 32 592 | 32 592 |
| target requests (= SRAM line fills) | 49 290 | 49 272 | 32 752 | 32 580 | 16 384 |
| fanout element reads | 49 304 | 49 304 | 49 304 | 32 592 | 32 592 |
| dTLB scheduler probes | 529 646 | 492 140 | 316 773 | 384 228 | 167 099 |
| page-run switches | 3 206 | 1 670 | 1 024 | 2 048 | 512 |
| index lines fetched | 3 082 | 3 082 | 3 082 | 2 037 | 2 037 |
| **DRAM reads** | 50 372 | 52 330 | 35 873 | 37 149 | 19 730 |
| dedup merges | 14 | 32 | 16 552 | 12 | 16 208 |

### 7.3 Resulting energy deltas (relative, [C] on [M] activity)

| comparison | DRAM energy | on-chip energy | net | DRAM share |
|---|---|---|---|---|
| κ=0.5: 32K vs 8K | **−1.74 M E1** (−17 419 reads) | −0.25 M E1 (fewer probes *and* fewer fills) | **−2.0 M E1** | 87 % |
| κ=0.25: 32K vs 8K | −1.45 M E1 | −0.26 M E1 | −1.7 M E1 | 85 % |
| κ=0.75: 32K vs 8K | **+0.22 M E1** (+2 162 reads) | +0.05 M E1 | **+0.27 M E1** | 81 % |
| M13 K=16 384 vs 8K (κ=0.25) | −2.27 M E1 (−22 723 reads) | +0.04 M E1 (22 251 publishes + 18 170 hits) | −2.2 M E1 | 98 % |

Each on-chip event (buffer write, fill, fanout read, scheduler probe) is counted
at 1 E1, which *overstates* the on-chip side by roughly 5× for a probe and
understates a 64 B write by less; the DRAM share column is therefore a **lower
bound** on the DRAM contribution.

**Does reducing DRAM requests compensate for the added on-chip storage and
lookup energy?** Yes, decisively, wherever the requests are removed at all — the
DRAM/SRAM energy ratio (100–200×) swamps a directory probe (≈ 0.02 E1) and a
64 B retained-line write (1 E1). The mechanism that *fails* energetically is the
one that removes nothing: κ=0.75 at 32K is 2 162 reads **worse** and 5 % more
on-chip activity, so it loses on both axes.

Illustrative conversion (E3, order of magnitude only): 17 419 fewer 64 B DRAM
reads at ≈ 1 nJ each ≈ 17 µJ per run; over 320 616 cycles at 1 GHz (321 µs) that
is ≈ **54 mW** of average memory-system power removed during the run. This is a
derived illustration of magnitude, **not** a power measurement.

### 7.4 Static power

Scales with §4 area and is not separately estimated. Relative statement only:
the 2 × 32K window at L2 packing is 1.85 MB ≈ **1.8× a 1 MiB L2**, so its
retention power is on the order of the L2's — a meaningful fraction of the
memory-system leakage, and the reason the M13 directory (1.92 MB of *data* SRAM
with no traffic reduction beyond what 32K already achieves) is the worst
power/area trade in the design.

---

## 8. Performance versus hardware cost

Cycles are measured [M] (M14 κ sweep, 5 paired seeds; M13 for the directory
cells, which exist only at κ=0.25 — **not extrapolated**). Area is L2 layout,
nominal [E].

### 8.1 κ = 0.25, N = 65 536

| configuration | cycles | speedup vs SIMT | added storage vs 8K | area | %/mm² added |
|---|---|---|---|---|---|
| 2 × 8K | 421 827 | 1.521× | — | 0.171 mm² | — |
| 2 × 16K | 439 337 | 1.461× | +458 752 B | 0.336 | **−25 (worse)** |
| 2 × 32K | **298 580** | 2.149× | +1 376 256 B | 0.667 | **+59** |
| M13 K=4 096 | 397 514 | 1.614× | +360 448 B | 0.301 | +44 |
| M13 K=8 192 | 351 748 | 1.824× | +720 896 B | 0.480 | +54 |
| M13 K=16 384 | **250 318** | 2.563× | +1 441 792 B | 0.690 | +78 |

### 8.2 κ = 0.50, N = 65 536

| configuration | cycles | speedup | added storage | area | %/mm² added |
|---|---|---|---|---|---|
| 2 × 8K | 320 618 | 1.455× | — | 0.171 | — |
| 2 × 16K | 311 182 | 1.499× | +458 752 B | 0.336 | +18 |
| 2 × 32K | **172 612** | **2.703×** | +1 376 256 B | 0.667 | **+93** |

### 8.3 κ = 0.75, N = 65 536 (the negative cell)

| configuration | cycles | speedup | added storage | area | %/mm² added |
|---|---|---|---|---|---|
| **2 × 8K** | **245 041** | **1.199×** | — | **0.171** | — |
| 2 × 16K | 263 498 | 1.115× | +458 752 B | 0.336 | −46 |
| 2 × 32K | 263 419 | 1.116× | +1 376 256 B | 0.667 | −15 |

### 8.4 The comparison that matters for M13

| basis | 2 × 32K | M13 K=16 384 | winner |
|---|---|---|---|
| model charge (L1) | 540 800 B | 1 589 376 B | 32K by **2.9×** |
| packed nominal (L2) | 1 851 520 B | 1 917 056 B | **tie (1.04×)** |
| simulator realized (L3) | 2 441 760 B | 2 572 288 B | tie (1.05×) |
| cycles (κ=0.25, N=65 536) | 298 580 | **250 318** (−16.2 %) | M13 by 16 % |
| %/mm² (L2) | **59** | 78 | M13 per mm², 32K per absolute area |

The M13 report's "2.75× the SRAM" verdict is correct **for an ideal 8 B/entry
window**. As specified, the window needs ~3.2× that charge, and the two designs
land within 4 % of each other in area — at which point the faster one (M13) is
also the more reasonable one. **The audit's recommendation is not "build M13"; it
is "build the fused window", which is what makes 32K the cheap answer (§10).**

---

## 9. Pareto frontier

Cost = L2 nominal area; performance = cycles at the same (κ, N). A point is
dominated if another is at least as fast with no more area.

**κ = 0.25, N = 65 536** — frontier: **8K → M13 K=4 096 → M13 K=8 192 →
2 × 32K → M13 K=16 384**.

* `2 × 16K` is **dominated by 2 × 8K** (more area, 4.15 % slower).
* `M13 K=4 096` is on the frontier only because it is 0.30 mm² against 32K's
  0.67; 32K is 25 % faster for 2.2× the area, so 4 096 is a weak point.
* `M13 K=16 384` is 16 % faster than 32K at **1.03× its area** — the one point in
the whole audit where the directory is genuinely attractive, and it exists only
because the window is costed at L2 packing (see §8.4).

**κ = 0.50, N = 65 536** — frontier: **8K → 16K → 32K** (each step faster, each
step bigger). 32K saves 46 % for 3.9× area: 93 %/mm², the best trade in the
design.

**κ = 0.75, N = 65 536** — frontier: **8K alone**. 16K and 32K are strictly
dominated (more area, 7.5 % slower).

Robustness: L1 and L2 layouts differ by a near-constant factor (3.22–3.42×)
across capacities, so **the frontier ordering is identical in both layouts** and
the only conclusion sensitive to the layout choice is §8.4 (M13 vs 32K).

---

## 10. Recommended architecture configuration

**Fixed 2 × 8K default, one 32K mode bit, no 16K point, no reuse directory.**

1. **Ship the fused window (layout L1) as the design target.** 64-bit element
   entry = line 26, elem 14, offset 6, width 3, next 14, issued 1. This deletes
   `sorted`, `order` and the 32 B `LineReq` array, cutting the window from
   1.85 MB to 0.54 MB at 32K — a 1.3 MB saving that no single optimisation in
   this project can match on area. It requires the seal to become a streaming
   hash/grouping pass (§5), which is *simpler* than a sort, not harder.
2. **Physical array = 32K per buffer; default runtime limit 8K.** One 2 × 32K
   array (0.667 mm² nominal L2 layout) instantiated once, with the 8K default
   reached by setting `batch_window = 8192`. There is no need for three
   variants: the SRAM is sized for the maximum and the mode bit selects how much
   is used.
3. **Enable 32K when the dispatch can see reuse:** descriptor elements >
   window capacity **and** elements per distinct line ≥ 2 (the M14 rule). Do not
   enable it for regular-heavy mixes — measured +7.5 % for 3.9× area.
4. **Retire 2 × 16K** as a supported point. It is dominated in 2 of 3
   representative cells and never beats 32K by more than 0.13 %. Keep the
   ability to *set* it for reference runs; do not document or synthesise it.
5. **Do not build the M13 directory.** It is on the Pareto frontier only when
   the window is built badly; with the fused window it costs 3.5× the area of
   32K for 16 % in one κ/one N. Its one genuinely attractive property — 98 % of
   its energy win comes from removed DRAM traffic — is already delivered by 32K
   in every cell where 32K helps.
6. **Keep every essential mechanism**: windowed dedup, M7 page cohorts (0.003
   mm², halves row activations), M8 index tiles (0.001 mm²), the R-slot pool
   (0.02–0.03 mm² of flops — the second largest single cost, and the one that
   makes dispatch possible).

Area budget for the recommended build: **≈ 0.67 mm² SRAM + ≈ 0.05 mm² control**
(nominal, 7 nm-class, L2 packing). Context: that is ≈ 1.8× a 1 MiB L2 in SRAM
terms — a large but not implausible fraction of an SM, and the reason the fused
layout matters.

---

## 11. RTL implementation readiness

**Stage 1 — ready to translate now** (behaviour is final, structures are
packed-friendly, no simulator artifacts):
window element table, line record array, page directory + mask, page-cohort
scheduler (2 runs), request records (`Pending` → 56 B, width-check pending),
slot arbitration, descriptor fetch, completion scoreboard (as a bitmap, not two
`std::set`s), address generation.

**Stage 2 — needs a cycle-accurate model before RTL** (the model abstracts the
cost): the seal/grouping pass (W accesses, 1 port), fanout at 4 elements/cycle,
the 4-way tag lookup with LRU, buffer hand-off.

**Stage 3 — needs architectural simplification first:** replace the sort-based
seal with the streaming hash grouping of §5; narrow `LineReq` from 256 to ≈ 96
bits; delete `sorted`/`order` by fusing the next-pointer into the element entry.

**Stage 4 — keep optional, behind a mode bit:** 2 × 32K capacity; the M13
directory (only if a future shared-L2 study shows the reuse cannot be served
there — the M13 report's own suggested follow-up).

**Stage 5 — discard:** `val_`/`addr_` maps, `WinRec`/`m13_dump`, the two
`std::set` scoreboards, the 16K configuration point, and the `reuse_ent` field
in `LineReq` (only M13 reads it).

No RTL was written and the Vortex reference was not touched.

---

## 12. Limitations and unresolved questions

1. **No synthesis, no floorplan, no SRAM compiler.** Every mm² figure is
   `bytes × 8 × assumed µm²/bit`. The assumptions are visible (§4) and the
   low/high spread is 3.3×, so *ratios* between configurations are far more
   trustworthy than absolute values.
2. **No power measurement.** §7 is an activity model with a published-ratio
   energy assumption; the mW figure in §7.3 is a labelled illustration.
3. **The L2 layout is a judgement call.** It is the honest cost of the structure
   as specified, but the recommended fused layout is 1.0×. Both are shown; the
   frontier is invariant, the M13 comparison is not.
4. **`val_`/`addr_` are estimated, not measured** — the size of a
   `std::unordered_map` node is library-dependent (≈ 32–40 B/entry assumed).
   This does not affect any hardware number, only the statement about the
   simulator's own footprint.
5. **One workload family.** All performance numbers come from the W1
   permutation gather (plus W0/PSCR/STRIDmix controls in earlier milestones).
   The 32K benefit in particular is workload-shaped; a graph or CSR workload
   with different reuse would move the crossover.
6. **The κ=0.75 regression mechanism is inferred** from per-client DRAM and L2
   counts, not measured per client (the model has no per-client L2 counter).
7. **Frequency is unspecified.** The 1 GHz used in §6 and §7.3 is a placeholder
   for arithmetic, not a claim.
8. **Open question worth one study:** is the shared L2 (1 MiB, measured 12.8 %
   hit rate on W1) sized correctly for a 1 MiB footprint? If the L2 is the real
   limiter, capacity there buys the same 66.8 % of repeats that M13's directory
   buys, with hardware the machine already pays for — and that is the only
   direction in which this audit sees the storage economics improve.

---

### Appendix A — reproducible calculations

```
# entry widths (measured, this ABI)
cd DX100_MEM/sim && g++ -std=c++17 -O2 -I src build/wm/audit_sizes.cpp \
  -o build/wm/audit_sizes && ./build/wm/audit_sizes

# per-buffer storage, any W:  16*W + 4*W + 32*L + 4*L + 16*D + 32*C
#   W = batch_window, L = distinct lines (<= W), D = page_dir, C = page_cohorts
# modelled charge (model):    bufs * (8*W + 16*D + 32*C)
# measured realized:          bulk_win_vec_bytes in section [16] of any log

# area:  mm^2 = bytes * 8 * um2_per_bit / 1e6
#   low 0.030   nominal 0.045   high 0.100  um2/bit
python3 -c "b=147584; print(b*8*0.045/1e6)"     # 0.0531 mm^2

# measured storage and cycles for every configuration
cd DX100_MEM/sim && python3 build/wm/m14_extract.py perf   # kappa sweep
                       python3 build/wm/m13_extract.py perf # M13 directory
```

### Appendix B — files used

| path | contents |
|---|---|
| `sim/src/sim.hpp` | every structure sized in §2 (`WinEnt`, `LineReq`, `Batch`, `FanJob`, `Pending`, `ReuseEnt`, `Cohort`) |
| `sim/src/bulk.cpp` | allocation sites (`reserve`/`resize`), the model charges (`bulk_win_bytes`, `bulk_idx_bytes`, `bulk_sched_bytes`), the `val_`/`addr_` writes |
| `sim/src/ab.cpp` | `[16]`/`[16b]` exports the audit reads |
| `sim/build/wm/audit_sizes.cpp` | the sizeof probe (new, measurement only) |
| `docs/m12-window-capacity-scaling-report.md` | the 6–9× finding this audit reconciles |
| `docs/mixed-kappa-buffer-capacity-report.md` | performance source for §8–§9 |
| `docs/m13-cross-window-reuse-report.md` | directory storage and cycles |

No simulator source, configuration or baseline was modified for this audit.
