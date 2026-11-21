# M28-A — Dispatch-only workflow validation

Validates whether the M27 dispatcher applies its classification correctly and whether that
classification represents the intended access patterns. **Dispatch only**: no request, cache, DRAM,
walk or timing model was instantiated. Every number below comes from loading a trace and building the
`ClassMap` — the same call the simulator makes before its first request.

**Result in one line:** the dispatcher applies its supplied classification exactly (5/5, zero
unclassified or duplicated instructions, exact instruction and element conservation); the
classification *source* is what limits it — the address-only rule misclassifies the small
working-set gather (W2), and on generated traces the annotation comes from a name suffix, not from
measured behaviour.

---

## 1. Test methodology

The dispatch decision is made in two steps, and the experiment measures them separately:

| step | code | what it does |
|---|---|---|
| source selection | `build_op_classes(cfg, tr)` — `sim/src/workload.cpp:291` | copies one of four metadata sources into a `ClassMap` |
| decision application | `cm.at(i)` — `sim/src/bulk.cpp:3301-3310`, `3318`, `3341`, `3383`, `3186` | partitions instructions and elements, generates each path |

Four metadata sources exist (`Config::op_class`): `gen` (the trace's own per-instruction annotation —
the production setting), `pattern` (recover the annotation from the addresses using the contiguity
rule κ_real), `simt` and `bulk` (uniform maps, the G1/G2 equivalence gates).

**Method: run every trace twice, changing only the metadata source.**

- `op_class=gen` answers question 1: *does the dispatcher apply the supplied classification?*
- `op_class=pattern` answers question 2: *could that classification have been derived from the access
  pattern alone?* Where the two disagree, the annotation is carrying information the address rule does
  not have.
- `op_class=simt` / `bulk` on W5 confirm the uniform gates still work (32 → all-SIMT, all-Bulk).

The harness is `sim/tools/m28a_dispatch.cpp` (new, added for this experiment). It parses the config
exactly like `main.cpp`, calls `build_trace(cfg)` and `build_op_classes(cfg, tr)`, then prints one row
per instruction (class, access count, contiguity, distinct lines, distinct pages, address range) plus
accounting checks. It constructs no `MemorySystem`, no `BulkEngine`, no event and no clock: nothing can
execute. Exit status is 0 only if the accounting checks pass; all 12 runs returned 0.

Trace generator: `tools/m28a_build_traces.py` (new). It writes the documented `M18TRC1` format
(`workload.cpp:29-37`): magic, `lanes`, `elem_bytes`, `n_instr`, `n_acc`, a `u8 cls[n_instr]` table,
then `n_acc` × `{u64 addr, u32 instr, u32 lane}`. Declared classes: `0 = SIMT`, `1 = Bulk`
(`OpClass`).

**Nothing in `sim/src/` or `configs/` was modified.** The only new files are the two tools above; the
dispatch source, the metadata sources and the configuration are byte-identical to the M27 state
(hashes in §3).

## 2. Trace descriptions and provenance

All five traces: `lanes = 32`, `elem_bytes = 8`, **64 instructions**, **2 048 accesses**,
read-only, no zero-access instruction, accesses grouped in instruction order (the loader's
constraints). Generator seed 20280201, so every trace is reproducible byte-for-byte.

| trace | construction | distinct lines | distinct pages | κ_real | declared |
|---|---|---|---|---|---|
| **W1** `w1_contiguous.trc` | instruction *i* covers `base + (i·32 + lane)·8` — one contiguous 256 B run (4 lines) | 256 | 4 | 1.0000 | 64 SIMT |
| **W2** `w2_small_gather.trc` | 32 distinct words sampled from a **512-word (4 KiB) pool**, reshuffled each instruction | **64** | **1** | 0.0000 | 64 SIMT |
| **W3** `w3_large_gather.trc` | 32 words uniform random over **8 MiB** | 2 028 | 1 275 | 0.0000 | 64 Bulk |
| **W4** `w4_page_clustered.trc` | one 4 KiB page per instruction, 16 distinct lines, 2 elements per line, irregular slot order | 1 024 | 64 | 0.0000 | 64 Bulk |
| **W5** `w5_mixed.trc` | 3 regions: 24 contiguous · 20 large-random · 20 page-clustered | 1 055 | 564 | 0.3750 | 24 SIMT / 40 Bulk |

The traces are synthetic and minimal by design: each isolates one characteristic, all have the same
instruction and element count, and W1/W3/W4 differ only in the pattern (not in size or lane count), so
a difference in selection cannot come from anything but the pattern.

W2's footprint is deliberately tiny (1 page, 64 lines — 4 KiB) so that "small working set" is a
measurable fact, not a claim: it fits in the L1 of any plausible configuration, and revisits the same
64 lines 32 times.

## 3. Exact source revision and configuration

`DX100_MEM` is not a git repository, so revisions are recorded as SHA-256 over the inspected files:

| file | sha256 |
|---|---|
| `sim/src/workload.cpp` (loader + `build_op_classes`) | `670c7ca9…6ae35ab` |
| `sim/src/sim.hpp` (`Config`, `Trace`, `OpClass`, `ClassMap`) | `449641e2…73835b6c` |
| `sim/src/bulk.cpp` (`run_hybrid` partition) | `6354949b…490501951` |
| `configs/m27_bulk.cfg` (production config) | `08487009…d20e76b` |
| `tools/m28a_build_traces.py` (new) | `f88f12af…f3d7676` |
| `sim/tools/m28a_dispatch.cpp` (new) | `bd72cf9c…fd439ef8` |

Effective configuration for every run: `configs/m27_bulk.cfg`, plus `--set lanes=32 --set
elem_bytes=8 --set dataset=<trace> --set op_class={gen|pattern|simt|bulk}`. The config file is used
only to obtain a `Config` object; `dispatch`, timing, cache, DRAM and window keys are never read by the
harness, as the harness stops at the map.

## 4. Dispatch implementation inspected

```cpp
// workload.cpp:291 — the only place a class is decided
ClassMap build_op_classes(const Config& cfg, const Trace& tr) {
  cm.cls.assign(tr.n_instr, OpClass::Simt);
  if (cfg.op_class == "simt")    return cm;                       // uniform SIMT
  if (cfg.op_class == "bulk")  { fill Bulk; return cm; }           // uniform Bulk
  if (cfg.op_class == "pattern") { /* contiguity rule, per instruction */ return cm; }
  if (!tr.instr_class.empty()) cm.cls = tr.instr_class;            // "gen": a copy
  return cm;
}
```

- **Where classification occurs:** `build_op_classes` (`workload.cpp:291-335`), called once from
  `run_hybrid` at **`bulk.cpp:3293`**, after the trace is loaded and **before** any request, descriptor
  or index element exists. It reads only the trace and the config.
- **Where the decision becomes fixed:** the returned `ClassMap` is a `const` local. Nothing re-reads
  the config, recomputes a class or consults a counter afterwards. Every later use is `cm.at(instr)`
  (`bulk.cpp:3301, 3308, 3318, 3341, 3383` and the element filter at `3186`). `Config::dispatch_order`
  (`bulk.cpp:3250`) selects the *order* in which the two paths are driven, not the *class*.
- **What the metadata is:** for `gen`, `cm.cls = tr.instr_class` — a **verbatim copy** of a `u8` table
  read from the trace file (`workload.cpp:69-70`, `t.instr_class.resize(n_instr); f.read(...)`). For a
  synthetic trace the table is written at generation time (`workload.cpp:193-201`,
  `regular = uni(rng) < cfg.kappa`); for a file trace it comes from the file.
- **What it is not:** no cost model, no occupancy estimate, no cache state, no timing input, no runtime
  feedback. `build_bulk_workload` *verifies* the compaction against the map (`bulk.cpp:3318`) but
  cannot change it.

So step 1 (`gen`) is a straight table lookup: disagreements between the dispatcher and the declared
expectation are impossible by construction, and that is exactly why the `pattern` comparison is the
informative half of this experiment.

## 5. Results for W1–W5

`instr`/`acc` columns are counts; "match" compares the observed selection with the expected
classification in the request table.

| workload | expected | `gen`: SIMT / Bulk instr | `gen`: SIMT / Bulk elems | match | `pattern`: SIMT / Bulk | pattern match |
|---|---|---|---|---|---|---|
| W1 contiguous | SIMT | **64 / 0** | 2 048 / 0 | ✅ | 64 / 0 | ✅ |
| W2 small working-set gather | SIMT | **64 / 0** | 2 048 / 0 | ✅ | **0 / 64** | ❌ |
| W3 large random gather | Bulk | **0 / 64** | 0 / 2 048 | ✅ | 0 / 64 | ✅ |
| W4 page-clustered gather | Bulk | **0 / 64** | 0 / 2 048 | ✅ | 0 / 64 | ✅ |
| W5 mixed | both | **24 / 40** | 768 / 1 280 | ✅ (mixed=1) | 24 / 40 | ✅ |
| W5 forced `simt` | – | 64 / 0 | 2 048 / 0 | gate check | – | – |
| W5 forced `bulk` | – | 0 / 64 | 0 / 2 048 | gate check | – | – |

**W5, instruction by instruction** (verified at every region boundary from the dump; the full 64-row
table is in `build/wm/m28a/w5_mixed_gen.txt`):

| instruction | region | access pattern | selected | per-instruction evidence |
|---|---|---|---|---|
| 0–23 (24 instr) | A | contiguous 256 B | **SIMT** | `contig=1`, 4 lines, 1 page |
| 24–43 (20 instr) | B | 32 random words over 8 MiB | **BULK** | `contig=0`, 32 lines, 32 pages |
| 44–63 (20 instr) | C | one 4 KiB page, 16 lines | **BULK** | `contig=0`, 16 lines, 1 page |

Boundary rows as printed (`instr class n_acc contig lines pages first last`):

```
22  SIMT  32 1  4  1   5248512  5248760
23  SIMT  32 1  4  1   5248768  5249016
24  BULK  32 0 32 32   6345680 14503424
43  BULK  32 0 32 32   6706272 14574160
44  BULK  32 0 16  1   7340608  7344008
63  BULK  32 0 16  1   7418048  7421704
```

The dispatcher therefore **does select different paths within one workload**, and the transition is
exactly at the region boundary: two classes coexist in one run (W5: 24 SIMT / 40 Bulk, `mixed=1`).

## 6. Selection accuracy and accounting checks

| check | W1 | W2 | W3 | W4 | W5 |
|---|---|---|---|---|---|
| total instructions | 64 | 64 | 64 | 64 | 64 |
| total accesses | 2 048 | 2 048 | 2 048 | 2 048 | 2 048 |
| SIMT + Bulk instr = total (`instr_sum_ok`) | 1 | 1 | 1 | 1 | 1 |
| SIMT + Bulk acc = total (`acc_sum_ok`) | 1 | 1 | 1 | 1 | 1 |
| unclassified instructions | 0 | 0 | 0 | 0 | 0 |
| missing / zero-access instructions | 0 | 0 | 0 | 0 | 0 |
| duplicated assignment (`class_entries` = 64, `one_assignment_per_instr`) | 1 | 1 | 1 | 1 | 1 |
| matches expected (`gen`) | ✅ | ✅ | ✅ | ✅ | ✅ |

- **One assignment per instruction:** `ClassMap::at()` returns exactly one `u8` per instruction index
  (`sim.hpp:553-556`), `cm.cls.size() == trace.n_instr` in every run, and the parallel loop
  `for i < tr.n_instr` visits each index once. There is no path that assigns twice or skips.
- **No element can be lost:** the partition counts elements by their own instruction's class
  (`bulk.cpp:3307-3310`) and the compaction is cross-checked against the map at `bulk.cpp:3318`;
  `simt_acc + bulk_acc = 2 048` in all runs.
- **Selection accuracy against the declared expectation is 5/5 with `gen`** — but see §8: with `gen`
  this is a table copy, so the interesting accuracy figure is the `pattern` column (4/5).

## 7. Dispatch behaviour: what the two sources say

| | W1 | W2 | W3 | W4 | W5 |
|---|---|---|---|---|---|
| declared (`gen`) | all SIMT | all SIMT | all Bulk | all Bulk | 24 SIMT / 40 Bulk |
| address-derived (`pattern`) | all SIMT | **all Bulk** | all Bulk | all Bulk | 24 / 40 |
| agree? | yes | **no** | yes | yes | yes |

The single disagreement is the informative one. **W2 is irregular at the instruction level** (32
scattered words, κ_real = 0, 27–28 distinct lines per instruction) **but cheap in aggregate** (64
lines total, 1 page, re-read 32 times). A rule that looks only at one instruction's address spread
cannot see the reuse, so `pattern` sends it to Bulk. The annotation says SIMT. Both statements are
consistent with the data; only one of them is right for the hardware.

## 8. Limitations of the current static selector

Three separate questions, kept separate:

**(1) Does the dispatcher correctly apply the classification it is given?**
**Yes, verified.** It is a verbatim copy of a `u8` table (`workload.cpp:332`), consumed by a single
`at()` lookup per instruction, with exact instruction and element conservation and no unclassified or
duplicated instruction in any of the 12 runs. There is no loss, no reordering, no re-decision. This is
a property of the *application*, not of the *judgement*.

**(2) Does the supplied classification represent the intended workload characteristics?**
**Only as well as the annotation.** With `gen` it matches the intended answer for all five workload
classes, but that is because I wrote both the trace and its annotation: the experiment shows the
pipeline honours the annotation, not that the annotation was inferred. The `pattern` column shows the
real content: an address-only rule reproduces 4 of 5 classes and **fails on W2** (small working-set
gather), because instruction-local irregularity and workload-local footprint are different properties
and the rule only sees the first.

Worse for real traces, and worth stating plainly: for a generated trace the annotation is
`regular = uni(rng) < cfg.kappa` (a random draw), and in `tools/build_trace.py` the class is
`BULK if kind.endswith("_gather") else SIMT` — **a string suffix on the opcode name**. So a real CSR
`x_gather` is annotated Bulk regardless of whether its column footprint is 4 KiB or 4 GiB. The
annotation encodes the *name of the operation*, not its measured behaviour.

**(3) Could a compiler or a runtime generate good classifications?**
**Not established here — and the evidence points to "not from addresses alone".** A compiler could
supply the annotation (the format supports it, and `pattern` proves a contiguity rule is
implementable), but W2 shows a contiguity rule is insufficient: distinguishing a hot 4 KiB gather from
a cold 8 MiB gather needs a footprint/reuse estimate (working-set analysis, a profile, or a cache
simulation), none of which the current selector has. A runtime policy would need cache state that the
selector deliberately never reads. **This experiment does not validate intelligent runtime
selection** and nothing in it should be read as showing one.

Additional limits, measured:

- The map is fixed before execution and cannot be revised; a misclassification is permanent for the
  run (there is no feedback path from stalls back to `cm`).
- Two classes only. No "small irregular" class exists, which is exactly the W2 case; the annotation
  must force it into SIMT or Bulk, and the only lever is a human/generator decision.
- `dispatch_order` changes interleaving, not selection: the same trace yields the same classes in
  either order.
- The selector is per instruction, never per element: a single instruction whose 32 lanes span a whole
  page (W4) and one with a single hot line are indistinguishable to `build_op_classes`.

## 9. Unexpected behaviour

1. **None of the 12 dispatch runs violated accounting.** All returned 0, all had
   `instr_sum_ok = acc_sum_ok = 1`, `unclassified = 0`, `missing = 0`.
2. **W2 is the designed surprise**, and it is a limitation rather than a malfunction: `pattern`
   classifies it Bulk. Recorded, not "fixed" — changing the rule would be a selector redesign, which
   this experiment is not authorized to do.
3. **A harness bug in this experiment, disclosed:** the first build printed `last_addr` as
   `18446744073709551615` because the per-instruction maximum was initialised to `~0ull`. Fixed by
   initialising to 0, rebuilt, and all dumps in `build/wm/m28a/` are from the corrected binary (the
   class columns, which are the object of study, were never affected — the affected field is a printed
   address range only).
4. **A trace-generator bug, disclosed:** the first version of `page_clustered()` sampled
   `LANES // 4` line slots from `range(LINE // ELEM)` and produced 16 addresses instead of 32. The
   assertion caught it before any dump was produced; the corrected version samples `LANES // 2` line
   slots from `range(4096 // LINE)` = 64 lines. Every trace used here is from the corrected generator
   (hashes in `build/wm/m28a/*.json`).
5. **`kappa_real` is 0 for W2, W3 and W4** — three very different workloads share one κ value. κ is a
   contiguity statistic, and this is a direct demonstration that it cannot separate them; if a future
   selector uses κ, W2/W3/W4 are indistinguishable to it.

## 10. Conclusion and recommended next step

**Conclusion.** The M27 dispatcher applies its supplied classification exactly and losslessly, and it
does select different paths inside a single workload (W5: 24 SIMT / 40 Bulk, boundary at instruction
24). What it does not do — and does not claim to do — is *infer* the classification: the decision is a
lookup in a table that, on real traces, is written from an opcode-name suffix, and the one workload
where the intended answer (a hot 4 KiB gather) is not visible in the instruction's own addresses is the
one the address-only rule gets wrong. The experiment therefore validates **the application of the
classification**, not the intelligence of it.

**Recommended next step (one, on authorization):** measure how much a W2-style misclassification
*actually costs* — run W2 as SIMT (as annotated) and as Bulk (as `pattern` would decide) and compare
resource behaviour, with the other four traces as controls. That converts "the rule cannot see the
working set" into a quantified error bound, and it is the only evidence that would justify building a
footprint-aware classifier. No full workload panel, no selector change.

**Stopped here.** Dispatch decisions recorded, no memory request executed, no simulator run, no
architecture or configuration change, all historical artifacts untouched.

---

### Artifacts produced (new files only)

| path | content |
|---|---|
| `build/wm/m28a/{w1..w5}_*.trc` | the five traces |
| `build/wm/m28a/{w1..w5}_*.trc.json` | per-trace metadata: declared classes, per-instruction table, sha256 |
| `build/wm/m28a/<trace>_{gen,pattern}.txt` | full per-instruction dispatch dumps |
| `build/wm/m28a/w5_mixed_{simt,bulk}.txt` | uniform-gate dumps |
| `tools/m28a_build_traces.py`, `sim/tools/m28a_dispatch.cpp` | the trace generator and the dispatch-only harness |
