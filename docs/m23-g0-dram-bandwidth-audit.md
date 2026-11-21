# M23-G0 — DRAM bandwidth characterization and commercial GPU comparison

**Status:** audit and measurement only. No source file of the simulator, no frozen
configuration, no historical log, trace or report was modified. No bandwidth
throttle, no cache/TLB/translation/execution parameter and no DRAM timing constant
was changed. The only new code is an isolated microbenchmark driver
(`sim/src/g0bench.cpp`) that is **not** in the Makefile's `SRC` list, so
`bin/dx100sim` and `bin/dx100_tests` link and behave exactly as before
(`./bin/dx100_tests` → **4 735 checks, 0 failures**).

Evidence tags, as in the M11/M23 reports:

| Tag | Meaning |
| --- | --- |
| **[S]** | Verified in source; the quoted code is the authority. |
| **[M]** | Measured in this milestone; the log is in `build/wm/m23g0/`. |
| **[M*]** | Measured earlier, in an archived M18–M23 log in this tree. |
| **[D]** | Derived by arithmetic from [S]/[M]; the derivation is shown. |
| **[H]** | Hypothesis. Not tested here. |

---

## 1. What this milestone measured, and what it deliberately did not

The question is whether the current four-lane DX-100 model has DRAM bandwidth that
is disproportionate to the machine that generates its requests. Answering it needs
three separate things, kept separate throughout:

1. **The model's semantics** — what a "DRAM command" is, whether a data bus exists,
   and what the largest throughput is that the code can possibly express (§2, §3).
2. **The model's measured capability** — the throughput a designed, saturated
   access stream actually obtains (§4).
3. **The workloads' actual demand** — what the M18–M23 traces asked the controller
   for (§7).

Only (3) answers the overprovisioning question; (1) and (2) bound it. A number from
(2) alone would be an invented specification, and a number from (3) alone could not
distinguish "the DRAM is fast" from "the client is slow".

Not done, by instruction: no page-walk cache, no huge pages, no new translation
architecture, no TLB fix, no bandwidth throttle, no DRAM model change, and no
implementation of the experiment recommended in §10.

---

## 2. Part 1 — the existing DRAM implementation

Everything below is in `sim/src/memory.cpp` and `sim/src/sim.hpp`. There is one
memory model (`class MemorySystem`) and one arbiter; there is no separate DRAM file
and no second queue.

### 2.1 Where DRAM requests enter the controller

Exactly one site produces them: `MemorySystem::l2_probe`, `memory.cpp:395`

```cpp
dram_wait_.push_back(DQ{line, ++seq_, cycle, bank, row, client});
dram.max_q = std::max<u64>(dram.max_q, dram_wait_.size());
```

reached only after an L1 miss **and** an L2 miss **and** a successful L2-MSHR
allocation (`memory.cpp:361`). A request therefore enters the DRAM queue at the
cycle its L2 MSHR is allocated; `DQ::arrive = cycle`. L1 hits, L1 MSHR joins, L2
hits and L2 MSHR joins never reach it **[S]**. A PTE fetch by the page walker is a
normal request and reaches the same queue.

The only admission gate is at that allocation (`memory.cpp:354`), and it is disabled
in every published configuration because `dram_q = 0`:
`if (cfg_.dram_q && dram_wait_.size() >= cfg_.dram_q) { … return 2; }` **[S]**.

### 2.2 How requests are assigned to banks and rows

`memory.cpp:374–375`, from the **physical** line number (`pa_line`, 64 B units):

```cpp
u64  bidx = line;
if (cfg_.bank_hash) bidx ^= (line >> 6) ^ (line >> 12);
u32  bank = static_cast<u32>(bidx % cfg_.banks);
u32  row  = static_cast<u32>(((line / cfg_.banks) / lines_per_row) % cfg_.rows);
```

with `lines_per_row = row_bytes / 64`. In the frozen M23 configuration
(`banks = 2, rows = 4096, row_bytes = 1024, bank_hash = 1`) this is
`bank = parity(pa_line ^ pa_line>>6 ^ pa_line>>12)` and
`row = (pa_line / 32) mod 4096`, i.e. **16 lines per bank-row**, a full row cycle
every 131 072 lines (8 MiB). There is no channel, bank-group or column dimension
**[S]**. A diagnostic map switch (`dram_map`, default 0 = production) exists at
`memory.cpp:378–393` and is never set by a published config.

### 2.3 How FR-FCFS selects a command

`MemorySystem::dram_tick`, `memory.cpp:529`. Per cycle, one pass over the whole
queue:

* skip entries whose bank is not ready — `if (bank_ready_[q.bank] > cycle) continue;`
* among the rest, prefer a row hit (`bank_open_[bank] && bank_row_[bank] == q.row`),
  strictly, regardless of age;
* inside a class, earliest `arrive` wins; `DQ::seq` is filled but never read.

There is no aging, no round-robin, no per-bank queue, no window and no fairness
bound. Removal is `dram_wait_.erase(begin() + best)` — arbitrary position **[S]**.
Row outcomes are counted three ways: `row_hits`, `row_conflicts` (bank open,
different row) and *cold activations* (bank closed), the last counted in neither, so
`reads − hits − conflicts` is the cold-activation count **[S]**.

### 2.4 What a command is, and what it transfers

A command is **one request for one 64-byte line**. `memory.cpp:594`:

```cpp
dram.bytes += LINE_BYTES;                        // LINE_BYTES = 64 (sim.hpp:27)
```

There is no burst length, no partial line, no write, no byte mask and no column
address. `dram.bytes` is therefore exactly `64 × dram_reads`; verified for all 45
benchmark cells (§4.2) **[M]**. Because the byte count is credited at *issue*, it
is "bytes requested from the controller", not "bytes observed on a bus" — there is
no bus to observe (§2.7).

### 2.5 Are command issue and data transfer modelled separately?

**No.** One statement does both (`memory.cpp:595–596`):

```cpp
const u64 lat = (open ? cfg_.t_hit : cfg_.t_miss) + cfg_.link;
bank_ready_[q.bank] = cycle + lat;
```

The per-command cost is a single scalar that contains activation, access, data
return and the link, undivided (`t_hit = 14`, `t_miss = 26`, `link = 0` in the M23
configurations). There is no separate data-return phase, no data-bus occupancy
interval, and no completion notification distinct from the latency value: the L2
MSHR is released at `cycle + lat + l2_latency` (`memory.cpp:601–610`).

### 2.6 Is there a global data-bus or channel bandwidth constraint?

**No.** No code path counts bytes per cycle, no resource is consumed by a transfer,
and no constant states a bus width or a data rate. `bank_ready_` is the only
throughput-carrying state, and it is per bank. The words "bus", "channel",
"bandwidth" and "GB/s" do not appear in `dram_tick` **[S]**.

### 2.7 Can multiple banks transfer data concurrently?

Two distinct answers, and conflating them is the main trap in reading these logs:

* **Bank state: yes.** Each bank has its own `bank_ready_`, `bank_row_`, `bank_open_`,
  so both banks are busy at once after two commands, and their busy intervals
  overlap freely **[S]**.
* **Issue: no.** `dram_tick` returns after serving one command, so the model issues
  **at most one command per cycle in total**, independent of `banks` **[S]**.

The benchmark confirms the first directly: in pattern A both banks are continuously
occupied (`bank_utilization = 1.000` at K ≥ 4, §4.3) while only 0.143 commands per
cycle are issued.

### 2.8 How bank timing constrains throughput

A bank accepts a command, then refuses every command for that bank for `lat`
cycles. So the per-bank command **period** is 14 cycles for a row hit and 26 for a
row conflict, and the achievable command rate is
`banks / period`, not `banks` commands per cycle. Row activation therefore costs
throughput twice: the bank is unavailable 12 cycles longer, and every activation is
a command that could have been a 14-cycle hit.

Two further limits are structural rather than parametric: the one-command-per-cycle
rule (§2.7) and the fact that a hit and a miss cannot overlap *on the same bank*.

### 2.9 Are DRAM latency and throughput separate concepts here?

**Only weakly.** In this model the per-command latency *is* the per-bank occupancy,
so the same constant sets both the response time of one request and the throughput
cost of the stream. There is no pipelining inside a bank (a second command cannot
start while the first is returning data), and no bus-level overlap between banks —
only *bank-level* overlap. Consequences:

* Throughput can be predicted exactly from the timing constants and the row-hit
  rate, with no hidden efficiency factor (§3).
* Latency cannot be improved without changing throughput and vice versa; a model
  change that makes DRAM "faster" must move `t_hit`/`t_miss`/`banks`, which moves
  both.
* Queueing delay is *not* part of `lat`: `dram_wait_sum` measures
  `cycle − arrive` (issue time minus L2-MSHR allocation), and the client-visible
  latency adds the cache latencies (§4.2).

### 2.10 The ten questions, in one table

| # | Question | Answer | Evidence |
| --- | --- | --- | --- |
| 1 | Where do requests enter? | `l2_probe`, `memory.cpp:395`, one site, after L1+L2 miss and L2-MSHR allocation at `:361`; gate at `:354`, disabled by `dram_q = 0` | [S] |
| 2 | Bank/row assignment? | `memory.cpp:374–375` from the PA line: `bank = parity(x ^ x>>6 ^ x>>12)`, `row = (x/32) mod 4096`, 16 lines per bank-row, no channel/column | [S] |
| 3 | FR-FCFS? | `dram_tick` `memory.cpp:529`: bank-ready filter, then strict open-row preference, then `arrive`; no aging or starvation bound | [S] |
| 4 | What is a command? | One 64 B line read; no burst, no partial line, no column, no write | [S] |
| 5 | Full line or partial? | Exactly one full 64 B line (`dram.bytes += LINE_BYTES`) | [S], [M] |
| 6 | Issue vs transfer separate? | No — one scalar `lat` covers both and occupies the bank | [S] |
| 7 | Global bus/channel limit? | None; no bus state and no bytes/cycle resource anywhere | [S] |
| 8 | Concurrent banks? | Busy: yes (per-bank timers). Issue: no — at most one command per cycle total | [S], [M] |
| 9 | Bank timing effect? | Per-bank period 14 (hit) / 26 (conflict); achievable rate = `banks / period` | [S], [D] |
| 10 | Latency vs throughput? | Not separable: latency is the bank occupancy that limits throughput; no in-bank pipelining, no bus overlap | [S] |

**Consequence for the rest of the report:** the simulator has **no directly defined
physical DRAM bandwidth.** It has a per-command transfer size (64 B), a per-command
issue limit (1/cycle) and per-bank occupancy. "Bandwidth" in this report always
means *derived* bytes per cycle, and the ceiling is computed from those constants,
never assumed from `commands_per_cycle`.

---

## 3. Part 2 — theoretical limits, derived from the implementation

Configuration: `banks = 2`, `t_hit = 14`, `t_miss = 26`, `link = 0`,
`row_bytes = 1024`, `rows = 4096`, `dram_q = 0`, `frfcfs = 1`, `bank_hash = 1`
(`configs/m23_m0.cfg` §1, reproduced verbatim in `configs/m23g0_bench.cfg`).

| Quantity | Value | Derivation |
| --- | --- | --- |
| Max commands issued per cycle | **1** | `dram_tick` returns after one service [S] |
| Bytes per command | **64** | `dram.bytes += LINE_BYTES` [S] |
| Max bytes/cycle by **issue** | **64** | 1 × 64 [D] |
| Max lines/cycle by **issue** | **1** | [D] |
| Max **sustained** lines/cycle, both banks, all row hits | **0.1429** | `banks / t_hit = 2 / 14` [D] |
| Max **sustained** bytes/cycle, all row hits | **9.143** | 2/14 × 64 [D] |
| Max **sustained** lines/cycle, both banks, all conflicts | **0.0769** | `2 / t_miss = 2 / 26` [D] |
| Max **sustained** bytes/cycle, all conflicts | **4.923** | 2/26 × 64 [D] |
| Max **sustained** bytes/cycle, one bank only | **4.571** | `1 / t_hit × 64` [D] |
| Max ACT/PRE overhead | 12 cycles per activation | `t_miss − t_hit` [D] |
| Refresh, writeback, R/W turnaround, bank groups, channels | **not modelled** | [S] |

Three things follow, and they are the load-bearing results of this section:

1. **Command-issue bandwidth is not the binding constraint anywhere in this model.**
   Even the best possible stream issues 0.143 commands/cycle, i.e. **7× below** the
   one-per-cycle issue limit. Any claim that the DRAM "can do 64 B/cycle" is true
   only of the issue port, which no access pattern can saturate.
2. **The sustainable ceiling is a bank-occupancy ceiling:** 9.143 B/cycle
   (row-local) to 4.923 B/cycle (row-hostile), a 1.86× spread driven purely by the
   row-hit rate.
3. **There is no separate data-transfer bandwidth to state.** Because issue and
   transfer share one scalar, the honest specification is
   "64 B per command, ≤ 1 command/cycle, bank occupied 14/26 cycles" — the numbers
   above are consequences of those three facts, not an independent bus width.

---

## 4. Part 3 — controlled microbenchmarks

### 4.1 Method

Instrument (all new, all isolated):

* `sim/src/g0bench.cpp` — a driver that constructs the unmodified
  `MemorySystem` and submits a designed sequence of VA lines, at most `K`
  outstanding (`K` = the request generator's independent-request window, the
  analogue of `R`/the MSHR pool). It calls `MemorySystem::submit` and `::tick`
  exactly as the SIMT front-end does, so every command, bank state, row state and
  byte count comes from the production code. Requests stop when `K` are in flight.
* `sim/configs/m23g0_bench.cfg` — the instrument configuration. **Every DRAM field
  is identical to the frozen M23 baselines**; three fields are instrument
  properties and are labelled as such in the file: `l1_mshr = l2_mshr = 256`
  (the measurement must be able to drive the controller harder than any published
  architecture can, or it could not separate "DRAM is the limit" from "the client
  is"), `ideal_translation = 1` (M23-F1's flag: no dTLB, no walk, no PTE traffic,
  so every command is demand traffic) and `dram_diag = 1` (M11.2, inert when off).
* `build/wm/m23g0_run.sh` — builds the driver and runs the matrix;
  `tools/m23g0_extract.py` — logs → `build/wm/m23g0/summary.csv`.

Patterns. Every request is a compulsory L1/L2 miss (verified: `l2_hits = 0`,
`l2_reads = dram_reads` in all 45 cells), so the measured stream is demand DRAM
traffic and nothing else.

| Pattern | Construction | Designed structure |
| --- | --- | --- |
| A | 32-line runs repeated on successive 8 MiB periods | one bank-row per bank, 16 lines each, row 0 → 100 % row hits |
| B | linear sweep, 100 000 lines | the M23 workloads' own order → 15/16 row hits |
| C | lines `v` and `v+32` on successive periods | same bank, rows 0 and 1 in turn → bank 1 idle |
| D | uniform random unique lines over a 1.6 M-line footprint | unpredictable bank/row → cold-activation-dominated |
| E | 16 independent linear streams on disjoint 8 MiB periods, round-robin | many unrelated streams | The driver prints `predicted_row_hit_rate`, a replay of per-bank row state over the
designed sequence using the same map formula. For `A`, `B` and `E` it matches the
measured `dram_row_hits/dram_reads` **to four decimal places** at every depth
(`A` 1.0000/1.0000, `B` 0.9375/0.9375, `E` 0.9960/0.9960), which is the independent
check that the designed pattern is the pattern the simulator executed **[M]**. Two
patterns deviate, and both deviations are results rather than errors: `D` predicts
0.0000 and measures 0.0000–0.030 as the window deepens (reordering converts a few
coincidences into hits), and `C` predicts 0.0000 and measures up to 0.998 (the
arbiter batches 32 same-row entries, §4.4).

Steady state: 100 000 requests per cell; the headline rate is measured between the
10 % and 90 % service marks (`steady_window_lines / steady_window_cycles`), so
startup (first request's translation + cache fill) and drain (the last command's
26-cycle bank occupancy) are excluded. Both are reported separately per cell
(`first_service_cycle`, `last_service_cycle`, `cycles`).

### 4.2 Integrity checks (all 45 cells)

| Check | Result |
| --- | --- |
| `dram_bytes == 64 × dram_reads` | **45/45** — the byte count is command-derived, not L2-derived |
| `l2_hits == 0` and `l2_reads == dram_reads` | **45/45** — every request reached the controller; no cache absorption |
| `walks == 0`, `pte_fetches == 0` | **45/45** — no translation traffic in the measured bytes |
| rerun reproducibility | `B, K=64` re-run is **byte-identical** to the swept log |
| totals | 4 500 000 commands, 288 000 000 bytes |
| `./bin/dx100_tests` | **4 735 checks, 0 failures** |

### 4.3 Measured throughput vs outstanding depth

Steady-state bytes per cycle (100 000 requests per cell; `K` = outstanding):

| K | A (one row) | B (linear) | C (alt. rows, 1 bank) | D (random) | E (16 streams) |
| --- | --- | --- | --- | --- | --- |
| 1 | 2.909 | 2.813 | 1.882 | 1.883 | 2.903 |
| 2 | 5.818 | 5.626 | 2.462 | 3.201 | 4.679 |
| 4 | **9.143** | **8.678** | 3.902 | 4.198 | 5.354 |
| 8 | 9.143 | 8.678 | 4.289 | 4.601 | 7.522 |
| 16 | 9.143 | 8.678 | 4.440 | 4.767 | 9.058 |
| 32 | 9.143 | 8.678 | 4.508 | 4.847 | **9.112** |
| 64 | 9.143 | 8.678 | 4.540 | 4.904 | 9.112 |
| 128 | 9.143 | 8.678 | 4.556 | 4.951 | 9.112 |
| 256 | 9.143 | 8.678 | **4.564** | **4.992** | 9.112 |

Row-hit rate and bank utilization, same cells:

| K | A hit / util | B hit / util | C hit / util | D hit / util | E hit / util |
| --- | --- | --- | --- | --- | --- |
| 1 | 1.000 / 0.32 | 0.938 / 0.32 | 0.000 / 0.38 | 0.000 / 0.38 | 0.996 / 0.32 |
| 4 | 1.000 / 1.00 | 0.938 / 1.00 | 0.800 / 0.50 | 0.000 / 0.85 | 0.996 / 0.59 |
| 16 | 1.000 / 1.00 | 0.938 / 1.00 | 0.966 / 0.50 | 0.002 / 0.97 | 0.996 / 0.99 |
| 64 | 1.000 / 1.00 | 0.938 / 1.00 | 0.992 / 0.50 | 0.008 / 0.99 | 0.996 / 1.00 |
| 256 | 1.000 / 1.00 | 0.938 / 1.00 | 0.998 / 0.50 | 0.030 / 1.00 | 0.996 / 1.00 |

Mean queue depth grows exactly with `K` (61.0 at `K = 64`, 252.7 at `K = 256`), i.e.
the generator, not the controller, sets the queue depth in every cell: the queue is
unbounded and the controller never refuses. Mean queueing wait rises with `K`
(1 cycle at `K = 1` up to 1 769 cycles at `K = 256` for pattern A) because a deeper
window adds requests behind an already-saturated bank; end-to-end latency tracks it
(22 cycles at `K = 1` to 1 790 at `K = 256`), which is the standard
latency–throughput trade-off of an open-loop load, not a scheduling defect.

### 4.4 What the measurements say

**The sustainable ceiling is reached and not exceeded.** Pattern A attains
**9.1429 B/cycle** at `K ≥ 4` and stays there to `K = 256`, which is exactly
`2 lines / 14 cycles × 64 B` (§3). The measured value equals the derived one to
four significant figures, so the timing constants and the 64 B command size are
consistent and there is no unmodelled inefficiency to find. The knee is at
`K = 4`: with two banks occupied 14 cycles each, four outstanding requests already
keep both busy (`4 / 28 = 0.1429`); added depth only lengthens the queue.

**A row-hostile stream costs 46 % of the ceiling.** Pattern D — random lines,
100 % conflicts after warm-up — saturates at **4.992 B/cycle** with
`bank_utilization = 1.000`, i.e. both banks continuously occupied, against the
all-miss bound `2/26 × 64 = 4.923` (the 1.4 % excess is the 3 % of accidental row
hits). So the entire "bank scheduling" penalty is the activation penalty
`9.143 → 4.923`; there is no additional arbitration loss, because the arbiter has no
other cost.

**Row alternation is recovered by visibility, not lost.** Pattern C is the
informative case. At `K = 1` the alternating-row stream gets **0 % row hits and
1.882 B/cycle** — the worst cell in the matrix. At `K = 256` the *same address
stream* gets **99.8 % row hits and 4.564 B/cycle**, exactly the single-bank
hit limit `64/14 = 4.571`. The queue lets FR-FCFS batch 32 same-row entries before
paying the activation, so what a naive reading would call "row thrashing" is a
*latency* problem (no reordering window) rather than a capacity problem. Pattern C
never exceeds half the ceiling because it only uses one bank — that is by design.

**Sixteen independent streams behave like one row-local stream** (9.112 B/cycle at
`K ≥ 32`, 392 rows touched), i.e. bank-level parallelism is available to any client
that can keep ~32 requests in flight, regardless of how the streams are related.

**Measured capability, summarized:**

| Stream | B/cycle | lines/cycle | % of 9.143 ceiling | % of 64 B/cycle issue limit |
| --- | --- | --- | --- | --- |
| A row-local, both banks | **9.143** | 0.1429 | 100 % | 14.3 % |
| B linear, 93.75 % hits | 8.678 | 0.1356 | 94.9 % | 13.6 % |
| E 16 streams | 9.112 | 0.1424 | 99.7 % | 14.2 % |
| D random, both banks | 4.992 | 0.0780 | 54.6 % | 7.8 % |
| C alt. rows, one bank | 4.564 | 0.0713 | 49.9 % | 7.1 % |

At the codebase's own clock convention (`system.cpp:285`, `clk_hz = 400e6`, used by
constant behind the printed `dram_gbs`), 9.143 B/cycle is **3.66 GB/s**.

---

## 5. Part 4 — commercial GPU normalization

Specifications, from vendor documentation:

| | A100 40 GB (SXM4) | H100 SXM (SXM5, 80 GB) |
| --- | --- | --- |
| SMs | 108 | 132 |
| Max resident warps / SM | 64 | 64 |
| Warp width (threads) | 32 | 32 |
| Max resident threads / SM | 2 048 | 2 048 |
| Peak memory bandwidth | 1 555 GB/s (HBM2) | 3 352 GB/s (HBM3) |
| Memory clock (data rate) | 1 215 MHz DDR | 1 313 MHz (5.3 Gbps effective) |
| Memory interface | 5 120-bit | 5 120-bit |
| Boost clock (used for /cycle) | 1 410 MHz | 1 980 MHz |

Sources: NVIDIA A100 datasheet tables (108 SMs; memory clock 1 215 MHz; 5 120-bit;
1 555 GB/s; boost 1 410 MHz) and NVIDIA H100 specifications (3.35 TB/s; Hopper
whitepaper memory table 5 120-bit HBM3, 2 619 MHz DDR = 3 352 GB/s; H100 SXM5 boost
1 980 MHz); maximum resident warps per SM = 64 for both compute capability 8.0 and
9.0 per the CUDA Programming Guide's compute-capability table and the Ampere/Hopper
tuning guides ("the maximum number of concurrent warps per SM remains … 64").

Derived metrics. Bytes/cycle uses the boost clock, so it is the *most favourable*
reading for the GPUs; a lower clock would raise the per-SM share.

| Metric | A100 40 GB | H100 SXM | Formula |
| --- | --- | --- | --- |
| Total resident warps | 6 912 | 8 448 | SMs × 64 |
| Total resident threads | 221 184 | 270 336 | SMs × 2 048 |
| Peak bytes/cycle (whole GPU) | 1 102.8 | 1 692.9 | GB/s ÷ boost GHz |
| Peak bandwidth per SM | 14.40 GB/s | 25.39 GB/s | GB/s ÷ SMs |
| Peak bandwidth per resident warp | **0.225 GB/s** | **0.397 GB/s** | per-SM ÷ 64 |
| Bytes/cycle per SM | **10.21** | **12.83** | see above |
| Bytes/cycle per resident warp | **0.1596** | **0.2004** | per-SM ÷ 64 |
| Bytes/cycle per resident thread | 0.00499 | 0.00626 | per-SM ÷ 2 048 |

**These are equal-share normalizations, not per-warp guarantees.** A real warp's
achievable bandwidth depends on coalescing, sector granularity, L2 partitioning,
access pattern and contention; no warp has a reserved slice of HBM. The numbers
answer only "how much DRAM does the machine provide per unit of declared
parallelism", which is exactly what is needed to judge whether a 4-lane model's DRAM
is proportionate.

Clock assumption, stated once: bytes/cycle = peak GB/s ÷ boost GHz. If the sustained
(sustained-clock) frequency is below boost, the per-cycle figures rise
proportionally, so a lower clock only strengthens the conclusion in §6.

---

## 6. Part 5 — DX-100 against those numbers, and capacity against generation

### 6.1 The declared machine

`configs/m23_m0.cfg`: `cores = 1`, `warps_per_core = 4`, `lanes = 4`,
`elem_bytes = 4`, `R = 128`. DX-100 therefore models **one core, 4 warps of 4 lanes
= 16 resident threads** with one shared memory hierarchy. Four lanes are **not** a
commercial warp (32 threads) and the modeled core is **not** an SM (64 warps,
2 048 threads); both facts are used explicitly below rather than assumed away.

### 6.2 Measured DX-100 bandwidth

| Metric | Value | Source |
| --- | --- | --- |
| Measured DRAM bytes/cycle (sustained ceiling) | **9.143** | pattern A, K ≥ 4 [M] |
| Measured DRAM lines/cycle | **0.1429** | [M] |
| Bytes/cycle per execution lane (4 physical lanes) | **2.286** | 9.143 ÷ 4 [D] |
| Bytes/cycle per modeled warp (4 resident warps) | **2.286** | 9.143 ÷ 4 [D] |
| Bytes/cycle per resident thread context (4 warps × 4 lanes) | **0.571** | 9.143 ÷ 16 [D] |
| GB/s at the codebase's 400 MHz convention | **3.66** | `system.cpp:285` [D] |
| Worst-case (row-hostile, both banks) | **4.992** | pattern D, K = 256 [M] |

(One clarity note on the geometry: `lanes = 4` is the datapath width (4 physical
lanes), `warps_per_core = 4` means four warp contexts are resident, each 4 lanes
wide, so the core holds 16 thread contexts behind 4 physical lanes. "Per lane" and
"per warp" therefore share the divisor 4 and both read 2.286 B/cycle; "per thread
context" divides by 16 and reads 0.571. The last row is the one that removes the
geometry from the comparison.)

### 6.3 Normalized comparison — and why no single ratio settles anything

| Normalization | DX-100 | A100 40 GB | H100 SXM | DX ÷ A100 | DX ÷ H100 |
| --- | --- | --- | --- | --- | --- |
| B/cycle, whole machine | 9.14 | 1 102.8 | 1 692.9 | 0.008× | 0.005× |
| B/cycle per SM / core | **9.14** | **10.21** | **12.83** | **0.90×** | **0.71×** |
| B/cycle per execution lane (4 on DX, 32-thread warp on GPU) | **2.29** | 0.160 | 0.200 | **14.3×** | **11.4×** |
| B/cycle per resident thread context | 0.571 | 0.00499 | 0.00626 | **114×** | **91×** |

Read the middle row first: **per modeled core, the DX-100 DRAM ceiling is 0.90× an
A100's per-SM share.** In other words the simulated memory system is provisioned at
roughly the strength of one commercial SM's slice of HBM — an entirely ordinary
number for one core.

The bottom rows are where the disproportion appears, and it is not a DRAM figure at
all: it is a *parallelism* figure. Attached to that per-SM-sized memory system is a
core with 16 resident thread contexts instead of 2 048, so the same bandwidth
divided by declared parallelism is 14× a commercial per-warp share and ~100× a
per-thread share. Any statement of the form "DX-100 has 14× a GPU's bandwidth per
warp" is therefore a statement about the ratio of threads to memory, not about the
DRAM model.

### 6.4 Available capacity vs actual request generation — the decisive distinction

Capacity is not the question; the question is whether the four-lane generator can
use it. Measured demand, from the archived M23 logs (all `[M*]`, recomputed in
`build/wm/m23g0/historical_utilization.csv`):

| cell | cycles | DRAM reads | DRAM bytes | B/cycle | bank util | row-hit | max queue | mean queue wait |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Criteo M0 | 28 511 774 | 86 886 | 5 560 704 | 0.195 | 3.85 % | 5.97 % | 5 | 5.6 |
| Criteo DX 2×8K | 15 778 298 | 103 182 | 6 603 648 | 0.419 | 7.34 % | 29.54 % | 62 | — |
| Criteo DX 2×16K | 12 273 324 | 103 617 | 6 631 488 | 0.540 | 9.40 % | 31.00 % | 62 | 57.0 |
| Criteo DX 2×32K | 8 942 139 | 106 675 | 6 827 200 | 0.764 | 13.13 % | 33.22 % | 62 | 64.3 |
| Orkut BFS M0 | 49 067 545 | 519 337 | 33 237 568 | 0.677 | 11.87 % | 29.82 % | 63 | 85.8 |
| Orkut BFS DX 2×16K | 16 714 538 | 651 309 | 41 683 776 | 2.494 | 36.53 % | 60.43 % | 63 | 185.6 |
| Orkut BFS DX 2×32K | 10 401 464 | 689 344 | 44 118 016 | **4.242** | **57.04 %** | 73.22 % | 63 | 273.3 |
| Orkut hub M0 | 41 463 846 | 680 163 | 43 530 432 | 1.050 | 17.39 % | 40.02 % | 62 | 94.3 |
| Orkut hub DX 2×16K | 17 209 462 | 819 949 | 52 476 736 | 3.049 | 43.28 % | 65.28 % | 63 | 207.5 |
| ogbn train M0 | 176 184 939 | 1 194 355 | 76 438 720 | 0.434 | 8.81 % | 0.14 % | 5 | 8.2 |
| ogbn train DX 2×16K | 139 088 324 | 1 369 035 | 87 618 240 | 0.630 | 12.17 % | 10.60 % | 62 | 23.8 |
| ogbn uniform DX 2×16K | 144 443 603 | 1 357 779 | 86 897 856 | 0.602 | 11.69 % | 9.33 % | 62 | 24.6 |

The best measured cell in the whole M18–M23 programme — Orkut BFS on DX 2×32K —
reaches **4.242 B/cycle, 46 % of the ceiling, 57 % bank utilization**. The Criteo
M0 cell reaches **2.1 % of the ceiling**. No published workload comes close to
saturating the controller.

Two different limits are visible in this table, and they must not be merged:

* **The M0 arm cannot generate parallelism at all.** `dram_max_q = 5`,
  `candidates/decision = 1.10`, `inflight_max = 13` (Criteo M0, `[M*]`): the
  controller sees one or two requests at a time, so FR-FCFS is a pass-through and
  its 5.97 % row-hit rate measures *starvation*, not a bad schedule.
* **The DX arms can, but do not fill the DRAM.** `dram_max_q = 62`,
  `inflight_max = 119–128`, `candidates/decision = 4.31–4.75` `[M*]`. Even so they
  spend 63–90 % of their cycles with both banks idle, because — as all the M23 work
  shows — the run is paced by translation and by the L1/L2 MSHR funnel, not by the
  DRAM.

**Can the four-lane generator saturate this DRAM if it tries?** Yes, and cheaply.
The measurements give the requirement directly: pattern A needs `K = 4`, pattern B
`K = 4`, pattern D (all misses) `K = 64–256` to reach 99 % of its asymptote, pattern
E `K = 32`. The architecture already declares `R = 128` outstanding requests and the
DX cells measured `inflight_max = 128` `[M*]`, i.e. **2–32× the depth needed**. The
DRAM is therefore over-provisioned relative to *what these traces ask for*, but it
is not over-provisioned relative to *what the front-end is able to ask for*: a
4-lane core that issues one line per instruction could demand 4 lines/cycle, or
~5.8 by Little's law at `R = 128` and ~22-cycle latency, versus a sustainable
0.143 lines/cycle. At peak issue the generator can oversubscribe the DRAM by 28–40×.

So the honest characterization is: **the DRAM is sized for a client roughly 30–40×
more request-hungry than the one this simulator's workloads contain, and 11–14×
larger per unit of declared parallelism than a commercial GPU's share — but it is
not larger than the theoretical issue rate of the modeled front-end.** It is not a
data-bus artefact (there is no bus); it is a consequence of `banks = 2`,
`t_hit = 14`, `t_miss = 26` and 64 B per command, which together give a per-SM-sized
memory system attached to a 16-thread core.

---

## 7. Limitations

**Model facts that bound every number above**

1. No data bus, no channel, no bank group, no column, no refresh, no writeback, no
   read/write turnaround. Bandwidth is derived from bank occupancy, not measured on
   a transfer resource.
2. One command per cycle in total, whatever `banks` is. Any configuration with
   `banks ≥ 2` is issue-limited only in the sense that it can never exceed 64 B/cycle
   — a bound no stream approaches.
3. A `row` is 16 lines and there is no column dimension, so "row locality" here
   means "consecutive lines in a 1 KiB bank-row", not DRAM-page spatial locality
   within an open row of thousands of bits.
4. Per-command latency is a single scalar (`14`/`26`), so latency and throughput
   cannot be varied independently. Any experiment that "scales bandwidth" by moving
   `t_hit`/`t_miss` also moves response time.
5. The address map is periodic with period 8 MiB, so lines 8 MiB apart share a
   `(bank,row)`; patterns A, C and E use exactly that property to build long
   repeated single-row streams out of *distinct* lines. This is a legitimate use of
   the model's map, but it is also a reminder that row aliasing at 8 MiB
   (M11 §3.4) is a model property, not a device property.
6. `dram.bytes` is credited at command issue, not at completion. It is a faithful
   count of 64 B commands (`bytes == 64 × reads`, 45/45) but not an observation of
   transferred data, because no transfer is modelled.

**Instrument caveats**

7. The benchmark config raises `l1_mshr`/`l2_mshr` to 256 and enables
   `ideal_translation`, so the measured ceiling is a *capability* number and must
   not be read as an achievable figure for any published architecture. The
   architectures' own outstanding capacity is quoted separately (§6.4).
8. `K` is the driver's window, not `R`; the driver has no instruction stream, so it
   measures what the memory system can do under demand, not what a program will ask
   for.
9. `bank_utilization` is reported against the first-to-last-service span and
   therefore reads marginally above 1.000 in saturated cells (1.0003, 1.0000): the
   final command's occupancy extends past the last service event. It is not an error
   in the model.

**Comparison caveats**

10. A commercial GPU's per-warp and per-thread shares are equal-share arithmetic on
    peak specifications at boost clock. They are not achievable per-warp bandwidths
    and no warp has a reserved slice of HBM.
11. A DX-100 lane is one thread, a warp here is 4 lanes and a core is 4 warps. Any
    ratio to a 32-thread warp or a 2 048-thread SM mixes execution models; §6.3
    reports per-SM, per-warp and per-thread rows precisely so the reader can see
    which one is being quoted.
12. The DX-100 numbers are simulator cycles at an arbitrary clock. The 3.66 GB/s
    figure uses `clk_hz = 400e6` because that is the convention already in
    `system.cpp`; it implies nothing about a fabricated design.

**Not tested [H]**

13. Whether a *device-realistic* controller (16–32 banks, 64–256 queue entries, a
    real bus, refresh) would show the same overprovisioning. This model cannot
    answer that: adding banks would multiply the ceiling without changing the
    client.
14. Whether any M23 workload would slow down if capacity were halved. §10 predicts
    which cells would and would not, and it is untested by design.

---

## 8. Direct answers to the eight required questions

**1. Does the current simulator have an explicit DRAM data-transfer bandwidth
limit?**
**No.** There is no bus, channel or bytes/cycle resource anywhere in the model. What
exists is an explicit *transfer size* (64 B per command, `memory.cpp:594`), an
explicit *issue limit* (one command per cycle, `dram_tick`), and explicit *per-bank
occupancy* (`bank_ready_[bank] = cycle + lat`, `memory.cpp:596`). Bandwidth is
emergent from those three. (§2.6, §2.10)

**2. If yes, what is its theoretical maximum in bytes/cycle?**
There is no declared bus bandwidth, so the question is answered for the two limits
that do exist: **64 B/cycle by command issue** (`1 × 64 B`), which nothing can
approach, and **9.143 B/cycle sustainable** with both banks continuously busy on row
hits (`2 lines / 14 cycles × 64 B`), falling to **4.923 B/cycle** if every command is
a row conflict. (§3)

**3. What is its experimentally measured sustainable bandwidth?**
**9.1429 B/cycle** (0.1429 lines/cycle) on a row-local stream with both banks busy,
stable from `K = 4` to `K = 256` outstanding and equal to the derived ceiling to
four figures. With the model's own 400 MHz convention that is **3.66 GB/s**. A
realistic linear stream reaches 8.678, a row-hostile random stream 4.992, and a
single-bank alternating-row stream 4.564 B/cycle. (§4.3, §4.4)

**4. How much does bank scheduling reduce throughput relative to the theoretical
limit?**
Against the 64 B/cycle issue limit, bank occupancy costs **7.0×** — that is the
whole difference, and no arbiter can recover it. Against the 9.143 B/cycle
sustainable ceiling, a 100 %-conflict stream costs **46 %** (4.923 B/cycle) and that
is the only scheduling-related loss the model can express, since the arbiter has no
other cost. The most striking scheduling effect runs the other way: row alternation
in one bank costs **79 %** at `K = 1` (1.882 B/cycle, 0 % hits) but only **50 %** at
`K = 256` (4.564, 99.8 % hits), because the queue lets FR-FCFS batch 32 same-row
entries. Scheduling loss here is therefore a *visibility* loss, not a policy loss.
(§4.4)

**5. How does DX-100's bandwidth per execution lane compare with commercial GPU
normalization?**
DX-100's ceiling is **2.286 B/cycle per lane** (9.143 ÷ 4) against **0.160 B/cycle
per resident warp** on A100 40 GB and **0.200** on H100 SXM: **14.3×** and **11.4×**
respectively. Per *modeled core* the same ceiling is 9.143 B/cycle against 10.21
(A100) and 12.83 (H100) B/cycle per SM — **0.90×** and **0.71×**, i.e. unremarkable.
The 14× appears only because a DX-100 core holds 16 resident thread contexts against
an SM's 2 048; per thread context the ratio is 114× (A100) and 91× (H100). These are
equal-share normalizations on vendor peak specifications at boost clock, not
achievable per-warp bandwidths.
(§5, §6.3)

**6. Is the current DRAM system potentially overprovisioned relative to the
four-lane request generator?**
**Yes relative to measured demand, no relative to peak issue capacity — and the gap
between those two statements is the finding.** The fastest published cell (Orkut BFS
DX 2×32K) uses 4.242 B/cycle = **46 %** of the ceiling at **57 %** bank utilization;
Criteo M0 uses **2.1 %**. Nothing in M18–M23 exceeds 57 % utilization, so for these
traces the DRAM has at least ~2× and up to ~47× headroom. That headroom is not
caused by the DRAM being oversized for its client's *ability*: patterns A and B
saturate it at `K = 4` and pattern D at `K = 64–256`, while the architecture already
provides `R = 128` and the DX cells measured `inflight_max = 128`. The real cause is
that the M23 workloads are paced   by translation and the L1/L2 MSHR funnel before they can become DRAM-bound (M23-F1
  removed translation entirely and found DX then 0.89–0.98× **slower** than M0: only
  once translation is free do the arms become memory-bound, and even then the
  controller stays far from saturation). So: overprovisioned for the workload, not
  for the machine. (§6.4)

**7. What is the most defensible next experiment for scaling DRAM capacity down?**
**A `banks` sweep, 2 → 1, on the frozen M23 workloads, with `t_hit`/`t_miss`,
`row_bytes`, `rows`, `bank_hash` and every client parameter held fixed.** Reason:
the model has no bandwidth parameter, so "capacity" can only be moved by `banks` or
by the timing constants, and the timing constants also change per-access latency,
which would confound the result. `banks = 2 → 1` halves the ceiling
(9.143 → 4.571 B/cycle) and halves bank-level parallelism while leaving the latency
of any single request and the address map's row structure untouched. It is also
cheap: the map already supports it (`dram_map = 2` exists, and `banks` is a plain
config key), so it needs no source change.

With a falsifiable prediction from §6.4: **Orkut BFS DX 2×32K (57 % utilization)
should slow down measurably, Orkut hub DX 2×16K (43 %) slightly, and Criteo and
ogbn (4–13 %) should be unchanged**, because their bank utilization is far below
50 % and their runs are translation/latency-bound. If instead *every* cell is
unchanged, the DRAM is not merely underused but irrelevant to the M23 conclusions,
which would be a stronger and more useful result than a slowdown. Report
`dram_busy_cycles`, mean queue wait and row-hit rate per cell as well as cycles, so
a null result can be attributed to idle banks rather than assumed.

A second, orthogonal variant — halving the *per-command occupancy* (`t_hit`,
`t_miss`) — should not be used first: it changes latency and throughput together,
so it cannot separate capacity from speed.

**Not implemented, per the brief.** No configuration, source or historical file was
changed to prepare it.

---

## 9. Reproduction

```bash
cd DX100_MEM

# 1. build the isolated instrument (does not touch bin/dx100sim or bin/dx100_tests)
clang++ -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Isim/src \
  sim/src/g0bench.cpp sim/src/memory.cpp sim/src/util.cpp sim/src/workload.cpp \
  -o sim/bin/dx100_g0bench

# 2. one cell
cd sim && ./bin/dx100_g0bench --config configs/m23g0_bench.cfg \
  --pattern B --count 100000 --outstanding 64 --seed 1

# 3. the full matrix (45 cells, ~5 min)
cd .. && bash build/wm/m23g0_run.sh

# 4. logs -> CSV
python3 tools/m23g0_extract.py      # build/wm/m23g0/summary.csv

# 5. regression
cd sim && ./bin/dx100_tests          # 4735 checks, 0 failures
```

Artifacts and hashes:

| Path | What it is | SHA-256 (when it is a file that must not drift) |
| --- | --- | --- |
| `sim/src/g0bench.cpp` | bench driver (new, not in `SRC`) | `ad24f1d405044a129fd33578914e45c17eb9f914769d131b500e9b234f023ff7` |
| `sim/configs/m23g0_bench.cfg` | instrument config | `df4af48c72156a332f9cff36684dd194ddc3c12eb2090f1163e4a8fd2f60d970` |
| `sim/src/memory.cpp` | **unmodified** DRAM model | `0f44e6f4ac89cc1886f5b8be9ad4ece8308f4a0ad658d6f10ba423b450d23715` |
| `sim/src/sim.hpp` | **unmodified** config/stats | `9f4586ef2a730c2ecfd3464b8e7e315000ae054f962f924372b7853899432bcc` |
| `build/wm/m23g0/` | 45 cell logs + `summary.csv` + `historical_utilization.csv` + `hashes.txt` | — |
| `build/wm/m23g0_run.sh`, `tools/m23g0_extract.py` | runner and extractor | — |

Source locations cited in this report, all verified against the hashes above:
`memory.cpp:354` (admission gate), `:361` (L2 MSHR allocation), `:374–375`
(bank/row map), `:395` (the only DRAM queue push), `:529` (`dram_tick`),
`:594` (bytes per command), `:595–596` (latency and bank occupancy),
`:601–610` (completion); `sim.hpp:27` (`LINE_BYTES`), `:478` (`DramStats`),
`:67` (DRAM defaults); `system.cpp:285` (`clk_hz = 400e6`, used at `:287`).

**Preservation.** No file under `build/wm/m23b_*`, `m23c_*`, `m23d_*`, `m23e_*`,
`m23f0/`, `m23f1/`, and no file under `docs/` or `configs/` that existed before this
milestone was modified; the only additions are the six artifacts listed above.
`docs/m11-dram-scheduler-audit.md` was re-verified against the current source (its
arbiter description still holds; its line numbers are 40–70 lines stale after the
M15/M23 additions, and §2 here cites the current ones).
