# M19 — Operation Equivalence and Per-Stream Accounting

Scope: investigations 1, 2, 4, 5 and 6 of the M19 brief. The conclusions that
depend on these — and the one defect they exposed — are in
`m19-accounting-audit-report.md`.

All numbers below are reproduced by `tools/m19_recompute.py`, whose parsers are
written from scratch (it does not import `m18_extract.py`, `m18_metrics.py` or
`m18a_audit.py`), so agreement with the M18 tables is evidence rather than a
shared bug.

```
python3 tools/m19_recompute.py streams  build/wm/m18_bcsstk25_full.bin
python3 tools/m19_recompute.py verify
python3 tools/m19_recompute.py cells
python3 tools/m19_recompute.py integrity
```

---

## 1. Every logical operation, on both architectures

The SpMV trace is emitted in CSR program order: per row, `rowptr[i]` and
`rowptr[i+1]` as one 2-lane instruction, then the row's `colidx` group, its
`value` group, and the `x[colidx[j]]` gather for that group
(`tools/build_trace.py::build_spmv`). Each stream is a separate address region,
and the regions are disjoint (`layout` in the trace sidecar).

The class decision is per **instruction**, made by the generator and read by the
loader (`workload.cpp`: `op_class=gen` → `tr.instr_class`), and it is exactly
`kind.endswith("_gather") → Bulk` (`build_trace.py::Trace::instr`). So:

| logical operation | stream | class | who performs it, A | who performs it, C |
|---|---|---|---|---|
| row bounds | `rowptr` | SIMT | SIMT front end | SIMT front end |
| column indices | `colidx` | SIMT | SIMT front end | engine (`e-idx`), and SIMT too in the `full` trace |
| matrix values | `value` | SIMT | SIMT front end | SIMT front end |
| `x[colidx[j]]` | `x_gather` | **Bulk** | SIMT front end | engine (`e-tgt`) |
| `y[i] += a*x[...]` | — | — | **not modelled** | **not modelled** |

The last row is the only logical operation with no trace representation:
`build_trace.py` writes `write_accesses: 0` and `read_write_ratio: "read-only
trace"`, and the application output is a checksum computed in Python, not a
store stream. See `m19-accounting-audit-report.md` §6 for the classification and
the bound on its effect.

## 2. Per-stream requests, exact

"Requests" = distinct lines per instruction, i.e. what the coalescer hands to the
L1 after intra-instruction dedup. "Compulsory" = distinct lines the stream touches
at all. The ratio is what each stream amplifies.

### bcsstk25 (15 439 rows, 252 241 nnz)

| stream | accesses | compulsory lines | requests | bytes/request | amplification |
|---|---:|---:|---:|---:|---:|
| rowptr | 30 878 | 965 | 16 403 | 7.5 | 17.0× |
| colidx | 252 241 | 15 766 | 79 573 | 12.7 | 5.05× |
| value | 252 241 | 31 531 | 91 086 | 22.2 | 2.89× |
| x_gather | 252 241 | 1 930 | 136 747 | 14.8 | 70.9× |
| **total** | 787 601 | 50 192 | 323 809 | | 6.45× |

### qc2534 (2 534 rows, 463 360 nnz)

| stream | accesses | compulsory | requests | bytes/request | amplification |
|---|---:|---:|---:|---:|---:|
| rowptr | 5 068 | 159 | 2 692 | 7.5 | 16.9× |
| colidx | 463 360 | 28 960 | 137 199 | 13.5 | 4.74× |
| value | 463 360 | 57 920 | 157 832 | 23.5 | 2.72× |
| x_gather | 463 360 | 317 | 157 917 | 23.5 | 498× |
| **total** | 1 395 148 | 87 356 | 455 640 | | 5.22× |

### lnsp3937 (3 937 rows, 25 407 nnz)

| stream | accesses | compulsory | requests | bytes/request | amplification |
|---|---:|---:|---:|---:|---:|
| rowptr | 7 874 | 247 | 4 183 | 7.5 | 16.9× |
| colidx | 25 407 | 1 588 | 9 095 | 11.2 | 5.73× |
| value | 25 407 | 3 176 | 10 178 | 20.0 | 3.20× |
| x_gather | 25 407 | 493 | 16 650 | 12.2 | 33.8× |
| **total** | 84 095 | 5 504 | 40 106 | | 7.29× |

### dubcova3, 45 000-row prefix (1 589 539 nnz) — **M18 layout (defective, §5)**

| stream | accesses | compulsory | requests | bytes/request | amplification |
|---|---:|---:|---:|---:|---:|
| rowptr | 90 000 | *aliased* | *aliased* | — | — |
| colidx | 1 589 539 | 99 347 | 495 868 | 12.8 | 4.99× |
| value | 1 589 539 | 198 693 | 581 919 | 21.9 | 2.93× |
| x_gather | 1 589 539 | 18 337 | 870 247 | 14.6 | 47.5× |
| **total** | 4 858 617 | 316 377 | 1 948 034 | | 6.16× |

The `rowptr` row is the defect: 90 000 accesses whose lines are shared with `x`
(§5). The other three rows are unaffected — `colidx`, `value` and `x` keep their
M18 addresses under the correction.

## 3. What is shared, duplicated, merged or omitted

**Shared (one request serves both front ends):** nothing. Every request belongs
to exactly one client. What *is* shared is the *data*: in the `full` trace the
engine's index array and the SIMT `colidx` stream are the **same physical
array at the same addresses** (`bulk_index_base = layout.colidx_base`), so a line
the engine has already fetched can be hit by the SIMT reader later.

**Duplicated:** in `C(full)` the `colidx` lines are requested twice — 79 573
SIMT requests plus 15 766 engine requests for the same 15 766 lines. This is the
double charge that convention A measures. It is *not* exactly 2× traffic, because
the second reader partially hits:

| dataset | C(full) SIMT index L2 misses | A(full) SIMT index L2 misses | engine index L2 misses |
|---|---:|---:|---:|
| bcsstk25 | 10 358 | 15 783 | 15 766 |
| qc2534 | 25 645 | 28 991 | 28 960 |
| lnsp3937 | 0 | 1 589 | 1 588 |
| dubcova3 | 98 157 | 101 769 | 99 347 |

**Merged:** the engine merges target requests inside a window —
`bulk_dedup_merged` = 248 810 / 461 147 / 24 846 / 1 478 260, i.e. elements per
issued target request 72.5 / 208.4 / 44.3 / 13.3. The SIMT arm has no equivalent
merge; it issues one request per distinct line per instruction. That asymmetry is
the architecture, and both sides pay their own requests in every convention.

**Omitted:** the output accumulation and the `y` store to memory (not modelled in
either arm, §1); DRAM *write* traffic and writeback (none, read-only trace); L1
allocation for engine requests (`bypass_l1 = true`, `bulk.cpp::issue` — the engine
has no private L1, so every engine request is an L2 access and it can never
benefit from an L1 hit).

## 4. Equivalence of logical elements and target addresses

Three independent checks, all passing on every reported cell
(`build/wm/m19_integrity.json`):

1. **Element partition.** Each cell prints `class map: S SIMT + B bulk
   instructions -> e_s + e_b elements`. Exhaustiveness (`e_s + e_b == accesses`)
   holds on all 52 recorded cells, and equals the trace's own `accesses` field.
2. **Independent request reconciliation** (`m19_recompute.py verify`): the
   trace-derived request count equals the simulator's own counter exactly, for
   both arms, in both variants — 8/8 cells, 0 mismatches:

   | cell | A `requests_generated` | trace (SIMT+BULK) | C SIMT requests | trace (SIMT class) |
   |---|---:|---:|---:|---:|
   | bcsstk25 full | 323 809 | 323 809 | 187 062 | 187 062 |
   | bcsstk25 noidx | 244 236 | 244 236 | 107 489 | 107 489 |
   | qc2534 full | 455 640 | 455 640 | 297 723 | 297 723 |
   | qc2534 noidx | 318 441 | 318 441 | 160 524 | 160 524 |
   | lnsp3937 full | 40 106 | 40 106 | 23 456 | 23 456 |
   | lnsp3937 noidx | 31 011 | 31 011 | 14 361 | 14 361 |
   | dubcova3 full | 1 948 034 | 1 948 034 | 1 125 599 | 1 125 599 |
   | dubcova3 noidx | 1 452 166 | 1 452 166 | 629 731 | 629 731 |

   This is stronger than M18's own gate: it shows the hybrid's SIMT client issues
   *exactly* the requests of the SIMT-class instructions of the shared trace, so
   the class split neither drops nor duplicates work.
3. **Output and target identity.** `output check (C): value hash … vs
   expectation …, mismatched addresses 0` on all 52 cells; `bulk_elems_done ==
   bulk_elems_total` on all cells that arm the engine; `bulk_addr_mismatch = 0`
   everywhere. The engine's targets are re-derived from the index image at
   completion, so a reordered, duplicated or substituted target would be counted.

Note what is **not** gated by M9: there is no gate comparing arm A's address set
to arm C's. Equivalence there rests on the shared trace object plus the
exhaustive partition (checked above), and on the M18A proof that `full` minus the
`colidx` stream is exactly `noidx`. Gates passing is therefore necessary but not
sufficient evidence — which is why the two checks above were run independently.

## 5. Defect: `rowptr` aliases `x` for a row-prefix trace (dubcova3)

`build_spmv` lays out `x` at 0 and then places `rowptr` at `align(m * 8)`:

```python
x_base = 0
rowptr_base = align(m * 8)          # M18
```

`x` has one entry per **column**, so it spans `cols * 8`. For a square matrix
`m == cols` and the two regions are exactly adjacent — this is why the defect
never showed. For a **row prefix of a wider matrix** (`m=45 000 < cols=146 689`,
dubcova3) the row-pointer array lands **813 512 bytes inside `x`**:

| dataset | rows | cols | `rowptr_base` | `x` span | overlap |
|---|---:|---:|---:|---:|---|
| bcsstk25 | 15 439 | 15 439 | 123 520 | 123 512 | no |
| qc2534 | 2 534 | 2 534 | 20 288 | 20 272 | no |
| lnsp3937 | 3 937 | 3 937 | 31 552 | 31 496 | no |
| dubcova3 | 45 000 | 146 689 | 360 000 | 1 173 512 | **yes, 813 512 B** |

Effect: 45 000 row-pointer instructions (90 000 reads, 4 B each) read lines that
also hold `x` elements, so they are served from `x` lines that the gather keeps
hot instead of from their own ~2 813-line region. It is an **accounting** defect,
not a correctness one: both arms see the same addresses and the output hash is
unaffected. The affected result is dubcova3's absolute cycles and its
speedup; §5.1 measures the size.

**Correction** (separate, minimal, recorded here):

```python
rowptr_base = align(max(m, n) * 8)  # M19: x is sized by columns, not rows
```

For every square matrix `max(m, n) == m`, so this is a no-op: bcsstk25, qc2534
and lnsp3937 rebuild to **byte-identical traces** (verified — `trace_sha256`
unchanged for all three). Only dubcova3 changes, and only its `rowptr_base`
(360 000 → 1 173 568 = the first 64 B line after `x`); `colidx_base`,
`value_base`, `x_base` and `max_addr` are unchanged.

The affected cells were rebuilt and re-run as `build/wm/m19_dubcova3_*.txt`,
leaving every historical `m18_*` file untouched. Results are in
`m19-corrected-metrics.md`.

## 6. Index stream versus target gather in the shared cache

There is one `MemorySystem` per arm (`mem_path=l1`, `cores=1`), so the index
stream and the target gather genuinely contend. Three measurements show the
contention is modelled rather than assumed, and bound it:

**(a) Removing the stream changes the *other* stream's misses.** Differencing the
baseline variants isolates the index stream's full effect, including its
evictions:

| dataset | SIMT index requests | index DRAM (differenced) | compulsory index lines | excess |
|---|---:|---:|---:|---:|
| bcsstk25 | 79 573 | 15 783 | 15 766 | +17 |
| qc2534 | 137 199 | 28 991 | 28 960 | +31 |
| lnsp3937 | 9 095 | 1 589 | 1 588 | +1 |
| dubcova3 | 495 868 | 101 769 | 99 347 | +2 422 |

The excess is the eviction cost the index stream imposes on the gather: negligible
(≤0.1 %) where `x` is small relative to L2, 2.4 % on dubcova3 where the 6.4 MB
index array displaces the 1.2 MB `x`.

**(b) The hybrid shows the same effect on its own target stream.** Comparing the
two variants of the *hybrid* holds the target work fixed and changes only whether
the index array is read:

| dataset | target requests | target DRAM, C(full) | target DRAM, C(noidx) | index-induced target misses |
|---|---:|---:|---:|---:|
| bcsstk25 | 3 432 / 3 437 | 1 931 | 1 931 | 0 |
| qc2534 | 2 213 / 2 213 | 318 | 318 | 0 |
| lnsp3937 | 561 / 559 | 494 | 494 | 0 |
| dubcova3 | 111 280 / 111 307 | 82 190 | 80 063 | **+2 127** |

**(c) The engine's own index stream evicts.** `e-idx` evicts 7 467 / 18 449 / 0 /
98 197 lines (`L2ev` column) — i.e. the engine's acquisition is a first-class
tenant of L2, not a side channel.

Verdict for investigation 6: the interaction is represented and measurable, and
it is asymmetric between datasets in exactly the way the working-set sizes
predict. What the model does **not** represent: any priority, bypass, pinning or
stream-buffer policy that would protect the gather from the index stream. Every
client competes under one LRU. That is a limitation of the architecture model,
not an accounting error, and it is listed as such in the audit report.

## 7. Nothing is free — code-path evidence

| claim | evidence |
|---|---|
| the engine cannot form a target before its index line returns | `bulk.cpp::generate`: `auto it = idx_done_.find(chunk); if (it == idx_done_.end()) { st_.idx_wait_stall++; break; }` — the value is read out of a *returned* line |
| address generation is rate-limited | `u32 budget = max(1, cfg_.ag_lanes)` (8/cycle) consumed once per generated element |
| engine requests traverse the same hierarchy and translate | `issue()`: `r.vpn = p.va_line >> 6; mem_.submit(r, cycle)` — the same `submit` the SIMT client uses; the `walk` client row and the C arm's `tlb0_misses` (2 068 vs A's 1 575 on bcsstk25) show the engine's pages miss and walk |
| the engine gets no L1 hit | `r.bypass_l1 = true` (no private L1) and `e-idx`/`e-tgt` L1 hits are 0/95 (bcsstk25) — the 95 are target-side sector hits inside a line the engine itself brought in |
| nothing is precomputed | `idx_oracle` is false in every M18 cell; the oracle variant exists only as an ablation and is *not* what M18 measured |
| targets are the trace's own gathers | `bulk_addr_mismatch = 0` on every cell, re-derived from the index image at completion |

M18's `noidx` arm is the only place where work is removed, and §2 of the
equivalence work shows it removes exactly the index-load instructions: nothing
else changes address, instruction or lane.
