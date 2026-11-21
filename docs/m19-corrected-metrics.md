# M19 — Corrected Metrics

This file exists because the audit found a **trace-layout defect**. It records the
defect, the minimal correction, the proof that the correction is inert for the
unaffected datasets, and the before/after numbers. **No historical `m18_*` file
was modified**: every corrected cell was rebuilt and re-run under an `m19_*` name,
so both readings stay available.

---

## 1. The defect

`tools/build_trace.py` laid out each trace by placing the payload array first and
sizing the gap after it with the *row-pointer* stride instead of the *payload*
stride. Two instances, one per workload family:

| # | code (M18) | payload span actually used | gap reserved | consequence |
|---|---|---|---|---|
| D1 | SpMV: `rowptr_base = align(m * 8)` | `x` = `cols * 8` | `m * 8` | for a **row prefix of a wider matrix** (`m < cols`) `rowptr` lands inside `x` |
| D2 | BFS: `rowptr_base = align(n * 4)` | `dist` = `n * 8` | `n * 4` | `rowptr` lands inside `dist` on **every** graph |

D1 never fired for the three square matrices (`m == cols`, regions exactly
adjacent) and always fired for dubcova3's 45 000-row prefix of a 146 689-column
matrix: **813 512 bytes of overlap**. D2 fired on all four BFS graphs.

Nature: an **addressing/accounting** defect, not a correctness one. Both arms of a
cell see the same addresses, the element counts and output hashes are unaffected,
and every gate passed. What it corrupts is *which lines a stream touches*: the
45 000 row-pointer instructions in dubcova3 (90 000 reads at 4 B) were served out
of `x` lines that the gather already keeps hot, instead of their own region. The
traces' `unique_lines` / `unique_pages` were therefore understated (316 377 →
319 190, 4 945 → 4 989) and the reported cycle counts slightly low.

## 2. The correction

One line per family, no other change:

```python
# SpMV
rowptr_base = align(max(m, n) * 8)   # was align(m * 8); x is sized by columns
# BFS
rowptr_base = align(n * 8)           # was align(n * 4); dist is 8 B per vertex
```

## 3. Proof of inertness for the unaffected datasets

For a square matrix `max(m, n) == m`, so the SpMV correction is a no-op. Rebuilding
the three square traces reproduces them **byte for byte** (the traces are hashed by
the builder over every `(address, instruction, lane)` record):

| dataset | `trace_sha256` before | after | verdict |
|---|---|---|---|
| bcsstk25 | `ffb82ff9…fff129` | identical | unchanged |
| qc2534 | `14d3b645…de8a306` | identical | unchanged |
| lnsp3937 | `8cc3d0ba…57dc19` | identical | unchanged |

Consequently **18 of the 24 reported SpMV cells are untouched** (the 6 dubcova3
cells are the rest) and their M18 numbers stand as published (verified
independently in §2 of the audit report).

For dubcova3 only `rowptr_base` moves (360 000 → 1 173 568, the first 64 B line
after `x`); `x_base`, `colidx_base`, `value_base` and `max_addr` are unchanged, as
are `accesses`, `instructions` and the application checksum (3 973 286.0).

## 4. Dubcova3 SpMV — corrected cells (window sweep, lanes 4, `page_sched=1`)

| window | arm | M18 | **M19 corrected** | Δ |
|---|---|---:|---:|---:|
| 2×8K | A(full) | 3 522 146 | **3 531 289** | +9 143 (+0.26 %) |
| 2×8K | A(noidx) | 2 461 123 | **2 474 298** | +13 175 (+0.54 %) |
| 2×8K | C(full) | 4 011 805 | **4 014 770** | +2 965 (+0.07 %) |
| 2×8K | C(noidx) | 2 988 123 | **2 992 191** | +4 068 (+0.14 %) |
| 2×16K | A(full) | 3 522 146 | **3 531 289** | +0.26 % |
| 2×16K | C(full) | 4 013 206 | **4 015 080** | +0.05 % |
| 2×16K | C(noidx) | 2 996 821 | **2 999 785** | +0.10 % |

| window | metric | M18 | **M19 corrected** | Δ |
|---|---|---:|---:|---:|
| 2×8K | **S_faithful** = A(full)/C(noidx) | 1.179 | **1.180** | +0.001 |
| 2×8K | S_same = A(full)/C(full) | 0.878 | **0.880** | +0.002 |
| 2×16K | **S_faithful** | 1.175 | **1.177** | +0.002 |
| 2×16K | S_same | 0.878 | **0.880** | +0.002 |

Other counters (2×8K): A DRAM 382 419 → 383 142 (+0.19 %), C(full) DRAM 481 726 →
481 993 (+0.06 %), A walks 5 963 → 5 993, C(full) walks 7 640 → 7 653,
A requests **unchanged** at 1 948 034 (a request is issued per distinct line per
instruction; only which lines changed), C(full) requests 1 336 226 → 1 336 235.

All four corrected cells report `M9 GATES PASSED`; the output check is
`value hash 3 973 286.0 vs expectation 3 973 286.0, mismatched addresses 0`, the
same as M18.

**Reading:** the correction moves dubcova3's headline speedup by **+0.001** and its
same-trace ratio by +0.002. No M18 conclusion depends on a difference that small;
the corrected value is the one to quote from now on.

## 5. BFS — corrected cells (2×8K, all-SIMT observations)

| graph | variant | M18 cycles | **M19 corrected** | Δ |
|---|---|---:|---:|---:|
| facebook_combined | full | 142 264 | **143 187** | +0.65 % |
| facebook_combined | noprobe | 113 931 | **114 019** | +0.08 % |
| email-Enron | full | 376 124 | **385 363** | +2.46 % |
| email-Enron | noprobe | 277 115 | **278 582** | +0.53 % |
| wiki-Vote | full | 176 395 | **178 015** | +0.92 % |
| wiki-Vote | noprobe | — | **132 594** | new |

The M18 BFS appendix's derived quantity — the share of runtime held by the
`dist` probe, i.e. an upper bound on what a one-level engine could ever save:

| graph | M18 probe share | **M19 corrected** |
|---|---:|---:|
| facebook_combined | 19.9 % | **20.4 %** |
| email-Enron | 26.3 % | **27.7 %** |
| wiki-Vote | — | **25.5 %** |

BFS's conclusion is unaffected and if anything slightly strengthened: the stream
the engine cannot consume is a fifth to a quarter of the runtime, and the engine
still cannot represent the chained gather (`indirection_levels` in the sidecar).

`build/wm/m19_bfs_soc-Epinions1.*` is *not* re-run: the M18 probe fails its own
descriptor validation (`bulk_data_base overlaps the index array`) and was never a
reported cell.

## 6. Cells affected in total

| family | reported cells affected | corrected files |
|---|---|---|
| SpMV dubcova3 | 4 (2 windows × 2 variants) + 2 `mshr128` | `m19_dubcova3_*` |
| BFS | 6 (3 graphs × full/noprobe) | `m19_bfs_*` |
| everything else | **0** | traces byte-identical |

The M18A lane sweep's dubcova3 cells were built with the pre-correction builder and
therefore carry D1 as well; its 4-lane row is the corrected number now (1.180), and
its 8/16/32-lane dubcova3 points inherit a defect of the same size (≤0.3 %), which
does not disturb the monotone decay the sweep found.

## Reproduce

```
cd DX100_MEM
python3 tools/build_trace.py spmv --mtx ref/vortex/tests/opencl/spmv/Dubcova3.mtx \
    --max-rows 45000 --variant full  --lanes 4 --out build/wm/m19_dubcova3_full.bin
python3 tools/build_trace.py spmv --mtx ref/vortex/tests/opencl/spmv/Dubcova3.mtx \
    --max-rows 45000 --variant noidx --lanes 4 --out build/wm/m19_dubcova3_noidx.bin
for w in 8192 16384; do for v in full noidx; do
  ./sim/build/wm/m18_run.sh build/wm/m19_dubcova3_$v.bin $w m19_dubcova3_${v}_w$w \
      > build/wm/m19_dubcova3_${v}_w$w.txt 2>&1
done; done
for g in facebook_combined email-Enron wiki-Vote; do for v in full noprobe; do
  python3 tools/build_trace.py bfs --edges data/$g.txt.gz --variant $v \
      --out build/wm/m19_bfs_$g$([ $v = noprobe ] && echo _noprobe).bin
done; done
```
