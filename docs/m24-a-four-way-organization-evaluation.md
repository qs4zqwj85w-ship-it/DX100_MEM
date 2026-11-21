# M24-A — Four-way page and DRAM organization evaluation

**Question.** Do virtual-page/translation organization and explicit physical-DRAM organization
each buy performance on their own, and does their combination buy more than the sum?

**Design.** A 2×2 factorial — page organization ON/OFF × explicit DRAM organization ON/OFF — run
on three workloads and four physical mappings, with every other resource frozen: same traces,
window, translation latency, TLB/MSHR/walker counts, pool capacity, caches, DRAM geometry, timing,
FR-FCFS and queue. 44 of the 48 planned cells were completed (the four ogbn M2 cells were cancelled
once M1/M2 had shown identical, ≤0.08 % behaviour elsewhere — §11), plus a SIMT reference on the same
frozen resources over four mappings (8 cells).

**Two switches, no redesign.** The audit (§1) found both mechanisms already present and already
independent, so the milestone needed two knobs and no new policy:

| factor | ON | OFF |
|---|---|---|
| page organization | `bulk_org = 8` — page-cohort admission, `page_cohorts = 8` descriptors, shared translated pool | `bulk_org = 7` — one cursor over the sealed run array, in stored order, no cohorts |
| explicit DRAM organization | `m24_dram_org = 1` — `ford` sorted by the controller's own `(row, bank, line)`, so the pool's heap front is the global physical minimum | `m24_dram_org = 0` — `ford` empty, release order = sealed order (ascending line, P0's published order) |

The second switch is new (§5). It is the *only* thing it changes: no sort, no physical rank, no
organizer pass. FR-FCFS, the queue, the map, the caches, the pool and the cohort table are
identical in all four cells.

**The short answer** (full evidence in §6–§9):

* Page organization is the whole effect: it turns the same engine from **21 991 083 cycles into
  6 130 484 on Orkut M0** (−72.1 %) and from 17.9 M SIMT cycles into a 2.9× win. Without it the
  bulk engine is 23 % *slower* than the SIMT baseline it is meant to beat.
* Explicit DRAM organization is worth **+10 cycles of 6.13 M** on Orkut under the historical map
  (0.0002 %), i.e. nothing, and it cannot be worth more: with `dram_q = 0` the controller's queue is
  unbounded, FR-FCFS already reorders the whole population, and the neutral release order is itself
  a nearly linear physical order.
* The factorial interaction is therefore ≈ −(DRAM-only saving) in ten of eleven cells: page-only is
  the entire gain, DRAM-only is ≈ 0, and combining them reproduces page-only. The exception is
  Orkut/M3, where the DRAM factor is real (1.51 %) and the interaction is −92 156 cycles
  (complementary — §8).
* The mapping experiments are the informative part. M1/M2 move only the bank index and change
  cycles by ≤0.08 % — they cannot change how many lines a row owns. M3 (a seeded permutation of
  physical page frames) is the one map that makes two virtual pages share a DRAM row, and it is the
  only configuration in which the DRAM factor pays: **−1.51 % on Orkut**, where multi-page cells in
  the pool go from 0 to 71 per sample. On LiveJournal and ogbn the same map yields too few shared
  cells in view (0.8 and 0.3 per sample) and the factor costs 0.08 % / 0.02 %. The brief's hypothesis
  is confirmed in direction and wrong in magnitude: it stays 48× smaller than the page factor.

## 1. Source audit

### 1.1 What already exists — and is not the experiment

The baseline is not a strawman. `dispatch = 0` runs the plain SIMT front-end on the full memory
system, and every one of the following is present in **all four** arms and in the SIMT reference:

| mechanism | where | state in every cell |
|---|---|---|
| dTLB (16 × 4), L2 TLB (512 × 4), 2-level walk @ 150 cycles, 8 walkers, 8 dTLB MSHRs, 8 L2-TLB MSHRs | `memory.cpp: tlb_probe/l2tlb_tick/ptw_tick` | identical |
| MSHRs: L1 128, L2 64; `R = 128` outstanding line requests | `sim.hpp`, `memory.cpp` | identical |
| L1 256 × 4 @ 2 cy, L2 16 384 × 8 @ 4 cy | `memory.cpp` | identical |
| DRAM: 2 banks × 4096 rows × 1 KiB, `t_hit` 14, `t_miss` 26, `link` 0 | `configs/m23g6b_base.cfg` | identical |
| **FR-FCFS** arbitration, one command/cycle, open-row-first then oldest-arrival | `memory.cpp: dram_tick` | **always on, never toggled** |
| window 16 384, dedup to one run per line, P0 page order (`win_seal`) | `bulk.cpp` | identical |
| the 4 096-entry shared translated pool (`g6b_pa_capacity`) and its capacity rule | `bulk.cpp: win_issue_g6b` | identical |

Two properties of the historical control matter for reading everything below, and both are
inherited, not introduced:

1. **`dram_q = 0` means an unbounded DRAM queue.** The controller therefore sees *every* request the
   hierarchy has admitted and can reorder all of it by open row. Any engine-side ordering is layered
   on top of a controller that is already maximally free to group.
2. **The neutral release order is not "random".** For the bulk engine the sealed order is ascending
   line, and under M0 the map is close to linear, so ascending line ≈ ascending row. Cell C is
   therefore "no organizer" but not "no locality"; §8 quantifies what is left when a mapping (M3)
   removes that accidental locality.

### 1.2 The two factors, and the exact switches

**Page organization** is the translation-side admission policy, and it has two interchangeable
implementations over the same sealed run array:

```cpp
// bulk.cpp, win_issue_g6b, step (2)
if (g6c) win_admit_g6c(cycle, b);          // bulk_org = 8: page_cohorts descriptors,
                                           // oldest-runnable first, refusal parks that page
                                           // and the same cycle admits another page
else while (tbudget && b.g6b_tr_next < n) { // bulk_org = 7: ONE cursor in stored order,
    if (ready.size() + pend >= cap) break;  //   a refusal ends the cycle
    ... mem_.submit(...) or break;
}
```

Both push completed translations into the same pool and both release through the same
`win_emit_fused`. The only difference is how many virtual pages may be translating at once.

**Explicit DRAM organization** is the release order out of that pool, and it is decided in one place,
`BulkEngine::win_order_fused`, which fills `ford` — the run permutation that the pool's heap front
follows:

```cpp
void BulkEngine::win_order_fused(Batch& b) {
  b.ford.clear();
  const u32 org = cfg_.bulk_org;
  if (!cfg_.m24_dram_org) return;   // M24-A: OFF -- sealed order, no physical rank
  ...
  std::stable_sort(..., [&](u32 x, u32 y) {          // ON -- the controller's own key
    if (row[x] != row[y]) return row[x] < row[y];
    if (bank[x] != bank[y]) return bank[x] < bank[y];
    return x < y;                                     // line order inside the cell
  });
}
```

`row[]`/`bank[]` come from `mem_.dram_bank_row(mem_.pa_line_of_va_line(run_line(b, i)), ...)` — the
controller's own decode, applied to the *translated* physical line. That is why G6H measured that the
pool's heap front is already the global physical minimum: the heap is keyed by the rank in `ford`.

### 1.3 Not counting the physical ordering twice

There is exactly one ordering mechanism in the code and exactly one knob:

* `win_order_fused`'s sort (cell D and C: one organizer visit per run, `bulk_org_runs = n`);
* the pool's heap key, which is that rank (no separate mechanism);
* FR-FCFS in the controller, which is **never** switched by this experiment and is present in all
  cells including the SIMT reference.

Cell B has `bulk_org_runs = 0`: the organizer does not run at all, so no run carries a physical rank
and the release order is the sealed one. Cell D counts the sort once. Nothing is counted twice.

### 1.4 The M11.3 `dram_map = 1..5` could not be used

M23-G4C rejected them for a precise reason and this milestone inherited it: they are applied at the
**enqueue site** (`memory.cpp:407`), i.e. after a request is already in the controller's queue, so the
engine's organization key would disagree with the map the DRAM actually uses — and `BulkEngine`'s
constructor throws for `bulk_org ≥ 2` with `dram_map ≠ 0`. The four M24 mappings are therefore
implemented in the **single decode function** `MemorySystem::dram_bank_row`, which every agent calls
(controller, row-open probe, engine key), and, for M3, in the **single PA function**
`pa_page_of_vpn`, which every producer and consumer of a physical address calls. One map, one place.

### 1.5 What this design cannot isolate — stated before the results

These are the honest limits of the matrix; §10 repeats them with numbers.

1. **"Page organization OFF" inside the engine is not the SIMT baseline.** With one cursor, a dTLB
   refusal ends the cycle, so the arm serialises on a page — 21.99 M cycles on Orkut against
   17.90 M for SIMT. The factorial's A is an engine-internal control (the brief's "page organization
   OFF"); the SIMT row is the external reference, and both are reported in every table.
2. **M3 permutes every page**, including the descriptor page and the 8 MiB index array, because the
   model has one mapping function. A real system could place those contiguously. The cost is
   measurable and small (§7: index traffic is `bulk_idx_*`, and the DRAM client split attributes it).
3. **M1 and M2 can only move the bank index.** At `banks = 2`, `row_bytes = 1024` a row always owns
   32 lines (16 per bank), so no bank map can change how many lines a row owns or the size of a
   `(bank,row)` cell. They test bank balance, not grouping. M3 is the only map that changes
   *which* pages share a row.
4. **`dram_q = 0` (unbounded) is the historical control.** It gives the controller the maximum
   freedom to reorder, so it is the configuration *least* likely to reward engine-side ordering. A
   finite queue would raise the value of ordering; it is not part of this matrix (documented as the
   first follow-up in §11).

## 2. Configurations

One pipeline, two switches. Everything outside the two marked stages is shared by all four cells:

```
   trace ──► window 16 384 ──► seal (dedup: one run per line, stored order) ──┐
                                                                             ▼
                        ┌──────────── [ P ] page organization ────────────┐
                        │ ON  8 page-cohort descriptors, oldest-runnable    │
                        │     first, refusal parks that page only          │
                        │ OFF one cursor, stored order, refusal ends cycle  │
                        └───────────────────────┬─────────────────────────┘
                                                ▼  probes
                              dTLB 16x4 ── L2 TLB 512x4 ── 8 walkers @150
                                                │  completions
                                                ▼
                        ┌──────── shared ready pool, 4 096 x 16 B ────────┐
                        │ heap keyed by the release rank of each run      │
                        └───────────────────────┬─────────────────────────┘
                        ┌──────────── [ D ] DRAM organization ─────────┐ │
                        │ ON  rank = (row, bank, line) of the           │ │ release
                        │     translated PA  (one sort per window)      │◄┘
                        │ OFF sealed order: ascending line, P0's order  │
                        └───────────────────────┬───────────────────────┘
                                                ▼
                        L1 256x4 ── L2 16384x8 ── L2 MSHR 64 ── DRAM queue (unbounded)
                                                └─► 2 banks x 4096 rows, FR-FCFS
```

| Cell | page org | DRAM org | `bulk_org` | `m24_dram_org` | what it is |
|---|---|---|---|---|---|
| **A** | OFF | OFF | 7 | 0 | in-order admission, release in sealed order |
| **B** | ON | OFF | 8 | 0 | cohort admission, release in sealed order |
| **C** | OFF | ON | 7 | 1 | in-order admission, release in physical order |
| **D** | ON | ON | 8 | 1 | cohort admission, release in physical order — the shipped G6H/G6I pipeline |

Cell D with M0 is the historical baseline and must reproduce it exactly; it does (§9: 761 keys, 0
differ versus `build/wm/m23g6h/g6h_G0r_*.txt`).

## 3. Mappings

All four keep the DRAM hierarchy, capacity (8 MiB), timing and queue identical. `L` is the 64 B
physical line number, `banks = 2`, `rows = 4096`, `row_bytes = 1024` (16 lines per bank-row), so a row
index spans 32 consecutive lines and the address map repeats every 8 MiB.

**M0 — historical** (`memory.cpp: dram_bank_row`, unchanged):

```
bank = (L ^ (L >> 6) ^ (L >> 12)) mod 2
row  = (L >> 5) mod 4096
```

A 4 KiB page is 64 lines, so page `p` owns rows `2p` and `2p+1`, both banks: four 16-line cells,
all inside that page. Two pages share a row only if their page numbers differ by 2048 (8 MiB).

**M1 — line-interleaved, no XOR fold**:

```
bank = L mod 2
row  = (L >> 5) mod 4096          (unchanged)
```

Adjacent lines alternate banks, exactly as in M0; the difference is that M0's fold adds a
page-constant term to bit 0 (`L>>6` and `L>>12` are constant inside a page), so M0 and M1 differ by a
per-page bank reflection. This is the mapping the brief calls "adjacent cache lines across banks" —
and the audit point is that **M0 already is that map**; M1 is the same rule without the fold.

**M2 — XOR fold over four address bits**:

```
bank = (L ^ (L >> 5) ^ (L >> 11) ^ (L >> 17)) mod 2
row  = (L >> 5) mod 4096          (unchanged)
```

Bit 5 is the *row* bit, so folding it into the bank decorrelates bank from row parity: consecutive
rows alternate the bank that owns their low half. Bits 11 and 17 add page and 8 MiB-period terms.

**M3 — seeded physical-page permutation**, offsets preserved, decoded by M0:

```
vpn  = VA >> 12                  (20 bits: the VA space is 4 GiB)
(a, b) = (vpn >> 10, vpn & 1023)
for r in 0..3:   (a, b) = (b, a ^ h(b, k_r))        h = a 10-bit mix of (x ^ k_r)
frame = (a << 10) | b
pa    = ((PPN_OFFSET + frame) << 6) | (L & 63)
```

A four-round Feistel network over two 10-bit halves is a **bijection for any round function**, so
for any seed no frame is lost, none is produced twice, and the 64 lines of a page stay contiguous —
the permutation only decides *which* frame a page occupies. `m24_perm_seed` selects it. Under M3 two
pages share a physical row whenever their frames differ by 2048, which is now common rather than
never — this is the map that tests the "less-aligned mapping" hypothesis.

`pa_page_of_vpn` is the single PA producer: `pa_line_of_va`, `tlb_fill`, the L2-TLB fill and the
ideal-translation path all call it, so the TLBs, the engine's key and the controller cannot disagree.

## 4. Method

* **Workloads**: Orkut S1 BFS, LiveJournal BFS, ogbn-products train — the frozen M23 traces
  (`build/wm/m23_*.bin`), same seeds and element counts.
* **Frozen**: `configs/m23g6b_base.cfg`, window 16 384, `walkers = 8` (Orkut) / 4 (others),
  `page_cohorts = 8`, `dtlb_mshr = 8`, `walk_latency_per_level = 150`, `g6b_pa_capacity = 4096`,
  `sched_vis = 0`, `sched_group = 0`, `dram_q = 0`, FR-FCFS, the same L1/L2/MSHR geometry.
* **Per cell**: one `dx100sim` run with `--all`; each log states its own `m24_map` and
  `m24_dram_org` on the address-map identity line.
* **SIMT reference**: `dispatch = 0, bulk_org = 0` on the same frozen resources and all four maps,
  because the historical M23-G6C `m0` cell used the older translation settings and is not comparable.
* **Determinism**: every cell is a pure function of its configuration (verified by a byte-identical
  repeat, §9); four cells run concurrently, which cannot change a count.

## 5. Implementation

| file | change |
|---|---|
| `sim/src/sim.hpp` | `Config::m24_dram_org` (default **1** = shipped), `m24_map` (0..3, default 0), `m24_perm_seed`; `BulkStats::m24_rel_pairs`/`m24_rel_inversions`; `pa_page_of_vpn` declaration; one engine scratch word |
| `sim/src/bulk.cpp` | the early return in `win_order_fused`; the release-order counters; `m24_prev_pos_` reset per window; two exports; three constructor guards (`m24_dram_org = 0` only for `bulk_org 7/8`; `m24_map ≤ 3`; `m24_map` not combined with `dram_map`) |
| `sim/src/memory.cpp` | M1/M2 bank selection inside `dram_bank_row`; the Feistel permutation and `pa_page_of_vpn`; six call sites routed through it (`pa_line_of_va`, two `tlb_fill` sites, two L2-TLB fill sites, the ideal-translation path) |
| `sim/src/util.cpp` | parse the three keys; echo `m24_map m24_dram_org m24_perm_seed` on the address-map identity line |
| `sim/tests/tests.cpp` | `test_m24_four_way_organization` — 55 checks (mapping equations, M3 bijection/offset/determinism/seed, the four cells' order and conservation properties, determinism, defaults = cell D, M3 under the engine) |

Nothing else was touched: no policy, no threshold, no sort, no queue change, no cache change. The
suite is **4 861 checks, 0 failures** (4 806 before).

## 6. Results at the historical mapping (M0)

### 6.1 The four cells

All cycles are whole runs of the frozen trace; `walker` is `walker_busy / (cycles × walkers)`, i.e.
the fraction of page-walker capacity the run actually used; `inv` is the share of releases whose
position in the chosen order went backwards (how much the release order differs from the stored
order); `e/cl` is runs per physical `(bank,row)` cell present in the pool.

| workload | cell | cycles | vs A | reads | row hit | activations | r/a | pool | pages | cells | e/cl | inv | walker |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut | A | 21 991 083 | — | 651 198 | 60.47 % | 257 449 | 2.53 | 41.8 | 0.8 | 2.9 | 14.44 | 0.00 % | 18.3 % |
| Orkut | B | 6 130 484 | −72.12 % | 644 155 | 60.52 % | 254 332 | 2.53 | 2 047.1 | 256.2 | 690.2 | 2.97 | 0.06 % | 65.7 % |
| Orkut | C | 21 992 384 | +0.01 % | 651 207 | 60.47 % | 257 410 | 2.53 | 41.5 | 0.8 | 2.9 | 14.46 | 12.94 % | 18.3 % |
| Orkut | **D** | **6 130 494** | **−72.12 %** | 644 142 | 60.53 % | 254 249 | 2.53 | 2 045.3 | 255.9 | 689.3 | 2.97 | 0.43 % | 65.7 % |
| LiveJournal | A | 31 905 038 | — | 697 207 | 46.12 % | 375 661 | 1.86 | 15.1 | 0.3 | 1.1 | 14.10 | 0.00 % | 43.9 % |
| LiveJournal | B | 14 146 092 | −55.66 % | 697 088 | 46.14 % | 375 452 | 1.86 | 59.0 | 3.7 | 11.3 | 5.21 | 7.56 % | 99.1 % |
| LiveJournal | C | 31 904 610 | −0.00 % | 697 270 | 46.12 % | 375 670 | 1.86 | 15.0 | 0.3 | 1.1 | 13.83 | 9.29 % | 43.9 % |
| LiveJournal | **D** | **14 146 860** | **−55.66 %** | 697 141 | 46.14 % | 375 452 | 1.86 | 59.0 | 3.7 | 11.3 | 5.24 | 11.47 % | 99.1 % |
| ogbn train | A | 374 594 109 | — | 9 927 536 | 61.50 % | 3 821 951 | 2.60 | 0.4 | 0.1 | 0.1 | 3.12 | 0.00 % | 26.3 % |
| ogbn train | B | 98 542 852 | −73.69 % | 9 825 696 | 65.02 % | 3 437 481 | 2.86 | 24.2 | 3.8 | 7.9 | 3.07 | 5.09 % | 99.8 % |
| ogbn train | C | 374 613 218 | +0.01 % | 9 927 568 | 61.50 % | 3 822 058 | 2.60 | 0.4 | 0.1 | 0.1 | 3.13 | 21.10 % | 26.3 % |
| ogbn train | **D** | **98 563 897** | **−73.69 %** | 9 826 463 | 65.04 % | 3 435 646 | 2.86 | 23.8 | 3.8 | 7.6 | 3.13 | 11.82 % | 99.8 % |

**Page organization (A→B) is worth 55.7 %–73.7 % of the runtime**, and it is not a DRAM effect:

| Orkut M0 | A | B | D |
|---|---:|---:|---:|
| page walks | 107 439 | 107 398 | 107 405 |
| L2-TLB hit rate | 0.13 % | 0.15 % | 0.15 % |
| mean dTLB latency | 65 | 65 | 65 |
| dTLB MSHR occupancy (of 8) | **1.48** | **5.31** | **5.31** |
| MSHR stall events | 22 025 914 | 31 273 599 | 31 289 825 |
| cycles with admission stalled on a refusal | **21 678 127** (98.6 %) | 0 | 0 |
| translation throughput (runs/cycle) | 0.037 | 0.134 | 0.134 |
| DRAM bank busy (bank 0 + 1, of 2 × cycles) | 27.8 % | 98.4 % | 98.4 % |
| mean DRAM queue depth | 4.5 | 60.3 | 60.3 |
| row hit | 60.47 % | 60.52 % | 60.53 % |

The walk count, the walk latency and the L2-TLB behaviour are identical in A and B: the page factor
does not save translation work, it removes the **serialisation** that kept 6.5 of the 8 dTLB MSHRs
idle. `bulk_g6b_tr_stall_cycles` states it exactly — in A the admission spends 21.7 M of its 22.0 M
cycles blocked on a refusal from the single page in flight. Making the same walk work overlap is what
turns 18.3 % walker utilisation into 65.7 % and 27.8 % bank utilisation into 98.4 %.

On LiveJournal and ogbn the same mechanism applies from a different starting point: walker
utilisation 43.9 % → 99.1 % and 26.3 % → 99.8 %, and the page factor is 55.66 % and 73.69 %. Where
the in-order arm already saturated its walkers (LJ at 44 %), the gain is smaller.

### 6.2 The DRAM factor is zero at M0 — and the records say why

| workload | B (page only) | D (page + DRAM) | D − B | rows touched | activations B→D |
|---|---:|---:|---:|---|---|
| Orkut | 6 130 484 | 6 130 494 | **+10 (+0.0002 %)** | 8 185 | 254 332 → 254 249 |
| LiveJournal | 14 146 092 | 14 146 860 | **+768 (+0.0054 %)** | — | 375 452 → 375 452 |
| ogbn train | 98 542 852 | 98 563 897 | **+21 045 (+0.0214 %)** | — | 3 437 481 → 3 435 646 |

Two measured reasons, both structural:

1. **Under M0 a physical row belongs to one page.** A 4 KiB page is 64 lines and a row index spans 32
   lines, so page `p` owns rows `2p, 2p+1` and **no cell is fed by two pages**: measured
   `xpage = 0.00` on Orkut at both cells, and the counter's peak (`bulk_g6i_xpage_cells_max`) is 0.
   There is no cross-page group for the organizer to burst, so reordering cannot create one.
2. **The two orders nearly coincide.** Releasing in stored order already walks ascending line, and
   ascending line is ascending row under M0: only **0.43 %** of the 823 568 release pairs go backwards
   (they are translation stragglers). Sorting by physical rank reproduces an order the sealed array
   already had, and pays 205 938 extra organizer cycles (§10) for the privilege.

Cell C isolates the DRAM factor completely: with the page mechanism off it reorders 12.94 % of its
release pairs, builds no group worth bursting, and lands within **+0.01 %** of A on Orkut and
−0.00 %/+0.01 % on LiveJournal/ogbn. The DRAM mechanism *acts*; it just has nothing to act on.

### 6.3 Speedup against the SIMT baseline

The SIMT reference is `dispatch = 0` on the same frozen resources (walkers 8 on Orkut, 4 elsewhere),
all four mappings. Maps move it by ≤0.01 % (Orkut 17 904 629 / 17 904 726 / 17 904 636 / 17 905 489;
LiveJournal 34 506 181 / 34 506 160 / 34 505 461 / 34 506 401), so one number per workload suffices:

| workload | SIMT | A | B | D | D vs SIMT |
|---|---:|---:|---:|---:|---:|
| Orkut | 17 904 629 | 21 991 083 (0.81×) | 6 130 484 | 6 130 494 | **2.92×** |
| LiveJournal | 34 506 181 | 31 905 038 (1.08×) | 14 146 092 | 14 146 860 | **2.44×** |
| ogbn train | not measured | 374 594 109 | 98 542 852 | 98 563 897 | — |

The ogbn SIMT cell did not complete inside the run budget (it is the largest trace and was still
running after 55 minutes at four-way contention), so no ogbn SIMT speedup is claimed. The historical
M23-G6C `m0` number for ogbn (207 287 728) was measured at `walkers = 2` and is **not** comparable
with these cells.

Note what the A column says: with page organisation off, the bulk engine is **slower than plain
SIMT** on Orkut (0.81×) — one cursor is a worse translation stream than four warps' worth of
independent misses — and only slightly faster on LiveJournal. The page mechanism is therefore not
recovering a loss the engine introduced; it is what makes the engine win at all.

## 7. Mapping sensitivity

Cycles for every cell and mapping. M1/M2 change only the bank index; M3 permutes physical page
frames. Rows that cannot exist are marked — (ogbn M2, §11).

| workload | map | A | B | C | D | D vs B |
|---|---|---:|---:|---:|---:|---:|
| Orkut | M0 | 21 991 083 | 6 130 484 | 21 992 384 | 6 130 494 | +0.0002 % |
| Orkut | M1 | 21 981 526 | 6 125 884 | 21 981 127 | 6 125 221 | −0.0108 % |
| Orkut | M2 | 21 975 882 | 6 126 463 | 21 976 708 | 6 126 651 | +0.0031 % |
| Orkut | M3 | 21 978 569 | 6 128 013 | 21 978 490 | **6 035 778** | **−1.5051 %** |
| LiveJournal | M0 | 31 905 038 | 14 146 092 | 31 904 610 | 14 146 860 | +0.005 % |
| LiveJournal | M1 | 31 893 386 | 14 145 517 | 31 892 312 | 14 146 688 | +0.0083 % |
| LiveJournal | M2 | 31 911 121 | 14 146 034 | 31 910 635 | 14 145 850 | −0.0013 % |
| LiveJournal | M3 | 31 914 707 | 14 150 023 | 31 912 370 | 14 160 774 | +0.0760 % |
| ogbn train | M0 | 374 594 109 | 98 542 852 | 374 613 218 | 98 563 897 | +0.021 % |
| ogbn train | M1 | 374 672 993 | 98 544 886 | 374 677 389 | 98 555 270 | +0.0105 % |
| ogbn train | M3 | 374 552 320 | 98 543 870 | 374 549 439 | 98 563 346 | +0.0198 % |

**M1 and M2 are inert, and predictably so.** They move the bank index and nothing else; at
`banks = 2`, `row_bytes = 1024` every row still owns 32 lines (16 per bank), so neither map can
change the number of lines a row owns, the number of cells a page spans, or the page↔row
relationship. Measured: the page factor is 72.12 % on Orkut under all four maps (72.13 %, 72.12 %,
72.12 %), LiveJournal 55.66 / 55.65 / 55.67 / 55.66 %, ogbn 73.69 / 73.70 / 73.69 %. The largest cell
effect of a bank-only remap is −0.07 % (Orkut A, M2) and the row-hit rates move by ≤0.09 pt. The one
trend worth recording is that *both* bank maps are marginally better than the historical one on
Orkut (−0.04 % and −0.07 %), i.e. the XOR fold's page-constant term costs a few hundred cycles there —
too small to be a finding, and not a reason to change the production map.

**M3 is where the DRAM factor wakes up, on one workload out of three.**

| Orkut, cells B → D | M0 | M3 |
|---|---:|---:|
| cycles | 6 130 484 → 6 130 494 | 6 128 013 → **6 035 778** |
| row hit | 60.52 % → 60.53 % | 60.82 % → **62.95 %** |
| activations | 254 332 → 254 249 | 253 164 → **239 042** (−5.6 %) |
| runs per cell in the pool | 2.97 → 2.97 | 3.15 → **3.95** |
| cells in the pool (mean) | 690.2 → 689.3 | 653.2 → **541.1** |
| rows touched by the run | 8 185 | 8 185 → **6 529** |
| **multi-page cells in the pool (mean / max)** | 0.00 / 0 | 36.91→**70.83** / 229→**313** |
| release pairs that go backwards | 0.06 % → 0.43 % | 0.06 % → 5.83 % |

A page permutation is the only change in this matrix that makes two virtual pages share a physical
row (frames 2048 apart), and the counters show the consequence directly: multi-page cells in the pool
go from **0 to 71 per sample** and the organizer collapses the simultaneously-visible cell population
from 653 to 541 while every cell it keeps carries 3.95 runs instead of 3.15. That is the first and
only configuration in which sorting by physical rank can build a burst the arrival order could not.
It buys 5.6 % fewer activations, +2.13 pt row hit and **1.51 % of the runtime**.

On the other two workloads the same mechanism is present but too thin to matter: LiveJournal's pool
holds 2.7 pages per sample at M3, so multi-page cells are 0.45 → 0.81 per sample, and ogbn's 0.15 →
0.29. With fewer than one cross-page cell in view on average there is nothing to burst, and the
organizer's extra work becomes a small net cost (+0.076 % and +0.020 %).

This is the quantitative statement of the paper's own mechanism: **the organizer needs shared rows and
a deep pool at the same time**, and the benefit tracks their product. Orkut at M3 has both
(70.8 shared cells in view); LiveJournal and ogbn at M3 have neither enough pages in the pool nor
enough sharing.

## 8. Interaction analysis

`Interaction = T_D − T_B − T_C + T_A`, in cycles. Negative means the two mechanisms together save more
than the sum of their separate savings (complementary); positive means they interfere. The band for
"no interaction" is 0.05 % of the runtime.

| workload | map | page only | DRAM only | combined | incremental DRAM after page | incremental page after DRAM | interaction | class |
|---|---|---:|---:|---:|---:|---:|---:|---|
| Orkut | M0 | 72.12 % | −0.01 % | 72.12 % | −0.00 % | 72.12 % | −1 291 | additive |
| Orkut | M1 | 72.13 % | 0.00 % | 72.13 % | 0.01 % | 72.13 % | −264 | additive |
| Orkut | M2 | 72.12 % | −0.00 % | 72.12 % | −0.00 % | 72.12 % | −638 | additive |
| Orkut | M3 | 72.12 % | 0.00 % | **72.54 %** | **1.51 %** | 72.54 % | **−92 156** | complementary |
| LiveJournal | M0 | 55.66 % | 0.00 % | 55.66 % | −0.01 % | 55.66 % | +1 196 | additive |
| LiveJournal | M1 | 55.65 % | 0.00 % | 55.64 % | −0.01 % | 55.64 % | +2 245 | additive |
| LiveJournal | M2 | 55.67 % | 0.00 % | 55.67 % | 0.00 % | 55.67 % | +302 | additive |
| LiveJournal | M3 | 55.66 % | 0.01 % | 55.63 % | −0.08 % | 55.63 % | +13 088 | additive |
| ogbn train | M0 | 73.69 % | −0.01 % | 73.69 % | −0.02 % | 73.69 % | +1 936 | additive |
| ogbn train | M1 | 73.70 % | −0.00 % | 73.70 % | −0.01 % | 73.70 % | +5 988 | additive |
| ogbn train | M3 | 73.69 % | 0.00 % | 73.69 % | −0.02 % | 73.68 % | +22 357 | additive |

Reading:

* **The mechanisms are neither additive nor redundant in the usual sense — one of them is empty.**
  In 10 of 11 cells the DRAM factor alone is 0.00 % ± 0.01 %, because C cannot build a population
  (§6.2); so "page only" and "combined" are the same number to four digits and the interaction is
  whatever the DRAM factor's own sign was. Calling that "redundant" would credit the DRAM mechanism
  with a role it never plays; the honest label is **additive over an empty second factor**.
* **Where the DRAM factor is not empty, it is complementary, and completely so.** Orkut/M3: page-only
  saves 15.85 M cycles, DRAM-only saves 79, and together they save 15.94 M — 92 156 cycles more than
  the sum. The interaction is −92 156 cycles, i.e. −1.5 % of the runtime, and it is the *entire* DRAM
  benefit: without the page factor the DRAM factor has no pool to order, and without the map the
  organizer has no shared row to aim at.
* **The interaction is mapping-dependent, and only through M3.** The same −1.5 % cannot be produced at
  M0/M1/M2 on any workload, because none of them creates a page-shared row.
* **Which resource changed.** Page factor: dTLB MSHR concurrency (1.48 → 5.31 of 8) and bank
  utilisation (27.8 % → 98.4 %), with the walk count and latency fixed. DRAM factor: activations
  (−5.6 %) and rows touched (8 185 → 6 529) on Orkut/M3 only; on every other cell nothing that matters
  moves. No cell changes the number of DRAM reads by more than 1.2 %.

## 9. Correctness and reproducibility

| check | result |
|---|---|
| `make -j4` | clean, no warnings |
| full suite | **4 861 checks, 0 failures** (4 806 before; +55 new) |
| new test `m24_four_way_organization` | mapping equations for M0/M1/M2 (8 192 lines), M3 bijection over 4 096 pages (no duplicate frame, all in range), 64-line offset preservation, seed sensitivity, M0 identity for M1/M2/M3's PA path, the four cells' order and conservation properties, determinism, `defaults == cell D`, M3 under the engine |
| request conservation | `bulk_g6b_tr_requests == bulk_g6b_tr_completed == bulk_g6b_runs_released` and `conservation_ok == 1` in **all 44 cells** |
| no request lost | `bulk_elems_done == bulk_elems_total` in all 44 cells |
| no over-capacity pool | `bulk_g6b_ready_peak ≤ 4 096` in all 44 cells |
| DRAM factor really off | `bulk_org_runs == 0` in all 22 neutral cells, `> 0` in all 22 physical cells |
| page factor really off | `bulk_g6c_admit_accepted == 0` in all 22 in-order cells, `> 0` in all 22 cohort cells |
| historical anchor | cell D/M0 equals `build/wm/m23g6h/g6h_G0r_*.txt` on **761 common keys, 0 differ**, all three workloads; equals `build/wm/m23g6i/g6i_A1_*.txt` on **785 common keys, 0 differ** |
| deterministic repeat | D/M0/Orkut rerun: **byte-identical log**; the test reruns cell D and compares cycles and counters |
| mapping validation | M3's permutation is a bijection by construction (Feistel) and verified on 4 096 pages; every map is deterministic; M0/M1/M2 leave the PA untouched, so address uniqueness and capacity are those of the base model |

44 of the 48 planned cells exist: the four ogbn M2 cells were cancelled mid-run (see §11) once
M1 and M2 had shown identical, ≤0.08 % behaviour on both other workloads. The extractor states the
omission on every invocation rather than filling the row.

## 10. Resources, overhead, and where the time went

* **Ready pool**: 4 096 × 16 B = **65 536 B** (`bulk_g6b_pa_bytes`), identical in all four cells — it
  is the vehicle of the pipeline, not either factor.
* **The DRAM factor's own storage**: `ford` — 4 B per run, 64 KiB at a 16 384-run window — plus two
  4-B-per-run `(bank,row)` arrays built during the seal, 128 KiB transient. The neutral arms allocate
  none of them (`org_runs = 0`).
* **The DRAM factor's own work**: one organizer pass per run, charged to the seal as
  `bulk_org_cycles` = 805 876 versus the seal-only 599 938 — **+205 938 cycles (3.4 % of the run,
  overlapped)**. It is charged but not additive: D and B differ by 10 cycles because the sort runs on
  the seal's slack.
* **Implementation complexity**: one config flag with a three-line guard, one early return, one
  counter pair, three mapping arms inside an existing decode function, one page-permutation function
  and six call sites. No new policy, no threshold, no tuning.
* **Where the page factor's 3.6× came from**: not fewer requests (651 198 → 644 155 reads, −1.1 %), not
  fewer walks (107 439 → 107 398), not a better map — it is the difference between using 1.48 and 5.31
  of 8 dTLB MSHRs and between 27.8 % and 98.4 % bank utilisation. 21.7 M of A's 22.0 M cycles are
  spent with admission stalled on a single page's refusal; B spends none there and instead waits on
  DRAM service (mean queue 60.3, mean wait 574 cycles) — the bottleneck the engine was built to
  attack.

## 11. Limitations

1. **ogbn has no SIMT row.** The `dispatch = 0` cell was still running after 55 minutes of four-way
   contention and was stopped; no ogbn speedup is quoted. The historical `m0` (walkers = 2) is not
   comparable and is not used.
2. **ogbn M2 was cancelled** once M1/M2 had shown ≤0.08 % effects on the other two workloads and no
   change to the page factor under any map. Every other mapping/workload pair is complete.
3. **M1 and M2 cannot test grouping.** With `banks = 2` and `row_bytes = 1024` a row always owns 32
   lines, so a bank map cannot change cell size, which is why M1/M2 are inert by construction rather
   than by measurement. A map that changes row *span* (e.g. `row_bytes = 512`) would test something
   else and would also change capacity, so it is out of scope for this milestone.
4. **M3 permutes every page**, including the descriptor page and the 8 MiB index array, because the
   model has one mapping function. A real system could place those contiguously; the cost is visible
   but small (Orkut M3 changes reads by +0.7 % and the page factor is unchanged).
5. **One seed for M3.** `m24_perm_seed = 1` in every cell; the permutation is deterministic and the
   test proves seeds select different bijections, but no seed sweep was run, so the 1.5 % on
   Orkut/M3 is one draw from that family, not an average over it.
6. **`dram_q = 0` (unbounded queue)** is the historical control and gives the controller maximal
   freedom to reorder. It is the configuration least likely to reward engine-side ordering; a finite
   queue is the most obvious follow-up.
7. **Cell A is not SIMT.** Page organisation off inside the engine serialises on one cursor and is
   0.81× SIMT on Orkut. The factorial's baseline is an engine-internal control by design; the SIMT row
   is reported separately for exactly this reason.

## 12. Verdict

**Do the two mechanisms provide independent benefits?** No — and the asymmetry is total.

* **Page organization is large, robust and independent of everything else in the matrix**: 55.7 %–
  73.7 % of the runtime, unchanged to within 0.02 pt by any of the four mappings on all three
  workloads, and unchanged when the DRAM factor is on. Its mechanism is measured, not inferred:
  walker utilisation 18.3 → 65.7 %, 43.9 → 99.1 %, 26.3 → 99.8 %; dTLB MSHR occupancy 1.48 → 5.31 of
  8; bank utilisation 27.8 → 98.4 %; 21.7 M of A's 22.0 M cycles spent blocked on one page.
* **Explicit DRAM organization is worth 0.0002 % on Orkut, 0.005 % on LiveJournal and 0.021 % on
  ogbn** (negative in all three: it costs a few cycles) at the historical mapping, and remains 0
  under both bank maps. It has no independent benefit in this architecture, because under M0 a
  physical row belongs to exactly one page (`xpage = 0`) and the sealed order is already the row
  order (0.43 % inverted pairs).
* **Combining them adds nothing at M0/M1/M2.** Combined equals page-only to four significant digits;
  the interaction is ±0.01 % of runtime.
* **Combining them is genuinely complementary on exactly one cell**: Orkut under the page
  permutation, where shared rows first exist (multi-page cells 0 → 71 per sample) and the organizer
  can collapse 653 visible cells into 541 carrying 3.95 runs each. There it saves 1.51 % on top of
  page-only, −92 156 cycles of interaction, all of it conditional on the page factor having built a
  pool deep enough (2 054.8 entries) to hold both pages of a shared row at once.
* **So the brief's hypothesis is confirmed in direction and wrong in magnitude.** DRAM organisation
  does become more valuable under a less-aligned map — by two orders of magnitude, 0.0002 % → 1.51 % —
  but it stays **48× smaller than the page factor** even in the one configuration built to favour it,
  and it is negative on the other two workloads under the same map. The binding constraint is not
  ordering but population: with `dram_q = 0` the controller already reorders everything it is given,
  and the only thing the engine-side order can add is the cross-page burst that a page-private map
  makes impossible.

The design rule this supports: **organise translation for concurrency first; organise the release
order only if the physical map can put more than one page in a row.** In this map, and on these three
workloads, the second condition holds only when pages are deliberately permuted.

## Reproduce

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests        # 4861 checks, 0 failures
cd .. && ./build/wm/m24a_run.sh                          # 48 cells (4-way x 4 maps x 3 workloads)
cd .. && ./build/wm/m24a_simt.sh                         # SIMT reference, 4 maps x 3 workloads
cd .. && python3 tools/m24a_extract.py                   # tables, factorial, identities, anchors
```

Per-cell command (Orkut, cell D, mapping M3):

```bash
cd DX100_MEM/sim && ./bin/dx100sim --config configs/m23g6b_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 \
  --set bulk_org=8 --set m24_dram_org=1 --set m24_map=3 \
  --set g6b_pa_capacity=4096 --set walkers=8 --set page_cohorts=8 \
  --set dtlb_mshr=8 --set walk_latency_per_level=150 \
  --set g6i_diag=1 --set sched_vis=0 --set sched_group=0 --all
```
