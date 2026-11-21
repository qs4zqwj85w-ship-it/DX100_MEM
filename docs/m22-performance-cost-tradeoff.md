# M22 — Performance versus Cost

Cost from `m22-storage-accounting.md` and `m22-area-power-estimates.md`; benefit
from the M19–M21 measurements, not from the M17 speedups. Every performance number
here is 4 lanes, the calibrated configuration (M20 established that wider widths
are formation-only models).

---

## 1. The benefit, restated in the terms the cost is paid in

| regime | workload | target set | S_f 8K | S_f 16K | cycles saved vs SIMT |
|---|---|---|---:|---:|---:|
| A (ordering) | w4_duplow | 8 MiB, 0 % reuse | **2.994** | **5.655** | 66.6 % / 82.3 % |
| A | w8_wslarge | 2 MiB, reuse 0.21 | 1.532 | 1.690 | 34.7 % / 40.8 % |
| B (index request count) | w1_uniform | 128 KiB | 1.159 | 1.164 | 13.7 % / 14.1 % |
| B | w5_sequential | 1 MiB, ordered | 1.132 | 1.138 | 11.6 % / 12.1 % |
| B | w7_wssmall | 16 KiB | 1.087 | 1.097 | 8.0 % / 8.8 % |
| B | w6_zipf | 128 KiB | 1.074 | 1.081 | 6.9 % / 7.5 % |
| parity | w3_duphigh | 4 KiB, 64 lines | 1.039 | 1.039 | 3.8 % |
| parity | w2_clustered | 128 KiB, 90 % hot | 1.038 | 1.048 | 3.7 % |

The project's **real** matrices sit in regime B (M20, 4 lanes, 2×8K):

| matrix | x (target set) | S_f 8K | S_f 16K |
|---|---|---:|---:|
| lnsp3937 | 31 KB | 1.236 | 1.237 |
| bcsstk25 | 123 KB | 1.182 | 1.177 |
| dubcova3 prefix | 1.17 MB | 1.180 | 1.177 |
| qc2534 | 20 KB | 1.047 | 1.025 |

**The 1.5–3.0× regime is not what the application work has ever measured.** It is
reachable only with a target set above L2 and low reuse, which is a synthetic
construction here (an 8 MiB permuted index vector) and is exactly what M21 was
built to isolate. On everything real, the honest figure is **1.05–1.24×**.

## 2. Marginal value of each mechanism, measured

Nothing below is inferred from the architecture's description; each row is an
existing configuration point of the same engine.

| mechanism | storage it costs (build F, 8K) | measured contribution | verdict |
|---|---|---|---|
| **window (element store + fanout)** | 8 B/element = 65 536 B/buffer | **W = 0 vs W = 8K**: w1 0.975 → 1.159, w3 0.893 → 1.039, w4 0.994 → 2.994 | **load-bearing** |
| **page ordering** inside the window | 0 B (produced by the seal) | w4 2.994 → **1.001** with `page_order=0`; w2 1.038 → 1.003; w5 unchanged | **load-bearing in regime A** |
| **cohorts = 2** vs 1 | 64 B | w4 2.994 → **1.833** with one cohort | **load-bearing in regime A** |
| **dedup / merge** | 0 B extra (fused grouping) | w3 is the merge-only family (1 995 : 1 collapse) and gains 3.8 %, of which the index stream is 60 562 cycles and the engine overhead 53 178 → **merge ≈ 0** | **cheap, but not the source of the win** |
| **index pipeline + descriptors** | 5 396 B + 40 B/depth | necessary but *not* sufficient: the W = 0 path has the full index pipeline and still scores 0.893–0.994 | **necessary, insufficient** |
| **page directory + mask** | 16 384 B (two buffers) | **no decision reads it**; direct-mapped, 6 391 pages against 512 entries on w4 | **dead weight** |
| permutation + order arrays (build U only) | 65 536 B | removed by build F with no behavioural change (M15) | **rent** |

The W = 0 rows are the most informative measurement in this audit. They say that
**the engine's cheap half cannot beat SIMT at all**: with no window the engine is
0.89–0.99× even though it still removes the index stream from the SIMT path. The
win, in both regimes, comes from the expensive half — holding elements, grouping
them by line and page, and issuing one request per line in page order.

That reframes the cost question: the 246–475 KB is not overhead around a cheap
core; it *is* the mechanism.

## 3. Cost per unit of delivered benefit

Two builds, four configurations. Area is the nominal assumption E1 from
`m22-area-power-estimates.md` (0.045 µm²/bit); ratios are more trustworthy than
absolutes.

| configuration | total storage | nominal area | × 1 MiB L2 | regime-B cycles saved | regime-A cycles saved |
|---|---:|---:|---:|---:|---:|
| U 2×8K (shipped) | 937 KiB | 0.345 mm² | 0.92× | 11.6–15.4 % | 34.7–66.6 % |
| U 2×16K | 1.79 MiB | 0.676 mm² | 1.79× | 11.6–15.4 % | 40.8–82.3 % |
| **F 2×8K** | **273 KiB** | **0.101 mm²** | **0.27×** | **11.6–15.4 %** | 34.7–66.6 % |
| **F 2×16K** | **497 KiB** | **0.183 mm²** | **0.49×** | 11.7–15.5 % | **40.8–82.3 %** |

Read the columns: **build F delivers exactly the performance of build U at 3.4–3.7×
less area**, because M15 measured the two representations equivalent and the
shipped default simply happens to be the expensive one. On the real matrices
(1.05–1.24×), the applicable cost is F 2×8K = 0.101 mm² = **0.27× a 1 MiB L2**, and
the alternative to spending it is nothing — the engine does not exist.

Per-percent-of-cycles-saved, on regime B's realistic 13.7 % (w1): 0.101 mm² →
7.4 %/0.01 mm². Compare the machine's existing L2: 0.377 mm² buys the cache that
both arms already share. The window is 27 % of an L2 for 13.7 % of the run.

## 4. 2×8K versus 2×16K

| | 2×8K | 2×16K | Δ |
|---|---:|---:|---:|
| storage (F) | 273 KiB | 497 KiB | +224 KiB |
| area (nominal) | 0.101 mm² | 0.183 mm² | +0.082 mm² |
| w4_duplow | 2.994 | 5.655 | **+2.66×** |
| w8_wslarge | 1.532 | 1.690 | +0.16 |
| w1_uniform | 1.159 | 1.164 | +0.005 |
| w5_sequential | 1.132 | 1.138 | +0.006 |
| w2_clustered | 1.038 | 1.048 | +0.010 |
| w3_duphigh | 1.039 | 1.039 | 0.000 |

The larger window is a **regime-A instrument only**. It doubles the length of the
ordered run, which is worth 2.66× on an 8 MiB scattered set and literally zero on a
one-page set. In regime B it buys 0.5–1.0 % for +0.082 mm². M20 found the same
thing from the other direction (2×16K was never better than 2×8K at ≥ 8 lanes).

**Recommendation: instantiate 8K per buffer by default; make 16K a mode bit over
the same physical array only if regime A is a target workload.** Do not ship two
arrays.

## 5. Hardware that is expensive relative to its demonstrated contribution

| item | cost | demonstrated contribution | disposition |
|---|---|---|---|
| page directory + mask | 16 384 B (two buffers), 6.7 % of F 8K | **none** — no decision reads it; it cannot even represent the page set it describes | **delete** |
| permutation `sorted` + `order` | 65 536 B (U only) | none — the fused grouping replaces both | **delete with the fused build** |
| 32 B `LineReq` | 524 288 B (U, 16K worst case) | the fields it holds (va, first, count, seq, slot, issued) fit in 111 bits | **delete with the fused build** |
| `reuse_ent` field | 4 B/line | only M13 reads it, and M13 is off by default | **drop from the shipped build** |
| merge/dedup as a *mechanism* | ~0 B (folded into grouping) | ≈ 0 cycles in the one workload built to expose it | **keep** — it costs nothing, and it is what makes the grouping pass cheap |
| 2×16K | +0.082 mm² | +0.005–0.010 in regime B | **mode bit, not a second array** |

What is *not* on this list is anything that carries the win: the element store, the
seal/grouping, the page ordering and the cohorts all pay for themselves in both
regimes. The audit's criticism is of implementation choices (an unfused
representation shipped as the default, a directory that does nothing), not of the
architecture's core.

## 6. Energy

From `m22-area-power-estimates.md` §3: the engine **increases** DRAM traffic in
every measured workload (+0.3 % to +11.4 %, because targets bypass L1 and windows
fetch whole lines). In regime A that is outweighed by 67 000 fewer row activations
(0.1 % → 45.5 % row hits); in regime B there is nothing to outweigh it, so the
engine is **faster and less energy-efficient at the same time**. A deployment that
is bandwidth- or energy-limited should not enable it for regime-B workloads even
though it is 4–16 % faster there.

## 7. Verdict on cost versus benefit

**2 — promising only after reducing hardware complexity.**

The justification, stated in the form the evidence supports:

1. **The core mechanism pays for itself.** The window, not the index pipeline, is
   what beats SIMT (W = 0 scores 0.89–0.99), and it delivers 11.6–15.4 % on the
   real matrices and 34.7–82.3 % on scattered >L2 gathers.
2. **The shipped implementation is 3.4–3.7× more area than the same performance
   requires.** Build F at 8K is 273 KiB / 0.101 mm² nominal; build U at 8K is
   937 KiB / 0.345 mm² and behaves identically (M15). Nothing in M19–M21 depends on
   the unfused representation.
3. **One structure in the brief's audit list is dead**: the page directory and
   page masks cost 16 KiB and are read by no issue decision.
4. **Two structures are under-modelled, not over-costed**: the index ring holds no
   line data (add 8 KiB), and `Pending` at 56 B is really 57 k flops (0.017–0.034
   mm²).
5. **The 16K point is a mode bit, not a second array.**

Residual honest caveat, which keeps this from being verdict 1: **the 1.5–3.0×
regime has never been observed on a real matrix in this project.** M18/M20's
matrices are all regime B (1.05–1.24×). Whether a real application produces an
8 MiB, low-reuse, scattered gather is the open question that decides between
verdict 2 and verdict 3, and it is not answerable from the data in this tree.
