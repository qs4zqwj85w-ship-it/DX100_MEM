# M19 — M18 Accounting, Equivalence and Correctness Audit

**Question.** Are SIMT, Bulk 2×8K and Bulk 2×16K charged for equivalent logical
work and for all the memory-system activity each architecture actually performs?

**Conclusion: option 2 — M18's accounting is sound, with clearly bounded
limitations.** One real trace-layout defect was found and corrected; it moved the
affected speedup by +0.001 and one appendix's cycles by ≤2.5 %. No reported
conclusion changes. The remaining limitations are enumerated, bounded and
classified in §5.

The audit also identified what the M18 gates do *not* check, and why passing all
28 cells is not by itself evidence of fair accounting (§4).

Companion documents: `m19-operation-equivalence-report.md` (per-stream accounting,
equivalence, nothing-for-free, cache interaction) and `m19-corrected-metrics.md`
(the correction record). Raw artifacts: `build/wm/m19_integrity.json`,
`build/wm/m19_recomputed.json`, `build/wm/m19_*.txt`.

---

## 1. Investigation 3 — the two comparisons, derived from counters

M18 reports two ratios. Here they are derived from arm counters rather than
restated, which also shows exactly what each one can and cannot mean.

### 1.1 The decomposition

Four measured cells per dataset. Write `A_n = A(noidx)`, `A_f = A(full)`,
`C_n = C(noidx)`, `C_f = C(full)`. All four arms execute the same physical
trace; `noidx` removes the SIMT index-load instructions from it. Define, purely
as differences of measured cycle counts:

| term | definition | meaning |
|---|---|---|
| `ΔA_idx` | `A_f − A_n` | what the index stream costs the SIMT machine *in the baseline*: requests, L1/L2 path, TLB, issue slots |
| `ΔC_eng` | `C_n − A_n` | what the whole engine machinery costs *on top of a program with no SIMT index loads*: index fetch + descriptor + AG + target requests, net of the gathers it replaces |
| `ΔC_dup` | `C_f − C_n` | what it costs to have the SIMT front end read the index array *while the engine also does* |

They close exactly (they are differences of the same four numbers):

| dataset | A_n | ΔA_idx | ΔC_eng | ΔC_dup | check |
|---|---:|---:|---:|---:|---|
| bcsstk25 | 308 704 | 152 984 | 81 791 | 101 954 | 308 704+81 791 = 390 495 = C_n; +101 954 = 492 449 = C_f |
| qc2534 | 446 600 | 315 903 | 281 462 | 195 728 | likewise |
| lnsp3937 | 37 422 | 19 592 | 8 691 | 2 185 | likewise |
| dubcova3¹ | 2 474 298 | 1 056 991 | 517 893 | 1 022 579 | likewise |

¹ corrected traces, 2×8K (`m19-corrected-metrics.md`).

### 1.2 "Index charged once"

```
S_f = A_f / C_n
    = (A_n + ΔA_idx) / (A_n + ΔC_eng)
    = 1 + (ΔA_idx − ΔC_eng) / C_n
```

So **the faithful ratio is greater than one exactly when the baseline's index-load
cost exceeds the hybrid's entire dispatch overhead** — not when "the index is
free". The engine's acquisition is inside `ΔC_eng`: the descriptor, 15 766 /
28 960 / 1 588 / 99 347 index-line requests, the AG, and 3 431 / 2 213 / 561 /
111 279 target requests. Nothing about it is assumed away.

| dataset | ΔA_idx | ΔC_eng | surplus | S_f |
|---|---:|---:|---:|---:|
| bcsstk25 | 152 984 | 81 791 | +71 193 | 1.182 |
| qc2534 | 315 903 | 281 462 | +34 441 | 1.047 |
| lnsp3937 | 19 592 | 8 691 | +10 901 | 1.236 |
| dubcova3 | 1 056 991 | 517 893 | +539 098 | 1.180 |

The engine's overhead is **44–89 %** of what the index stream costs the baseline
(53.5 %, 89.1 %, 44.4 %, 49.0 %). That ratio — not any byte accounting — is what
M18 measures, and it is why qc2534's speedup is only 4.7 %: the engine nearly
matches the cost it removes.

### 1.3 "Same-trace"

```
S_s = A_f / C_f = (A_n + ΔA_idx) / (A_n + ΔC_eng + ΔC_dup)
    = 1 + (ΔA_idx − ΔC_eng − ΔC_dup) / C_f
```

This charges the index stream **twice**: once to the SIMT front end and once to
the engine, for the same array at the same addresses. Its sign is therefore a
statement about duplication, not about the design: bcsstk25, qc2534 and dubcova3
fall below 1 (0.938 / 0.825 / 0.880) because `ΔC_dup` (101 954 / 195 728 /
1 022 579) is larger than the surplus; lnsp stays above 1 (1.180) because its
index stream is small and the engine cheap.

Neither convention is a *performance prediction*. `S_f` is the ratio of two
measured programs that differ only in the index-load instructions — the program a
compiler would emit if it dispatched the gather, versus the program it would emit
if it did not. `S_s` is the measured penalty for emitting both.

### 1.4 Why the engine's index fetch is cheaper per line

| dataset | SIMT index requests | engine index requests | bytes/request SIMT | bytes/request engine | request ratio |
|---|---:|---:|---:|---:|---:|
| bcsstk25 | 79 573 | 15 766 | 12.7 | 64.0 | 5.05× |
| qc2534 | 137 199 | 28 960 | 13.5 | 64.0 | 4.74× |
| lnsp3937 | 9 095 | 1 588 | 11.2 | 64.0 | 5.73× |
| dubcova3 | 495 868 | 99 347 | 12.8 | 64.0 | 4.99× |

Both readers move the same compulsory lines (the engine's request count *equals*
⌈4·nnz/64⌉); the SIMT front end needs ~5× as many requests to do it because a
4-lane group covers 16 B of a 64 B line and 26 % of groups straddle a boundary.
That is the whole mechanism, and it is why the advantage shrinks as the warp
widens (M18A lane sweep).

---

## 2. Investigation 8 — reproduction from raw logs

`tools/m19_recompute.py` re-parses the raw cell logs with its own regexes and
recomputes `S_f` and `S_s` from arm counters only (no shared code with
`m18_extract.py` / `m18_metrics.py` / `m18a_audit.py`).

| dataset | window | A(full) | C(noidx) | **S_f recomputed** | S_f published | C(full) | S_s recomputed | S_s published |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 2×8K | 461 688 | 390 495 | **1.182** | 1.182 | 492 449 | 0.938 | 0.938 |
| qc2534 | 2×8K | 762 503 | 728 062 | **1.047** | 1.047 | 923 790 | 0.825 | 0.825 |
| lnsp3937 | 2×8K | 57 014 | 46 113 | **1.236** | 1.236 | 48 298 | 1.180 | 1.180 |
| dubcova3 | 2×8K | 3 522 146 | 2 988 123 | **1.179** | 1.179 | 4 011 805 | 0.878 | 0.878 |
| bcsstk25 | 2×16K | 461 688 | 392 188 | **1.177** | 1.177 | 505 260 | 0.914 | 0.914 |
| qc2534 | 2×16K | 762 503 | 743 702 | **1.025** | 1.025 | 923 269 | 0.826 | 0.826 |
| lnsp3937 | 2×16K | 57 014 | 46 104 | **1.237** | 1.237 | 48 205 | 1.183 | 1.183 |
| dubcova3 | 2×16K | 3 522 146 | 2 996 821 | **1.175** | 1.175 | 4 013 206 | 0.878 | 0.878 |
| bcsstk25 | 128 MSHR | 455 871 | 387 872 | **1.175** | 1.175 | 492 589 | 0.925 | 0.925 |
| qc2534 | 128 MSHR | 742 629 | 727 950 | **1.020** | 1.020 | 888 562 | 0.836 | 0.836 |
| lnsp3937 | 128 MSHR | 55 834 | 45 778 | **1.220** | 1.220 | 47 136 | 1.185 | 1.185 |
| dubcova3 | 128 MSHR | 3 330 465 | 2 968 817 | **1.122** | 1.122 | 3 931 433 | 0.847 | 0.847 |

Every published primary metric reproduces to the last digit — the audit is
reporting the same arithmetic from the same logs, not a re-derivation that could
drift. Integrity records (raw-log SHA-256, trace hash, gate status, element
partition, output-check hashes) for all 52 recorded cells:
`build/wm/m19_integrity.json`. Summary: **0 output-hash mismatches, 0 bulk element
shortfalls, and the element partition equals the trace's own access count on all
34 cells that have a paired trace (0 mismatched)**; the only three gate failures
in the tree are cells that were never part of the results (two declared-invalid
probes and the M18A 32-lane deadlock).

---

## 3. Investigation 4/5/6 — findings

Summarised here; detail in `m19-operation-equivalence-report.md`.

* **Equivalence.** The element partition is exhaustive on every cell; the
  trace-derived request count matches the simulator's own counter *exactly* for
  both arms in both variants (8/8, 0 mismatches); the C-arm output check is
  `mismatched addresses 0` everywhere; `bulk_addr_mismatch = 0` everywhere. The
  hybrid's SIMT client therefore issues precisely the requests of the SIMT-class
  instructions of the shared trace — nothing dropped, nothing duplicated.
* **Nothing for free.** The engine cannot generate a target before its index line
  returns (`idx_done_` lookup, `bulk.cpp::generate`); AG throughput is bounded by
  `ag_lanes`; engine requests go through the same `submit()` with `vpn` set, so
  they translate and miss (C's `tlb0_misses` 2 068 vs A's 1 575 on bcsstk25); the
  engine has no L1 (`bypass_l1`), which is *worse* than a private L1, not better;
  the oracle path is off in every M18 cell.
* **Cache interaction.** The index stream and the gather compete under one LRU.
  Removing the index stream reveals its full cost including evictions: +17 / +31 /
  +1 / **+2 422** DRAM reads over the compulsory lines, and in the hybrid the
  index stream adds **+2 127** target misses on dubcova3 and 0 elsewhere. The
  interaction is real, measured, and scaled by working-set size — but the model
  has **no priority, pinning or stream-buffer policy** that could mitigate it
  (issue I7).

---

## 4. What passing the gates does *not* prove

All 28 M18 cells reported `M9 GATES PASSED`, and four of them (dubcova3) carried
the trace-layout defect of §5.1 without a single gate noticing. That is the
concrete demonstration that gate passage ≠ sound accounting, and it is worth
stating what the gates actually cover:

| gate | checks | does not check |
|---|---|---|
| G3 (mixed) | partition exhaustive; one descriptor; `bulk_elems_done + faults == elems_bulk`; `idx_lines == ⌈elems/per⌉`; `simt_completed == simt_generated`; residency ≤ pool; `addr_mismatch == 0`; C output == image expectation | that arm A's address set equals arm C's; that the trace layout is non-aliasing; that the *workload* is a faithful SpMV (no `y` store) |
| G1/G2 (degenerate paths) | full counter equality between arms on all-SIMT / all-bulk traces | anything about the mixed configuration |
| output check | C's values against an image-derived expectation | A's output — A *is* the truth by construction |

Two consequences the audit had to handle explicitly:

1. A/C equivalence is **not** gated; it was verified independently (§3, and
   M18A's proof that `full − colidx == noidx` exactly on
   `(address, renumbered instruction, lane)`).
2. Layout aliasing is invisible to every gate. Correctness is unaffected (both
   arms see the same addresses) but *which lines a stream touches* is not, and
   that is exactly what an accounting audit must catch.

---

## 5. Issue register

Severity: **High** = changes a reported conclusion · **Medium** = corrects a
published number materially, or bounds a conclusion · **Low** = attribution,
provenance or documentation · **Info** = outside the reported configurations.

| # | issue | severity | affected | effect on reported result | status |
|---|---|---|---|---|---|
| I1 | SpMV `rowptr_base = align(m*8)` places the row-pointer array inside `x` when `m < cols` (`dubcova3`: 813 512 B overlap) | **Medium** | dubcova3 (4 reported cells + ablation + lane sweep) | A +0.26 %, S_f 1.179 → **1.180**, S_s 0.878 → 0.880, `unique_lines` 316 377 → 319 190, pages 4 945 → 4 989 | **fixed, rerun** (`m19-corrected-metrics.md`) |
| I2 | BFS `rowptr_base = align(n*4)` places `rowptr` inside `dist` (8 B/vertex) on every graph | **Medium** | BFS appendix (6 cells) | cycles +0.65 % / +2.46 % / +0.92 %; probe share 19.9 %→20.4 %, 26.3 %→27.7 % | **fixed, rerun** |
| I3 | `y[i] += a·x[…]` accumulation and the `y` store are not modelled; traces are read-only | **Medium** (limitation) | all SpMV, both arms | symmetric in both arms; bound: 4 B/row = 965 / 159 / 247 / 2 813 lines, i.e. ≤1.9 % / 0.2 % / 4.5 % / 0.7 % of A's DRAM reads | documented; **not corrected** (architecture model, §6.2) |
| I4 | `bulk_win_bytes` (147 584) and `bulk_idx_bytes` (2 720) are modelled buffer *charges*, not traffic | **Low** | any reader quoting them as bytes | none — M18 does not report them; quoting `bulk_idx_bytes` as index traffic would understate it 371× (2 720 vs 1 009 024 B) | documented |
| I5 | per-requester `cache_diag` L2-miss rows do not sum exactly to the arm's `dram_reads` (+2, +2, +1, +52) | **Low** | attribution tables only | ≤0.011 % of traffic; no index or target figure affected | documented (`m18a` §2 note) |
| I6 | M18A re-ran the 8 primary cells after adding 19 print-only `m15` keys, overwriting the historical raw logs | **Low** (provenance) | 8 primary cells | numbers identical — verified by §2 reproduction against the published tables; the files gained the extra diagnostic lines | disclosed here |
| I7 | one shared LRU for index stream and gather; no priority / pinning / stream buffer | **Medium** (limitation) | all; dubcova3 most | the contention is *charged* (+2 422 / +2 127 misses), but no mitigation can be represented, so the hybrid is modelled without any cache-policy remedy a real design could have | documented, §6.2 |
| I8 | `rowptr` is re-read per row (2 scalar reads per row instruction = 8 B for a 4 B array) | **Low** (symmetric) | all SpMV | 17× amplification on a 965–2 813-line array (5 % of requests); identical in both arms, so no bias | documented |
| I9 | `m18_extract.py` / `m18_metrics.py` row regex could absorb per-requester rows into bogus metric keys | **Low** (tooling) | M18 tables | no reported number changed (regenerated table byte-identical apart from 4 cells that already existed on disk) | fixed in M18A |
| I10 | M18 prose calls 4 945 "pages of index array"; it is whole-trace `unique_pages` (the index array is 1 552 pages) | **Low** (documentation) | mechanism-ablation report | prose only; corrected value 4 989 whole-trace | documented |
| I11 | 32-lane SIMT run deadlocks on dubcova3 (1 request never completes, 200 M-cycle guard) | **Info** | M18A lane sweep only | not an M18 configuration; no reported number depends on it | documented in M18A |
| I12 | `noidx` traces undercharge arm A by 4 B/nnz by construction | **Info** (by design) | the instrument | `A(noidx)` is a diagnostic baseline, never a claim; the builder's own comment says so | disclosed in M18 |

Nothing in the register is High. No reported conclusion is withdrawn.

---

## 6. Investigation 7 — omitted costs, classified three ways

### 6.1 Genuinely absent from the architecture

* **Engine L1 hits.** The engine has no private L1 (`bypass_l1`), so it can never
  benefit from an L1 hit; every index and target request is an L2 access. This is
  a property of the design and if anything is modelled *conservatively*.
* **L1 allocation of streamed index lines.** Same mechanism, same direction.
* **Reuse of index values across windows.** One descriptor covers the run; the
  array is streamed once. Absent because the architecture streams it once.

### 6.2 Absent from the simulator (i.e. neither arm pays)

| omitted cost | why it matters | bound |
|---|---|---|
| `y` accumulation and store; writeback; DRAM write traffic | a real SpMV writes one 4 B output per row and reads it back for the reduction | 4 B/row ⇒ 965 / 159 / 247 / 2 813 lines = ≤1.9 % / 0.2 % / 4.5 % / 0.7 % of A's DRAM (largest relative on lnsp3937, where the whole working set fits L2) |
| cache policy alternatives (priority, pinning, stream buffers, bypass hints) | the index stream's eviction of the gather is charged (+2 422 misses on dubcova3) but no mitigation exists, so the hybrid is modelled *without* a remedy it could plausibly deploy | up to 2.4 % of dubcova3 DRAM; 0 on the other three |
| DIMM-level write turnaround, refresh | affects absolute cycles | not modelled in any arm |
| engine area / power / occupancy | economics of the accelerator | not modelled anywhere in M5–M19 |

Both SpMV arms are affected *symmetrically* by the first item and
*asymmetrically* by the second, which is why the second is called a limitation of
the conclusion rather than of the accounting.

### 6.3 Modelled but excluded from the reported metrics

* `bulk_win_bytes` (147 584 B) and `bulk_idx_bytes` (2 720 B) are storage charges,
  not traffic (I4).
* L2 evictions per client (`L2ev`) are modelled and printed but not tabulated in
  the M18 reports; they are what makes the interaction in §3 measurable.
* The duplicated index traffic inside `S_s` is modelled and disclosed as the
  double-charged bound (I12 and §1.3).
* `A(noidx)`'s 4 B/nnz undercharge (I12).

---

## 7. What may be carried forward — and what may not

**Valid for comparison (measured, equivalence-verified, correctly charged):**

* the four faithful ratios `S_f` = 1.182 / 1.047 / 1.236 / **1.180**(dubcova3
  corrected) at 2×8K, and 1.177 / 1.025 / 1.237 / **1.177** at 2×16K;
* the same-trace ratios as the *double-charged* bound, with that label;
* the 128-MSHR variants (1.175 / 1.020 / 1.220 / 1.122);
* the mechanism claims: the engine's index acquisition is 4.7–5.7× fewer requests
  for the same compulsory lines, and the hybrid's whole overhead is 44–89 % of
  what the index stream costs the SIMT baseline;
* the M18A lane-width decay, with its 4-lane endpoint restated as 1.180.

**Not valid, or valid only with the stated caveat:**

* any statement about *absolute* cycle counts as if `y` were written — the store
  path is absent (I3);
* the dubcova3 `unique_lines` / `unique_pages` figures in the M18 tables (I1);
* BFS absolute cycles from the M18 appendix (I2) — the "engine cannot represent
  BFS" conclusion stands;
* any claim that a real design's cache-policy mitigation is included — it is not
  (I7);
* extrapolation to wider GPUs (M18A scope, unchanged).

**M20 may proceed** on the SpMV comparison set above, with I1/I2 corrected numbers
and with I3/I7 carried as stated limitations. It must not treat the absolute
cycle counts as including output writeback.

---

## 8. Regression

Run after the M19 correction, from `DX100_MEM/sim`:

| check | result |
|---|---|
| `./bin/dx100_tests` | **4 470 checks, 0 failures — ALL CHECKS PASSED** |
| M17 anchor, κ = 0.25, 2×8K, seed 1 | **A 220 123 / B 138 924 / C 125 581 — identical**, `M9 GATES PASSED` |
| M18 SpMV cells (18 unaffected of 24) | traces byte-identical (§3 of the correction record); all published numbers reproduced (§2) |
| M18 dubcova3 + BFS cells | rebuilt and re-run; 10/10 `M9 GATES PASSED`, output hashes unchanged |
| historical `m18_*` raw data | **not modified** in this audit; corrected cells live in `m19_*` |

The synthetic workload path is untouched: M19 changed two `align()` expressions in
`tools/build_trace.py` and nothing else; `sim/src` is unmodified apart from the
print-only `m15` keys added and recorded in M18A.

---

## Reproduce

```
cd DX100_MEM
python3 tools/m19_recompute.py cells      # independent S_f / S_s for every cell
python3 tools/m19_recompute.py verify     # trace-derived vs simulator requests
python3 tools/m19_recompute.py streams build/wm/m18_bcsstk25_full.bin
python3 tools/m19_recompute.py integrity  # raw-log hashes + output/partition checks
cd sim && ./bin/dx100_tests
```
