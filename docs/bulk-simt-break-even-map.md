# SIMT vs bulk accelerator: break-even map

**Record type.** Each number is tagged **[M]** measured with the standalone DX100 simulator, **[A]** analytical (closed form, or an identity checked against a measurement), or **[H]** hypothesised. Nothing in the results below is hypothesised; the single **[H]** item is the heuristic's behaviour outside the swept grid.

**Scope.** This experiment adds no code, no ISA or RTL change, and no capacity. R = 128 outstanding requests is unchanged, and the bulk engine draws from the same R pool. M9 does not exist and is not used. All previously published benchmark numbers are unchanged and reproduce exactly (Section 8).

---

## 1. What the two arms actually pay

**Configuration, identical in both arms** [M]: 1 core; L1 16 KiB (256 lines, 4-way, 16 MSHRs); L2 1 MiB (16 384 lines, 8-way, 64 MSHRs); dTLB 16×4 (4 MSHRs); L2 TLB 512×4 (8 MSHRs); 2 page walkers at 2 levels × 150 cycles with a 16-entry walk cache; `dram_q = 0`, `seed = 1`, `max_cycles = 2·10⁸`. Request budget R = 128 outstanding, shared by both arms.

**Arms** [M]:

| arm | settings |
|---|---|
| A (SIMT) | `mode=baseline` |
| M6 | `mode=bulk page_sched=0 idx_mode=0` |
| M7 | `page_sched=1 idx_mode=0` |
| M8 | `page_sched=1 idx_mode=2` |

Common bulk settings: `batch_window=8192`, `bulk_max_elems=262144`.

**Cost structure.** A SIMT run issues N accesses and pays whatever the memory hierarchy charges for each; its DRAM traffic is essentially the compulsory set of distinct lines plus page-table fetches. A bulk run issues exactly

  *I = ⌈N·w / 64⌉ index-line requests*  [A]

plus *T* target requests, where T is the number of distinct target lines after intra-window deduplication, and it issues them page-ordered. So the accelerator's fixed tax is I, which depends only on N and the index width — both known from the descriptor — and its benefit is order plus deduplication.

Measured confirmation of the tax: for `r_W1` (N = 16 384, w = 4) the engine issues I = 1024 index lines and 16 384 target requests, and reads 17 446 DRAM lines against SIMT's 16 417 — **+6.3% traffic** [M], exactly the I/(I+T) ratio at that point.

**The floor.** Bulk's cycles per element cannot fall below *(I/N)·c_req*. With I/N = w/64 and c_req ≈ 10.3 cycles per request (calibrated from the measured floors below, i.e. latency amortised over R = 128 in flight) that is ≈ 0.65 cycles/element for 4-byte indices [A]. Measured bulk floors: 0.646 (`r_HOT`), 0.660 (`t_lam64`), 0.663 (`h_128_4`), 0.683 (`r_DUP`), 0.705 (`t_lam256`) [M]. SIMT's floor is the issue rate, 0.266 cycles/element (`r_HOT`) [M].

Since 0.65 > 0.27, **the accelerator can only win by making SIMT's per-element cost rise above its own floor**. That happens when the footprint leaves L1: SIMT costs 0.417 cycles/element at L = 256 (`r_DUP`) and 0.668 at L = 512 (`t_lam512`) [M]. The L1 boundary and the amortization rule of Section 6 are two views of the same crossing.

---

## 2. Representability

Read from `src/workload.cpp` [M, source]:

- **λ is derived, not free.** λ = clamp(Λ/P, 1, 64); the line set is P·λ entries, truncated to Λ. **Λ > 64P is not representable** — the footprint silently shrinks. `r_W0` sits exactly at the edge (Λ = 16 384, P = 256, λ = 64).
- **κ is a sequential fraction, not a scatter factor.** The regular path walks the line array linearly, one entry per instruction (the source calls this W0-style streaming). κ therefore means "this instruction is on the linear sweep", and κ = 1 is the *best* case for SIMT, not the worst.
- **An instruction spans ⌈4w/64⌉ lines**: one line at w = 4 (8 elements/line) and one at w = 8 (4 elements/line). With κ = 1 the distinct-line count is therefore the instruction count, not N.
- **ρ and Λ are coupled.** ρ = 0 is a permutation (no duplicates); ρ > 0 draws with replacement from M = solve_M(N, ρ) clamped to Λ, so the realized footprint shrinks as ρ rises: L = 16 384 / 10 344 / 8 162 / 4 091 / 1 638 for ρ = 0 / 0.25 / 0.50 / 0.75 / 0.90 at Λ = 16 384 [M].
- **κ and Λ are coupled too.** A mixed stream realizes fewer lines than requested: κ = 0.25 → L = 12 636, κ = 0.50 → 9 319, κ = 0.75 → 6 376, κ = 1.0 → 4 096 (Λ = 16 384, P = 512) [M].
- **N < Λ truncates the permutation**: `t_n4k` (N = 4096, Λ = 16 384) realizes L = 4096 [M].
- **Page density is a first-order control that L does not reveal.** Same Λ, N, κ, w, with P = 512 (λ = 32) vs P = 2048 (λ = 8): 40 281 vs 156 303 SIMT cycles, a 3.9× swing [M]. The map therefore reports realized pages alongside realized lines.
- **P > 2048 needs shifted block bases** (`bulk_index_base = 134 217 728`, `bulk_desc_line = 1 572 864`) because the 16 384-line data span exceeds the 8 MiB region the default placement leaves clear. This is measurement-enabling only; it changes no published number, and the 2048-page points reproduce exactly under both placements (`r_PSCR` ≡ `t_p2048`, 1 908 638 / 1 160 292 / 807 159 / 857 517 [M]).
- **Not swept / not representable**: Λ > 64P; κ > 0 combined with ρ > 0 (each is representable, the combination is untested here); index arrays larger than 2 MiB; more than one kernel in flight.

---

## 3. The map

42 configurations × 4 arms = 168 runs, one process per arm, every run `conservation_ok=1` [M]. Classification: **BULK** at sp ≥ 1.05, **≈** at 0.95 < sp < 1.05, **SIMT** at sp ≤ 0.95. Result: **28 BULK, 5 ≈, 9 SIMT**.

| point | N | Λ | P | λ | L | Pg | I | T | I/L | q̂ | pages/line | A cycles | best arm | best cycles | sp | class |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|:-:|--:|--:|:-:|
| `h_1024_1024` | 16384 | 1024 | 1024 | 1 | 1024 | 1024 | 1024 | 3072 | 1.00 | 0.00 | 1.000 | 2027739 | M8 | 466531 | 4.346 | BULK |
| `h_rho60p2048` | 16384 | 16384 | 2048 | 8 | 6533 | 918 | 1024 | 10598 | 0.16 | 0.00 | 0.125 | 1119909 | M7 | 389034 | 2.879 | BULK |
| `h_p1024` | 16384 | 16384 | 1024 | 16 | 16384 | 1024 | 1024 | 16384 | 0.06 | 0.00 | 0.062 | 1285238 | M7 | 454947 | 2.825 | BULK |
| `h_512_512` | 16384 | 512 | 512 | 1 | 512 | 512 | 1024 | 1536 | 2.00 | 0.00 | 1.000 | 243488 | M6 | 95990 | 2.537 | BULK |
| `t_p2048` | 16384 | 16384 | 2048 | 8 | 16384 | 2048 | 1024 | 16384 | 0.06 | 0.00 | 0.125 | 1908638 | M7 | 807159 | 2.365 | BULK |
| `r_PSCR` | 16384 | 16384 | 2048 | 8 | 16384 | 2048 | 1024 | 16384 | 0.06 | 0.00 | 0.125 | 1908638 | M7 | 807159 | 2.365 | BULK |
| `h_n8k` | 8192 | 8192 | 256 | 32 | 8192 | 256 | 512 | 8192 | 0.06 | 0.00 | 0.031 | 135155 | M8 | 71107 | 1.901 | BULK |
| `h_8192_128` | 16384 | 8192 | 128 | 64 | 8192 | 128 | 1024 | 16384 | 0.12 | 0.00 | 0.016 | 128494 | M8 | 71397 | 1.800 | BULK |
| `t_lam4096` | 16384 | 4096 | 128 | 32 | 4096 | 128 | 1024 | 11320 | 0.25 | 0.00 | 0.031 | 75917 | M8 | 43678 | 1.738 | BULK |
| `t_lam16384` | 16384 | 16384 | 512 | 32 | 16384 | 512 | 1024 | 16384 | 0.06 | 0.00 | 0.031 | 270753 | M8 | 156419 | 1.731 | BULK |
| `r_W1` | 16384 | 16384 | 512 | 32 | 16384 | 512 | 1024 | 16384 | 0.06 | 0.00 | 0.031 | 270753 | M8 | 156419 | 1.731 | BULK |
| `r_PERM` | 16384 | 16384 | 512 | 32 | 16384 | 512 | 1024 | 16384 | 0.06 | 0.00 | 0.031 | 270753 | M8 | 156419 | 1.731 | BULK |
| `t_rho50` | 16384 | 16384 | 512 | 32 | 8162 | 322 | 1024 | 12304 | 0.13 | 0.00 | 0.031 | 141882 | M7 | 82790 | 1.714 | BULK |
| `t_p256` | 16384 | 16384 | 256 | 64 | 16384 | 256 | 1024 | 16384 | 0.06 | 0.00 | 0.016 | 244093 | M8 | 146066 | 1.671 | BULK |
| `t_rho75` | 16384 | 16384 | 512 | 32 | 4091 | 131 | 1024 | 8736 | 0.25 | 0.00 | 0.031 | 69903 | M8 | 42092 | 1.661 | BULK |
| `t_n64k` | 65536 | 16384 | 512 | 32 | 16384 | 512 | 4096 | 62557 | 0.25 | 0.00 | 0.031 | 815070 | M7 | 499167 | 1.633 | BULK |
| `t_rho25` | 16384 | 16384 | 512 | 32 | 10344 | 512 | 1024 | 13689 | 0.10 | 0.00 | 0.031 | 199934 | M7 | 127195 | 1.572 | BULK |
| `t_k25` | 16384 | 16384 | 512 | 32 | 12636 | 512 | 1024 | 13120 | 0.08 | 0.19 | 0.031 | 220123 | M7 | 144946 | 1.519 | BULK |
| `h_2048_256` | 16384 | 2048 | 256 | 8 | 2048 | 256 | 1024 | 6128 | 0.50 | 0.00 | 0.125 | 68237 | M7 | 47168 | 1.447 | BULK |
| `h_4096_4096` | 16384 | 4096 | 4096 | 1 | 4096 | 4096 | 1024 | 11304 | 0.25 | 0.00 | 1.000 | 2438138 | M6 | 1701372 | 1.433 | BULK |
| `t_w8_142` | 16384 | 16384 | 512 | 32 | 16384 | 512 | 2048 | 16384 | 0.12 | 0.00 | 0.031 | 270753 | M7 | 192480 | 1.407 | BULK |
| `t_rho90` | 16384 | 16384 | 512 | 32 | 1638 | 52 | 1024 | 4610 | 0.63 | 0.00 | 0.031 | 31495 | M8 | 23079 | 1.365 | BULK |
| `t_n4k` | 4096 | 16384 | 512 | 32 | 4096 | 512 | 256 | 4096 | 0.06 | 0.00 | 0.031 | 107541 | M8 | 80497 | 1.336 | BULK |
| `t_k50` | 16384 | 16384 | 512 | 32 | 9319 | 512 | 1024 | 9913 | 0.11 | 0.38 | 0.031 | 169826 | M8 | 131199 | 1.294 | BULK |
| `r_STRID` | 16384 | 1024 | 64 | 16 | 1024 | 64 | 1024 | 3024 | 1.00 | 0.75 | 0.062 | 22664 | M8 | 17953 | 1.262 | BULK |
| `h_w8_2048` | 16384 | 2048 | 64 | 32 | 2048 | 64 | 2048 | 7928 | 1.00 | 0.00 | 0.031 | 42043 | M8 | 34543 | 1.217 | BULK |
| `t_p8192` | 16384 | 16384 | 8192 | 2 | 16384 | 8192 | 1024 | 16384 | 0.06 | 0.00 | 0.500 | 2380239 | M6 | 1989946 | 1.196 | BULK |
| `t_lam1024` | 16384 | 1024 | 32 | 32 | 1024 | 32 | 1024 | 3072 | 1.00 | 0.00 | 0.031 | 21390 | M8 | 18283 | 1.170 | BULK |
| `r_PGLO` | 16384 | 1024 | 16 | 64 | 1024 | 16 | 1024 | 3072 | 1.00 | 0.00 | 0.016 | 17797 | M8 | 17018 | 1.046 | ≈ |
| `t_k75` | 16384 | 16384 | 512 | 32 | 6376 | 512 | 1024 | 6853 | 0.16 | 0.56 | 0.031 | 120110 | M8 | 118689 | 1.012 | ≈ |
| `h_1024_k50` | 16384 | 1024 | 32 | 32 | 1024 | 32 | 1024 | 3071 | 1.00 | 0.38 | 0.031 | 18389 | M8 | 18282 | 1.006 | ≈ |
| `t_p16384` | 16384 | 16384 | 16384 | 1 | 16384 | 16384 | 1024 | 16384 | 0.06 | 0.00 | 1.000 | 2457664 | M8 | 2463318 | 0.998 | ≈ |
| `t_k100` | 16384 | 16384 | 512 | 32 | 4096 | 128 | 1024 | 4096 | 0.25 | 0.75 | 0.031 | 40281 | M8 | 42305 | 0.952 | ≈ |
| `h_8192_k90` | 16384 | 8192 | 256 | 32 | 4590 | 256 | 1024 | 5079 | 0.22 | 0.68 | 0.031 | 65530 | M8 | 69648 | 0.941 | SIMT |
| `t_w8_1024` | 16384 | 1024 | 32 | 32 | 1024 | 32 | 2048 | 4992 | 2.00 | 0.00 | 0.031 | 21390 | M8 | 26159 | 0.818 | SIMT |
| `r_W0` | 16384 | 16384 | 256 | 64 | 4096 | 64 | 1024 | 4096 | 0.25 | 1.00 | 0.016 | 30573 | M8 | 38775 | 0.788 | SIMT |
| `t_lam512` | 16384 | 512 | 16 | 32 | 512 | 16 | 1024 | 1536 | 2.00 | 0.00 | 0.031 | 10941 | M8 | 14170 | 0.772 | SIMT |
| `t_lam256` | 16384 | 256 | 8 | 32 | 256 | 8 | 1024 | 768 | 4.00 | 0.00 | 0.031 | 7289 | M8 | 11555 | 0.631 | SIMT |
| `r_DUP` | 16384 | 256 | 4 | 64 | 256 | 4 | 1024 | 768 | 4.00 | 0.00 | 0.016 | 6831 | M8 | 11190 | 0.610 | SIMT |
| `h_128_4` | 16384 | 128 | 4 | 32 | 128 | 4 | 1024 | 384 | 8.00 | 0.00 | 0.031 | 5442 | M8 | 10869 | 0.501 | SIMT |
| `t_lam64` | 16384 | 64 | 2 | 32 | 64 | 2 | 1024 | 192 | 16.00 | 0.00 | 0.031 | 4671 | M7 | 10810 | 0.432 | SIMT |
| `r_HOT` | 16384 | 2 | 1 | 2 | 2 | 1 | 1024 | 4 | 512.00 | 0.00 | 0.500 | 4361 | M6 | 10580 | 0.412 | SIMT |

Legend: Λ requested lines, P requested pages, λ lines per page, L and Pg realized distinct lines and pages, I index-line requests, T target requests issued, q̂ the sequential-coherence statistic of Section 6, sp = A cycles / best bulk-arm cycles.

### 3.1 Regions where SIMT clearly wins

*Footprint at or below L1* — `r_HOT` (L = 2, **0.412**), `t_lam64` (64, 0.432), `h_128_4` (128, 0.501), `r_DUP` (256, 0.610), `t_lam256` (256, 0.631). Bulk pays 1024 index lines to replace at most 768 target requests, and its floor is 0.65 cycles/element against SIMT's 0.27–0.45. The mechanism is sharper than a ratio: the **acquisition prologue** issues its first target at cycle 4115–4431 while the entire SIMT run takes 4361–7289 cycles [M] — the prologue alone would consume the whole baseline execution.

*Index cost not amortized* — I/L ≥ 2 with dense pages: `t_lam512` (2.00, **0.772**), `t_w8_1024` (2.00, 0.818). The engine spends one index line per two target lines and cannot recover it.

*Stream already ordered* — q̂ = 1.0: `r_W0` (**0.788**) at I/L = 0.25, a value deep inside bulk territory. Every 4-lane instruction already fetches exactly the lines the engine would have requested, in the same order, so ordering adds nothing and the index stream is pure tax.

### 3.2 Regions where bulk clearly wins

*Amortized index, scattered targets* — I/L ≤ 1: from `t_lam1024` (1.170), `h_w8_2048` (1.217), `r_STRID` (1.262), `t_k50` (1.294), `t_n4k` (1.336), `t_rho90` (1.365), up to `r_PERM`/`t_lam16384` (**1.731**). Deduplication and page ordering convert a scattered stream into a near-sequential one: `t_lam16384` issues 17 408 requests for 16 384 accesses and reads 17 446 DRAM lines where SIMT reads 16 417, yet finishes 1.73× faster.

*Large footprints on medium page counts* — `h_rho60p2048` (**2.879**), `h_p1024` (2.825), `t_p2048`/`r_PSCR` (2.365), `h_n8k` (1.901). SIMT is walk- and row-limited here: 7 465 / 8 567 / 12 723 walks against the engine's 2 545 / 2 977 / 5 326 [M].

*Fully dispersed small footprint* — `h_512_512` (**2.537**) with I/L = 2.0. Every line sits on its own page, so SIMT pays a walk-bound 14.9 cycles/element while the engine's page-ordered run touches 2597 DRAM lines against 13 231 [M]. This is the one place where a high I/L is overridden.

*Page-shared hot set* — `h_1024_1024` (**4.346**), the maximum in the grid: 1024 lines over 1024 pages, SIMT 13 518 walks against 3090, DRAM 15 835 against 4167.

### 3.3 Regions of approximate equality

Five points fall inside the ±5% band — `r_PGLO` (1.046), `t_k75` (1.012), `h_1024_k50` (1.006), `t_p16384` (0.998), `t_k100` (0.952) — and `h_8192_k90` (0.941) sits 0.9 pp outside it, the grid's only genuine near-miss for the heuristic. Two mechanisms produce parity:

- **Partial sequentiality**: `t_k75` (I/L 0.16, κ 0.75) and `h_1024_k50` (I/L 1.00, κ 0.50) both land on the crossing.
- **Walker saturation** at P ≥ 8192: both arms need ≈ 1 walk per access, and 2 walkers × 300 cycles gives 16 384 · 300/2 = 2 457 600 cycles — measured SIMT 2 457 664 at P = 16 384 and 2 438 138 at P = 4096 [M, A]. Once SIMT is pinned at the walker ceiling the engine can only match it (16 402 vs 16 384 walks), so parity is structural, not incidental.

---

## 4. Mechanism detail

| point | arm | I | T | I/(I+T) | L1 hit | L2 hit | L1 joins | walks | SIMT walks | row hit | SIMT row hit | bank | SIMT bank | buf max | slot stall | idx wait | sched idle | DRAM bulk | DRAM SIMT |
|---|:-:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| `h_1024_1024` | M8 | 1024 | 3072 | 0.25 | 0.00 | 0.59 | 0 | 3090 | 13518 | 0.23 | 0.00 | 0.10 | 0.10 | 69 | 0 | 5732 | 458973 | 4167 | 15835 |
| `h_rho60p2048` | M7 | 1024 | 10598 | 0.09 | 0.00 | 0.31 | 0 | 2545 | 7465 | 0.60 | 0.00 | 0.28 | 0.14 | 86 | 0 | 2941 | 379495 | 11514 | 12217 |
| `h_p1024` | M7 | 1024 | 16384 | 0.06 | 0.00 | 0.25 | 0 | 2977 | 8567 | 0.68 | 0.01 | 0.34 | 0.17 | 106 | 0 | 3127 | 444470 | 17499 | 16449 |
| `h_512_512` | M6 | 1024 | 1536 | 0.40 | 0.00 | 0.28 | 0 | 534 | 512 | 0.37 | 0.00 | 0.29 | 0.71 | 128 | 9336 | 3444 | 0 | 2597 | 13231 |
| `t_p2048` | M7 | 1024 | 16384 | 0.06 | 0.00 | 0.37 | 0 | 5326 | 12723 | 0.48 | 0.00 | 0.22 | 0.11 | 83 | 0 | 3103 | 794871 | 17572 | 16513 |
| `r_PSCR` | M7 | 1024 | 16384 | 0.06 | 0.00 | 0.37 | 0 | 5326 | 12723 | 0.48 | 0.00 | 0.22 | 0.11 | 83 | 0 | 3103 | 794871 | 17572 | 16513 |
| `h_n8k` | M8 | 512 | 8192 | 0.06 | 0.00 | 0.05 | 0 | 265 | 256 | 0.93 | 0.03 | 0.91 | 0.78 | 120 | 55094 | 2500 | 10274 | 8724 | 8209 |
| `h_8192_128` | M8 | 1024 | 16384 | 0.06 | 0.00 | 0.48 | 0 | 145 | 128 | 0.92 | 0.03 | 0.97 | 0.82 | 128 | 45703 | 4112 | 17913 | 9228 | 8201 |
| `t_lam4096` | M8 | 1024 | 11320 | 0.08 | 0.00 | 0.59 | 0 | 145 | 128 | 0.93 | 0.06 | 0.87 | 0.68 | 128 | 30882 | 4516 | 5444 | 5132 | 4105 |
| `t_lam16384` | M8 | 1024 | 16384 | 0.06 | 0.00 | 0.06 | 0 | 544 | 512 | 0.82 | 0.02 | 0.90 | 0.78 | 123 | 68629 | 5981 | 77894 | 17446 | 16417 |
| `r_W1` | M8 | 1024 | 16384 | 0.06 | 0.00 | 0.06 | 0 | 544 | 512 | 0.82 | 0.02 | 0.90 | 0.78 | 123 | 68629 | 5981 | 77894 | 17446 | 16417 |
| `r_PERM` | M8 | 1024 | 16384 | 0.06 | 0.00 | 0.06 | 0 | 544 | 512 | 0.82 | 0.02 | 0.90 | 0.78 | 123 | 68629 | 5981 | 77894 | 17446 | 16417 |
| `t_rho50` | M7 | 1024 | 12304 | 0.08 | 0.00 | 0.33 | 0 | 339 | 322 | 0.80 | 0.02 | 0.93 | 0.76 | 128 | 28712 | 3471 | 45979 | 9334 | 8338 |
| `t_p256` | M8 | 1024 | 16384 | 0.06 | 0.00 | 0.03 | 0 | 273 | 256 | 0.80 | 0.02 | 0.98 | 0.87 | 128 | 122325 | 6241 | 15079 | 17428 | 16401 |
| `t_rho75` | M8 | 1024 | 8736 | 0.10 | 0.00 | 0.49 | 0 | 148 | 131 | 0.87 | 0.05 | 0.95 | 0.75 | 128 | 24779 | 5164 | 10496 | 5128 | 4101 |
| `t_n64k` | M7 | 4096 | 62557 | 0.06 | 0.00 | 0.13 | 0 | 651 | 512 | 0.83 | 0.01 | 0.95 | 0.91 | 128 | 424678 | 3459 | 56153 | 59311 | 57224 |
| `t_rho25` | M7 | 1024 | 13689 | 0.07 | 0.00 | 0.24 | 0 | 533 | 512 | 0.75 | 0.01 | 0.80 | 0.71 | 128 | 41495 | 3419 | 76520 | 11938 | 10931 |
| `t_k25` | M7 | 1024 | 13120 | 0.07 | 0.00 | 0.09 | 0 | 534 | 512 | 0.78 | 0.04 | 0.80 | 0.74 | 128 | 61047 | 3967 | 73915 | 13877 | 12783 |
| `h_2048_256` | M7 | 1024 | 6128 | 0.14 | 0.00 | 0.59 | 0 | 273 | 256 | 0.80 | 0.03 | 0.55 | 0.39 | 128 | 95 | 3137 | 39699 | 3130 | 2068 |
| `h_4096_4096` | M6 | 1024 | 11304 | 0.08 | 0.00 | 0.64 | 0 | 11321 | 16253 | 0.08 | 0.00 | 0.09 | 0.09 | 69 | 0 | 3444 | 0 | 12596 | 16726 |
| `t_w8_142` | M7 | 2048 | 16384 | 0.11 | 0.00 | 0.06 | 0 | 564 | 512 | 0.79 | 0.02 | 0.79 | 0.78 | 128 | 98351 | 6134 | 83010 | 18469 | 16417 |
| `t_rho90` | M8 | 1024 | 4610 | 0.18 | 0.00 | 0.54 | 0 | 69 | 52 | 0.93 | 0.12 | 0.86 | 0.64 | 128 | 13544 | 4535 | 2705 | 2670 | 1643 |
| `t_n4k` | M8 | 256 | 4096 | 0.06 | 0.00 | 0.18 | 0 | 517 | 512 | 0.76 | 0.02 | 0.46 | 0.49 | 65 | 0 | 1252 | 76791 | 4388 | 4129 |
| `t_k50` | M8 | 1024 | 9913 | 0.09 | 0.00 | 0.13 | 0 | 541 | 512 | 0.73 | 0.13 | 0.68 | 0.68 | 128 | 46997 | 6051 | 75028 | 10427 | 9397 |
| `r_STRID` | M8 | 1024 | 3024 | 0.25 | 0.00 | 0.51 | 0 | 81 | 64 | 0.89 | 0.87 | 0.88 | 0.35 | 121 | 1710 | 4078 | 9437 | 2056 | 1029 |
| `h_w8_2048` | M8 | 2048 | 7928 | 0.21 | 0.00 | 0.60 | 0 | 97 | 64 | 0.93 | 0.10 | 0.88 | 0.60 | 128 | 19903 | 12405 | 3597 | 4105 | 2053 |
| `t_p8192` | M6 | 1024 | 16384 | 0.06 | 0.00 | 0.59 | 0 | 13244 | 15868 | 0.05 | 0.00 | 0.11 | 0.09 | 72 | 0 | 3436 | 0 | 17956 | 17089 |
| `t_lam1024` | M8 | 1024 | 3072 | 0.25 | 0.00 | 0.51 | 0 | 49 | 32 | 0.93 | 0.19 | 0.83 | 0.57 | 128 | 8171 | 4516 | 1926 | 2054 | 1027 |
| `r_PGLO` | M8 | 1024 | 3072 | 0.25 | 0.00 | 0.51 | 0 | 33 | 16 | 0.92 | 0.19 | 0.90 | 0.68 | 128 | 7282 | 4112 | 2283 | 2053 | 1026 |
| `t_k75` | M8 | 1024 | 6853 | 0.13 | 0.00 | 0.17 | 0 | 538 | 512 | 0.70 | 0.37 | 0.55 | 0.58 | 128 | 39726 | 6579 | 70130 | 7438 | 6413 |
| `h_1024_k50` | M8 | 1024 | 3071 | 0.25 | 0.00 | 0.51 | 0 | 49 | 32 | 0.93 | 0.28 | 0.83 | 0.63 | 128 | 8171 | 4516 | 1925 | 2054 | 1027 |
| `t_p16384` | M8 | 1024 | 16384 | 0.06 | 0.00 | 0.62 | 0 | 16402 | 16384 | 0.05 | 0.00 | 0.10 | 0.09 | 69 | 0 | 5493 | 2449115 | 18469 | 17595 |
| `t_k100` | M8 | 1024 | 4096 | 0.20 | 0.00 | 0.05 | 0 | 145 | 128 | 0.93 | 0.94 | 0.90 | 0.75 | 128 | 27881 | 6576 | 6934 | 5132 | 4105 |
| `h_8192_k90` | M8 | 1024 | 5079 | 0.17 | 0.00 | 0.15 | 0 | 273 | 256 | 0.82 | 0.66 | 0.65 | 0.63 | 128 | 30716 | 6004 | 30981 | 5634 | 4607 |
| `t_w8_1024` | M8 | 2048 | 4992 | 0.29 | 0.00 | 0.57 | 0 | 65 | 32 | 0.93 | 0.19 | 0.88 | 0.57 | 128 | 10712 | 11698 | 2495 | 3079 | 1027 |
| `r_W0` | M8 | 1024 | 4096 | 0.20 | 0.00 | 0.03 | 0 | 81 | 64 | 0.92 | 0.94 | 0.99 | 0.99 | 128 | 22612 | 5210 | 9251 | 5128 | 4101 |
| `t_lam512` | M8 | 1024 | 1536 | 0.40 | 0.00 | 0.41 | 0 | 33 | 16 | 0.93 | 0.37 | 0.81 | 0.51 | 128 | 4631 | 4516 | 1335 | 1541 | 514 |
| `t_lam256` | M8 | 1024 | 768 | 0.57 | 0.00 | 0.30 | 0 | 25 | 8 | 0.93 | 0.54 | 0.82 | 0.35 | 128 | 2800 | 4114 | 701 | 1285 | 258 |
| `r_DUP` | M8 | 1024 | 768 | 0.57 | 0.00 | 0.30 | 0 | 21 | 4 | 0.92 | 0.52 | 0.86 | 0.37 | 128 | 2204 | 3675 | 615 | 1285 | 258 |
| `h_128_4` | M8 | 1024 | 384 | 0.73 | 0.00 | 0.20 | 0 | 21 | 4 | 0.93 | 0.72 | 0.79 | 0.21 | 121 | 993 | 3287 | 612 | 1157 | 130 |
| `t_lam64` | M7 | 1024 | 192 | 0.84 | 0.05 | 0.08 | 0 | 19 | 2 | 0.93 | 0.88 | 0.75 | 0.11 | 128 | 0 | 2861 | 302 | 1093 | 66 |
| `r_HOT` | M6 | 1024 | 4 | 1.00 | 0.00 | 0.03 | 0 | 18 | 1 | 0.93 | 0.50 | 0.72 | 0.01 | 66 | 0 | 2559 | 0 | 1031 | 4 |

Columns are for the best bulk arm except the SIMT ones. `I/(I+T)` is the index share of the request stream; `buf max` is peak buffer occupancy in lines; `slot stall`, `idx wait` and `sched idle` are cycles.

1. **The index share spans 0.996 → 0.06** (`r_HOT` → `r_PSCR`). This single column is the amortization axis of Section 6 made explicit, and it is computable before dispatch.
2. **Bulk never hits in L1** (0.00 everywhere) and hits in L2 only 0.03–0.64. Its wins never come from caching; they come from ordering and deduplication. SIMT's small-footprint wins come from precisely the L1 residency the engine cannot use.
3. **L1 MSHR joins are zero for every bulk row** [M]: deduplication happens in the buffer before requests are issued, so `T < N` always — 1536 vs 16 384 at `t_lam512`, 3072 at `h_1024_1024`, 16 384 at the permutation points where nothing can merge.
4. **Walk reduction is the engine's largest lever and it saturates**: 5326 vs 12 723 walks at P = 2048 (2.4×), 13 244 vs 15 868 at P = 8192 (1.2×), 16 402 vs 16 384 at P = 16 384 (1.0×). The two-walker ceiling, not the engine, binds above P ≈ 8192.
5. **Row locality**: the engine holds row hit ≥ 0.92 in every L2-resident scattered case, degrading to 0.70–0.82 for the larger footprints and to 0.05 at P = 16 384, where no ordering can help. SIMT's row hit is 0.05–0.23 in exactly the page-dispersed cases (`h_1024_1024` 0.23, `t_p16384` 0.05) — that gap is the 4.3× at `h_1024_1024`.
6. **Buffer occupancy is bimodal**: 128 lines (the buffer cap) whenever requests retire at memory speed, and 65–86 in the walk-bound rows. Occupancy is a symptom of the walker ceiling, not a tuning knob [M].
7. **Slot stalls track T, not I**: 424 678 at `t_n64k`, 122 325 at `t_p256`, 98 351 at `t_w8_142`, versus ≤ 9 336 in the small-footprint rows. With R = 128 shared, the target stream is the binding constraint once T is large.
8. **Scheduler idle is the walk-bound signature**: 794 871 cycles at `r_PSCR` (42% of a 1.9 M-cycle run) and 458 973 at `h_1024_1024`, but ≤ 3 600 in most L2-resident rows (9 437 at `r_STRID`, the exception).
9. **Index-wait stalls are small but systematic** (2 500–12 405 cycles): the index stream is starved behind target issue, worst where T is largest, which is the expected arbitration cost of giving targets priority.
10. **Acquisition overlap**: descriptor organization costs 8192 cycles flat (`r_HOT` through `t_n64k`), the first target is issued at 4115–4431, and the index stream completes anywhere from 8940 (`t_lam64`) to 87 242 (`t_lam16384`) against a 156 419-cycle run — fully overlapped when T is large, and the dominant cost when L is small.

---

## 5. Which characteristics predict the crossover

Ranked by how much classification error each one removes on the 42 points:

1. **I/L (index lines per distinct target line)** — the primary axis. Every point with I/L ≥ 2 and dense pages is SIMT; every point with I/L ≤ 1 is bulk unless the stream is already ordered. It also absorbs index width: 8-byte indices are just a doubling of I/L (`t_w8_1024`, I/L = 2.00, 0.818, next to `t_lam512`, I/L = 2.00, 0.772) [M].
2. **q̂, the sequential-coherence fraction** — the conditional guard. At fixed geometry the κ sweep is monotone: 1.519 (κ = 0.25) → 1.294 (0.50) → 1.012 (0.75) → 0.952 (1.00). The crossover sits between κ = 0.50 and 0.75, i.e. the stream is worth ordering until roughly half of it is already ordered.
3. **Pages per line** — the exception guard. It separates `h_512_512` (2.537) from `t_lam512` (0.772) at identical I/L = 2.0, and it is the only feature that predicts the h_* page-dispersed wins.
4. **L against L1 capacity (256 lines)** — an unconditional floor, with no counter-example in the grid.
5. **P against the walker ceiling (≈ 8192 pages)** — turns any predicted win into parity; no ordering can reduce a walk count that is already one per access.

ρ and N are **not** independent predictors: their effects are fully explained by realized L and by I. The ρ sweep is mild and non-monotone in sign of mechanism (1.572 / 1.714 / 1.661 / 1.365 for ρ = 0.25 / 0.50 / 0.75 / 0.90) because rising ρ shrinks the footprint and therefore SIMT's compulsory traffic, while the N sweep (1.336 / 1.731 / 1.633 at N = 4096 / 16 384 / 65 536) reflects only how many index windows the run covers.

---

## 6. Candidate dispatch heuristic

**Derived from observable descriptor metadata only.** The descriptor exposes `count`, `index_bytes`, `data_bytes`, `index_base`, `data_base` — no distinctness, footprint, or page count. So:

| feature | how a runtime obtains it | cost |
|---|---|---|
| I | exact: ⌈count·index_bytes/64⌉ from descriptor scalars | zero |
| L̂ | sample s index lines, estimate the pool from two halves (Lincoln–Petersen), cap at count | s line reads |
| q̂ | fraction of consecutive sampled addresses within 64 B | free with the sample |
| p̂ | distinct pages / distinct lines in the sample | free with the sample |

q̂ was chosen because it is the ordering statistic that actually decides the comparison, and it is not the same as κ: for a 4-lane instruction, three of four consecutive pairs are intra-instruction (4 B apart) and the fourth spans the inter-line stride. A dense line set (λ = 64) gives q̂ = 1.0; a strided set gives q̂ = 0.75 for a *fully sequential* stream. The simulator's own `rho_line` counter reports the same 0.75 for those runs [M] — an independent check that q̂ measures something real.

**Rule:**

```
if L̂ <= 256:                       SIMT      # footprint inside L1
elif I >= 1.5 * L̂:                 BULK if p̂ >= 0.5 else SIMT   # unamortized, unless pages are dispersed
else:                              BULK if q̂ < 0.80 else SIMT
```

Thresholds: 256 = L1 line capacity; 1.5 = the midpoint in log space between the measured crossing points I/L = 2 (`t_lam512`, SIMT) and I/L = 1 (`t_lam1024`, bulk); 0.80 = "at least four consecutive pairs in five stay in one line"; 0.5 = "at least half the lines are alone on their page".

**Evaluation** — truth is the measured winner; a wrong choice costs the full gap to SIMT.

| point | split | I/L | q̂ | pages/line | sp | predicted | actual | loss if wrong |
|---|:-:|--:|--:|--:|--:|:-:|:-:|--:|
| `h_1024_1024` | held | 1.00 | 0.00 | 1.000 | 4.346 | BULK | BULK | — |
| `h_rho60p2048` | held | 0.16 | 0.00 | 0.125 | 2.879 | BULK | BULK | — |
| `h_p1024` | held | 0.06 | 0.00 | 0.062 | 2.825 | BULK | BULK | — |
| `h_512_512` | held | 2.00 | 0.00 | 1.000 | 2.537 | BULK | BULK | — |
| `t_p2048` | train | 0.06 | 0.00 | 0.125 | 2.365 | BULK | BULK | — |
| `r_PSCR` | ref | 0.06 | 0.00 | 0.125 | 2.365 | BULK | BULK | — |
| `h_n8k` | held | 0.06 | 0.00 | 0.031 | 1.901 | BULK | BULK | — |
| `h_8192_128` | held | 0.12 | 0.00 | 0.016 | 1.800 | BULK | BULK | — |
| `t_lam4096` | train | 0.25 | 0.00 | 0.031 | 1.738 | BULK | BULK | — |
| `t_lam16384` | train | 0.06 | 0.00 | 0.031 | 1.731 | BULK | BULK | — |
| `r_W1` | ref | 0.06 | 0.00 | 0.031 | 1.731 | BULK | BULK | — |
| `r_PERM` | ref | 0.06 | 0.00 | 0.031 | 1.731 | BULK | BULK | — |
| `t_rho50` | train | 0.13 | 0.00 | 0.031 | 1.714 | BULK | BULK | — |
| `t_p256` | train | 0.06 | 0.00 | 0.016 | 1.671 | BULK | BULK | — |
| `t_rho75` | train | 0.25 | 0.00 | 0.031 | 1.661 | BULK | BULK | — |
| `t_n64k` | train | 0.25 | 0.00 | 0.031 | 1.633 | BULK | BULK | — |
| `t_rho25` | train | 0.10 | 0.00 | 0.031 | 1.572 | BULK | BULK | — |
| `t_k25` | train | 0.08 | 0.19 | 0.031 | 1.519 | BULK | BULK | — |
| `h_2048_256` | held | 0.50 | 0.00 | 0.125 | 1.447 | BULK | BULK | — |
| `h_4096_4096` | held | 0.25 | 0.00 | 1.000 | 1.433 | BULK | BULK | — |
| `t_w8_142` | train | 0.12 | 0.00 | 0.031 | 1.407 | BULK | BULK | — |
| `t_rho90` | train | 0.63 | 0.00 | 0.031 | 1.365 | BULK | BULK | — |
| `t_n4k` | train | 0.06 | 0.00 | 0.031 | 1.336 | BULK | BULK | — |
| `t_k50` | train | 0.11 | 0.38 | 0.031 | 1.294 | BULK | BULK | — |
| `r_STRID` | ref | 1.00 | 0.75 | 0.062 | 1.262 | BULK | BULK | — |
| `h_w8_2048` | held | 1.00 | 0.00 | 0.031 | 1.217 | BULK | BULK | — |
| `t_p8192` | train | 0.06 | 0.00 | 0.500 | 1.196 | BULK | BULK | — |
| `t_lam1024` | train | 1.00 | 0.00 | 0.031 | 1.170 | BULK | BULK | — |
| `r_PGLO` | ref | 1.00 | 0.00 | 0.016 | 1.046 | BULK | ≈ | — |
| `t_k75` | train | 0.16 | 0.56 | 0.031 | 1.012 | BULK | ≈ | — |
| `h_1024_k50` | held | 1.00 | 0.38 | 0.031 | 1.006 | BULK | ≈ | — |
| `t_p16384` | train | 0.06 | 0.00 | 1.000 | 0.998 | BULK | ≈ | 0.2% |
| `t_k100` | train | 0.25 | 0.75 | 0.031 | 0.952 | BULK | ≈ | 4.8% |
| `h_8192_k90` | held | 0.22 | 0.68 | 0.031 | 0.941 | BULK | SIMT | 5.9% |
| `t_w8_1024` | train | 2.00 | 0.00 | 0.031 | 0.818 | SIMT | SIMT | — |
| `r_W0` | ref | 0.25 | 1.00 | 0.016 | 0.788 | SIMT | SIMT | — |
| `t_lam512` | train | 2.00 | 0.00 | 0.031 | 0.772 | SIMT | SIMT | — |
| `t_lam256` | train | 4.00 | 0.00 | 0.031 | 0.631 | SIMT | SIMT | — |
| `r_DUP` | ref | 4.00 | 0.00 | 0.016 | 0.610 | SIMT | SIMT | — |
| `h_128_4` | held | 8.00 | 0.00 | 0.031 | 0.501 | SIMT | SIMT | — |
| `t_lam64` | train | 16.00 | 0.00 | 0.031 | 0.432 | SIMT | SIMT | — |
| `r_HOT` | ref | 512.00 | 0.00 | 0.500 | 0.412 | SIMT | SIMT | — |

| set | points | correct | regret |
|---|--:|--:|--:|
| training | 22 | 20/22 | 0.23% |
| reference (W0/W1/HOT/DUP/PGLO/PSCR/STRID/PERM) | 8 | 8/8 | 0.00% |
| **held-out** | 12 | **11/12** | **0.49%** |
| all | 42 | 39/42 | 0.26% (worst 5.91%) |

Three misclassifications, all toward bulk and all small: `t_p16384` (0.2% loss, a tie), `t_k100` (4.8%), `h_8192_k90` (5.9%). The last is the only error outside the ±5% no-decision band, and it is the κ ≈ 0.9 corner where the κ sweep's own crossing (between 0.50 and 0.75) sits 7% away in speedup.

**Baselines over the same 42 points** [M]:

| policy | mean speedup vs SIMT | geomean | mean regret | worst regret |
|---|--:|--:|--:|--:|
| always SIMT | 1.000 | 1.000 | 26.5% | 77.0% |
| always M6 | 1.188 | 1.062 | 24.3% | 58.8% |
| always M7 | 1.455 | 1.286 | 8.7% | 58.8% |
| always M8 | 1.449 | 1.289 | 8.5% | 59.9% |
| always bulk, best bulk arm (oracle arm pick) | 1.471 | 1.304 | 7.5% | 58.8% |
| **heuristic, then best bulk arm** | 1.543 | 1.436 | 0.3% | 5.9% |

The heuristic's advantage over "always bulk with an oracle choice of bulk arm" comes precisely from the nine points where it declines to dispatch.

**Sensitivity** — the rule is flat across a wide band: every α ∈ [1.25, 2.0] gives identical results at every q̂ max tested, and only α = 1.0 (which sends `h_512_512` down the amortization branch) or α = 2.5 (which mislabels `t_lam512`) changes anything. Lowering q̂ max to 0.60 classifies `h_8192_k90` as SIMT and so fixes the single held-out miss — but that threshold is fitted between two measurements only 7% apart in speedup (0.941 vs 1.012 at `t_k75`), so it is reported as observed sensitivity, not adopted [M]:

| α | q̂ max | pages/line ≥ | train+ref (30) | held-out (12) |
|--:|--:|--:|--:|--:|
| 1.00 | 0.60 | 0.5 | 86.7% | 83.3% |
| 1.00 | 0.70 | 0.5 | 86.7% | 75.0% |
| 1.00 | 0.80 | 0.5 | 83.3% | 75.0% |
| 1.00 | 0.90 | 0.5 | 83.3% | 75.0% |
| 1.25 | 0.60 | 0.5 | 93.3% | 100.0% |
| 1.25 | 0.70 | 0.5 | 93.3% | 91.7% |
| 1.25 | 0.80 | 0.5 | 93.3% | 91.7% |
| 1.25 | 0.90 | 0.5 | 93.3% | 91.7% |
| 1.50 | 0.60 | 0.5 | 93.3% | 100.0% |
| 1.50 | 0.70 | 0.5 | 93.3% | 91.7% |
| 1.50 | 0.80 | 0.5 | 93.3% | 91.7% |
| 1.50 | 0.90 | 0.5 | 93.3% | 91.7% |
| 1.75 | 0.60 | 0.5 | 93.3% | 100.0% |
| 1.75 | 0.70 | 0.5 | 93.3% | 91.7% |
| 1.75 | 0.80 | 0.5 | 93.3% | 91.7% |
| 1.75 | 0.90 | 0.5 | 93.3% | 91.7% |
| 2.00 | 0.60 | 0.5 | 93.3% | 100.0% |
| 2.00 | 0.70 | 0.5 | 93.3% | 91.7% |
| 2.00 | 0.80 | 0.5 | 93.3% | 91.7% |
| 2.00 | 0.90 | 0.5 | 93.3% | 91.7% |
| 2.50 | 0.60 | 0.5 | 86.7% | 100.0% |
| 2.50 | 0.70 | 0.5 | 86.7% | 91.7% |
| 2.50 | 0.80 | 0.5 | 86.7% | 91.7% |
| 2.50 | 0.90 | 0.5 | 86.7% | 91.7% |

**Not implemented.** No scheduling, dispatch or ISA change corresponding to this rule has been made; it is a candidate only.

---

## 7. Limitations

- **No statistical error bars.** The simulator is deterministic and single-seeded, so repeat variance is zero by construction. Uncertainty is *coverage*, not noise: the 42 points are a grid, not a random sample, and interior behaviour is interpolated.
- **q̂ is emulated, not measured.** The simulator does not dump the index array, so q̂ and L̂ were computed by replaying the generator's documented layout and draw process, not by reading real index lines. Values agree with the simulator's `rho_line` counter where that counter is defined (0.75 for κ = 1 strided streams) [M], but a runtime sampling real index lines may see different q̂ when the index array is itself cached across windows.
- **Sampling estimator quality.** With a 64-line sample the rule scores 90.8% on the 30 training+reference points and 91.6% on the 12 held-out points, against 93.3% and 91.7% with population values [M, emulated sampling, 200 trials]. At 16 lines the held-out range widens to 75–100%. The sample-size/latency trade-off is not costed here.
- **Index placement.** I assumes a cold read of the whole index stream once per window. Caching index lines across windows, or an index layout that shares lines between windows, would reduce I and move the frontier toward bulk.
- **Concurrent dimensions.** κ and ρ were swept separately. Their combination is representable but untested.
- **Page count beyond 8192** is inside the walker ceiling for both arms, so the map's high-P edge reflects the translation design (2 walkers, 300 cycles) more than the engine. A different walker budget would move the P-axis edge without touching the I/L or q̂ axes.
- **Only these four arms.** No M9, no prefetching, no stream priority, no interleaving of two kernels. M7 and M8 serve as the bulk representative (`M7` best in 11 points, `M8` in 27, `M6` in 4); the heuristic's "bulk" outcome assumes the best arm is selectable, and arm choice inside bulk is left to the existing diagnostics.
- **Two of the three errors are near-ties**, so the reported accuracy understates robustness; the regret column is the more meaningful figure and is reported alongside every accuracy.

---

## 8. Preservation of earlier results

All eight published reference points reproduce their established cycle counts exactly in this experiment [M]:

| workload | A | M6 | M7 | M8 | sp |
|---|--:|--:|--:|--:|--:|
| W0 | 30573 | 39322 | 39530 | 38775 | 0.788 |
| W1 | 270753 | 233475 | 156912 | 156419 | 1.731 |
| HOT | 4361 | 10580 | 10580 | 10867 | 0.412 |
| DUP | 6831 | 11483 | 11489 | 11190 | 0.610 |
| PGLO | 17797 | 18251 | 17774 | 17018 | 1.046 |
| PSCR | 1908638 | 1160292 | 807159 | 857517 | 2.365 |
| STRID | 22664 | 26397 | 18876 | 17953 | 1.262 |
| PERM | 270753 | 233475 | 156912 | 156419 | 1.731 |

`t_lam16384` and `r_W1`/`r_PERM` coincide (`t_p2048` and `r_PSCR` likewise), which is the expected identity: the reference workloads are specific points of the same generator.

No file under `DX100_MEM/sim` was modified for this experiment; the only artefact added is this document.

### Reproducing

```
cd DX100_MEM/sim
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=none \
  --set mode=baseline --set N=16384 --set Lambda=16384 --set P=512 \
  --set kappa=0 --set rho_dup=0 --set elem_bytes=4 --set bulk_wl=0 \
  --set seed=1 --set batch_window=8192 --set bulk_max_elems=262144 \
  --set max_cycles=200000000 --set bulk_index_base=8388608 \
  --set bulk_desc_line=98304 --all
```

Swap `mode=baseline` for `--set mode=bulk --set page_sched=(0, 1) --set idx_mode=(0, 2)` for M6/M7/M8. For P > 2048 use `bulk_index_base=134217728` and `bulk_desc_line=1572864`. Realized geometry prints as `accesses=… instrs=… lines=… pages=… kappa=… rho_line=…` in any non-quiet plain run.

### Full per-arm cycles

| point | A | M6 | M7 | M8 | best | sp |
|---|--:|--:|--:|--:|:-:|--:|
| `h_1024_1024` | 2027739 | 466549 | 466549 | **466531** | M8 | 4.346 |
| `h_rho60p2048` | 1119909 | 595811 | **389034** | 419916 | M7 | 2.879 |
| `h_p1024` | 1285238 | 725580 | **454947** | 469932 | M7 | 2.825 |
| `h_512_512` | 243488 | **95990** | 95990 | 96344 | M6 | 2.537 |
| `t_p2048` | 1908638 | 1160292 | **807159** | 857517 | M7 | 2.365 |
| `r_PSCR` | 1908638 | 1160292 | **807159** | 857517 | M7 | 2.365 |
| `h_n8k` | 135155 | 84520 | 71367 | **71107** | M8 | 1.901 |
| `h_8192_128` | 128494 | 71867 | 72813 | **71397** | M8 | 1.800 |
| `t_lam4096` | 75917 | 49975 | 44855 | **43678** | M8 | 1.738 |
| `t_lam16384` | 270753 | 233475 | 156912 | **156419** | M8 | 1.731 |
| `r_W1` | 270753 | 233475 | 156912 | **156419** | M8 | 1.731 |
| `r_PERM` | 270753 | 233475 | 156912 | **156419** | M8 | 1.731 |
| `t_rho50` | 141882 | 128953 | **82790** | 83065 | M7 | 1.714 |
| `t_p256` | 244093 | 157523 | 146891 | **146066** | M8 | 1.671 |
| `t_rho75` | 69903 | 52361 | 42576 | **42092** | M8 | 1.661 |
| `t_n64k` | 815070 | 565531 | **499167** | 530571 | M7 | 1.633 |
| `t_rho25` | 199934 | 203508 | **127195** | 128142 | M7 | 1.572 |
| `t_k25` | 220123 | 221457 | **144946** | 145032 | M7 | 1.519 |
| `h_2048_256` | 68237 | 85371 | **47168** | 47235 | M7 | 1.447 |
| `h_4096_4096` | 2438138 | **1701372** | 1701372 | 1845318 | M6 | 1.433 |
| `t_w8_142` | 270753 | 266867 | **192480** | 202755 | M7 | 1.407 |
| `t_rho90` | 31495 | 25382 | 24990 | **23079** | M8 | 1.365 |
| `t_n4k` | 107541 | 158306 | 80813 | **80497** | M8 | 1.336 |
| `t_k50` | 169826 | 205667 | 131736 | **131199** | M8 | 1.294 |
| `r_STRID` | 22664 | 26397 | 18876 | **17953** | M8 | 1.262 |
| `h_w8_2048` | 42043 | 39666 | 35514 | **34543** | M8 | 1.217 |
| `t_p8192` | 2380239 | **1989946** | 1989946 | 2001019 | M6 | 1.196 |
| `t_lam1024` | 21390 | 19370 | 19939 | **18283** | M8 | 1.170 |
| `r_PGLO` | 17797 | 18251 | 17774 | **17018** | M8 | 1.046 |
| `t_k75` | 120110 | 165794 | 118723 | **118689** | M8 | 1.012 |
| `h_1024_k50` | 18389 | 19369 | 19938 | **18282** | M8 | 1.006 |
| `t_p16384` | 2457664 | 2463347 | 2463347 | **2463318** | M8 | 0.998 |
| `t_k100` | 40281 | 47814 | 42555 | **42305** | M8 | 0.952 |
| `h_8192_k90` | 65530 | 87850 | 70075 | **69648** | M8 | 0.941 |
| `t_w8_1024` | 21390 | 26652 | 26207 | **26159** | M8 | 0.818 |
| `r_W0` | 30573 | 39322 | 39530 | **38775** | M8 | 0.788 |
| `t_lam512` | 10941 | 15065 | 14195 | **14170** | M8 | 0.772 |
| `t_lam256` | 7289 | 11879 | 11884 | **11555** | M8 | 0.631 |
| `r_DUP` | 6831 | 11483 | 11489 | **11190** | M8 | 0.610 |
| `h_128_4` | 5442 | 11148 | 11113 | **10869** | M8 | 0.501 |
| `t_lam64` | 4671 | 10858 | **10810** | 10867 | M7 | 0.432 |
| `r_HOT` | 4361 | **10580** | 10580 | 10867 | M6 | 0.412 |
