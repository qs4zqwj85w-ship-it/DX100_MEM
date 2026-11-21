# M23-G2C — SIMT DRAM queue starvation test

**Question.** Under ideal translation, do the Orkut BFS and ogbn-products train
SIMT request streams starve the DRAM, or saturate its service capacity?

**Answer.** Both **saturate it**. In 5.77 M and 15.32 M cycles the DRAM queue is
empty for **35 cycles** in each run — all of them in the first ~800 cycles of
ramp-up — and for the rest of the run every cycle is either starting a command
(8.9 % / 7.7 %) or blocked with both banks still busy (91.1 % / 92.3 %). There is
no third state: the `eligible-but-waiting` guard reads 0 in both cells. Arrivals
match the model's own sustained command rate to **99.4 % / 99.2 %**, the queue
sits at an average depth of **61.6 / 61.7** commands (max 63), and a queued
command waits **692 / 806 cycles** (max 1 678 / 1 689) before a bank frees up.

The workloads are therefore **DRAM-service-limited, not DRAM-starved**, and the
service time is row-conflict dominated: the mean command occupies a bank for
**22.3** (Orkut, 30.7 % row hits) or **25.9** (ogbn, **0.78 %** row hits)
cycles, against a 14-cycle row hit and a 26-cycle row miss. Neither workload is
waiting for requests; requests are waiting for banks.

---

## 1. Instrumentation

The frozen DRAM model already exported most of what this test needs — the
`dram_diag` block of M11.2 supplies the queue-depth sum (`dram_q_occ_sum`), the
per-cycle depth histogram (`dram_q_h0..h15` in log2 buckets), empty-queue cycles
(`dram_starved_cycles`), bank-busy stalls (`dram_blocked_cycles`), command
cycles (`dram_svc_events`), and the arrival→issue wait (`dram_wait_sum`,
`dram_wait_max`, `dram_wait_h0..h15`). Three requested quantities were not
represented:

| requested | before | now |
|---|---|---|
| DRAM request arrivals per cycle | only the run total (`dram_reads`) | `dram_arrivals`, `dram_arrive_cycles`, `dram_arrive_multi_cycles` — counted at the enqueue site, in the cycle the request actually enters the queue |
| command completions per cycle | `dram_svc_events` (cycles a command started) | unchanged, re-reported as `completions/cycle` |
| % cycles with eligible requests waiting | not represented at all | `dram_elig_wait_cycles` — a guard for the state "queue non-empty, a candidate's bank is free, nothing issued" |

The additions are 6 fields in `DramStats` (`sim/src/sim.hpp`), 10 lines in
`MemorySystem::l2_probe` and `dram_tick` (`sim/src/memory.cpp`), and 4 exports in
`collect_mem_stats` (`sim/src/system.cpp`). All of it is gated on the existing
`Config::dram_diag`, all of it is written **after** the arbitration decision, and
none of it is read by a probe, an arbitration, a replacement or a cycle decision.
`MemorySystem::dram_tick` still contains exactly one `return` per state; the
decision expression itself is untouched.

**Inertness, proved by rerun rather than asserted.** The two cells were run on
the instrumented binary and diffed against the M23-G2A logs of the same traces,
which came from the pre-instrumentation binary:

```
$ diff build/wm/m23g2a/g2a_orkut_m64.txt build/wm/m23g2c/g2c_orkut.txt
131a132,134
> dram_arrivals=513650
> dram_arrive_cycles=513473
> dram_arrive_multi_cycles=145
190a194
> dram_elig_wait_cycles=0

$ diff build/wm/m23g2a/g2a_ogbn_m64.txt build/wm/m23g2c/g2c_ogbn.txt
131a132,134
> dram_arrivals=1173937
> dram_arrive_cycles=1173834
> dram_arrive_multi_cycles=71
190a194
> dram_elig_wait_cycles=0
```

Four added lines each, nothing else: **cycles, every architectural counter,
every completion hash and `conservation=OK` are byte-identical to the
uninstrumented baseline** (5 766 215 and 15 323 803 cycles; 32 873 600 and
75 131 968 DRAM bytes). `./bin/dx100_tests` → **4 735 checks, 0 failures** on the
instrumented binary.

---

## 2. Measurements

Frozen configuration throughout: `R` 128, `l1_mshr` 128, `l2_mshr` 64, 2 banks x
4096 rows, `t_hit` 14 / `t_miss` 26, FCFS, `dram_q` unbounded, `warps_per_core` 4,
`lanes` 4, `ideal_translation = 1`. No architecture parameter was changed.

| | Orkut BFS | ogbn train |
|---|---|---|
| cycles | 5 766 215 | 15 323 803 |
| DRAM commands (= arrivals = completions) | 513 650 | 1 173 937 |
| **arrivals / cycle** | **0.0891** | **0.0766** |
| cycles with >= 1 arrival | 513 473 (8.90 %) | 1 173 834 (7.66 %) |
| cycles with >= 2 arrivals | 145 (0.0025 %) | 71 (0.0005 %) |
| **completions / cycle** | **0.0891** | **0.0766** |
| **queue occupancy, average** | **61.65** | **61.70** |
| queue occupancy, peak | 63 | 63 |
| **cycles with empty queue** | **35 (0.0006 %)** | **35 (0.0002 %)** |
| cycles blocked (queue non-empty, both banks busy) | 5 252 531 (91.09 %) | 14 149 832 (92.34 %) |
| cycles issuing a command | 513 650 (8.91 %) | 1 173 937 (7.66 %) |
| cycles with a candidate eligible but unissued | **0 (0 %)** | **0 (0 %)** |
| bank-busy (both banks, of 2 x cycles) | 99.41 % | 99.23 % |
| **average queue wait, arrive→issue** | **692.1 cycles** | **805.5 cycles** |
| maximum queue wait | 1 678 | 1 689 |
| row hits / conflicts | 157 554 / 356 094 | 9 199 / 1 164 736 |
| row-hit rate | 30.67 % | 0.78 % |
| mean service per command | 22.32 bank-cycles | 25.91 bank-cycles |
| sustained command capacity (2 banks / service) | 0.0896 /cycle | 0.0772 /cycle |
| **arrivals as % of that capacity** | **99.41 %** | **99.23 %** |
| DRAM throughput | 5.701 B/cycle (2.28 GB/s) | 4.903 B/cycle (1.96 GB/s) |

### 2.1 Queue occupancy histogram

Per-cycle depth, log2 buckets, from `dram_q_h*`:

`log2_bucket` puts depths 0 and 1 in the first bucket, so the empty-queue cycles
and the 26 depth-1 cycles share it.

| depth | Orkut cycles | ogbn cycles |
|---|---|---|
| 0-1 | 61 (0.001 %) | 61 (0.000 %) |
| 2-3 | 52 (0.001 %) | 52 (0.000 %) |
| 4-7 | 106 (0.002 %) | 106 (0.001 %) |
| 8-15 | 198 (0.003 %) | 210 (0.001 %) |
| 16-31 | 394 (0.007 %) | 420 (0.003 %) |
| **32-63** | **5 765 405 (99.986 %)** | **15 322 955 (99.995 %)** |

The two histograms are the same shape: a short ramp (the 811 cycles before the
queue reaches 32) and then the entire run pinned in the top bucket. Average 61.6
of a 63 observed maximum, for millions of cycles. This is a backlog, not a
pipeline that occasionally queues.

### 2.2 The cycle partition

`dram_tick` has exactly three exits per cycle — queue empty, no bank free, or a
command issued — so the states are exhaustive and mutually exclusive. Measured:
`empty + blocked + issued = cycles + 1` in both cells (the `+1` is the tick at
cycle 0, before any request exists; the loop returns one past the last ticked
cycle). The fourth state, a free bank with nothing issued, is zero.

| | Orkut | ogbn |
|---|---|---|
| empty (starved) | 0.0006 % | 0.0002 % |
| blocked (waiting for a bank) | 91.09 % | 92.34 % |
| issued (service) | 8.91 % | 7.66 % |
| eligible but unissued | 0 % | 0 % |

---

## 3. Classification

**Both workloads: DRAM-service-limited.** Not "mixed", and not starved.

The test for starvation is whether the queue ever runs dry, and it does not: 35
empty cycles in each run, all inside the first 800 cycles, after which the DRAM
has a 60-deep backlog it never works off. The test for saturation is whether the
command rate reaches what the banks can sustain, and it does: arrivals are
99.4 % / 99.2 % of `2 banks / mean service`, with both banks busy 99.4 % /
99.2 % of the time. A workload cannot be both starved and at 99 % of service
capacity; these are at the second.

The reason the ceiling is where it is, is the **address stream, not the
scheduler**: the model can start one command per cycle, so the binding quantity
is how long each command holds its bank. Orkut gets a row hit 31 % of the time
(22.3 bank-cycles/command); ogbn gets one 0.78 % of the time (25.9). With 2
banks that is one command every 11.2 / 12.95 cycles — the 5.70 / 4.90 B/cycle
observed. Compare M23-G0's bounds for this frozen model: 9.143 B/cycle at
sustained row hits, 4.923 at all conflicts. ogbn is within 0.4 % of the
all-conflict bound; Orkut sits between the two in proportion to its hit rate.

### 3.1 Waiting for a bank vs not yet admitted to the queue

These are different populations and the run keeps them apart:

- **Waiting for bank availability** (in the queue, admitted, not yet served):
  91-92 % of all cycles are of this kind, and every command spends an average of
  692 / 806 cycles in it. It is caused entirely by the other bank's command still
  occupying its bank: `dram_blocked_cycles` counts exactly the cycles in which a
  non-empty queue had no candidate whose bank was free.
- **Not yet admitted to the DRAM queue** (upstream of it): the coalescer holds
  formed lines with no free `R` slot in **94.7 % / 98.0 %** of cycles, and the
  64-entry L2 MSHR pool refuses **62.8 / 63.7 allocations per cycle** over the
  run. That is a large, continuous population — and it never produces a DRAM
  idle cycle.

They are linked, not competing: an L2 MSHR is allocated exactly when a command is
enqueued and released when that command's fill returns, so L2 MSHR occupancy is
the queue depth plus the fill tail (M23-G2A measured 63.9 occupied MSHRs against
a 61.6-deep queue). The refusals are the full queue pushing back through the
cache hierarchy — the *consequence* of the backlog, not a shortage of work. This
is also why M23-G2A found that raising the pool to 128 buys only 0.5-0.9 %: it
shortens the line in front of the queue, not the service time behind it.

**What would move these numbers:** fewer row conflicts (a different address
mapping or a larger row buffer) or more banks. More outstanding capacity, a
wider issue, or a bigger L2 MSHR pool cannot — each of those is upstream of a
queue that is already 62 commands deep.

---

## 4. Verification and correctness

| check | result |
|---|---|
| `conservation=OK`, `conservation_ok=1`, no violations | 2/2 |
| **cycles and every pre-existing counter identical to the uninstrumented baseline** (`diff` of the full `--all` dumps) | 2/2 |
| `completed == requests_generated` | 2/2 |
| `dram_bytes == 64 x dram_reads` | 2/2 |
| `arrivals == dram_svc_events == dram_reads` (every arrival is served, every command completes) | 2/2 |
| `starved + blocked + issued == cycles + 1` | 2/2 |
| `elig_wait_cycles == 0` | 2/2 |
| `arrive_cycles <= cycles` | 2/2 |

**Two independent identities hold exactly**, which is why the wait and occupancy
numbers can be trusted rather than merely averaged:

- `dram_wait_sum == dram_q_occ_sum` to the cycle in both cells (355 474 431 and
  945 562 740). Summed sojourn time equals the area under the queue length — the
  queueing identity — and it only holds exactly if the queue is sampled
  consistently and starts and ends empty.
- `dram_q_occ_sum / dram_svc_events` equals the measured mean wait to two
  decimals (692.06 and 805.46): Little's law on the same counters.

---

## 5. Limitations

- The DRAM model has **no data bus** and starts at most one 64 B command per
  cycle (M23-G0). "Service capacity" here is a command rate derived from the
  model's own bank timings (14 / 26 cycles), not a bandwidth figure; 5.70 and
  4.90 B/cycle are what this model yields, not what a GPU would.
- `ideal_translation = 1` by request. With real translation these traces are
  dTLB-bound and the DRAM is far less loaded (M23-B/C/D); nothing here describes
  that regime.
- One core, 4 warps, 4 lanes, one requester class reaching the DRAM, so there is
  no cross-requester interleaving to break up row locality.
- "Completions per cycle" is counted at the command start (`dram_svc_events`);
  the model has no separate completion event, and every started command
  completes.
- **The pre-M23-G2C binary was rebuilt in place.** Its SHA-256 is recorded below
  (`…2556823908ba3b6b`) along with the three source hashes it was built from, and
  the two cells above reproduce its output byte-for-byte apart from the four
  added lines, but the old binary itself no longer exists on disk.

---

## 6. Files

| path | what |
|---|---|
| `docs/m23-g2c-dram-queue-starvation.md` | this report |
| `build/wm/m23g2c_run.sh` | two-cell runner (reuses `configs/m23g2b_base.cfg`; no new config) |
| `tools/m23g2c_extract.py` | extractor, partition checks, histogram |
| `build/wm/m23g2c/g2c_{orkut,ogbn}.txt` | raw logs (instrumented) |
| `build/wm/m23g2c/summary.csv`, `queue_hist.csv`, `checks.txt` | tabulated results |
| `build/wm/m23g2c/hashes.txt` | source/binary hashes before and after, trace and log SHA-256 |
| `sim/src/sim.hpp`, `sim/src/memory.cpp`, `sim/src/system.cpp` | +6 fields, +10 gated lines, +4 exports |

Pre-G2C sources (the ones every historical number came from): `memory.cpp`
`0f44e6f4…`, `system.cpp` `2aaf3e4d…`, `sim.hpp` `9f4586ef…`, binary
`2556823908ba3b6b…`. Instrumented sources: `memory.cpp` `e4db3df3…`,
`system.cpp` `20516df4…`, `sim.hpp` `b31f66dd…`, binary `cba7f31a…`; test binary
`e6c4bfc4…`. Full values in `build/wm/m23g2c/hashes.txt`. No historical config, trace, log or report was modified, and the
instrumentation is inert with `dram_diag = 0`, which is the default.
