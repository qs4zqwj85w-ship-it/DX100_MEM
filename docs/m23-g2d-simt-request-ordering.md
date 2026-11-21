# M23-G2D — SIMT request-ordering sensitivity

**Question.** How much of Bulk's DRAM advantage can conventional SIMT recover from
request reordering and row locality alone — with the same traces, the same requests,
the same address stream, the same FR-FCFS controller and no Bulk request generation
or larger outstanding window?

**Answer, in three numbers.**

1. **Row locality is worth 1.49× (Orkut) / 1.83× (ogbn) at constant work.** One DRAM
   command costs a bank 14 cycles on a row hit and 26 on a conflict, so the entire
   service-time range a reordering can move is 26/14 = **1.86×**. Measured
   constant-work speedups of the *idealized* order: **1.49×** and **1.83×**
   (§3.2). That is the whole prize, and both cells are within 2 % of the
   hit-rate-weighted prediction.
2. **Whole-stream ordering delivers 7.16× / 3.13× — but only 38 % / 66 % of that is
   row locality.** The remainder is the cache, not the DRAM: grouping requests makes
   each line's repeats adjacent, so the DRAM command count falls to exactly the
   compulsory-miss count (Orkut **513 650 → 100 282**, ogbn **1 173 937 → 689 990**).
   Both effects are separated exactly in §3.2.
3. **The bound is unreachable by SIMT, by a factor of ~2 000.** A row batch needs
   ~16 requests to one (bank,row) in flight at once; with requests spread over
   8 181 / 8 192 distinct cells that needs a window of ≈16 × cells ≈ **131 000
   requests**. The SIMT path has **64** outstanding (L2 MSHRs) and **62** of them are
   already sitting in the DRAM queue. And FR-FCFS is not the thing in the way: it
   took **every** row hit that was ever present — `dram_missed_hit = 0` in all eight
   cells — with only **1.84** (Orkut) and **0.015** (ogbn) row-hit candidates in the
   queue per decision (§6).

**Answer to the final question.** With request generation, the request count and the
outstanding window all held fixed, ordering and row locality alone can explain **at
most 1.8×** — the 26 → 14 cycle service range — and the measured idealized value is
**1.49× / 1.83×**. Any Bulk improvement beyond that cannot be ordering: it has to
come from changing *which* requests exist at the same time, i.e. from Bulk's request
generation and context accumulation, which this experiment excluded by construction.

---

## 1. Method

Two existing SIMT traces, one frozen configuration, four request orders per trace.
**No simulator source, binary, configuration field, bank count, row map, timing
constant or scheduler was changed** — the binary is the M23-G2C one
(`cba7f31a…`), and cell A reproduces both M23-G2C baselines **byte for byte**
(`sha256` of `g2d_orkut_orig.txt` = `g2d` vs `g2c` logs identical; see §7).

| cell | order | what it is |
|---|---|---|
| **A** | original SIMT order | the unmodified trace — baseline |
| **R** | original order, instructions re-formed | control: isolates re-formation from reordering |
| **B** | grouped by virtual page number | diagnostic: does page grouping help under this map? |
| **C** | grouped by modeled DRAM (bank,row) | **offline upper bound** on perfect row-aware order |

Each mode writes a **new** trace file under `build/wm/m23g2d/traces/`; the historical
traces are opened read-only and their hashes are unchanged (§7).

### 1.1 What "preserve every request" means here, and why re-formation is legal

The trace stores accesses grouped into memory instructions of ≤ 4 accesses, and the
SIMT coalescer deduplicates accesses that fall in the same 64 B line **inside one
instruction** (`system.cpp:192`). A permutation therefore cannot be applied to raw
accesses without risking that two accesses which used to be in different
instructions end up in one, silently deleting a request.

**Neither trace has anything to delete.** Both logs report
`requests_generated = accesses = 1 200 003`: 4 accesses per instruction, every one
in its own line, so *no instruction contains two accesses to one line*. The reorder
tool verifies this independently (`orig_intra_instr_dedup = 0` for both traces, from
the class-and-line table of the file itself) and then re-forms the accesses into
groups of ≤ 4 that are **guaranteed to hold distinct lines** — an access is only
appended to a group that does not already hold its line. Consequences, all checked in
§7 rather than asserted:

* every access is emitted exactly once — nothing deduplicated, nothing added;
* the address multiset is bit-identical in all four modes (sha256 of the sorted
  address list: `a7006aa23a6f36f7…` Orkut, `863cf989521c4fde…` ogbn, one value per
  workload across all four cells);
* the coalesced request count is unchanged: `requests_generated = 1 200 003` in all
  eight cells, which the cell control **R** proves is also true of the re-formation
  machinery itself (§2.1).

**Labelling.** B and C are **offline scheduling bounds, not SIMT performance.** They
do not bypass any modelled limit — the 1-instruction-per-cycle coalescer, `R = 128`,
the 128-entry L1 MSHR pool, the 64-entry L2 MSHR pool, coalescing and the request
count are all exactly as in A — but they assume a reorder window equal to the whole
trace, which no hardware provides (§6). Mode A is ordinary SIMT; R is ordinary SIMT
with a different instruction grouping; B and C are bounds.

### 1.2 Replicating the address map offline

`memory.cpp:374–375` with `bank_hash = 1`, `banks = 2`, `row_bytes = 1024`:

```
bank = parity(line ^ line>>6 ^ line>>12)        row = (line/32) mod 4096
```

Under `ideal_translation` the DRAM line is the VA line plus a constant
(`PPN_OFFSET × 64 = 2^20` lines). That constant shifts the XOR fold by 2¹⁴ and 2⁸ —
both even positions, so the bank is unchanged — and shifts the row by 2¹⁵ mod 4096 = 0.
`--check-pa` **verifies this on all 1 200 003 records of both traces** instead of
assuming it.

The map matters to the answer: a 4 KiB page is exactly **4 complete (bank,row)
cells** (2 rows × 2 banks, 16 lines each) and a bank-row is 16 lines = 1 KiB.

---

## 2. Results

Frozen configuration throughout: 4 lanes, `R` 128, `l1_mshr` 128, `l2_mshr` 64,
2 banks × 4096 rows, `t_hit` 14 / `t_miss` 26, FR-FCFS, unbounded `dram_q`,
`ideal_translation = 1`. 1 200 003 accesses per cell, all four cells of a workload
byte-identical in address multiset.

### 2.1 Controls first

| | Orkut R vs A | ogbn R vs A |
|---|---|---|
| cycles | 5 766 215 vs 5 766 215 (**identical**) | 15 323 803 vs 15 323 803 (**identical**) |
| DRAM commands, hits, conflicts, activations | identical | identical |
| `dram_q_occ_sum`, `dram_wait_sum`, `dram_wait_max` | identical | identical |
| `l1_hits` / `l2_hits` | identical (25 620 / 659 833) | identical (421 / 25 525) |
| what differs | `trace_instr` 301 191 → 300 001, `replays` +94, latency sums +103 cycles over 1.2 M accesses | `trace_instr` 303 162 → 300 001, same class of difference |

Re-forming the instruction stream (301 191 short instructions → 300 001 full ones)
changes **nothing** measurable: the same cycles to the cycle, the same DRAM counters,
the same cache hits. So every difference in B and C is the *order*, not the
re-formation, and not the all-SIMT class table that the tool writes (with
`dispatch = 0` there is no Bulk client and the class table is never read).

### 2.2 Orkut BFS

| | A (baseline) | R (control) | **B (page)** | **C (row)** |
|---|---|---|---|---|
| cycles | 5 766 215 | 5 766 215 | **804 914** | **804 914** |
| DRAM commands | 513 650 | 513 650 | **100 282** | **100 282** |
| DRAM bytes | 32 873 600 | 32 873 600 | **6 418 048** | **6 418 048** |
| **throughput, B/cycle** | **5.701** | 5.701 | **7.974** | **7.974** |
| **row hits** | 157 554 (30.67 %) | 157 554 (30.67 %) | **92 101 (91.84 %)** | **92 101 (91.84 %)** |
| row conflicts | 356 094 | 356 094 | **8 179** | **8 179** |
| row activations (`cmds − hits`) | 356 096 | 356 096 | **8 181** | **8 181** |
| distinct (bank,row) activated | 8 181 | 8 181 | 8 181 | 8 181 |
| **mean service per command** | 22.319 bank-cycles | 22.319 | **14.979** | **14.979** |
| bank utilisation (0 / 1) | 99.13 % / 99.69 % | same | **93.20 % / 93.42 %** | same |
| **queue occupancy avg (max)** | 61.65 (63) | 61.65 (63) | **25.96 (62)** | **25.96 (62)** |
| **queue wait avg (max)** | 692.1 (1 678) | 692.1 | **208.4 (815)** | **208.4 (815)** |
| L1 / L2 hits | 25 620 / 659 833 | same | **360 193 / 0** | **360 193 / 0** |
| cycles that are DRAM-service | 5 732 126 (99.41 %) | 99.41 % | **751 060 (93.31 %)** | **93.31 %** |
| cycles with an empty DRAM queue | 35 | 35 | **70 751 (8.8 %)** | 8.8 % |

B and C are **the same permutation** for this workload — the trace files hash
identically (`e1eee4c316ba27aa…`) and so do the logs (`4048fc995a741082…`) — because
Orkut's addresses span exactly **one** period of the 8 MiB map (max address
8 388 544 = 8.00 MiB, i.e. 131 072 lines of which 100 282 are touched), so ascending
page, ascending line and ascending row are the same order (§5).

### 2.3 ogbn-products train

| | A (baseline) | R (control) | **B (page)** | **C (row)** |
|---|---|---|---|---|
| cycles | 15 323 803 | 15 323 803 | **5 765 842** | **4 903 258** |
| DRAM commands | 1 173 937 | 1 173 937 | **689 990** | **689 990** |
| DRAM bytes | 75 131 968 | 75 131 968 | **44 159 360** | **44 159 360** |
| **throughput, B/cycle** | **4.903** | 4.903 | **7.659** | **9.006** |
| **row hits** | 9 199 (0.78 %) | 9 199 (0.78 %) | **538 546 (78.05 %)** | **681 798 (98.81 %)** |
| row conflicts | 1 164 736 | 1 164 736 | **151 442** | **8 190** |
| row activations (`cmds − hits`) | 1 164 738 | 1 164 738 | **151 444** | **8 192** |
| distinct (bank,row) activated | 8 192 | 8 192 | 8 192 | 8 192 |
| **mean service per command** | 25.906 bank-cycles | 25.906 | **16.634** | **14.142** |
| bank utilisation (0 / 1) | 99.15 % / 99.31 % | same | **99.38 % / 99.68 %** | **99.34 % / 99.68 %** |
| **queue occupancy avg (max)** | 61.71 (63) | 61.71 (63) | **60.37 (63)** | **61.13 (63)** |
| **queue wait avg (max)** | 805.5 (1 689) | 805.5 | **504.5 (1 106)** | **434.4 (890)** |
| L1 / L2 hits | 421 / 25 525 | same | **2 386 / 0** | **2 138 / 0** |
| cycles that are DRAM-service | 15 205 987 (99.23 %) | 99.23 % | **5 738 594 (99.53 %)** | **4 879 082 (99.51 %)** |

Here B and C differ, and the reason is §5: with a 156 MB footprint the map's 8 MiB
period aliases 19 times, so *row* grouping can batch lines that *page* grouping keeps
19 MiB apart.

### 2.4 One identity that validates every service-time number

For every one of the eight cells, the measured mean service equals the hit-rate
weighted timing exactly: `14·h + 26·(1−h)` reproduces `dram_svc_cycles / dram_reads`
to three decimals (22.319, 14.979; 25.906, 16.634, 14.142 — all exact). The DRAM
model has no hidden efficiency factor, so a change in service time *is* a change in
hit rate and nothing else.

---

## 3. Comparison against the original SIMT baseline

### 3.1 Headline

| | Orkut B/C vs A | ogbn B vs A | ogbn C vs A |
|---|---|---|---|
| cycles | 5 766 215 → 804 914 (**7.16×**) | 15 323 803 → 5 765 842 (**2.66×**) | 15 323 803 → 4 903 258 (**3.13×**) |
| row-hit rate | 30.67 % → **91.84 %** | 0.78 % → **78.05 %** | 0.78 % → **98.81 %** |
| throughput | 5.701 → **7.974 B/cycle** | 4.903 → **7.659** | 4.903 → **9.006** |
| mean queue wait | 692 → **208 cycles** | 805 → **504** | 805 → **434** |

### 3.2 Decomposing it: row locality vs work removed by the cache

Because `cycles ≈ commands × mean_service / banks` (§2.4; the DRAM is 93–99.5 % of
every cell), the cycle difference splits exactly into a term that holds the command
count fixed (latitude for row locality) and a term that comes from having fewer
commands. `decomposition.csv`:

| | Δcycles | from ordering (const. work) | from fewer commands | residual | const-work speedup |
|---|---|---|---|---|---|
| Orkut B/C | −4 961 301 (7.16×) | **−1 885 096 (38.0 %)** | −3 095 920 (62.4 %) | +19 714 (0.4 %) | **1.49×** |
| ogbn B | −9 557 961 (2.66×) | **−5 442 372 (56.9 %)** | −4 024 987 (42.1 %) | −90 602 (0.9 %) | **1.56×** |
| ogbn C | −10 420 545 (3.13×) | **−6 905 097 (66.3 %)** | −3 421 989 (32.8 %) | −93 458 (0.9 %) | **1.83×** |

The `from fewer commands` column is not a DRAM effect at all. With the stream sorted,
a line's repeats are adjacent, so the L1 absorbs them (`l1_hits` 25 620 → 360 193 on
Orkut, `l2_hits` → 0) and the DRAM sees each line **once**: 100 282 commands for
100 282 distinct lines, 689 990 for 689 990 — compulsory misses, down from
`513 650` and `1 173 937`. Ordering therefore buys bandwidth twice: fewer commands
(cache) and cheaper commands (rows). Only the second is row locality, and it is the
1.49× / 1.83×.

The residual is the non-DRAM part of each run (ramp-up before the queue fills and the
last command's bank occupancy), below 1 % everywhere.

### 3.3 What the reordering did *not* fix

Orkut's ordered cell is no longer DRAM-saturated: `dram_starved_cycles` rises from 35
to **70 751 (8.8 %)** and bank utilisation falls to 93.3 %, because at 7.97 B/cycle the
SIMT front-end (one instruction per cycle through the coalescer) stops being the
faster side. Its pure DRAM-service time is 751 060 cycles = **8.54 B/cycle**, so even
the fully ordered Orkut stream would stop at 93 % of the ceiling — the remaining 7 %
is start-up, not scheduling.

---

## 4. Against the M23-G0 ceilings

G0 derived the frozen model's sustained ceilings from its own constants: **9.143
B/cycle** with both banks continuously on row hits (2/14 × 64), **4.923 B/cycle** if
every command conflicts (2/26 × 64), **4.571** for one bank only.

| cell | B/cycle | % of 9.143 (all-hit) | row-hit rate | service |
|---|---|---|---|---|
| Orkut A | 5.701 | 62.4 % | 30.67 % | 22.32 |
| Orkut B = C | **7.974** | **87.2 %** | 91.84 % | 14.98 |
| ogbn A | 4.903 | 53.6 % (**99.6 % of the all-conflict bound**) | 0.78 % | 25.91 |
| ogbn B | 7.659 | 83.8 % | 78.05 % | 16.63 |
| ogbn C | **9.006** | **98.5 %** | 98.81 % | 14.14 |

Two readings, both useful:

* **ogbn's baseline is the row-hostile bound.** 4.903 against G0's 4.923 is within
  0.4 %: the workload behaves as a stream of pure conflicts and has no locality for a
  scheduler to find. That is why ordering helps it most in relative terms (3.13×).
* **Ordering alone reaches the ceiling.** ogbn C at 9.006 B/cycle is 98.5 % of the
  analytic all-row-hit ceiling, and its residual cost (14.14 vs 14.0 cycles) is
  exactly the 1.19 % of commands that still conflict. There is no further DRAM gain
  available in this model once the stream is row-ordered — only 1.5 % of headroom.

---

## 5. Does page grouping provide meaningful benefit?

**Yes — and for Orkut it is literally the same thing as row grouping.** In this map a
4 KiB page is 4 complete (bank,row) cells (2 rows × 2 banks × 16 lines), so sorting by
page and sorting by row produce the *same* permutation whenever the footprint stays
inside the map's 8 MiB period. Orkut's addresses end at exactly 8 388 544 B = **8.00
MiB**, one full period, so nothing aliases:

```
orkut  page_sha=e1eee4c316ba27aa  row_sha=e1eee4c316ba27aa  identical=yes
```

The two trace files are byte-identical and so are the two logs. Page grouping is
therefore *sufficient*, and no page-directory mechanism can beat it for a footprint
that fits the period.

**For a footprint larger than the period it is not sufficient.** ogbn's addresses run
to 156.7 MB = **149.5 MiB**, 18.7 periods, so lines 8 MiB apart share a row but not a
page. The cost is exactly countable from what a page contains: 4 (bank,row) cells,
and a page-ordered stream opens each of them once, so

```
activations(page order) = 4 x 38 246 pages = 152 984   predicted
                           151 444                        measured (99.0 %)
activations(row order)  =  8 192 cells = one per cell     predicted
                             8 192                        measured
```

Measured, page grouping reaches **78.05 %** row hits and row grouping **98.81 %**, and
the cycle difference is 5 765 842 → 4 903 258 (18 %). Predicted from the trace alone,
before running anything, the stream-order predictor gave 0.8738 for page and 0.9932
for row grouping — both within 10 % of what the simulator then measured (0.7805,
0.9881), the residue being FR-FCFS's ability to recover hits that the stream order
does not place adjacently.

**Conclusion:** page grouping recovers **79 %** of the available row locality on the
aliasing workload and **100 %** of it on the non-aliasing one, at zero cost in
address-map changes — but it recovers it *only* through the map's linearity, and it
inherits the aliasing limit whenever the footprint leaves one period. Whether it is
*useful* is a separate question, answered in §6: the permutation is only worth having
if something can hold enough requests to build the batch.

---

## 6. The idealized upper bound, and why SIMT cannot have it

**The bound.** C is the strongest ordering this model can express: every request
kept, every address kept, nothing deduplicated, an unbounded reorder window, and the
only key the DRAM itself uses. It yields 91.84 % / 98.81 % row hits and 7.97 / 9.01
B/cycle, i.e. **7.16× / 3.13× fewer cycles**, of which **1.49× / 1.83×** is row
locality (§3.2).

**Why a 64-request architecture cannot approach it.** A row batch is worth having
when ~16 requests to one (bank,row) are in flight together. Requests in these
workloads are spread over 8 181 / 8 192 distinct (bank,row) cells, so a window of `W`
requests holds on average `W / 8192` per cell; reaching 16 needs

```
W ≈ 16 × 8 192 ≈ 131 000 requests
```

against `R = 128` slots and a **64-entry L2 MSHR pool** — of which **62 are already
inside the DRAM queue** in the baseline (queue occupancy 61.65 / 61.71, max 63).
A reorder buffer cannot add a window here: it would have to take entries out of the
same 64-request budget the queue is holding, and the queue is the thing doing the
batching.

**The evidence that the scheduler is not the problem.** `dram_missed_hit = 0` in all
eight cells: whenever a row hit was *selectable* (bank free), FR-FCFS selected it — no
exceptions, in any mode. What the queue actually contained is measured directly:
`dram_hit_cand_sum / decisions` = **1.84** (Orkut) and **0.015** (ogbn) row-hit
candidates per decision. A perfect scheduler given those queue contents could not
have done better; the miss is in what the window *contains*.

**What did move the hit rate, then?** Widening the interval over which requests are
allowed to coexist — the whole trace. That is exactly the resource the brief excludes.
Mode C is therefore an upper bound on ordering, and the honest achievable statement
for SIMT at 64 outstanding is *zero measured gain*: no reordering of a 64-request
window drawn from 8 192 cells can assemble a 16-line row batch, and the arbiter already
takes every hit present.

---

## 7. Achievable mechanisms vs offline bounds

| mechanism | window | achievable on this model? | measured effect |
|---|---|---|---|
| FR-FCFS on arrival order (A) | 62 commands | yes — the frozen design | 5.70 / 4.90 B/cycle |
| row-hit preference in the queue (already present) | 62 | yes | 0 further: `missed_hit = 0` |
| page-grouped arrival order (B) | whole trace | **no** — offline bound | 7.97 / 7.66 B/cycle |
| row-grouped arrival order (C) | whole trace | **no** — offline bound | 7.97 / 9.01 B/cycle |
| any reorder buffer of ≤ 64 entries | ≤ 64 | yes, but **no benefit** (§6) | not measurable |
| Bulk-style generation in page order | 4 096-request cohort | excluded by the brief | not measured here |

B and C bypass no modelled limit — same 1-instruction-per-cycle coalescer, same `R`,
same MSHR pools, same coalescing, same command count before the cache absorbs
repeats — but they assume a reorder window of 1 200 003 requests. Every B/C number
above is **an offline scheduling upper bound, not SIMT performance**.

---

## 8. Verification

| check | result |
|---|---|
| mode A reproduces the M23-G2C baseline | **2/2 byte-identical** (`g2d_orkut_orig.txt` = `g2c_orkut.txt`, `g2d_ogbn_orig.txt` = `g2c_ogbn.txt`, same sha256) |
| address multiset identical across A/R/B/C | **2/2 workloads**, one sha256 per workload over all four cells (`a7006aa2…`, `863cf989…`) |
| identical total request count | **8/8** — `requests_generated = accesses = 1 200 003` in every cell |
| no request loss or duplication | **8/8** — `completed = 1 200 003`, `conservation_ok=1`, 0 violations |
| footprint unchanged | **8/8** — 100 282 lines / 2 048 pages / 8 181 cells; 689 990 / 38 246 / 8 192 |
| re-formation control (R) | **2/2 identical cycles and DRAM counters** to A |
| `dram_bytes = 64 × commands` | **8/8** |
| arrivals = service events = commands | **8/8** |
| cycle partition `starved + blocked + issued = cycles + 1` | **8/8** |
| `dram_wait_sum = dram_q_occ_sum` (queueing identity) | **8/8** |
| `elig_wait_cycles = 0` | **8/8** |
| `mean service = 14h + 26(1−h)` | **8/8 exact** |
| `./bin/dx100_tests` | **4 735 checks, 0 failures** |
| historical artifacts | untouched: `g2c_*.txt` hashes unchanged, input traces unchanged, no source or config edited |

---

## 9. Limitations

* **B and C are offline bounds.** Their reorder window is the entire trace (§7). They
  say what the model's DRAM *could* do with this address stream, not what any SIMT
  machine does.
* **The command-count change is a cache effect, not a DRAM one**, and is reported
  separately for that reason (§3.2). A stream permutation changes cache behaviour by
  construction; the only count-preserving comparison is the constant-work column.
* **`ideal_translation = 1`, by request.** With real translation these traces are
  dTLB-bound and the DRAM is far less loaded (M23-B/C/D, M23-F1); nothing here
  describes that regime.
* **One core, 4 warps, 4 lanes, one requester class**: there is no other client
  interleaving to break up locality, which is the most favourable case for ordering.
* **8 MiB map aliasing is a model property.** ogbn's 98.81 % in C draws on rows that
  recur every 8 MiB (M23-G0 §7.5); a real device's row/bank mapping would not alias
  this way, so C's ogbn number is optimistic even as a bound on *this* trace.
* **The `row_activations` column is `commands − hits`** (every command either hits an
  open row or activates one). A bank is never precharged in this model, so only **2**
  of them (one per bank) are cold activations, and `row_conflicts` counts activations
  that interrupt an open row.
* The window argument in §6 uses the average `W / cells`; it is an expectation, not a
  measured queue census. The measured corroborations are `missed_hit = 0` and
  1.84 / 0.015 hit candidates per decision.

---

## 10. Files

| path | what |
|---|---|
| `docs/m23-g2d-simt-request-ordering.md` | this report |
| `tools/m23g2d_reorder.py` | builds the permuted traces; verifies the address multiset, the no-dedup property, the offline map (incl. the PA-offset check) and predicts the stream-order hit rate |
| `build/wm/m23g2d_run.sh` | 8-cell runner (reuses `configs/m23g2b_base.cfg`; no new config) |
| `tools/m23g2d_extract.py` | tables, identity checks, ordering-vs-work decomposition |
| `build/wm/m23g2d/traces/*.bin`, `*.json` | 6 new traces + their metadata (input traces untouched) |
| `build/wm/m23g2d/g2d_*.txt` | 8 raw logs |
| `build/wm/m23g2d/summary.csv`, `decomposition.csv`, `checks.txt` | results |
| `build/wm/m23g2d/hashes.txt` | provenance |

**Nothing in the simulator changed.** `sim/src/memory.cpp` `e4db3df3…`,
`system.cpp` `20516df4…`, `sim.hpp` `b31f66dd…`, `workload.cpp` `c85d1141…` and
`bin/dx100sim` `cba7f31a…` are the M23-G2C artifacts; `configs/m23g2b_base.cfg`
`923514f4…` is field-identical to `m23_m0.cfg` plus `ideal_translation` and
`dram_diag`. Input traces: `m23_orkut_bfs.bin` `11cc8da4…`, `m23_ogbn.bin`
`1f10240f…`. No historical config, trace, log or report was modified.

## 11. Reproduction

```bash
cd DX100_MEM
./build/wm/m23g2d_run.sh            # ~15 min: 6 traces + 8 cells
python3 tools/m23g2d_extract.py     # summary.csv, decomposition.csv, checks.txt
cd sim && ./bin/dx100_tests         # 4735 checks, 0 failures
```
