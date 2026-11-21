# M25 — Is the separate DRAM organizer necessary?

**Question.** Under the physical-address mapping the architecture actually uses, does page
(cohort) organization already produce the DRAM row locality the engine needs, making a separate
global physical-order sort of the sealed window redundant? And if the answer depends on the
mapping, what policy is justified?

**Status.** Audit plus six new cells (§5). No architecture change, no source change, no historical
artifact modified. Tags: [M] measured, [A] derived from the decode arithmetic, [H] hypothesis.

---

## 0. Answer

1. **Under the production map the organizer is worth nothing**, and it cannot be: page order is row
   order up to a factor of two, and the controller's own FR-FCFS window already contains more
   open-row candidates per entry than the engine's 4 096-entry pool does (§3, §4).
2. **It is not worthless everywhere.** The same mechanism is worth **1.5 %** on Orkut under a
   page-permuted map (M24-A, reused) and **3.7 %** on `criteo_2k` at the production geometry
   (§5.2, new). Those are the two cases where the *unsorted* release order genuinely mismatches
   row order.
3. **The cost is out of proportion to that.** The organizer is `ford` + `fpos` = **114 688 B
   packed (16 % of the engine)** plus a 16 384-entry sort per window, charged at 0.98 cycles per
   run (805 876 cycles on Orkut = 13 % of the run).
4. **Recommendation: remove the global sort as a default; keep it behind a mapping/workload flag.**
   A universal removal throws away 1.5–3.7 % on the cases that do use it; keeping it universally
   pays 16 % of the engine for ≈0 on the production path. The mapping-aware rule is in §6.
5. **A correction to the prior record.** At 4 KiB pages and 1 KiB rows a page covers exactly two
   whole rows, so *two pages can never share a row* (§2.2). M24-A's phrase "M3 makes two virtual
   pages share a DRAM row" is not reproducible from the decode; what M3 does is destroy the
   page-order → row-order correlation. The brief's "pages share rows" case needs a **4 KiB row**
   geometry, which is why §5 adds it (one row = two pages).

---

## 1. What the release order actually is

Two independent orderings exist in the engine, and only one is selectable:

| ordering | produced by | storage | when it exists |
|---|---|---|---|
| **sealed order** | `win_seal` sorts the fused window by virtual line (dedup to one run per line) | none beyond `fent` | always |
| **physical order** (`ford`) | `win_order_fused`: `stable_sort` by `(row, bank, line)` where `row`/`bank` come from `mem_.dram_bank_row(pa_line_of_va_line(...))` | `ford` 57 344 B + `fpos` 57 344 B | `m24_dram_org = 1` and `bulk_org ∉ {0, 4}` |

`m24_dram_org = 0` leaves `ford` empty, `fpos` the identity and the pool's heap key the stored
position, so the release order is the sealed order and **nothing else changes** — same window, same
pool, same cohorts, same map, same caches, same FR-FCFS (the constructor enforces org 7/8 for this
switch so no earlier policy can be silently reinterpreted).

The DRAM controller's own scheduling is never touched by either setting: FR-FCFS over an
open-row-first, oldest-arrival-second policy on a queue bounded by the 64-entry L2 MSHR file
(`dram_max_q` = 63 in every cell). Removing the engine's organizer does not disable the
controller's.

---

## 2. The decode, precisely

### 2.1 The DDR model (`MemorySystem::dram_bank_row`)

With `il = dram_il_lines`, `B = banks`, `R = rows`, `row_bytes`, `L = 64 B lines`:

```
lines_per_row = row_bytes / 64                     // 16 at the production 1 KiB row
bidx          = L / il;  if (bank_hash) bidx ^= bidx>>6 ^ bidx>>12
bank          = bidx % B
row           = ((L / (il*B)) / (lines_per_row/il)) % R
```

At the production geometry (`row_bytes` 1024, `B` 2, `R` 4096, `il` 1, capacity 8 MiB):

```
bank = (L ^ L>>6 ^ L>>12) & 1        // alternates with the line
row  = L / 32                        // 32 lines per row = 16 per bank
```

so **one row = 32 consecutive lines = 512 B of one bank's share**, and the address space is
`[row][bank][line-in-row]`.

M24-A's other maps replace only the bank index:

* **M1** `bank = L % B` (adjacent lines alternate bank, no fold);
* **M2** `bank = (L ^ L>>5 ^ L>>11 ^ L>>17) % B` (bit 5 is the row bit, so bank is decorrelated
  from row parity);
* **M3** a seeded permutation of the **page frame** (`pa_page_of_vpn` → `m24_permute_page`), bank
  and row decode unchanged.

The HBM model (`hbm.cpp`) has its own four maps: A `col|row|bank|bg|pc|stack` (row-major, minimum
bank parallelism), B `col[il-1:0]|pc|col[hi]|bg|bank|row|stack` (GPU-style interleave, 4-line
channel chunks), C = B plus the documented XORs of bank/bg/channel with row bits, D = a page
permutation on top of a base map. The M24 production path is DDR, map M0; HBM is a separate
hierarchy (`dram_kind=hbm`) and is not the mapping under audit.

### 2.2 When does sorting by page group requests by DRAM row?

A 4 KiB page is 64 consecutive lines.

| geometry | lines per row | rows per page | pages per row | page order ⇒ row order? |
|---|---:|---:|---:|---|
| production (1 KiB row, 2 banks) | 32 | **2** | **0.5** | **yes**, exactly: page *p* owns rows 2p, 2p+1 |
| 4 KiB row, 2 banks, rows 1024 | 128 | 0.5 | **2** | **no**: one row spans two pages, so ordering by page leaves each row half-served twice |
| page-permuted (M3) | 32 | 2 | 0.5 | **no**: page *p* owns rows 2·π(p), 2·π(p)+1, and π has no order relation to *p* |

So:

* **Under the production map, ascending virtual page order *is* ascending physical row order**, as
  long as the page frames are allocated in virtual order (which the model's identity
  `pa_page_of_vpn` does, and which a linear `mmap`/huge-page allocation approximates). The engine's
  sealed order is therefore already an almost-perfect row order, and the organizer's sort can only
  reorder *within* the 32-line row and across the two rows of a page. That is the whole of its
  opportunity on this map.
* **Under M3 it is not**, and the organizer has real work to do.
* **At 4 KiB rows it is not either**, but for the opposite reason: page granularity is now *finer*
  than the row, so two pages must be merged to fill a row — a page-ordered release delivers half a
  row, leaves, and comes back. §5 measures exactly that.

**One further condition matters and is easy to miss:** the row also depends on the *bank hash*
(`bank_hash = 1`, as in Vortex's `VX_CFG_L2_BANK_HASH`). A page's 32 lines alternate bank, so a
32-line row is 16 lines of bank A and 16 of bank B interleaved — the two banks can both be serving
the same row of their own arrays at once, which is what makes the page-order ⇒ row-order
equivalence hold at all.

---

## 3. Why the organizer cannot pay on the production map

Three independent measurements say the same thing, and the third explains it:

1. **The organizer changes the release order but not the outcome.** M24-A's factorial, Orkut M0:
   cohorts + sealed order 6 130 484 cycles; cohorts + physical order 6 130 494 (+10 cycles,
   +0.0002 %) [M]. Row hits 389 823 → 389 893 (+70 of 644 000). The organizer's own 823 642 visits
   cost 805 876 cycles *inside those totals*.
2. **Full visibility does not help either.** M23-G6F let the release compare up to 4 096 candidates
   instead of the heap front: −0.038 % (Orkut), −0.009 % (LiveJournal), +0.015 % (ogbn) [M]. The
   best possible *release-side* choice is worth ≤ 0.04 %.
3. **The two candidate populations are disjoint.** G6F counted, per release decision, the open-row
   candidates in the DRAM queue (4.26 per 60.8 queued requests = 7.0 %) against those in the ready
   buffer (0.011 per 2 060 entries = 0.0005 %). **The buffer is 34× larger and 400× emptier of
   row hits.** Per entry it is ~13 000× less likely to hit an open row than the controller's own
   queue. A larger, better-sorted population of the *wrong* requests cannot help.

That is the structural reason: with the queue bounded by the L2 MSHR file at ~60 entries and
FR-FCFS free to reorder all of it, the controller is already extracting the row locality that
exists. The engine's sort acts on requests that have not been admitted yet, and the rows that are
open belong to requests already in the queue.

4. **Even with perfect translation the ceiling is ≈0.** M23-G3's ideal-translation cells put P2
   (physical order over the *complete* sealed window) at 6 076 339 cycles against P0's 6 077 523 on
   Orkut (−0.02 %), and 0.27 % / 0.10 % *worse* on ogbn-uniform and Criteo [M]. No admission, no
   cohorts, no pool — just the ordering — and it is worth 0.02 %.

---

## 4. The four mappings, side by side (M24-A, reused)

Cells B (cohorts, sealed order) and D (cohorts, physical order); the only difference between them
is `m24_dram_org`. D − B is the organizer's whole effect.

| workload | map | B cycles | D cycles | D/B − 1 | B row hit | D row hit | B acts | D acts |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Orkut | M0 production | 6 130 484 | 6 130 494 | **+0.0002 %** | 60.517 % | 60.529 % | 254 332 | 254 249 |
| Orkut | M1 line-interleaved | 6 125 884 | 6 125 221 | −0.011 % | 60.545 % | 60.562 % | 254 118 | 253 975 |
| Orkut | M2 XOR bank | 6 126 463 | 6 126 651 | +0.003 % | 60.527 % | 60.537 % | 254 125 | 254 053 |
| Orkut | M3 page-permuted | 6 128 013 | **6 035 778** | **−1.51 %** | 60.817 % | **62.947 %** | 253 164 | 239 042 |
| LiveJournal | M0 | 14 146 092 | 14 146 860 | +0.005 % | 46.140 % | 46.144 % | 375 452 | 375 452 |
| LiveJournal | M3 | 14 150 023 | 14 160 774 | +0.076 % | 46.256 % | 46.267 % | 374 590 | 374 807 |
| ogbn train | M0 | 98 542 852 | 98 563 897 | +0.021 % | 65.015 % | 65.037 % | 3 437 481 | 3 435 646 |
| ogbn train | M3 | 98 543 870 | 98 563 346 | +0.020 % | 65.035 % | 65.013 % | 3 435 786 | 3 437 711 |

Reading: M1 and M2 change the bank a line lands in, and therefore cannot change how many lines a row
owns — they move cycles by ≤ 0.011 %, which is the null result the brief predicts for a mapping that
does not disturb grouping. M3 is the only map that removes the page→row correlation, and it is the
only map on which the organizer pays (Orkut, −1.51 %, row hits +2.1 pp, activations −5.6 %). On the
other two workloads it still does not pay, because their pools hold too few distinct cells for the
sort to have anything to group (LiveJournal 11.3 cells in view, ogbn 7.6).

---

## 5. The missing mapping: one DRAM row spanning two pages

### 5.1 Why a new point was needed

At 4 KiB pages and 1 KiB rows, a page *contains* two rows (§2.2): no mapping can make two pages
share a row, because pages are page-aligned and cover whole rows. The brief's case 2 therefore
requires a different **row geometry**, not a different bank map. Doubling the row to 4 KiB
(`row_bytes = 4096`, `rows = 1024`, capacity unchanged at 8 MiB) gives

```
row = (L / (il*B)) / (lines_per_row/il) = L / 128
```

so one row spans lines 0–127 = pages 0 and 1. Six cells: two traces × two geometries ×
`m24_dram_org` 0/1, `bulk_org = 8`, everything else the frozen M24-A configuration
(`build/wm/m25_run.sh`, logs in `build/wm/m25/map2/`). No sweep, no architecture change.

### 5.2 Measured

| trace | geometry | sealed order (d0) | physical order (d1) | d1/d0 − 1 | d0 row hit | d1 row hit | d0 acts | d1 acts | organizer visits / cycles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut | 1 KiB rows (production) | 6 130 484 | 6 130 494 | **+0.0002 %** | 60.52 % | 60.53 % | 254 330 | 254 247 | 823 642 / 805 876 |
| Orkut | 4 KiB rows (2 pages/row) | 5 308 393 | 5 309 111 | **+0.014 %** | 83.52 % | 83.57 % | 106 284 | 105 945 | 823 827 / 805 920 |
| `criteo_2k` | 1 KiB rows (production) | 421 650 | **406 007** | **−3.71 %** | 42.29 % | 61.79 % | 8 627 | 5 729 | 13 097 / 20 684 |
| `criteo_2k` | 4 KiB rows (2 pages/row) | 406 402 | **399 588** | **−1.68 %** | 68.90 % | 77.64 % | 4 652 | 3 348 | 13 097 / 20 684 |

Three findings, one of which was not predicted by the brief's hypothesis:

1. **Doubling the row helps far more than any ordering does.** Orkut 6 130 484 → 5 308 393
   (**−13.4 %**) and `criteo_2k` 421 650 → 406 402 (−3.6 %) with the *same* release order. The
   geometry is doing the work that the organizer was supposed to do. [M]
2. **The shared-row mapping does not increase the organizer's value; it decreases it** (Orkut:
   +0.0002 % → +0.014 %; `criteo_2k`: −3.71 % → −1.68 %). With two pages per row, page order
   already delivers half a row at a time and the remaining half arrives from the neighbouring page,
   so the sort's marginal contribution falls. The brief's hypothesis — that shared rows would make
   the organizer pay — is **not** supported at this geometry.
3. **The organizer's value is a property of the stream, not of the mapping alone.** `criteo_2k` is
   the one workload where the *sealed* order is already scrambled relative to physical order
   (d0 row hit 42.3 %, the lowest in the matrix; M24-F reports 63.7 % at production settings), and
   it is the one where the sort pays 3.7 %. Orkut's sealed order is already near-perfect
   (60.5 % row hits) and the sort adds nothing at either geometry.

Caveat on magnitude: these `criteo_2k` cells run 421 650 cycles against 242 935 in M24-F because
this configuration is the frozen M24-A one (walkers 4, different descriptor bases), so the 3.7 % is
a property of *this* release path and should be re-measured at the production configuration before
any decision depends on it. The direction — the only cell where the organizer matters is the one
whose sealed order is worst — is consistent across both.

---

## 6. Mapping-aware policy

The evidence supports three regimes, and the engine can be told which one it is in without reading
any runtime state:

| regime | how it is known | policy | expected effect |
|---|---|---|---|
| **page-correlated placement** (linear/lowmem allocation, huge pages whose frames follow the virtual order) | a page-table property the kernel/allocator already knows; a compiler flag or a one-line driver check on the extent's PFNs | **sealed order** (organizer off) | 0 on Orkut/LiveJournal/ogbn; costs ≤ 0.02 % |
| **page-permuted or interleaved placement** (NUMA/page-colouring, random frame allocation, M3-like) | same check, opposite answer; or a profile sample of PFN order | **physical order** (organizer on) | +1.5 % (Orkut M3), and up to +3.7 % where the stream is scrambled |
| **row ≥ page** (any geometry where one row spans ≥ 2 pages) | the memory controller's own configured geometry | **physical order only if the stream is already scrambled**; otherwise sealed | +0.01 % when sealed, −1.7 % when scrambled |

Two implementation options, in order of preference:

1. **Build-time/instance-time choice.** The engine's map is a hardware property; the placement is
   a platform property. Make `m24_dram_org` a register field set once by the driver at context
   setup, default 0 (sealed order), with the 114 688 B `ford`/`fpos` arrays present only in the
   build that enables it. This is not "mapping-aware at runtime"; it is "the integrator decides",
   which is honest and costs nothing.
2. **Cheap runtime test (not recommended yet).** A per-kernel sample of the first window could
   compare the sealed order's row-hit rate against the physical order's; the difference is what the
   organizer would buy. The simulator can compute this from `ford` and the map with one pass over
   16 384 runs, but nothing in the current engine is wired to make a decision from it, and the
   measured spread (0 to 3.7 %) does not justify the mechanism.

**What must not be done:** remove `ford`/`fpos` and the `m24_dram_org` switch outright. That would
silently delete the only mechanism that pays on a permuted mapping, and it would make the M24-A
factorial unreproducible.

---

## 7. What this audit does not establish

* **The controller's reorder window is the L2 MSHR file (~60 entries).** The production
  configuration sets `dram_q = 0` (no additional queue bound), so the queue depth is whatever the
  MSHRs admit. A real controller with a smaller or larger reorder window would change the
  balance — the organizer's value rises as the controller's own window shrinks. This is untested
  here and is Experiment E3 of the proposal.
* **Writes are not modelled at all** (`MemorySystem` has no store path). Row locality for stores,
  write-combining and write-to-read turnaround are outside this audit.
* **Two of the four mappings are bank-only** (M1, M2) and therefore cannot move row grouping by
  construction; they are reported as nulls, not as evidence against organizing.
* **The 4 KiB-row cells are one configuration point** on two traces, not a sweep, and they use the
  M24-A frozen environment rather than the production one (§5.2 caveat).
* **No area, energy or timing claim** is made for removing the sort. M24-H priced `ford` + `fpos` at
  114 688 B packed and flagged the sort as the second-largest unmodelled logic block; deleting it
  removes that block, but this audit does not quantify the critical path.
