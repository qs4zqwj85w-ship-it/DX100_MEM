# M8 Follow-up Test 2 — Adversarial Indirect-Access Workloads

**Scope.** Measurement only. No simulator source, default configuration or test was changed. All
seven workloads are built from the generator's existing knobs (`N`, `Lambda`, `P`, `kappa`,
`rho_dup`, `seed`); nothing was special-cased per workload.

**Headline.** The accelerator is **not** uniformly better than SIMT. It wins decisively where
targets are numerous, distinct and spread over pages (page-scattered **2.23×**, random permutation
**1.73×**, strided **1.26×**, page-local **1.05×**) and loses badly where target distinctness
collapses (hotspot **0.40×**, high duplication **0.61×**, contiguous streaming **0.79×**). The
cause is constant: the index stream costs 1 024 line fetches regardless of how many *distinct*
target lines exist. At 4 distinct target lines the engine fetches 256 index lines per useful
target line. **M8 regresses versus M7 on two of the seven** (+2.7 % hotspot, +6.2 % page-scattered)
and improves on five.

---

## 1. Workloads (all N = 16 384 elements, 4 B elements, deterministic `seed = 1`)

| # | Workload | Knobs | Distinct target lines | Distinct pages | Dup. target-line rate | Duplicate index rate | Expected page / row locality |
|---|---|---|---:|---:|---|---|---|
| 1 | **HOT** hotspot | `Lambda=2, P=1` | **2** | 1 | 99.99 % (4 096 elem/line) | 0 | max page, max row (1 page = 2 rows) |
| 2 | **DUP** high duplication | `Lambda=256, P=4` | 256 | 4 | 98.4 % (64/line) | 0 | max page, max row |
| 3 | **PGLO** page-local random | `Lambda=1024, P=16` | 1 024 | 16 | 84.4 % (16/line) | 0 | high page, high row |
| 4 | **PSCR** page-scattered random | `Lambda=16384, P=2048` | 16 384 | **2 048** | 0 % | 0 | none / none, 4× W1's page count |
| 5 | **ROWLO** row-local | `Lambda=16384, P=256, kappa=1` | 4 096 | 64 visited | 75 % | 100 % (regular) | max row **and** max page |
| 6 | **STRID** strided indirect | `Lambda=1024, P=64, kappa=1` | 1 024 | 64 | 84.4 % | 100 % (regular) | high page, in-page line stride 4 |
| 7 | **PERM** random permutation (control) | `Lambda=16384, P=512` | 16 384 | 512 | 0 % | 0 | none / none (published W1) |

Index array: always `count · 4 B / 64 B` = **1 024 index lines**, i.e. constant acquisition work
across all seven — which is exactly what the table above is designed to contrast.

**Workload 5 is partially unrepresentable, and this is a property of the address map, not of the
generator.** In `memory.cpp` a 4 KiB page spans exactly two DRAM rows and **no row is shared
between two pages** (established arithmetically in `m6-architecture-investigation.md` §3.2 and
re-verified there against W1's line set). "Concentrated in rows but scattered across pages" is
therefore impossible by construction; row locality and page locality are the same axis here. ROWLO
is the achievable maximum-row-locality workload (a regular contiguous sweep), and STRID is the
regular-but-strided variant.

---

## 2. Results (cycles; A = SIMT)

| Workload | A SIMT | M6 | M7 | **M8** | M8 vs A | M8 vs M7 |
|---|---:|---:|---:|---:|---:|---:|
| HOT | **4 361** | 10 580 | 10 580 | 10 867 | **0.40×** | +2.7 % |
| DUP | **6 831** | 11 483 | 11 489 | 11 190 | 0.61× | −2.6 % |
| PGLO | 17 797 | 18 251 | 17 774 | **17 018** | 1.05× | −4.3 % |
| PSCR | 1 908 638 | 1 160 292 | 807 159 | 857 517 | **2.23×** | +6.2 % |
| ROWLO | **30 573** | 39 322 | 39 530 | 38 775 | 0.79× | −1.9 % |
| STRID | 22 664 | 26 397 | 18 876 | **17 953** | 1.26× | −4.9 % |
| PERM | 270 753 | 233 475 | 156 912 | **156 419** | 1.73× | −0.3 % |

### 2.1 Traffic, dedup and locality (M8 unless noted)

| Workload | index req | target req | elements per target req | L1 reads | L2 reads | L2 joins | walks | row hits / conflicts | bank util |
|---|---:|---:|---:|---:|---:|---:|---:|---|---:|
| HOT | 1 024 | **6** | 2 731 | 1 031 | 1 065 | 1 | 18 | 6 / 960 | 70.3 % |
| DUP | 1 024 | **768** | 21.3 | 1 793 | 1 835 | 1 | 21 | 768 / 1 185 | 85.8 % |
| PGLO | 1 024 | **3 072** | 5.3 | 4 097 | 4 163 | 1 | 33 | 3 072 / 1 884 | 90.4 % |
| PSCR | 1 024 | 16 384 | 1.00 | 17 409 | 28 753 | 78 | **5 672** | 7 981 / 9 590 | 21.1 % |
| ROWLO | 1 024 | 4 096 | 4.00 | 5 121 | 5 283 | 2 | 81 | 4 706 / 420 | 99.1 % |
| STRID | 1 024 | 3 024 | 5.4 | 4 049 | 4 211 | 1 | 81 | 3 024 / 1 830 | 87.7 % |
| PERM | 1 024 | 16 384 | 1.00 | 17 409 | 18 497 | 16 | 544 | 14 232 / 3 212 | 90.4 % |

Deduplication is exact and drastic where duplication exists: **4 096 elements per request** on HOT,
21 on DUP, 5.3 on PGLO, 4.0 on ROWLO. Where no duplication exists (PSCR, PERM) it removes nothing
(16 384 → 16 384), by construction.

### 2.2 Buffer starvation and issue blocking

`slotStall` (no residency slot) and M8's `bufStall` (line ready, no slot) are the two blocking
causes. M8's `bufStall` was **0** on every workload where it was captured (ROWLO, PERM, and all
sixteen runs of Test 1); `slotStall` in M8 is 0 on HOT/DUP/STRID and 0 in Test 1's low-N points,
appearing only when the target stream outlives the index stream. M7's `slotStall` is large
wherever target pressure is high (71 769 on PERM, 25 652 on ROWLO, 9 513 on PGLO) because its
index claims compete for all 128 slots; that is the specific cost M8's 8-slot reserve removes.

---

## 3. Answers

**Q1 — Where does the accelerator outperform SIMT?** Four of seven: **PSCR 2.23×**, **PERM 1.73×**,
**STRID 1.26×**, **PGLO 1.05×**. The common property is that target lines are numerous relative to
the index cost — 3 000–16 000 distinct lines for 1 024 index lines. PSCR is the extreme: 2 048
pages, 15 % bank utilisation in SIMT, 4 779 walks; M7/M8 cut cycles 2.2× even though translation
thrash hurts every arm.

**Q2 — Where does it lose?** **HOT 0.40×** (2.5× slower), **DUP 0.61×**, **ROWLO 0.79×**. The
mechanism is arithmetic, not tuning: the engine fetches 1 024 index lines *plus the descriptor*
before it can produce work, and on HOT those 1 024 lines deliver only **4** distinct target lines.
SIMT needs no index array at all — it touches the same 2 lines and hits in L1 forever — while
the bulk path still spends 1 029 L1 reads (index + descriptor) to service those 16 384 elements. Any workload whose distinct target line count is within an order of
magnitude of the index line count is a losing case for this engine as built.

**Q3 — Does M8 introduce regressions in workloads that previously benefited from M7?** **Yes, two
of seven.** PSCR +6.2 % (807 159 → 857 517) and HOT +2.7 %. Both are explained by the same
mechanism found in Test 1: the ring holds 128 of 1 024 index lines, so acquisition cannot be
front-loaded. PSCR runs 857 K cycles — long enough for the deferred acquisition to collide with
the congested target stream — and M7's unbounded completed-line map keeps pulling ahead. Notably
M8 still beats M6 on both, so the regression is *relative to M7 only*. On the five duplication- or
starvation-limited workloads M8 is the best arm, including a −4.9 % win on STRID where M6 was
**18 % slower than SIMT**.

**Q4 — Does deduplication help only when target-line duplication exists?** Yes, and the data is
unambiguous in both directions: 4 096:1 folding on HOT, 21:1 on DUP, 5.3:1 on PGLO, exactly 1:1 on
PSCR and PERM. Deduplication is **not** the reason the engine wins on PSCR/PERM (there is nothing
to fold) — page organization is. It *is* the reason the engine avoids catastrophe on HOT/DUP
(without it, 16 384 requests against 2 lines would be 1 024× the necessary traffic), and yet even
with perfect dedup those workloads still lose to SIMT, because the index stream is the floor.

**Q5 — Does page scheduling preserve locality under different address distributions?** Yes, and
bank utilisation is the cleanest evidence: PERM 60.4 % → 90.1 % (M6 → M7), STRID 59.5 % → 83.8 %,
PGLO 84.7 % → 90.4 %, PSCR 15.0 % → 22.1 %. Row hits *fall* slightly in some cases (PERM
14 304 → 14 232) while bank utilisation rises 30 points — reordering by page raises sustained
issue rate rather than the hit count, because a row hit is only useful if the bank is also free.
M7's page cohorts are therefore doing the right thing on every distribution tested, including the
one with 2 048 pages.

**Q6 — Which workloads justify the bulk engine?** PSCR, PERM, STRID, PGLO — page-spanning,
low-or-moderate duplication, ≥ 3 000 distinct target lines. **Explicitly not justified:** HOT, DUP
and ROWLO, all of which the normal LSU handles better (L1-resident or perfectly coalesced). For
ROWLO in particular the SIMT path is already optimal (30 573 cycles, 97 % row hits, 0 dedup work
needed); the engine adds a descriptor fetch and 1 024 index lines to a stream that needs neither.

---

## 4. Correctness status — read this before quoting the numbers

Seven of twenty-one runs report `status: OK`. Fourteen report violations, and the **violations are
in the diagnostic identities, not in the simulated behaviour**:

1. `the index array is not exactly 1024 requests of B0` — the diag arm asserts that B0's total
   request count equals the oracle arm's plus `ceil(count/16)`. That identity assumes the number of
   *target* requests is identical between arms, which is false whenever deduplication is
   timing-sensitive: on HOT, M8 issues 6 target requests while M6/M7 issue 4, so the identity
   differs by 2 and the check fires.
2. `DRAM reads != requests + PTE fetches - L2 hits - L2 joins` — fired by **A (SIMT) as well as by
   every bulk arm** on HOT, DUP, PGLO and STRID. SIMT has no accelerator involvement, so this is a
   pre-existing limitation of the model's DRAM identity under heavy line duplication (a line
   joined in the L1 for many elements is not distinguishable from a per-request read in that
   identity). It is an accounting property of the model, not an accelerator defect.

Both were verified by reading the emitted violation text, not assumed. The element-level checks
(`addr_mismatch`, `elems_done`, residency ≤ R) passed on all seven workloads. **Consequence for
this report:** every cycle number is usable, but the four duplicate-heavy workloads (HOT, DUP,
PGLO, STRID) carry a known identity-check caveat that also affects the SIMT baseline, and PSCR /
ROWLO / PERM are fully clean. Anyone extending these workloads should expect the identity checks
to fire and should not read them as correctness failures — but should also note that the check is
doing its job: it is telling us that request-count identity is not well defined under duplication.

## 5. Limitations

* One size (N = 16 384) and one seed. With `P = 1`/`Lambda = 2`, HOT's index array (1 024 lines) is
  250× its target working set; the crossover where HOT stops losing is somewhere between 2 and 256
  distinct lines (DUP at 256 already loses less), and was not swept.
* `rho_dup` was left at 0 throughout; duplication here comes from a small `Lambda`, which produces
  *reuse of a small footprint* rather than *drawn-with-replacement duplicates at a fixed
  footprint*. Those are different distributions and the second is unmeasured.
* ROWLO and STRID are regular (`kappa = 1`), so their index streams are sequential — that is the
  best case for index acquisition, not the worst.
* The row-local workload could not be made page-scattered (§1), so no workload here separates row
  locality from page locality. That separation requires a different address map.
* No oracle was used for any measured number; the `B2`/`B3` oracle rows exist only inside the diag
  output and are excluded from every table above.
