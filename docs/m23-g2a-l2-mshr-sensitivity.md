# M23-G2A — SIMT L2 MSHR sensitivity

**Question.** With translation removed from the path, does L2 MSHR capacity limit
conventional SIMT DRAM throughput — and if it is raised, where does the limit go?

**Answer in one line.** At the frozen 64 entries the L2 MSHR pool is completely
occupied (99.8-99.9 % average, peak = capacity) and refuses roughly 60-64
allocations per cycle, but it is **not** the throughput limit: 64 concurrent
fills already hold the two-bank DRAM 99.2-99.5 % busy, so doubling the pool buys
**0.5-1.3 %** of cycles. Raising it to 256 changes nothing at all — the run is
byte-identical to 128 — because the binding resource has already moved to the
global outstanding budget (`R` = `l1_mshr` = 128), and past that to the DRAM
itself.

Nine cells, three real traces, three capacities. No RTL, model, pipeline or
historical configuration was modified.

---

## 1. What was run

| field | value |
|---|---|
| front-end | SIMT, `dispatch = 0`, `batch_window = 0` (M23-B/C/D `m0` arm) |
| core | `cores` 1, `warps_per_core` 4, `lanes` 4, `elem_bytes` 4 |
| global budget | `R` = 128, `lsu_pending` = 0, `mem_path = l1` |
| L1 | 256 lines x 4 ways, 2 cy, **`l1_mshr` = 128** (unchanged) |
| L2 | 16384 lines x 8 ways, 4 cy, **`l2_mshr` = 64 / 128 / 256** |
| translation | **`ideal_translation = 1`** (no dTLB, no walker, no PTE traffic) |
| DRAM | 2 banks x 4096 rows, 1024 B rows, `t_hit` 14, `t_miss` 26, `link` 0, `dram_q` 0, FCFS, bank hash |
| clock | 400 MHz (`system.cpp:285`), 64 B per command |

Configuration: `sim/configs/m23g2a_base.cfg`, which is `sim/configs/m23_m0.cfg`
with exactly two lines added — `ideal_translation = 1` and `dram_diag = 1` —
and nothing else changed (verified by `diff`; see §6). The only swept field is
`l2_mshr`, supplied per cell on the command line.

Traces are the existing ones, passed with the same `--set` arguments as their
historical `m0` cells (`docs/m23-b-criteo-embedding.md` §9,
`docs/m23-c-orkut-graph.md` §9, `docs/m23-d-ogbn-products-gnn-gather.md` §9):

| workload | trace | sha256 (first 16) |
|---|---|---|
| Criteo (16 384 records, 26 categorical fields) | `build/wm/m23_criteo_16k.bin` | `4b4e333aec0b6e84` |
| Orkut BFS frontier (max-degree seed, 131 072-vertex induced subgraph) | `build/wm/m23_orkut_bfs.bin` | `11cc8da4cfaf9a99` |
| ogbn-products train minibatch (8 455 targets, 16 of 100 features) | `build/wm/m23_ogbn.bin` | `1f10240f78bc95ad` |

```bash
# from DX100_MEM
./build/wm/m23g2a_run.sh                      # nine cells -> build/wm/m23g2a/
python3 tools/m23g2a_extract.py                # summary.csv, checks.txt
```

---

## 2. Results

`m64` is the frozen baseline. `Δ cycles` is against `m64`. Occupancy is
`l2_mshr_occ / cycles` (the counter is occupancy summed over every cycle);
"MSHR allocation failures" is `cc0_l2_stalls`, the cycles in which an L2 MSHR
allocation was refused (the front-end retries such a request every cycle, so
this is a cycle count, equal to `replays` in every cell).

| workload | L2 MSHRs | cycles | Δ cycles | DRAM cmds | B/cycle | GB/s | row-hit | MSHR peak | MSHR avg (% of cap) | alloc failures | DRAM q avg / peak | DRAM busy | outstanding avg |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Criteo | 64 | 1 010 690 | — | 82 380 | 5.217 | 2.087 | 13.2 % | 64 | 63.87 (99.8 %) | 60 482 736 | 61.6 / 63 | 99.5 % | 127.10 |
| Criteo | 128 | 997 570 | **−1.30 %** | 82 318 | 5.281 | 2.113 | 15.1 % | 128 | 122.54 (95.7 %) | **0** | 120.3 / 126 | 99.8 % | 127.06 |
| Criteo | 256 | 997 570 | −1.30 % | 82 318 | 5.281 | 2.113 | 15.1 % | 128 | 122.54 (47.9 %) | 0 | 120.3 / 126 | 99.8 % | 127.06 |
| Orkut BFS | 64 | 5 766 215 | — | 513 650 | 5.701 | 2.280 | 30.7 % | 64 | 63.90 (99.9 %) | 362 273 765 | 61.6 / 63 | 99.4 % | 127.55 |
| Orkut BFS | 128 | 5 735 060 | **−0.54 %** | 513 600 | 5.731 | 2.293 | 31.1 % | 128 | 126.67 (99.0 %) | **0** | 124.4 / 127 | 99.7 % | 127.54 |
| Orkut BFS | 256 | 5 735 060 | −0.54 % | 513 600 | 5.731 | 2.293 | 31.1 % | 128 | 126.67 (49.5 %) | 0 | 124.4 / 127 | 99.7 % | 127.54 |
| ogbn train | 64 | 15 323 803 | — | 1 173 937 | 4.903 | 1.961 | 0.78 % | 64 | 63.92 (99.9 %) | 975 683 147 | 61.7 / 63 | 99.2 % | 127.83 |
| ogbn train | 128 | 15 190 916 | **−0.87 %** | 1 173 923 | 4.946 | 1.978 | 1.54 % | 128 | 127.59 (99.7 %) | **0** | 125.4 / 127 | 99.7 % | 127.83 |
| ogbn train | 256 | 15 190 916 | −0.87 % | 1 173 923 | 4.946 | 1.978 | 1.54 % | 128 | 127.59 (49.8 %) | 0 | 125.4 / 127 | 99.7 % | 127.83 |

`DRAM busy` is `dram_busy_cycles / (2 x cycles)` — both banks busy, since the
model's `busy` counter is summed across banks. Completed commands are
`dram_reads`; `dram_svc_events` equals it in all nine cells.

### 2.1 L2 MSHR capacity is saturated at 64, and it does not matter

The pool is at **99.8-99.9 % average occupancy with peak = capacity** in all
three 64-entry cells: it is not merely busy, it is the single most contended
resource in the machine. Yet those cells also show the DRAM **99.2-99.5 % busy**,
`dram_starved_cycles = 35` (the queue is never empty), and an average of
**127.1-127.8 outstanding requests** against `R` = 128. The pool is saturated
*downstream* of an already-saturated bus: 64 fills in flight are more than the
two banks can consume, so the 60-976 M allocation-failure cycles are the
front-end spinning for a slot that the bus would not have served any sooner.

That is why 128 recovers only 0.54-1.30 %. The residue is not extra concurrency
— `inflight_avg` is unchanged to within 0.04 % — it is **scheduling**: with more
requests resident, FCFS sees more candidates per decision and finds more row
hits (Criteo 13.2 → 15.1 %, ogbn 0.78 → 1.54 %), so the same workload needs
62 / 50 / 14 fewer commands and a slightly shorter run. A capacity that changes
command *count* by 0.001-0.08 % can only change cycles by about that much.

### 2.2 Raising it moves the limit to `R`, then to DRAM

At 128 the peak occupancy is exactly 128 — equal to `R` and to `l1_mshr` — in
all three workloads, and allocation failures are zero. The next resource is
therefore the global outstanding budget, not the L2. Capacity 256 makes that
explicit: peak occupancy stays at 128 (**50 % of the pool unused**), and the
`diff` of the 128 and 256 logs is a single line, the echoed capacity:

```
4c4
< l2=16384x8/4cy mshr=128
---
> l2=16384x8/4cy mshr=256
```

for all three workloads. Cycles, commands, row hits, queue depth and every
counter are identical, so 256 is not "no better than 128" — it is
indistinguishable from it.

What remains below the pool is the DRAM. Achieved throughput is
4.90-5.73 B/cycle, against M23-G0's derived bounds for this frozen model of
9.143 B/cycle (sustained row hits, 2 banks x 14 cy) and 4.923 B/cycle (all
conflicts). ogbn runs at **4.903 B/cycle with 0.78 % row hits — 0.4 % below the
all-conflict bound**, and Criteo/Orkut sit at 57-63 % of the row-hit bound
because their addresses are mostly conflict-missing (row-hit 13-31 %). With the
L2 pool at 128 and the budget at `R` = 128 the chain is:

```
issue (<= 1 instruction/cycle, M23-G1)  ->  global budget R = 128
    -> L1 MSHR 128  ->  L2 MSHR 64..256  ->  DRAM (2 banks, 1 cmd/cycle)
```

and the last stage is the one that is 99 % busy. Raising L2 MSHRs alone cannot
move it; raising `R` without raising the pool would merely return the system to
the 64-entry state, because 128 requests in flight still need only ~64 fills.

### 2.3 Contrast with the real-translation cells

These numbers are not the whole picture of the workloads, and the difference is
worth keeping straight. In the historical `m0` cells of M23-B/C/D — same traces,
real translation, same 64 L2 MSHRs — the runs took 28.5 M / 49.1 M / 176.2 M
cycles with **average** outstanding of just 4.39 / 6.40 / 4.28 (peaks 13 / 128 / 12)
and at most 7 / 64 / 7 L2 MSHRs in use. There, a dTLB translation MSHR, not the L2, is the first limiter
(M23-F1: ideal translation moves outstanding to 127-128 and throughput up
10-27x). M23-G2A deliberately removes that upstream limit, and the finding is
that **even with translation out of the way, L2 MSHR capacity is still not the
throughput limiter** — the DRAM is.

---

## 3. Deliverable answer

1. **Does L2 MSHR capacity limit SIMT DRAM throughput?** No. At 64 the pool is
   fully occupied (99.8-99.9 %, peak = capacity) and refuses allocations
   constantly, but the DRAM is already 99.2-99.5 % busy with 127 outstanding
   requests; the ceiling it imposes is on queue *scheduling* (row-hit grouping),
   worth at most 1.3 % of cycles across three workloads. The retry cycles it
   causes (60 M / 362 M / 976 M) are spent waiting for a bus that has no spare
   capacity, not for concurrency that is missing.
2. **Does increasing it move the bottleneck?** Yes, and immediately. 128 removes
   every allocation failure but stops at 128 = `R` = `l1_mshr`; 256 is
   byte-identical to 128 with half its pool unused. The limit moves first to the
   global outstanding budget (`R`, `l1_mshr`), then to the DRAM, which is the
   only resource still at 99 % utilisation once the pool is enlarged. Any further
   gain must come from the address stream (row locality) or from a wider DRAM
   model, not from more L2 MSHRs.

---

## 4. Accounting and correctness

Checked programmatically for all nine cells (`tools/m23g2a_extract.py`,
`build/wm/m23g2a/checks.txt`; all rows `yes`):

| check | result |
|---|---|
| `conservation=OK`, `conservation_ok=1`, `conservation_violations=0` | 9/9 |
| `completed == cc0_done == cc0_issue == l1_reads` | 9/9 |
| `dram_bytes == 64 x dram_reads` | 9/9 |
| `l2_reads == l2_hits + l2_misses` | 9/9 |
| `l2_mshr_allocs + l2_mshr_joins == l2_misses` | 9/9 |
| `dram_reads == l2_mshr_allocs` (every command is a fill) | 9/9 |
| `dram_svc_events == dram_reads` | 9/9 |
| `walks == 0`, `pte_fetches == 0`, no client-2/3 L2 reads (ideal translation really is inert) | 9/9 |
| `l2_mshr_max <= l2_mshr`; saturation implies `l2_mshr_max == l2_mshr` | 9/9 |
| `cc0_l2_stalls == replays`; no other client stalls | 9/9 |
| `dram_q_full == 0` (the DRAM queue is unbounded, as configured) | 9/9 |

**Identical address traces across configurations.** The three cells of a
workload read the same trace file (SHA-256 above, byte-identical) through an
unmodified loader, and the realized-workload block that the loader prints
(`accesses`, `instrs`, `lines`, `pages`, `rho_line`) is identical across the
three capacities — one distinct tuple per workload, as `checks.txt` records.
The issued request stream is also identical: `completed` and `l1_reads` are
exactly 360 326 / 1 200 003 / 1 200 003 in all three capacities of Criteo /
Orkut / ogbn, which are the SIMT request counts M23-F1 recorded for the same
traces. (Criteo's 360 326 is below its 638 976 traced elements because the trace
also carries an engine-classed element stream that an arm with no engine does
not issue; that is inherited from M23-B and unchanged here.)

**Independent reproduction.** The `m64` cells reproduce the M23-F1 ideal-translation
rows for the same traces exactly — Criteo 1 010 690 cycles / 82 380 commands /
60 482 736 stalls and Orkut 5 766 215 cycles / 513 650 commands / 362 273 765
stalls, matching `docs/m23-f1-translation-sensitivity.md` §6 to the cycle. Those
prior cells were produced before M23-G2A and their logs were not touched.

**Regression suite.** `./bin/dx100_tests` → **4 735 checks, 0 failures**, run
against the unmodified `sim/` tree after the nine cells (no source file was
edited, so the suite covers the same binary that produced them).

---

## 5. Simulator limitations

- One core, 4 warps, 4 lanes. The DRAM model has no data bus, issues one 64 B
  command per cycle and models only bank/row state (M23-G0), so "99 % busy" is
  a command-slot statement, not a bandwidth measurement.
- `ideal_translation = 1` removes the dTLB and the walkers from the primary
  experiment by design. These numbers say what the L2 MSHR pool does once
  translation is out of the way; they are not the throughput these workloads
  achieve with translation on (§2.3).
- MSHR occupancy is reported from the simulator's own per-cycle occupancy sum
  (`l2_mshr_occ / cycles`); allocation failures are per-cycle refusals, and with
  `l1_mshr` > `l2_mshr` a refused request re-probes every cycle, so the raw
  failure count (60-976 M) overstates the number of distinct requests affected.
- No physical GPU is measured or claimed anywhere in this note; the DRAM bounds
  are properties of this frozen model (M23-G0).

---

## 6. Files and provenance

| path | what |
|---|---|
| `docs/m23-g2a-l2-mshr-sensitivity.md` | this report |
| `sim/configs/m23g2a_base.cfg` | instrument configuration (= `m23_m0.cfg` + `ideal_translation`, `dram_diag`) |
| `build/wm/m23g2a_run.sh` | nine-cell runner (optional workload filter) |
| `tools/m23g2a_extract.py` | extractor and accounting checks |
| `build/wm/m23g2a/g2a_{criteo,orkut,ogbn}_m{64,128,256}.txt` | raw logs, 9 cells |
| `build/wm/m23g2a/summary.csv` | one row per cell, all reported metrics |
| `build/wm/m23g2a/checks.txt` | per-cell accounting verdicts and trace identity |
| `build/wm/m23g2a/hashes.txt` | config / runner / extractor / binary / source / trace SHA-256 |

Unchanged, and verified by hash against the M23-G1 record:
`sim/src/system.cpp` `2aaf3e4d…`, `sim/src/memory.cpp` `0f44e6f4…`,
`sim/src/sim.hpp` `9f4586ef…`, `sim/bin/dx100sim` `2556823908ba3b6b…`.
No historical config, trace, log or report was modified.
