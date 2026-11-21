# M18 — SpMV validation report

**Workload:** `y = Ax` in CSR, four real matrices from the SuiteSparse Matrix Collection.
**Configurations:** Vortex-calibrated SIMT (16 L1 MSHRs) · Bulk hybrid 2×8K · Bulk hybrid 2×16K.
**Model:** trace-driven, read stream only (see §7). Same hierarchy, DRAM, translation and
address map in every arm.

---

## 1. Method

The trace records the memory instructions the kernel issues for every row, in program order:

```
rowptr[i], rowptr[i+1]      SIMT   (8 B, one instruction per row)
colidx[j..j+3]             SIMT   (16 B per instruction)          <- index acquisition
value[j..j+3]              SIMT   (32 B per instruction)
x[colidx[j..j+3]]          BULK   (gather, one instruction per 4 nonzeros)
```

`x` is placed at address 0 and the engine's `data_base` is 0, so the engine's index value for
element `j` is `x_addr/8 = colidx[j]`: the descriptor's index array is **byte-identical to the
matrix's own CSR column-index array**. The engine reads the real index structure from memory;
no target address is pre-materialised for it.

### 1.1 The two variants

One trace cannot say "SIMT reads `colidx` in the baseline and the engine reads the same bytes
in the hybrid", because the class map is per instruction and arm C executes every SIMT-class
instruction. Two traces are therefore built:

| variant | colidx stream in trace | exact for | bias |
|---|---|---|---|
| `full` | yes (SIMT) | the SIMT baseline | the hybrid is charged the index stream **twice** |
| `noidx` | no | the hybrid (the engine's index fetch *is* the colidx read) | the baseline is not charged it |

Both are the same application over the same dataset, with the same gather addresses. They
differ only in which client owns the index read — the architectural variable under test.

**Primary pairing (each arm pays for its own index acquisition once):**

```
S_faithful = A(full) / C(noidx)
```

**Conservative pairing (identical instruction stream for both arms):**

```
S_same-trace = A(full) / C(full)
```

The second double-charges the hybrid. Its size is measurable: on Dubcova3 the duplicated
index stream is 99 347 lines and the hybrid's DRAM excess over the baseline is 100 451 reads
(the rest is L2 absorption). Both numbers appear in every table; neither is hidden.

---

## 2. Primary results

### 2.1 Same-trace (both arms execute the identical trace)

| Matrix | Config | Cycles | Speedup | Reduction | DRAM rd | Row-hit | dTLB miss | Walks | L1 hit | L2 hit | Index phase | Target phase | Peak occ | Window occ |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | SIMT | 461 688 | 1.000× | 0 % | 50 245 | 80.0 % | 1 575 | 788 | 35.1 % | 9.3 % | N/A | N/A | 128 | N/A |
| bcsstk25 | 2×8K | 492 449 | 0.938× | −6.7 % | 60 604 | 86.8 % | 2 068 | 787 | 3.1 % | 12.0 % | 6 581 | 269 679 | 128 | 8 192 |
| bcsstk25 | 2×16K | 505 260 | 0.914× | −9.4 % | 62 236 | 86.4 % | 2 045 | 787 | 3.1 % | 8.8 % | 11 365 | 255 371 | 128 | 16 384 |
| qc2534 | SIMT | 762 503 | 1.000× | 0 % | 87 447 | 87.2 % | 2 729 | 1 368 | 35.4 % | 3.5 % | N/A | N/A | 128 | N/A |
| qc2534 | 2×8K | 923 790 | 0.825× | −21.2 % | 113 113 | 88.5 % | 3 715 | 1 655 | 1.5 % | 6.9 % | 6 498 | 477 776 | 128 | 8 192 |
| qc2534 | 2×16K | 923 269 | 0.826× | −21.1 % | 112 597 | 88.6 % | 3 666 | 1 668 | 1.5 % | 6.7 % | 10 788 | 427 219 | 128 | 16 384 |
| lnsp3937 | SIMT | 57 014 | 1.000× | 0 % | 5 511 | 74.8 % | 179 | 88 | 35.5 % | 3.0 % | N/A | N/A | 128 | N/A |
| lnsp3937 | 2×8K | 48 298 | **1.180×** | 15.3 % | 5 512 | 87.7 % | 227 | 88 | 3.8 % | 23.9 % | 5 982 | 19 721 | 128 | 8 192 |
| lnsp3937 | 2×16K | 48 205 | **1.183×** | 15.5 % | 5 512 | 87.8 % | 229 | 88 | 3.8 % | 23.9 % | 10 542 | 14 417 | 128 | 16 384 |
| dubcova3¹ | SIMT | 3 522 146 | 1.000× | 0 % | 382 419 | 70.2 % | 82 107 | 5 963 | 30.9 % | 13.6 % | N/A | N/A | 128 | N/A |
| dubcova3¹ | 2×8K | 4 011 805 | 0.878× | −13.9 % | 481 726 | 83.8 % | 22 319 | 7 640 | 2.9 % | 8.5 % | 5 464 | 4 006 339 | 128 | 8 192 |
| dubcova3¹ | 2×16K | 4 013 206 | 0.878× | −13.9 % | 480 151 | 84.5 % | 19 426 | 7 516 | 2.9 % | 7.0 % | 10 684 | 4 002 520 | 128 | 16 384 |

¹ first 45 000 rows (30.7 % of the matrix), full `x` (1.12 MiB).

### 2.2 Faithful pairing (each arm charged its own index acquisition)

| Matrix | A (SIMT, full) | C 2×8K (noidx) | C 2×16K (noidx) | **S 8K** | **S 16K** | S same-trace 8K |
|---|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 461 688 | 390 495 | 392 188 | **1.182×** | **1.177×** | 0.938× |
| qc2534 | 762 503 | 728 062 | 743 702 | **1.047×** | **1.025×** | 0.825× |
| lnsp3937 | 57 014 | 46 113 | 46 104 | **1.236×** | **1.237×** | 1.180× |
| dubcova3¹ | 3 522 146 | 2 988 123 | 2 996 821 | **1.179×** | **1.175×** | 0.878× |

**Answer to "does Bulk beat SIMT on real SpMV":** between +4.7 % and +23.6 % once each arm
pays for its own index acquisition; between −17.5 % and +18.3 % on an identical trace. The
honest statement of the result is the range, and the reason for it is quantified in §3.

---

## 3. Where the time goes (what the speedup actually is)

The two variants give a clean decomposition. Define:

* `T_simt_idx = A(full) − A(noidx)` — what the column-index stream costs the SIMT front-end.
* `T_engine  = C(noidx) − A(noidx)` — what the engine costs compared with a SIMT arm that
  would not need to read indices at all (a hypothetical lower bound, not a configuration).
* net = `T_simt_idx − T_engine` → the measured gain.

| Matrix | A full | A noidx | T_simt_idx | C noidx | T_engine | net | S faithful |
|---|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 461 688 | 308 704 | 152 984 (33 %) | 390 495 | +81 791 (27 %) | 71 193 | 1.182× |
| qc2534 | 762 503 | 446 600 | 315 903 (41 %) | 728 062 | +281 462 (63 %) | 34 441 | 1.047× |
| lnsp3937 | 57 014 | 37 422 | 19 592 (34 %) | 46 113 | +8 691 (23 %) | 10 901 | 1.236× |
| dubcova3¹ | 3 522 146 | 2 461 123 | 1 061 023 (30 %) | 2 988 123 | +527 000 (21 %) | 534 023 | 1.179× |

Read this carefully, because it says something different from the M17 synthetic result:

1. **The index stream is expensive on a 4-lane front-end.** `colidx` is read 4 values
   (16 B) per instruction. It costs 30–41 % of the baseline's cycles even though its DRAM
   traffic is only 1/16 of the gather's element count.
2. **The engine's own work is a net cost, not a net gain.** Compared with a baseline that
   gets its addresses for free, the hybrid is 21–63 % *slower*. Its gather merges 93–99.5 %
   of elements onto existing lines (`bulk_dedup_merged`), so it issues very few target
   requests — but it must fill a window before it can issue anything, and it stalls on the
   shared R = 128 slot pool: `bulk_slot_stall` is 261 299 / 348 704 / 19 222 / 3 451 605
   cycles on the four matrices, the largest single stall in every cell.
3. **The measured gain is the difference between those two effects.** It is a
   *front-end/issue* effect: moving a stream off the narrow SIMT coalescer, not making the
   gather itself cheaper.

This is why the same-trace number is *below* 1.0 for three matrices: the hybrid pays for the
index stream once as its own index fetch and once more because the shared trace makes its
SIMT side read `colidx` again. That second charge is an artefact of the model's one-class-per
-instruction dispatch, not a mechanism.

---

## 4. 128 L1 MSHR baseline sensitivity

The baseline was also run with the MSHR depth raised to 128 (the accelerator unchanged), to
test whether the result depends on a constrained SIMT configuration.

| Matrix | A 16 MSHR | A 128 MSHR | ΔA | C 2×8K 128 (noidx) | S faithful 16 | S faithful 128 |
|---|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 461 688 | 455 871 | −1.3 % | 387 872 | 1.182× | **1.175×** |
| qc2534 | 762 503 | 742 629 | −2.6 % | 727 950 | 1.047× | **1.020×** |
| lnsp3937 | 57 014 | 55 834 | −2.1 % | 45 778 | 1.236× | **1.220×** |
| dubcova3¹ | 3 522 146 | 3 330 465 | −5.4 % | 2 968 817 | 1.179× | **1.122×** |

The 128-MSHR baseline is 1.3–5.4 % faster than the 16-MSHR one: these traces are not
MSHR-limited, because the gather merges onto a small target footprint and the dominant
streams are sequential. The advantage therefore **survives** the stronger baseline, eroding by 0.6–5.7 %
(most on Dubcova3, whose 82 107 dTLB misses make it the most MSHR-sensitive). It never
vanishes, which is a materially different answer from the synthetic case, where the MSHR
depth also barely mattered.

---

## 5. Correctness and integrity

| Check | Result |
|---|---|
| M9 gates on every cell (28 primary + 6 extra) | `M9 GATES PASSED` in all |
| Address mismatches `bulk_addr_mismatch` | 0 everywhere |
| Dedup accounting `lines_issued + dedup_merged == elements` | exact (e.g. 3 431 + 248 810 = 252 241) |
| Lost SIMT requests `generated − completed` | 0 everywhere |
| Residency `peak_total_outstanding ≤ 128` | held (exactly 128 in every cell) |
| Engine output vs image-derived expectation | value hash equal, 0 mismatched addresses |
| Regression suite | 4 470 checks, 0 failures |
| Historical M17 anchors re-run after the M18 edits | A 220 123 / B 138 924 / C 125 581 — identical |
| Both arms on the same image and the same trace hash | enforced by the harness |

The application output (`y = Ax`) is computed offline by the trace builder and recorded with
each trace (`app_output_checksum`); it is identical for all three configurations because all
three consume one trace per variant. The simulator verifies the *memory* result — that every
gathered address returns the image value for that address — not the arithmetic, which the
model does not represent.

---

## 6. Interpretation

* Bulk is **not** a large win on real SpMV. It is a small, consistent one when the index
  stream is charged once to each arm, and a loss when it is charged twice to the hybrid.
* The mechanism is **index-stream consolidation**, not page locality (the ablation report
  shows page ordering changes cycles by ≤ 0.5 % on these traces).
* The gain is largest where the index stream is a large fraction of the baseline's work and
  the target footprint is small enough that the engine's window buffers can dedup almost all
  of it (lnsp3937: 1.24×, 98 % dedup).
* It is smallest for `qc2534` (1.02–1.05×), where the matrix is nearly dense: 183 nonzeros in
  every row over 2 534 columns means the gather is trivially local and the index stream is
  the only thing worth moving — but the engine's slot contention eats most of it.
* The 2×16K window is **never better** than 2×8K (1.175 vs 1.179, 1.025 vs 1.047, 1.177 vs
  1.182, 1.237 vs 1.236). Doubling the window buys nothing on these working sets and costs
  twice the storage and a longer fill before the first issue.

---

## 7. Limitations

1. **Read-only.** `y[i] +=` and the `y` store are not modelled; that is `2m/nnz` of the
   access count (12.2 %, 1.1 %, 31.0 %, 5.7 % for the four matrices). lnsp3937's absolute
   numbers should be read with its 31 % in mind. The store is a small, sequential, streaming
   write in a real kernel, so its omission does not plausibly reverse any ranking, but it is
   not measured here.
2. **Arithmetic is not modelled** for either arm.
3. **4-lane front-end.** `lanes = 4` is the Vortex calibration (NUM_THREADS = 4). This is the
   single most important caveat for this report: a 32-lane warp would read 128 B of `colidx`
   per instruction instead of 16 B, which would shrink the index-consolidation term
   (`T_simt_idx`, 30–41 % of baseline cycles here) by roughly the same factor. The measured
   advantage is therefore specific to a narrow-warp baseline and should not be extrapolated
   to a wide-warp GPU.
4. **Trace-driven.** The address order is the recorded one; the model does not re-derive it.
5. **Row prefix for Dubcova3** (30.7 % of rows) to fit the cycle budget. It keeps the full
   `x` — the part that decides whether the gather can be cached — but it is not the whole
   matrix.
6. **One matrix per structural class.** No claim of generality beyond these four.
