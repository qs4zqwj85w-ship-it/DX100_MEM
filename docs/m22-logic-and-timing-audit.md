# M22 — Logic and Timing Audit

What the engine must compare, select, sort and route per cycle, taken from the
implemented control path (`sim/src/bulk.cpp`). No gate counts are inferred from
C++; the classification is structural — which operations are associative, which
are sequential, which are on a critical path — and every claimed cost is labelled.

Tags: **[M]** measured (simulator counters) · **[C]** derived from the code ·
**[A]** assumed modelling choice · **[?]** would need RTL to settle.

---

## 1. Inventory of logic operations

| # | operation | implementation (file:line) | class | relative cost [?] |
|---|---|---|---|---|
| 1 | window admission | `win_admit_one` (`bulk.cpp:1031`) | 1 element/visit, `org_lanes` = 4 | small |
| 2 | **seal / grouping** | `win_seal` (`:1137`), `win_seal_fused` (`:1452`) | **sort of W elements by `(va_line, id)`** | **largest sequential block** |
| 3 | dedup (collapse equal lines) | implicit in the sort: equal `va_line` → one `LineReq` with `first`/`count` | post-sort run detection | small *given* (2) |
| 4 | page mask build | `dir_mask[s] \|= 1ull << (vl & 63)` (`:1249`) | 64-bit OR per line | trivial |
| 5 | page directory probe | `dir_vpn[vpn % 512] != vpn` (`:1243`) | **direct-mapped**: 1 read + 1 tag compare | trivial (no CAM) |
| 6 | page-run open | `win_open_cohort` (`:1349`) | linear pointer scan of `order` until `vpn` changes | sequential, O(run) |
| 7 | cohort select | `win_issue_sched` (`:1384`) | scan ≤ `page_cohorts` (2) entries, take oldest ready | 2-way priority |
| 8 | slot arbitration | `win_emit` (`:1282`), `tgt_reserve` (`:1275`) | free-list pop + reserved-share compare | small |
| 9 | dedup → request formation | `win_emit` per line | 1 request per distinct line | small |
| 10 | address generation | `base + index × b`, `ag_lanes` = 8 | 8 × 64-bit multiply-add | medium |
| 11 | response matching | `info_[slot]` direct index; `seq` for OOO detection | **no CAM** — slot id is the index | trivial |
| 12 | fanout | `push_fan` (`:1824`), `win_fanout_fused` (`:1595`) | 64 B copy + dependant walk, `fanout_lanes` = 4 | medium (wire-heavy) |
| 13 | completion / retirement | `pending`/`retired` per batch | counters + equality | trivial |
| 14 | descriptor fetch | `BulkDesc::decode`, 64 B line | 1 line, 1 decode | trivial |

**There is no CAM anywhere in the implemented engine.** Everything that looks
associative is either (a) replaced by sorting (dedup), (b) direct-mapped with a
tag compare (page directory), or (c) direct-indexed by a physical id (slot →
request record, response matching). That is a genuine architectural strength and
it materially reduces the logic estimate relative to a naive "tag-compare
everything" design.

## 2. The one sequential block: seal / grouping

The model sorts W elements with `std::stable_sort` on a 64-bit compound key and
charges one visit per entry (`org_backlog += b.n_ent`, `bulk.cpp:1253`). Its cost
is therefore **W element-visits per window**, not `W log W` — the model assumes a
grouping pass, not a comparator network.

**This is the right assumption and it is implementable**: a streaming pass that
hashes `va_line` into a line-record slot, appends the element to that line's run
(next-pointer or start/count), and detects a new line on tag mismatch — one
element per cycle, no comparators. The fused representation already stores
exactly the fields such a pass produces (line, position, offset) and needs no
permutation array at all.

| quantity | value | source |
|---|---|---|
| elements per window | 8 192 (8K) / 16 384 (16K) | [M] `bulk_win_occ_max` |
| seal cost at 1 element/cycle | 8 192 / 16 384 cycles per window | [C] |
| windows per 131 072-element run | 16 | [M] |
| **seal hidden by the other buffer's drain?** | **yes** | [M] see below |

Measured overlap: on w4_duplow the mean gap from "sealed" to "processing start" is
**292 671 cycles** [M] — the seal of window *n+1* completes long before window 2n
has finished issuing, because issuing is DRAM/TLB limited. The seal is a
throughput term that hides in the shadow of the memory system, not a latency term.

## 3. Page-run logic: linear scan, one cycle per install

`win_open_cohort` starts at `open_pos` and advances `end` while the page number is
unchanged, the comment noting that "the sealed order already knows" the run
boundary. It charges exactly **one cycle per run install** (`sched_cycles++`,
`sched_busy_` deasserted the next cycle) and counts the pointer steps separately
(`sched_scan`).

| workload | runs opened | scheduler switches | install cycles (= runs) | source |
|---|---:|---:|---:|---|
| w4_duplow | 32 224 | 25 267 | 32 224 | [M] "scheduler opens / switches", `C_bulk_sched_switches` |
| w2_clustered | 512 | 871 | 512 | [M] |
| w1_uniform | 512 | 949 | 512 | [M] |
| w5_sequential | 262 | 474 | 262 | [M] |

The scan is 1 pointer step/cycle over an array that is already in page order, so
it is a streaming read of `order`, perfectly pipelineable, and it does not touch
the issue port except for the single install cycle. Switches exceed runs because a
switch is counted per deferral/resume event, not per run. **This is cheap logic doing
the work that the (dead) page directory looks like it should do.**

## 4. Critical paths

Ordered by what would set the clock in a real build. No FO4 or gate counts are
claimed; the ranking is structural.

| path | chain | pipelineable? | risk |
|---|---|---|---|
| **fanout** | 64 B line read → 64 B write mux → dependant walk at 4 elements/cycle | yes, but wide: 512 bits/cycle | **highest** wire/port risk [?] |
| **address generation** | 64-bit `base + idx × b` per lane × 8 lanes | yes (2–3 stages) | low–medium |
| seal/grouping | hash → tag compare → record write, 1 element/cycle | **is** the pipeline | low (throughput, not frequency) |
| slot arbitration | free-list head + reserve compare → grant | yes (1 cycle) | low |
| cohort select | 2-way scan + ready test | yes (1 cycle) | low |
| page-directory probe | 1 SRAM read + tag compare | 1 cycle | **none** — and it is not on the functional path (§6.1 of the storage report) |
| response match | `info_[slot]` index + valid bit | 1 cycle | none |
| buffer switch | state write + pointer swap | 1–2 cycles | none |

**Nothing in this list needs a deeper pipeline than the L1/L2 access it feeds.**
The engine's own control is shallow; the memory system's latency (300-cycle walks,
26-cycle DRAM misses) dominates by two orders of magnitude.

## 5. Where the time actually goes (measured)

From the readiness timeline `[14]` of the M21 cells, engine-active cycles by state:

| state | w4_duplow | w1_uniform | interpretation |
|---|---:|---:|---|
| A: capacity free, no ready index element | 73 (0.0 %) | — | index pipeline rarely starves |
| B: ready index, target capacity refused | 238 988 (4.7 %) | — | slot pressure |
| C: capacity free, sealed line waiting, no issue | 4 776 368 (94.3 %) | — | **issue blocked** |
| — C1: **refused by translation (dTLB)** | 4 746 336 (93.7 %) | — | translation |
| — C2: no window to fill | 26 117 (0.5 %) | — | buffer turnover |
| — C3: port budget / run install | 3 915 (0.1 %) | — | scheduler |
| D: target request issued | 41 818 (0.8 %) | — | actual issue |

This is the timing answer that matters: **94 % of the engine's active cycles on
the workload where it wins most are spent waiting for the work to become
issuable** — and 93.7 % of the run is dTLB refusal, not any engine-internal
structure. Address generation, directory lookup, dedup, page selection,
arbitration, response matching and buffer allocation together account for the
0.1–0.8 % of cycles in states B, C3 and D.

The architectural reading: **Bulk's benefit is not that it computes faster, it is
that it makes translation and DRAM see a friendlier stream.** The scheduler's
`resume_at` mechanism (deferred runs) is precisely how it converts a 300-cycle
walk from a stall into an event.

## 6. Pipelining and latency implications

| requirement | why | cost |
|---|---|---|
| seal must overlap the other buffer's issue | otherwise W cycles are exposed per window | already true, measured (§2) |
| fanout must sustain 4 elements/cycle from one 64 B line | `fanout_lanes` = 4 | a 64 B × 4 mux per cycle; bank the line across 4 SRAM macros if needed |
| AG must produce 1 target/cycle | `eng_issue` = 4 requests/cycle max, 8 AG lanes | 8 multiplier-accumulators |
| index operand must be available every cycle | §6.2 of the storage report | 8 KiB line buffers or a dedicated L2 port |
| cohort resume must not block | else a dTLB refusal stalls the run | 2-entry `resume_at` compare — trivial |

**The only real timing risk is the fanout datapath and the index operand supply.**
Both are plumbing, not control complexity, and both have a concrete fix
(4-way banking; 2 × 64-line buffers).

## 7. What this section does not claim

* No gate counts, no FO4 numbers, no frequency. The ranking above is structural.
* The model's `org_lanes = 4` (four visits/cycle) is optimistic by 4× for a
  one-element-per-cycle admission stage; M14 measured that the affected phase is
  0.4 % of cycles [M], so it cannot change any conclusion here.
* `page_order=0` degenerates a run to a single line by construction, so the
  scheduler logic is exercised in both the "one run" and "many runs" regimes; the
  M21 ablation shows the cycles move by 3× on w4 [M], which is the behavioural
  proof that this logic is live.
