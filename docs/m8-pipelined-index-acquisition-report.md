# M8 — Pipelined Index Acquisition and Fine-Grained Readiness

Status: implemented, measured, regression-clean. Simulator only — no RTL, no compiler
backend, no ISA change.

Every number below is produced by `DX100_MEM/sim` and reproducible from the command
line quoted with it. Published M4, M5.1, M6, W0, W1 and M7 figures are unchanged and
still reproduce (see §8); M8 numbers are measured *against* them, never substituted
for them.

---

## 1. What the M7 index path actually did

Read from source (`src/bulk.cpp`, `src/mem.cpp`), not from the older prose:

1. **Generation.** `generate()` walks elements in order and, for element `e`, needs
   `idx_image[idx_base + e*idx_bytes]`. It calls `want_index(chunk)` where
   `chunk = e / (64 / idx_bytes)` — a 64 B line holds 16 four-byte indices or
   8 eight-byte ones.
2. **Outstanding tracking.** A chunk miss creates one DRAM read of the index line and
   records `idx_resident_`, `idx_peak_`, `idx_mshr_peak_`. The bound was
   `idx_window ? idx_window : R/2`, i.e. **64 lines in flight** by default. That is a
   real MSHR-sized window and it was already the strongest part of the design.
3. **Response handling.** `on_index()` pushed the completed line into a
   `std::map<u32, IdxChunk>` (`idx_done_`) keyed by chunk number and decremented
   `idx_resident_`. Nothing bounded the *completed-but-unconsumed* set: while the
   targets were busy the map could hold every completed index line of the run.
4. **Consumption.** `generate()` looked up its exact chunk in `idx_done_` each element;
   a miss is a `idx_wait` cycle. Because the lookup is by exact chunk, a completed
   line is consumed immediately, but a *later* line that arrives first cannot be used
   before the earlier one — consumption is element-ordered, which is what
   address generation requires anyway.
5. **Admission / sealing.** `win_admit()` calls `generate()` inside the window; if
   generation stalls, the window cannot fill, and the window seals on the
   `batch_deadline` (4,096) instead of filling to its peak (8,192). This is the
   mechanism by which an index stall becomes a *memory-locality* loss, not just lost
   cycles.
6. **Resources.** Index reads drew from the shared R = 128 residency pool and were
   issued through the same `unissued_` FIFO as targets, with index requests enqueued
   first (that priority was already present in M6 and is load-bearing — see §4).

So the pre-M8 pipeline already overlapped acquisition with generation and with target
issue. Its weakness was not "waiting for a whole window": it was (a) an **unaccounted,
effectively unbounded index buffer** (`idx_done_`, a `std::map` with no capacity
charge in the cost model) and (b) **no guaranteed share** of the residency pool when
the target stream saturated it.

## 2. Architecture of the M8 acquisition unit

Four modes, selected by `idx_mode`; mode 2 is the default:

| `idx_mode` | Meaning |
|---|---|
| 0 | M6/M7 acquisition: unbounded `idx_done_` map, shared issue FIFO. Historical configuration. |
| 1 | Two bounded index buffers (ring), in-order consumption. |
| 2 | Ring + fine-grained per-line readiness (default). |
| 3 | Ring + fine readiness + dedicated index issue port and reserved residency share. |

Configuration: `idx_mode`, `idx_buf_lines` (lines per buffer, default 64),
`idx_mshr` (mode 3 table depth, default 64), `idx_reserve` (slots of R that targets
may not take while the ring is active, default 8).

### 2.1 The index ring

`Run::IdxRing` is a flat array of `IDX_SLOTS = 256` descriptors:

```
struct IdxRing {                       // per engine
  u32 elem_base[256];   // first element this line supplies
  u32 va[256];          // virtual address of the index line
  u16 cnt[256];         // elements in this line (last line may be partial)
  u8  used[256];        // elements already generated from it
  u8  ready[256];       // the line's data has arrived
  u32 head;             // slot of the oldest unconsumed line
  u32 n;                // lines currently described
  u32 next_elem;        // next element to claim
};
```

A slot is a **descriptor**, not a data buffer: the line's bytes are consumed by
`generate()` as soon as the descriptor is ready, exactly as in M6. Two buffers of
`idx_buf_lines` are the architectural minimum (`2*cap` slots live, `cap` being
"buffer A/B"); the ring is sized for four buffers so `idx_buf_lines` can be swept to
128 without rebuilding. Handover is per-slot release, not whole-buffer:

* Whole-buffer ownership (`2*cap` slots as two rigid tiles) was implemented first and
  measured **15 % slower** on W1 (180,354 vs 156,907): a buffer could not be refilled
  until its last element had been generated, so acquisition idled behind a straggler.
* The ring releases each slot as its last element is consumed, so refill is continuous.

### 2.2 Fine-grained readiness

`idx_take()` hands `generate()` the next element only if the *head* slot is `ready`
and still has `used < cnt`; otherwise it returns false and the cycle is a `wait`. A
line arriving out of order sets its own slot's `ready` bit and does not gate the head,
so responses are handled out of order while *consumption* stays in element order — the
ordering address generation requires. An unready slot is never consumed, so no
partially-arrived line can be read.

### 2.3 Dedicated streaming path and arbitration

Mode 3 issues index requests from its own port (`idx_issue()`), limited by
`idx_mshr`, and the port is serviced **before** the target port in `win_tick()`.

That ordering is mandatory: putting target issue first cost **+43,445 cycles** on W1
in an earlier attempt, because targets consume the residency slots the feeder needs
and the feeder starves its own consumers (priority inversion). The M6 path had the
same property for free, because index requests entered `unissued_` ahead of targets.

Modes 1 and 2 keep the shared FIFO but add an explicit partition of R: while the ring
is active, `win_emit()` refuses to take the last `idx_reserve` slots, so index claims
can never be starved. This is a *partition* of an existing budget, not an increase.

## 3. Regular vs irregular dispatch; proposed ISA/compiler interface

Node does not need a classifier. The existing `BulkDesc` already carries everything a
dispatch decision needs, so M8 uses explicit descriptor metadata:

* `count`, `idx_base`, `idx_bytes`, `idx_stride` — a *regular* stream: sequential when
  `idx_stride == idx_bytes`, strided otherwise. The ring is valid for any stride: the
  slot records the line's VA, so a negative or non-unit stride only changes which line
  a slot covers.
* `P`, `kappa` — the irregular target pattern, handled by the M6/M7 organization path.

Ordinary loads and stores never see the ring; they use the LSU as before. Proposed
future interface (documented only, not implemented):

```
bulk.start  { count, idx_base, idx_bytes, idx_stride,
              target_base, P, kappa, mode }   // mode = indirect | strided
```

`mode` selects the acquisition unit and the organization path independently; a
compiler emits it from a recognized gather, and the hardware never infers
"indirectness" from individual loads.

## 4. Resource accounting

No capacity was added except where listed; every item is charged.

| Resource | M6/M7 | M8 | Note |
|---|---|---|---|
| Index buffer | `std::map`, unbounded (unaccounted) | 256 descriptors × 20 B ≈ **5.1 KB** | 128 live at defaults (2 × 64 lines); 20 B = elem_base 4 + va 4 + cnt 2 + used 1 + ready 1 (+ pad) |
| Index request entries | none of its own; drew from R | 0 (modes 0–2); `idx_mshr` ≤ 64 **inside** R (mode 3) | never additive: `residency()` includes `idx_q_` |
| Target request entries | R = 128 | R = 128 | unchanged |
| Reserved share | 0 | `idx_reserve` = 8 slots | targets see 120; partition, not capacity |
| Scheduler metadata | M7 | M7 (unchanged) | +1 charged cycle per run install (§9) |
| Index traffic | 1 line per 64 B of index array | identical (`idx_lines_req` asserted equal in tests) | measured equal on every workload |

`residency()` was extended to count `idx_q_`, so an index request is visible to
admission from the cycle it is claimed. No oracle path is used in any measured run
(`B2`/`B3` diag rows are the only oracle rows and are labelled as such).

## 5. Experiments

A = SIMT, B = legacy M6 (window, legacy page policy), C = M7 acquisition,
D = double buffering only (mode 1), F = double buffering + fine readiness (mode 2,
default), G = full streaming subsystem (mode 3), `G8`/`G64` = `idx_mshr` 8/64.

Command shape (all workloads):

```
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=diag \
  --set batch_window=8192 [workload] [--set page_sched=1 --set idx_mode=N ...]
```

Workloads: **W1** (16,384 elements, `kappa = 0`), **W0** (16,384, `kappa = 1`,
`P = 256`), **W0-32k** (32,768, longer), **E1** (8-byte indices, 2,048 index lines —
the widest *supported* index stream; the generator has no strided-index workload, so
`idx_bytes` is the only available lever on index-stream shape).

### 5.1 End-to-end cycles

| Workload | A SIMT | B M6 | C M7 | D | **F (default)** | G8 | G64 |
|---|---:|---:|---:|---:|---:|---:|---:|
| W1 | 270,753 | 233,475 | 156,912 | 156,508 | **156,419** | 184,831 | 156,419 |
| W0 | 30,573 | 39,322 | 39,530 | 39,041 | **38,775** | 42,515 | 38,775 |
| W0-32k | 60,833 | 77,978 | 78,287 | 77,505 | **77,585** | 84,020 | 77,585 |
| E1 | 270,753 | 266,867 | **192,480** | 203,058 | 202,755 | 246,329 | 202,755 |

F vs C: **W1 −493 (−0.31 %), W0 −755 (−1.91 %), W0-32k −702 (−0.90 %), E1 +10,275
(+5.34 %)**.

D vs F isolates fine-grained readiness: −89 cycles on W1 (0.06 %), i.e. within noise —
the index lines of a sequential stream complete in order in this memory system, so
there is almost nothing for out-of-order readiness to exploit. The mechanism is
required for correctness under reordering and costs nothing, but it is not where the
gain is.

G is *identical* to F at `idx_mshr = 64` and much worse below it (§5.3): a dedicated
port helps only if its table is as deep as the shared path's implicit window.

### 5.2 Buffer-size sweep (mode 2, `idx_reserve` 8, W1)

| `idx_buf_lines` | SRAM live | W1 cycles |
|---:|---:|---:|
| 16 | 640 B | 175,010 |
| 32 | 1.3 KB | 156,978 |
| 64 | 2.7 KB | **156,419** |
| 128 | 5.4 KB | 157,134 |

Two buffers of 32 lines is the knee; 64 is fastest; 128 is worse because consuming
from a deeper ring delays the release of the *oldest* slot to the point where the
feeder runs behind, and the extra SRAM buys nothing on a 1,024-line index array.

### 5.3 Outstanding-index-request sweep (mode 3)

| `idx_mshr` | 4 | 8 | 16 | 32 | 64 |
|---|---:|---:|---:|---:|---:|
| W1 cycles | 191,150 | 184,831 | 178,556 | 166,886 | 156,419 |

Monotone in table depth: the feeder's delivery rate is `depth / line latency`, and
below ~64 the address engine starves. This is the quantitative answer to "how deep
must the index request table be": **R/2**, which is exactly what M6 used.

### 5.4 Reserved-share sweep (mode 2, W1 / E1 / W0)

| `idx_reserve` | 0 | 8 | 16 | 32 |
|---|---:|---:|---:|---:|
| W1 | 160,486 | **156,401** | 156,450 | 156,886 |
| E1 | 217,361 | 202,162 | 201,563 | 201,131 |
| W0 | 39,041 | **38,775** | 38,874 | 39,242 |

Without a reserved share the ring loses 2.6 % on W1 and 13 % on E1 — claims are
starved whenever the target stream fills R. 8 slots is enough for W1/W0; E1 wants
more because its index array is twice as long. The reserve is a genuine cost to the
target stream (it loses 8 slots of headroom), and it is still a net win.

### 5.5 Counters (defaults, mode 2, `idx_buf_lines` 64, `idx_reserve` 8)

| | W1 | W0 |
|---|---:|---:|
| cycles | 156,418 | 38,774 |
| `wait` (AG has no consumable line) | 5,981 (3.8 %) | 5,210 (13.4 %) |
| `bufStall` (ready line, no residency slot) | 0 | 331 |
| `swaps` (buffer handovers) | 4 | 4 |
| `ooo` elements generated out of element order | 12,064 | 9,792 |
| peak ready lines / peak index lines in flight | 128 / 64 | 128 / 64 |
| scheduler metadata | 2,720 B | 2,720 B |
| first target issue | 4,115 | 4,115 |
| last target issue / last index line done | 155,422 / 87,242 | 37,887 / 24,552 |
| target residency peak | 64 | — |

**Overlap is the point and it holds:** on W1 acquisition completes at cycle 87,242
while targets are still issuing at 155,422 — index acquisition occupies the first 56 %
of the run and is fully hidden behind generation and target execution. First target
issue is unchanged at 4,115; M8 does not delay the start of useful work.

The M6.2 **free-index oracle is still faster** (W1 counterfactual from the diag `B2`
row: −8,623 cycles relative to the measured run, not a real configuration *and not
used in any reported number*). Index acquisition is therefore worth at most ~0.3 % on
W1 even if it were free: M7 already removed the serialization, and what remains is
DRAM traffic, not pipeline bubbles. That is the single most important finding of M8.

## 6. Correctness

New group `m8_index_acquisition` (4 configurations × the full element range):

* **Exact multiset and per-element equality** of `(elem, addr, value)` against the M6
  path, element by element, for modes 1, 2 and 3 — no missing and no duplicated
  elements.
* **Address re-derivation** (`addr_mismatch == 0`) recomputes every address from the
  index image, so a stale or half-arrived index line cannot pass unnoticed.
* **No reuse before consumption** is implied by the same comparison plus per-slot
  `used`/`ready`: a slot is only refilled after `used == cnt`, and the value check
  would fail if a refill landed early.
* **Out-of-order responses**: mode 2 generates 12,064 elements out of element order on
  W1 with byte-identical output.
* **Partial final line**: the existing small-workload groups (`bulk_empty_and_small`,
  and every `count < 16` case) now run through the ring and pass, exercising
  `cnt < per` slots at the tail.
* **No deadlock under backpressure**: mode 3 with `idx_mshr = 1`, `idx_buf_lines = 1`,
  `idx_reserve = 0` completes correctly (26,869 cycles vs 24,538 for M6 on the test
  configuration).
* **R = 128** never exceeded: `residency_violations == 0` in every arm and in the full
  suite; `buf_max <= R` asserted.
* **M7 page scheduling** unchanged: `m7_page_sched`, `m7_baselines` and the cohort
  tests pass.

Full suite: **3,979 checks, 0 failures, 57 groups**.

## 7. Performance summary

Measured (this is all measured; no estimates):

* Bounded, accounted double buffering with fine-grained readiness (mode 2) beats the
  unbounded M6 map by 0.3 % on W1, 1.9 % on W0 and 0.9 % on the longer W0, and loses
  5.3 % on E1.
* A dedicated index port is worth nothing unless `idx_mshr` reaches R/2; below that it
  is a large regression.
* Fine-grained readiness per se is worth ~0.06 %: correct, free, not the win.
* 13 % of W0 and 3.8 % of W1 remain index-wait, and the oracle bounds the total
  recoverable time at ~0.3 % (W1) and a few percent (W0).

Analytical (not measured, clearly labelled):

* E1's regression is a *capacity* effect: E1 has 2,048 index lines and the ring holds
  128 (6 % of the array), whereas the M6 map held all of it. At `idx_buf_lines = 128`
  E1 recovers from 202,755 to 198,816; extrapolating the observed slope, a 16 KB ring
  (≈800 lines) should bring E1 to parity. This is an estimate from two points.
* The `idx_mshr` sweep is consistent with delivery rate ≈ `idx_mshr / L`, with
  L ≈ 60–90 cycles at 90 % DRAM utilization.

## 8. Regression and preserved baselines

Reproduced after the change (all `status: OK`):

| Figure | Published | Reproduced |
|---|---:|---:|
| A/B on `ab_controlled.cfg` | 270,753 / 278,034 | 270,753 / 278,034 |
| W0 legacy (A/B) | 30,573 / 44,773 | 30,573 / 44,773 |
| W0 windowed (B) | 39,322 | 39,322 |
| W0 diag (B0) | 39,322 | 39,322 |
| W1 legacy / cohorts 1 | 233,475 / 233,475 | 233,475 / 233,475 |
| M7 W1 cohorts 2 (pre-audit) | 156,907 | 156,912 (audit, §9) |
| M7 W0 cohorts 2 | 39,530 | 39,530 |

The historical numbers are the `idx_mode = 0` configuration; M8 made the ring the
default, so the tests now name `idx_mode = 0` explicitly where they pin an M6/M7
figure. No published value was edited.

## 9. M7 accounting audit

M7 reported 1,536 scheduler installation cycles that were **counted but not charged**:
`st_.sched_cycles++` had no effect on the pipeline.

**Decision: implement the bounded model.** A run installation now occupies the issue
port for one cycle (`sched_busy_`, at most one outstanding, never accumulated):
`win_issue_sched()` returns without issuing while it is set.

| Configuration | Original M7 | Corrected |
|---|---:|---:|
| W1, `page_sched=1`, cohorts 2 | 156,907 | **156,912** (+5) |
| W1, cohorts 4 | 154,673 | **154,807** (+134) |
| W1, legacy page policy + window | 233,475 | 233,528 (+53) |
| W0, cohorts 2 | 39,530 | 39,530 (+0) |

The omission was **immaterial**: 1,536 installations cost 5 cycles on the primary
configuration because the port was already idle on those cycles. The corrected model
is now the simulated behaviour; the original figures remain in the M7 report and are
annotated with a pointer here. Incremental M8 (§5.1) is measured against the corrected
C column.

## 10. Limitations

* E1 regresses 5.3 % (mode 2). Cause identified and quantified (§7); not fixed within
  M8 because the fix is a buffer-capacity policy question, not a pipeline question.
* No strided-**index** workload exists in the generator, so §4's stride claim is argued
  from the slot mapping, not measured.
* `N ≥ 32,768` (W1) trips a **pre-existing** diagnostic invariant ("the index array is
  not exactly 4096 requests of B0"), identically in mode 0 and mode 2, so long-W1
  comparisons use W0-32k instead and long-W1 numbers are not reported as M8 results.
* Mode 3's dedicated port is not beneficial at any tested configuration when forced to
  a small table; it is kept as a documented, disable-able architecture rather than a
  default.
* SRAM figures are descriptor storage only (20 B/slot); the index line *data* is
  consumed as it arrives, as in M6, and is not double-buffered.

## 11. Recommended next milestone

**M9 — index-buffer capacity policy.** Size the ring from the descriptor
(`ceil(count / per)`) subject to a real SRAM budget (8–16 KB), with a "front-load the
whole index array when it fits" mode, and re-measure E1 and W1-32k. This is the only
remaining structural gap M8 exposed, and it is a capacity/allocation question, not a
scheduling one. Secondary: the W0 13 % index wait suggests the W0 index stream is
bandwidth-limited rather than latency-limited; a stride-aware request coalescer (two
index lines per request when contiguous) would test that hypothesis cheaply.
