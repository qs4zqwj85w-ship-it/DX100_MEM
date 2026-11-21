# M9 double-buffer balance diagnostic — E3

Read-only investigation. No policy, default, architecture or published result changed.

## 1. Events, and where they are recorded

The window manager (`bulk.cpp`, `win_tick`) has exactly two buffers and one cursor each for
producing and consuming. The five events this question needs map onto the following code
points; all five are now stamped per window in `BulkStats::WinRec` at the event itself:

| Event | Source site | Record field |
|---|---|---|
| buffer allocated, filling starts | `win_tick` step (4), `nb.fill_start = cycle` | `alloc` |
| first / last element admitted | `win_admit`, after each successful admission | `first`, `last` |
| buffer sealed | `win_seal`, after the reason is chosen | `seal` (`why`: full / descriptor-end / partial) |
| **target processing starts** | `win_tick` step (3), `iss_ = i` | `issue` |
| **last target line submitted** | `win_emit`, on the successful issue path | `last_issue` |
| buffer released (all fills returned, all elements retired) | `win_tick` step (2) | `retire` |

A window is adopted for issue the first tick after it is sealed (`iss_ < 0` is required), so
`seal → issue` is the *buffer-ready wait*: how long a complete window sat ready while the
other buffer was still being processed. `issue → last_issue` is the issuing phase,
`last_issue → retire` is the drain (fills plus fan-out), and `issue → retire` is the
consumer's total occupancy of the buffer.

The previous records carried `alloc`, `seal` and `retire` only; `issue` and `last_issue` were
added for this diagnostic (two stores on existing paths, neither read by any decision). The
control cells reproduce their earlier cycle counts exactly: E0 W1 κ = 0.25 is still
**153 475** and W0 κ = 0.25 still **17 160**, and `dx100_tests` remains 4032 checks / 0
failures. Only six cells were run: E3 at W1 κ = 0.25/0.50/0.75 and W0 κ = 0.25, plus E0 at
W1/W0 κ = 0.25 for contrast.

Timeline notation below: `[elements / alloc / seal / issue / last_issue / retire]`.

## 2. Timelines — E3 (`win_policy = 2`, `batch_deadline = 4096`), 128 slots, seed 1

### W1, κ = 0.25 — run 125 581 cycles

| buffer | record | fill (alloc→seal) | ready wait (seal→issue) | issuing (issue→last) | drain (last→retire) | processing |
|---|---|---|---:|---:|---:|---:|
| 1 (8 192 elem, full) | [8192/586/8200/8201/89867/90105] | 7 614 | 1 | 81 666 | 238 | 81 904 |
| 2 (4 184 elem, descriptor end) | [4184/8201/13646/90105/124491/125581] | 5 445 | **76 459** | 34 386 | 1 090 | 35 476 |

Buffer 2 was sealed at 13 646 while buffer 1 was still processing, and processing of buffer 1
ended (90 105) with buffer 2 long since sealed. Buffer 2 starts issuing in the **same cycle**
buffer 1 is released.

### W1, κ = 0.50 — run 94 035 cycles

| buffer | record | fill | ready wait | issuing | drain | processing |
|---|---|---|---:|---:|---:|---:|
| 1 (8 192, full) | [8192/586/8226/8227/92873/93111] | 7 640 | 1 | 84 646 | 238 | 84 884 |
| 2 (68, descriptor end) | [68/8227/10291/93111/93172/94035] | 2 064 | 82 820 | 61 | 863 | 8 724 |

### W1, κ = 0.75 — run 95 194 cycles

| buffer | record | fill | ready wait | issuing | drain | processing |
|---|---|---|---:|---:|---:|---:|
| 1 (4 052, descriptor end) | [4052/586/3898/3899/95051/95194] | 3 312 | 1 | 91 152 | 143 | 91 295 |

One window: the operation is smaller than W, so there is no second buffer to overlap with.

### W0, κ = 0.25 (streaming, 11 550 dedup merges, 770 target lines) — run 18 142 cycles

| buffer | record | fill | ready wait | issuing | drain | processing |
|---|---|---|---:|---:|---:|---:|
| 1 (8 192, full) | [8192/586/8200/8201/15388/17105] | 7 614 | 1 | 7 187 | 1 717 | 8 904 |
| 2 (4 128, descriptor end) | [4128/8201/13325/17105/17182/18142] | 5 124 | 3 780 | 77 | 960 | 1 037 |

## 3. Summary

Mean over the windows of each cell. "Producer blocked" is
`cycles with work but no filling window`, i.e. cycles in which the producer had elements and
no buffer could accept them.

| workload | policy | mean fill | mean processing | mean ready wait | producer blocked |
|---|---|---:|---:|---:|---:|
| W1 κ = 0.25 | E3 | 6 530 | **58 690** | 38 230 | 0 |
| W1 κ = 0.50 | E3 | 4 852 | **42 904** | 41 410 | 0 |
| W1 κ = 0.75 | E3 | 3 312 | **91 295** | 1 | 0 |
| W0 κ = 0.25 | E3 | 6 369 | **4 970** | 1 890 | 0 |
| W1 κ = 0.25 | E0 (contrast) | 3 572 | 49 597 | 36 705 | 79 043 |
| W0 κ = 0.25 | E0 (contrast) | 3 045 | 4 159 | 2 175 | 6 471 |

Answers to the specific questions:

* **Is the next buffer sealed before the current finishes processing?** Yes, in every E3 cell
  and every buffer: by 76 459 cycles (W1 κ = 0.25), 82 820 (κ = 0.50), whose only remaining
  work is a 68-element tail, and 3 780 (W0 κ = 0.25).
* **Does processing finish while the next buffer is still filling?** No, never under E3. The
  producer is always ahead; `producer blocked = 0` in all four cells.
* **Waiting for a buffer to become ready:** the ready wait column. It is the *producer's
  lead*, not a cost to the consumer.
* **Does the engine ever wait unnecessarily before switching buffers?** No. `cycles sealed
  but nothing adopting` is **1** in every cell — the cold start of the first window (sealed
  8 200, issued 8 201). Every later hand-off happens in the same cycle the previous buffer is
  released (W1 κ = 0.25: release 90 105, next issue 90 105), so the switch itself costs 0
  cycles.

## 4. Verdict

**4. The result varies by workload — but only in one direction that matters.**

* **W1 is consumer-bound at every κ.** Processing occupies the buffer 8–27× longer than
  filling takes: 58 690 vs 6 530 cycles at κ = 0.25, 42 904 vs 4 852 at κ = 0.50, 91 295 vs
  3 312 at κ = 0.75. The second buffer is sealed and idle for 76 459 / 82 820 cycles before it
  is needed.
* **W0 κ = 0.25 is the one near-balanced case, and its mean is misleading.** The first (full)
  window is still consumer-bound (8 904 processing vs 7 614 filling), but the 4 128-element
  tail window is producer-bound (5 124 filling vs 1 037 processing), which drags the mean
  below the producer's. It is a two-window operation ending in a fragment, not evidence of
  balance in the steady state.
* **They are never "reasonably balanced" in the sense the double buffer exists for.** In no
  cell does the producer and consumer duration come within a factor of two on a full window.

Evidence that makes this more than a duration comparison:

1. The consumer's cost is *not* a memory-latency tail. `last_issue → retire` (the drain after
   the last line is submitted) is only 238 cycles at κ = 0.25 and 143 at κ = 0.75 — the fills
   are back almost immediately. The time is in `issue → last_issue`: 81 666 cycles to submit
   8 192 lines, i.e. **0.10 lines/cycle** against an issue port that can take 4/cycle.
2. That rate is set by translation and residency, not by the buffer: in the same run 72 193
   cycles are classified C1 (issue refused by the dTLB) and 63 059 refusals are attributed to
   the index-reserve share of the pool.
3. The producer never blocks (`producer blocked = 0` under E3), so adding producer speed,
   a third buffer, or a smaller window cannot shorten these runs: the producer is already
   waiting on the consumer 76 459 cycles before it is needed.

Under E0 the pairing looks different for the *wrong* reason: the producer is blocked for
79 043 cycles (W1 κ = 0.25) because both buffers are sealed at once, so the E0 row shows a
smaller mean fill and a larger ready wait while the run is 27 894 cycles slower. That was the
seal-deadline effect of the previous investigation, not a property of the double buffer.

## 5. Measured, derived, and not measured

**Directly measured** (cycle stamps at the five events, per window): all `alloc`, `seal`,
`issue`, `last_issue`, `retire` values above; `cycles sealed but nothing adopting`; `cycles
with work but no filling window`; the C1 and refusal counters.

**Derived** (arithmetic on those stamps): fill, ready wait, issuing phase, drain, processing,
and the per-cell means. The "consumer-bound / producer-bound" characterisation is a
comparison of two derived durations.

**Not measured / open:**

* No per-window breakdown of *why* issuing takes 0.10 lines/cycle (dTLB refusal vs pool
  refusal vs page-run installation). The C1 aggregate is a run total, not per window.
* Whether a third buffer would help anything is not tested here — and on this evidence it
  would not, since the second is never consumed in time.
* Single seed (1) and one footprint; the κ = 0.75 case has a single window, so it cannot
  speak to buffer balance at all.
