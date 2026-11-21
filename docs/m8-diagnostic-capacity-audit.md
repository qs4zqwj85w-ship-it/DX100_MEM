# M8 Follow-up 3 — Diagnostic Integrity and Capacity Attribution

**Scope.** An audit of the M8 failures reported in `m8-adversarial-workloads.md` and of the
large-N regressions reported in `m8-scaling-experiment.md`. No M9 work, no ISA change, no RTL
change, no baseline re-tuning, no historical result altered. `R = 128`, the cache/TLB/DRAM
hierarchy, `idx_mshr = 64`, `idx_reserve = 8` and every shared parameter are exactly the published
ones. No oracle arm was used for any performance number; the two oracle rows are quoted only as a
labelled diagnostic bound in §3.4.

**Headline.** All 34 violation messages from the adversarial suite reduce to **two diagnostic
defects and zero simulator defects**. Neither is a hardware or model fault: one is an incomplete
accounting identity (it omits the two terms that absorb a request above the L2), the other is an
unstated cross-arm assumption (it compares total request counts across arms whose target counts
legitimately differ). Both survive the "does SIMT fail too?" test that the earlier report used, and
both are fixed here as *additional* invariants with the originals preserved. Separately, M8's
large-N regression is a **capacity** problem — the 128-live-line index ring — whose cost is paid
through **translation** (extra page walks) at zero duplication and through **deduplication
efficiency** under heavy duplication. Raising the ring from 64 to 128 lines per tile, changing
nothing else, moves N = 65 536 from +11.2 % to +0.09 % versus M7.

---

## 0. Method and reproducibility

Every number below was produced by the unmodified binaries built from `make -j4` at this
commit-less working tree, with `configs/ab_controlled.cfg` as the base. The anchor points reproduce
the published figures exactly: W1 N = 16 384 → 270 753 / 233 475 / 156 912 / 156 419, and
W1 N = 262 144 → A = 36 901 839, M7 = 25 327 321, M8 = 28 538 534.

The only source change in this work is the diagnostic one described in §2.4. After it, the test
suite reports **3 979 checks, 0 failures** — identical to the M8 state — and a
row-by-row diff of the 21 adversarial diag runs shows **0 cycle changes in 105 rows**, so the
change reclassified results without touching any measurement.

Arms are unchanged from M8: **A** = SIMT, **M6** = `batch_window=8192, page_sched=0, idx_mode=0`,
**M7** = `page_sched=1, idx_mode=0`, **M8** = `page_sched=1, idx_mode=2`.

---

## 1. Part 1 — audit of every adversarial failure

### 1.1 What was actually run, and a correction to the published count

The measured (`experiment=ab`) adversarial runs are **all clean**: 21/21 report `status: OK`,
including every identity in `run_ab` (generated == issued == completed, `lines issued + merged ==
elements`, descriptor/index counts, residency ≤ R). The failures live exclusively in the
`experiment=diag` runs, because both offending identities are defined only there.

Reproducing those (7 workloads × 3 arms, N = 16 384, seed 1) gives **9 clean runs and 12 runs with
violations**, carrying **34 messages**. `m8-adversarial-workloads.md` §4 states "seven of
twenty-one runs report `status: OK`; fourteen report violations". That count does not reproduce.
The report's own prose ("HOT, DUP, PGLO, STRID") implies 4 × 3 = 12, which is what is measured.
The likely cause is that two of the arms in that run differed from the arms reproduced here; the
exact commands are not recorded in the report, which is itself a reproducibility gap. Nothing in
this audit depends on the count, but the published sentence should be treated as wrong.

### 1.2 Failure matrix (pre-fix binary)

`DRAM` = `DRAM reads != requests + PTE fetches - L2 hits - L2 joins`; `IDX` = `the index array is
not exactly 1024 requests of B0`; `DESC` = `the descriptor is not exactly one request of B0`.
Arms are the `B0..B3` diag rows; `A` is the SIMT reference.

| Workload | Arm | Msgs | Checks fired (rows) | SIMT fails the same check? |
|---|---|---:|---|---|
| HOT | M6 | 3 | DRAM: A, B2, B3 | **yes** |
| HOT | M7 | 3 | DRAM: A, B2, B3 | **yes** |
| HOT | M8 | 6 | IDX; DRAM: A, B0, B1, B2, B3 | **yes** |
| DUP | M6 | 4 | IDX; DRAM: A, B2, B3 | **yes** |
| DUP | M7 | 4 | IDX; DRAM: A, B2, B3 | **yes** |
| DUP | M8 | 4 | IDX; DRAM: A, B2, B3 | **yes** |
| PGLO | M6 | 2 | IDX; DRAM: A | **yes** |
| PGLO | M7 | 2 | IDX; DRAM: A | **yes** |
| PGLO | M8 | 2 | IDX; DRAM: A | **yes** |
| STRID | M6 | 1 | IDX | n/a (not a DRAM check) |
| STRID | M7 | 1 | IDX | n/a |
| STRID | M8 | 2 | IDX; DESC | n/a |
| PSCR, ROWLO, PERM | all | 0 | — | — |

Two details in that table contradict the published §4 and matter for classification:

* The report says the DRAM identity "fired by **A** as well as by **every bulk arm** on HOT, DUP,
  PGLO and STRID". Measured: it fires on **no** bulk arm for PGLO, on **no** arm at all for STRID,
  and on the bulk arms for HOT/DUP only via the *oracle* rows B2/B3 — except HOT-M8, where B0/B1
  also fire (by exactly 2 reads, see §1.3).
* The report says PSCR/ROWLO/PERM are fully clean. That part is **correct**.

### 1.3 Defect 1 — the DRAM identity is incomplete (not a model error)

The identity is checked in `src/ab.cpp` (`run_index_oracle_diag`, the per-row loop over
`all = {A, B0..B3}`):

```
dram_reads == total_req + pte_fetches - l2_hits - l2_joins
```

Source: `MemorySystem::l1_probe` (`src/memory.cpp:342`) returns 0 on an L1 hit and 1 on an L1 MSHR
join **without ever calling** `MemorySystem::l2_probe` (`src/memory.cpp:298`). `l2_probe` is the
only caller that can enqueue DRAM work (`src/memory.cpp:424`, `dram_tick`). A request that is
absorbed by the L1 therefore contributes to `total_req` but produces no L2 read, no L2 hit and no
DRAM read. The identity omits exactly those two terms:

```
dram_reads == total_req - l1_hits - l1_joins + pte_fetches - l2_hits - l2_joins      (corrected)
```

Measured, from the paired runs that print every term:

| Run / arm | req | L1 hits | L1 joins | PTE | L2 hits | L2 joins | DRAM | original | corrected |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| HOT / A | 8 192 | 8 063 | 127 | 2 | 0 | 0 | **4** | 8 194 (✗) | **4** ✓ |
| HOT / B0 (M8) | 1 031 | 2 | 0 | 36 | 33 | 1 | **1 031** | 1 033 (✗) | **1 031** ✓ |
| HOT / B0 (M6) | 1 029 | 0 | 0 | 36 | 34 | 0 | 1 031 | 1 031 ✓ | 1 031 ✓ |
| DUP / A | 16 384 | 15 888 | 240 | 8 | 4 | 2 | **258** | 16 386 (✗) | **258** ✓ |
| DUP / B0 | 1 793 | 0 | 0 | 42 | 550 | 0 | 1 285 | 1 285 ✓ | 1 285 ✓ |
| PGLO / A | 16 384 | 765 | 13 | 32 | 14 610 | 2 | **1 026** | 1 804 (✗) | **1 026** ✓ |
| STRID / A | 4 096 | 0 | 0 | 128 | 3 195 | 0 | 1 029 | 1 029 ✓ | 1 029 ✓ |

The corrected form is **exact on all 42 rows** (21 runs × {A, B}), including every row where the
original fires and every row where it does not. The failure is therefore a property of the
workload, not of an arm: it fires wherever `l1_hits + l1_joins > 0`, i.e. wherever the distribution
has reuse, which is why the SIMT arm fails it too. **Classification: diagnostic defect (false
positive). Not a simulator or accelerator defect.**

The earlier report's causal sentence ("a line joined in the L1 for many elements is not
distinguishable from a per-request read in that identity") is right in spirit but names the wrong
term: L1 *hits* dominate (8 063 versus 127 joins on HOT) and joins are a minor contributor.

### 1.4 Defect 2 — the index/descriptor identities compare totals across arms

```
if (b0.total_req != b2.total_req + expect_idx) fail("the index array is not exactly ... requests of B0");
if (b0.total_req != b1.total_req + 1)         fail("the descriptor is not exactly one request of B0");
```

`expect_idx` (1 024) is a per-arm volume, and B0's index count is separately and correctly checked
(`b0.index_req == expect_idx`, which passed in all 21 runs). What the total-based form additionally
asserts is that `b0.target_req == b2.target_req` — that removing the index array does not change how
many *target* requests the measured arm forms. That is false whenever deduplication is
timing-sensitive: B2 starts issuing 2–4 K cycles earlier (`firstTgt` 4 115 → 2 382 on HOT), so its
windows are composed from a different slice of elements and fold a different number of elements
onto the same line.

The residual is *exactly* the target-count difference in every case:

| Run | B0 req | B2 req | B1 req | B0 tgt | B2 tgt | B1 tgt | IDX residual | target residual | DESC residual | target residual |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| HOT M8 | 1 031 | 5 | 1 030 | 6 | 4 | 6 | **2** | **2** | 0 | 0 |
| DUP M6/M7/M8 | 1 793 | 513 | 1 792 | 768 | 512 | 768 | **256** | **256** | 0 | 0 |
| PGLO M6/M7/M8 | 4 097 | 2 049 | 4 096 | 3 072 | 2 048 | 3 072 | **1 024** | **1 024** | 0 | 0 |
| STRID M8 | 4 049 | 2 049 | 4 068 | 3 024 | 2 048 | 3 044 | **976** | **976** | **−20** | **−20** |
| PSCR / ROWLO / PERM | — | — | — | equal | equal | equal | 0 | 0 | 0 | 0 |

`IDX residual == target residual` in 21/21 runs, and STRID-M8's descriptor residual is its own
target delta. No arm loses or invents an index or descriptor request. **Classification: invalid
cross-arm assumption (false positive). Not a defect in any arm.**

Note the asymmetry that the earlier report got wrong: on HOT, only M8 trips `IDX` (2 extra target
requests); M6/M7 do not, because their dedup happens to land on the same count as the oracle.

### 1.5 What is genuinely correct

These passed on all 21 runs and are the checks that would catch a real defect:

| Property | Where | Result |
|---|---|---|
| every element completes exactly once | `elements total == done + faults`, `window retired every element`, `elems == desc.count` | 21/21 |
| correct target address | `addr_mismatch == 0`, `addr_set_hash == A` (order-independent multiset) | 21/21, `hashOK: yes` in all 42 rows |
| correct returned value | `value_hash == A.expected_hash` | 21/21 |
| merged requests fan out correctly | `lines issued + merged == win_entries` (`src/bulk.cpp`, conservation block) | 21/21 |
| no lost/duplicated elements | `generated == issued == completed`, `index lines requested == completed` | 21/21 |
| residency preserved | `peak_slots <= R` | 21/21 |

**Verdict for Part 1: 12 failing runs, 34 messages, 2 root causes, 0 real correctness defects.**
Every failure is a false positive produced by an identity that is either incomplete (§1.3) or
applied across arms whose target volumes legitimately differ (§1.4).

---

## 2. Part 2 — correct diagnostic invariants

### 2.1 The three categories, and where each lives

**Category 1 — semantic correctness** (arm-independent, must hold identically in every arm):

* S1 `elements_done == desc.count` and `elements_total == done + addr_faults`, with `addr_faults = 0`.
* S2 `addr_mismatch == 0` — every generated address is the one the index array names.
* S3 `addr_set_hash` equal across arms — the *multiset* of target addresses, not their order.
* S4 `value_hash == expected_hash` from the SIMT reference, element for element.
* S5 `completion_hash` is *not* comparable (it folds at retire time); the existing code already
  says so and uses S3/S4 instead. That decision is correct and is preserved.

**Category 2 — per-arm accounting** (each arm's own books balance):

* P1 descriptor + index + target == total requests.
* P2 `dram_reads == total - l1_hits - l1_joins + pte_fetches - l2_hits - l2_joins`.
* P3 `l1_reads == l1_hits + l1_misses`, `l2_reads == l2_hits + l2_misses`, and the same for both
  TLBs — already enforced in `src/bulk.cpp` and `src/system.cpp`.
* P4 `lines_issued + dedup_merged == elements` — fanout conservation.
* P5 `peak_slots <= R`; `win_occ_max <= W`; `win_total_max <= 2W`.

**Category 3 — cross-arm comparison** (workload identity, not traffic identity):

* C1 the arms start from the same memory image (`image_hash`).
* C2 the index array costs exactly `ceil(count · index_bytes / 64)` requests in B0 and zero in B2.
* C3 the descriptor costs exactly one request in B0/B2 and zero in B1/B3.
* C4 **the non-target request volume** is related across arms by C2/C3 — never the total.
* C5 physical request counts and DRAM traffic **may differ** between arms, and between B0 and its
  oracle, exactly when deduplication or coalescing differs.

C5 is the invariant the adversarial suite needed and did not have. The measured evidence that it is
the right rule: DRAM traffic varies by up to 1 024× between arms on HOT (`dram` 4 for A versus
1 031 for B0) with identical addresses and identical values — deduplication is doing its job, and
any identity that forces equal traffic across arms is wrong.

### 2.2 The corrected invariants, and what they replace

| # | Corrected invariant | Replaces | Strength |
|---|---|---|---|
| P2′ | `dram_reads == total - l1_hits - l1_joins + pte - l2_hits - l2_joins` | original P2 | strictly stronger: the original is P2′ plus an unstated `l1_hits + l1_joins == 0` |
| C4a | `(b0.total - b0.target) == (b2.total - b2.target) + expect_idx` | `b0.total == b2.total + expect_idx` | same claim about the index array, with the target term removed |
| C4b | `(b0.total - b0.target) == (b1.total - b1.target) + 1` | `b0.total == b1.total + 1` | same claim about the descriptor |

Nothing is deleted and nothing is weakened. The originals still run — in full, with their original
message — whenever their assumption holds (`l1_hits + l1_joins == 0`, and equal target counts
respectively). When it does not hold, the run emits a `~` note naming the identity, the reason and
the measured offset, so a skipped check is visible rather than silently absent.

### 2.3 A third defect found while auditing: the `page_dir` invariant is tautological

`src/bulk.cpp` (conservation block, end of `validate`):

```cpp
if (b.dir_collisions == 0)
  check("resident pages <= page_dir", b.dir_occ_max <= cfg_.page_dir ? 0 : 1, 0);
```

The directory is direct-mapped over `page_dir` slots (`src/bulk.cpp:855`, index `vpn % page_dir`),
and `n_pages` is incremented only on a *new* vpn at a slot. Zero collisions therefore implies
`n_pages <= page_dir` by construction, so the check can never fail. It is vacuous.

It is also the only place `page_dir` is enforced, which means **`page_dir` does not gate anything**.
Measured: N = 16 384, P = 2 048, M8 cycles are **857 219** for `page_dir` = 512, 2 048 **and** 1 —
byte-identical, while `peak pages resident` is reported as 8 190, 1 317× the configured capacity.
For M6 and M7 the same holds at P = 2 048, 8 192 and 16 384. Consequently the scaling rule's
`page_dir = N/32` line is inert, and the M6 config comment that "`page_dir` bounds distinct pages in
a window" overstates the implementation. No published number depends on it (the knob never moved a
cycle), but any claim about page-directory capacity is unsupported until it is enforced.

### 2.4 Implementation

`src/sim.hpp` (`DiagResult`): added `std::vector<std::string> notes` — "not applicable" records,
never a pass and never a failure.
`src/ab.cpp` (`run_index_oracle_diag`): the three original identities are retained verbatim, gated
on their assumption; P2′, C4a and C4b are added as unconditional failures
(`ab.cpp:840`, `833`, `889`). The row-2 diag table gained `l1hit`, `l2hit` and `pte` columns so the
arithmetic is auditable from the printout, and notes print as `~ ...` after the violations.

Effect: the 21 adversarial diag runs move from **9 OK / 12 VIOLATIONS** to **21 OK / 0 VIOLATIONS**
with 34 notes, 0 cycle changes, and no change to any baseline test.

---

## 3. Part 3 — attributing the large-N regression

### 3.1 The published facts to explain

| N (W1) | M7 | M8 | M8 vs M7 | walks M7 → M8 |
|---:|---:|---:|---:|---|
| 4 096 | 36 141 | 35 881 | −0.72 % | — |
| 16 384 | 156 912 | 156 419 | −0.31 % | 533 → 544 |
| 65 536 | 2 626 525 | 2 920 204 | **+11.2 %** | 17 348 → 19 324 |
| 262 144 | 25 327 321 | 28 538 534 | **+12.7 %** | 168 794 → 190 195 |

### 3.2 The identity that does the explaining

A walk costs `levels × walk_latency_per_level = 2 × 150 = 300` cycles (`src/memory.cpp:238`,
`ptw_tick`), and there are `walkers = 2`. At steady state the translation system therefore retires
**one walk per 150 cycles**, and once it is saturated the whole run is bounded by that rate:

| Point | cycles | walks | walks × 150 | ratio |
|---|---:|---:|---:|---:|
| N = 65 536, M6 | 4 117 296 | 16 661 | 2 499 150 | 1.647 |
| N = 65 536, M7 | 2 626 525 | 17 348 | 2 602 200 | **1.009** |
| N = 65 536, M8 | 2 920 204 | 19 324 | 2 898 600 | **1.007** |
| N = 262 144, M7 | 25 327 321 | 168 794 | 25 319 100 | **1.000** |
| N = 262 144, M8 | 28 538 534 | 190 195 | 28 529 250 | **1.000** |

The walkers are measured at 99.1 % (M7) and 99.3 % (M8) occupancy at N = 65 536
(`walker_busy` 5 204 400 and 5 797 200 against `2 × cycles`). So at both published large-N points
**the entire run is walk-throughput-bound**, and the M7↔M8 difference is a difference in *walk
count*:

* N = 262 144: gap 3 211 213 cycles; Δwalks 21 401 × 150 = **3 210 150** (99.97 %).
* N = 65 536: gap 293 679 cycles; Δwalks 1 976 × 150 = **296 400** (100.9 %).

### 3.3 Ruling out each candidate with counters

| Candidate | Prediction if it were the cause | Measured | Verdict |
|---|---|---|---|
| **Index acquisition throughput** | `idxwait` / `bufStall` grow with N and dominate | at N = 65 536 M8: `wait` 23 567 = **0.8 %** of cycles, `bufStall` **0**; at every ring depth 0.5–1.2 %, `bufStall` 0 | **ruled out** |
| **Buffer reuse / backpressure** | `bufStall > 0`, `peak slots < R` | `bufStall` **0** in every M8 run of the scaling sweep; peak residency **128 = R** at N = 262 144 | **ruled out** |
| **Target scheduling (M7 cohorts)** | `sched_idle > 0` or `defer` grows | `schedIdle` **0**, `defer` **0** in every diag run at N = 65 536 | **ruled out** |
| **Index-ring capacity** | reducing capacity with nothing else fixed reproduces the regression; increasing it removes it | §4-D and §4-E: b16 → b128 moves N = 65 536 from 4 234 734 to 2 628 828 with only `idx_buf_lines` changed | **confirmed** |
| **Translation working-set pressure** | page count alone reproduces the regression | §4-C: pages 512 → 2 048 costs +6.2 %, but 8 192 → +0.6 % and 16 384 → +0.0 %; all arms converge on the same wall | **secondary / conditional**, not the cause |

### 3.4 The mechanism, in two regimes

The ring holds `2 × idx_buf_lines` index lines (`IDX_SLOTS = 256`, two tiles,
`src/bulk.cpp:614`), i.e. 128 lines at the default 64. The index array is `N/16` lines, so ring
coverage falls from 12.5 % at N = 16 384 to 3.1 % at 65 536 and 0.8 % at 262 144. A ring that cannot
front-load the array spreads index acquisition across the whole run, and that changes *which
addresses are being translated at the same time*:

* **Zero duplication (published W1).** Index pages are interleaved with target pages throughout the
  run instead of being consumed in an early burst, so index translations are evicted and re-walked.
  Evidence: with the target set held to 512 translation-clean pages (§4-A), A performs exactly 512
  walks at every N while M8's walks grow 544 → 1 921 as the index array grows 1 024 → 16 384 lines.
  The target set is unchanged, so the growth is index-side.
* **Heavy duplication (footprint fixed, N growing).** The ring also changes window composition, so
  deduplication degrades. M8 generates elements out of index order (`ooo` 156 864 at N = 262 144,
  §4-A), a window therefore covers fewer complete lines, and M8 issues **3.9 % more target
  requests** than M7 (256 818 vs 247 233) costing 12 036 more DRAM reads — which, at 96.5 % bank
  utilisation, accounts for ~92 % of that experiment's 163 234-cycle gap.

Both are capacity-mediated; only the transmission differs. This is why "the regression is
translation" and "the regression is deduplication" are both wrong as causes and both right as
mechanisms.

---

## 4. Part 4 — controlled isolation experiments

Every experiment varies one dimension. `R = 128`, the hierarchy and the engine's internal bounds
are untouched; the only swept quantities are the ones named. Block bases were moved where the
address span would otherwise alias the index array (P = 16 384 spans 64 MiB, so the index base
moves from 6/8 MiB to 96/128 MiB); this is measurement-enabling and is reported, not tuned.
Realized address sets were measured with `experiment=none`, not assumed.

### 4.1 A — N grows; target footprint and page count fixed

`Lambda = 16 384`, `P = 512`, `page_dir = 512`, `kappa = 0`. Target lines 16 384 and pages 512 are
constant; the index array grows 1 024 → 16 384 lines. Realized duplicate line rate rises with N
(0 → 0.9375, verified), which is unavoidable: with a permutation generator, distinct lines =
min(Λ, N).

| N | idx lines | A | M6 | M7 | M8 | M8/M7 | walks M6/M7/M8 | target req M7→M8 | DRAM M7→M8 |
|---:|---:|---:|---:|---:|---:|---:|---|---|---|
| 16 384 | 1 024 | 270 753 | 233 475 | 156 912 | **156 419** | 0.997 | 529 / 533 / 544 | 16 384 → 16 384 | 17 445 → 17 446 |
| 32 768 | 2 048 | 451 411 | 343 000 | 270 378 | 282 597 | 1.045 | 561 / 565 / 630 | 31 781 → 32 503 | 31 418 → 32 271 |
| 65 536 | 4 096 | 815 070 | 565 531 | 499 167 | 530 571 | 1.063 | 625 / 651 / 816 | 62 557 → 64 578 | 59 311 → 61 892 |
| 131 072 | 8 192 | 1 538 996 | 1 016 816 | 958 952 | 1 036 667 | 1.081 | 792 / 927 / 1 191 | 124 141 → 128 858 | 115 120 → 121 405 |
| 262 144 | 16 384 | 2 993 532 | 1 952 649 | 1 872 684 | 2 035 918 | 1.087 | 1 254 / 1 568 / 1 921 | 247 233 → 256 818 | 227 855 → 239 891 |

**Reading.** With translation pressure and footprint frozen, M8 still loses, monotonically, as the
index array grows — 0 % → 4.5 % → 6.3 % → 8.1 % → 8.7 %. The regression therefore does **not**
require the footprint or the page set to grow. At N = 262 144 the diag view shows `wait` 130 982
(6.4 %) and the only non-zero `bufStall` of any experiment (9 533), i.e. the ring becomes a real
constraint on address generation in this regime. **Caveat:** duplicate rate rises with N, so the
tail of this series also carries a dedup-efficiency contribution (§3.4); the clean zero-duplication
statement comes from D and E.

### 4.2 B — target footprint grows; N fixed

`N = 16 384`, `P = Lambda/32`.

| Λ | P | realized lines / pages | A | M6 | M7 | M8 | M8/M7 |
|---:|---:|---|---:|---:|---:|---:|---:|
| 1 024 | 32 | 1 024 / 32 | 21 390 | 19 370 | 19 939 | **18 283** | 0.917 |
| 4 096 | 128 | 4 096 / 128 | 75 917 | 49 975 | 44 855 | **43 678** | 0.974 |
| 16 384 | 512 | 16 384 / 512 | 270 753 | 233 475 | 156 912 | **156 419** | 0.997 |
| 32 768 | 1 024 | **16 384 / 1 024** | 1 287 038 | 721 451 | 455 763 | 467 004 | 1.025 |

**Reading.** Growing the *distinct target line count* at fixed N and fixed index size does **not**
produce the regression — M8 wins by 8.3 % at 1 024 lines and breaks even at 16 384. It is the
*opposite* direction from the scaling failure, and consistent with the adversarial suite: M8's
advantage grows with distinct targets per index line.

**Representability limit, measured not assumed.** The last row is not a footprint-growth point:
with `rho_dup = 0` the generator cycles a permutation of `Λ` entries and draws only `N` of them, so
the realized line count saturates at `N = 16 384`. `experiment=none` reports
`lines=16384 pages=1024` for `Lambda = 32768`. **A target footprint larger than N cannot be
represented** with this generator; only the page count changes, which is experiment C.

### 4.3 C — page count grows; N and target-line count fixed

`N = 16 384`, `Lambda = 16 384`, `kappa = 0` → 16 384 lines and 16 384 elements, duplicate rate 0.
`page_dir` was run twice per point (fixed at 512, and scaled to P) to separate directory capacity
from TLB capacity; as §2.3 predicts, **both series are byte-identical**, so one is shown.

| P | lines/page | A | M6 | M7 | M8 | M8/M7 | walks M6/M7/M8 | bank util M8 |
|---:|---:|---:|---:|---:|---:|---:|---|---:|
| 512 | 32 | 270 753 | 233 475 | 156 912 | **156 419** | 0.997 | 529 / 533 / 544 | 90.4 % |
| 2 048 | 8 | 1 908 638 | 1 160 292 | 807 159 | 857 219 | **1.062** | 4 779 / 5 326 / 5 669 | 21.1 % |
| 8 192 | 2 | 2 380 239 | 1 989 946 | 1 989 946 | 2 001 019 | 1.006 | 13 244 / 13 244 / 13 321 | 11.4 % |
| 16 384 | 1 | 2 457 664 | 2 463 347 | 2 463 347 | 2 463 318 | 1.000 | 16 401 / 16 401 / 16 402 | 9.5 % |

**Reading.** Page dispersion alone does produce a regression, but a bounded and non-monotonic one:
+6.2 % at 2 048 pages, then +0.6 %, then +0.0 %. The mechanism is again walks: at P = 2 048 the
50 060-cycle gap is 343 × 150 = 51 450 (100 %); at P = 8 192 and 16 384 every arm converges on one
walk per page with no reuse at all (13 244 and 16 401 walks for 8 193 and 16 385 pages), translation
serialises everything, and ordering stops mattering. The accelerator's advantage over SIMT is also
non-monotone in page count: A/M8 = 1.73× at P = 512, **2.23× at P = 2 048**, 1.19× at P = 8 192, and
0.998× at P = 16 384 — the one point in this audit where the engine loses to SIMT
(A 2 457 664 vs M8 2 463 318).

### 4.4 D — index-array size independent of target footprint

The element count sets the index size (`N · index_bytes / 64`), and with zero duplication the
footprint is also tied to N, so the only lever that changes index size alone is **index width**:
`bulk_wl = E1` stores 8 B indices instead of 4 B. Target lines (16 384), pages (512) and N are
identical; the index array doubles 1 024 → 2 048 lines.

| variant | idx lines | A | M6 | M7 | M8 | M8/M7 | walks M7→M8 |
|---|---:|---:|---:|---:|---:|---:|---|
| 4 B (`E0`) | 1 024 | 270 753 | 233 475 | 156 912 | **156 419** | 0.997 | 533 → 544 |
| 8 B (`E1`) | 2 048 | 270 753 | 266 867 | 192 480 | 202 755 | **1.053** | 564 → 617 |

**Reading.** Doubling the index array with footprint, page set, N and duplicate rate all fixed costs
M8 5.6 points relative to M7. This is the cleanest single-factor demonstration that index-array
size — hence ring coverage — is the variable that matters. It also reproduces the M8 report's E1
figure (202 755) exactly, which is an independent check on the harness.

### 4.5 E — ring depth at the published large-N point (the decisive test)

`N = 65 536`, `Lambda = 65 536`, `P = 2 048` — the published W1 point, duplicate rate 0. Only
`idx_buf_lines` (or `idx_mode`) changes.

| config | cycles | vs M7 | walks | `wait` | `wait%` | `bufStall` | ready lines | `resMax` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| M6 (`idx_mode=0`, no sched) | 4 117 296 | +56.8 % | 16 661 | — | — | 0 | — | — |
| M7 (`idx_mode=0`) | 2 626 525 | — | 17 348 | 2 999 | 0.1 % | 0 | — | 64 |
| M8, 16 lines/tile | 4 234 734 | +61.2 % | 28 150 | 51 174 | 1.2 % | 0 | 32 | 32 |
| M8, 32 lines/tile | 3 211 954 | +22.3 % | 21 288 | — | — | 0 | 64 | 64 |
| M8, 64 lines/tile (default) | 2 920 204 | **+11.2 %** | 19 324 | 23 567 | 0.8 % | 0 | 128 | 64 |
| M8, 128 lines/tile | 2 628 828 | **+0.09 %** | 17 366 | 12 836 | 0.5 % | 0 | 256 | 64 |
| M8, `idx_mode=1`, 64 | 2 952 461 | +12.4 % | 19 544 | — | — | 0 | 128 | 64 |
| M8, `idx_mode=3`, 64 | 2 920 204 | +11.2 % | 19 324 | — | — | 0 | 128 | 64 |

**Reading.** Walk count is monotone in ring depth (28 150 → 21 288 → 19 324 → 17 366) and the
cycle count follows it at 150 cycles per walk almost exactly. At 128 lines/tile M8 is within
2 303 cycles (+0.09 %) of M7 and 18 walks of its walk count. `bufStall` stays 0 throughout, so this
is not a slot problem; the phase table shows the whole difference sits in the issue span
(4 229 652 → 2 915 467 → 2 624 065 cycles) with startup nearly unchanged (4 104–4 435). Fine-grained readiness
is worth 1.1 % (`idx_mode=1` 2 952 461 → `idx_mode=2` 2 920 204); the dedicated port (`idx_mode=3`)
is a **no-op** at this size — bit-identical counters to `idx_mode=2`.

Two implementation notes that matter for interpreting this table:

* `idx_buf_lines` is clamped at 128 per tile (`src/bulk.cpp:614`, `IDX_SLOTS/2` = 128), not at 64 as
  the comment in `src/sim.hpp:163` says. 128 per tile is therefore legal and gives 256 live lines.
* `bulk_idx_buf_lines` **under-reports** for values above 64: `src/bulk.cpp:1378` clamps the
  reported value to 64. Any capacity claim must come from `ready` (which correctly shows 256 at
  128/tile), not from that counter.

---

## 5. Part 5 — conclusions

**Q1. Are the 14 adversarial failures genuine defects or invalid diagnostics?**
Twelve runs (34 messages) failed, not fourteen. All are **invalid diagnostics**: two root causes,
both false positives, both reproduced on the SIMT arm or explained exactly by a target-count
difference. No semantic, fanout, residency or traffic-accounting defect exists in any arm: every
element completed once, at the right address, with the right value, in all 21 runs. The M8
adversarial report's headline numbers stand; its §4 counts and its per-arm attribution of the DRAM
failure are wrong and are corrected here.

**Q2. Is M8's large-N regression a capacity, translation, or both?**
**Capacity** is the cause; **translation** is how it is paid for at zero duplication. Concretely:
the ring holds 128 index lines, the array is `N/16` lines, and at 65 536 / 262 144 the run is 100 %
walk-throughput-bound, so +11.2 % / +12.7 % is exactly `150 × Δwalks`. Holding footprint and pages
fixed (§4-A) the loss persists and grows with index size; holding N fixed and growing pages (§4-C)
gives only +0.6 % to +6.2 % and vanishes at full dispersion; doubling the index array alone (§4-D)
costs 5.6 points; and raising the ring alone (§4-E) removes the regression. Acquisition throughput,
buffer backpressure and target scheduling are each ruled out by counters (`wait` ≤ 1.2 %,
`bufStall` 0, `schedIdle` 0). This report therefore **rejects** the earlier framing that the
regression is a translation working-set problem: translation is the meter, not the cause.

**Q3. Does M8 provide value beyond M7 after accounting for its resource cost?**
At the shipped 128 live lines: **yes where starvation exists, no at large N.**
Value: (a) it converts M7's catastrophic buffer starvation into steady demand-paced acquisition —
W0 at N = 262 144 has M7 `bufStall` 528 641 and M8 structurally 0, a 1.0 % win; (b) it removes the
index/target slot fight — M7's `slotStall` is 26 554 at N = 4 096 and 71 769 at 16 384, M8's 0 —
worth 0.7–3.0 %; (c) it never loses to M6. Cost: at N ≥ 65 536 it loses to M7 (11.2 %, 12.7 %).
With 256 live lines it reaches parity at 65 536 (+0.09 %) but costs 16 KiB of SRAM
(`2 × 128 × 64 B`) instead of 8 KiB, which is real capacity that must be declared against the same
budget the window already draws on. Net: M8's measured value is **insurance against starvation, not
a throughput gain at scale**.

**Q4. Should M8 remain the default, become optional, or be redesigned?**
**Keep it as the default, but redesign its capacity policy before any large-N claim is made.**
`idx_mode=2` is strictly better than `idx_mode=1` (−1.1 %) and never worse than M6, so it should not
be reverted to `idx_mode=0`; and it is the only arm that bounds buffer starvation. But the shipped
128 live lines are *descriptor-sized in neither direction* — they cover 12.5 % of the index array at
16 K and 0.8 % at 262 K — so the choice between 64 and 128 lines per tile, and any adaptive policy,
must be justified by a declared SRAM budget rather than left at a value that happens to be optimal
at one size. `idx_mode=3`'s dedicated port should not be adopted: it is bit-identical to mode 2 at
65 536, so it buys nothing measurable at a real cost.

**Q5. What evidence is still required before starting M9?**
1. **The 262 144 confirmation of §4-E.** Parity was shown at 65 536 only; running `idx_buf_lines=128`
   at 262 144 is the missing half of the capacity claim.
2. **Per-page-class walk attribution.** The simulator reports aggregate walks. "The extra walks are
   on index pages" is inferred from §4-A (target set fixed at 512 translation-clean pages while M8's
   walks grow) — strong, but indirect. A walk counter tagged by request class would make it direct.
3. **An enforced `page_dir`.** Until §2.3 is fixed, no page-directory result can be interpreted, and
   the scaling rule's `page_dir = N/32` line is decoration.
4. **A generator knob decoupling index size from element count** (e.g. `index_array_lines`). §4-D
   only reaches a 2× index change through index width; every larger decoupling is structurally
   impossible today, which is why §4-A carries a duplicate-rate caveat.
5. **A declared SRAM/latency cost model for the ring**, so that 8 KiB versus 16 KiB can be compared
   against the window's own budget instead of being free.
6. **An adversarial case where M8's dedup advantage actually pays**, since §4-A shows the ring can
   *cost* dedup (−3.9 % target requests) as well as gain it.

---

## 6. Limitations

* **One seed, one element width per experiment.** `seed = 1` throughout; §4-D is the only index-width
  point, and it changes element packing as well as index width (8 B indices mean 8 elements per 64 B
  line), so it isolates index size but not element layout.
* **§4-A is confounded by duplicate rate.** With a permutation generator at fixed footprint, growing
  N necessarily recycles lines (0 → 93.75 %). Its tail therefore mixes capacity and dedup effects;
  the clean zero-duplication evidence is §4-D and §4-E.
* **§4-C's high-P end is translation-saturated for every arm**, so it bounds the regression's
  magnitude rather than measuring M8's ordering in isolation; at P = 16 384 all three engine arms are
  within 29 cycles of each other and SIMT is 5 654 cycles ahead of M8.
* **Block bases were moved** for §4-C and §4-E to keep the index array out of the data VA span.
  Counts (pages, lines, requests) are unaffected; VA page numbers differ from the published runs.
* **`page_dir` is inert** (§2.3), so no experiment here can separate page-directory capacity from
  TLB capacity — they are the same experiment until it is enforced.
* **The diagnostic change is a classification change only**, verified by a 105-row cycle diff, but it
  does not add any new instrumentation: the corrected invariants are expressed entirely in counters
  that already existed.
* **No M9 mechanism was designed or implemented**, no ISA or RTL file was touched, and no published
  report was edited; `m8-adversarial-workloads.md` and `m8-scaling-experiment.md` stand as written,
  with the two corrections to the former recorded here.

---

## 7. Reproduction

```
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests        # 3979 checks, 0 failures (54 groups)

# §1: one adversarial diag run (repeat per workload x arm; knobs in the table below)
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=diag \
  --set N=16384 --set seed=1 --set Lambda=2 --set P=1 --set batch_window=8192 \
  --set page_sched=1 --set idx_mode=2 --set bulk_max_elems=262144

# §4-E: the ring sweep at the published large-N point
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=ab \
  --set N=65536 --set Lambda=65536 --set P=2048 --set page_dir=2048 --set seed=1 \
  --set batch_window=8192 --set bulk_max_elems=262144 --set max_cycles=200000000 \
  --set bulk_index_base=33554432 --set bulk_desc_line=393216 \
  --set page_sched=1 --set idx_mode=2 --set idx_buf_lines=128
```

Adversarial knobs: HOT `Lambda=2,P=1`; DUP `Lambda=256,P=4`; PGLO `Lambda=1024,P=16`;
PSCR `Lambda=16384,P=2048`; ROWLO `Lambda=16384,P=256,kappa=1`; STRID `Lambda=1024,P=64,kappa=1`;
PERM `Lambda=16384,P=512`. Experiment knobs: A `Lambda=16384,P=512` for N ∈
{16384, 32768, 65536, 131072, 262144}; B `N=16384, P=Lambda/32`; C `N=16384, Lambda=16384` for
P ∈ {512, 2048, 8192, 16384}; D `elem_bytes=8, bulk_wl=E1`; E as above.
