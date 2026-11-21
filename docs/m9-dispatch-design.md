# M9 — Compiler-Directed SIMT/Bulk Dispatch: Implementation Design

**Status: implemented (§11 records the as-built delta and the gate results).** Sections 0–10 were
written when no source file had been modified; every claim about the simulator in them was read out
of `sim/src/` at the working tree of that time, and every number was produced by running the then-
unmodified binary. Sections marked *hypothesis* are predictions that M9 exists to test; they are not
results. S3 (metadata, class filter, compaction, hybrid driver) is complete and all gates pass — see
`docs/m9-dispatch-implementation-report.md` for commands, configurations, results and limitations.

M9 adds one decision above both front-ends: an operation whose address is a direct function of
its own lane index runs as SIMT (M1–M4 path); an operation whose address comes from a loaded
index array runs on the M6/M7/M8 bulk accelerator. Nothing below that decision changes.

---

## 0. Summary

| Question | Answer |
|---|---|
| Where does metadata enter? | At the trace boundary, as one class bit per memory instruction, carried alongside `Trace` exactly as `BulkWorkload` already is (`sim.hpp:731`). |
| Smallest representation | 1 bit per instruction (`OpClass`, two values). For a bulk op the remaining metadata is *already* `BulkDesc` — no new IR. |
| How are direct vs indirect distinguished? | By the annotation, not by inspecting addresses at run time. The generator's own `regular` decision is recorded and handed in as "compiler metadata"; a post-hoc contiguity test is used only to cross-check it. |
| Can both front-ends consume one workload? | Yes, and they already do: `build_bulk_workload` derives its index array from the *same* `Trace` the SIMT arm consumes (`bulk.cpp:1218`). M9 generalises that derivation with a class filter. |
| What must change before dispatch? | Three structural defects that would silently give the hybrid 2×R residency and 2 ticks/cycle: independent slot pools, two callers of `MemorySystem::tick`, and an ambiguous completion record. |
| Ready for implementation? | **Yes**, after those three fixes, which are provably inert for every published M4–M8 number. One scope exclusion: stores are not modelled. |

---

## 1. Current architecture and source-code integration points

### 1.1 The two front-ends and the one hierarchy

| Element | File / line | Role |
|---|---|---|
| `Mode {Baseline, Neutral, Bulk}` | `sim.hpp:39` | front-end selector; `Neutral` is the M4a control issuer |
| `Config` | `sim.hpp:42` | every knob, including all M6/M7/M8 engine parameters |
| `Access` | `sim.hpp:214` | one trace element: `{addr, line, elem, instr, lane}` |
| `Trace` | `sim.hpp:222` | the materialized workload + realized ground truth |
| `build_trace()` | `workload.cpp:24` | the workload generator (Λ, P, κ, ρ, N, seed) |
| `Req` | `sim.hpp:275` | one line request as the hierarchy sees it: `{va_line, pa_line, vpn, core, slot, seq, stage, bypass_l1}` |
| `MemorySystem::submit / tick` | `memory.cpp:467 / 479` | the single entry point and the single per-cycle engine |
| `MemorySystem` internals | `memory.cpp` | per-core dTLB, shared L2 TLB, PTW + walk cache + walkers, L1 per core, L2, banked DRAM with FR-FCFS |
| `Sim::run(Mode)` | `system.cpp:70` | the SIMT front-end: per-core instruction lists, coalescer, per-core slot vectors |
| `Sim::run()` | `system.cpp:68` | the fork: `mode == Bulk ? run_bulk() : run(mode)` |
| `BulkDesc` | `sim.hpp:442` | 64 B on-memory descriptor: `{desc_line, index_base, data_base, count, elem_base, index_bytes, data_bytes}` |
| `BulkEngine` | `sim.hpp:533` | M5 engine + M6 window + M7 cohorts + M8 tiles |
| `BulkEngine::tick` | `bulk.cpp:491` | **calls `mem_.tick` itself**, then attributes, then progresses |
| `BulkEngine::attribute` | `bulk.cpp:360` | completion attribution by slot |
| `build_bulk_workload` | `bulk.cpp:1218` | derives the index array from the trace addresses |
| `Sim::run_bulk(w)` | `bulk.cpp:1280` | the bulk driver loop |
| `BulkWorkload` | `sim.hpp:731` | the immutable workload *value*: descriptor + image writes + index values |
| `run_ab` / `print_ab` | `ab.cpp:219 / 361` | the M5.1 controlled A/B driver and its report |
| `truth_hash` / `engine_hash` | `ab.cpp:563 / 589` | host-side ground truth and per-element engine verification |

The hierarchy is one shared object. `mem_path = l1` (the frozen A/B environment,
`configs/ab_controlled.cfg`) routes *both* front-ends through core 0's L1
(`MemorySystem::path_bypasses_l1`, `sim.hpp`), so the two classes already contend in the same
cache, the same dTLB `MSHR`s, the same L2 TLB, the same walkers and the same DRAM. That is why
dispatch needs no new structure: the sharing already exists and is already measured.

### 1.2 One workload, two derivations (this is the existing precedent)

`build_trace` produces `Trace::acc`, a flat element list in instruction order, with `instr`
monotonic. The SIMT arm groups it by `instr` and coalesces each group's lanes into distinct
lines. The bulk arm does **not** regenerate anything: `build_bulk_workload` writes

```
index[i] = trace.acc[i].addr / elem_bytes
```

into the image at `bulk_index_base` and a descriptor at `bulk_desc_line`, and sets
`bulk_data_base = 0` so that `data_base + index[i] * elem_bytes` is *exactly* the address the
SIMT arm accesses. Both front-ends therefore touch the same lines at the same in-line offsets;
the engine additionally pays one descriptor fetch and `ceil(N·4/64)` index lines.

This is the property M9 must preserve and it is a structural one: the index array is a
*function of the trace*, not a second workload. §5 keeps it that way.

### 1.3 What does not exist (the actual M9 gap)

* **No class anywhere.** `Access` has no class field, `Trace` has none, `Config` has none. The
  only grouping is `instr`.
* **One descriptor for the whole trace.** `run_bulk` submits `w.desc` with `count = acc.size()`
  and `elem_base = 0`; element identity is `elem_base + pos` (`bulk.cpp:on_target`,
  `on_line`/`win_fanout`).
* **The engine serialises descriptors.** `queue_` holds several but `start_next` (`bulk.cpp:191`)
  returns while `active_`, so descriptors execute one at a time.
* **Two independent residency pools.** `Sim::run` gives core 0 `per_core = R/cores − latch`
  slots (`system.cpp:70`, `R = 128`, `lanes = 4`, `ab_charge_latch = 0` → 128 slots);
  `BulkEngine`'s constructor (`bulk.cpp`, `for (u32 i = cfg_.R; i-- > 0;) free_.push_back(i)`)
  creates its own 128. Both live in one process ⇒ **up to 256 outstanding line requests**
  against a budget of 128.
* **`MemorySystem::tick` has two potential callers.** `Sim::run` calls it once per cycle
  (`system.cpp:70`, main loop); `BulkEngine::tick` calls it once per cycle (`bulk.cpp:491`).
  A driver that runs both front-ends naively advances the hierarchy **twice per cycle**.
* **The completion record is ambiguous.** `memory.cpp` (`MemorySystem::tick`) packs
  `done.push_back(static_cast<u64>(r.core) << 32 | r.slot)`. Engine requests are submitted with
  `r.core = 0` (`bulk.cpp`, `issue`/`win_emit`/`idx_issue`), and SIMT requests also use core 0.
  Engine slot 3 and SIMT slot 3 are indistinguishable.
* **No dispatch driver.** `Config::set` (`util.cpp:22`) validates `experiment` against a fixed
  list (`none|ab|ab_sweep|m6|m6_sweep|diag|m7|m7_sweep`), and `main.cpp:68–96` has branches for
  `ab_sweep`, `m6_sweep`, `diag` only — **anything else falls through to `run_ab`**. A new
  `experiment=m9` that is not given a branch in `main.cpp` would silently run the M5.1 driver.

Those five bullets are the whole of M9's implementation surface. Nothing else in the machine
needs to move.

### 1.4 Current measured state (unmodified binary, this working tree)

`./bin/dx100_tests` → **3 979 checks, 0 failures**. The eight published anchors, re-derived with
the exact flags of §7.1, reproduce to the cycle (14/14 runs, all `conservation=OK`):

| Workload | knobs (N = 16 384, ρ = 0, elem 4 B, seed 1) | A (SIMT) | B (M8 bulk) | B vs A |
|---|---|---:|---:|---:|
| W0 | Λ=16384, P=256, κ=1 | 30 573 | 38 775 | 0.788 |
| W1 | Λ=16384, P=512, κ=0 | 270 753 | 156 419 | 1.731 |
| PSCR | Λ=16384, P=2048, κ=0 | 1 908 638 | 857 517 | 2.226 |
| STRID | Λ=1024, P=64, κ=1 | 22 664 | 17 953 | 1.262 |
| HOT | Λ=2, P=1, κ=0 | 4 361 | 10 867 | 0.401 |
| DUP | Λ=256, P=4, κ=0 | 6 831 | 11 190 | 0.610 |
| PGLO | Λ=1024, P=16, κ=0 | 17 797 | 17 018 | 1.046 |
| PERM | Λ=16384, P=512, κ=0 | 270 753 | 156 419 | 1.731 |

The spread is the motivation for M9 and it is not a small effect: on the same memory system,
with the same footprint and the same element count, B is **2.6× faster** on PSCR and **2.3×
slower** on HOT. B is slower than A on W0, HOT, DUP and (marginally) STRID, and in every one of
those cases the loss is the descriptor fetch plus the index stream plus the organizer — work
that carries no information for a direct or regular access.

---

## 2. Proposed operation metadata representation

### 2.1 The smallest practical representation: one bit per instruction

```cpp
// sim.hpp, next to Trace
enum class OpClass : u8 { Simt = 0, Bulk = 1 };   // exactly two; no third category

// A compiler-supplied annotation, one entry per memory instruction, in
// instruction order. Size == trace.n_instr. Immutable once built, and carried
// as a value exactly like BulkWorkload.
struct ClassMap {
  std::vector<u8> cls;                        // OpClass per instruction id
  u8 at(u32 instr) const { return instr < cls.size() ? cls[instr] : 0; }
  u64 count(u8 k) const;                      // instructions in class k
};
```

Two values, no scoring, no confidence, no "maybe". For a 16 384-element / 4-lane workload that
is 4 096 bytes of input — the same order as the descriptor itself.

**Why one bit is enough.** Everything else a bulk operation needs is already an architectural
field of `BulkDesc` (`sim.hpp:442`):

| Dispatch-relevant fact | Where it already lives |
|---|---|
| this op is an indirect gather | the new class bit |
| index array base, element width | `index_base`, `index_bytes` |
| data base, element width | `data_base`, `data_bytes` |
| element count, first element id | `count`, `elem_base` |
| where the descriptor itself lives | `desc_line` |

So the compiler's contribution for a bulk op is the class bit **plus the descriptor it already
had to emit**, and its contribution for a SIMT op is the class bit alone. No new IR, no operand
descriptor, no address-expression encoding.

**Cost accounting.** The annotation is *not* charged to any SRAM budget, because the simulator
has no instruction-fetch model: there is nowhere to charge it and no mechanism that would read
it repeatedly. It is host-side input, like the index array's *values* before they are written
into the image. This is stated rather than silently assumed; if M10 adds an instruction fetch
model, one bit per memory instruction becomes a real (if negligible) cost.

### 2.2 Where the metadata enters

At the **trace boundary**, in the driver — the same seam `BulkWorkload` already uses. Concretely:

```cpp
ClassMap build_op_classes(const Config& cfg, const Trace& tr);   // workload.cpp
RunResult Sim::run_hybrid();                                     // builds both, then runs
```

`Config` gains three keys and no per-workload data:

```cpp
u32  dispatch      = 0;      // 0 = pure path (today's behaviour), 1 = hybrid
std::string op_class = "gen"; // "gen" = generator's recorded decision,
                              // "pattern" = post-hoc contiguity test (cross-check)
u32  dispatch_order = 0;      // 0 = engine submits first, 1 = SIMT submits first
```

The class map itself is **not** in `Config`. `Config` holds knobs that describe the machine;
the class map is per-workload content, and putting it in `Config` would make it part of
`environment_signature` (`ab.cpp:207`), which is exactly wrong — the two arms of a controlled
comparison must share one environment and may differ only in workload.

### 2.3 Distinguishing direct from indirect: annotation, not inspection

The policy is *static*, so the distinction is made once, before any request is formed:

| Source program | Class | Why (the metadata's justification) |
|---|---|---|
| `data[i]` | SIMT | address is affine in the lane index; no dependent load |
| `data[2*i]` | SIMT | affine; stride is a constant |
| sequential / fixed-stride sweep | SIMT | special case of the above with a linear cursor |
| `data[index[i]]` | Bulk | address depends on a loaded value |
| `data[random_index[i]]` | Bulk | same; the distribution is irrelevant to the class |

A real compiler derives this from the address expression in the IR: an induction variable with
a constant coefficient is SIMT; a value loaded from memory is Bulk. **M9 does not implement
that analysis and does not claim to.** What M9 does is impose the *interface* such an analysis
would produce, and then verify that a workload annotated that way behaves as predicted.

### 2.4 The metadata is emulated — stated precisely

The generator already makes this decision and then throws it away. In `build_trace`
(`workload.cpp:24`), per instruction:

```cpp
const bool regular = uni(rng) < cfg.kappa;   // then discarded; only the accesses are kept
```

and `kappa_real` is recovered afterwards by a contiguity test over each instruction's addresses
(`hi - lo == (n-1) * elem_bytes`).

M9's proposal:

* **`op_class = "gen"` (primary).** Record that `regular` bit into `Trace::instr_class` during
  generation and hand it to the simulator as the compiler's annotation. This is the honest
  abstraction: the simulator is *told* the class, exactly as it would be told by a frontend.
* **`op_class = "pattern"` (cross-check only).** Recompute the class from the addresses with the
  same contiguity rule `kappa_real` already uses, and require the two to agree (gate G0, §7.3).
  This is the strongest claim available without a compiler: the classification is *recoverable*
  from the trace by a rule a compiler could implement.

What this is **not**: no LLVM, no IR, no analysis pass, no cost model, no heuristics on the
memory system's own state. The class bit is not derived from cache behaviour, occupancy,
timing, or the benchmark identity.

**Why the emulation is conservative.** A `regular` instruction is genuinely sequential. A
scattered instruction draws 4 lanes from a 16 384-line pool; with `elem_bytes = 4` and 16
elements per line, four lanes landing in one line and in contiguous slots has probability
≈ 10⁻³ per instruction, so the annotation and the address pattern agree essentially always. The
residual error runs in the direction of calling an accidental-SIMT access "bulk", i.e. **the
emulation can only understate the hybrid's benefit, never overstate it**. That is the direction
one wants an emulated annotation to err in.

### 2.5 What the compiler abstraction does *not* provide

* No re-classification, no fallback, no deoptimisation, no per-element mixing. If the compiler
  says Bulk, the operation is bulk even when SIMT would be faster (HOT, DUP in §6.6).
* No cost model: `Config::dispatch` selects a static policy, and the policy does not consult
  `kappa_real`, occupancy, or any measured quantity.
* No store classification. See §6.4 and §8 Q1 — the simulator has no store path at all, so the
  "direct stores → SIMT" row of the policy is specified but **cannot be exercised**.

---

## 3. Dispatch architecture

```
                    Trace (one materialization, one seed)
        Access{addr, line, elem, instr, lane}   +   ClassMap.cls[instr] ∈ {SIMT, BULK}
                                  |
                    +-------------+--------------+
                    |   dispatch (static)        |   host-side partition, computed once,
                    |   cls[instr] ? BULK : SIMT |   before any request exists
                    +------+--------------+------+
                           |              |
     instructions with     |              |  elements whose instruction is BULK,
     cls == SIMT           |              |  compacted in trace order
                           v              v
            +----------------------+   +--------------------------------------+
            | SIMT front-end       |   | bulk workload builder                |
            | (M1–M4, unchanged)   |   | index[] compacted, ONE descriptor,   |
            | per-instruction      |   | elem_ids[] position→trace element    |
            | coalescer, per-core  |   | (armed = false if no bulk element)   |
            | slot vectors         |   +------------------+-------------------+
            +----------+-----------+                      |
                       |  line requests (client 0)         |  DESC, INDEX, TARGET
                       |                                   |  (client 1)
                       v                                   v
                  +---------------------------------------------+
                  |   ONE shared residency pool, R = 128        |
                  |   (partitioned by nobody; first-come)       |
                  +----------------------+----------------------+
                                         |
                  +----------------------v----------------------+
                  |   ONE MemorySystem, ticked exactly ONCE      |
                  |   per cycle                                  |
                  |   dTLB · L2 TLB · PTW/walkers · L1 · L2 · DRAM|
                  |   (byte-identical to M4–M8; no new structure)|
                  +----------------------+----------------------+
                                         |
                        done[] tagged by client bit (bit 63)
                    +--------------------+--------------------+
                    v                                         v
          SIMT completions                          engine completions
          free the slot, count                      M6 window / M7 cohorts / M8 tiles
                                                    → fanout → val_[], addr_[], run_.done
```

Three properties of the diagram are the design:

1. **One hierarchy, one tick.** The engine does not own the hierarchy in hybrid mode; it is a
   client of it, exactly like a core. `BulkEngine::tick` splits into `attribute_done` +
   `progress` so the driver can tick the hierarchy once and hand each client its own records.
2. **One residency pool.** The engine's `free_` *is* the shared pool, not a copy of it. The
   guard `residency() >= cfg_.R` in `generate`/`stream_index`/`idx_stream`/`start_next`
   becomes `free_.empty()`, which is provably the same predicate in the pure path (§6.1).
3. **Static dispatch, dynamic contention.** Which class an operation belongs to is fixed before
   the run; how the two classes share `R`, MSHRs, banks and ports is the same dynamic
   arbitration the machine already performed between its own streams. No new scheduler is added.

---

## 4. Execution flows

### 4.1 SIMT flow (unchanged except the instruction list)

1. `Sim::run` builds `begin[instr]` from `trace.acc` (instruction → trace range).
2. Each core collects its instruction ids (`i % cores == c`, or blocked mapping), **filtered by
   class** in hybrid mode: `if (cm.at(i) != (u8)OpClass::Simt) continue;`
3. Current instruction → coalesce its lanes into distinct `va_line`s (`c.cur_lines`).
4. Take slots from the pool; `held` requests are submitted up to `warps_per_core` per cycle.
5. `MemorySystem::submit` → `tlb_probe` → dTLB hit / MSHR join / refusal (backpressure).
6. `MemorySystem::tick` translates, probes L1 (or L2 under `mem_path`), DRAM, completes.
7. Completion frees the slot; `completed`, `completion_hash` updated.

Steps 3–7 are untouched. Only step 2 gains a predicate — that is the entire SIMT-side change,
and it is why an all-SIMT hybrid run must reproduce arm A to the cycle (gate G1).

### 4.2 Bulk flow (untouched M7/M8 path)

1. `build_bulk_workload` writes the compacted index array + one descriptor into the image.
2. `BulkEngine::submit` queues the descriptor; `start_next` fetches it (`desc_fetch = 1`).
3. `on_desc` validates and opens phase 1.
4. M8: `idx_stream` claims index lines into the tiled ring; `idx_issue` (mode 3) or the shared
   FIFO submits them; `on_index` marks the owning line ready.
5. M6/M8: `win_admit` consumes ready index values, forms target addresses, admits entries into
   the active window; `win_seal` sorts by line (dedup), orders by page, builds the page
   directory; M7 `win_issue_sched` issues from whichever page run the dTLB accepts.
6. `on_line` copies the filled line and queues fanout; `win_fanout` reconstructs each dependent
   element into `val_[elem]`, `addr_[elem]`, and re-derives the address from the index image
   (`addr_mismatch`).
7. `finish_desc` when `run_.done == run_.d.count` and no window or fill is outstanding.

Nothing in this flow changes. `count` is now the *bulk-class* element count, and the descriptor
is the same 64 B structure — which is the point: M9 does not add a mode to the accelerator, it
stops feeding it operations it was never meant to serve.

### 4.3 The hybrid driver (new, `Sim::run_hybrid`)

The loop is a merge of the two existing loops, and its intra-cycle order is fixed and documented
because it is a real contention decision:

```
build ClassMap (cm) and BulkWorkload (w) from the one Trace
partition: simt_instrs, bulk_count, elem_ids
eng = BulkEngine(cfg, *mem, &shared_pool)      // alias, not a copy
if (w.armed) eng.submit(w.desc)

for (cycle = 0;; ++cycle) {
    std::vector<u64> done;
    mem_->tick(cycle, done);                   // ONCE — the only tick in the process
    for (rec : done) {
        if (rec >> 63) eng_done.push_back(rec);        // client 1
        else          simt_done.push_back(rec);        // client 0
    }
    for (rec : simt_done)  free_slot(rec & 0xffffffff), ++completed;   // core 0
    eng.attribute_done(cycle, eng_done);       // was the first half of BulkEngine::tick
    eng.progress(cycle);                       // was the second half
    simt_issue(cycle);                         // system.cpp:70's per-core loop body
    peak_all = max(peak_all, simt_outstanding + eng_outstanding());
    if (peak_all > R) violation;               // NEW invariant, checked every cycle
    if (!simt_has_work() && eng.idle() && mem_->idle()) break;
}
```

**Ordering rationale.** Completions first (both clients must see their own fills before forming
new requests), then the engine, then the SIMT front-end. This preserves the order the pure bulk
path already uses internally (it attributes before it progresses) and gives the engine the first
submission opportunity in each cycle. It is a *tie-break*, not a scheduling policy: with
`eng_issue = 4 = warps_per_core` both front-ends form at most four line requests per cycle, so a
fixed order can bias which class gets a contended resource first. `dispatch_order = 1` reverses
it, and §7.5 requires that ablation to be reported so the bias is measured rather than hidden.

### 4.4 Element identity without touching the engine

The engine derives element identity as `elem = d.elem_base + pos`. Compaction renumbers elements,
so trace identity is lost inside the engine. It does not need to be recovered there:

* `BulkWorkload::elem_ids[pos] = trace element id of the pos-th bulk element` is kept host-side,
  alongside the index values it already keeps.
* The engine's own verification (`addr_mismatch`) already re-derives every target address from
  the index image and needs no element identity at all.
* The driver's value/address comparison (`truth_hash`/`engine_hash`, `ab.cpp:563/589`) iterates
  `i ∈ [0, count)` and hashes `elem_ids[i]` instead of `elem_base + i`. For the legacy path
  `elem_ids[i] == elem_base + i`, so the change is a no-op on every published run.

This keeps `BulkDesc` byte-for-byte as published — no on-memory format change, so `desc_fetch`
decoding and every M5–M8 baseline are untouched.

---

## 5. Required source changes

All line counts are estimates. "Additive" means no existing statement is modified.

### 5.1 `sim/src/sim.hpp`

| Change | Where | ~LOC |
|---|---|---|
| `enum class OpClass : u8 { Simt, Bulk }`; `struct ClassMap` | after `Trace` (`:222`) | 12 |
| `Trace::instr_class` (`std::vector<u8>`) | `struct Trace` | 1 |
| `Config::dispatch`, `Config::op_class`, `Config::dispatch_order` | `struct Config` (`:42`) | 3 |
| `BulkWorkload::elem_ids`, `BulkWorkload::bulk_count` | `:731` | 3 |
| `build_op_classes(const Config&, const Trace&)` | after `build_trace` decl | 1 |
| `build_bulk_workload(..., const ClassMap*)` overload (default `nullptr`) | `:744` | 1 |
| `BulkEngine(const Config&, MemorySystem&, std::vector<u32>* pool = nullptr)` | `:533` | 1 |
| `BulkEngine::attribute_done`, `BulkEngine::progress`, `pool()` accessor | `class BulkEngine` | 4 |
| `SlotPool` (`free`, `owner`, `held[2]`, `take/give/residency/has_free/consistent`) | after `Req` | 25 |
| `Sim::run_hybrid()` | `:751` | 1 |
| `M9Arms` / `M9Result` / `run_m9` / `print_m9` | after `AbResult` (`:813`) | ~30 |

### 5.2 `sim/src/workload.cpp`

| Change | ~LOC |
|---|---|
| Record `regular` into `t.instr_class` in the instruction loop of `build_trace` (one `push_back` next to the existing `regular` draw), plus the `"pattern"` variant that reuses the `kappa_real` contiguity block | 10 |
| `build_op_classes()`: choose source by `cfg.op_class`, validate size == `n_instr` | 15 |

### 5.3 `sim/src/bulk.cpp`

| Change | ~LOC |
|---|---|
| `BulkEngine` ctor: if `pool != nullptr`, `free_` is not populated and `pool_` is used by `alloc_and_enqueue`/`win_emit`/`abort_desc`/`finish_desc` | 6 |
| Replace the four `residency() >= cfg_.R` guards with `!slot_available()` (`start_next`, `generate`, `stream_index`, `idx_stream`) | 4 |
| `tick` → `attribute_done` + `progress` (make `attribute` reachable; keep `tick` as their composition) | 8 |
| `build_bulk_workload`: class filter — compact `tr.acc` by class, fill `index_values`/`elem_ids`, `armed = (count > 0)` | 14 |
| `run_bulk(w)` unchanged; `run_bulk()` passes `nullptr` | 0 |

### 5.4 `sim/src/system.cpp`

| Change | ~LOC |
|---|---|
| `Sim::run(Mode)`: instruction collection gains `if (cm_ && cm_->at(i) != Simt) continue;` (a nullable `const ClassMap*` member, null on the pure path) | 2 |
| `Sim::run()`: `if (cfg_.dispatch) return run_hybrid();` | 1 |
| `Sim::run_hybrid()`: the loop of §4.3, per-path conservation, new cross-path invariants, counters | ~110 |

### 5.5 `sim/src/memory.cpp`

**No change.** The pre-implementation review (§9.3) replaced the completion-tag proposal below
with a slot→owner table owned by the driver, so `memory.cpp`, `Req` and the
`(core << 32) | slot` identifier are left exactly as published. `tlb_probe`, `l1_probe`,
`l2_probe`, `cache_tick`, `dram_tick` and `finish` are untouched either way, which also removes
the only reason this milestone would have edited the hierarchy at all.

*Superseded detail (kept for the record):* `Req::client`, packed as
`(u64(client) << 63) | (core << 32) | slot`, was the original proposal — 2 LOC here plus one mask
in `system.cpp`. It remains the fallback if a later milestone needs multi-core hybrid runs (the
owner table assumes `cores == 1`).

### 5.6 `sim/src/ab.cpp` (additive only)

| Change | ~LOC |
|---|---|
| `truth_hash` / `engine_hash`: hash `w.elem_ids[i]` instead of `d.elem_base + i` (no-op when `elem_ids` is empty/identity) | 4 |
| `run_m9()`: arms A (SIMT), B (bulk), C (hybrid) on one materialized trace and one image; M9-specific identities (§6.2); returns `M9Result` | ~120 |
| `print_m9()`: the report table of §7 | ~50 |

Appending to `ab.cpp` rather than adding `m9.cpp` keeps `collect()` and `check_capacity()` (both
in an anonymous namespace in `ab.cpp:24`) available without promoting them to `sim.hpp` — which
would be a bigger, riskier edit to a published driver. No existing function body is modified
except the two hash helpers.

### 5.7 `sim/src/util.cpp`, `sim/src/main.cpp`, `Makefile`

| Change | ~LOC |
|---|---|
| `Config::set`: `dispatch`, `op_class`, `dispatch_order`; add `m9` to the `experiment` allow-list; report them in `to_string()` | 8 |
| `main.cpp`: `if (cfg.experiment == "m9") { run_m9 ... }` **before** the `run_ab` fallthrough | 6 |
| `Makefile`: unchanged (no new translation unit) | 0 |

**Total ≈ 330 lines added, ≈ 20 modified, 0 deleted.**

---

## 6. Correctness and accounting

### 6.1 Three defects that must be fixed first (blocking)

**(D1) Two residency pools over one budget.** `Sim::run` allocates `R/cores − latch = 128`
slots per core (`system.cpp:70`); `BulkEngine` allocates its own `R = 128`
(`bulk.cpp`, constructor). Running both in one process gives the hybrid up to 256 outstanding
line requests while reporting `R = 128`. Every timing number produced in that state is invalid,
and it would look *better* than either pure path for a reason that has nothing to do with
dispatch.

*Fix:* the engine's `free_` becomes an alias of one pool of size `R`, taken by whoever asks
first. The guard substitution is exact, not approximate: in the pure engine path every slot is
in exactly one of `{free_, unissued_, idx_q_, mem_.inflight_}`, so
`residency() == R − free_.size()` and `residency() >= R ⟺ free_.empty()`. Therefore replacing
`residency() >= cfg_.R` with `slot_available()` **cannot change a pure bulk run**, which gate S2
verifies by reproducing 156 419.

Hybrid mode additionally requires `cores == 1`, the same restriction `run_ab` already enforces
(`ab.cpp:219`), because with `cores > 1` the SIMT arm holds one slot vector *per core* and merging
them into one pool would change its behaviour. Pure runs at any core count are untouched, so no
published result depends on this restriction.

**(D2) Two callers of `MemorySystem::tick`.** If both front-ends keep their own loop, the
hierarchy advances twice per simulated cycle: DRAM moves two lines per cycle instead of one,
MSHRs resolve twice as fast, and the run is silently ~2× optimistic. *Fix:* the split of
§5.3; in hybrid only `run_hybrid` calls `mem_->tick`.

**(D3) Ambiguous completion record.** `(core << 32) | slot` cannot distinguish engine slot 3
from SIMT slot 3, both core 0. A mis-attributed record either frees the wrong slot (double-free
→ two requests holding one slot, breaking the R invariant) or completes an element that was
never fetched (silent value corruption). *Fix:* the client bit of §5.5. It is inert for both
pure paths because neither ever sees a record from the other client.

### 6.2 Invariants

| Invariant | Status | Checked in |
|---|---|---|
| `simt_generated == simt_issued + simt_unissued` | existing, over the filtered universe | `Sim::run_hybrid` |
| `simt_issued == simt_completed + simt_inflight` | existing | `Sim::run_hybrid` |
| `simt_completed == simt_generated` | existing | `Sim::run_hybrid` |
| `bulk: elems_done == desc.count` | existing (`finish_desc`) | `BulkEngine` |
| `bulk: addr_mismatch == 0` | existing | `BulkEngine::on_target` / `win_fanout` |
| `bulk: residency() <= R` | existing | `BulkEngine::tick` |
| **`held[SIMT] + held[BULK] <= R` on slot residency, every cycle** (the coalescer latch is reported separately; §9.1) | **new — the M9 invariant** | `run_hybrid` |
| **`simt_instr + bulk_instr == n_instr`** | new | `run_hybrid` |
| **`simt_elems + bulk_elems == N`** | new | `run_hybrid` |
| **`bulk.desc_submitted == (bulk_elems ? 1 : 0)`** | new | `run_m9` |
| **`bulk.idx_lines_req == ceil(bulk_elems·index_bytes/64)`** | new | `run_m9` |
| peak `dram_max_q <= dram_q`, L1/L2 MSHR peaks ≤ capacity | existing | `check_capacity` |
| `bank_mask` inside configured banks | existing | `check_capacity` |

The central one is the third from the last block: `simt_outstanding + engine_outstanding <= R`.
It is the only statement that distinguishes an M9 hybrid from a machine with double the
residency, and it must be asserted **every cycle**, not at peak, because a transient overrun is
exactly the failure mode D1 produces.

### 6.3 Element completion and result delivery

Completion is per element on the bulk side (`win_fanout` retires every element that depends on
a filled line; `on_target` retires one per element on the M5 path) and per **line** on the SIMT
side. Result delivery is asymmetric and this must be stated plainly:

* The bulk path *stores* results: `val_[elem]` and `addr_[elem]` (`sim.hpp:533`), which is what
  `engine_hash` and `addr_mismatch` verify.
* The SIMT path stores nothing. It is a memory-*timing* model: it forms requests, coalesces, and
  completes; no value is captured anywhere and no register file exists.

Consequence: **M9 cannot compare the two paths' output values for the same element, because only
one path produces values.** What can be verified is (a) every element is processed exactly once,
(b) the union of the two paths' element sets is the whole workload, (c) the bulk path's addresses
agree with the index image element by element, and (d) the union of touched lines matches the
pure-SIMT arm's touched lines (`addr_set_hash`, `ab.cpp:579`). Claims beyond that require a
value path in the SIMT front-end, which is a different milestone.

### 6.4 Memory ordering, dependencies, duplicates

* **Ordering.** The generator emits independent loads; there are no stores in the model at all
  (`MemorySystem` has no write path — `write_va` is host image setup, not a coherent store).
  So no ordering rule is exercised or constrained by M9, and none is claimed. If stores are ever
  added, the class boundary becomes a *memory-ordering boundary*: `data[index[i]] = x` followed by
  `data[j]` must not be reordered across paths, because the two paths have no shared ordering
  point. This is the single largest future risk of the design (§8 Q1).
* **Dependencies.** None in the trace. The engine does serialise descriptors (`start_next`
  returns while `active_`), so a workload with several *dependent* bulk operations would be
  correct by construction, if slow. M9 uses **one** compacted descriptor, so nothing changes.
* **Duplicates.** Dedup happens inside a sealed window across bulk elements only; SIMT
  coalescing dedups only within one instruction. Two classes never share a dedup structure, so
  no duplicate can be merged away across the boundary — i.e. an element is never "satisfied" by
  the other path's fetch. This is required for the element-count identity to mean anything, and
  it falls out of the existing design rather than needing a rule.
* **HOT and DUP remain bulk** even though SIMT is 2.5×/1.6× faster there (§1.4). That is a
  deliberate policy choice: M9 tests *dispatch*, not *classification*. Reporting C(HOT) ≈ B(HOT)
  = 10 867 against A = 4 361 is the honest outcome of the stated policy, and it is the evidence
  a future classifier would need.

### 6.5 Faults and translation

* Address faults (`addr_faults`, `addr + data_bytes > VA_LIMIT`) can only occur on the bulk
  side, because SIMT addresses come from the generator and are always in range. The existing
  behaviour — counted, not silently dropped — is unchanged, and the element still counts as
  generated, so `elems_done + addr_faults == count` remains the identity to assert.
* Translation is fully shared: the engine submits with `core = 0` and `bypass_l1 = true`, and
  under `mem_path = l1` both classes traverse core 0's dTLB, the L2 TLB, the walk cache and the
  walkers (`memory.cpp:107`, `:238`). M7's page cohorts still operate on the engine's own sealed
  order, so they are unaffected by SIMT traffic — but they now *compete* for the same 4 dTLB
  MSHRs. Expect `tlb_refuse_parked`/`tlb_refuse_full` to rise in hybrid runs; that is contention
  in an existing structure, not a new mechanism, and it is measured, not modelled away.
* No new fault category, no new page-table path, no change to walk latency or walker count.

### 6.6 Can both classes coexist in one workload?

Yes, and the partition is exhaustive and disjoint by construction: `ClassMap` has exactly one
entry per instruction, `OpClass` has exactly two values, so every instruction — and therefore
every element — belongs to exactly one path. What *cannot* coexist is a single element in both
paths; that is excluded by the partition and asserted (`simt_elems + bulk_elems == N` plus the
disjointness of the two element lists).

Two consequences worth stating: a workload may be entirely SIMT (bulk workload `armed = false`,
zero descriptors, zero index lines — gate G1) or entirely bulk (input list empty — an idle SIMT
front-end, gate G2), and both are legal single-class members of the hybrid arm. That is what
makes the routing table of §7.2 a set of *hybrid* runs rather than a set of special cases.

### 6.7 Does dispatch change existing accounting or residency?

| Counter | Effect |
|---|---|
| `slots_total` | unchanged (`= R`), now one pool instead of two disjoint ones in hybrid |
| `simt_peak_slot`, `simt_peak_outstanding`, `bulk_buf_max` | unchanged meaning, now *partial*: each describes one class. The sum is what R bounds. |
| `requests_generated`, `issued`, `completed` | per front-end, unchanged. In hybrid they are **not** a whole-workload total; the driver reports the union explicitly. |
| `completion_hash` | two independent hashes. The all-SIMT gate requires C's SIMT hash to equal A's exactly; the all-bulk gate requires C's engine hash to equal B's exactly. |
| `l1_reads/hits`, `l2_*`, `dram_*`, `walks`, `pte_fetches` | unchanged, and now genuinely aggregate over both classes — which is what makes them comparable across arms |
| `bulk_*` engine counters | unchanged; all zero when no bulk element exists |
| `tlb0_*` | unchanged; aggregate over both classes, so **not** attributable per class without a new counter (§8 Q5) |

No published number changes: the pure paths keep their own pools, their own tick, and their own
completion records, so every M4–M8 result must reproduce bit-exactly under S2's gate.

---

## 7. Controlled benchmark plan

### 7.1 Arms

Frozen environment: `configs/ab_controlled.cfg` (cores 1, warps 4, lanes 4, R 128,
`mem_path = l1`, `lsu_pending = 0`, L1 256×4-way, L2 16384×8-way, dTLB 16×4 with 4 MSHRs
`dtlb_parked = 2`, L2 TLB 512, 2 walkers × 150 cy, 2 banks, `frfcfs = 1`, `dram_q = 0`). Common
overrides: `experiment=none --set N=16384 rho_dup=0 elem_bytes=4 bulk_wl=E0 seed=1
batch_window=8192 bulk_max_elems=262144 max_cycles=200000000 bulk_index_base=8388608
bulk_desc_line=98304`.

| Arm | Flags | Meaning |
|---|---|---|
| **A** | `mode=baseline` | SIMT only, today's M1–M4 path |
| **B** | `mode=bulk page_sched=1 idx_mode=2` | bulk only, M8 as published |
| **C** | `dispatch=1` (with the class policy of §2) | M9 hybrid |

`bulk_max_elems = 262144` is carried unchanged from the published sweeps for comparability; it is
not binding here (C's descriptor covers `count` ≤ 16 384, and every workload's index array is
`ceil(16384·4/64) = 1 024` lines). The shifted bases (`bulk_index_base = 134217728`,
`bulk_desc_line = 1572864`) are needed only for P > 2048 and therefore affect none of the
workloads below.

### 7.2 Routing (single-class fidelity)

Initial policy, unchanged from the brief: W0 → SIMT; W1, PSCR, STRID, HOT, DUP, PGLO → Bulk.

| Workload | Class map | C must equal | Expected C | Tolerance |
|---|---|---|---|---|
| W0 | all SIMT | A = 30 573 | 30 573 | **exact** |
| W1 | all Bulk | B = 156 419 | 156 419 | **exact** |
| PSCR | all Bulk | B = 857 517 | 857 517 | **exact** |
| STRID | all Bulk | B = 17 953 | 17 953 | **exact** |
| HOT | all Bulk | B = 10 867 | 10 867 | **exact** |
| DUP | all Bulk | B = 11 190 | 11 190 | **exact** |
| PGLO | all Bulk | B = 17 018 | 17 018 | **exact** |

"Exact" is the right tolerance, not a convenience: a single-class hybrid is the same
computation with one client idle, so any difference is a defect in the driver or in D1–D3. The
gates are not "close", they are cycle-identical, with identical `l1_reads`, `l2_reads`,
`dram_reads`, `walks` and `completion_hash`.

### 7.3 Fidelity gates

| Gate | Statement | Why it can fail |
|---|---|---|
| **G0** | `op_class = "gen"` and `"pattern"` produce the same map on all M9 workloads | generator draw vs contiguity accident (§2.4) |
| **G1** | C(all-SIMT) ≡ A: cycles, L1/L2/DRAM/TLB reads, `completion_hash` | the class predicate perturbs instruction order; an idle engine still ticks |
| **G2** | C(all-Bulk) ≡ B: cycles, `bulk_desc_submitted`, `bulk_idx_lines_req`, `bulk_tgt_req_gen`, `addr_mismatch` | compaction renumbers elements; shared pool changes the reserve rule |
| **G3** | C never issues a descriptor for a SIMT-only workload: `bulk_desc_submitted == 0`, `bulk_idx_lines_req == 0`, `bulk_tgt_req_gen == 0` | the workload builder must return `armed = false` |
| **G4** | No element lost or duplicated: `simt_elems + bulk_elems == N`, `bulk.elems_done + bulk.addr_faults == count`, `addr_mismatch == 0`, union `addr_set_hash` == pure-SIMT `addr_set_hash` | compaction, `elem_ids` bookkeeping |
| **G5** | `simt_outstanding + engine_outstanding <= R` every cycle; peak == R on at least one point (no under-use) | D1 |
| **G6** | Determinism: same config twice → same cycles and hashes | any hidden state (iterator order, uninitialised `elem_ids`) |
| **G7** | Pure-path regression: the 14 runs of §1.4 still reproduce exactly, `dx100_tests` still 3 979/0 | D2/D3 touching the shared code |

### 7.4 Mixed-class points (the actual M9 measurement)

Single-class runs verify the plumbing; they cannot show a benefit, because each routes everything
the way one existing arm already routes it. The measurement is κ ∈ (0,1). These A and B numbers
are **measured now**, with the flags of §7.1 at Λ = 16384, P = 512, fixed footprint, varying only
κ — so A and B are the same workload under both existing paths, and C is the new composite:

| κ | κ_realized | A (SIMT only) | B (bulk only) | B vs A | B `tgt_req` | B `dram_reads` |
|---:|---:|---:|---:|---:|---:|---:|
| 0.00 | 0.0000 | 270 753 | 156 419 | 1.731 | 16 384 | 17 446 |
| 0.25 | 0.2446 | 220 123 | 145 032 | 1.518 | 13 121 | 13 872 |
| 0.50 | 0.4958 | 169 826 | 131 199 | 1.294 | 9 913 | 10 427 |
| 0.75 | 0.7527 | 120 110 | 118 689 | 1.012 | 6 853 | 7 438 |
| 1.00 | 1.0000 | 40 281 | 42 305 | 0.952 | 4 096 | 5 132 |

Two facts to carry into the analysis: the crossover for this footprint lies between κ = 0.75
(1.012) and κ = 1.0 (0.952) — note that κ = 1 at P = 512 is 40 281, not W0's 30 573, because W0
uses P = 256, so footprint and κ are not confounded here — and the bulk arm's
request count falls faster than the SIMT arm's because a regular instruction's four lanes
coalesce into one line while four scattered lanes are four lines.

**Hypothesis to test (not a result):** C(κ) < min(A(κ), B(κ)) for 0 < κ < 1, with the gap widening
with κ, because C stops paying descriptor + index + organizer work on the regular share while
still organising the scattered share. If C ≈ A on some point, the bulk path is contributing
nothing there and the routing policy — not the dispatch mechanism — is what needs revisiting;
those are different conclusions and the report must separate them.

### 7.5 Counters and tests

**Counters to add** (all in `run_hybrid`, exported under stable names):

| Counter | Meaning | Expected / identity |
|---|---|---|
| `dispatch_mode` | 0/1 | gate |
| `simt_instr`, `bulk_instr` | instructions per class | sum == `n_instr` |
| `simt_elems`, `bulk_elems` | elements per class | sum == N |
| `simt_share_x1000` | SIMT element fraction | ≈ κ_realized |
| `peak_total_outstanding` | max over cycles of SIMT + engine **held slots** (latch excluded, reported as `simt_staging_max`) | **≤ R** |
| `dispatch_order` | which client submits first | ablation axis |
| `simt_completed`, `bulk_elems_done` | per-class completion | == expected |
| `bulk_desc_submitted`, `bulk_idx_lines_req`, `bulk_tgt_req_gen` | unnecessary-work proof | 0 for all-SIMT |

Every existing counter is retained unchanged; the report must quote both the per-class numbers
and their sum, never the per-class number alone as if it were the workload's.

**Ablations** (each changes one thing): `dispatch_order` 0/1; `idx_mode` 0/2 in C; `page_sched`
0/1 in C; `desc_fetch = 0` and `idx_oracle = 1` in C (the M6.1 diagnostics must still work, and
they bound how much of C's bulk cost is acquisition).

**Tests to add** to `tests/tests.cpp` (existing harness, `CHECK` macros, same style as
`test_m4a_exact_equivalence`):

1. `test_dispatch_partition` — random class maps: exhaustive, disjoint, sizes sum to `n_instr`.
2. `test_dispatch_all_simt_identical` — hybrid ≡ `Mode::Baseline` on cycles, hierarchy reads and
   `completion_hash` (G1).
3. `test_dispatch_all_bulk_identical` — hybrid ≡ `run_bulk` on cycles and all `bulk_*` volumes (G2).
4. `test_dispatch_no_index_for_simt` — all-SIMT: three volume counters are zero (G3).
5. `test_dispatch_shared_residency` — adversarial map, assert `simt + engine <= R` every cycle
   and that the peak reaches R (D1 regression).
6. `test_dispatch_element_integrity` — union `addr_set_hash` == pure-SIMT (G4).
7. `test_dispatch_determinism` — two runs, identical (G6).

### 7.6 Threats to validity

* **Emulated classification** (§2.4): the class is *supplied*. G0 bounds the disagreement with the
  only rule a compiler could implement here, and the error direction understates the benefit.
* **One seed.** Every number is `seed = 1`. The κ sweep's realized κ differs from nominal by up
  to 0.005, so the x-axis is the realized value; both are reported.
* **Fixed submission order.** `dispatch_order = 0` gives the engine priority on a contended
  cycle. The ablation in §7.5 exists because this is a real bias, not a detail.
* **One footprint in the mixed sweep.** Λ = 16384, P = 512. The routing table covers the
  footprint axis (PSCR at P = 2048, HOT at Λ = 2), but the mixed points do not, so nothing here
  says how C behaves at high page dispersion with a mixed class map.
* **No stores** (§6.4). Half the stated policy (direct stores, sequential stores) is unimplemented.
* **Per-class translation attribution** is not separable: `tlb0_*`, `l2tlb_*` and `walks` are
  aggregates over clients. Per-class attribution is available only by running the pure paths.

---

## 8. Risks, unresolved questions, implementation sequence

### 8.1 Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | Shared pool implemented as a copy → 2R capacity | **invalidates every C number, silently optimistic** | G5 asserted every cycle; `test_dispatch_shared_residency` |
| R2 | `mem_.tick` called twice | ~2× optimistic DRAM/MSHR behaviour | S2 gate: pure paths bit-exact; single tick site in `run_hybrid` |
| R3 | Completion mis-attribution | double-free or phantom completion | client bit + `test_dispatch_shared_residency` |
| R4 | `idx_reserve` no longer honoured under contention (SIMT takes the reserved slots) | index stream starvation; bulk side slows for a non-obvious reason | measure `bulk_idx_buf_stall`/`bulk_idx_wait_stall`; do **not** retune the reserve in M9 (that would be a new optimization) — report it |
| R5 | Hybrid measured as "better" purely from a smaller bulk `count` (less index traffic) | overstates M9: less work, not better routing | always report A, B, C at identical N and identical footprint; C's bulk `count` = bulk elements, and the *total* elements must still equal N |
| R6 | Compaction changes dedup opportunities (a window now holds only bulk elements) | C ≠ B on a single-class bulk workload | G2 makes this a hard failure, so it cannot pass unnoticed |
| R7 | `experiment=m9` without a `main.cpp` branch | silently runs `run_ab` and prints a plausible report | §5.7; assert the driver name in the first line of output |

### 8.2 Unresolved questions

| # | Question | What would resolve it |
|---|---|---|
| Q1 | Do stores need a class-specific ordering rule? | Out of M9 scope: the model has no store path. Adding one changes DRAM accounting and every baseline. Defer to M10, and until then M9 claims nothing about stores. |
| Q2 | Should several bulk operations in one workload stay one compacted descriptor or become one descriptor each? | M9 uses one; the engine serialises descriptors anyway (`start_next`), so per-op descriptors could only *lose* overlap. Measure only if a future compiler emits dependent bulk ops. |
| Q3 | Is the fixed submission order (engine first) the right default? | The `dispatch_order` ablation in §7.5. If the difference is large, the tie-break is a policy decision that belongs in M10, not a silent default. |
| Q4 | Should an all-SIMT hybrid construct an engine at all? | Design choice: constructing it and never arming it costs nothing (no requests, no state beyond `R` slot ids). G1 proves it is inert. |
| Q5 | Can per-class translation cost be attributed without new counters? | No. Either add per-client dTLB/L2TLB counters (small, but a `MemorySystem` change) or report the aggregate and the pure-path decomposition. M9 recommends the latter. |
| Q6 | Does a mixed class map change the M7 cohort policy's behaviour on the bulk share? | It cannot: cohorts index the engine's own sealed order. It can change *translation contention*, which R4 covers. |

### 8.3 Implementation sequence

| Step | Content | Gate before proceeding |
|---|---|---|
| **S0** | this document | reviewed |
| **S1** | `OpClass`, `ClassMap`, `Trace::instr_class`, `build_op_classes`, partition counters, tests 1 and 7 | 3 979 existing checks still pass; G0 holds on all 7 routing workloads + 5 κ points |
| **S2** | the three structural fixes: client bit (`memory.cpp`), `tick` split (`bulk.cpp`), shared pool (`bulk.cpp` ctor + guards) | **14 pure runs of §1.4 reproduce exactly; `dx100_tests` 3 979/0** |
| **S3** | `build_bulk_workload` class filter, `elem_ids`, SIMT instruction filter, `run_hybrid` with the engine idle, `experiment=m9` | **G1**: C(W0) == 30 573 with identical hierarchy counters; **G3**; tests 1, 2, 4, 6 |
| **S4** | engine inside the hybrid loop | **G2**: C(W1) == 156 419, C(PSCR) == 857 517, C(STRID) == 17 953, C(HOT) == 10 867, C(DUP) == 11 190, C(PGLO) == 17 018; **G5**; tests 3, 5 |
| **S5** | κ sweep + ablations + `print_m9` | **G4, G6**; the 5 mixed points of §7.4 with C vs A/B |
| **S6** | `docs/m9-report.md` | separates measured from predicted; reports R4/R5/R6 exposures |

S1 and S2 are independent and can land in either order; both must be green before S3, because a
C measurement obtained before them is not a measurement of dispatch.

---

## 9. Pre-Implementation Correctness Review

This section is the pre-implementation gate. It reviews only the three infrastructure issues (D1
shared residency, D2 cycle advancement, D3 completion identity), plus the pure-path equivalence
argument and the execution-model confirmation. **No source file was modified for this review.**

### 9.0 Verdict

| Issue | Status | Basis |
|---|---|---|
| **D1** shared residency budget | **PASS — specifiable, one correction needed** | every allocation and release site is accounted for; the `SlotPool` ownership below makes `held[SIMT] + held[BULK] + free == R` structural. The correction: the invariant is on **slot residency**, not on "outstanding", because the SIMT coalescer latch (≤ `lanes`, default 4) has always lived outside the budget. |
| **D2** cycle advancement | **PASS — single owner, phase-preserving** | both existing loops already have the same phase (tick → completions → submit); merging them with one `mem.tick` at the top of the cycle executes the same statements in the same relative order. |
| **D3** completion identity | **PASS — smaller fix than proposed** | `Req::slot` is **opaque payload**: `MemorySystem` never reads it (only echoes it at `memory.cpp:554`). A slot→owner table therefore routes every response with **zero** change to `memory.cpp`, `Req`, or the `(core<<32)|slot` format. |
| Pure-path equivalence | **PASS — with two implementation constraints** | §9.4. The constraints: the hybrid pool's fill order must match `Sim::run`'s, and the shared pool must not be installed in the pure SIMT path at `cores > 1`. |

### 9.1 Shared residency budget (D1)

#### Exact allocation, release and accounting sites

| Site | File:line | Slot action |
|---|---|---|
| SIMT pool construction | `system.cpp:95-96` | `resize(per_core)`, fill `0..per_core-1`; `per_core = max(1, R/cores − latch_charge)` (`:85-87`) |
| SIMT allocation | `system.cpp:193-201` | at **coalescing time**, `r.slot = free_slots.back(); pop_back();` then pushed to `held` |
| SIMT submission | `system.cpp:167` | `mem_->submit(r, cycle)` — slot already held; a refusal keeps it held (retry next cycle) |
| SIMT release | `system.cpp:146` (and `:149` neutral) | on completion, `free_slots.push_back(slot)` |
| SIMT accounting | `system.cpp:236-245`, `:290`, `:314` | `slot += held + in_sys`; `peak_slot`; `check("peak slot residency <= R")` |
| Engine pool construction | `bulk.cpp:126-127` | `info_.assign(R)`, then `for (i = R; i-- > 0;) free_.push_back(i)` |
| Engine allocation | `bulk.cpp:175-176` (`alloc_and_enqueue`), `:895-896` (`win_emit`) | `free_.back(); pop_back()` — at **formation / line claim** |
| Engine refusal return | `bulk.cpp:905` | `free_.push_back(slot)` when `mem_.submit` refuses |
| Engine release | `bulk.cpp:365` (`attribute`), `:325`/`:354` (queued index slots on abort/finish) | `free_.push_back(slot)` |
| Engine capacity guards | `bulk.cpp:193, 413, 532, 628` | `if (residency() >= cfg_.R)` — the four admission tests |
| Engine residency | `bulk.cpp:145-147` | `unissued_ + idx_q_ + mem_.inflight_count()` |

Two facts from this table decide the whole design:

* **Lifetime is already identical on both sides**: a slot is taken at *formation* (SIMT when an
  instruction is coalesced, engine when an element is admitted or a line is claimed) and returned
  when the **fill** arrives — not at issue, not at retirement. That is why one pool can serve both
  without changing either front-end's behaviour.
* **Both sides already tolerate an empty pool**: SIMT stops expanding the current instruction at
  `system.cpp:193` and leaves the rest in `cur_lines` (the latch); the engine returns
  `slot_stall`/`buf_full_stall` and retries. Neither assumes it owns all of `R`, so neither needs
  a reservation protocol to share it.

#### Ownership

The pool is owned by **`Sim`** (the driver), not by either front-end. It is constructed only when
`cfg.dispatch == 1`:

```cpp
// sim.hpp — the only new shared state in M9
struct SlotPool {
  std::vector<u32> free;              // slot ids, LIFO (same access idiom as today)
  std::vector<u8>  owner;             // slot -> 0 SIMT, 1 BULK, 0xff FREE
  u64 held[2] = {0, 0};

  // Fill order reproduces the front-end whose published numbers must be preserved:
  //   ascending  == Sim::run     (per_core-1 handed out first)
  //   descending == BulkEngine   (0 handed out first)
  SlotPool(u32 slots, bool ascending);

  u32  take(u8 who) {                 // NONE when empty; never blocks, never over-issues
    if (free.empty()) return NONE;
    const u32 s = free.back(); free.pop_back();
    owner[s] = who; ++held[who];
    return s;
  }
  bool give(u32 s) {                  // false on double-free / phantom completion
    if (s >= owner.size() || owner[s] > 1) return false;
    --held[owner[s]]; owner[s] = 0xff; free.push_back(s);
    return true;
  }
  u64 residency() const { return held[0] + held[1]; }
  bool has_free() const { return !free.empty(); }
  bool consistent() const { return free.size() + held[0] + held[1] == owner.size(); }
};
```

`BulkEngine` receives `SlotPool* pool = nullptr`. When null it behaves exactly as today (own
`free_`); when non-null, `free_` **is** `pool->free` and the guards become `pool->has_free()`.
`SimtCore` receives the same optional pointer for its `free_slots` access at `system.cpp:193-201`
and `:146`.

#### How each path reserves and releases

| | reserve | release | on refusal |
|---|---|---|---|
| SIMT | `pool.take(0)` when a line of the current instruction is formed (`system.cpp:198`) | `pool.give(slot)` on completion (`:146`) | keeps the slot, holds the request in `held`, retries next cycle (`:167`) |
| Bulk | `pool.take(1)` at admission/claim (`bulk.cpp:175`, `:895`) | `pool.give(slot)` in `attribute` (`:365`) | returns it immediately (`:905`) |

No reservation, priority, or quota is introduced. First-come-first-served in the documented cyclic
order (§9.2) is the whole arbitration, which is what the brief requires: dispatch is a *class*
decision, not a residency entitlement.

#### How the coalescer latch is accounted for

Unchanged, and deliberately so. In `Sim::run` the latch (`cur_lines` not yet formed) is **outside**
the slot pool: `peak_slot` counts `held + in_sys`; `peak_outstanding = peak_slot + latched`; and
`ab_charge_latch` decides whether the latch is subtracted from `per_core` (`system.cpp:85-87`).
With the frozen environment (`lanes = 4`, `ab_charge_latch = 0`) arm A therefore runs at up to
**128 slots + 4 latched requests**. The M6 reports quote the other profile of the same flag
(**124 slots + 4 latched = 128 total**); §9.7 reconciles the two and fixes which one M9 must use in
each arm. M9 inherits the arm's convention rather than tightening it, so C remains comparable with
A and B:

* hard, every cycle: `pool.residency() <= R` (slots only);
* existing: `staging_max <= lanes` (`system.cpp:314`, `ab.cpp:219` §6);
* only if `ab_charge_latch = 1`: `pool.residency() + latched <= R`.

This is a wording correction to §6.2/§7.5 above: the M9 invariant is
**`held[SIMT] + held[BULK] <= R` on slots**, not `simt_outstanding + bulk_outstanding <= R`
undefined. Written the loose way, arm A alone would appear to violate it at 132, which is why the
definition has to be slot residency.

#### Simultaneous allocation, and enforcement

Both front-ends attempt allocation in the same cycle, at different points in the sequence. The
rule is a fixed cyclic order, not a score:

* `dispatch_order = 0` (default): engine progress (step 5) before SIMT progress (step 6).
* `dispatch_order = 1`: the reverse.

The loser sees an empty pool and stalls exactly as it would if the machine were smaller — no work
is dropped, no slot is double-taken. `take()` is atomic within the cycle because the loop is
single-threaded; there is no interleaving to arbitrate beyond which step runs first.

Enforcement is three independent checks, all cheap, all every cycle:

```
1. structural : pool.consistent()            // free + held0 + held1 == R, no slot in two sets
2. bound      : pool.residency() <= R        // implied by (1) for a well-formed pool
3. attribution: peak_slot_simt, peak_slot_bulk, peak_total  // reported; peak_total <= R must hold
```

Check (1) is the one that catches the real failure: a slot taken twice, or released into the pool
while still referenced by an in-flight request. Check (2) alone would not — a double-take makes
`free.size()` smaller and `held` larger, so (1) still balances while a request is unaccounted; the
`owner[]` tag is what makes that visible.

#### Does pure-path behaviour remain exactly unchanged?

**Yes, by not installing the pool in the pure paths.** `Sim::run` keeps its per-core vectors and
`BulkEngine` keeps its own `free_` whenever `pool == nullptr`. This matters beyond tidiness: with
`cores > 1`, `Sim::run` deliberately gives each core `R/S` slots (`system.cpp:85-87`); a single
pool of `R` would let one core exceed its share, which would change every multi-core result ever
published. The pool exists only in `run_hybrid`, which requires `cores == 1` (the same restriction
`run_ab` enforces).

### 9.2 Cycle advancement (D2)

#### The actual call graph

```
Sim::run(Mode)          system.cpp:70      for (;; ++cycle) { mem_->tick(cycle, done) ... }
  |-- mem_->tick       :137               hierarchy advances; fills this cycle's `done`
  |-- completions      :138-149           release slots, hash, in_sys--
  |-- submit+coalesce  :159-205           mem_->submit at :167

BulkEngine::tick        bulk.cpp:491      { mem_.tick(cycle, done); attribute(...); progress }
  |-- mem_.tick        :493               hierarchy advances; fills `done`
  |-- attribute        :494 -> :360       on_desc / on_index / on_target / on_line; frees slots
  |-- ag_cycles/start_next/progress  :497-513

Sim::run_bulk          bulk.cpp:1280     for (;; ++cycle) { eng_->tick(cycle); if (eng_->idle()) break; }
```

Both loops have **the same phase**: advance the hierarchy at the top of the cycle, process this
cycle's completions, then submit new requests. A request submitted at cycle *c* is therefore first
advanced by the tick at *c+1* in both paths. That identity is what makes a merged loop
timing-neutral, and it fails the moment either path ticks twice.

#### One authoritative owner

**The driver owns the cycle.** In hybrid mode `Sim::run_hybrid` is the only caller of
`MemorySystem::tick`; `BulkEngine::tick` splits into the two halves the driver needs
(`bulk.cpp:491-513`):

```cpp
// bulk.cpp — pure-path behaviour preserved by construction: same statements, same order
void BulkEngine::tick(u64 c) {
  std::vector<u64> done;
  mem_.tick(c, done);          // :493
  attribute_done(c, done);     // :494  (was attribute)
  progress(c);                 // rest of the old tick body
}
void BulkEngine::progress(u64 c) {
  if (active_) st_.ag_cycles++;
  start_next(c);
  if (windowed()) win_tick(c); else { generate(c); issue(c); }
  /* occupancy: buf_occ, buf_max, unissued_max, residency_violations */
}
```

In hybrid, `progress()` on an idle engine is provably inert: `active_ == false` skips `ag_cycles`,
`start_next` returns on an empty queue, `win_tick` finds no window (`fill_ < 0`, `iss_ < 0`),
`idx_stream` iterates zero times because `run_.d.count == 0`, and no `bulk_*` counter moves. That
is checkable directly: G1 requires every `bulk_*` counter to be zero on an all-SIMT run.

#### Exact per-cycle sequence (hybrid)

```
cycle c:
  1  ADVANCE     mem_->tick(c, done)                       // the only tick in the process
  2  ROUTE       for rec in done, in the order produced (stable filter, never sorted):
                     slot = rec & 0xffffffff;  core = rec >> 32
                     require slot < R, owner[slot] != FREE   // else hard failure
                     append to simt_done if owner==0 else eng_done
  3  SIMT RESP   for rec in simt_done: pool.give(slot); in_sys[core]--;
                     hash = fnv(hash, slot); hash = fnv(hash, c);   // same order as :141-142
                     completed++
  4  BULK RESP   eng.attribute_done(c, eng_done)            // frees via pool.give
  5  BULK GEN    eng.progress(c)                            // admit/generate/claim/submit
  6  SIMT GEN    simt_advance(c)                            // submit held; coalesce; take slots
  7  ACCOUNT     peak = max(peak, pool.residency()); require pool.consistent()
  8  TERMINATE   if (!simt_has_work() && eng.idle()) break
  -- mem_->finish(cycle); res.cycles = cycle
```

Mapping to the six required orderings: (1) front-end request generation = steps 5 and 6;
(2) submission = the `mem_->submit` calls inside 5 and 6; (3) advancement = step 1; (4) response
processing = steps 2–4; (5) resource release = `give()` inside 3 and 4 (and `bulk.cpp:905` for a
refused line); (6) completion checks = steps 7–8 plus the per-path identities of §6.2.

**Two details that are not cosmetic.**

* **Routing is a stable filter.** `done` is produced in `inflight_` order (`memory.cpp:554`), and
  the SIMT completion hash folds in both slot and cycle *in that order* (`system.cpp:141-142`).
  Filtering preserves it; sorting or bucketing would change `completion_hash` while leaving cycles
  intact — a G1 failure that would look like a hash bug rather than a routing one.
* **Step 5 before step 6 is a documented tie-break**, not a policy: it decides who wins a contended
  dTLB MSHR (`dtlb_mshr = 4`, `dtlb_parked = 2`), a contended L1 MSHR and the last free slot.
  `dispatch_order` reverses it and §7.5 requires the ablation.

#### Why pure-path timing is preserved

With one client active, the merged sequence reduces to exactly the existing one: steps 4–5 are
inert when no bulk element exists (§9.2 above), and step 2 is the identity filter, so
`mem.tick → process done → submit` occurs once per cycle, in that order, with an unchanged
`inflight_` population. The only new statement on the SIMT path is `pool.give/take` replacing
`free_slots.push_back/back`, which touches the same ids in the same order when the pool is filled
the same way (§9.4).

### 9.3 Completion identity (D3)

#### The smallest collision-free mechanism: a slot→owner table

`Req::slot` is opaque to the hierarchy. `memory.cpp` reads `core`, `vpn`, `va_line`, `stage`,
`probed`, `ready`, `gen`, `bypass_l1` — and never `slot`, which appears exactly once, echoed back:

```cpp
// memory.cpp:554
done.push_back(static_cast<u64>(r.core) << 32 | r.slot);
```

Because the hybrid shares one pool of `R` slot ids, and `cores == 1`, **the slot id already
uniquely identifies the request** within the cycle. The front-end is then read from `owner[slot]`,
which the pool maintains for free. So:

* **no new field on `Req`** (the `client` bit of §5.5 is dropped);
* **no change to `memory.cpp`** — the identifier format is preserved literally, in every mode;
* **no change to `Sim::run`'s `p >> 32` parsing**;
* one 128-byte `owner[]` vector and two counters, both owned by the driver.

This is strictly smaller than the bit-63 tagging proposed in §5.5 and it satisfies the format-
preservation requirement outright rather than by masking. It does depend on `cores == 1` and on
`R <= 2^32`, both already required. The bit-63 tag remains the fallback if a later milestone needs
multi-core hybrid runs; it costs one line in `memory.cpp` and one mask in `system.cpp`.

#### Routing a response

```
slot = rec & 0xffffffff
core = rec >> 32
require slot < R                      // range
require owner[slot] in {0,1}          // allocated, not FREE -> rejects phantom + double completion
who  = owner[slot]
pool.give(slot)                       // clears owner[slot] -> a second record for it fails
if who == SIMT:  in_sys[core]--; completed++; hash(slot, cycle)
else            : attribute_done -> on_desc / on_index / on_target / on_line
```

#### Verifying no response reaches the wrong operation or element

| Check | Catches |
|---|---|
| `slot < R` | malformed record, corrupt pack |
| `owner[slot] != FREE` before `give` | phantom completion, double completion, a record from an unsubmitted request |
| `pool.consistent()` every cycle | a slot in two sets — the D1 failure signature |
| `simt: completed == requests_generated`; `bulk: elems_done == count` | a response credited to the wrong class |
| `addr_mismatch == 0` (`on_target` / `win_fanout`) | a *right-class, wrong-element* attribution: every bulk element re-derives its address from the index image |
| union `addr_set_hash` == pure-SIMT hash | a lost or duplicated element in either path |
| `truth_hash` over `elem_ids[i]` | element identity after compaction (§4.4) |

One residual gap, stated rather than papered over: the SIMT path carries no element payload, so
"wrong element" for SIMT can only mean "wrong line request" — verified through the line set and
`completion_hash`, not through values (§6.3).

### 9.4 Pure-path equivalence argument

Each change is inert by construction, not by observation. The arguments are per change; the gates
of §7.3 are the empirical confirmation, and any failure means the argument is wrong.

**E1 — Slot pool (D1).** In the pure paths the pool is not installed (`pool == nullptr`), so
`SimtCore::free_slots` and `BulkEngine::free_` are the same objects with the same initial contents
and the same LIFO access. State transitions are unchanged: `take()` ↔ `back()+pop_back()`,
`give()` ↔ `push_back()`. No counter reads `owner[]` unless the pool exists. **Inert.**

**E2 — Guard substitution in the engine.** Claim: in the pure engine path,
`residency() >= cfg_.R` ⟺ `free_.empty()`. Proof: every slot is in exactly one of `{free_,
unissued_, idx_q_, inflight}`, because `alloc_and_enqueue` moves a slot `free_ → unissued_|idx_q_`,
`issue`/`idx_issue` move it `unissued_|idx_q_ → inflight`, and `attribute` moves it `inflight →
free_` (`bulk.cpp:175, 360, 365, 905`). Hence `residency() = unissued_ + idx_q_ + inflight =
R − free_.size()`, and the two predicates are the same function of the same state. The four guard
sites (`:193, 413, 532, 628`) therefore cannot change which cycle they fire on. **Inert.**

**E3 — Tick split (D2).** `tick` becomes `tick = mem.tick; attribute_done; progress`, i.e. the
same statements in the same order over the same state. No statement moved across the `mem.tick`
boundary. **Inert** for `run_bulk`, which still calls `eng_->tick(cycle)` and never calls
`attribute_done`/`progress` directly.

**E4 — Slot id *values* (the one non-inert detail).** Two questions, opposite answers:

* *Engine*: slot ids do not affect behaviour at all. The only id-dependent state is `info_[slot]`,
  an array whose contents move with the id; every access goes through a stored handle
  (`unissued_`, `idx_q_`, `r.slot`), `p.seq = seq_++` is id-independent, and `BulkStats::
  completion_hash` is folded over `elem`, not slot. So a differently ordered pool cannot change
  any `bulk_*` counter, any hash, or any cycle.
* *SIMT*: slot ids **do** enter `completion_hash` (`system.cpp:141`). Arm A fills
  `0..per_core-1` and takes from the back, so its first slot is `per_core-1`; the engine fills
  descending so its first slot is `0` (`bulk.cpp:127`).

**Constraint C1 (required):** the hybrid pool must be filled **ascending**, matching `Sim::run`.
Otherwise an all-SIMT hybrid reproduces A's cycles but not A's `completion_hash`, and G1 fails on a
cosmetic difference that would be easy to misdiagnose. With ascending fill and no engine
allocations, the id sequence is identical to A's.

**E5 — Class filter.** `if (cm_ && cm_->at(i) != Simt) continue;` with `cm_ == nullptr` in every
pure run: the instruction list, and therefore `begin[]`, `cur_lines`, `held`, `slot_allocs`,
`requests_generated` and the hash order are byte-identical. **Inert.**

**E6 — `elem_ids` in `truth_hash`/`engine_hash`.** For the legacy workload `elem_ids[i] ==
elem_base + i`, so the hashed tuple `(elem, addr, value)` is unchanged. **Inert.**

**Constraint C2 (required):** the shared pool must not be installed in `Sim::run`. With `cores >
1`, one pool of `R` would replace `S` pools of `R/S`, letting a core exceed its per-core share and
changing multi-core results. Hybrid is `cores == 1`; pure runs keep their vectors.

**What the gates then prove.** G1 (all-SIMT hybrid ≡ A: cycles, `l1/l2/dram/tlb` reads, `walks`,
`completion_hash`) and G2 (all-bulk hybrid ≡ B: cycles and every `bulk_*` volume) together test
E1–E6 on real runs. G7 re-runs the fourteen anchors of §1.4, which is the only statement that
matters about the *unmodified* paths. If any of those three fails, the corresponding argument above
is wrong — the gates are the falsifier, not a formality.

**If bit-exactness could not be guaranteed**, the smallest correction would be to drop pool sharing
and give the engine a *static* partition of `R` (e.g. 64/64) with a documented, measured loss of
peak residency. That is a worse experiment — it changes capacity rather than sharing it — and it is
not needed: E1–E6 hold.

### 9.5 M9 execution model — confirmation

| Requirement | Status | Where |
|---|---|---|
| One shared logical `Trace` | confirmed | one `build_trace`, one `shared_ptr<const Trace>` (`system.cpp:60-66`) |
| One `ClassMap` of emulated compiler metadata | confirmed | §2.1, §2.4 (two values, no score, no fallback) |
| SIMT-classified elements → SIMT front-end | confirmed | instruction filter, §4.1 / E5 |
| Bulk-classified elements → one `BulkWorkload`, one descriptor | confirmed | `build_bulk_workload` class filter, §5.3 |
| Original element identities preserved | confirmed | `BulkWorkload::elem_ids`, §4.4 |
| One `MemorySystem`, one global `R = 128` | confirmed | §9.1, §9.2 |
| Both paths advance concurrently | confirmed | merged loop, steps 5 and 6 |
| No runtime classifier / fallback / switching | confirmed | the class map is an input and is never consulted after step 0; `dispatch` is static |
| No multi-descriptor engine redesign | confirmed | below |
| **Dispatch precedes any descriptor/index acquisition** | **confirmed structurally** | the class filter runs on the host *before* `build_bulk_workload` writes anything; a SIMT element never enters the index array, so the engine cannot acquire anything for it. An all-SIMT class map yields `armed == false` — no descriptor is even submitted (G3) |

#### Is the serial descriptor queue a blocker?

**No, and it is deferred explicitly.** Three reasons:

1. **Cardinality.** The hybrid submits exactly one descriptor per run (all bulk elements compacted).
   `start_next` (`bulk.cpp:191`) serialises *across* descriptors; with one descriptor there is no
   second operation to overlap with, so serialisation costs nothing.
2. **Comparability.** Published arm B is also one descriptor (`run_bulk` submits one `w.desc`). C's
   bulk side therefore executes the same structure as B, which is what makes the κ sweep of §7.4 an
   apples-to-apples comparison.
3. **No hidden path.** A single descriptor also means the engine cannot discover work the compiler
   did not classify — there is no second descriptor to enqueue at run time, no queue to inspect,
   and no way to turn a SIMT-classified element into bulk work.

Deferred to a future milestone: multiple bulk operations per workload, with either dependent
ordering (correct today, but serialised) or independent overlap (which would need concurrent runs
and is an engine redesign, hence out of M9 scope by §4).

### 9.6 Remaining implementation risks from this review

| # | Risk | Why it survives review | Mitigation |
|---|---|---|---|
| C1 | Hybrid pool filled like the engine (descending) instead of like `Sim::run` | only `completion_hash` differs; cycles match, so it looks like a hash bug | E4; G1 compares the hash explicitly |
| C2 | Pool installed in `Sim::run` for pure runs | multi-core (`cores > 1`) results would move | E5/E6; E1's `pool == nullptr` rule; G7 |
| C3 | `residency()` left as the engine's admission test in hybrid | it counts **all** of `mem_.inflight_`, including SIMT requests → engine under-admits and its reported `bulk_buf_max` is wrong | use `pool.held[1]` / `pool.has_free()`; §9.1 |
| C4 | `idx_reserve` semantics under contention (`bulk.cpp:893` uses `free_.size()`) | SIMT can take the slots the index stream was meant to keep, starving acquisition | expected and measured (`bulk_idx_buf_stall`, `bulk_idx_wait_stall`); **do not retune** in M9 |
| C5 | Routing implemented as a sort/bucket instead of a stable filter | `completion_hash` changes with cycles intact | §9.2; G1 |
| C6 | A `done` record reaching a front-end that has not yet allocated that slot | phantom completion on an unowned slot id | `owner[slot] != FREE` assertion; the pool is built before the first tick |

---

### 9.7 Residency accounting: `R`, the latch, and the 124 + 4 reconciliation

Two accounts of arm A's capacity appeared in this project and they are both right — they describe
two settings of one flag. `ab_charge_latch` (`sim.hpp:180`, `system.cpp:85-87`) decides whether the
coalescer latch is subtracted from A's slot pool:

```cpp
const u32 latch_charge = cfg_.ab_charge_latch ? cfg_.lanes : 0;
const u32 per_core = std::max<u32>(1, cfg_.R / std::max<u32>(1, cfg_.cores) - latch_charge);
```

| Profile | `per_core` | Slots | Latch | Max **formed** requests | Where it is used |
|---|---:|---:|---:|---:|---|
| `ab_charge_latch = 0` (**shipped** `ab_controlled.cfg:127`) | 128 | 128 | ≤ 4, outside | **132** | every M5.1 / M7 / M8 published number, and all fourteen anchors of §1.4 |
| `ab_charge_latch = 1` (M6 fairness profile) | 124 | 124 | ≤ 4, charged | **128** | the M6 report's fairness claim and its `experiment=m6_sweep --set ab_charge_latch=1` runs |

Measured on this working tree, `experiment=none`, frozen config, `mode=baseline`:

| Profile | Workload | cycles | `slot_pool_per_core` | `latch_charge` | `slots_total` | `simt_peak_slot` | `simt_peak_outstanding` | `simt_staging_max` | `completion_hash` |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | W0 | 30 573 | 128 | 0 | 128 | 128 | 129 | 1 | 4 117 390 930 |
| 0 | W1 | 270 753 | 128 | 0 | 128 | 128 | **132** | 4 | 145 307 694 |
| 1 | W0 | 30 573 | 124 | 4 | 128 | 124 | 125 | 1 | 2 307 502 861 |
| 1 | W1 | 270 753 | 124 | 4 | 128 | 124 | **128** | 4 | 2 573 324 557 |

Three things follow from that table.

1. **`slots_total` is 128 in both profiles** (`cores·per_core + cores·latch_charge`), which is why
   `check_capacity`'s `slots_total == cfg.R` never distinguished them. The *decomposition* moves,
   not the total.
2. **Cycles are identical** at both settings on both anchors, because neither workload saturates
   the slot budget (W1 sits at `R ≈ 64`). Only `completion_hash` moves — slot ids shift by 4 — and
   that is the only reason the two profiles were ever distinguishable in the published tables.
3. The M6 report's own §8.1 recorded the observed M5.1 asymmetry first ("peak resident 132 (128 + 4
   latch)") and then *proposed* charging the latch (`per_core = R − lanes`, 124). The proposal was
   measured as an option, not adopted as the shipped default: `m6-bulk-window-implementation-report`
   records `ab_controlled.cfg` shipping `ab_charge_latch = 0` so the M5.1 anchors reproduce exactly,
   and the M6 sweep was invoked with an explicit `--set ab_charge_latch=1`. The stale sentence in
   `sim.hpp` ("the M6 sweep turns it on") describes that invocation, not `run_m6_sweep`'s code —
   the driver itself copies `base` unchanged (`ab.cpp:471-482`).

#### What `R` denotes in each arm

`R = 128` is the number of **formed line requests that may hold residency** in the whole memory
system, from the cycle a request is formed until its fill returns — the budget `MemorySystem`
reserves `inflight_` for, and the one every report means by "R".

| Arm | What `R` bounds | Capacity |
|---|---|---|
| **A** (SIMT) | the sum of the per-core slot vectors, `cores·per_core` = 128 (or 124) | 128 + ≤4 latch, or 124 + ≤4 = 128 |
| **B** (bulk) | `unissued_ + idx_q_ + inflight` (`bulk.cpp:145`), one client | 128 slots, no latch (engine has no coalescer; `run_ab` asserts `staging_peak == 0`) |
| **C** (hybrid) | one `SlotPool` of `R/cores − latch_charge`, shared by both clients | 128 + ≤4 latch, or 124 + ≤4 = 128 — **the same as A** |

The latch is not a formed request. It is `cur_lines.size() − cur_pos` (`system.cpp:236`): the line
**addresses** of the instruction currently being expanded, which have not been submitted, hold no
slot, do no translation and occupy no MSHR. Its capacity is bounded by `lanes` and asserted
(`system.cpp:314`). That is why it was reported separately in M5.1 and why the M6 fairness profile
charges it rather than ignoring it.

#### Does the shared pool add effective capacity? No

* **Against A:** the pool is sized to A's own slot count (`R/cores − latch_charge`) and the latch is
  accounted exactly as A accounts it. C's ceiling is *identical* to A's, not A + B.
* **Against B:** the pool replaces B's 128, it does not sit beside it. C can exceed B only by the
  ≤4-request staging latch — the asymmetry M5.1 §4 documented and M6 §8.1 proposed to charge. It is
  a published property of the SIMT front-end, not new capacity introduced by M9.
* **Against A + B naive:** 256 is exactly the defect D1 removed; the pool makes
  `held[0] + held[1] ≤ slots` structural.

If a strictly `≤ R` total is wanted for the hybrid, run **A and C with `ab_charge_latch = 1`** (the
M6 fairness profile, 124 + 4 = 128) and report it as the sensitivity row. Recommendation: keep
`ab_charge_latch = 0` as the primary C configuration, because that is the environment every
published anchor was measured in (G7), and report the charged variant alongside it. Never mix the
two profiles between arms of one comparison — `ab_charge_latch` is already in
`environment_signature` (`ab.cpp:202`), so `run_ab` fails the pair if they differ.

#### The two rules this fixes for the implementation

1. **Pool size** = `R / max(1, cores) − latch_charge`, not `R`. At `latch = 0` that is 128; at
   `latch = 1` it is 124. Sizing it at `R` unconditionally would give C four more slots than A in
   the charged profile — a real, if small, capacity increase.
2. **Fill order** = ascending (`0..n−1`, taken from the back), matching `Sim::run`'s
   `for (u32 i = 0; i < per_core; ++i) free_slots[i] = i` (`system.cpp:96`). With matching size and
   order, an all-SIMT C hands out the same slot ids in the same sequence as A and therefore
   reproduces A's `completion_hash` exactly (4 117 390 930 on W0 at `latch = 0`) — the check that
   would otherwise fail while cycles matched.

---

## 10. Recommendation

**Verdict: READY — proceed to M9 implementation.**

§9 reviews the three infrastructure issues and passes all three, subject to two implementation
constraints (C1: the hybrid pool must be filled ascending like `Sim::run`; C2: the pool must not
be installed in the pure SIMT path) and one scope exclusion (stores). No unresolved blocking issue
remains.

The design decisions the brief asked for are settled and grounded in the code: metadata is one
class bit per instruction entering at the trace boundary alongside the existing `BulkWorkload`;
the two front-ends already consume one logical workload through two derivations of one `Trace`,
so no workload or address generation is duplicated; dispatch happens before either front-end
forms a request, so a SIMT operation cannot reach a descriptor or an index buffer; and the
existing coalescer, M6 window, M7 cohorts and M8 tile ring are untouched — the hybrid only
changes which operations are handed to them.

**What was blocking is D1 — two independent residency pools over one `R` budget.** As the code
stands, running both front-ends in one process gives the hybrid up to 2R = 256 outstanding line
requests while reporting `R = 128`. Any cycle count measured in that state is invalid and, worse,
flatters M9 for a reason unrelated to dispatch. D2 (two `tick` callers) and D3 (ambiguous
completion record) have the same character: they are not design gaps, they are code-structure
preconditions, and the three together are ≈40 lines across `sim.hpp`, `bulk.cpp` and `system.cpp`
(`memory.cpp` is not touched at all, §9.3) whose correctness is checkable
because the pure paths must reproduce all fourteen anchor runs of §1.4 to the cycle afterwards.

Beyond that, one scope exclusion must be stated in any M9 report: **stores are not modelled**, so
the "direct stores → SIMT" half of the dispatch policy is specified but untested. Closing that
gap is a new milestone (M10), not a defect in M9.

---

## 11. As built (S3)

This section records what actually shipped and where it departs from §5. Nothing here was tuned
against benchmark results; where a measurement contradicted a prediction it is recorded as a
contradiction.

### 11.1 Delta from §5

| Design (§5) | As built | Why |
|---|---|---|
| `ClassMap` as a bitset over instructions | `std::vector<u8> cls`, one byte per instruction | The map is inspected once per instruction id and once per trace access; a byte avoids a second index computation and costs 4 KB at `n_instr = 4096`. The 1-bit-per-instruction *representation* claim of §2.1 is unchanged in substance. |
| `build_op_classes` from the generator only | `gen` (generator's recorded draw) plus `pattern` (contiguity rule), plus uniform `simt` / `bulk` | `pattern` is the §2.4 cross-check; the uniform maps exist so G1/G2 can hold the trace fixed and vary the annotation alone. All four are pre-execution and deterministic. |
| Class filter inside `run_bulk`, producing a filtered workload | Filter inside `build_bulk_workload`, before `BulkDesc` is constructed | §3 requires the decision to precede descriptor and index acquisition. Filtering at derivation makes that structural: no descriptor object ever exists for a SIMT element, and `desc.count` *is* the compacted count. The legacy path passes `cm = nullptr` and is byte-for-byte unchanged. |
| `Sim::run_hybrid` in `system.cpp` | `Sim::run_hybrid` in `bulk.cpp`, with its own copy of the SIMT issue/coalesce loop; `system.cpp` gains one branch | Keeps `Sim::run` textually identical, so the published SIMT path cannot be perturbed by dispatch. G1 (exact cycles and hash on seven traces) is the guard that the copy is faithful. |
| Completion identity via a tag in `Req::slot` | `SlotPool::owner[slot]` lookup (S2) | `Req::slot` is opaque to `MemorySystem` (it echoes the id at `memory.cpp:554`), so no tag was needed and `Req`, `memory.cpp` and `(core << 32) \| slot` are untouched. |
| `elem_ids` as an engine-side addition | `BulkWorkload::elem_ids`; `truth_hash`/`engine_hash` fold `elem_ids[i]` | The engine stays keyed on `elem_base + pos`; only the two verification hashes and the driver know the original identity. Empty `elem_ids` reproduces the published hash exactly. |
| §6.1's three blocking defects fixed in shared code | Same fixes, all in S2, all inert for the published paths | Independent pools → one `SlotPool`; two `tick` callers → one; ambiguous completion → owner table. |

Two additions the design did not list: `Config::dispatch_order` (a per-cycle generation-order
sensitivity knob, default 0, used for no reported number) and `Config::op_class` validation, so a
misspelt policy is an error rather than a silent fall-through to `gen`.

### 11.2 The design's own gates (§7.3), as run

| Gate | Result |
|---|---|
| **G0** `gen` ≡ `pattern` | PASS — identical instruction and element counts and identical C cycles on every tested workload (e.g. κ = 0.5: 2031 + 2065 instructions, 8124 + 8260 elements, 119 798 cycles both). |
| **G1** C(all-SIMT) ≡ A | PASS — 7/7 workloads, exact cycles and `completion_hash` (W0 30 573 / 4 117 390 930, W1 270 753 / 145 307 694), zero descriptors, zero index lines. |
| **G2** C(all-Bulk) ≡ B | PASS — 7/7, B reproduces every published M8 cycle count (W0 38 775, W1 156 419, PSCR 857 517, STRID 17 953, HOT 10 867, DUP 11 190, PGLO 17 018), C matches B on ~90 counters and on both output hashes. |
| **G3** no descriptor without bulk work | PASS — `bulk_desc_submitted = 0`, `bulk_idx_lines_req = 0`, `bulk_tgt_req_gen = 0` in every all-SIMT run; exactly 1 whenever bulk elements exist. |
| **G4** nothing lost or duplicated | PASS — partition exhaustive, `elems_done + addr_faults == count`, `addr_mismatch = 0`, `elem_ids` identity checked element by element. (The design's union-`addr_set_hash` check was not needed: A/B address multisets are compared directly at all-bulk, and the per-element check is strictly stronger than a set hash.) |
| **G5** shared residency | PASS — `peak_total_outstanding ≤ slot_pool_total` every cycle of every run; with `ab_charge_latch = 1` the pool is 124 and C peaks at 124. |
| **G6** determinism | PASS — two κ = 0.5 runs diff clean. |
| **G7** pure-path regression | PASS — 14/14 anchors exact, `dx100_tests` 4 032 / 0, `experiment=ab` status OK, `experiment=diag` OK. |

### 11.3 What the measurement changed in the reasoning

§7.4 hypothesised `C < min(A, B)` for all `0 < κ < 1`, widening with κ. Measured (Λ = 16384,
P = 512, shipped `ab_charge_latch = 0`): C/B = 1.058 at κ = 0.25, 0.913 at κ = 0.50, 0.802 at
κ = 0.75. The hypothesis holds for κ ≥ 0.50 and fails at κ = 0.25. The mechanism is the
acquisition prologue relative to a short run: at κ = 0.25 the engine's first target issue is at
cycle 13 350 of 153 475, so the accelerator contributes almost nothing early while the SIMT share
is paid for at SIMT's per-element rate. The design's claim in §6.6 that both classes can coexist was
about correctness, and that part held; its *performance* prediction was too optimistic at low κ.
No policy was changed in response — the classification rule is still the generator's, and HOT/DUP
were not re-routed.

**Correction after the contention diagnostic** (`docs/m9-dispatch-implementation-report.md` §9.10,
§9.12–§9.15): the per-element arithmetic in the paragraph above ("the SIMT share is paid for at
SIMT's per-element rate") is a decomposition of two phase lengths, not an isolation of the engine's
cost, and it is withdrawn as an explanation. The refusals are attributed event by event in the
report: at κ = 0.25 they occur mostly while the engine holds ~110 of 128 slots itself (89 % with only
the index reserve free), and at κ = 0.75 while SIMT holds ~126. The κ = 0.25 loss at the published
capacity disappears when the shared pool is halved, with the class map, workload and traffic
unchanged — so the sign at that κ is capacity-dependent rather than a fixed price of the regular
share. Respectively, concurrent execution does occur (11 953 / 9 973 / 5 063 cycles of shared
residency) but simultaneous *progress* is rare (134 / 132 / 134 cycles).

### Reproducing the numbers in this document

```bash
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests        # 4032 checks, 0 failures

# single arm, e.g. W1 bulk (M8)
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=none \
  --set N=16384 --set Lambda=16384 --set P=512 --set kappa=0 --set rho_dup=0 \
  --set elem_bytes=4 --set bulk_wl=E0 --set seed=1 --set batch_window=8192 \
  --set bulk_max_elems=262144 --set max_cycles=200000000 \
  --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
  --set mode=bulk --set page_sched=1 --set idx_mode=2          # → 156419

# dispatched run (M9): three arms + gates
./bin/dx100sim --config configs/ab_controlled.cfg --set experiment=m9 \
  --set N=16384 --set Lambda=16384 --set P=512 --set kappa=0.5 --set rho_dup=0 \
  --set elem_bytes=4 --set bulk_wl=E0 --set seed=1 --set batch_window=8192 \
  --set bulk_max_elems=262144 --set max_cycles=200000000 \
  --set bulk_index_base=8388608 --set bulk_desc_line=98304 \
  --set page_sched=1 --set idx_mode=2                         # → 119 798, gates PASSED
```

Workload knobs: W0 `Λ=16384, P=256, κ=1`; W1 `Λ=16384, P=512, κ=0`; PSCR `Λ=16384, P=2048`;
STRID `Λ=1024, P=64, κ=1`; HOT `Λ=2, P=1`; DUP `Λ=256, P=4`; PGLO `Λ=1024, P=16`; κ sweep at
`Λ=16384, P=512`. Complete command list, gate tables and limitations:
`docs/m9-dispatch-implementation-report.md`.


