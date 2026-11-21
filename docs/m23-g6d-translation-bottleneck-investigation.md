# M23-G6D — What actually limits translation throughput in G6C

**Investigation only. No source file, configuration file or trace was modified, and no simulation
was run for this report.** Every number below is either read out of the shipped source, or taken
from the 15 completed M23-G6C logs in `build/wm/m23g6c/` and the published M23-G4A cells quoted in
`docs/m23-g4b-page-first-row-second.md` §6.

## 0. Answer in one paragraph

G6C is limited by **the two page walkers' own serial time**: `walker_busy = walks × 300` **exactly**
(no walk-cache hits, no partial walks) on all three workloads, and the walkers are busy **97.6 % /
99.2 % / 98.6 %** of the run. Measured cycles sit **2.45 % / 0.80 % / 1.47 %** above
`walks × 300 / 2`, and that residual is 3.7 / 1.2 / 2.2 wall cycles per page — the post-fill admission
burst, not a scheduling failure. The walk count itself is not an artefact of bad handling: it is
**one walk per distinct page per window** (`l2tlb_dedups = 0`, 7.7 / 3.9 / 6.4 probes coalesced onto
each walk, `l2tlb_hits` ≈ 0.1 %) because the 512-entry L2 TLB is 2.8–5.0× smaller than each window's
page working set, so no translation survives to the next window. The binding *ceiling* is not the
walker count alone: only **2 page runs can be admitted at once** (`page_cohorts = 2`) and only **4
distinct pages can hold an outstanding translation** (`dtlb_mshr = 4`), which is why the published
M23-G4A Orkut cell with 4 walkers bought **−21.4 %**, not −50 %. The L2 TLB MSHR array (8) is
**provably non-binding** — `l2tlb_max_occ = 4` in all 15 cells and `l2tlb_walk_stalls = 0`
everywhere, because a dTLB MSHR caps the distinct pages in flight at 4 in the first place. The dTLB
park limit (`dtlb_parked = 2`) produces 66.8–99.9 % of all refusals but costs **no walker time**:
those refusals are retries on an admission port that is only half used (2 attempts/cycle of a 4-slot
budget). The next wall after translation is **DRAM bank service**: the busier bank is occupied
6.13 M / 7.15 M / 91.68 M cycles on the three workloads, so the earliest cycle count any
translation fix can reach is 6.1 M / 7.1 M / 91.7 M before DRAM binds.

Source and binary are unchanged from the M23-G6C matrix:
`sim/src/bulk.cpp` `ba13e595…`, `sim/src/sim.hpp` `f0bcf8e4…`, `sim/src/memory.cpp` `adf43b86…`,
`sim/configs/m23g6b_base.cfg` `abc91fab…`, `sim/bin/dx100sim` `e41bcd8a…`.

---

## 1. The translation pipeline as implemented

Order inside a cycle is fixed (`memory.cpp:749-765`):
`l2tlb_tick → ptw_tick → dram_tick → cache_tick → pending_fill → inflight_ sweep`.

| # | Stage | Where | Capacity / rate | Admission & refusal | Wait / retry | Sharing | Serialized? | Counters |
|---|---|---|---|---|---|---|---|---|
| 1 | Window seal → run array + cohort table | `bulk.cpp:1235-1240`, `win_seal` | `page_cohorts = 2` cohorts × 32 B; `fmeta` one entry per distinct line | cohorts claimed in stored order; `page_sched = 1` required | — | one run per distinct **line**; a page's 64 runs are contiguous, split into ≤`page_cohorts` open spans | — | `bulk_sched_cohorts`, `bulk_sched_bytes`, `bulk_org_runs` |
| 2 | Translation admission | `win_admit_g6c`, `bulk.cpp:2047-2143` | `eng_issue = 4` probes/cycle; 1 port cycle per installed page run (`g6c_busy_`) | oldest eligible cohort first; refusal → `resume_at = cycle + park_retry (1)`, **other cohort tried in the same cycle** | refused run re-probed every cycle until its page's dTLB entry lands | a run is a `Req` with `xlate_only = 1`, `slot = none` (holds no residency slot) | no — up to 2 pages progress per cycle | `bulk_g6c_admit_attempts/accepted/refused/refuse_parked/refuse_full`, `…_admit_idle_cycles`, `…_cohorts_opened`, `…_defer_age_*`, `…_defer_depth_*` |
| 3 | dTLB | `tlb_probe`, `memory.cpp:126-184` | 16 entries × 4-way = 4 sets (vpn % 4); **4 MSHRs**, **2 parkable per page** | hit → ready at `cycle+1`; same-page probe with `parked < 2` → joins; `parked == 2` → **refuse_parked**; no free MSHR → **refuse_full** | a refused probe is backpressure: `reads`/`misses` are decremented, caller retries | yes: MSHR join for the same vpn, and 64 lines of a page after the fill | fill is per page, once | `tlb0_reads/hits/misses/evict`, `tlb0_mshr_occ/_max`, `tlb_refuse_parked/full`, `tlb0_mshr_stalls` |
| 4 | L2 TLB | `l2tlb_tick`, `memory.cpp:216-266` | 512 entries × 4-way = 128 sets; **8 MSHRs**; `l2tlb_req_per_entry = 8`; pipe latency 4 | hit → fill; miss → join or allocate; no MSHR free, or a join over 8 → requeue after 4 cycles (**walk_stall**) | requeued entry loses its read/miss accounting | yes: one MSHR per vpn, up to 8 joining dTLB probes | allocation is per page | `l2tlb_reads/hits/misses/allocs/dedups/evict/occ/max_occ/walk_stalls` |
| 5 | Page walkers | `ptw_tick`, `memory.cpp:267-320` | **2 walkers**; service = `levels × walk_latency_per_level = 2 × 150 = 300` cycles, charged from the issue cycle | a walk is issued only for an unreached MSHR; if all walkers are busy the issue loop **breaks** (remaining MSHRs wait in slot order) | queued MSHRs are re-examined every cycle | **one walk per page**; the 16-entry walk cache is a FIFO consulted only at issue | concurrent across walkers; completion is a fixed delay, not an event on PTE data | `walks`, `walker_busy`, `walker_max`, `walk_latency`, `walk_wc_hits`, `pte_fetches` |
| 6 | Completion → fill | `ptw_tick` retire loop, `pending_fill_` in `tick` | unbounded per cycle | — | — | one L2 TLB install + one dTLB install per completed page, same tick | — | (implicit in `tlb0_hits` growth) |
| 7 | G6C ready buffer → DRAM release | `win_issue_g6b`, `win_emit_fused` | `g6b_pa_capacity = 4096` entries, 16 B each | full buffer backpressures admission (`g6b_backpressure_cycles`) | released in P2 order | — | independent of translation (separate port) | `bulk_g6b_ready_*`, `…_tr_wait_dram`, `…_no_ready_cycles`, `…_overlap_cycles` |

Two properties of stage 5 matter more than any other line in the model:

```cpp
// memory.cpp:281-289 — the walk is a fixed delay; the PTE probe's result is discarded
u64 lat = static_cast<u64>(cfg_.levels) * cfg_.walk_latency_per_level;
for (u32 lvl = 0; lvl < cfg_.levels; ++lvl) {
  u64 ready = NONE;
  l2_probe(pte_line_of(lvl, m.vpn), cycle, ready);   // real traffic, `ready` unused
  ptw.pte_fetches++;
}
ptw.walks++; ptw.walk_latency += lat;
m.ready = cycle + lat;
walk_busy_[w] = m.ready;
```

* **The walk cannot be delayed by memory.** PTE fetches are charged as real L2/DRAM reads, but a walk
  always completes `300` cycles after issue. The model can only ever *understate* walk latency under
  contention, never overstate it.
* **A walk is charged as one 300-cycle unit, not two pipelined levels.** A real walker complex can
  overlap level 1 of walk B with level 2 of walk A; here it cannot, so per-walker throughput at a
  given latency is the least favourable interpretation of "2 levels × 150".

Bandwidth of the translation path, measured (`p8` cells):

| Workload | probes admitted | page walks | probes / walk | L2 TLB hits | L2 TLB evictions | walk-cache hits | `l2tlb_walk_stalls` |
|---|---:|---:|---:|---:|---:|---:|---:|
| Orkut | 823 786 | 107 400 | 7.67 | 148 | 106 888 | 0 | 0 |
| LiveJournal | 729 139 | 186 875 | 3.90 | 307 | 186 363 | 0 | 0 |
| ogbn-products | 8 380 253 | 1 312 297 | 6.39 | 81 | 1 311 785 | 0 | 0 |

So intra-page coalescing is **already perfect** (every walk serves ~4–8 lines through the dTLB) and
inter-window reuse is **zero** (the TLB is flushed by capacity, not by policy).

## 2. Resource and concurrency limits

| Resource | Value | Observed use (Orkut / LJ / ogbn, G6C) | Binding? |
|---|---|---|---|
| page walkers | 2 | `walker_max = 2`, busiest 97.6 / 99.2 / 98.6 % | **yes — this is the bottleneck** |
| walk service interval | 300 cycles | `walker_busy == walks × 300` exactly | defines the cost per walk |
| dTLB MSHRs | 4 | avg **2.80 / 3.24 / 2.13**, peak 4 | sets the **page-concurrency ceiling** |
| dTLB parked/page | 2 | 76.8 / 66.8 / 99.9 % of refusals are `refuse_parked` | no (retries only) |
| L2 TLB MSHRs | 8 | avg **2.78 / 3.23 / 2.11**, peak **4** in all 15 cells | **no — provably non-binding** |
| L2 TLB entries | 512 (128 sets × 4) | 0.1 % hit rate | sets the **walk count** |
| dTLB entries | 16 (4 sets × 4) | 78.6 / 60.5 / 71.0 % hit | no (intra-page reuse works) |
| open page cohorts | 2 | `defer_depth_mean` 1.50, max 2 | **caps walker usefulness at ~2.4** |
| ready buffer | 4096 entries | avg 93.3 / 18.0 / 3.8, peak 4096, backpressure 0.71 / 0.17 / 0.04 % | no |
| R / L2 MSHRs / DRAM | 128 / 64 / 2 banks | DRAM bank busy 6.08 / 7.15 / 91.66 M cycles | **next wall** |

The brief's ceiling expression is **incomplete**:

```
W_eff ≤ min(W_walkers, M_dTLB, M_L2TLB)
```

Correct for the *walker* population, but it omits four things the source enforces:

1. **`M_L2TLB` can never be reached.** An L2 TLB MSHR exists only for a page that already holds a
   dTLB MSHR, and `dtlb_mshr = 4` allows at most 4 distinct pages (`tlb_probe` joins on vpn rather
   than allocating again). Hence `l2tlb_max_occ ≤ 4 < 8` by construction, and `walk_stalls = 0`
   everywhere. The 8-entry array is **dead capacity** for this design point.
2. **Pages enter translation only through an admission cohort.** With `page_cohorts = 2`, at most two
   *new* pages can be offered to the translator per admission episode, so the page arrival rate is
   ~2 per (300 + tail) cycles regardless of how many walkers exist. This is the term that caps the
   walker gain, and it is missing from the min-expression.
3. **A page's MSHR is held for the whole walk**, not just while its cohort is being served, so pages
   in flight (`l2tlb.occ`) can exceed the open-cohort count — measured 2.78 / 3.23 / 2.11 against 2
   cohorts. That surplus is the only headroom a 3rd/4th walker can absorb.
4. **A walk-cache hit consumes a walker for 1 cycle, not 300** (`m.ready = cycle+1`,
   `walk_busy_[w] = m.ready`), and a dTLB hit is not a walker event at all. Both short-cuts are
   inactive here (`walk_wc_hits = 0`), so every walk costs the full 300.

Corrected form:

```
pages_in_flight ≤ min(page_cohorts + slack, dtlb_mshr)          # injection width , measured 2.11-3.24
W_eff           ≤ min(W_walkers, pages_in_flight, M_dTLB)        # M_L2TLB is never binding
cycles         ≈ max( walks × 300 / W_eff , DRAM_busy_per_bank )  # + per-page refill tail
```

## 3. The existing G6C measurements

### 3.1 Cycles, translation and the floor

| Workload | Arm | cycles | walks | `walks×300/2` | gap | walker util | idle walker-cycles / walk |
|---|---|---:|---:|---:|---:|---:|---:|
| Orkut | G6C | 16 505 468 | 107 400 | 16 110 000 | **+2.45 %** | 97.60 % | 7.4 |
| Orkut | P0 | 16 714 538 | 107 399 | 16 109 850 | +3.75 % | 96.38 % | 11.2 |
| LiveJournal | G6C | 28 256 445 | 186 875 | 28 031 250 | **+0.80 %** | 99.20 % | 2.4 |
| LiveJournal | P0 | 28 309 390 | 186 923 | 28 038 450 | +0.97 % | 99.04 % | 2.9 |
| ogbn | G6C | 199 738 512 | 1 312 297 | 196 844 550 | **+1.47 %** | 98.55 % | 4.4 |
| ogbn | P0 | 199 882 548 | 1 313 192 | 196 978 800 | +1.47 % | 98.55 % | 4.4 |

`idle walker-cycles / walk` = `(2 × cycles − walker_busy) / walks`. It is 2.4–7.4 walker-cycles per
page, i.e. **1.2–3.7 wall cycles per page** — the same size as the post-fill burst (7.7 runs at
4 probes/cycle = 2 cycles, plus 1 cycle to install the next page run). There is no hidden stall
between walks.

G6C's −1.25 % against P0 on Orkut is *not* more translation capacity: it is the separate DRAM
release port. P0 pays 31.8 M `sched_defer` iterations on the single port that also issues its
target requests;
G6C pays 32.1 M refusals on a port that only admits translations. The translation statistics
themselves are nearly identical (cohorts opened 106 289 vs 106 286; admit-idle 15.87 M vs 16.10 M).

### 3.2 MSHR and admission occupancy

| Workload | dTLB MSHR avg / peak | L2 TLB MSHR avg / peak | refusals (parked / full) | attempts | accepted | admit-idle cycles | refusal age mean / max |
|---|---|---|---:|---:|---:|---:|---:|
| Orkut | 2.80 / 4 | 2.78 / 4 | 25 014 863 / 7 561 055 (76.8 %) | 32 891 032 | 823 786 | 16 097 476 (97.5 %) | 316.4 / 9 900 |
| LiveJournal | 3.24 / 4 | 3.23 / 4 | 37 677 270 / 18 712 283 (66.8 %) | 56 483 162 | 729 139 | 98.8 % | 320.0 / 9 300 |
| ogbn | 2.13 / 4 | 2.11 / 4 | 399 049 803 / 458 571 (99.9 %) | 403 213 439 | 8 380 253 | 98.7 % | 323.9 / 48 712 |

Two readings that matter:

* **Refusals are free in walker time.** 32.9 M attempts on Orkut is 2.0 attempts/cycle against a
  4-slot budget, and only 823 786 of them are accepted — the port is idle in the sense that it has
  spare slots, and every refusal is a re-probe of a page whose walk is already in flight. Refusal
  *age* (~316, 320, 324 cycles) is the walk latency itself: a cohort is deferred exactly until its
  page's translation lands.
* **`bulk_g6c_admit_idle_cycles` does not mean "no work".** In the G6C loop a refusal sets
  `resume_at = cycle + 1`, which makes the refusing cohort ineligible for the rest of *that* cycle;
  if both cohorts refuse, the loop finds no eligible cohort, fails to open a new one (both slots
  open), increments `g6c_admit_idle`, and breaks (`bulk.cpp:2072-2080`). So 97.5 % "idle" is the
  steady state of a **saturated translator** — both pages waiting on the two walkers — not evidence of
  a scheduling defect. The M23-G6C report's phrase "admission idles 97.5 % of cycles" should be read
  with this correction.

### 3.3 Ready buffer and DRAM (second order, for completeness)

| Workload | buffer avg / peak | backpressure | no-ready | overlap/tgt-out | bank busy (0 / 1) | row hit | DRAM reads | of which walker PTEs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Orkut | 93.3 / 4096 | 0.71 % | 91.4 % | 97.47 % | 6 078 156 / 6 125 930 | 60.49 % | 651 193 | 2 757 (0.42 %) |
| LiveJournal | 18.0 / 4096 | 0.17 % | 98.0 % | 99.21 % | 7 149 732 / 7 116 752 | 46.14 % | 697 162 | 3 631 (0.52 %) |
| ogbn | 3.8 / 4096 | 0.04 % | 98.0 % | 99.96 % | 91 657 468 / 91 677 254 | 62.73 % | 9 924 795 | 1 022 334 (10.30 %) |

The buffer is never the limiter (except a single 0.71 % episode on Orkut) and the DRAM keeps up.
The per-bank busy totals are the important column: they are the lower bound any faster translation
runs into.

## 4. The five hypotheses, ranked by evidence

**H-A — Page-walker throughput. Confirmed as the primary limit.**
`walker_busy = walks × 300` exactly and utilization 97.6–99.2 %; cycles are within 0.8–2.5 % of
`walks × 300 / 2`; the residual decomposes into the per-page refill tail. There is nothing else left
in the cycle budget. **But** the *useful* walker count is capped by injection width (§2), which is
why the walker is "saturated" at 2 rather than merely "the first of several limits".

**H-C — Translation reuse / TLB capacity. Confirmed as the reason the walk count is what it is.**
Walks are `windows × distinct pages per window` = 74 × 1 451.4, 74 × 2 525.3, 514 × 2 553.1. The
per-window page working set exceeds the 512-entry L2 TLB by 2.8–5.0×, so `l2tlb_hits` is 148 / 307 /
81 against 107 400 / 186 875 / 1 312 297 walks, `l2tlb_evict ≈ walks`, and every window re-walks
pages it walked in the previous window. Handling is *not* the problem: `l2tlb_dedups = 0` and
`walk_wc_hits = 0`, i.e. no duplicate walks within a page's translation lifetime and no temporal
locality close enough for a 16-entry cache. So the workloads are limited by **an inherently large
distinct-page count relative to TLB capacity**, not by inefficient reuse — and the number of walks
scales with the *window count*, which is a property of the algorithm, not of the MMU.

**H-B — TLB MSHR / admission constraints. Only the dTLB MSHR matters, and only as a ceiling.**
* L2 TLB MSHRs: **non-binding, proven** (`max_occ = 4 < 8` in all 15 cells, `walk_stalls = 0` in all
  15 cells). No experiment is needed to reject this; adding L2 TLB MSHRs cannot change any cycle.
* dTLB MSHRs (4): they are the ceiling on distinct pages in flight, and `l2tlb_max_occ = 4` shows the
  ceiling is reached. But with only 2 cohorts injecting pages, 4 MSHRs are already ~1.4× the
  injection rate, so raising them alone does nothing — the published M23-G4A Orkut cell
  `dtlb_mshr 4→8, walkers 2` moved cycles by **−0.06 %** (16 714 538 → 16 704 951).
* `dtlb_parked = 2`: generates 66.8–99.9 % of refusals, but refusals cost no walker time and the
  admission port has 2 spare slots per cycle. Predicted cycle effect of raising it: **0**.

**H-E — Admission scheduling efficiency. Not supported; G6C is at the floor.**
1.2–3.7 cycles/page of walker idle, `defer_depth_mean = 1.50` (both cohorts parked, as expected
under saturation), and G6C ≤ P0 on all three workloads. Oldest-ready-first cannot hurt here because
every cohort is waiting on the same 2 walkers. The only scheduling-shaped lever left is the *number*
of open cohorts — a declared capacity, not an ordering policy.

**H-D — Model fidelity. Both artefact and real, and they must be separated.**
* Real: the topology (2 walkers, 512-entry L2 TLB, 16-entry dTLB, 4 MSHRs, two-level Sv32) is the
  calibrated Vortex/DX-100 configuration; the walk *count* is a genuine capacity consequence.
* Artefact 1: **`walk_latency_per_level = 150` is an [A] parameter and the walk is a constant 300-cycle
  delay that PTE traffic cannot extend.** Absolute cycles are therefore 100 % controlled by an
  assumed number; the *structure* (`cycles ≈ walks × 300/2`) is what is trustworthy, not `16.5 M`.
* Artefact 2: the two levels are charged serially inside one walker, so the model cannot exploit
  level overlap; with 300 cycles that is 2× pessimistic in per-walker throughput if a real PTW
  pipelines level 1 and level 2.
* Artefact 3: **no page-walk cache can help here.** The walk cache is a 16-entry FIFO consulted only
  at issue; the re-walk distance is ~1 451–2 553 pages, so even a 512-entry PWC would miss, and a
  PWC sized to the working set would be a second TLB. `walk_wc_hits = 0` in 14 of 15 cells (6 on
  M0/LiveJournal) settles the question empirically.
* Artefact 4 (direction of error): PTE fetches are charged as real traffic but never gate completion,
   so under DRAM contention the model **overstates** translation throughput. On ogbn the walker
  already issues 10.3 % of all DRAM reads (1.02 M) against a 42 % L2 hit rate; a latency-gated walk
  would be slower, not faster.

## 5. Architectural constraint vs simulator artefact

| Claim | Status | Evidence |
|---|---|---|
| "2 walkers × 300 cycles is the binding resource" | **real constraint, correctly modelled** | `walker_busy = walks×300`; 97.6–99.2 % busy; `docs/dx100-experiment-audit.md` C15 |
| "the 8-entry L2 TLB MSHR array limits translation" | **real capacity, non-binding here** | `l2tlb_max_occ = 4`, `walk_stalls = 0`, 15/15 cells |
| "the dTLB MSHR array (4) limits translation" | **real, and it is the ceiling on pages in flight — but slack under 2 cohorts** | `max_occ = 4`; G4A `m8w2` = −0.06 % |
| "the dTLB park limit (2) limits translation" | **not on the critical path** | refusal cost = retries on a half-used port; 0 walker-cycles |
| "the walk cache reduces walk demand" | **artefact: it cannot, at this working-set size** | 0 hits in 14/15 cells; 16-entry FIFO; rewalk distance ≫ 16 |
| "translation is DRAM-latency dependent" | **artefact: it is not modelled at all** | `ptw_tick` discards `l2_probe`'s ready; `m.ready = cycle + lat` |
| "the walk latency is 300 cycles" | **assumption ([A])**, 2.5–5× above a dependent 2-bank fetch chain | `levels × walk_latency_per_level`; `t_hit 14`, `t_miss 26` |
| "the walk count is a workload property" | **real, but capacity-mediated** | walks = windows × pages/window; 74/74/514 windows; 1 451–2 553 pages/window vs 512 entries |
| "more walkers would halve the time" | **false as stated** | capped by `page_cohorts = 2`; G4A measured −21.4 % for 2→4 |

## 6. Recommended minimal experiment sequence

Frozen in every cell: `configs/m23g6b_base.cfg` (`abc91fab…`), `bulk_org = 8`, `batch_window = 16384`,
`g6b_pa_capacity = 4096`, P2 release order, `park_retry = 1`, R = 128, L1/L2/DRAM geometry and
mapping, the three trace files (`11cc8da4…`, `7363867a…`, `c20ce42a…`), the binary (`e41bcd8a…`).
Only the flagged field moves, on the command line. Cell template (Orkut shown):

```
cd DX100_MEM/sim && ./bin/dx100sim --config configs/m23g6b_base.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 --set bulk_org=8 \
  <FLAGS> --all
```

`walkers`, `walk_latency_per_level`, `dtlb_mshr`, `dtlb_parked`, `l2tlb_mshr` and `page_cohorts` are
all parsed by `util.cpp` (`:40-50`, `:102`), so **no source change is required for any cell** and
each cell's own log header (`walkers=… dtlb=… mshr=…` / `l2tlb=… mshr=…`) proves the knob moved.

**Stage 1 — is it the walkers, or the injection width? (Orkut, 2 cells, ~1 min)**
| Cell | `--set` | Purpose |
|---|---|---|
| G1 | `walkers=4` | walker count at fixed page concurrency |
| G2 | `walkers=8` | does a walker count above the injection width buy anything? |

**Stage 2 — free the injection width (Orkut, 2 cells; needs approval, see caveat)**
| Cell | `--set` | Purpose |
|---|---|---|
| G3 | `walkers=4 dtlb_mshr=8 page_cohorts=4` | page concurrency 4 with walkers to match |
| G4 | `walkers=8 dtlb_mshr=8 page_cohorts=8` | saturation: text-book translation scaling |

*Caveat:* `page_cohorts` is a **declared capacity of the admission policy**, not its algorithm — the
deferral rule, ordering, retry and buffer are untouched — but the brief's frozen list does not name
it, so Stage 2 should only run on the user's explicit approval. Stage 1 needs no approval and alone
decides between "walkers bind" and "injection width binds". Note also that `page_cohorts = 2` is
documented as *derived* from `dtlb_mshr / dtlb_parked`, so raising `dtlb_mshr` without raising it
creates an internally inconsistent design point; that is why G3/G4 move the two together.

**Stage 3 — is the 300-cycle service term doing all the work? (Orkut, 1 cell)**
| Cell | `--set` | Purpose |
|---|---|---|
| G5 | `walk_latency_per_level=75` | halves the service interval: a pure-delay control |

**Stage 4 — generalisation (2 cells)** — repeat whichever of {G1, G3} won on LiveJournal and
ogbn-products. Nothing else is needed: the three workloads already bracket page working set
(1 451 / 2 525 / 2 553 pages per window) and DRAM load (37 / 25 / 46 % bank occupancy).

**Deliberately not proposed, with reasons.** The full 3 × 3 × 2 × 2 product is unnecessary because
each factor is either already rejected by existing counters (L2 TLB MSHRs: `max_occ 4`, `stalls 0`;
`dtlb_parked`: 0 walker-cycles) or is not a factor at all (window, buffer, DRAM map are frozen and
none of them is near its limit — buffer peak 4096 with 0.04–0.71 % backpressure). A `dtlb_parked`
cell would be a pure negative control: worth 23 s if anyone doubts the reasoning, worthless
otherwise.

## 7. Expected outcomes under each hypothesis

Using `cycles ≈ max(walks × 300 / W_eff, bank_busy_max)` and the walking floor
`walks × 150 / W_eff`:

* **G5 halves the floor** to 8.06 M (Orkut) — still above the 6.13 M DRAM wall, so the prediction is
  a clean **−50 %** (≈ 8.3 M). If G5 instead lands near 16.5 M, the floor model in §0 is wrong and
  every conclusion here needs revisiting; if it lands near 6.2 M, the run is DRAM-bound sooner than
  the bank totals suggest.
* **G1 (walkers 4)** — the one directly measured analogue is the published G4A Orkut cell
  `m4w4` = 13 139 921 (−21.4 % vs P0): **13.0–13.5 M** here, i.e. an effective 2.4–2.5 pages in
  flight. If G1 ≈ 16.5 M instead, walkers were never the constraint and injection width is.
* **G2 (walkers 8)** — flat versus G1 (within ±2 %) if injection width caps at ~2.5 pages; a further
  −10 % or more would mean pages in flight can exceed the cohort count and Stage 2 is unnecessary.
* **G3 (4 walkers, 4 cohorts, 8 MSHRs)** — 8.2–8.6 M (−48 %): the walking floor plus the refill tail.
* **G4 (8 walkers, 8 cohorts, 8 MSHRs)** — 6.2–6.6 M (−60 %): the translation floor (4.03 M) is now
  below the DRAM wall, so this cell should *stop improving* at DRAM service. Landing far below 6.2 M
  falsifies the DRAM-wall reading of §3.3; landing far above it means the dTLB (4 sets!) or R/L2
  MSHRs became binding, which the per-client counters will show.
* **LiveJournal / ogbn repeats** — the same shape with different walls: LJ can reach 7.15 M
  (translation stops mattering at −75 %), ogbn can reach 91.7 M (−54 %). ogbn's G1 should gain least
  (−1 to −6 %) because its measured pages-in-flight average is only 2.11, and its walker PTE traffic
  (10.3 % of DRAM reads) will shrink with the walk count.

Decision rule: **if G1 ≈ G2 ≫ baseline**, the limit is walkers and the design question becomes
"more walkers + more injection width, and what they cost". **If G1 ≈ baseline**, the limit is the
2-cohort injection width and the honest conclusion is that G6C's admission and its walker budget were
co-designed at 2 and cannot be separated without resizing both. Either way, the ceiling is DRAM at
6.1 / 7.1 / 91.7 M cycles, and no translation change can beat that.

## 8. Unresolved questions

1. **Why is the average live dTLB MSHR count 2.8 with only 2 open cohorts?** The surplus is the whole
   of the walker headroom, so it sets the size of the G1 gain. Candidate explanations: (a) a page's
   MSHR is held for the full 300 cycles while its cohort fills and moves on; (b) ~0.5–1.0 % of pages
   are re-walked inside one cohort after a dTLB eviction — the walk/cohort-open deficit is
   1 014 / 1 007 / 9 246 walks out of 107 400 / 186 875 / 1 312 297, which is far too small to
   explain +0.8 average occupancy, so (a) is the more likely mechanism but neither is proven.
2. **No occupancy histogram exists.** `tlb0_mshr_max` and `l2tlb_max_occ` record only the peak, and
   `occ` only the sum. "Cycles at maximum MSHR occupancy" — a required measurement in the brief —
   cannot be computed from any current counter. A 4-bin (or W+1-bin) occupancy histogram would be a
   *diagnostic* addition, which this task forbids; it should be added in G6D before the matrix runs
   if that distribution is wanted.
3. **`walker_util_pct` in `tools/m23g6c_extract.py` divides by `BANKS × cycles`.** That equals
   `walkers × cycles` only because walkers = banks = 2 in this configuration. Any Stage-1/2 cell with
   `walkers = 4` or `8` would report 195 % or 390 % utilisation. The G6D extractor must divide by
   `walkers × cycles` (and report "busy per walker") before a single cell is run.
4. **Is the two-level serial charge realistic?** 2 × 150 cycles is charged as one unit, so the model
   gives a walker no level overlap. Both the *number* (300) and the *pipelining* are assumptions; the
   report's conclusions depend on the structure (`cycles ≈ walks × L / W`), not on `L = 300`.
5. **Is 512 L2 TLB entries the right capacity for a 1 451–2 553-page window working set?** The value
   is [V] (`VX_config.toml:219`), and the resulting zero reuse is what makes the walk count large.
   A capacity sweep (`l2tlb_entries`, `dtlb_entries`) is the only lever that changes the *number* of
   walks rather than the rate; it is outside this brief's knob list and is therefore not proposed
   above, but it is the obvious follow-up if Stage 2 saturates at the DRAM wall.
6. **Would a real page-table hierarchy change the PTE picture?** The model fetches 2 lines per walk
   (root: one shared line, always an L2 hit; leaf: one line per 16 pages). On ogbn that is 10.3 % of
   DRAM reads at a 42 % L2 hit rate — a real design with a PWC and a warm leaf table would fetch
   less, but since PTE latency never gates a walk in this model, the effect is on bandwidth only.
   The bandwidth headroom is real (37–46 % bank occupancy), so this does not threaten any conclusion.

---

**Reproduction.** Read-only analysis of `build/wm/m23g6c/*.txt` (the 15-cell G6C matrix, unchanged,
`summary.csv` `e44970c6…`) plus `sim/src/{memory.cpp,bulk.cpp,sim.hpp}` and
`docs/m23-g4b-page-first-row-second.md` §6. No cell was run and no file outside this report was
written. Checks actually performed: source reading of the four translation stages; exact equality
`walker_busy == walks × 300` on all three G6C cells; `l2tlb_max_occ` and `l2tlb_walk_stalls` read on
all 15 cells; per-workload recomputation of `walks × 300 / 2` and the per-page residual; per-bank
DRAM busy totals; hash verification that `bulk.cpp`, `sim.hpp`, `memory.cpp`, the configuration, the
binary and the traces are the ones the M23-G6C matrix used. **Not** performed: any sensitivity run,
any new counter, any test-suite execution (nothing was modified, so there was nothing to re-verify).
