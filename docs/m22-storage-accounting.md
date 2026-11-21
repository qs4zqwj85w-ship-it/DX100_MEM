# M22 — Storage Accounting

Exact storage for every implemented structure, at 2×8K and 2×16K. Entry widths
are taken from `sim/src/sim.hpp` and were measured with the repository's own
`sizeof` probe (`sim/build/wm/audit_sizes.cpp`, built and run for this audit).
Three numbers are reported per structure — **declared bits**, **ABI bytes** (what
the simulator allocates) and **packed-minimum bits** (what an RTL build of the
same fields needs) — because the ABI figure includes C++ padding that hardware
would not pay.

Tags: **[M]** measured · **[C]** calculated from the declared fields · **[A]**
assumed (labelled modelling choice).

---

## 1. Which representation the final architecture actually uses

This matters more than any individual entry width.

* `win_fused` **defaults to `false`** (`sim.hpp:310`).
* Every M18, M19, M20 and M21 cell reports `bulk_win_fused_vec_bytes=0`
  **[M]**. The measured performance of the "final architecture" therefore comes
  from the **unfused** representation: element table + permutation + 32 B line
  records + issue order.
* The fused representation exists, is selectable, and was measured equivalent in
  M15 (`docs/fused-window-representation-report.md`), but it is **not** what any
  published number was produced with.

So the audit reports two hardware builds:

| build | element storage | line storage | status |
|---|---|---|---|
| **U** — unfused (shipped default) | 16 B `WinEnt` | 32 B `LineReq` + 4 B permutation + 4 B order | what M18–M21 measured |
| **F** — fused (`win_fused=1`) | 8 B word (line 26 \| position 32 \| offset 6) | 4 B `fmeta` + 1 B `fslot` + 1 B `fflag` | implemented, M15-verified, not default |

## 2. Per-entry widths

| record | declared fields | declared bits | ABI bytes [M] | packed min [C] |
|---|---|---:|---:|---:|
| `WinEnt` | `u64 va_line`, `u32 elem`, `u16 off`, `u8 width` | 120 | **16** | 108 (line 27, elem 17, off 6, width 2, +pad 8) |
| `LineReq` | `u64 va_line`, `u32 first`, `u32 count`, `u32 seq`, `u32 slot`, `bool issued`, `u32 reuse_ent` | 232 | **32** | 111 without M13's `reuse_ent`, 143 with it |
| `Batch::Cohort` | `u32 pos`, `u32 end`, `u64 vpn`, `u64 resume_at`, `bool open` | 200 | **32** | 168 |
| `FanJob` | `u32 batch`, `u32 line`, `u32 pos`, `u8 bytes[64]` | 608 | **76** | 608 (data dominates) |
| `Pending` | kind, `va_line`, `elem`, `off`, `width`, `seq`, `win_batch`, `win_line`, `reuse_ent` | ~360 | **56** | ~44 |
| `BulkDesc` | 8 fields, 64 B on-media format | 320 | **40** | 40 |
| fused `fent` | line 26 \| pos 32 \| off 6 | 64 | 8 | 64 |
| fused `fmeta` | start 16 \| count 16 | 32 | 4 | 32 |
| fused `fslot` / `fflag` | slot ≤ 255 / issued bit + pad | 8 / 8 | 1 / 1 | 7 / 1 |

`LINE_BYTES = 64`, so a page holds 64 lines and `dir_mask` is one 64-bit word.

## 3. Window storage (the dominant term)

Per buffer, worst case (every element on its own line, L = W), D = 512 pages,
C = 2 cohorts, plus ~56 B of batch control:

### Build U — unfused (the shipped default)

| structure | entries | ABI bytes @8K | @16K |
|---|---:|---:|---:|
| element table (`ent`, 16 B) | W | 131 072 | 262 144 |
| permutation (`sorted`, 4 B) | W | 32 768 | 65 536 |
| line records (`lines`, 32 B) | L ≤ W | 262 144 | 524 288 |
| issue order (`order`, 4 B) | L ≤ W | 32 768 | 65 536 |
| page directory (`dir_vpn`+`dir_mask`, 16 B) | 512 | 8 192 | 8 192 |
| page cohorts (32 B) | 2 | 64 | 64 |
| batch control | 1 | 56 | 56 |
| **per buffer** | | **467 064** | **925 816** |
| **both buffers** | | **934 128** | **1 851 632** |

### Build F — fused

| structure | entries | ABI bytes @8K | @16K |
|---|---:|---:|---:|
| fused entry (8 B) | W | 65 536 | 131 072 |
| `fmeta` (4 B) | L ≤ W | 32 768 | 65 536 |
| `fslot` (1 B) | L ≤ W | 8 192 | 16 384 |
| `fflag` (1 B) | L ≤ W | 8 192 | 16 384 |
| page directory | 512 | 8 192 | 8 192 |
| cohorts + control | 2 + 1 | 120 | 120 |
| **per buffer** | | **123 000** | **237 688** |
| **both buffers** | | **246 000** | **475 376** |

### Cross-check against the measured runs

| cell | build | predicted [C] | measured [M] |
|---|---|---:|---:|
| w4_duplow, 2×8K, every element its own line | U | 934 128 | **933 888** (`bulk_win_vec_bytes`) |
| bcsstk25, 2×8K (264 lines/window) | U | 934 128 (worst case) | 353 232 (realized) |
| bcsstk25, 2×16K | U | 1 851 632 (worst case) | 689 608 (realized) |
| w4_duplow, 2×8K | F | 246 000 | 0 (not the shipped path) |

The 240 B difference on w4 is `std::vector` capacity rounding, exactly as M14
found. Two important consequences:

1. **The worst case is reachable, not theoretical.** w4 (8 MiB scattered target
   set) fills every window with 8 192 distinct lines and realizes 933 888 B — 2.8 %
   of the 32 KiB L1+L2 cache hierarchy's area equivalent, for one workload family.
2. **Build F is 3.80× smaller at the same window size** (934 128 → 246 000), and
   **F at 16K (475 376 B) is half of U at 8K (934 128 B)**. The capacity the
   performance work wants is affordable only in build F.

## 4. Engine-wide structures (independent of W)

| structure | entries | bytes [C] | port / kind [A] | scales with |
|---|---:|---:|---|---|
| index ring `IdxRing` (256 slots × 21 B: va 8, elem_base 8, cnt 2, used 2, ready 1) | 256 | 5 396 | 1R1W SRAM + small regs | idx_buf_lines, **not W** |
| request records `info_` (56 B) | R = 128 | 7 168 | flops / register file | **R** |
| slot free list + owner | 128 | 640 | LIFO stack | R |
| descriptor queue (`BulkDesc` 40 B) | depth | 40 × depth | FIFO | descriptor rate |
| fanout queue (`FanJob` 76 B incl. 64 B line) | ≤ R | ≤ 9 728 | FIFO of line images | outstanding fills |
| run state (desc + ring + cursors) | 1 | ≈ 200 | flops | — |
| counters, hashes, scheduler scalars | ~120 | ≈ 2 000 | flops | — |
| **total** | | **≈ 25 KB** | | |

Fixed structures are **2.7 % of the 2×8K window** in build U and **1.3 % of 2×16K**
in build F. The window is the entire cost story.

## 5. Structures whose cost scales with window size

Everything in §3 rows 1–4 (build U) or 1–4 (build F): element table, permutation
(U only), line records, order (U only), and the fused per-line arrays (F only).
Doubling W doubles each.

**Fixed** regardless of W: page directory (512 entries), cohorts (2), index ring
(256 slots, set by `idx_buf_lines`), request records (R = 128), free list,
descriptor queue, fanout queue depth (bounded by R), counters.

## 6. Two structures that should not be built as they stand

### 6.1 The page directory is dead state, 16 384 B for two buffers

`dir_vpn` / `dir_mask` are written at seal (`bulk.cpp:1236-1249`) and read only by
a collision counter and an occupancy check. **No issue decision reads them.**
Page runs are found by scanning `order` (`win_open_cohort`), not by the directory.

It would also be unusable as a functional structure at the footprints that matter.
It is direct-mapped (`s = vpn % page_dir`) with 512 entries, and the measured
occupancy per window on w4_duplow is **`dir_occ_max = 6 391`** [M] — 12× the table.
Every one of those is a collision that would thrash a real directory.

**Verdict: delete.** It costs 8 192 B per buffer to produce statistics, and the
functional page grouping comes from the sealed order, which is sorted anyway.

### 6.2 The index ring stores no index data

`idx_take` returns `*line = mem_.line_bytes(...)` — a pointer into the **cache
array**. The engine keeps only `(va, elem_base, cnt, used, ready)` = 21 B/slot.
The modelled charge `bulk_idx_bytes` is 2 720 B [M]; the realized ring is 5 396 B;
neither includes the 64 B line images the address generator reads from.

Hardware must choose one of:

* buffer the lines: 2 × 64 lines × 64 B = **8 192 B** of data SRAM (+ the 5 396 B
  of metadata), or
* accept one L2/L1 read port per AG cycle for the index operand.

The measured peak ring occupancy is 128 lines and 2 048 ready elements [M], so the
2 × 64-line choice is the right size; the audit costs it at +8 KiB, which is 1 % of
build F's 2×8K window and 4 % of the fixed structures.

## 7. Total

| configuration | window | fixed | **total** | = | vs 1 MiB L2 |
|---|---:|---:|---:|---:|---:|
| U, 2×8K | 934 128 B | 25 600 B | **959 728 B** | 937 KiB | 0.92× |
| U, 2×16K | 1 851 632 B | 25 600 B | **1 877 232 B** | 1.79 MiB | 1.79× |
| F, 2×8K | 246 000 B | 33 792 B¹ | **279 792 B** | 273 KiB | 0.27× |
| F, 2×16K | 475 376 B | 33 792 B¹ | **509 168 B** | 497 KiB | 0.49× |

¹ fixed costs include the +8 KiB index line buffers of §6.2.

The M21 workloads' representative cases, with the two buffers' occupancy as
measured:

| workload | realized U @8K [M] | F @8K [C] |
|---|---:|---:|
| w4_duplow (all lines distinct) | 933 888 | 246 000 |
| w8_wslarge | 926 640 | 246 000 |
| w1_uniform | 491 216 | 246 000 |
| w3_duphigh (64 target lines) | 348 672 | 246 000 |

Note the asymmetry: build F is *constant* in the target distribution (its per-line
arrays are sized L ≤ W and allocate the worst case) while build U ranges 348–934 KB
because its line-record array tracks distinct lines. For a fixed RTL budget, F is
the only build whose size does not depend on which workload it meets.
