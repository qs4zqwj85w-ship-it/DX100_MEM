# M23B — Baseline Inventory and Repair

Every exact-value assertion in `sim/tests/tests.cpp`, what it anchors, the
configuration that produced it, and its status after M23B. The point of the
inventory is that a baseline without a named configuration is not a baseline —
it is a number that happens to be true today.

---

## 1. What was there

`grep` over the suite finds **40 exact-value sites** (`CHECK(x == <literal>)`,
three digits or more) in ten test functions, plus two `CHECK_NEAR` tolerances.
They split into three groups.

### 1.1 Legacy / calibration anchors — keep as published

| test | literals | config | status |
|---|---|---|---|
| `ab_baseline_unchanged` | cycles 280 432; requests 16 384; dram 16 417; l1 16 384; peak_slot 128 | `configs/vortex_ref.cfg`, no engine | **current** — the M5 measurement, deliberately frozen |
| `m6_switch_off` | 278 034 / 17 409 / 17 444 | `ab_controlled`, window 0 | **current** (M5.1 §8) |
| `m6_legacy_identity` | 270 753 | `ab_controlled`, `experiment=none` | **current** |

These are the only baselines that are cheap to trust: they are the SIMT path,
which M5–M22 never modified, and they are the project's regression floor.

### 1.2 Windowed diagnostic anchors — keep, but they name a non-default path

| test | literals | config | status |
|---|---|---|---|
| `m6_w0_diagnostic` | A 30 573; B0 39 322 / B1 38 985 / B2 33 268 / B3 32 931; dram 5 128 / 4 101; first_tgt 4 431; prefix 4 432; park 20 577; full 0 | `ab_controlled`, W0, `P=256`, `W=8192`, **`idx_mode=0`** | **current, non-default path** |
| `m6_w1_diagnostic` | A 270 753; B0 233 475 / B1 233 138 / B2 225 560 / B3 225 223; dram 17 444 / 16 417; idx 1 024 / 0; refuse 155 501 / 155 407; walker 153 600 | `ab_controlled`, W1, `W=8192`, **`idx_mode=0`** | **current, non-default path** |
| `m7_baselines` | 233 475 / 156 912 / 154 807 (W1 legacy / 2 cohorts / 4 cohorts); 39 322 / 39 530 (W0) | `ab_controlled`, `W=8192`, **`idx_mode=0`**, `page_sched` on | **current, non-default path** |

The `idx_mode = 0` in all three is documented in the tests themselves (M8 moved
the acquisition default and the M8 audit charged scheduler installation). It is
correct as history, but it means **the shipped acquisition path had no windowed
baseline at all**.

### 1.3 Structural counts — keep

`bulk_memory_accounting` (1 024 elements, 64/128 index lines, 1 024 targets),
`m6_resource_accounting` (2 048 entries, 8 batches, 2 × 2 048/4 organization
cycles), `m6_window_advantage` (16 384 lines, 529 walks), `m6_fanout_accounting`
(512 lines, 1 536 merged, 512 + 128 + 1 requests), `vortex_ref_preset`,
`m6_index_oracle` (128 index requests, 2 048 oracle elements). These assert
*ratios and counts* derived from the workload definition, not timings, so they
survive a model change by construction. No action.

## 2. The three defects the inventory exposes

**(D1) The shipped path had no windowed baseline.** All windowed pins use
`idx_mode = 0`; every M18–M22 number uses `idx_mode = 2`. A regression in the
tiled acquisition path at 8K would have failed nothing.

**(D2) Five literals are duplicated across functions** and are silently coupled:
`39 322` (`m6_w0_diagnostic` B0 and `m7_baselines` W0 legacy), `233 475`
(`m6_w1_diagnostic` B0 and `m7_baselines` W1 legacy), `270 753`
(`m6_switch_off` and `m6_w1_diagnostic` A), `17 444` (`m6_switch_off` dram and
`m6_w1_diagnostic` B0 dram), `16 417` (`ab_baseline_unchanged` and
`m6_w1_diagnostic` B3). One model change would produce five failures with five
different messages for one cause. They are **left as literals on purpose** —
they are published figures tied to their reports — but M23B documents the
coupling so a future break is diagnosed as one change, not five.

**(D3) `m6_resource_accounting` pins the modelled charge as if it were the
hardware cost.** `CHECK(r.b.win_bytes == 2 * (256*8 + 512*16))` = 20 480 B is the
M6 *model*: 8 B/element and 16 B/line. M22 measured the realized unfused arrays
at 16 B/element + 32 B/line + 4 B/element permutation + 4 B/line order, i.e.
47 104 B at that shape (2.30×) and 6.33× at `W = L = 8K`. The assertion is
correct about the model and misleading about the hardware.

## 3. What M23B changed

Three tests were added; **no existing literal was edited**, because every one of
them is a published figure and re-anchoring it would be a silent claim about
history rather than a repair.

| new test | what it pins | why it closes a defect |
|---|---|---|
| `m23_archetype_config` | the 13 knobs the archetype depends on, including the two that default to **off** (`batch_window`, `page_sched`) and the one that is the **expensive** build (`win_fused = false`) | a default change now fails loudly instead of moving every number |
| `m23_archetype_baseline` | fused **and** unfused at 2×8K and 2×16K on the shipped acquisition path: cycles, realized arrays, DRAM reads, element/line counts, and fused ≡ unfused exactly | D1: the shipped path and the 16K point now have baselines |
| `m23_model_vs_realized` | the M6 charge (20 480 B) **and** the M22 realized arrays (47 104 B) at the same shape, and that the second is at least 2× the first | D3: neither number can silently become the other |

### The pinned numbers

| window | build | cycles | realized arrays | DRAM reads |
|---|---:|---:|---:|---:|
| 2×8K | unfused (A0) | 156 419 | 703 104 B | 17 446 |
| 2×8K | **fused (A1)** | **156 419** | **188 064 B** | 17 446 |
| 2×16K | unfused | 152 059 | 1 097 472 B | 17 447 |
| 2×16K | **fused** | **152 059** | **286 656 B** | 17 447 |

Both builds: 16 384 elements, 16 384 lines issued, 0 merges (a permutation has
nothing to coalesce), 0 address mismatches. The representation ratio is 3.74× at
8K and 3.83× at 16K — the M15 formula reproduced at scale.

## 4. What is still not baselined

| item | why it is left open |
|---|---|
| the irregular patterns | M23C generates them; they cannot have a baseline before they exist |
| `page_dir` / `dir_collisions` | still dead state (M22); a baseline of a counter nothing reads adds nothing |
| **absolute cycle counts under `win_fused = true` as a default** | there is no such default; M23A deliberately left it alone |
| area / power | not measurable without synthesis; M22's density assumption is the only conversion in the tree |
| the M17 anchor (220 123 / 138 924 / 125 581) | lives in the M17 report's command line, not in a test; M23B does not move it into the suite because it is a published cross-check, not a unit baseline |

## 5. Procedure for the next baseline change

1. Run the suite; note which assertion moved.
2. If it is an archetype number, the archetype changed — that is an M23A event,
   not a test edit.
3. If it is a historical figure, the model changed under a published number —
   which requires a report explaining why the old value is no longer reproducible,
   exactly as M8's audit did for the M7 figures.
4. Only then edit the literal, and edit the report reference in the same commit,
   so no number exists without the document that explains it.
