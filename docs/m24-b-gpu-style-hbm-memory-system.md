# M24-B — A GPU-style HBM memory system

**Question.** Does hierarchical organization of indirect memory requests — virtual-page
organization for translation and physical DRAM organization for memory service — provide
independent and complementary benefits under a GPU-style HBM memory architecture?

**Short answer.** Independence survives; complementarity is *conditional on the address
map*, and it flips sign. On the row-oriented map, explicit physical-DRAM organization is
worth −5.7 % on LiveJournal and −13.5 % on Orkut once the bank population is 32 banks per
channel, and the two factors are complementary (interaction −8.9 %…−11.8 % of baseline).
On the interleaved and XOR maps the same factor *costs* 2.5 %…10.4 % after page
organization, because a global physical-rank sort serializes 32 channels that the map had
just put to work. The page factor itself is large everywhere (33 %…81 % on Orkut), and
its mechanism is unchanged from M24-A: it is translation-admission and bank-occupancy
relief, not a DRAM effect. A 4 GiB HBM hierarchy with 32 pseudochannels therefore does
**not** replicate M24-A's "DRAM organization is inert" conclusion — it replaces it with a
mapping-dependent one.

## 1. Sources: what is documented, what is abstraction, what is assumption

Sources read for this milestone:

* NVIDIA, *CUDA Programming Guide* — Unified Memory (4.1) and Virtual Memory
  Management (4.18): <https://docs.nvidia.com/cuda/cuda-programming-guide/>
* AMD, *AXI HBM Controller (PG276)* — HBM Address Map and Protocol Considerations,
  HBM Topology, Product Specification:
  <https://docs.amd.com/r/en-US/pg276-axi-hbm/HBM-Address-Map-and-Protocol-Considerations>
  (with PG313's address-map options for the field-ordering claim)
* C. S. Lin, J. Qu, G. Saileshwar, *GPUHammer: Rowhammer Attacks on GPU Memories are
  Practical*, USENIX Security 2025:
  <https://www.usenix.org/system/files/usenixsecurity25-lin-shaopeng.pdf>


### 1.1 AMD PG276, *AXI HBM Controller* — HBM Address Map and Protocol Considerations

| documented fact | value |
|---|---|
| stacks | 1–2 per device, 1024-bit data each |
| channels per stack | 8, 128-bit, **one memory controller per channel** |
| pseudochannels | 2 per channel, 64-bit data each, **sharing** the command/address bus |
| 4H stack | 4 GB, 4 Gb/channel, 2 Gb/pseudochannel, **16 banks = 4 groups × 4 banks**, RA[13:0], CA[5:1] |
| 8H stack | 8 GB, 8 Gb/channel, 4 Gb/pseudochannel, **32 banks = 8 groups × 4 banks**, extra bank bit on SID |
| user address bits | 23 (4H) / 24 (8H) per stack |
| AXI map | bits [4:0] unused, HBM address at [27:5], AXI port [31:28], stack select [32]/[33] |
| protocol | "closely follows … DDR4": banks and bank groups, open row, tRCD/tRP/tRAS/tCCD/tRRD |

PG313 in the same family documents that the *order* of the ROW/BANK/COLUMN/BG fields is a
configuration choice (`ROW, BANK, COLUMN, BG0`; `ROW, BANK, COLUMN, BA2`;
`ROW, BANK, COLUMN`; or a user-defined RA/SID/BA/BG/CA order). That is the licence for
this milestone's four mapping policies: the hierarchy is documented, its bit order is not
fixed, and different orderings are exactly what a memory controller vendor exposes.

### 1.2 NVIDIA CUDA Programming Guide

Documented: every supported system uses a virtual address space with paging; a virtual
page is a fixed-size chunk mapped to a physical page; **NVIDIA GPUs support multiple
physical page sizes and prefer 2 MiB or larger**; x86_64 defaults to 4 KiB; Arm supports
4/16/32/64 KiB. The VMM API reserves VA and maps it to physical GPU memory later, so
VA→PA is a mapping and not an identity.

### 1.3 GPUHammer (USENIX Security 2025)

Documented: GPU physical addresses are not exposed even to a privileged user; the
physical→bank/row mapping is **proprietary**, recovered only by row-conflict timing; a
2 KB row is split into 256 B chunks that land in different banks/rows; the distance
between successive rows of one bank "does not appear to have any obvious trend", i.e. a
hash is involved. GPUHammer studies GDDR6 on an A6000; it is cited here for the
*methodological* facts (proprietary hashed maps, sub-row interleave granularity of a few
hundred bytes), not as an HBM specification.

### 1.4 What is therefore not claimed

No commercial GPU's decoding equation, capacity or timing is reproduced. The model is an
HBM-*style* hierarchy with PG276's documented bank-group and bank counts and the
documented stack→channel→pseudochannel topology, plus explicitly labelled assumptions.

### 1.5 Assumptions (all configurable, none claimed as device values)

| assumption | value | why |
|---|---|---|
| total capacity | 4 GiB | equal to the simulator's 4 GiB VA space, so the decode is injective over everything the traces touch |
| stacks × pseudochannels | 2 × 16 | PG276: 8 channels × 2 pseudochannels per stack |
| row size | 1 KiB (16 lines) | between the 1–2 KiB rows of current HBM parts; small enough that a 4 KiB page spans 4 rows |
| interleave granule (maps B/C) | 4 lines = 256 B | GPUHammer measures 256 B chunks on a real GPU |
| tRCD / tRP / tRAS | 12 / 12 / 28 | HBM-class ratios in the simulator's cycle unit |
| tCAS / tBURST | 12 / 4 | a 64 B line over a 64-bit pseudochannel |
| tCCD_S / tCCD_L | 2 / 4 | different / same bank group |
| tRRD_S / tRRD_L | 4 / 6 | different / same bank group |
| tFAW, refresh | off by default | implemented (t_faw, t_refi/t_rfc) and exercised by tests, not enabled in the matrix |

Consequences: a row hit costs 16 cycles, a conflict 40. These are *the model*, and every
timing comparison in this report is between cells that share them.

## 2. Hierarchy and address decoding

One `pa_line` is one 64 B physical cache line. Widths derive from the configured geometry
and are all powers of two:

```
cols   = row_bytes / 64            col_bits  = log2(cols)      = 4
banks  = hbm_banks_pg              bank_bits = log2(banks)     = 2
groups = hbm_bank_groups           bg_bits   = log2(groups)    = 2 or 3
pcs    = hbm_pcs (per stack)       ch_bits   = log2(pcs)       = 4
stacks = hbm_stacks                st_bits   = log2(stacks)    = 1
rows   = capacity / (stacks*pcs*groups*banks*row_bytes)
```

Capacity is held at 4 GiB in both bank configurations, so 16 banks × 8192 rows and
32 banks × 4096 rows differ only in organization — never in how much memory exists.

**Mapping A — row-oriented** (`hbm_map=0`), fields low→high: `col | row | bank | bg | pc | stack`.
Sixteen consecutive lines fill one 1 KiB row, and consecutive rows stay in the same bank:
maximum row locality, minimum bank parallelism.

**Mapping B — channel/bank interleaving** (`hbm_map=1`), fields low→high:
`col[il-1:0] | pc | col[cb-1:il] | bg | bank | row | stack`, with `il = il_lines = 4`:

```
col_lo = L & 3                         (the 256 B granule stays in one pseudochannel)
pc     = (L >> 2)  & 15
col_hi = (L >> 6)  & 3
bg     = (L >> 8)  & (groups-1)
bank   = (L >> 8+gb) & 3
row    = (L >> 10+gb) & (rows-1)
stack  = (L >> 10+gb+rb) & 1
```

so a 1 KiB row is spread over all 16 pseudochannels of a stack, 4 lines each.

**Mapping C — XOR/hash** (`hbm_map=2`) is B's layout plus three documented XORs:

```
bank' = bank ^ (row            & 3)
bg'   = bg   ^ ((row >> 2)     & (groups-1))
pc'   = pc   ^ ((row >> 2+gb)  & 15)
```

Each XOR is with a constant for a given row, so C is a bijection exactly where B is, and
it decorrelates bank/group/channel selection from row parity the way a proprietary GPU
hash does (GPUHammer's "no obvious trend").

**Mapping D — physical-page permutation** (`hbm_map=3`): the virtual page number is
permuted by a seeded four-round Feistel network over 20 bits (the same bijection M24-A
uses), then decoded with `hbm_base_map`. Only the page is permuted: offsets inside a page
are untouched, so alignment and the row/column structure inside a page are preserved and
no individual cache line is randomized.

**Injectivity.** A and B are pure bit-field extractions of a 26-bit line address; C is
that plus XOR with row-derived constants; D is a bijection composed with A/B/C. The test
suite checks, for both bank organisations and all four maps, that 2^17 consecutive lines
decode to 2^17 distinct (stack, pc, bg, bank, row, col) tuples with every field in range.

## 3. The two HBM configurations

| configuration | `hbm_bank_groups` | banks/channel | rows/bank | capacity | banks in the system |
|---|---|---|---|---|---|
| 16-bank (4H-style) | 4 | 16 | 8192 | 4 GiB | 512 |
| 32-bank (8H-style) | 8 | 32 | 4096 | 4 GiB | 1024 |

Both keep 2 stacks × 16 pseudochannels × 4 banks per group × 1 KiB rows. The 32-bank
configuration halves rows per bank to hold capacity constant, so a 32-bank cell has twice
the banks and half the rows *at the same address map* — which is exactly the comparison
"does a larger bank population help?" that §7 reports.

## 4. Page sizes

`page_bytes` ∈ {4096, 65536, 2097152} threads through every place a page is a unit:

| site | change |
|---|---|
| VPN decomposition | `vpn = va_line >> page_line_bits()` (6, 10, 15) at every front-end |
| physical allocation | `ppn = vpn + (64 MiB >> page_shift)` — a *byte* offset, so the PA window stays inside 4 GiB at every page size (a page-count offset would push 2 MiB pages to 38 GiB and alias them) |
| PA line | `(ppn << page_line_bits) | (line & (page_lines-1))` |
| page-table indexing | one PTE per page, `pte_line = LEAF_BASE + vpn/16`; a two-level walk at every page size (an assumption: a large page's PTE is still reached through two dependent fetches) |
| TLB tags and reach | tags are VPNs at the configured granularity, so reach = entries × ways × page_bytes and grows 16× or 512× |
| page-walk bookkeeping | unchanged in cost per walk; only the number of distinct VPNs changes |
| page cohorts | cohort unit = one page = `page_lines()` runs |
| window page directory | `vpn = vl >> page_line_bits()`; the 64-bit per-page line mask is a 4 KiB-era diagnostic that is only *reported*, and is now filled only when page_lines == 64 rather than silently mis-sized |

The 4 KiB path is byte-identical to M24-A: the historical D/M0 configuration reproduces
`6 130 494` cycles, `644 142` reads and `389 893` row hits exactly (`§9`).

## 5. The controller

One model controller per pseudochannel, which is PG276's arrangement read literally: each
pseudochannel owns its banks and its 64-bit data path, and only command/address is shared
between the two pseudochannels of a channel.

* **State per bank**: open row, activation cycle, earliest CAS (`bank_ready`), plus
  per-channel data-bus occupancy, last activation per bank group (`tRRD_L/S`), a 4-deep
  activation ring (`tFAW`) and a refresh timer.
* **Arbitration**: FR-FCFS over a **bounded window of 16 requests per channel** — open row
  first, then oldest arrival, then queue order. The bound is a real controller property
  (a scheduler compares a fixed number of entries per cycle), not a shortcut: it is
  documented, and it is why the sorting factor cannot see the whole queue.
* **Row hit**: `ready = max(now, data_bus) + tCAS + tBURST` = 16 cycles.
* **Conflict**: `pre = max(tRP, act_cycle + tRAS − now)`, then the activate obeys
  `tRRD_L`/`tRRD_S` against the last activation in the same/other bank group, then
  `ready = act + tRCD + tCAS + tBURST` = 40 cycles with the defaults.
* **Refresh** (optional): every `t_refi` each channel closes all banks for `t_rfc`;
  checked before the queue, so an idle channel refreshes too.
* **Parallelism**: `nch = 32` channels each issue at most one command per cycle, so the
  model can serve 32 lines per cycle; a request completes when *its* channel's data is
  ready, not when a global command slot frees.
* **Queue**: one shared arrival queue (`dram_q = 0` = unbounded), sliced per channel; the
  per-channel window is what the arbitration sees.

## 6. What was reused and what was new

Reused unchanged: both front-ends, dTLB/L2 TLB/walker, L1/L2 caches and MSHRs, the ready
pool, cohort admission (`bulk_org`), the release-order engine (`m24_dram_org`), the
completion path, and every existing counter. The page and DRAM factors are the same two
switches as M24-A.

New: `hbm.hpp`/`hbm.cpp` (`HbmController`: decode, per-channel queues, timing, stats), the
`dram_kind` selector, the page-size plumbing of §4, and delegation of
`dram_bank_row`/`dram_row_open` to the HBM decoder. That last point matters: the DRAM
factor of M24-A ranks releases by the controller's own physical order, so under HBM it
ranks by `(row, bank, channel)` of the HBM hierarchy with no new policy. `dram_kind=ddr`
remains the default and constructs none of it.

## 7. Results — the four-way factorial

80 cells, all on the same traces and frozen resources. `A` = both switches off, `B` =
page only, `C` = DRAM only, `D` = both. Every cell is one process; the table is
`build/wm/m24b/extract.txt`.

### 7.1 Orkut (row-oriented map, 16 banks = 4 groups × 4)

| cell | cycles | vs A |
|---|---|---|
| A | 22 552 854 | — |
| B | 15 081 335 | −33.13 % |
| C | 22 552 854 | +0.00 % |
| D | 15 081 335 | −33.13 % |

Page-only is −33 %; DRAM-only is *exactly* A. Under M0 the sealed release order is already
ascending physical order, so sorting by physical rank produces the identical sequence —
the factor is not weak here, it is a no-op, and C's cycle count is bit-identical to A.

### 7.2 Orkut, 32 banks — the complementary cell

| cell | cycles | vs A | vs B |
|---|---|---|---|
| A | 22 551 627 | — | |
| B | 15 045 295 | −33.29 % | |
| C | 22 537 139 | −0.06 % | |
| D | **13 013 054** | **−42.30 %** | **−13.51 %** |

`Interaction = D − B − C + A = −2 017 753` cycles (−8.95 % of A): combining is worth more
than the sum of the parts, because the 32-bank geometry is what gives the physical sort
something to sort — the sealed order now crosses 1024 banks instead of 512, and the
sort groups same-row requests that the page pipeline released apart.

### 7.3 LiveJournal

| banks | A | B | C | D | B/A | D/B | interaction |
|---|---|---|---|---|---|---|---|
| 16, M0 | 29 418 883 | 18 457 166 | 29 418 883 | 17 412 232 | −37.26 % | −5.66 % | −1 044 934 (−3.55 %) |
| 32, M0 | 29 416 404 | 18 395 675 | 29 414 122 | **14 929 240** | −37.46 % | **−18.84 %** | −3 464 153 (−11.78 %) |
| 16, M1 | 29 089 384 | 7 135 504 | 29 089 384 | 7 313 088 | −75.47 % | +2.49 % | +177 584 (+0.61 %) |
| 32, M1 | 29 089 380 | 7 135 295 | 29 089 380 | 7 309 183 | −75.47 % | +2.44 % | +173 888 (+0.60 %) |

### 7.4 ogbn-products (interleaved map, 16 banks)

| page | A | B | C | D | B/A | D/B | interaction |
|---|---|---|---|---|---|---|---|
| 4 KiB | 75 428 591 | 69 549 815 | 75 431 416 | 69 549 797 | −7.79 % | −0.00 % | −2 843 |
| 2 MiB | 75 361 008 | 69 463 360 | 75 361 008 | 69 463 360 | −7.83 % | +0.00 % | 0 |

ogbn is the workload whose page factor is small (7.8 %): its translation stall was already
low, so the page pipeline has little to recover, and its DRAM queue never grows deep
enough for a release-order change to matter.

## 8. Mapping sensitivity — why the interaction flips

| Orkut, 16 banks | A | B | C | D | B/A | D/B | interaction |
|---|---|---|---|---|---|---|---|
| M0 row-oriented | 22 552 854 | 15 081 335 | 22 552 854 | 15 081 335 | −33.13 % | +0.00 % | 0 |
| M1 interleaved | 21 858 571 | 4 139 774 | 21 858 571 | 4 570 523 | −81.06 % | +10.41 % | +430 749 (+1.97 %) |
| M2 XOR | 21 858 571 | 4 139 783 | 21 858 571 | 4 569 269 | −81.06 % | +10.37 % | +429 486 (+1.96 %) |
| M3 page-permuted | 21 858 811 | 4 148 631 | 21 858 810 | 4 150 488 | −81.02 % | +0.04 % | +1 858 (+0.01 %) |

The channel census explains all four rows:

| Orkut cell | channels used | banks used | row-hit rate | mean queue wait | cycles/read |
|---|---|---|---|---|---|
| M0, A | 4 | 5 of 512 | 61.0 % | 325 128 716 | 34.6 |
| M0, B | 4 | 5 | 60.8 % | 940 002 041 | 23.4 |
| M1, A | 32 | 512 | 59.7 % | 7 646 126 | 33.6 |
| M1, B | 32 | 512 | 59.7 % | 11 208 879 | 6.4 |
| M1, D | 32 | 512 | 59.7 % | 43 139 823 | 7.0 |

* **M0** concentrates every request in 4 pseudochannels and 5 banks. The DRAM is the
  bottleneck (queue wait 325 M cycles), the sealed order is already physical order, so
  the explicit sort changes nothing; the page factor is only −33 % because the banks it
  feeds are starved of parallelism.
* **M1/M2** spread the same traffic over all 32 channels and 512 banks. The page factor
  jumps to −81 % because both resources are now usable, and the DRAM factor becomes
  *negative*: a global `(row, bank, channel)` sort makes the pool's front one channel's
  row at a time, so the 32 channels that were serving in parallel now take turns. The
  queue wait rises 3.8× (7.6 M → 43.1 M) and the cost is +10.4 %.
* **M3** permutes pages so no two consecutive releases share a row; the sort finds little
  to group and costs +0.04 %.

The sign is therefore not a property of the mechanism alone. It is a property of the
mechanism *against the map*: physical-rank ordering helps when the natural order is
bank-poor and row-rich (M0 with a big bank population), and hurts when the natural order
is already channel-parallel (M1/M2).

## 9. Page size

Orkut, interleaved map, 16 banks:

| page | A | B | row-hit % | activations | rows used | acts/read | walks | L2 TLB hits |
|---|---|---|---|---|---|---|---|---|
| 4 KiB | 21 858 571 | 4 139 774 | 59.7 | 262 414 | 12 913 | 0.403 | 107 390 | 146 |
| 64 KiB | 21 836 920 | 4 129 428 | 36.1 | 552 803 | 35 790 | 0.639 | 106 287 | 221 |
| 2 MiB | 21 836 875 | 4 137 084 | 33.1 | 435 722 | 35 753 | 0.669 | 106 211 | 226 |

Two results, one expected and one not:

* **Translation does not improve.** Walks fall only 1.1 % (107 390 → 106 211) despite a
  512× larger page. The L2 TLB has 512 entries and is set-indexed by `vpn % 64`; orkut's
  page working set already exceeds it at 4 KiB, and at 2 MiB the ~1 100 distinct pages
  still exceed it, so the miss count is set by re-walks after eviction rather than by the
  number of distinct pages. Reach grows; misses do not fall. This is a property of the
  modelled TLB (16 entries × 4 ways per core, 512 × 8 ways shared) and of these traces,
  and it is reported as measured, not as a claim that large pages cannot help.
* **Row locality gets worse.** A 1 KiB row holds 16 lines, so a 4 KiB page spans 4 rows
  and a 2 MiB page spans 2 048. Under the interleaved map the larger page spreads its
  lines over all 32 channels, so each channel sees fewer lines per row: the row-hit rate
  falls 59.7 % → 33.1 % and activations per read rise 0.403 → 0.669. Cycles barely move
  (21.86 M vs 21.86 M) because the DRAM is not the bottleneck in cell A — the point of
  the metric breakdown is exactly that a worse hit rate need not cost cycles here.

The four-way cells at the other two page sizes (Orkut, interleaved map, 16 banks) show the
same ordering, so nothing above is an artefact of one geometry:

| page | A | B | C | D | B/A | D/B |
|---|---|---|---|---|---|---|
| 64 KiB | 21 836 920 | 4 129 428 | 21 836 920 | 4 412 232 | −81.09 % | +6.85 % |
| 2 MiB | 21 836 875 | 4 137 084 | 21 836 874 | 4 535 274 | −81.05 % | +9.62 % |

Page size therefore changes *which* resource is scarce without changing the total much —
and it does not rescue translation, because the limiting resource is TLB capacity, not
page count.

## 10. 16 banks versus 32 banks

| Orkut | 16 banks | 32 banks | Δ |
|---|---|---|---|
| M0 A | 22 552 854 | 22 551 627 | −0.01 % |
| M0 B | 15 081 335 | 15 045 295 | −0.24 % |
| M0 C | 22 552 854 | 22 537 139 | −0.07 % |
| M0 D | 15 081 335 | 13 013 054 | **−13.71 %** |
| M1 B | 4 139 774 | 4 139 598 | −0.00 % |
| M1 D | 4 570 523 | 4 596 887 | +0.58 % |

Doubling the banks is worth nothing to A, B or C and 13.7 % to D. That is the cleanest
statement of complementarity in the milestone: the extra banks are only *usable* by the
organization that exploits them, and the page pipeline alone cannot (it has no physical
rank to group by). LiveJournal shows the same shape: −14.26 % for M0 D, ≤0.01 % for the
interleaved cells.

## 11. Translation and cost breakdown

Translation is untouched by the HBM model: the dTLB, L2 TLB, walker and their timings are
the M24-A ones, and the HBM hierarchy only serves the misses. Consequently the page factor
is the same mechanism as M24-A (admission and MSHR relief), and the HBM model changes
only *what the released traffic does*.

| resource | value |
|---|---|
| ready pool | 4 096 slots × 16 B = 65 536 B (unchanged) |
| HBM bank state | 21 B/bank → 10.8 KB (512 banks) or 21.5 KB (1 024 banks) |
| per-channel state | ~120 B × 32 channels ≈ 3.8 KB |
| controller queue | 56 B/entry × queue depth (bounded by the 64 L2 MSHRs) ≈ 3.6 KB |
| release-order metadata | one 64-bit rank per pool entry, sorted per window (unchanged from M24-A) |
| scheduler work | one 16-entry window scan per channel per issued cycle; a channel with no ready bank is skipped by a single comparison |

The HBM model is therefore ~15–26 KB of state and no extra pipeline stage: the completion
path is the same one the DDR model uses.

## 12. Validation

* `make -j4` clean; **4 924 checks, 0 failures** in `./bin/dx100_tests` (4 861 previous +
  63 new in `m24b_hbm`).
* New tests: the two documented bank organisations; decode injectivity (2^17 lines × 4
  maps × 2 geometries) and field ranges; mapping A's contiguous row, B's 16-channel
  spread and 256 B granule, C differing from B but deterministic; D's seed sensitivity
  and offset preservation and its bijection over 4 096 pages; cold-activation, hit and
  conflict latencies (32/16/40 cycles); one command per channel per cycle (8 channels in
  one cycle); refresh off by default and firing when configured; page-size decomposition,
  TLB reach and PA offsets for all three sizes.
* **Identities over all 80 cells** (`tools/m24b_extract.py`): `hbm_reads == dram_reads`,
  `hbm_bytes == 64 × reads`, `hbm_ch_busy_sum == 4 × reads`, conflicts ≤ activations,
  hits/conflicts ≤ reads, reads ⇒ activations, `channels ≤ 32`, window ≤ 16 — **0
  failures**.
* **Determinism**: `m24b_orkut_D_16banks_map1_p4k` rerun is byte-identical.
* **Historical invariance**: the 4 KiB DDR configuration reproduces M24-A's Orkut D/M0
  cell exactly (6 130 494 cycles, 644 142 reads, 389 893 hits), and the full suite that
  contains the M24-A tests still passes.

## 13. Limitations

* **Coverage.** The design space is 3 workloads × 2 bank organisations × 4 maps × 3 page
  sizes × 4 cells = 288 cells; **80 were run**. ogbn appears only at the interleaved map
  (4 KiB and 2 MiB); LiveJournal is missing maps 2 and 3; the page-size sweep covers only
  the interleaved map. The missing cells are the natural next step and are not
  extrapolated.
* **Timings are assumptions**, not silicon: 12/12/28/12/4 with tFAW and refresh off. They
  are shared by every cell, so the comparisons hold, but no absolute number is a device
  measurement.
* **A global sort is not a GPU scheduler.** The M24-A DRAM factor reorders the *release
  order* of one software pipeline. A hardware controller sees a request queue and cannot
  reorder the producer; the factor is an upper bound on what address-aware release order
  could buy, and §8 shows that bound is negative under an interleaved map.
* **One command per channel per cycle**, data-bus occupancy through `tBURST` only; no
  read/write turnaround, no ECC, no bank-level refresh overlap.
* **The row-oriented map concentrates traffic** to 4 channels by construction; its low
  performance is a property of the map, not an accident of the model.
* GPUHammer's mapping facts come from GDDR6 on an A6000, not from HBM; they motivate the
  256 B granule and the hashed-map policy, they do not specify them.
* The 64-bit per-page line mask in the window directory is filled only for 4 KiB pages;
  it feeds one reported occupancy number and no decision.

## 14. Conclusions

1. **Page organization is the dominant factor under HBM too**, and its size depends on the
   map: −33 % with a row-oriented map (where the DRAM, not translation, is the bottleneck)
   and −81 % with an interleaved one on Orkut; −37 %…−75 % on LiveJournal; −7.8 % on ogbn,
   whose translation was already cheap.
2. **Explicit physical-DRAM organization is not inert under HBM — it is map-dependent.**
   With a row-oriented map and 16 banks it is exactly zero (C ≡ A). With 32 banks it is
   −13.5 % on Orkut and −18.8 % on LiveJournal, and it is **complementary** with page
   organization (interaction −8.9 %…−11.8 % of baseline).
3. **Under an interleaved or hashed map it is harmful**: +10.4 % on Orkut and +2.5 % on
   LiveJournal after page organization, because a global physical-rank order serializes
   the 32 channels the map just exposed (queue wait 7.6 M → 43.1 M cycles). The two
   mechanisms then *interfere*, and the correct engineering choice is to leave release
   order alone.
4. **Page size does not buy translation here.** Walks fall 1.1 % going from 4 KiB to 2 MiB
   because the TLB's capacity, not the page count, is the binding constraint; the larger
   page does cost row locality (59.7 % → 33.1 % hit rate) without changing cycles, because
   the DRAM was not the bottleneck.
5. **More banks only pay through organization**: 32 banks are worth ≤0.6 % to A, B and C
   and 13.7 % to D on Orkut.

Answering the research question: the two levels are independent — page organization works
through translation admission and MSHR/bank occupancy, physical organization through
row grouping — and their complementarity is real but **conditional on the physical address
map**, with the sign set by whether the natural release order is bank-poor (helpful) or
already channel-parallel (harmful). M24-A's DDR conclusion, that the DRAM factor is inert,
is a property of a two-bank model with one command per cycle; under a 32-pseudochannel HBM
hierarchy the same factor is worth −13.5 %…−18.8 % of baseline in the configuration that
can use it, and costs 10 % in the one that cannot.

## 15. Reproduce

```
cd sim && make -j4 && ./bin/dx100_tests            # 4 924 checks, 0 failures
cd ..  && bash build/wm/m24b_run.sh smoke          # 24 validation cells
cd ..  && PAR=6 bash build/wm/m24b_run.sh main     # 80-cell matrix (restartable)
cd ..  && python3 tools/m24b_extract.py            # tables, identities, summary.csv
```

One cell by hand:

```
cd DX100_MEM/sim && ./bin/dx100sim --config configs/m23g6b_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin --set dram_kind=hbm \
  --set hbm_bank_groups=8 --set hbm_map=1 --set page_bytes=4096 \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 \
  --set g6b_pa_capacity=4096 --set walkers=8 --set page_cohorts=8 \
  --set dtlb_mshr=8 --set walk_latency_per_level=150 \
  --set g6i_diag=1 --set sched_vis=0 --set sched_group=0 \
  --set bulk_org=8 --set m24_dram_org=1 --all
```

Artifacts: `build/wm/m24b/*.txt` (80 cell logs), `build/wm/m24b/summary.csv`,
`build/wm/m24b/extract.txt`, `build/wm/m24b/design.md` (the pre-implementation
architecture proposal), `build/wm/m24b_run.sh`, `build/wm/m24b_cell.sh`,
`build/wm/m24b_main.log`, `tools/m24b_extract.py`, `sim/src/hbm.{hpp,cpp}`.
No M23 or M24-A artifact was modified; the M24-A cell above was re-run only to prove the
4 KiB path still reproduces it.
