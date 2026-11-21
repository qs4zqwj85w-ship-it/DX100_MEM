# M11.2 — DRAM scheduler observability and Vortex-baseline fidelity

**Scope.** Part 1 (baseline preservation), Part 2 (diagnostic instrumentation) and Part 3
(FIFO control) are implemented. Part 4 is a source comparison. Nothing in the published
experiment path changed: FR-FCFS remains the default policy, every published configuration
is untouched, and the instrumentation is gated behind a new key that defaults to off.

Evidence labels: **[S]** verified in source · **[M]** measured (archived log, command given
in the section) · **[D]** derived by arithmetic · **[H]** hypothesis requiring a new
experiment.

**Companion:** `m11-3-diagnostic-experiments-report.md` continues this work with Parts 7–8
(visibility sensitivity, requester isolation, mapping sensitivity, bank stress, multi-seed).
It re-derives the row-locality pool as a *measured* 6.71 % (§13 there) rather than this
document's 5.5 % estimate; the recommendation is unchanged, and §9 below is read with that
number as the tighter bound.

---

## 0. What changed, and proof it is inert

| Change | File | Gate |
| --- | --- | --- |
| Requester tag on a DRAM request | `sim.hpp` (`Req::client`, `DQ::client`), `memory.cpp` (push site), `bulk.cpp` ×3 | none needed — it is data, never read by a decision |
| 12 scheduler counters + 2 histograms + per-bank/per-row vectors | `sim.hpp` (`DramStats`), `memory.cpp` (`dram_tick`) | `Config::dram_diag`, default **off** |
| Export of those counters | `system.cpp` (`collect_mem_stats`) | values stay 0 when off |
| `[17]` diagnostic block | `ab.cpp` (`print_dram_diag`, called from `print_m9`) | printed only when `dram_diag` is set |
| `--set dram_diag=0|1` | `util.cpp` | — |
| FIFO control | **no new code** — the pre-existing `frfcfs = 0` switch (`sim.hpp:61`, used at `memory.cpp:474`) | already an `environment_fields` entry (`ab.cpp:184`) |

**Inertness, measured.** The same five workloads were run with `dram_diag=0` and
`dram_diag=1`, all other flags identical:

| workload | C cycles (off → on) | B completion hash | C completion hash |
| --- | --- | --- | --- |
| W1 κ=0.25 | 125 581 → 125 581 | 2645928636 (same) | 1417164640 (same) |
| W0 κ=0.25 | 18 142 → 18 142 | 3761431536 (same) | 1307012864 (same) |
| PSCR | 623 096 → 623 096 | 1122449656 (same) | 1122449656 (same) |
| STRID | 22 664 → 22 664 | 4026732736 (same) | 710294689 (same) |
| HOT | 11 368 → 11 368 | 3287754952 (same) | 3287754952 (same) |

Every cycle count and every hash is bit-identical: the flag cannot influence a decision
**[M]**. Wall-clock cost of the diagnostic itself: 3 W1 runs take 2.02 s with the flag off
and 2.05 s with it on (**+1.5 %**, within run-to-run noise) **[M]**.

**Regression.** `make -j4` completes with no warnings; `./bin/dx100_tests` reports
**4032 checks, 0 failures**; the published M9 anchors reproduce (W1 κ=0.25 C = 125 581,
W0 κ=0.25 C = 18 142, E0 control still 153 475 — §8) and every FIFO run reports
`M9 GATES PASSED` **[M]**.

---

## 1. Instrumentation design and implementation (Part 2)

### 1.1 The twelve requested metrics, and where each comes from

| # | Metric | Field | Site |
| --- | --- | --- | --- |
| 1 | DRAM requests by requester | `client_reads[3]`, `client_hits[3]` (0 SIMT, 1 engine, 2 PTE) | `dram_tick`, after the issue |
| 2 | Queue occupancy mean/peak/distribution | `q_occ_sum`, `max_q` (existing) + `q_occ_hist[16]` | top of `dram_tick`, per cycle |
| 3 | Per-bank queue occupancy | `bank_q_sum[4]`, `bank_q_max[4]` | top of `dram_tick`, scanning the queue |
| 4 | Per-bank / per-row distribution | `bank_reads/hits/conf/busy[4]`, `row_acts_` (`banks × rows`), `rows_used`, `row_acts_sum/max` | after the issue |
| 5 | Row-hit candidates at each decision | `hit_cand_sum` | selection loop |
| 6 | Whether a hit was selected when eligible | `missed_hit` (case A) | after the issue |
| 7 | Requests delayed by bank timing | `stall_case_b`, `hit_busy_sum` | selection loop + after the issue |
| 8 | No eligible hit candidate available | `stall_case_c` | after the issue |
| 9 | Queue waiting time | `wait_sum`, `wait_max`, `wait_hist[16]` (`cycle − arrive`) | after the issue |
| 10 | DRAM service time | `svc_hit_cycles`, `svc_miss_cycles` | after the issue |
| 11 | Activations per bank and row | `bank_conf[4]` + cold starts, `row_acts_` | after the issue |
| 12 | `hit_busy_sum` | `dram.hit_busy_sum` | selection loop |

### 1.2 The three cases, kept separate

Inside the selection loop (`memory.cpp:459–477`) each queued entry is classified once per
decision, and the counters never mix:

* **A — a useful row hit was in the queue and selectable, but another request was chosen.**
  `missed_hit`. Under FR-FCFS this is impossible by construction (`memory.cpp:474` prefers
  any eligible open row), so a non-zero value is a bug indicator rather than a metric.
  **Measured: 0 in every workload and every arm** (§2) — which is the control that proves
  the other two counters are meaningful.
* **B — a row hit was in the queue but its bank was still busy.** `hit_busy` is accumulated
  per decision for every bank-blocked hit; `stall_case_b` counts the decisions where such a
  hit existed and a non-hit was issued instead (the only option, since the hit was
  ineligible).
* **C — no row hit existed in the queue at all.** `stall_case_c`.

`hit_busy_sum / decisions` is the average number of row hits the bank could not take, i.e.
the requested `hit_busy_sum` metric; `stall_case_b + stall_case_c` is exactly the number of
activation-causing commands (`reads − hits`), which is how the classification is validated
(§5).

### 1.3 What the instrumentation does *not* do

* It reads no state that any decision depends on: the classification happens *before* the
  selection but writes only to `dram.*` counters; nothing in the loop reads them.
* It changes no queue entry, no bank timer, no row state, no completion time. The bit-identical
  cycle counts and hashes in §0 are the proof, not the argument.
* The `row_acts_` vector (`banks × rows` = 8 192 `u32` = 32 KiB) is allocated **only** when
  the flag is set, so a normal run allocates exactly what it allocated before.

---

## 2. FR-FCFS behaviour, measured (Part 2/§4)

All cells: `N = 16384`, `seed = 1`, `R = 128` slots, `batch_window = 8192`,
`batch_deadline = 4096`, unbounded DRAM queue, `mem_path = l1`, `experiment=m9diag`,
`--set dram_diag=1`. Knobs per workload are in the table header of §6.

```
--config configs/ab_controlled.cfg --set N=16384 --set rho_dup=0 --set elem_bytes=4 \
  --set bulk_wl=E0 --set seed=1 --set batch_window=8192 --set bulk_max_elems=262144 \
  --set max_cycles=200000000 --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
  --set page_sched=1 --set idx_mode=2 --set experiment=m9diag --set pool_slots=128 \
  --set Lambda=<L> --set P=<P> --set kappa=<K> --set bulk_stream=<S> --set dram_diag=1
```

| workload | arm | commands | decisions | hit candidates / decision | eligible hits / decision | bank-blocked hits / decision | case A | case B | case C |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| W1 κ=0.25 | B (bulk) | 13 771 | 118 291 | 5.75 | 0.35 | 5.40 | **0** | 1 580 | 580 |
| W1 κ=0.25 | C (M10) | 13 589 | 113 917 | 5.79 | 0.36 | 5.43 | **0** | 1 611 | 595 |
| W0 κ=0.25 | B | 2 046 | 16 650 | 8.79 | 0.62 | 8.17 | **0** | 115 | 35 |
| W0 κ=0.25 | C | 1 792 | 15 109 | 8.28 | 0.56 | 7.72 | **0** | 129 | 63 |
| PSCR | B = C | 17 556 | 184 699 | 1.88 | 0.09 | 1.79 | **0** | 3 349 | 4 547 |
| STRID | B | 2 056 | 16 079 | 7.27 | 0.48 | 6.79 | **0** | 195 | 44 |
| STRID | C (all-SIMT) | 1 029 | 15 083 | 4.03 | 0.24 | 3.79 | **0** | 1 | 130 |
| HOT | B = C | 1 031 | 7 658 | 10.04 | 0.70 | 9.34 | **0** | 54 | 17 |

**[M]** Four facts follow directly:

1. **FR-FCFS never passes over a selectable row hit** (case A = 0 everywhere). The policy is
   implemented as documented; there is no "policy bug" headroom.
2. **Row hits are plentiful in the queue but rarely eligible.** On W1 ~94 % of the row hits
   present at a decision are bank-blocked (5.43 of 5.79), and only 0.36 is selectable on
   average. The scheduler is not short of information; it is short of *ready banks*.
3. **Cases B and C partition every activation** (`reads − hits` = B + C): W1 C
   2 206 = 1 611 + 595 ✓, W0 C 192 = 129 + 63 ✓, PSCR 7 896 = 3 349 + 4 547 ✓,
   STRID B 239 = 195 + 44 ✓, HOT 71 = 54 + 17 ✓ **[D]**. The classification is exhaustive and
   mutually exclusive.
4. **Case B dominates wherever locality is high** (73 % on W1, 67 % on W0, 84 % on STRID B,
   76 % on HOT) and case C dominates only on PSCR (58 %), the low-locality workload
   (55 % hit rate, 4 128 distinct rows). Interpretation: where the stream has locality, the
   queue already holds the right requests and bank timing is the limiter; where it does not,
   the requests are simply not row hits.

---

## 3. Per-bank visibility (Part 2/§3)

The M11.1 correction stands: occupancy is global. With the per-bank counters this is now
**measured** rather than inferred.

| workload / arm | bank 0 reads / hits / conflicts | bank 1 reads / hits / conflicts | bank 0 mean / peak depth | bank 1 mean / peak depth |
| --- | --- | --- | --- | --- |
| W1 κ=0.25 C | 6 807 / 5 712 / 1 094 | 6 782 / 5 671 / 1 110 | 5.09 / **16** | 5.57 / **16** |
| W1 κ=0.25 B | 6 912 / 5 830 / 1 081 | 6 859 / 5 781 / 1 077 | 4.85 / 15 | 4.96 / 16 |
| W0 κ=0.25 C | 901 / 813 / 87 | 891 / 787 / 103 | 6.22 / 15 | 4.91 / 15 |
| PSCR | 8 779 / 4 835 / 3 943 | 8 777 / 4 825 / 3 951 | 0.60 / 11 | 0.58 / 11 |
| STRID B | 1 029 / 911 / 117 | 1 027 / 906 / 120 | 6.26 / 15 | 4.92 / 15 |
| HOT | 518 / 480 / 37 | 513 / 480 / 32 | 4.86 / 10 | 3.88 / 7 |

**[M]** *Reads* split evenly — within 1 % on every workload — because consecutive lines
alternate banks by construction of the map (`bank = bit0(line) ^ …`, M11.1 §1.3). *Occupancy*
need not follow: the mean per-bank depth differs by up to 27 % (W0 6.22 vs 4.91, STRID B
6.26 vs 4.92, HOT 4.86 vs 3.88), always with bank 0 deeper. The target lines are not the
only traffic — the index array, the descriptor and the PTE fetches are not subject to the
even-line skew, and in HOT every target line lands in bank 0 — so the surplus sits where the
non-target requests go.

The M11.1 inference (8/8 at peak) is confirmed: **peak per-bank depth is 16 on W1**, so at
the peak the queue did hold a full bank-row's worth of lines for one bank. M11.1's "at most
half a row per bank" is right on average (5.09–5.57 per bank) and wrong at the peak.

Queue-depth distribution, W1 κ=0.25 C **[M]** (`q_occ_hist`, log2 buckets): depth 1 = 14 510
cycles, 2–3 = 5 151, 4–7 = 8 266, **8–15 = 97 571 (78 % of the run)**, 16–31 = 84, ≥32 = 0.
So the controller sits at 8–15 visible requests for most of the run and touches 16 rarely —
the 16-entry bound is real but rarely saturated.

---

## 4. Row-hit opportunity and activations (Part 2/§5)

| workload / arm | command | hits | activations | distinct (bank,row) | activations per row | max on one row | queue wait mean / max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| W1 κ=0.25 C | 13 589 | 11 383 | 2 206 | **1 050** | **2.10** | 19 | 98.5 / 328 |
| W1 κ=0.25 B | 13 771 | 11 611 | 2 160 | 1 057 | 2.04 | 20 | 99.0 / 450 |
| W0 κ=0.25 C | 1 792 | 1 600 | 192 | 90 | 2.13 | 6 | 112.7 / 328 |
| PSCR | 17 556 | 9 660 | 7 896 | 4 128 | 1.91 | 24 | 42.1 / 310 |
| STRID B | 2 056 | 1 817 | 239 | 161 | 1.48 | 5 | 101.2 / 396 |
| STRID C | 1 029 | 898 | 131 | 129 | 1.02 | 2 | 120.2 / 242 |
| HOT | 1 031 | 960 | 71 | 65 | 1.09 | 4 | 96.4 / 224 |

**This overturns the M11.1 estimate.** M11.1 assumed each 4 KiB page occupies 4 (bank,row)
pairs and derived a floor of 2 048 activations. The measured distinct-pair count is
**1 050**. The reason is in the workload generator, not in the scheduler:

**[S]** `workload.cpp:36–42` builds each page's line set as
`line = p·64 + j·(64/λ)` with `λ = Λ/P` lines per page. For W1 (Λ = 16 384, P = 512) λ = 32
and the stride is **2**, so a page's accessed lines are the **even** lines `0, 2, … 62`.
Even lines have `bit0 = 0`, so `bank = bit6 ^ bit12` is *constant within the page* and the
page's lines split over just **two row indices in one bank**. M11.1's "4 pairs per page"
was wrong by 2×; the correct floor is ≈ 1 024 (+ descriptor/PTE rows), i.e. ~1 050
**[D]**, exactly what the counter reports.

### 4.1 Why each row is activated twice — the window boundary, measured

| configuration | batches | activations | distinct rows | act/row | W1 C cycles |
| --- | --- | --- | --- | --- | --- |
| `batch_window = 8192` (published) | **2** | 2 160 | 1 057 | **2.04** | 125 581 |
| `batch_window = 16384` | **1** | 1 133 | 1 057 | **1.07** | 106 456 |
| `batch_window = 262144` | 1 | 1 133 | 1 057 | 1.07 | 106 456 |

**[M]** The same 1 057 rows, the same traffic, half the activations: with one window the
order is within **7 %** of the floor. The doubling is therefore caused by the *window
boundary*: each of the two windows re-treads all 512 pages once, so every row is activated
once per window. **Within a window the issue order is already near-optimal.**

The obvious competing explanation — the two live windows interleaving at the DRAM under
`org_overlap = 1` — was tested and **refuted** **[M]**:

| | activations | distinct rows | act/row |
| --- | --- | --- | --- |
| `org_overlap = 1` | 2 160 | 1 057 | 2.04 |
| `org_overlap = 0` | 2 149 | 1 057 | 2.03 |

Turning the overlap off does not help, because both windows exist either way; each still
walks every page once.

**Cost of the surplus [D]:** W1 C has 2 206 − 1 050 = 1 156 surplus activations;
1 156 × 12 cycles of bank time ÷ 2 banks = **6 936 cycles = 5.5 %** of 125 581. For arm B:
1 103 × 12 ÷ 2 = 6 618 = 4.8 % of 138 924. Note the single-window run saves 19 125 cycles
(15.2 %), six times more than the activation component — that run also changes the entire
engine schedule, so its 15 % is **not** a DRAM-ordering gain.

---

## 5. Bank-timing stalls (Part 2/§7, §8)

**[M]** On W1 κ=0.25 C: 113 917 decisions, of which 100 328 blocked (no bank ready, from
`[7]`), 11 665 starved (empty queue), 13 589 issued. Of the issued commands,
**1 611 (73 %) were activation-causing only because the wanted row's bank was busy**
(case B) and 595 because no row hit was queued at all (case C). Zero were scheduler
mistakes (case A).

The mean bank-blocked-hit count per decision is 5.43 — i.e. at nearly every decision the
queue holds ~5 row hits the controller cannot use this cycle. This is the quantitative form
of "the residual is bank timing, not visibility": the requests are *there*, the banks are
not ready. That is also why the arbiter's leverage is small (§6): it can only choose among
the 0.36 eligible hits on average.

PSCR is the exception in kind: 4 547 case-C decisions against 3 349 case-B, i.e. the queue
often holds no row hit at all — a property of the workload (55 % hit rate, 4 128 rows for
17 556 requests, 1.91 activations per row), not of the policy.

---

## 6. FIFO versus FR-FCFS (Part 3)

The policy switch already existed (`frfcfs = 0`, `memory.cpp:474`): with it false the open-row
preference is not applied, so selection is *oldest eligible arrival* — FIFO among requests
whose bank is ready (`memory.cpp:467` still enforces bank readiness, and the queue is kept in
arrival order, so the first eligible entry *is* the oldest eligible one; the later
`arrive <` comparison at `memory.cpp:475` can never fire). Nothing else changes: same queue,
same timing, same bank state, same completion path. FR-FCFS remains the
default and no published configuration sets `frfcfs = 0`.

| workload | policy | A cycles | B cycles | C cycles | B row hits | B activations | gates |
| --- | --- | --- | --- | --- | --- | --- | --- |
| W1 κ=0.25 | FR-FCFS | 220 123 | 138 924 | **125 581** | 11 611 | 2 160 | pass |
| W1 κ=0.25 | FIFO | 222 838 | 138 991 | **126 328** | 11 545 | 2 225 | pass |
| W0 κ=0.25 | FR-FCFS | 11 052 | 17 267 | **18 142** | 1 896 | 150 | pass |
| W0 κ=0.25 | FIFO | 11 052 | 17 393 | **18 890** | 1 833 | 213 | pass |
| PSCR | FR-FCFS | 1 908 638 | 623 096 | 623 096 | 9 660 | 7 896 | pass |
| PSCR | FIFO | 1 908 638 | 623 096 | 623 096 | 9 626 | 7 930 | pass |
| STRID | FR-FCFS | 22 664 | 18 596 | 22 664 | 1 817 | 239 | pass |
| STRID | FIFO | 22 664 | 19 093 | 22 664 | 1 714 | 342 | pass |
| HOT | FR-FCFS | 4 361 | 11 368 | 11 368 | 960 | 71 | pass |
| HOT | FIFO | 4 361 | 11 368 | 11 368 | 959 | 72 | pass |

**[M]** Deltas (FIFO − FR-FCFS):

| workload | A | B | C | comment |
| --- | --- | --- | --- | --- |
| W1 κ=0.25 | **+1.23 %** | +0.05 % | +0.59 % | FR-FCFS also helps pure SIMT (+1.2 %) despite SIMT's 4 % row-hit rate |
| W0 κ=0.25 | 0 | +0.73 % | **+4.13 %** | the largest policy effect in the set |
| PSCR | 0 | 0 | 0 | identical cycles despite +34 activations |
| STRID | 0 | **+2.67 %** | 0 (all-SIMT arm C) | 103 extra activations, +2.7 % on B |
| HOT | 0 | 0 | 0 | identical |

Three findings:

1. **The scheduler's row-hit priority is worth 0–4.1 %**, and exactly 0 on three of five
   workloads even where it cuts activations substantially (STRID: 239 → 342 activations,
   PSCR: 7 896 → 7 930, both 0 % cycles). Those runs are not DRAM-bound, so extra
   activations are absorbed.
2. **Where it does pay, it pays as bank time, not as hits**: W0 C gains 4.1 % from
   63 more hits (+150 → 213 activations saved).
3. **The arrive order matters more than the policy.** W1's order is page-major and achieves
   84 % hits with either policy; FIFO still scores 83.8 %.

So FR-FCFS is retained as the baseline (Part 1), and it is *doing what it claims*, but its
contribution is small compared with the effect of the issue order (§4).

---

## 7. Vortex / Ramulator fidelity (Part 4)

Our scheduler is **not** Ramulator and **not** Vortex; it is an independent 2-bank model
whose *policy* matches Ramulator's default and whose *parameters* are transcribed from
Vortex's platform configuration.

| Aspect | Our model (`sim/src/memory.cpp`) | Vortex default sim backend (Ramulator via `sim/common/dram_sim.cpp`) |
| --- | --- | --- |
| Selection rule | one scan per cycle, max one command; prefer any eligible open row, else oldest `arrive` **[S]** | `Controller/Scheduler = FRFCFS`, `RowPolicy = OpenRowPolicy`, `AddrMapper = RoBaRaCoCh` **[S]** (config only — see limitation below) |
| Age / tie-break | `arrive` cycle, strict `<`, first-in-queue wins ties **[S]** | Ramulator FRFCFS queue order (not inspected: source not vendored) **[?]** |
| Bank readiness | `bank_ready_[bank] > cycle` ⇒ ineligible **[S]** | per-bank command timing from the JEDEC timing table **[S]** |
| Row-hit priority | strict, among eligible **[S]** | FRFCFS = row hits first **[S]** (by name and config; implementation not in tree) |
| Channels / banks | **2 banks**, no channels, no bank groups **[S]** | 2 channels (one per platform memory bank), HBM2 8 Gb = 2 pseudo-channels × 4 bank groups × 4 banks per channel, 1 KiB rows **[S]** |
| Address mapping | `bank = (L ^ L>>6 ^ L>>12) % 2`, `row = (L/32) % 4096` **[S]** | `RoBaRaCoCh` (row / bank / rank / column / channel) with the bank XOR-folded by the row inside an 8 Gb channel window **[S]** |
| Open-row policy | implicit: a row stays open until another activates **[S]** | `OpenRowPolicy` **[S]** |
| Queue capacity | unbounded in the published config (`dram_q = 0`) **[S]** | `VX_CFG_PLATFORM_MEMORY_QUEUE_SIZE = 0` = unbounded per channel **[S]** |
| Timing parameters | flat `t_hit = 14`, `t_miss = 26`, `link = 0` **[S]** | full JEDEC set (tRCD/tRP/tCAS/tCCD/tWTR/…) from `dram_timing`, HBM2 at a 2:1 data rate; the config comment states the intent as "14 cycles for an idle open-row read, 26 for a row conflict" **[S]** |
| Refresh | none **[S]** | `RefreshManager = AllBank`, with an in-source note that refresh is effectively not modelled **[S]** |
| Write traffic | none **[S]** | full write buffering and bus turnaround **[S]** |
| Burst granularity | 64 B line, one command **[S]** | 32 B transactions, 2 per 64 B block **[S]** |
| Command bus | one command per cycle, no bus constraints **[S]** | one command per cycle; on HBM an ACT holds the row-command bus for two (bank model) **[S]** |

**Which is "the" Vortex behaviour.** Vortex RTL has no DRAM scheduler at all
(`hw/rtl/mem/` is AXI/bus arbitration; M11.1 §4). Both Vortex simulators drive `DramSim`
(`sim/simx/mem/memory.cpp:44`, `sim/rtlsim/processor.cpp:148`), whose **default** backend is
Ramulator with FRFCFS; the alternative `VORTEX_DRAM_MODEL=bank` backend is
**first-come-first-served per bank**, not FR-FCFS (`sim/common/dram_bank_model.h:33–40`).

**Therefore:** keeping FR-FCFS as our baseline represents Vortex's *default simulation*
behaviour, which is the relevant comparison. It does not represent Vortex RTL, and our
numbers are **not** cycle-comparable with Ramulator's: 2 flat banks at 14/26 cannot be
compared with 32 bank-group-ordered banks with per-command timing, refresh and write
turnaround. The FIFO control of §6 corresponds to neither Vortex backend — it is our own
control for the value of row-hit priority.

**Limitation:** `ref/vortex/third_party/ramulator/` is an **empty submodule directory** in
this tree, so Ramulator's scheduler code could not be inspected. Everything said about it
above comes from the YAML `DramSim` constructs and the surrounding comments; the fields we
could not verify are marked **[?]**.

---

## 8. Performance impact, validation and limitations

**Validation**

| Check | Result |
| --- | --- |
| `make -j4` | clean, no warnings |
| `./bin/dx100_tests` | **4032 checks, 0 failures** |
| FR-FCFS anchors unchanged | W1 κ=0.25 C = 125 581; W0 κ=0.25 C = 18 142; E0 control (`win_policy=0`) = 153 475 **[M]** |
| Hashes unchanged | identical with the flag on and off, all five workloads (§0) |
| Instrumentation inert | bit-identical cycles *and* hashes; 0 case-A events in every cell |
| FIFO validity | `M9 GATES PASSED` in all five workloads; element conservation, address match and completion hashes all checked by the gates. The *hash values* differ from FR-FCFS on W1/W0/STRID-B by construction (a different policy completes elements at different cycles) and agree on PSCR, HOT and STRID's all-SIMT arm, where the two policies produce the same timeline — the gates, not hash equality, are the correctness criterion |
| No lost/duplicated requests | gates `elements total == done + faults`, `window records account for every admitted element`, `target requests generated == issued` |

The per-bank line of `[17]` was also relabelled during this work — it printed one number
(reads) under a "reads / hits / conflicts" caption. That is print-only; all ten archived logs
were regenerated afterwards and every A/B/C cycle count reproduced exactly
(W1 220 123 / 138 924 / 125 581 and so on), so the archived logs and the report agree.

**Cost.** +1.5 % wall clock with the diagnostic on; 32 KiB of `u32` for the per-row vector,
only when enabled; 16-entry histograms; no effect on cycles or hashes.

**Limitations**

1. Two banks, no channels, no bank groups, no columns, no refresh, no writes, flat 14/26
   timing — every quantitative conclusion here is a property of *this* model (M11 §9).
2. One footprint (N = 16 384), one seed. The window-boundary result (§4.1) is measured at
   `batch_window = 8192` on two windows and should be re-checked at other sizes before it is
   generalised.
3. `wait_sum` measures from L2 MSHR allocation (`DQ::arrive`), not from the client's
   request formation; translation and L1 time are excluded by construction.
4. The activation floor (1 050) is a *measured* distinct-pair count for this run. It is a
   floor only if every pair must be activated at least once — true while rows are `% 4096`
   and the footprint is smaller than the 8 MiB period.
5. Case B vs C attributes each activation to the decision that caused it; it does not prove
   that a different *arrival order* would have avoided it. §4.1 supplies that evidence
   separately (one window ⇒ 1.07 act/row).
6. PSCR and HOT are all-bulk runs, so their C column equals B; STRID is all-SIMT, so its C
   column is the SIMT baseline. The FIFO comparison is therefore only meaningful on the B
   column for those three, and the table is read accordingly.

---

## 9. Recommendation

**Do not implement a Bulk-side DRAM organizer (Row Table / physical-address sorter) now.**

The instrumentation made the question decidable, and it decided against the organizer:

1. **The scheduler is not the problem.** Case A = 0 in every cell: FR-FCFS never passes over
   a selectable row hit. Its total contribution over FIFO is 0–4.1 %, and 0 on three of five
   workloads.
2. **Within a window, the issue order is already near-optimal.** One window of the same
   workload reaches **1.07 activations per row** against a floor of 1.00 (§4.1), i.e. a
   surplus of 0.07 × 1 057 ≈ 74 activations ≈ 444 cycles ≈ **0.35 %**. A Row Table reordering
   the *window's* contents has that much left to capture, no more.
3. **The measured surplus is a windowing artifact, and it is cross-window.** The published
   two-window run re-treads every page once per window (2.04 act/row), worth
   ≈ **5.5 %** of cycles as an upper bound. Recovering it requires issuing *across* windows
   in one row order — i.e. merging or co-scheduling the window streams — not a per-window
   organizer. M11.3 §13 replaces this estimate with a measurement: forcing every access to be
   a row hit saves 6.71 %, so that is the whole pool any row-ordering change can draw from.
4. **Bank timing dominates the residual.** 73 % of W1's activations are case B: the hits are
   in the queue and the banks are busy. An organizer that changes *which* requests are queued
   cannot make a bank ready sooner; at 86 % bank utilisation there is little slack to win.

**The cheaper next step, if this line is continued:** test cross-window row co-scheduling —
let the second window's issue advance only into rows that do not disturb the first's open
rows, or drain a window's rows to completion before interleaving the next. That is a bounded
change to `win_emit`'s page cursor (one config knob, no new storage), and the new counters
give it a direct success metric (`activations per row`, target 1.0–1.1). If it delivers
≲ 1 %, the DRAM-ordering avenue is closed for this architecture and the effort belongs
elsewhere; if it approaches the 5.5 % bound, an organizer becomes worth revisiting with
evidence.

No Row Table, address sorter or new bulk scheduling structure was implemented; FR-FCFS
remains the default and every published anchor reproduces exactly.
