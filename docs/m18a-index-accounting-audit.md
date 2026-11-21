# M18A — Index-Stream Accounting and Dispatch Audit

**Question.** M18 reported SpMV speedups of 4.7 %–23.6 % for a page-aware Bulk
indirect-memory accelerator against a Vortex-calibrated SIMT baseline, under the
convention "the index stream is charged once". This audit asks whether that
convention describes a *genuinely modelled compiler-directed dispatch* — the
index loads are handed to the engine, which fetches the same array itself — or
whether it is a post-hoc accounting adjustment applied to numbers the simulator
did not actually produce.

**Answer in one paragraph.** The dispatch is modelled, not assumed. In the hybrid
the engine reads the index array out of the same memory hierarchy with its own
requests: `e-idx` issues 15 766 / 28 960 / 1 588 / 99 347 line requests (one per
64 B line of the array) and misses L2 on every one of them, while in the
`full` trace the SIMT side simultaneously issues 79 573 / 137 199 / 9 095 /
495 868 requests for the same bytes and misses L2 15 783 / 28 991 / 1 589 /
101 769 times. *The same 1 MB is fetched twice, by two different clients, with
different request counts* — a property no accounting convention can fabricate.
The index values the engine consumes decode exactly to the CSR `colidx` array,
the gather addresses are formed from those loaded values (`data_base + v*8`,
oracle path off, 0 mismatches), and the two traces are proved equivalent
(`m18a-execution-equivalence.md`). The M18 comparison pairs `A(full)` with
`C(noidx)`: the C arm executes a program in which the index-load instructions
are *absent*, not a program whose cycles are later discounted. No cycle figure is
subtracted from any measured total anywhere in M18 or M18A. What the dispatch
buys is therefore not "free index traffic" — DRAM traffic is identical in both
arms — but **5× fewer L1 requests and the corresponding translation work for the
same compulsory lines**.

---

## 1. Phase 1 — execution paths

### 1.1 What the four workloads are

| dataset | rows used | expanded nnz | index array | compulsory lines | streams in the trace |
|---|---:|---:|---:|---:|---|
| bcsstk25 | 15 439 | 252 241 | 1 008 964 B | 15 766 | `rowptr`, `colidx`, `value`, `x_gather` |
| qc2534 | 2 534 | 463 360 | 1 853 440 B | 28 960 | same |
| lnsp3937 | 3 937 | 25 407 | 101 628 B | 1 588 | same |
| dubcova3 | 45 000 (prefix) | 1 589 539 | 6 358 156 B | 99 347 | same |

Compulsory lines = ⌈4·nnz / 64⌉; the engine's measured `e-idx` request count
equals it exactly on all four (15 766, 28 960, 1 588, 99 347).

### 1.2 Which component fetches `colidx`

* **`A(full)` / `C(full)`** — the SIMT front end. The trace contains `colidx` as
  an ordinary read stream, coalesced per instruction into distinct line requests:
  79 573 requests for 252 241 elements on bcsstk25 (≈ 0.32 requests/element;
  26 % of 4-lane groups straddle a 64 B boundary, which is why the count exceeds
  one request per four elements).
* **`C(full)` and `C(noidx)`** — *also* the engine, independently, through the
  descriptor's index array. `bulk.cpp:660-704`:

  ```cpp
  // j-th element of the descriptor
  const u8* b = mem_.line_bytes(mem_.pa_line_of_va_line(it->second));
  u64 v = load_le(b + pos * index_bytes, index_bytes);   // the index VALUE
  ...
  addr = data_base + v * data_bytes;                     // the target address
  ```

  `b` is a *fetched cache line* — the engine has to bring the line in ($pos$ is
  the byte offset within it) before it knows what address to generate. There is
  no side channel carrying index values: the alternative path,
  `cfg_.idx_oracle`, is `false` by default (`sim.hpp:118`) and is set true only
  inside the oracle experiment (`bulk.cpp:2785`), never in an M18 cell.

* **`A(noidx)` / `C(noidx)`** — neither. The index stream is absent from the
  trace; the engine is the only reader of the array.

### 1.3 Do SIMT index loads execute in the hybrid?

Yes, in `C(full)` — and that is precisely M18's "double-charged" bound. The
`cache_diag` per-requester attribution separates them:

| dataset | SIMT L1 reads, `C(full)` | `C(noidx)` | difference = index loads | engine `e-idx` reads (both) |
|---|---:|---:|---:|---:|
| bcsstk25 | 187 062 | 107 489 | 79 573 | 15 766 |
| qc2534 | 297 723 | 160 524 | 137 199 | 28 960 |
| lnsp3937 | 23 456 | 14 361 | 9 095 | 1 588 |
| dubcova3 | 1 125 599 | 629 731 | 495 868 | 99 347 |

The engine's `e-idx` count is **identical in both variants** — the engine's
acquisition is unaffected by whether the SIMT side also does it, which is what
"independently fetches the same data" means operationally.

### 1.4 Are the index values the ones the logical workload requires?

Yes, exactly. Recovering `index[j] = addr/8` from the addresses the engine
generated and comparing against the CSR `colidx` array (rebuilt from the matrix
with the same symmetric expansion) gives byte-identical arrays on all four
datasets, and the application output recomputed from the decoded values matches
the builder's recorded checksum. Details and raw tables:
`m18a-execution-equivalence.md` §2–3.

### 1.5 Are targets generated from fetched values?

Yes:

* `addr = data_base + v * data_bytes` with `v` loaded from the fetched line
  (`bulk.cpp:660-704`); `data_base = 0`, `data_bytes = 8`, so the target is a
  pure function of the fetched word.
* The fanout cross-check (`bulk.cpp:1616-1626`) re-derives the expected address
  from the index image and counts disagreement: `bulk_addr_mismatch = 0` on all
  four datasets, all four variants.
* The descriptor itself is a real memory object: one 64 B line at
  `desc_line` = 32 554 / 58 943 / 4 199 / 199 716, fetched before or at cycle
  362, with `bulk_desc_submitted = bulk_desc_completed = 1`,
  `bulk_desc_rejected = 0`.

**Phase 1 conclusion.** The hybrid contains no precomputed address path. The
engine's targets are a deterministic function of index words it fetched from
memory, and the fetch is visible in the hierarchy as its own client with its own
misses.

---

## 2. Phase 2 — event-level ledger

All values below are simulator counters, read from the cell logs
(`build/wm/m18_<m>_<var>_w8192.txt`); none is derived from a byte count. The
per-client rows are the `cache_diag` attribution block; the engine rows are the
`m15` Stats dump. "Writes" is not modelled: the traces are read-only
(`write_accesses = 0`), so write traffic and writeback are zero by construction,
not zero by omission of a counter.

Reading the tables: `A` is the SIMT-only baseline, `C` the hybrid. `full`/`noidx`
are the two traces. **Index (SIMT)** rows are obtained by differencing the two
variants of the *same* arm, because the simulator attributes cache traffic per
requester (`simt`, `e-idx`, `e-tgt`, `walk`) but does not split the SIMT client
by stream; every other row is a direct counter.

Two honesty notes on the attribution: (i) the per-requester L2-miss rows of a C
cell do not sum exactly to the arm's own `dram_reads` counter — +2 on bcsstk25
(60 606 vs 60 604), +2 on qc2534, +1 on lnsp3937, +52 on dubcova3 (481 778 vs
481 726, 0.011 %) — a duplicate count in the walk client, disclosed rather than
adjusted, and not affecting any index figure; (ii) the SIMT
index stream in `C(full)` misses L2 *less* than in `A(full)` because the engine
has already pulled the array into L2 by the time the SIMT warps reach it. Both
are visible in the tables rather than smoothed away.

### 2.1 bcsstk25 — 252 241 elements, 15 766 compulsory lines

| event | A(full) | A(noidx) | C(full) | C(noidx) |
|---|---:|---:|---:|---:|
| total cycles | 461 688 | 308 704 | 492 449 | 390 495 |
| logical index bytes | 1 008 964 | 1 008 964 | 1 008 964 | 1 008 964 |
| L1 requests, all streams | 323 809 | 244 236 | 206 260 | 126 692 |
| — index (SIMT), requests | 79 573 | 0 | 79 573 | 0 |
| — index (SIMT), L1 hits | 6 005 | 0 | 2 379 | 0 |
| — index (engine), L1 requests | — | — | 15 766 | 15 766 |
| — index (engine), L1 hits | — | — | 0 | 0 |
| index L2 reads / misses, SIMT | 17 407 / 15 783 | 0 / 0 | 15 721 / 10 358 | 0 / 0 |
| index L2 reads / misses, engine | — | — | 15 766 / 15 766 | 15 766 / 15 766 |
| index DRAM reads (all readers) | 15 783 | 0 | 26 124 (10 358 + 15 766) | 15 766 |
| descriptor: lines fetched / fetch done (cycle) | 0 | 0 | 1 / 362 | 1 / 362 |
| AG elements address-generated | — | — | 252 241 | 252 241 |
| engine active cycles (`bulk_ag_cycles`) | — | — | 276 262 | 318 538 |
| AG utilisation (`tgt_addr_gen/(ag_cycles·8)`) | — | — | 11.4 % | 9.9 % |
| index-wait stall cycles | — | — | 72 554 | 85 205 |
| TLB0 misses | 1 575 | 1 079 | 2 068 | 1 631 |
| page walks | 788 | 540 | 787 | 787 |
| PTE fetches | 1 576 | 1 080 | 1 574 | 1 574 |
| target requests | — | — | 3 432 | 3 437 |
| target DRAM reads | — | — | 1 931 | 1 931 |
| total DRAM reads | 50 245 | 34 462 | 60 604 | 50 246 |
| writes / writeback | 0 / — | 0 / — | 0 / — | 0 / — |

### 2.2 qc2534 — 463 360 elements, 28 960 compulsory lines

| event | A(full) | A(noidx) | C(full) | C(noidx) |
|---|---:|---:|---:|---:|
| total cycles | 762 503 | 446 600 | 923 790 | 728 062 |
| L1 requests | 455 640 | 318 441 | 328 896 | 191 697 |
| — index (SIMT) requests / L1 hits | 137 199 / 2 808 | 0 / 0 | 137 199 / 2 711 | 0 / 0 |
| — index (engine) requests / L1 hits | — | — | 28 960 / 0 | 28 960 / 0 |
| index L2 misses, SIMT (reads) | 28 991 (29 877) | 0 | 25 645 (28 916) | 0 |
| index L2 misses, engine | — | — | 28 960 | 28 960 |
| index DRAM reads (all readers) | 28 991 | 0 | 54 605 | 28 960 |
| descriptor: lines / fetch done | 0 | 0 | 1 line | 1 line |
| AG elements | — | — | 463 360 | 463 360 |
| engine active cycles | — | — | 484 276 | 728 062 |
| index-wait stall | — | — | 140 615 | 86 281 |
| TLB0 misses / walks | 2 729 / 1 368 | 1 823 / 915 | 3 715 / 1 655 | 2 733 / 1 369 |
| target requests / target DRAM | — | — | 2 213 / 318 | 2 213 / 318 |
| total DRAM reads | 87 447 | 58 456 | 113 113 | 87 449 |

### 2.3 lnsp3937 — 25 407 elements, 1 588 compulsory lines (fits L2)

| event | A(full) | A(noidx) | C(full) | C(noidx) |
|---|---:|---:|---:|---:|
| total cycles | 57 014 | 37 422 | 48 298 | 46 113 |
| L1 requests | 40 106 | 31 011 | 25 605 | 16 508 |
| — index (SIMT) requests / L1 hits | 9 095 / 431 | 0 / 0 | 9 095 / 582 | 0 / 0 |
| — index (engine) requests / L1 hits | — | — | 1 588 / 0 | 1 588 / 0 |
| index L2 misses, SIMT (reads) | 1 589 (1 638) | 0 | 0 (1 559) | 0 |
| index L2 misses, engine | — | — | 1 588 | 1 588 |
| index DRAM reads (all readers) | 1 589 | 0 | 1 588 | 1 588 |
| descriptor: lines / fetch done | 0 | 0 | 1 line | 1 line |
| AG elements | — | — | 25 407 | 25 407 |
| engine active cycles | — | — | 25 705 | 26 722 |
| index-wait stall | — | — | 8 315 | 9 990 |
| TLB0 misses / walks | 179 / 88 | 128 / 63 | 227 / 88 | 179 / 88 |
| target requests / target DRAM | — | — | 561 / 494 | 559 / 494 |
| total DRAM reads | 5 511 | 3 922 | 5 512 | 5 512 |

### 2.4 dubcova3 — 1 589 539 elements, 99 347 compulsory lines (45 000-row prefix)

| event | A(full) | A(noidx) | C(full) | C(noidx) |
|---|---:|---:|---:|---:|
| total cycles | 3 522 146 | 2 461 123 | 4 011 805 | 2 988 123 |
| L1 requests | 1 948 034 | 1 452 166 | 1 336 226 | 840 385 |
| — index (SIMT) requests / L1 hits | 495 868 / 36 734 | 0 / 0 | 495 868 / 20 992 | 0 / 0 |
| — index (engine) requests / L1 hits | — | — | 99 347 / 0 | 99 347 / 0 |
| index L2 misses, SIMT (reads) | 101 769 (110 366) | 0 | 98 157 (99 295) | 0 |
| index L2 misses, engine | — | — | 99 347 | 99 347 |
| index DRAM reads (all readers) | 101 769 | 0 | 197 504 | 99 347 |
| descriptor: lines / fetch done | 0 | 0 | 1 line | 1 line |
| AG elements | — | — | 1 589 539 | 1 589 539 |
| engine active cycles | — | — | 4 011 805 | 2 988 123 |
| index-wait stall | — | — | 517 269 | 524 520 |
| TLB0 misses / walks | 82 107 / 5 963 | 49 556 / 4 155 | 22 319 / 7 640 | 19 172 / 5 696 |
| target requests / target DRAM | — | — | 111 280 / 82 190 | 111 307 / 80 063 |
| total DRAM reads | 382 419 | 280 651 | 481 726 | 381 275 |

### 2.5 Component-cycle attribution (measured stall counters, `m16` block)

| dataset / arm | L1 stall | DRAM busy | DRAM starved | DRAM blocked |
|---|---:|---:|---:|---:|
| bcsstk25 A(full) | 12 068 434 | 824 318 | 55 032 | 356 412 |
| bcsstk25 C(noidx) | 23 667 374 | 765 556 | 10 267 | 329 983 |
| qc2534 A(full) | 24 688 176 | 1 359 114 | 90 113 | 584 944 |
| qc2534 C(noidx) | 28 441 609 | 1 294 450 | 82 784 | 557 830 |
| lnsp3937 A(full) | 1 091 092 | 93 870 | 10 980 | 40 524 |
| lnsp3937 C(noidx) | 2 333 800 | 85 556 | 3 882 | 36 720 |
| dubcova3 A(full) | 163 919 901 | 6 720 282 | 164 337 | 2 975 391 |
| dubcova3 C(noidx) | 174 140 005 | 5 846 002 | 66 549 | 2 540 300 |

These are *accumulated per-cycle stall counters* (they exceed the run length
because many slots stall in the same cycle), so they are used for attribution,
never summed into a runtime.

---

## 3. Phase 3 — the two conventions

### 3.1 Convention A: same-trace

`S_s = A(full) / C(full)`. Both arms execute the index loads; C additionally
fetches the array through the engine. The index stream is **charged twice**.

| dataset | A(full) | C(full) | S_s |
|---|---:|---:|---:|
| bcsstk25 | 461 688 | 492 449 | 0.938× |
| qc2534 | 762 503 | 923 790 | 0.825× |
| lnsp3937 | 57 014 | 48 298 | 1.180× |
| dubcova3 | 3 522 146 | 4 011 805 | 0.878× |

This is the *conservative* bound. It is a valid measured comparison of two
programs, but the program it measures is one no compiler would emit, so it is not
an architecture claim.

### 3.2 Convention B: index-charged-once

`S_f = A(full) / C(noidx)`. The C arm executes a program whose SIMT index loads
do not exist; the engine pays for the array itself.

| dataset | A(full) | C(noidx) | S_f | index-stream share of A(full) |
|---|---:|---:|---:|---:|
| bcsstk25 | 461 688 | 390 495 | **1.182×** | 33.1 % |
| qc2534 | 762 503 | 728 062 | **1.047×** | 41.4 % |
| lnsp3937 | 57 014 | 46 113 | **1.236×** | 34.4 % |
| dubcova3 | 3 522 146 | 2 988 123 | **1.179×** | 30.1 % |

### 3.3 Which events differ, and why

The two conventions differ in exactly two event classes:

1. **SIMT index-load instructions and their requests.** They are present in
   `C(full)` (79 573 / 137 199 / 9 095 / 495 868 requests, with their L1 traffic,
   L2 misses, TLB misses and instruction slots) and absent from `C(noidx)`.
2. **Nothing else.** Every other access in `noidx` is the same access, at the
   same address, in the same instruction, in the same lane — proved in
   `m18a-execution-equivalence.md` §1 (`full` − `colidx` == `noidx` exactly, on
   `(address, renumbered instruction, lane)`).

The engine's acquisition is *added* in both variants:
`e-idx` = 15 766 / 28 960 / 1 588 / 99 347 requests, all L2 misses, identical in
`full` and `noidx`. Convention B therefore charges the index stream once — to the
engine — and charges it in full: the same compulsory DRAM lines, plus descriptor
traffic, plus the engine's share of translation and target formation.

### 3.4 Is convention B a normalized estimate?

**No — it is a comparison of two measured executions, and the distinction is
checkable:**

* `C(noidx)` is a complete run with its own counters, its own completion hash and
  its own gate pass. Its cycles are not derived from `C(full)`.
* The work "removed" is *not present in the executed program*: the trace contains
  no index-load instruction, and the coalescer never forms those requests
  (`e-idx`/`simt` counts above are the evidence, not an assertion).
* No cycle estimate is subtracted from a runtime anywhere. The only subtractions
  in this report are *differences between measured counters* (e.g.
  `A(full) − A(noidx)` = 152 984 cycles on bcsstk25), used as a diagnostic of the
  stream's marginal cost — never as a speedup and never back-substituted into a
  total.

Where the "normalized estimate" label *would* apply, and is avoided: computing
`C(full) − (modelled index-load cost)` and calling the result "the hybrid's
runtime". M18 and M18A never do that. The one place a modelled quantity is
carried is convention A itself, and it is disclosed as the double-charged bound.

### 3.5 The marginal-cost cross-check

Differencing the two variants of the *baseline* measures what the index stream
actually costs the SIMT machine, and lets the accounting be checked against the
compulsory lines:

| dataset | index requests (SIMT) | index DRAM (SIMT) | compulsory lines | excess | cycles |
|---|---:|---:|---:|---:|---:|
| bcsstk25 | 79 573 | 15 783 | 15 766 | +17 (0.1 %) | 152 984 |
| qc2534 | 137 199 | 28 991 | 28 960 | +31 (0.1 %) | 315 903 |
| lnsp3937 | 9 095 | 1 589 | 1 588 | +1 | 19 592 |
| dubcova3 | 495 868 | 101 769 | 99 347 | +2 422 (2.4 %) | 1 061 023 |

The excess is the second-order cache interaction of removing a stream: on
dubcova3 the index array's 1 552 pages compete with the `x` gather for L2, so
deleting the stream also avoids 2 422 eviction-induced refetches. That is a real
effect of removing the stream, so it belongs in the difference; it is reported
separately so it is not mistaken for index traffic. **It is not subtracted from
anywhere.**

---

## 4. Phase 4 — execution equivalence (summary)

Full detail in `m18a-execution-equivalence.md`. For all four datasets:
element counts identical (`bulk_elems_total = bulk_elems_done = nnz`), gather
address sequences identical between variants, `full` − `colidx` == `noidx`
exactly, decoded index array == CSR `colidx` exactly, output checksums
recomputed from decoded indices match the builder's, `bulk_addr_mismatch = 0`,
same layout constants, same memory-system configuration. Dubcova3 is a 45 000-row
prefix; equivalence holds for the prefix.

---

## 5. Phase 5 — lane-width sensitivity (summary)

Full detail and the 32-lane defect in `m18a-lane-width-sensitivity.md`.

| dataset | 4 lanes | 8 lanes | 16 lanes | 32 lanes |
|---|---:|---:|---:|---:|
| bcsstk25 `S_f` | 1.182× | 1.100× | 1.065× | 1.058× |
| dubcova3 `S_f` | 1.179× | 1.133× | 1.124× | not measurable (deadlock) |

The advantage decays as the SIMT warp coalesces the index loads itself — the
request-amplification ratio between the two acquisition paths falls from 5.0× at
4 lanes to 1.9× at 32 lanes while compulsory traffic is unchanged. It does not
reverse in the measurable range. `lanes` in this simulator varies only trace
formation, intra-warp dedup and a latch charge that is disabled in every
published config; it does **not** widen the LSU, the MSHR file or the L1, so this
is a bounded sensitivity, not a wide-GPU result.

---

## 6. Classification of every number in this report

| class | what it covers | examples |
|---|---|---|
| **Directly measured simulator results** | every count, byte total and cycle figure in §2, §3.1–3.2, §3.5 — read from a completed run's counters | `e-idx` 15 766 requests / 15 766 L2 misses; `A(full)` 461 688 cycles; `bulk_addr_mismatch = 0`; 3 432 target requests, 1 931 target DRAM reads |
| **Architecturally valid accounting comparisons** | ratios between two *measured* runs whose programs differ only in the index-load instructions, proved by §4 equivalence | `S_f` = 1.182 / 1.047 / 1.236 / 1.179; `S_s` = 0.938 / 0.825 / 1.180 / 0.878; the 4-vs-16-lane trend |
| **Post-hoc normalized estimates** | none are used for any headline number. The only "modelled" quantities in the chain are the descriptor size (64 B), the index-array byte count (4·nnz) and the double-charge disclosure in §3.1 — all labelled where they appear | `+17`/`+31`/`+1`/`+2 421` excess DRAM is measured, not modelled; the `1,008,964 B` logical index size is a workload property |
| **Unverified assumptions about wider GPUs** | the 8/16/32-lane points beyond trace formation (§5); any claim that a real wider warp with a wider LSU would preserve the measured trend; the engine's real area/power cost, which the simulator does not model | the 32-lane `S_f` values; the extrapolation "the gain holds on a 32-lane GPU" |

**Headline conclusion, stated in those terms.** Under the faithful pairing, the
M18 speedups are *architecturally valid accounting comparisons* over *directly
measured* runs: the hybrid genuinely acquires the index stream itself, its
acquisition is visible and separately counterable, and the two traces are proved
to differ only by the index-load instructions. The speedup is not produced by
subtracting anything from a measured runtime. Its magnitude is a function of how
badly the competing SIMT front end coalesces the index stream, which the 4-lane
configuration sets at 5× request amplification — the largest single caveat in
the claim, quantified in §5 and in `m18a-lane-width-sensitivity.md`.

---

## 7. What this audit does not establish

* **That a real compiler emits this dispatch.** The audit shows the *modelled*
  dispatch is genuine — the class map is built before cycle 0, the descriptor is
  submitted and fetched as memory, index values drive address generation. It does
  not model an analysis pass deciding which gather to hand over, and it does not
  price that pass.
* **That the engine's index acquisition is free.** It is not: it costs the same
  compulsory DRAM lines (15 766 = ⌈252 241·4/64⌉), one descriptor line, a share
  of translation, and 26–37 % of the engine's active cycles in index-wait stall.
  The gain is in request count and the LSU/translation path, not in bytes.
* **Anything about a wider GPU.** See §5 / `m18a-lane-width-sensitivity.md` §6.
* **Write traffic.** The SpMV traces are read-only, so no store path, write
  allocate or writeback is modelled in any arm. A write-bearing workload would
  need its own accounting.
* **Area, power or occupancy of the engine.** Not modelled anywhere in M5–M18.

---

## 8. Reproduce

```
cd DX100_MEM

# Phase 2/3 ledger (per-requester attribution + engine counters) for one cell
python3 tools/m18a_audit.py ledger \
    build/wm/m18_bcsstk25_full_w8192.txt build/wm/m18_bcsstk25_noidx_w8192.txt

# Regenerate all eight primary cells (~6 min)
for m in bcsstk25 qc2534 lnsp3937 dubcova3; do for v in full noidx; do
  ./sim/build/wm/m18_run.sh build/wm/m18_${m}_${v}.bin 8192 ${m}_${v}_w8192 \
      > build/wm/m18_${m}_${v}_w8192.txt 2>&1
done; done

# Phase 4 equivalence (four datasets)
python3 tools/m18a_audit.py equiv bcsstk25 --full build/wm/m18_bcsstk25_full.bin \
    --noidx build/wm/m18_bcsstk25_noidx.bin --mtx data/bcsstk25.mtx
python3 tools/m18a_audit.py equiv dubcova3 --full build/wm/m18_dubcova3_full.bin \
    --noidx build/wm/m18_dubcova3_noidx.bin \
    --mtx ref/vortex/tests/opencl/spmv/Dubcova3.mtx --max-rows 45000

# Phase 5 lane sweep
bash build/wm/m18_lanes.sh
```

Raw per-cell logs: `build/wm/m18_<m>_<var>_w8192.txt`; machine-readable ledger:
`build/wm/m18a_ledger.md`. M18's own tables (`m18_tables.md`, `docs/m18-*.md`,
`docs/m18-results.svg`) are unchanged by this audit except for four added
`noidx_mshr128` rows in the M18 table that were produced by M18, not by M18A.
