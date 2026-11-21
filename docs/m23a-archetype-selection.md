# M23A — Archetype Selection

M22 left three implementation facts on the table: the shipped default is the
**unfused** build, the fused build is **3.80× smaller** for identical behaviour,
and the configuration that produced every published number is **not** what
`Config()` gives you. M23A turns those into one named archetype and pins it in
the test suite, so the irregular-access battery in M23D measures something
definite.

---

## 1. What the code actually defaults to [M]

Measured from `Config()`, not read from the header (probe:
`build/wm/m23_probe.cpp`):

| knob | default | note |
|---|---:|---|
| `batch_window` | **0** | the engine is *not* windowed by default |
| `page_sched` | **false** | the page scheduler is *off* by default |
| `page_order` | true | required by the fused layout |
| `page_cohorts` | 2 | |
| `dedup` | true | |
| `idx_mode` | **2** | tiled acquisition, the shipped path |
| `win_fused` | **false** | M22: the expensive representation |
| `page_dir` | 512 | direct-mapped, 12× oversubscribed on w4 |
| `reuse_dir` | false | M13 path, off |
| `ag_lanes` / `fanout_lanes` | 8 / 4 | |
| `idx_buf_lines` | 64 | |
| `R` | 128 | |

Two of these are traps. `batch_window = 0` and `page_sched = false` mean that a
bare `Config()` runs the **M5 streaming engine**, not the M7+ engine, and that
every windowed result in this tree was produced by explicitly setting both. A
reader who assumes "the default is the engine" is wrong, and a test that assumes
it will silently measure the wrong machine.

## 2. The three candidates

Measured on `configs/ab_controlled.cfg` (W1 permutation, 16 384 elements, one
descriptor), shipped acquisition path, scheduler on, two cohorts:

| | representation | window | cycles | realized arrays | vs A0 | area¹ |
|---|---|---:|---:|---:|---:|---:|
| **A0** | unfused (shipped) | 2×8K | 156 419 | 703 104 B | 1.00× | 0.253 mm² |
| **A1** | fused | 2×8K | 156 419 | 188 064 B | **3.74× smaller** | **0.068 mm²** |
| **A2** | fused | 2×16K | 152 059 | 286 656 B | 2.45× smaller | 0.103 mm² |
| (A0') | unfused | 2×16K | 152 059 | 1 097 472 B | — | 0.395 mm² |

¹ `bytes × 8 × 0.045 µm²/bit`, the labelled M22 density assumption; arrays only.

The fused build is cycle-identical to the unfused one at **both** window points
(156 419 and 152 059 exactly), which is the M15 result reproduced at scale rather
than at `W = 8`.

## 3. Selection

**Archetype A1: fused, 2×8K, `page_sched`, 2 cohorts, shipped acquisition.**

Reasons, in order of weight:

1. **Free.** 3.74× less storage for bit-identical cycles and 188 064 B of arrays
   (0.068 mm² nominal). There is no trade to argue about.
2. **The published matrix was measured against it.** A0 and A1 differ only in
   representation; every M19–M21 speedup transfers without re-measurement.
3. **8K, not 16K.** M21/M22: 16K buys +2.66× on an 8 MiB low-reuse set and
   0.5–1.0 % in the cache-resident regime, for +98 592 B. That is a regime-A
   instrument, so M23D keeps 16K as a *second data point* (A2), not as the
   archetype.
4. **It is the only candidate whose representation precondition is enforced.**
   A1 refuses `dedup = false`, `page_order = false`, `W > 65536` and `R > 255` at
   construction; A0 accepts them and silently costs 3.7× more.

## 4. What M23 pins, and what it leaves at the default

| item | M23 choice |
|---|---|
| `win_fused` default | **unchanged (false)** — flipping it is a one-line human decision after M23D; the archetype is asserted separately so both states stay visible |
| `batch_window` default | unchanged (0) — windowing stays opt-in |
| `page_sched` default | unchanged (false) |
| archetype knobs | **pinned** in `test_m23_archetype_config` |
| archetype numbers | **pinned** in `test_m23_archetype_baseline` (both builds, both window points) |
| modelled vs realized storage | **pinned** in `test_m23_model_vs_realized` |
| page directory | left in place, still dead; a deletion is not a baseline change and belongs to a later milestone |

## 5. What the battery will hold fixed

M23D varies access structure, so everything else has to be nailed down:

```
configs/ab_controlled.cfg
  experiment = none      batch_window = 8192 (and 16384 for the A2 point)
  page_sched = 1         page_cohorts = 2       page_order = 1
  idx_mode   = 2         win_fused  = 1         (A0 re-run for the ratio)
  R = 128   ag_lanes = 8   fanout_lanes = 4   lanes = 4
```

One variable per cell: the address pattern. Any cell that also changes a knob is
a different experiment and must be labelled as such.

## 6. Evidence base

| claim | source |
|---|---|
| fused ≡ unfused, exactly | M15 report; re-verified at 2×8K and 2×16K by `test_m23_archetype_baseline` |
| 3.74× / 3.83× array reduction | measured probe, this milestone |
| defaults as tabulated | `build/wm/m23_probe.cpp` against `Config()` |
| 16K worth +2.66× on 8 MiB, 0.5–1 % in regime B | M21 §4, M22 §4 |
| area conversion | M22 E1 (0.045 µm²/bit nominal, ±3.3×) |
