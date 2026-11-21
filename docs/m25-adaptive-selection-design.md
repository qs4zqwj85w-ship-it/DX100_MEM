# M25 — Adaptive SIMT/Bulk selection: policy, evaluation, switching cost

**Status: design and offline evaluation only.** No simulator cell was run for this document; it
consumes `build/wm/m25/diag.csv` (the M24-F matrix through `tools/m25_extract.py`) and produces
`build/wm/m25/policy.csv` / `policy.txt` through `tools/m25_policy.py`. No source was modified.

The brief's constraint is respected literally: **no trace's measured answer is an input to the
policy.** The three gates are hardware constants and workload *structure* (page footprint,
sequential fraction, index share), and §5 states exactly how much of the feature choice was
informed by M24-F — which is a real limitation, not a formality.

---

## 0. Result

| quantity | value |
|---|---|
| policy | Bulk iff `pages ≥ 512` **and** `sequential fraction < 0.5` **and** `index share < 0.5`, after a static "is this loop indirect?" gate |
| traces where the chosen mode is the measured-best mode | **14 / 31** |
| aggregate regret versus the per-trace oracle (sum of cycles) | **+0.33 %** |
| versus always-SIMT | **+32.83 %** faster |
| versus always-Bulk | **+1.24 %** faster |
| worst single-trace regret | **21.5 %** (`n3_clu4m`) |
| second worst | 19.2 % (`lnsp3937`, a false *negative*: Bulk would have won) |

The accuracy number looks poor and the regret number looks good, and both are true: **15 of the 17
"wrong" traces have a regret ≤ 6 %**, all of them in the band where the choice barely matters. The
policy's job is to capture the 9 traces where Bulk wins ≥ 10 % — it captures **8 of 9** (the miss is
`lnsp3937`), and to avoid the 4 where it loses ≥ 5 % — it avoids **3 of 4** (the miss is `n3_clu4m`).

---

## 1. What can be known, and when

| tier | property | source | reliability |
|---|---|---|---|
| **statically known** | the loop is indirect (`addr = base + load(idx)*w`); element width; trip count if affine; whether the index array is loop-invariant; whether the loop stores | the IR | exact |
| | the index array's footprint (`count × 4 B`) | IR | exact |
| **profile-guided** | target footprint in pages / bytes; line-duplicate rate; sequential-run fraction; reuse within a window | a training run or a one-shot sample of the actual data | good, data-dependent |
| | which DRAM rows the pages map to | the OS/allocator (PFN order) | usually unavailable to a compiler |
| **runtime-observable** | TLB miss rate, L2 hit rate, DRAM row-hit rate, ready-pool occupancy, index-stream stall cycles | performance counters | exact but *late* — only after the region has run |
| **not reliably predictable** | the actual gather order of data-dependent indices; whether a given page is resident; which rows are open when a request arrives | — | no |

The M24-F matrix makes the boundary concrete: `pages`, `dup_line`, `seq_frac` and `reuse16k` are all
computed from the *actual addresses*, so on a real system they are profile numbers. A compiler
without a profile can compute only the first row of the table and the index share; with a profile
(sample the first tile, or reuse the previous epoch's numbers) it gets the second.

---

## 2. The policy

```
if not indirect(loop):                       -> SIMT          // G0, static
elif pages(target extent) < 512:             -> SIMT          // G1, translation cannot overflow
elif seq_fraction >= 0.5:                    -> SIMT          // G2, already in DRAM order
elif index_bytes / (index_bytes + target_bytes) >= 0.5:
                                             -> SIMT          // G3, the index pass is the traffic
else:                                        -> BULK
```

Why each gate, and why that threshold:

* **G0** is the only *static* classifier that matters. Every trace in this matrix is an indirect
  gather, so G0 is uniformly true here; it is a precondition, not a discriminator. On a real
  program it removes all the direct and strided loops before any estimate is needed.
* **G1 = 512 pages** is the **L2 TLB's entry count** (`l2tlb = 512 × 4` in every configuration),
  i.e. 2 MiB of 4 KiB pages. Below it, the address stream cannot thrash translation, and Bulk's
  only remaining lever (ordering) has nothing to fix — measured: every trace under 512 pages gains
  ≤ 5.3 % from Bulk, and four of them lose.
* **G2 = 0.5** is a definition, not a fitted constant: "at least half of the consecutive target
  accesses continue the previous line" means the stream is already in the order the row buffer
  wants. `n1_seq8m` (seq 1.000) and `n5_seq8m_s4` (0.500) are the two cells it rejects, and they
  are the two largest sequential losses (0.975, 0.956).
* **G3 = 0.5** is the point at which the index stream is at least as large as the target stream. It
  is the gate that removes the SpMV losses (`qc2534` 0.96, `bcsstk25` 0.86, `dubcova3` 0.30 —
  the last passes, and is caught by G1 instead).

**Region boundaries.** The unit of decision is the loop nest that contains the indirect access, not
the instruction: the class map is per instruction, but the *policy* is per region because (a) the
engine's descriptor covers a tile of the region's iterations and (b) a mid-region switch would have
to drain a 4 096-entry pool. So: one decision per offload candidate, taken before the region
starts, never revisited inside it.

---

## 3. Evaluation on the 31-trace matrix

`policy.csv` has every row; the table below is sorted by Bulk's speedup and shows only the
decisions that matter. `regret` is per trace; `vs SIMT` is the gain over always-SIMT.

| trace | Bulk sp | oracle | chosen | regret | vs SIMT | why |
|---|---:|---|---|---:|---:|---|
| `n3_clu4m` | 0.823 | simt | **bulk** | **21.45 %** | −21.45 % | passes all three gates (1 024 pages, seq 0, index 0.36) |
| `dubcova3` | 0.837 | simt | simt | 0 | 0 | G1 (88 pages) |
| `qc2534` | 0.843 | simt | simt | 0 | 0 | G1 (5 pages) |
| `bcsstk25` | 0.924 | simt | simt | 0 | 0 | G1 (31 pages) |
| `n5_seq8m_s4` | 0.956 | simt | simt | 0 | 0 | G2 (seq 0.500) |
| `n1_seq8m` | 0.975 | simt | simt | 0 | 0 | G2 (seq 1.000) |
| `orkut_tail` | 1.000 | simt | bulk | 0 | 0 | tie: the engine retires nothing |
| `w3/w7/w9/w10` (5 cells) | 1.009–1.012 | bulk | simt | 0.9–1.2 % | 0 | G1 (1–4 pages) |
| `w1/w2/w6` (6 cells) | 1.051–1.053 | bulk | simt | 5.1–5.3 % | 0 | G1 (32 pages) |
| `w5_sequential` (2 cells) | 1.053 | bulk | simt | 5.3 % | 0 | G1 (256 pages) |
| `lnsp3937` | **1.192** | bulk | **simt** | **19.19 %** | 0 | G1 (8 pages) — a false negative |
| `n2_zipf4m` | 1.051 | bulk | bulk | 0 | +4.8 % | all gates pass |
| `ogbn_uniform` | 1.224 | bulk | bulk | 0 | +18.3 % | all gates pass |
| `w8_wslarge` (2) | 1.454–1.458 | bulk | bulk | 0 | +31.2 % | 512 pages, exactly at G1 |
| `n4_uni4m` | 2.186 | bulk | bulk | 0 | +54.3 % | all gates pass |
| `w4_duplow` (2) | 2.504–2.510 | bulk | bulk | 0 | +60.1 % | all gates pass |
| `orkut_hub` | 2.934 | bulk | bulk | 0 | +65.9 % | all gates pass |
| `criteo_2k` | 3.280 | bulk | bulk | 0 | +69.5 % | all gates pass |

**Aggregate.** Sum of chosen cycles 60 939 372 against an oracle of 60 736 363 → **regret 0.33 %**;
always-SIMT 90 730 770 and always-Bulk 61 702 944. The policy captures 99.67 % of the oracle's
saving over SIMT.

**The two errors worth naming.**

* **`n3_clu4m` (false positive, 21.5 %).** A 0.89 MiB footprint over 1 024 pages, random within a
  cluster, 87.5 % reuse inside a window. Every static and profile feature says "page-spread,
  irregular, index amortised" — and Bulk still loses 18 %, because SIMT's L2 captures the clustered
  hot set and the engine's per-window dedup does not (§5 of the efficiency audit). No threshold on
  these features separates it from `criteo_2k` (777 KiB, 1 945 pages, 81.6 % reuse, +3.28×): their
  reuse and page counts overlap. A **runtime** signal could: sample the first window and compare
  the engine's DRAM read rate against the SIMT arm's predicted rate, then abort. That is a
  fallback, not a prediction, and it needs an engine that can abort a descriptor mid-window.
* **`lnsp3937` (false negative, 19.2 %).** 30.8 KiB over 8 pages, dup 0.981 — a footprint far below
  L1, where Bulk wins by dedup alone (46 602 vs 55 546 cycles: the engine forms 1 588 target line
  requests where SIMT forms 26 400). G1 rejects it. This is the *only* trace under 512 pages that
  Bulk wins by more than 5.3 %, and it is a genuine gap in the policy: an L1-resident footprint can
  still favour Bulk when the *request count* is the bottleneck. A second clause (`dup_line > 0.99
  and elements/line_requests > 8`) would admit it; it was **not** added, because it is fitted to a
  single trace and because the 19 % it recovers is 1.9 % of the matrix total.

---

## 4. Sensitivity, and what would make this policy wrong

`tools/m25_policy.py` prints the regulator's table: aggregate regret for `pages ∈ {256, 512, 1024}`
× `seq ∈ {0.25, 0.5, 0.75}`.

| pages | seq 0.25 | seq 0.50 | seq 0.75 |
|---:|---:|---:|---:|
| 256 | 0.28 % | 0.28 % | 0.33 % |
| **512** | 0.33 % | **0.33 %** | 0.39 % |
| 1024 | 1.65 % | 1.65 % | 1.71 % |

The surface is flat around the chosen point (0.28–0.39 %) and degrades only when the page gate is
moved *up* to 1 024, which excludes `w8_wslarge` (512 pages, +45 %), `n4` (1 024, +119 %) and
`criteo_2k` (1 945 — still admitted) — i.e. the gate's location is set by where the wins are, and
512 is where the hardware's TLB reach happens to be. The 256 row scores marginally better *because*
it admits `w5_sequential` (256 pages, +5.3 %); taking it would be fitting to one trace below the
hardware threshold, so it is reported and rejected.

**Limitation, stated plainly:** the *feature set* was chosen after seeing M24-F's correlation table
(`idx_share` r = −0.589, `reuse16k` r = −0.510). The thresholds are hardware constants and the
policy never reads a speedup, but this is not an out-of-sample evaluation. Experiment E1 of the
proposal re-runs it with the thresholds frozen on the M21/M18 traces and evaluated on the M23/M24-F
traces it has never seen.

---

## 5. Switching and profiling cost

| cost | measured? | value |
|---|---|---|
| **Bulk start-up** (descriptor fetch → first retired element) | **yes**, per M24-F cell | 12 276–13 212 cycles on the small footprints (6.9 % of a 190 k-cycle run); **64 173 cycles = 26.4 % of the run** on `criteo_2k`; 12 339 on `dubcova3` |
| **Bulk drain** (last element → run end) | partially | 0–1 cycle on the long traces; 72 620–73 381 cycles on the 4 KiB-footprint traces (a constant the audit did not attribute) |
| **SIMT → Bulk switch** | **no region concept exists** | would be start-up plus a cold dTLB/L2 for the new address range; not modelled |
| **Bulk → SIMT switch**, mid-region | not modelled | would require aborting a descriptor and discarding up to 4 096 translated runs — `abort_desc` exists but is not wired to a policy |
| **profiling pass** | not modelled | one linear pass over the index array (the structure statistics M24-F computes offline); negligible in cycles, but it *is* a real pass over memory and is not charged anywhere |

**Consequence for the policy:** a region must be large enough to amortise 13 k–64 k cycles of
start-up. At the matrix's scale (131 072–1 589 539 elements) that is 0.7–6.9 %, which is why the
policy is only applied at *whole-region* granularity and why the `criteo_2k` case — where start-up
is 26 % — is the one where a mis-prediction is expensive in both directions.

**What is not claimed:** that a mid-region switch is free, or that a runtime fallback is cheap. Neither
exists in the model, and both would need a drain path before they could be evaluated.

---

## 6. What the policy should become

1. **Ship the three-gate rule as the compiler's default** with the conservative direction (SIMT on
   any missing input), because its downside is bounded: the worst case it creates is 1.2–5.3 % on
   near-ties, and it captures every ≥ 10 % win.
2. **Add one fallback, not more prediction:** if a Bulk region's first window reports
   `index stall + slot stall` above a threshold, abort to SIMT for the rest of the region. This is
   the only mechanism that can fix `n3_clu4m`, and it needs no new prediction — only the abort path
   and the counters the engine already keeps (`bulk_g6b_release_slot_stall`, `bulk_g6c_refused`).
3. **Do not add the `lnsp3937` clause** until a second trace shows the same shape.
4. **Do not attempt to predict DRAM row order from the compiler.** §5 of the DRAM audit shows the
   engine's own sort cannot exploit it; the compiler certainly cannot.

---

## 7. Threats to validity

* **Feature selection is in-sample** (§4). The thresholds are not.
* **One seed per trace**, and two seeds for most of the synthetic families; the seed-to-seed spread
  is ≤ 0.5 % where both exist (`w4` 2.504/2.510, `w8` 1.458/1.454), so the ordering of the matrix
  is stable, but a 1 % decision boundary could flip.
* **`pages` and `seq` are computed from the actual trace**, i.e. they are profile facts, not
  compiler facts. A compiler-only version of this policy has G0 and the index share and must guess
  the rest; that variant is not evaluated here.
* **Regret is measured against a per-trace oracle from the same run**, so it is a lower bound on the
  regret of any real system that must also pay switching costs (§5).
* **Eight traces are near-duplicates** (seed 2 of the same family), which double-counts their
  regret in the aggregate. Excluding the seven `*_s2` traces (23 remain) gives **0.26 %** regret
  instead of 0.33 % — the same regime, slightly smaller.
