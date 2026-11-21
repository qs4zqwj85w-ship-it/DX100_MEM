# M23 — Plan

M22 ended with verdict 2: the mechanism is real, the shipped implementation is
3.4–3.7× larger than it needs to be. M23 does the three things that verdict
implies — freeze an archetype, make the baselines maintainable, then test the
archetype against access patterns that are more irregular than anything measured
so far — and it does them in five gated steps.

Everything from here on assumes **human review at every step**: each step ends
with a report, and the next step does not begin until the previous one's numbers
have been looked at.

---

## The five steps

| step | name | output | gate to pass it |
|---|---|---|---|
| **M23A** | structure and archetype selection | `m23-plan.md` (this file), `m23a-archetype-selection.md` | the archetype is named, its knobs are pinned in code, and the tests that guard them pass |
| **M23B** | baseline repair | `m23b-baseline-inventory.md`, baseline tests in `sim/tests/tests.cpp` | every historical baseline is accounted for (kept, re-anchored, or declared stale with a reason), and the archetype has its own pinned numbers |
| **M23C** | irregular-access harness | `tools/m23_irregular.py`, `build/wm/m23_matrix.sh`, `m23c-harness-report.md` | the generator is deterministic from a seed, its output is validated by the trace checker, and one warm-up cell reproduces under the existing runner |
| **M23D** | the irregular battery | `build/wm/m23_*.txt`, `m23d-irregular-results.md` | >= 8 patterns x 2 window points x 2 builds, every cell accounted, no unexplained violation |
| **M23E** | findings and decision | `m23e-combined-findings.md` | the archetype is either confirmed, amended, or rejected — with the evidence that decides it |

"Archetype" throughout means **the configuration that M23 carries forward**: the
representation (fused or unfused), the window size, the scheduler knobs, and the
engine width. M22's measurements were all taken on a configuration that is
neither the cheapest nor the most representative one, which is the specific
problem M23A fixes.

## Why baselines before the battery (M23B before M23D)

The battery's output is only interpretable against a fixed reference. Today the
reference is a set of hand-written literals spread over five test functions,
each of which:

* pins an absolute cycle count with no record of the configuration that produced
  it beyond a comment (`idx_mode = 0`, "the M7 configuration");
* duplicates numbers across tests (`39 322` appears in `m6_w0_diagnostic` and in
  `m7_baselines`; `233 475` in `m7_baselines` and `m6_w1_diagnostic`), so one
  model change breaks several tests with different failure messages;
* has **no baseline at all for the fused build at scale** — the only fused test
  uses `batch_window = 8`. Every published M18–M22 number is unfused, so a fused
  regression at 8K would be invisible;
* encodes a per-entry charge (`2 × (W·8 + L·16)`) that M22 measured to be 6.33×
  below the realized arrays.

M23B therefore adds a baseline layer rather than editing numbers: the
configuration signature is asserted next to the value, the fused/unfused identity
is asserted at 8K and 16K, and the modelled charge is pinned *together with* its
measured error so it cannot be mistaken for the hardware cost.

## What is deliberately out of scope

| not in M23 | why |
|---|---|
| changing `win_fused` to `true` as the default | that is a one-line decision for a human, taken after M23D shows whether the archetype survives irregular access; M23A pins the *current* default and the archetype separately so both are visible |
| deleting the page directory | M22 recommendation, not a test baseline; it changes `dir_occ_max`/`dir_collisions` reporting |
| re-costing `win_bytes` to the realized arrays | changes every logged `bulk_win_vec_bytes`... no: it changes `bulk_win_bytes`, which appears in M6/M7/M18 logs. Left as a documented discrepancy instead |
| RTL, synthesis, timing closure | M22's area figures are labelled assumptions; M23 stays at the simulator |
| new memory-system features | the battery tests the existing engine against harder patterns, it does not add mechanisms |

## Commands and conventions

All runs are from `DX100_MEM/sim`.

```bash
make -j                       # rebuild simulator + tests
./bin/dx100_tests             # 4 470 checks, 0 failures (M22 exit state)
./bin/dx100_tests m23_archetype_baseline   # one test in isolation
```

* Long simulations run as `BACKGROUND`; the SYNC cap is 295 s.
* There is no git repository. Integrity is argued with file hashes and
  cross-checks against the earlier reports, so M23 reports must name the exact
  command and configuration for every number they quote.
* Existing `m17_*`–`m22_*` reports and `build/wm/m18_*`–`m22_*` logs are read-only
  reference material.

## Irregular access: what "more irregular than normal" means here

The measured matrix so far varies **locality** (target-set size) and **duplicate
rate**. It does not vary the *structure* of the address stream, which is where a
hardware engine is most likely to break:

| pattern family | what it attacks | provisional parameter |
|---|---|---|
| strided / diagonal | page and row balance inside a run | stride co-prime to the page stride |
| block-cyclic | window composition when a run mixes hot and cold pages | 2–4 hot blocks, cold remainder |
| community-shuffled | the assumption that index order is uncorrelated with target locality | block permutation with 5–20 % cross-block edges |
| bursty / phase-changing | seal timing and double-buffer balance | phase lengths 1–8 windows |
| anti-sorted | whether page ordering can be defeated by an adversary | target order reversed within each page |
| degenerate | one page, one line, one element | boundary counts |
| high-collision | dedup and directory pressure at 8 KiB pages | 256+ elements per page |
| mixed-width | E0/E1/E2 index widths in one run | per-phase width change |

These are generated by a script with an explicit seed, not by editing the
simulator's built-in `wl` families, so the existing 10-workload matrix and every
published number stay untouched.
