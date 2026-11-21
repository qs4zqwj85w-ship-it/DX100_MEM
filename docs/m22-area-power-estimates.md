# M22 — Area and Power Estimates

Bounded estimates only. There is **no synthesis, no SRAM compiler, no floorplan
and no power measurement** for this design, and none was performed for this audit.
Every number below is one of four things, always tagged:

| tag | meaning |
|---|---|
| **[C]** | **exact arithmetic** on measured or declared storage |
| **[E]** | **technology assumption** (SRAM bit density, flop size, energy per event) — a model input, not a measurement |
| **[D]** | **derived estimate** — arithmetic on [M] counts using an [E] assumption |
| **[?]** | **speculative** — would need RTL/synthesis; not claimed |

---

## 1. Exact storage (§ from `m22-storage-accounting.md`) [C]

| configuration | window arrays | fixed | total | = |
|---|---:|---:|---:|---:|
| U (unfused, shipped) 2×8K | 934 128 B | 25 600 B | **959 728 B** | 937 KiB |
| U 2×16K | 1 851 632 B | 25 600 B | **1 877 232 B** | 1.79 MiB |
| F (fused) 2×8K | 246 000 B | 33 792 B | **279 792 B** | 273 KiB |
| F 2×16K | 475 376 B | 33 792 B | **509 168 B** | 497 KiB |

These are the only *exact* figures in this document. Everything below multiplies
them by an assumed density.

## 2. Area model

**Assumption (E1), carried forward from the M14 audit unchanged** so the two
audits are comparable. 6T SRAM bitcell ≈ 0.021 µm² (5 nm), 0.027 µm² (7 nm),
0.05 µm² (14/16 nm); peripherals, sense amps and wiring add 40–100 % at these
array sizes. Net effective cost:

| point | effective µm²/bit |
|---|---:|
| low (5 nm-class) | 0.030 |
| **nominal (7 nm-class)** | **0.045** |
| high (16 nm-class) | 0.100 |

`mm² = bytes × 8 × µm²/bit / 10⁶`. No vendor data was used; the low/high spread is
3.3×, so **ratios are trustworthy and absolutes are not**.

### 2.1 Array area [D]

| configuration | bytes | low | **nominal** | high | × 1 MiB L2 |
|---|---:|---:|---:|---:|---:|
| U 2×8K | 959 728 | 0.230 | **0.345** | 0.768 | 0.92× |
| U 2×16K | 1 877 232 | 0.451 | **0.676** | 1.502 | 1.79× |
| F 2×8K | 279 792 | 0.067 | **0.101** | 0.224 | 0.27× |
| F 2×16K | 509 168 | 0.122 | **0.183** | 0.407 | 0.49× |

Reference points at the same assumption: a 1 MiB L2 = **0.377 mm²**, a 16 KiB L1 =
**0.0059 mm²**, the 16 KiB L1 the engine's requests already go through is
negligible against either window build.

The single most important number in this audit: **build F at 2×16K (497 KiB,
0.183 mm² nominal) is 5.3× smaller and 3.4× cheaper in area than build U at 2×8K
(937 KiB, 0.345 mm²)**, while M15 measured the two representations behaviourally
equivalent. The shipped configuration is the expensive one for no functional gain.

### 2.2 Flops / control [D]

The fixed structures are not SRAM and must not be costed as if they were.
`Pending` is 56 B × 128 = 7 168 B of state that is really registers:

| item | storage | DFF estimate | area at 0.3–0.6 µm²/DFF [E] |
|---|---:|---:|---:|
| `Pending` × 128 | 7 168 B | ≈ 57 k | **0.017–0.034 mm²** |
| index ring metadata | 5 396 B | ≈ 43 k (SRAM-able) | 0.003–0.006 mm² |
| free list + owner | 640 B | ≈ 5 k | 0.002–0.003 mm² |
| fanout queue ≤ R × 76 B | ≤ 9 728 B | SRAM | 0.001 mm² |
| counters/control | ≈ 2 KB | ≈ 16 k | 0.005–0.010 mm² |
| **control total** | | | **≈ 0.03–0.05 mm²** |

Request tracking is 3–10 % of build F's array area and 11–26 % of its *total*
area. It is not free, and it does not shrink with the window — it is the floor cost
of the engine existing.

### 2.3 Logic area [?]

**Not estimated.** No RTL exists, so any gate count would be invented. The
structural reading from `m22-logic-and-timing-audit.md` is that there is no CAM and
no comparator network in the design (dedup is folded into a streaming grouping
pass, the page directory is direct-mapped, response matching is direct-indexed).
That keeps the combinational logic small relative to the 273–497 KiB of SRAM, but
"small" is not a number and this audit does not give one.

## 3. Power and energy

### 3.1 What the engine changes in the memory system [M]

DRAM reads per run, arm A (SIMT) versus C (engine), M21 traces, 2×8K:

| workload | regime | A reads | C reads | Δ traffic | A row-hit | C row-hit |
|---|---|---:|---:|---:|---:|---:|
| w4_duplow | A (8 MiB) | 156 321 | 163 513 | **+4.6 %** | 0.1 % | 45.5 % |
| w8_wslarge | A (2 MiB) | 107 460 | 108 552 | +1.0 % | 6.2 % | 67.5 % |
| w1_uniform | B (128 KiB) | 27 165 | 30 231 | **+11.3 %** | 76.3 % | 84.1 % |
| w5_sequential | B (1 MiB) | 41 515 | 46 244 | **+11.4 %** | 78.6 % | 85.4 % |
| w3_duphigh | B (4 KiB) | 25 181 | 25 246 | +0.3 % | 86.8 % | 90.7 % |

Two facts that contradict the intuition the mechanism's name invites:

1. **The engine never reduces DRAM traffic; it increases it**, by 0.3–11 %, because
   its target requests bypass L1 and its window fetches whole lines regardless of
   whether the SIMT path would have hit.
2. **The benefit is row locality and translation, not traffic.** Regime A converts
   a 0.1 % row-hit stream into 45.5 %, which removes activations; regime B removes
   nothing in DRAM and only removes *requests*.

### 3.2 Bounded energy statement [D]

Using the M14 energy model (**E2**: one 64 B SRAM access = 1 E1; one 64 B DRAM read
including activate/precharge ≈ 100–200 E1), and the counts above:

| | regime A (w4) | regime B (w1) |
|---|---|---|
| DRAM activations removed | ≈ 67 000 (156 165 → 89 100) | 0 |
| DRAM lines added | +7 192 (+4.6 %) | +3 066 (+11.3 %) |
| on-chip accesses added | ≈ 4 per element × 131 072 ≈ 0.5 M E1 | same |
| **energy direction** | **improves** if an activation costs > ~8 E1, which it does | **worsens**: more DRAM lines *and* more on-chip traffic |

So: **in regime A the engine is plausibly energy-positive; in regime B it is
energy-negative on both axes while being 4–16 % faster.** That is a real
tradeoff, not a rounding error, and it is invisible in a cycles-only analysis.

The activation saving in DRAM *cycles* is also modest: 67 000 activations ×
(t_miss − t_hit = 12 cycles) ≈ 0.80 M cycles against a 9.92 M-cycle improvement on
w4. **92 % of the regime-A win is not DRAM time**; it is the translation and
request-order effect measured in `m22-logic-and-timing-audit.md` §5 (93.7 % dTLB
refusals).

### 3.3 Static power [?]

Scales with §2.1 area and is not separately estimated. Relative statement only:
build F 2×16K (497 KiB) retains on the order of half a 1 MiB L2; build U 2×16K
(1.79 MiB) retains on the order of two. No leakage number is given because none
would be defensible without a library.

## 4. What is and is not claimed

| claim | status |
|---|---|
| storage totals (937 KiB / 1.79 MiB / 273 KiB / 497 KiB) | **[C]** exact, from declared fields and the `sizeof` probe |
| area 0.101 / 0.183 / 0.345 / 0.676 mm² | **[D]** on assumption E1 (0.045 µm²/bit nominal); ±3.3× if the node differs |
| control ≈ 0.03–0.05 mm² | **[D]** on a flop-size assumption; order of magnitude only |
| logic area | **not claimed** |
| frequency | **not claimed**; 1 GHz appears nowhere in the conclusions |
| energy *direction* per regime | **[D]** on E2; the sign is robust, the magnitude is not |
| absolute power / mW / J | **not claimed** |
| leakage | **not claimed** |

Any statement of the form "the engine costs X mm² at Y GHz and Z mW" would be
fabricated from this evidence base. The audit's arithmetic stops at bytes, and its
one area conversion is a labelled density assumption inherited from M14.
