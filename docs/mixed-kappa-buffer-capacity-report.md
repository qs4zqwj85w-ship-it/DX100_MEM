# Mixed-workload κ sweep — buffer capacity comparison

**Question.** Should the accelerator run 2 × 8K, 2 × 16K or 2 × 32K buffers across
different regular/irregular mixtures?

**Verdict.** No capacity wins everywhere.

* **2 × 32K is not universally better.** It is the best configuration only where
  the *bulk stream itself* has line reuse inside a window: irregular-heavy and
  mid mixtures at N ≥ 32,768 (up to **−46.2 %** cycles vs 2 × 8K), and at
  N = 16,384 for κ = 0.25 (−15.0 %).
* **It is neutral** in every small cell (N = 16,384, κ ≥ 0.5; N = 32,768,
  κ = 0.75 — differences of 0.0 % to 0.25 %, several seeds exactly tied).
* **It is worse** in exactly one regime: **κ = 0.75 at N = 65,536, +7.50 %**
  (+18,378 cycles). There the irregular set is 16,420 elements spread over
  ~16,420 distinct lines — one element per line, so **44 merges out of 16,420
  elements** is all there is to win — while
  a single 32K window issues that whole set as one burst that flushes the
  shared L2 and costs the SIMT stream ~2,200 of its L2 hits.
* **2 × 16K is not a compromise.** It is the *worst* of the three at
  κ = 0.25/N = 65,536 (+4.15 %, the only cell where a bigger window loses to
  8K while 32K wins by 29 %), and it gives up most of 32K's deduplication
  everywhere else.

The official M10 default stays **2 × 8K**. 2 × 32K is recommended as a
**workload-adaptive option** with one runtime condition (§8), not as a new
default. Evidence, including the timing test that refutes the "filling slows
consumption" hypothesis, is below.

---

## 1. Methodology and controls

**Configuration.** `configs/ab_controlled.cfg`, unchanged: E3 window policy,
R = 128 shared target residency, L1 256×4 B/2 B per cycle/16 MSHR, L2
16 384×8 lines, 2 DRAM banks at 14/26 rows, FR-FCFS with bank hashing, 16 KiB
dTLB with MSHRs, M7 page-cohort scheduling (`page_sched = 1`), M8 pipelined
index acquisition (`idx_mode = 2`), `pool_slots = 128`, `elem_bytes = 4`,
`bulk_max_elems = 262144`, `dram_diag = 1`. Only `batch_window` differs inside a
row. **M13 reuse is off** (`reuse_dir = 0`, the default) in every cell.

**Trace.** W1: a permutation draw over Λ = 16 384 lines (1 MiB, 32 lines/page,
P = 512 pages, stride 2 lines), 16 elements per line. `kappa` is the
per-instruction probability that an instruction is *regular*; a regular
instruction is a contiguous run of lanes, an irregular one contributes one
element to the bulk descriptor. N is the element count. **The same
(κ, N, seed) triple produces the same trace in all three capacities** — verified,
not assumed: `accesses/instrs/lines/pages/kappa_real` and the output value hash
are bit-identical across W for all 45 groups (§7).

**Arms.** `experiment = m9diag` runs three arms per cell: **A** = SIMT only (the
same trace, all classes executed by the SIMT front-end — the speedup reference),
**B** = bulk only, **C** = dispatched by class. **C is the configuration under
test.** A and B are references and are labelled as such; no hybrid number in
this report is taken from a B-only run.

**Matrix.** κ ∈ {0.25, 0.50, 0.75} × N ∈ {16 384, 32 768, 65 536} ×
W ∈ {8 192, 16 384, 32 768} × 5 paired seeds = **135 hybrid cells**, plus
**8 reference cells** at κ = 0 and κ = 1 (N ∈ {16 384, 65 536},
W ∈ {8 192, 32 768}, seed 1). **No cell was omitted.** 143 runs, 143 exit 0,
143 gate passes.

**Harness.** `build/wm/m14_matrix.sh` → `build/wm/m14_matrix.log`;
`build/wm/m14_extract.py {perf|seeds|mem|win|check}`. Window timing is not
reconstructed: the simulator records, per window, allocation, first/last
admission, seal, issue, last issue and release cycles, and `[16]` prints those
records. Every fill/consume/overlap figure below is arithmetic on two recorded
events.

**No simulator change was required.** The M9 window records and the `[1]`
timeline already carry every quantity section 5 of the brief asks for, so this
milestone adds only a harness and an extractor — no code path the published
baselines run through was touched, which is why they reproduce exactly (§7.2).
The workload issues no stores, so DRAM writes are 0 in every cell by
construction (the model has no write path for this gather).

```
./bin/dx100sim --config configs/ab_controlled.cfg --set rho_dup=0 --set elem_bytes=4 \
  --set bulk_wl=E0 --set batch_window=8192 --set bulk_max_elems=262144 \
  --set max_cycles=200000000 --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
  --set page_sched=1 --set idx_mode=2 --set experiment=m9diag --set pool_slots=128 \
  --set dram_diag=1 --set Lambda=16384 --set P=512 --set bulk_stream=0 \
  --set kappa=0.25 --set N=65536 --set seed=1
```

---

## 2. Experimental matrix

`batch_window` is the capacity of **each** of the two buffers; the
2 × B architecture is untouched, and 2 × 8K is two 8K deduplication windows, not
one unified 16K window. Model SRAM at 8 B/element plus the shared page directory
and cohorts: **147 584 B / 278 656 B / 540 800 B** (×1.00 / ×1.89 / ×3.66).

| cell | κ=0.25 | κ=0.50 | κ=0.75 |
|---|---|---|---|
| N = 16 384 | 15 runs | 15 runs | 15 runs |
| N = 32 768 | 15 runs | 15 runs | 15 runs |
| N = 65 536 | 15 runs | 15 runs | 15 runs |
| reference | κ=0 (bulk only), κ=1 (regular only): 8 runs | | |

---

## 3. Performance by κ and N

Cycles in arm C, mean ± population sd over 5 paired seeds; `Δ` is the mean of
the per-seed paired percentage differences (so it is not the ratio of the means).

### 3.1 κ = 0.25 (irregular-heavy)

| N | configuration | cycles | ± sd | vs SIMT (A) | Δ vs 2×8K | Δ vs 2×32K |
|---|---|---|---|---|---|---|
| 16 384 | 2×8K | 124 405.6 | 1 014.4 | 1.769× | — | +17.68 % |
| 16 384 | 2×16K | 105 716.4 | 820.1 | 2.081× | **−15.02 %** | 0.00 % |
| 16 384 | 2×32K | 105 716.4 | 820.1 | 2.081× | **−15.02 %** | 0.00 % |
| 32 768 | 2×8K | 214 114.2 | 1 508.2 | 1.638× | — | +44.63 % |
| 32 768 | 2×16K | 206 826.0 | 1 632.3 | 1.695× | −3.40 % | +39.70 % |
| 32 768 | 2×32K | 148 046.8 | 41.0 | 2.368× | **−30.85 %** | 0.00 % |
| 65 536 | 2×8K | 421 827.4 | 1 710.0 | 1.521× | — | +41.30 % |
| 65 536 | 2×16K | 439 337.2 | 3 927.2 | 1.461× | **+4.15 %** | +47.15 % |
| 65 536 | 2×32K | 298 579.8 | 4 110.5 | 2.149× | **−29.22 %** | 0.00 % |

SIMT-only reference (arm A, same traces): 220 019 / 350 641 / 641 673 cycles.
Bulk-only reference (arm B): 138 380 / 106 445 / 106 445; 229 102 / 196 979 /
149 114; 430 527 / 389 666 / 304 113 — the bulk engine alone prefers capacity
in every row, which is the comparison §5.2 needs.
Per-seed paired deltas at N = 65 536 are −121 855, −121 271, −121 166, −127 676,
−124 270 (32K) — 5/5 wins. At N = 65 536 the 16K arm is *worse* in 5/5 seeds
(+13 313 … +19 936).

### 3.2 κ = 0.50

| N | configuration | cycles | ± sd | vs SIMT | Δ vs 2×8K | Δ vs 2×32K |
|---|---|---|---|---|---|---|
| 16 384 | 2×8K | 93 542.4 | 286.8 | 1.805× | — | +0.40 % |
| 16 384 | 2×16K | 93 171.8 | 434.1 | 1.813× | −0.40 % | 0.00 % |
| 16 384 | 2×32K | 93 171.8 | 434.1 | 1.813× | −0.40 % | 0.00 % |
| 32 768 | 2×8K | 169 627.2 | 718.0 | 1.632× | — | +14.73 % |
| 32 768 | 2×16K | 148 040.6 | 683.4 | 1.870× | −12.73 % | +0.13 % |
| 32 768 | 2×32K | 147 843.4 | 581.6 | 1.872× | **−12.84 %** | 0.00 % |
| 65 536 | 2×8K | 320 618.2 | 856.6 | 1.455× | — | +85.75 % |
| 65 536 | 2×16K | 311 182.0 | 656.7 | 1.499× | −2.94 % | +80.28 % |
| 65 536 | 2×32K | 172 612.4 | 924.4 | **2.703×** | **−46.16 %** | 0.00 % |

SIMT-only reference: 168 884 / 276 824 / 466 534 cycles. At N = 16 384 three of
the five seeds tie *exactly* between 8K and 16K/32K (both runs are a single
window over an 8 260-element descriptor); the −0.40 % is the other two seeds
(−905, −948).

### 3.3 κ = 0.75 (regular-heavy)

| N | configuration | cycles | ± sd | vs SIMT | Δ vs 2×8K | Δ vs 2×32K |
|---|---|---|---|---|---|---|
| 16 384 | 2×8K | 95 139.6 | 81.1 | 1.258× | — | 0.00 % |
| 16 384 | 2×16K | 95 139.6 | 81.1 | 1.258× | 0.00 % | 0.00 % |
| 16 384 | 2×32K | 95 139.6 | 81.1 | 1.258× | 0.00 % | 0.00 % |
| 32 768 | 2×8K | 114 072.8 | 353.6 | 1.567× | — | +0.25 % |
| 32 768 | 2×16K | 113 792.0 | 429.9 | 1.571× | −0.25 % | 0.00 % |
| 32 768 | 2×32K | 113 792.0 | 429.9 | 1.571× | −0.25 % | 0.00 % |
| 65 536 | 2×8K | 245 041.4 | 403.3 | **1.199×** | — | **−6.97 %** |
| 65 536 | 2×16K | 263 497.6 | 1 545.6 | 1.115× | **+7.53 %** | +0.03 % |
| 65 536 | 2×32K | 263 419.2 | 1 482.2 | 1.116× | **+7.50 %** | 0.00 % |

SIMT-only reference: 119 664 / 178 794 / 293 899 cycles. At N = 16 384 all
three capacities are the *same run* (one window, 4 052 elements — the window
never fills), hence sd identical and Δ exactly 0. At N = 32 768 four of five
seeds tie exactly and one gives −1 404. At N = 65 536 32K is worse in **5/5**
seeds (+16 290 … +19 904).

### 3.4 The two axes, compared directly

Nine (κ, N) cells, counted from the table above:

| | 2×8K | 2×16K | 2×32K |
|---|---|---|---|
| strictly fastest (mean C) | 1 — κ=0.75/N=65 536 | 0 | 4 — κ≤0.5, N≥32 768 |
| tied for fastest | 1 — κ=0.75/N=16 384 | 4 | 8 |
| beats 2×8K | — | 6 | 7 |
| ties 2×8K exactly | — | 1 | 1 |
| loses to 2×8K | — | 2 | 1 |
| worst cell | — | κ=0.25/N=65 536 **+4.15 %** | κ=0.75/N=65 536 **+7.50 %** |
| best cell | — | κ=0.25/N=16 384 −15.02 % | κ=0.50/N=65 536 −46.16 % |

2×32K is therefore *never the wrong choice by more than 7.5 %* and *often the
right one by 30–46 %*; 2×16K is dominated in both directions (its best cell is
one where 32K is identical, and it has a cell 4.15 % worse than the default
while 32K is 29 % better).

### 3.5 Per-cell inventory

Where each requested metric is reported in this document (`[k]` = the simulator
section it comes from):

| metric | where |
|---|---|
| absolute cycles | §3.1–§3.3 (`[1]` run end, arm C) |
| speedup vs SIMT-only | §3.1–§3.3 (arm A of the same trace) |
| % vs 2×8K, % vs 2×32K | §3.1–§3.3 (paired per seed), §6 |
| target requests, duplicates merged | §5.1 (`[7b]`) |
| index requests | §5.1 (`index lines requested`, `[7b]`) |
| L1 hits/misses | §5.2–§5.3 (`[7]`: L1 requests, L1 hits — 0–2 hits per cell) |
| DRAM reads / writes | §5.2–§5.3 (`[7]`; writes are 0 by construction) |
| row activations, row-hit rate | §5.2–§5.4 (`[17]`, `[15]`) |
| TLB misses, page walks | §5.2 (`[6]`: 1 890–4 681 dTLB misses, 562–691 walks) |
| peak target residency | §7.3 (128 in every cell, `[16b]`) |
| buffer stalls, overlap | §4.1–§4.2 (`[16]`, per-window records) |

### 3.6 Reference brackets (seed 1, arm C)

| | 2×8K | 2×32K | Δ |
|---|---|---|---|
| κ = 0 (all irregular), N = 16 384 | 151 145 | 141 559 | −6.3 % |
| κ = 0, N = 65 536 | 530 642 | 305 421 | **−42.4 %** |
| κ = 1 (all regular), N = 16 384 | 40 281 | 40 281 | 0.0 % |
| κ = 1, N = 65 536 | 159 705 | 159 705 | 0.0 % |

The irregular endpoint reproduces the M12 result (large windows help a pure
gather). The regular endpoint is exactly flat: with no bulk work the engine has
an empty descriptor and the two capacities are the same program. **κ alone does
not decide the winner — the bulk stream's reuse per window does** (§5).

---

## 4. Does filling slow consumption?

The hypothesis was that a bigger buffer takes longer to fill and that this
delay, not the reuse benefit, drives the difference. It is false, and the
recorded events say why.

### 4.1 The four timing components (seed 1, arm C)

`startup` = cycle of the first target issue; `issue phase` = first → last target
issue; `tail` = last issue → end (final drain). `age` = mean allocation → seal.
All in cycles.

| κ | N | W | wins | age | first seal | first issue | startup | issue phase | tail | C |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.25 | 65 536 | 8 192 | 7 | 5 802 | 8 200 | 46 114 | 46 114 | 376 909 | 769 | 423 792 |
| 0.25 | 65 536 | 16 384 | 4 | 12 720 | 16 715 | 53 536 | 53 536 | 388 178 | 705 | 442 419 |
| 0.25 | 65 536 | 32 768 | 2 | 28 652 | 33 925 | 59 962 | 59 962 | **240 861** | 1 114 | 301 937 |
| 0.50 | 65 536 | 8 192 | 4 | 7 164 | 8 200 | 86 752 | 86 752 | 232 554 | 953 | 320 259 |
| 0.50 | 65 536 | 16 384 | 2 | 17 944 | 16 715 | 93 159 | 93 159 | 216 214 | 1 093 | 310 466 |
| 0.50 | 65 536 | 32 768 | 1 | 33 035 | 33 621 | 93 331 | 93 331 | **79 442** | 1 043 | 173 816 |
| 0.75 | 65 536 | 8 192 | 3 | 5 602 | 8 200 | 125 111 | 125 111 | 119 246 | 466 | 244 823 |
| 0.75 | 65 536 | 16 384 | 2 | 10 117 | 16 715 | 125 234 | 125 234 | 139 045 | 445 | 264 724 |
| 0.75 | 65 536 | 32 768 | 1 | 16 632 | 17 218 | 125 234 | 125 234 | **138 264** | 1 043 | 264 541 |
| 0.25 | 32 768 | 8 192 | 4 | 5 587 | 8 200 | 26 488 | 26 488 | 188 327 | 600 | 215 415 |
| 0.25 | 32 768 | 32 768 | 1 | 24 982 | 25 568 | 29 489 | 29 489 | 117 521 | 1 043 | 148 053 |
| 0.25 | 16 384 | 8 192 | 2 | 6 529 | 8 200 | 14 282 | 14 282 | 110 209 | 1 090 | 125 581 |
| 0.25 | 16 384 | 32 768 | 1 | 12 242 | 12 828 | 14 272 | 14 272 | 91 181 | 1 003 | 106 456 |
| 0.75 | 16 384 | any | 1 | 3 312 | 3 898 | 30 344 | 30 344 | 64 707 | 143 | 95 194 |

Three observations, all from recorded events:

1. **The first issue does not follow the first seal closely.** At κ = 0.25,
   N = 65 536 the first window seals at 8 200 but the first target issue is at
   46 114: the engine cannot issue a window's line until that line's index
   value has been fetched, and index acquisition runs to 308 337. Window size
   moves the *seal* by +8.5 k (16K) and +25.7 k (32K) cycles, but moves the
   *first issue* by only +7.4 k and +13.8 k, because the fill and the index
   pipeline overlap.
2. **The issue phase dominates every run** (79 %–92 % of cycles), and that is
   where the capacity difference lives: at κ = 0.25/N = 65 536 it falls from
   376 909 to 240 861 (−36 %) because 32K removes 16 538 requests; at κ = 0.75
   it *rises* from 119 246 to 138 264 (+16 %) although the 32K window removes
   36 requests (§5 explains why).
3. **The tail is negligible** (0.2 %–1.1 %): no cell is bounded by a long final
   drain.

### 4.2 Overlap: fills are hidden; the consumer is the bottleneck

Per-window events (seed 1). `fill total` = Σ(seal − alloc); `consume mean` =
mean(retire − issue); `overlap` = Σ max(0, retireᵢ − allocᵢ₊₁); `consumer-busy
wait` = Σ max(0, retireᵢ − sealᵢ₊₁), the time a sealed window sits waiting for
the consumer to take it; `consumer starve` = Σ max(0, sealᵢ₊₁ − retireᵢ), the
time the consumer has no sealed window; `buffer-full stall` =
`cycles with work but no filling window`.

| κ | N | W | wins | fill total | consume mean | overlap | consumer-busy wait | consumer starve | buffer-full stall | two-live cycles | C |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0.25 | 65 536 | 8 192 | 7 | 40 619 | 59 370 | 414 603 | 381 598 | **0** | 316 171 | 412 555 | 423 792 |
| 0.25 | 65 536 | 16 384 | 4 | 50 880 | 106 426 | 424 580 | 389 829 | **0** | 250 575 | 420 484 | 442 419 |
| 0.25 | 65 536 | 32 768 | 2 | 57 305 | 134 006 | 129 021 | 105 055 | **0** | 0 | 120 829 | 301 937 |
| 0.50 | 65 536 | 8 192 | 4 | 28 657 | 78 014 | 247 930 | 226 887 | **0** | 177 008 | 245 882 | 320 259 |
| 0.50 | 65 536 | 32 768 | 1 | 33 035 | 140 194 | 0 | 0 | **0** | 0 | 0 | 173 816 |
| 0.75 | 65 536 | 8 192 | 3 | 16 806 | 78 874 | 236 116 | 226 924 | **0** | 161 424 | 234 068 | 244 823 |
| 0.75 | 65 536 | 16 384 | 2 | 20 235 | 124 004 | 247 527 | 243 421 | **0** | 0 | 243 431 | 264 724 |
| 0.75 | 65 536 | 32 768 | 1 | 16 632 | 247 322 | 0 | 0 | **0** | 0 | 0 | 264 541 |
| 0.25 | 32 768 | 8 192 | 4 | 22 351 | 51 804 | 206 578 | 191 841 | **0** | 140 923 | 204 530 | 215 415 |
| 0.25 | 16 384 | 8 192 | 2 | 13 059 | 58 690 | 81 904 | 76 459 | **0** | 0 | 79 856 | 125 581 |

**The hypothesis is refuted, quantitatively:**

* `consumer starve` is **0 in all 27 cells**. The engine never waits for a fill.
  There is no cell in which acquisition latency is exposed by buffer capacity.
* `buffer-full stall` is large wherever there are ≥ 3 windows (316 171 cycles =
  74.6 % of the run at κ = 0.25/N = 65 536/8K): the generator is held back
  because **both buffers are occupied**, i.e. the reservoir is ahead of the
  consumer, not behind it.
* `fill total` per window (5.8 k–28.7 k) is 2 %–27 % of `consume mean`
  (59 k–247 k). Even the exposed fill of a *single* 32K window at κ = 0.75
  (16 632 cycles, no overlap at all) is 6.3 % of that run, while the run is
  7.5 % **slower** than 8K — so fill time cannot be the cause, and the next
  section shows what is.
* With ≥ 2 windows the fills are fully overlapped (`overlap` ≈ 98 % of C at
  κ = 0.25/8K; 236 116 of 244 823 = 96.4 % at κ = 0.75/8K). With exactly one
  window (all κ = 0.75 cells at N ≤ 32 768, and the 32K cells at N = 65 536,
  κ ≥ 0.5) overlap is structurally 0 and the single fill is exposed — worth
  3.6 k–16.6 k cycles, and in three of those cells the run is still faster or
  equal, so even an exposed fill is not decisive.

**Answer:** double buffering does hide the extra filling time. Larger windows
do not lose because filling is slow; they lose (where they lose) because of
*what the issued stream does to the rest of the machine*, which is §5.

---

## 5. Regular versus irregular interpretation

### 5.1 What the window buys: merges, nothing else

A larger window only helps by folding duplicate target lines before submission.
That requires two accesses to the same line to fall inside one window. Measured
arm C, seed 1:

| κ | N | elements | requests 8K | requests 16K | requests 32K | merges 8K / 16K / 32K |
|---|---|---|---|---|---|---|
| 0.25 | 16 384 | 12 376 | 12 376 | 12 376 | 12 376 | 0 / 0 / 0 |
| 0.25 | 32 768 | 24 620 | 24 606 | 24 596 | 16 384 | 14 / 24 / 8 236 |
| 0.25 | 65 536 | 49 304 | 49 290 | 49 272 | 32 752 | 14 / 32 / 16 552 |
| 0.50 | 16 384 | 8 260 | 8 260 | 8 260 | 8 260 | 0 / 0 / 0 |
| 0.50 | 32 768 | 16 436 | 16 428 | 16 420 | 16 384 | 8 / 16 / 52 |
| 0.50 | 65 536 | 32 592 | 32 580 | 32 560 | 16 384 | 12 / 32 / 16 208 |
| 0.75 | 16 384 | 4 052 | 4 052 | 4 052 | 4 052 | 0 / 0 / 0 |
| 0.75 | 32 768 | 8 308 | 8 308 | 8 308 | 8 308 | 0 / 0 / 0 |
| 0.75 | 65 536 | 16 428 | 16 420 | 16 412 | 16 384 | 8 / 16 / 44 |

The pattern is exact: **a window helps when it is at least as large as the
descriptor** (16 384 and 32 768 cells collapse to one window at 16K/32K) **or
when it covers a whole pass of the footprint** (κ = 0.25/N = 65 536 needs 32K
to reach 16 552 merges; 16K reaches only 32).

At κ = 0.75/N = 65 536 there are 16 428 elements on 16 420 distinct lines —
**one element per line**. The 32K window merges 44 lines, i.e. 0.27 % of the
work. Everything else about it is pure overhead, and it is overhead that the
run pays for twice: the delayed single seal (§4.1) and §5.2.

### 5.2 Why the κ = 0.75 / N = 65 536 cell regresses

Same 16 420 requests either way, so the regression cannot come from the bulk
stream's own traffic. It comes from the *SIMT* stream sharing the machine:

| κ = 0.75, N = 65 536 | 2×8K | 2×16K | 2×32K |
|---|---|---|---|
| bulk target requests | 16 420 | 16 412 | 16 384 |
| bulk dedup merges | 8 | 16 | 44 |
| **total DRAM reads** | 27 564 | 29 742 | **29 726** |
| L2 hits | 3 211 | 1 029 | **1 017** |
| DRAM row activations | 3 007 | 2 038 | 2 007 |
| row-hit rate | 89.1 % | 93.1 % | 93.2 % |
| residual if the B-only arm's reads are the bulk share (25 946 / 25 274 / 25 262) | ≈ 1 618 | ≈ 4 468 | ≈ 4 464 |
| cycles | 244 823 | 264 724 | 264 541 |

With one element per line the bulk stream has almost no repeats, so its requests
are essentially all misses and its own DRAM traffic is fixed (the B-only arm
even *improves* with capacity: 25 946 → 25 262 reads). Yet total DRAM reads rise
by 2 162 and L2 hits fall by 2 194 — the two numbers match to 1.5 %. The lost
L2 hits are the **SIMT** stream's: a single 32K window issues its 16 384 target
lines as one uninterrupted burst covering the whole 1 MiB footprint, which is
exactly the shared L2's capacity, so the regular stream's working set is flushed
between its own accesses. Three 8K windows deliver the same lines in three
bursts with the SIMT arm's reuse intact between them.

That is the second cost of capacity, and it is invisible in the bulk-only arm:
**a large window converts a smooth gather into a burst that evicts the other
client's cache lines.** It costs ≈ 9 cycles per lost L2 hit here (19 718 extra
cycles for 2 194 lost hits, of which 2 162 reappear as DRAM reads), plus the
delayed first issue, and it is the whole of the +7.50 %.

The B-only column of that table is a *different descriptor* (arm B always runs
the whole N-element trace, not the irregular subset), so the residual row is an
indication of magnitude, not a per-client measurement; see §8.1.

The same effect is present but dominated elsewhere: L2 hits also fall at
κ = 0.25/N = 65 536 (7 425 → 5 132) and at κ = 0.5 (6 899 → 8 008, rising —
there the eliminated requests themselves free L2 capacity). The mechanism is
only *decisive* when the merge saving is ~0.

### 5.3 Why κ = 0.50 / N = 65 536 wins by 46 %

Half the elements sit on half the lines: 32 592 elements on 16 384 lines. A 32K
window folds all 16 208 duplicates at once (requests 32 580 → 16 384, exactly
one per line), DRAM reads 37 149 → 19 730 (−47 %), L2 hits *rise* (6 899 →
8 008) because 16 k fewer requests stop evicting it, and the issue phase
collapses from 232 554 to 79 442 cycles. Nothing about the SIMT arm is harmed
(the 8K configuration never had a locality advantage to lose — its bulk stream
was 32 k requests over 16 k lines). This is the best case for capacity, and it
is 3.66× the storage.

### 5.4 Small N: the descriptor fits, so capacity is irrelevant

At N = 16 384 the bulk descriptor is 4 052–12 376 elements. A 16K window always
holds the whole descriptor, so 16K and 32K are literally the same run, and at
κ ≥ 0.5 so is 8K (8 260 < 8 192 is false only marginally; at κ = 0.75 4 052 is
well under). The κ = 0.25 row is the exception where 8K is forced into two
windows (12 376 elements) and pays for it: −15.0 % with a bigger window, of
which the merge saving is nil (0 merges at any capacity — 12 376 distinct
lines) and the whole gain is **M7/M12 page grouping**: row activations 2 206 →
1 192 at an unchanged request count. A reusable, capacity-independent
observation: at small N a larger window pays for *page coherence*, not dedup.

---

## 6. Storage-independent performance analysis

SRAM is `2 × (W × 8 + page_dir × 16 + cohorts)`; the page directory and cohorts
are shared and unchanged, so the incremental cost of capacity is 131 072 B
(8K→16K) and 393 216 B (8K→32K).

| κ | N | Δ vs 8K @ +131 KB (16K) | Δ vs 8K @ +393 KB (32K) | marginal 16K→32K | verdict |
|---|---|---|---|---|---|
| 0.25 | 16 384 | −15.02 % | −15.02 % | 0 | **16K is enough** |
| 0.25 | 32 768 | −3.40 % | −30.85 % | −28.4 % | 32K only |
| 0.25 | 65 536 | **+4.15 %** | −29.22 % | −32.0 % | 32K only |
| 0.50 | 16 384 | −0.40 % | −0.40 % | 0 | neither |
| 0.50 | 32 768 | −12.73 % | −12.84 % | −0.1 % | **16K is enough** |
| 0.50 | 65 536 | −2.94 % | **−46.16 %** | −44.5 % | 32K only |
| 0.75 | 16 384 | 0.00 % | 0.00 % | 0 | neither |
| 0.75 | 32 768 | −0.25 % | −0.25 % | 0 | neither |
| 0.75 | 65 536 | **+7.53 %** | **+7.50 %** | +0.03 % | **8K** |

Reading of the table:

* **The 16K step is almost never worth it on its own.** It buys ≥ 10 % in one
  cell of nine (κ = 0.5, N = 32 768, −12.73 %, where it happens to be enough to
  cover the 16 436-element descriptor) and is actively harmful in one
  (−4.15 %). Its problem is precise: 16K covers a descriptor up to 16 384
  elements, so it captures *at most* one fold per line; 32K covers two passes
  and captures the second fold too. A capacity between the two is the worst of
  both.
* **Efficiency of the 32K step is not monotone in κ** — it is best at the mid
  mixture (−46.2 % for +393 KB at κ = 0.5/N = 65 536), good at irregular-heavy
  (−29.2 %), and negative at regular-heavy (+7.5 %). Normalized:
  **−12.6 %, −8.0 %, +2.0 % per unit of ×storage**.
* **Storage is not the binding constraint for the winner.** 32K costs 3.66× the
  buffer SRAM (516 KiB, half the shared 1 MiB L2); at −46 % cycles in the best
  cell it is an easy trade. The binding constraint is *when* it helps, and no
  storage amount around 32K buys the κ = 0.75 cell: capacity there has nothing
  to deduplicate, so more of it is more overhead.
* **The 8K configuration is never badly wrong.** Its worst relative position is
  +85.75 % against 32K at κ = 0.5/N = 65 536 (i.e. 32K is 1.86× faster), but it
  is the fastest or tied in every κ = 0.75 cell and in all small cells.

---

## 7. Correctness and regression results

### 7.1 Traces and outputs are identical across capacities

Verified programmatically over all 143 logs, not by inspection:

* `accesses / instrs / lines / pages / kappa_real` are identical across the three
  W for every (κ, N, seed) group — **45/45 groups, 0 disagreements**.
* `output check (C): value hash … vs expectation …` is identical, and identical
  to the independent expectation, across the three W for every group —
  **45/45 groups, 0 disagreements**.
* `mismatched addresses` = 0 in all 143 runs (single distinct value in the set).

This is the strongest available statement that the matrix compares one logical
workload under three capacities: the *answers* are bit-identical, only the
timing differs.

### 7.2 Published anchors (2 × 8K, seed 1)

| anchor | expected | measured |
|---|---|---|
| M10 E3 κ = 0.25 | 125 581 | 220 123 / 138 924 / **125 581** |
| M10 E3 κ = 0.50 | 94 035 | 169 826 / 127 138 / **94 035** |
| M10 E3 κ = 0.75 | 95 194 | 120 110 / 116 804 / **95 194** |

All three reproduce exactly, including the A and B arms. SIMT-only anchors are
preserved (220 123 / 169 826 / 120 110). No historical anchor or configuration
was modified; `configs/ab_controlled.cfg` is untouched and `batch_window`
remains 0 (i.e. 8 192 via the M10 configuration) by default.

### 7.3 Invariants and gates

* `M9 GATES PASSED` — **143/143**, exit status 0 in every run.
* Residency: `peak total outstanding` = **128 in all 143 runs**, never above
  R = 128; `peak bulk residency` 114–128. Larger windows create no additional
  outstanding capacity (the pool is unchanged and shared).
* Accounting: `target requests generated == lines issued`, `unissued +
  issued_not_filled ≤ R`, `dedup_merged + target_requests == window entries`,
  `windows sealed == seal reason counts` — all checked by the existing gate
  block in every run.
* Unit/regression suite: **4 338 checks, 0 failures** (`./bin/dx100_tests`),
  including the ten M13 tests, with M13 disabled as before.

---

## 8. Recommendation

**Keep 2 × 8K as the official default. Do not promote 2 × 32K globally.**

Against the brief's five options:

| option | verdict |
|---|---|
| universally better | **no** — worse by 7.5 % at κ = 0.75/N = 65 536; wins 7 of 9 cells, ties 1, loses 1 |
| better primarily for irregular-heavy workloads | **partly** — better for κ ≤ 0.5 at N ≥ 32 768; but the decisive variable is reuse per window, not κ |
| better only above certain sizes or reuse distances | **yes, this is the accurate statement** |
| not worth its storage | **no** — the storage is cheap where it helps (−46 % for 3.66× a 148 KB buffer) |
| candidate for workload-dependent configuration | **yes** — recommended form |

The rule the data supports, stated in the simulator's own terms:

> Enable 2 × 32K when the bulk descriptor's `elements / distinct target lines`
> ratio is ≥ 2 **and** the descriptor is larger than 16 384 elements; otherwise
> 2 × 8K.

Both quantities are already available at dispatch time in this model (the M8
index stream knows the descriptor length; the first window's seal knows its
distinct-line count). A cheaper proxy that separates every cell in this matrix:
*descriptor elements > window capacity* — exactly the condition under which a
second fold exists. The one cell where that proxy misleads is κ = 0.75/N = 65 536
(16 428 elements > 8 192, yet one element per line): there the distinct-line
count is equal to the element count, which the second quantity catches and the
first does not.

Concretely:

1. **M10 default unchanged**: 2 × 8K, `batch_window = 0` in the published
   configuration. No anchor moves.
2. **Add 2 × 32K as a documented option** (`--set batch_window=32768`), marked
   *irregular-heavy, N ≥ 32 768, ≥ 2 elements per target line*.
3. **Retire 2 × 16K as a candidate.** It is dominated: the worst cell in the
   matrix (+4.15 % where 32K wins 29 %), a tie where 32K wins 46 %, and it never
   beats 32K by more than 0.13 %. It should remain only as the M11.2/M12
   reference point it already is.
4. **Do not tune capacity for regular-heavy mixes.** At κ ≥ 0.75 every
   configuration within 4× storage lands within ±0.3 % except the one
   regression; the 8K default is the right answer there and the *reason* is
   measurable (no folds to win, L2 damage to lose).

### 8.1 What this does not establish

* The `+7.50 %` regression is explained by 2 162 extra DRAM reads against
  2 194 lost L2 hits, but the L2 attribution is inferred from counts, not from
  a per-client L2 hit counter — the model does not have one. A per-client L2
  accounting patch would settle it and is the smallest useful follow-up.
* Only W1 (permutation gather) was swept. W0, PSCR and STRIDmix were measured
  for capacity in M12/M13 under κ = 0.25 and are not re-swept here; the brief's
  matrix is κ × N × W, and it is complete.
* The 8.9 % `+85.75 %` figure in §3.2 is a *ratio against 32K*, not a claim that
  8K is bad: at that cell 8K is still 1.455× SIMT.

---

### Appendix A — files

| path | contents |
|---|---|
| `DX100_MEM/sim/build/wm/m14_matrix.sh` | 143-cell harness (135 hybrid + 8 reference) |
| `DX100_MEM/sim/build/wm/m14_extract.py` | `perf`, `seeds`, `mem`, `win`, `check`, `overlap`, `raw` |
| `DX100_MEM/sim/build/wm/m14_*.txt` | 143 logs, one per cell |
| `DX100_MEM/sim/build/wm/m14_matrix.log` | one line per cell: rc, gate, A/B/C cycles |
| `DX100_MEM/sim/src/ab.cpp`, `bulk.cpp` | the `[16]` window records used here (M9 instrumentation, unchanged by M14) |

### Appendix B — reproduction

```
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests      # 4338 checks, 0 failures
./build/wm/m14_matrix.sh                               # 143 cells, ~4 min
python3 build/wm/m14_extract.py perf                   # §3
python3 build/wm/m14_extract.py seeds                  # per-seed paired deltas
python3 build/wm/m14_extract.py win 1                  # §4 timing
python3 build/wm/m14_extract.py mem 1                  # §5 traffic
python3 build/wm/m14_extract.py check                  # §7 gates
```
