# M18A — Execution Equivalence of the `full` and `noidx` Traces

**Scope.** M18 compared two traces of the same SpMV problem: `full` (the whole
program, including the `colidx` index loads) and `noidx` (the same program with
the `colidx` loads removed, so that only the Bulk engine's descriptor fetch reads
those bytes). This report establishes, mechanically and for all four datasets,
that the two traces describe *the same logical computation* — identical gather
targets, identical index values, identical output, identical address mapping —
so that the difference between the two runs is attributable to *who fetches the
index stream*, and to nothing else.

Everything below is produced by `DX100_MEM/tools/m18a_audit.py equiv`, which
reads the trace binaries and their `.json` sidecars. No simulator source was
changed to produce it.

```
python3 tools/m18a_audit.py equiv <name> \
    --full  build/wm/m18_<name>_full.bin   --noidx build/wm/m18_<name>_noidx.bin \
    --mtx   <matrix> [--max-rows 45000]
```

---

## 1. Trace-level identity: `full` minus `colidx` is exactly `noidx`

The trace format (`M18TRC1`) is a flat sequence of `(address, instruction, lane)`
records. Removing a stream from a trace renumbers instructions positionally (a
removed instruction no longer occupies an id), so a naive tuple comparison would
report a spurious mismatch. The audit therefore compares records on
`(address, renumbered instruction, lane)`, where the renumbering is the order of
first appearance — i.e. it compares the *instruction structure* as well as the
addresses, not just the flat address sequence.

| dataset | `colidx`-region accesses, `full` | in `noidx` | `full` − `colidx` == `noidx` |
|---|---:|---:|---|
| bcsstk25 | 252 241 | 0 | **YES** |
| qc2534 | 463 360 | 0 | **YES** |
| lnsp3937 | 25 407 | 0 | **YES** |
| dubcova3 (45 000-row prefix) | 1 589 539 | 0 | **YES** |

The `colidx` region is `[colidx_base, value_base)` from the layout block of each
sidecar, so this is a *region* test, not a name test: every access at an address
inside the index array belongs to the index stream and nothing else does.

Consequences that the M18 accounting depends on:

* every non-index access in `full` survives in `noidx` with the same address,
  the same instruction grouping and the same lane slot;
* the `noidx` trace contains no access to the index array from the SIMT side;
* therefore `A_noidx` is *exactly* `A_full` with the index-load instructions
  deleted — no other instruction, address or ordering changes.

Instruction counts confirm the same decomposition at the trace level
(`instructions_simt | instructions_bulk`):

| dataset | variant | accesses | SIMT instr | Bulk instr | unique lines | pages |
|---|---|---:|---:|---:|---:|---:|
| bcsstk25 | full | 787 601 | 151 499 | 68 030 | 50 192 | 786 |
| bcsstk25 | noidx | 535 360 | 83 469 | 68 030 | 34 426 | 539 |
| qc2534 | full | 1 395 148 | 235 662 | 116 564 | 87 356 | 1 366 |
| qc2534 | noidx | 931 788 | 119 098 | 116 564 | 58 396 | 914 |
| lnsp3937 | full | 84 095 | 19 955 | 8 009 | 5 504 | 88 |
| lnsp3937 | noidx | 58 688 | 11 946 | 8 009 | 3 916 | 63 |
| dubcova3 | full | 4 858 617 | 864 468 | 409 734 | 316 377 | 4 945 |
| dubcova3 | noidx | 3 269 078 | 454 734 | 409 734 | 217 030 | 3 393 |

`instructions_bulk` is **identical in both variants for all four datasets**. The
compiler-directed dispatch is a property of the trace (which accesses are
gather-class), not an artefact of the accounting; removing the index stream from
the SIMT side does not move a single gather into or out of the engine's work set.

## 2. Index-value identity: the descriptor equals the CSR `colidx` array

The engine does not receive index *values* explicitly. It walks the descriptor's
index array as memory: for element `j` it reads `index[j]` from
`index_base + 4j` and forms the gather address `data_base + index[j] * 8`
(`bulk.cpp:660-704`). The audit recovers the value the engine actually read from
the *address the engine actually generated*, `index[j] = addr/8` (with
`data_base = 0`, `elem_bytes = 8`, `x_base = 0`), and compares the recovered
array against the CSR `colidx` array rebuilt from the matrix file with the same
symmetric expansion the trace builder used.

| dataset | nnz (expanded) | CSR `colidx` entries | decoded index array == CSR `colidx` |
|---|---:|---:|---|
| bcsstk25 | 252 241 | 252 241 | **YES** (first 4: 0, 1, 3, 5) |
| qc2534 | 463 360 | 463 360 | **YES** (first 4: 0, 1, 2, 3) |
| lnsp3937 | 25 407 | 25 407 | **YES** (first 4: 0, 1, 2, 82) |
| dubcova3 | 1 589 539 | 1 589 539 | **YES** (first 4: 0, 4 394, 4 395, 7 078) |

This closes the loop on the "uncharged precomputed path" question at the level of
*values*: the index values the engine consumes are the logical workload's own
column indices, in program order, one per element, with no substitution and no
re-sorting. (Section 3 of the accounting audit treats the *fetch* path itself.)

Note on scale: bcsstk25 and qc2534 are stored as one triangle and expanded by the
trace builder (`nnz_stored` 133 840 → `nnz_full` 252 241, and 232 947 → 463 360);
lnsp3937 is full-rank (`nnz_stored == nnz_full`); dubcova3's prefix uses
`nnz_stored` 1 891 669 → `nnz_full` 1 589 539 for the retained 45 000 rows. The
decoded array matches the *expanded* array in every case, which is the array the
program actually gathers.

## 3. Output identity

The application output is recomputed **from the decoded index values alone**
(`y = Σ_j x[colidx[j]]`, `x[i] = 1 + (i mod 7)/2`) and compared with the checksum
the trace builder recorded while emitting the trace:

| dataset | output from decoded indices | recorded by builder | match |
|---|---:|---:|---|
| bcsstk25 | 630 661.5 | 630 661.5 | **YES** |
| qc2534 | 1 158 400.0 | 1 158 400.0 | **YES** |
| lnsp3937 | 63 521.5 | 63 521.5 | **YES** |
| dubcova3 | 3 973 286.0 | 3 973 286.0 | **YES** |

The `full` and `noidx` sidecars carry the *same* `app_output_checksum` for each
dataset (the trace-level assertion that the two variants compute the same thing),
and the two `.bin` files differ in `trace_sha256` only because the index stream
is present in one and absent in the other.

## 4. Simulator-side invariants

The run itself asserts element-level completeness (`M9 GATES PASSED` for all 28
+ 4 extra M18 cells and all 12 M18A lane cells). Per-cell values:

| dataset | `bulk_elems_total` | `bulk_elems_done` | `bulk_addr_mismatch` | C-arm completion |
|---|---:|---:|---:|---|
| bcsstk25 | 252 241 | 252 241 | 0 | 252 241/252 241 OK |
| qc2534 | 463 360 | 463 360 | 0 | 463 360/463 360 OK |
| lnsp3937 | 25 407 | 25 407 | 0 | 25 407/25 407 OK |
| dubcova3 | 1 589 539 | 1 589 539 | 0 | 1 589 539/1 589 539 OK |

`bulk_addr_mismatch` is the fanout cross-check (`bulk.cpp:1616-1626`): for every
element the engine re-derives the expected target address from the index image
and compares it with the address it issued. Zero mismatches on every dataset
means the issued target stream is the trace's own gather stream, element for
element — no reordering, no duplication, no substitution.

Target-request composition (`e-tgt` rows of the `cache_diag` attribution) shows
the expected dedup shape rather than a per-element request stream:

| dataset | elements | `bulk_tgt_req_gen` | `bulk_dedup_merged` | merged per request |
|---|---:|---:|---:|---:|
| bcsstk25 | 252 241 | 3 431 | 248 810 | 72.5 |
| qc2534 | 463 360 | 2 213 | 461 147 | 208.4 |
| lnsp3937 | 25 407 | 561 | 24 846 | 44.3 |
| dubcova3 | 1 589 539 | 111 279 | 1 478 260 | 13.3 |

(`bulk_elems_total − bulk_dedup_merged = bulk_tgt_req_gen` exactly on all four,
to within the ±1 closure the engine reports at drain.)

## 5. Identical address mapping and memory-system settings

Both variants of a dataset share one layout: `x_base = 0`, `colidx_base =
index_base`, `value_base` and `desc_line` fixed per dataset, `elem_bytes = 8`,
`page_bytes` and the DRAM geometry from the same config. The run wrapper derives
`rows`, `bulk_index_base`, `bulk_desc_line`, `bulk_max_elems` from the sidecar,
so the two arms of a comparison are launched with byte-identical layout
constants. The only differing input is the presence of the index-load
instructions in the SIMT stream.

Run settings common to every cell in this audit (`m18_run.sh`):
`experiment=m9diag`, `elem_bytes=8`, `skip_b=1`, `page_sched=1`, `idx_mode=2`,
`pool_slots=128`, `cache_diag=1`, `bulk_max_elems` from the sidecar, `lanes=4`
unless stated, window 8 192 (or 16 384 in the window-sensitivity cells).

## 6. What this equivalence does *not* establish

* It does not establish that a real compiler would choose this dispatch. It
  establishes that *given* the dispatch, the two traces differ only in the index
  stream. Section 4 of `m18a-index-accounting-audit.md` states what the dispatch
  requires of a real front end.
* It does not make the `noidx` run a legal program by itself. `noidx` is an
  accounting instrument: it removes the index loads so that the residual cost of
  the index stream can be measured by differencing. Its absolute cycle count is
  not a claim about any real program.
* Dubcova3 is a 45 000-row prefix (30.7 % of rows) with the full `x` vector, for
  the cycle budget; equivalence is established for that prefix, not for the
  whole matrix.

## Reproduce

```
cd DX100_MEM
python3 tools/m18a_audit.py equiv bcsstk25  --full build/wm/m18_bcsstk25_full.bin \
    --noidx build/wm/m18_bcsstk25_noidx.bin --mtx data/bcsstk25.mtx
python3 tools/m18a_audit.py equiv qc2534    --full build/wm/m18_qc2534_full.bin \
    --noidx build/wm/m18_qc2534_noidx.bin   --mtx data/qc2534.mtx
python3 tools/m18a_audit.py equiv lnsp3937  --full build/wm/m18_lnsp3937_full.bin \
    --noidx build/wm/m18_lnsp3937_noidx.bin --mtx data/lnsp3937.mtx
python3 tools/m18a_audit.py equiv dubcova3  --full build/wm/m18_dubcova3_full.bin \
    --noidx build/wm/m18_dubcova3_noidx.bin \
    --mtx ref/vortex/tests/opencl/spmv/Dubcova3.mtx --max-rows 45000
```
