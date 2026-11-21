# Task 4 — Audit of `dx100-experiment-design-v2.md`

**Audit only.** No RTL, simulator, configuration, workload or design file was modified. The
Vortex reference tree at `DX100_MEM/ref/vortex` (`7300bc84981c34331b6247cc14500d8e07644988`) is
unmodified (`git status --porcelain` clean apart from `.DS_Store`). No experiment was run and no
performance number below is a measurement.

Conventions: **[V]** = verified by reading the cited source or by an exact calculation reproduced in
§3; **[A]** = assumption that the design depends on and does not defend; **[R]** = recommendation,
not a fact about the code.

---

## 1. Executive verdict

**READY AFTER CORRECTIONS.** Not "not ready": the architecture section (§2 of v2) is accurate —
every one of its thirteen chain-of-custody claims survived re-verification against the RTL, and the
headline structural finding (a virtual-addressed DXA behind a separate 8-entry unit MMU that shares
the cluster L2 TLB and the two cluster walkers) is correct. The experimental frame is also sound:
the three-way hypothesis split is real, the arms are separable, and the fairness rules are the right
ones.

But three defects would invalidate specific headline comparisons if implemented as written, and two
more mean that the single quantitative paragraph on which Hypothesis B's scoping rests (§6.3) is
mis-derived:

| Class | Count | Consequence if uncorrected |
|---|---|---|
| Invalidating a headline comparison | 2 (F1, F2) | "A refuted" and "B refuted" would both be uninterpretable |
| Wrong arithmetic (conclusion survives, derivation does not) | 3 (F3, F6, F9) | §6.3's justification is false; the numbers must be re-derived before they are quoted |
| Unrealizable experiment parameter | 2 (F6, F8) | Cells that the design says to run cannot be constructed |
| Instrumentation gap | 1 (F7) | Gates V-X2 and V8 cannot pass |
| Minor / cosmetic | 4 (F11–F14) | None on validity |

None of the required corrections needs new hardware or a simulator feature beyond instrumentation
(§8). All are edits to the design text plus, at most, one added arm and one added counter.

**The single most important finding** is F1: the "perfect-knowledge scheduling baseline" (A3), which
is the bar Hypothesis A must clear, silently grants unbounded *occupancy* in addition to unbounded
*visibility*. As specified, A3 is stronger than the engine on capacity, so `A5 ≤ A3` would be
uninformative — and `A5 ≤ A3` is the outcome v2 pre-registers as "A refuted".

---

## 2. Critical findings, ranked by severity

### F1 — [V] The oracle window (A3) adds unbounded capacity, not just visibility. **Severity: critical.**

v2 §5.2 states the enumeration rule correctly ("may not … hold a request beyond the capacity budget
of §8 R2") and then contradicts it in the same table: the A3 window is "unbounded (= all generated,
not yet issued)". A request that has been *formed* but not yet *accepted by memory* is outstanding
from the issuing warp's point of view: the warp cannot retire, and in Vortex the LSU cannot form the
next request, until an MSHR entry is available. A 256-entry (A2) or unbounded (A3) holding buffer
placed *before* MSHR allocation is therefore **additional in-flight capacity**, not additional
visibility.

Concretely, v2's budget is `L2_MSHR × L2_banks = 64 × 2 = 128` lines (v2 §6.1 R3). A3 holds up to
"all generated, not yet issued" — with 16 warps × 4 threads and a 16 K-element launch this is
bounded only by the kernel's unrolled depth, easily > 128. So A3's true in-flight capability is
`128 + window`, while A4/A5's is `128`. The table's row "Occupancy beyond budget: no" is an
*assertion of intent* that nothing in the design enforces.

**Impact on validity.** `A5 ≤ A3` would be explained equally well by "the engine gained nothing from
generating addresses early" and "the baseline was handed more capacity". Since v2 pre-registers
`A5 ≤ A3` as refutation of Hypothesis A, the primary conclusion of the study would rest on a
confounded comparison. This is the most consequential defect in the document.

**Required correction.** Restate the budget invariant as
`in_flight_to_memory + window_occupancy ≤ 128`, i.e. the window is *carved out of* the budget
(A3: window 96 + MSHR 32; A2: window 96 + MSHR 32; A1: no window, MSHR 128). Then add one sentence:
"the oracle's advantage is that it chooses *which* 96 of the generated requests occupy the budget,
not that it has more."

### F2 — [V] A5-versus-A3 does not isolate generation: it also changes the oracle's clairvoyance. **Severity: critical.**

v2 §5.2 asserts: "A5's ordering knowledge is *identical in kind* to A3's — only its provenance
differs." That is false as specified. A3 is **clairvoyant about MC bank/row state**; A5 orders "by
row-aware prediction from the address" (§5.3). These are different knowledge classes. A result
`A5 ≈ A3` is therefore ambiguous between "generation independence buys nothing" and "address-only
prediction costs exactly what generation gains".

**Impact on validity.** This is the second of the two comparisons that decides Hypothesis A, and it
has two independent variables.

**Required correction.** Add one arm, **A5\*** = engine with clairvoyant ordering *within its own
generated batch*. Then `A5* vs A3` differs in exactly one thing: who produced the addresses and
whether they existed before SIMT computed them. That is Hypothesis A, cleanly. A5 (predicted
ordering) stays as the realistic arm and is reported against A2 (predicted ordering), same
knowledge class.

### F3 — [V] The PTE-traffic figure is wrong by ≈14×, and the conclusion built on it does not follow. **Severity: high.**

v2 §6.3: "PTE traffic ≈ `10 000 × 3 × 64 B` ≈ **1.9 MB** for **1.05 MB** of data", concluding that
page-table traffic exceeds data traffic. Re-derived in §3 (C13): the correct *distinct* PTE traffic
for that workload is **≈136 KiB** (128 KiB of leaf lines + 8 KiB of interior tables) against
1 MiB of data — **13 %**, not 180 %. The 30 000 figure is a *fetch-request count*, not traffic, and
most of those requests hit in L1/L2 because the interior page tables are 8 KiB total.

The reason the true number is small is structural and worth stating in the design: with 4 KB pages
and 8 B PTEs, the page table is 1/512 the size of the data it maps, so PTE *bytes* can never exceed
data bytes for a fully-traversed region. They can only dominate when the access pattern touches many
pages with very few accesses each, i.e. exactly the sparse regime where the *walk count* rather than
PTE bytes is the problem.

**Impact on validity.** The paragraph that sets Hypothesis B's scope (§6.3) is derived from a false
premise. The *conclusion* ("at large P the workload is page-walk-dominated") is nevertheless correct
— but it must be established from walk **throughput**, not PTE bytes (C14, C15, C16 below:
1.58 M cycles of walks vs 4 096 cycles of data, ≈386×).

### F4 — [V] The useful `P` band's lower edge is 512, not 32. **Severity: high.**

v2 §6.3: "At `P ≤ 32` everything fits in a modest TLB and Hypothesis B is trivially nil… **the honest
window is `P ∈ [32, 8192]`**."

At `P = 32` the *L1* TLB does **not** fit (`VX_CFG_UTLB_SIZE = 8`, `VX_config.toml:212`; dTLB 16,
`:205`) — the correct reason a small `P` is cheap is that the **L2 TLB holds 512 entries**
(`VX_config.toml:219`) and absorbs every L1 miss without a walk. So the threshold set by *capacity*
is `P ≈ 512`, not 32. Everything in `[32, 512]` is a region where B's mechanisms have nothing to
recover except a 4-cycle L2 round trip on a single translation lane, which the existing 4-entry
VPN-dedup MSHR already amortizes for repeats. v2's band therefore includes 16× of range in which
the mechanism is *known* to be nil, and Stage 4 spends 5 arms × 2 workloads × 3 `P` × 2 `κ` × 3
seeds = 180 runs partly there.

**Required correction.** `P ∈ [512, 8192]`, i.e. 1–16× the L2 TLB; state the derivation
(L2 TLB entries ⇒ resident; 2 walkers ⇒ collapse) rather than citing "a modest TLB".

### F5 — [V] The duplicate-rate formula is wrong, and W1's text contradicts it. **Severity: high.**

v2 §6.3/W1: "draw from a pool of `M = N/(1+d)` distinct indices, so expected distinct fraction is
`1/(1+d)`". Correct value of the expectation for `N` draws with replacement from `M`:

```
E[distinct]/N = (1/(1+d)) · (1 − e^−(1+d))
```

giving 63.2 % / 43.2 % / 19.9 % distinct for `d = 0 / 1 / 4` — i.e. **36.8 % / 56.8 % / 80.1 %
duplicates** (exact, §3 C17). So v2's labels "0 / 50 % / 80 % of requests redundant" are wrong at
`d = 0` by 37 percentage points, and approximate at `d = 1`. Separately, W1's own text says "`B` is a
permutation of a chosen footprint", which *is* 0 % duplicates but is sampling **without**
replacement — the two definitions are mutually exclusive.

**Impact on validity.** (i) The `d = 0` arm does not contain the "no duplicates" control the dedup
ablation needs, so the ablation has no zero point. (ii) Gate V5 (`dups_eliminated == N − |unique(B[i])|`)
compares the model against an *analytic* prediction that the generator does not satisfy, so V5 would
fail on correct code.

**Required correction.** Two generators, named: `d = 0` ⇒ uniform random **permutation** (no
replacement, 0 duplicates, exact); `d > 0` ⇒ sampling with replacement from `M = N/(1+d)` with the
*measured* distinct count reported per run (never the closed form), because the design's dose-response
claim should be keyed to the realized duplicate rate, not the nominal one.

### F6 — [V] `P` and `f` are the same knob at fixed `N`, and the proposed `P` levels are unreachable. **Severity: high.**

With `N = 16 K` and 8 B elements on 4 KB pages, `P = f·N·8/4096 = 32·f` exactly. So:

| `f` | `\|A\|` | `P` |
|---|---|---|
| 8 | 1 MiB | **256** |
| 64 | 8 MiB | **2 048** |
| 512 | 64 MiB | **16 384** |

v2 §6.2 lists `f ∈ {8, 64, 512}` **and** `P ∈ {4, 32, 512, 8192, 16384}` as independent knobs and
says `P` is "derived from `f` and the page size, or forced independently by scattering the
footprint". Only three `P` values are reachable from `f` at all, and they are `{256, 2048, 16384}` —
**none of `{4, 32, 512, 8192}` is reachable**, and §6.2 never says how to force `P` independently.
Note also that F4 moves the interesting band to `[512, 8192]`, whose midpoint `f ≈ 16` is not in
v2's `f` grid either.

**Impact on validity.** Hypothesis B's independent variable cannot be set to the values the design
prescribes. Combined with F4, the entire `P` grid needs re-derivation.

**Required correction.** Collapse to one knob. Define `P` as primary and `f = P/32` (at `N = 16 K`,
8 B), or state explicitly that `P` is varied by *permuting which pages of a fixed-size region are
touched* (`f` fixed, pages sparse), which is a different experiment (it holds cache footprint while
varying TLB footprint) and is the *only* way to decouple them. Recommend the latter, with `P ∈
{256, 512, 2048, 8192}` and `f` fixed at 512, plus the `f` grid retained only for Hypothesis A.

### F7 — [V] No TLB counter is exported by SimX; `tlb_stall_cycles` does not exist in either model. **Severity: high (blocks gates).**

Counter *accessors* exist on the objects:
`Mmu::tlb_reads/hits/misses/evictions` (`sim/simx/mem/mmu.h:76-79`),
`L2Tlb::PerfStats{reads,hits,misses,evictions,mshr_dedups}` (`sim/simx/mem/tlb_l2.h:37-43`),
`Ptw::PerfStats{walks,walk_latency,wc_hits}` (`sim/simx/mem/ptw.h:33-37`).

But nothing consumes them. `Socket::perf_stats()` aggregates only `icache`, `dcache`, the DXA core,
and (conditionally) tex/rtu (`sim/simx/socket.cpp:451-465`) — **the unit MMU is absent**, and so are
the dMMU, iMMU, L2 TLB and PTW. A repository-wide grep for `tlb_reads|tlb_misses|mmu_perf|walk_latency|mshr_dedups`
across `processor.cpp`, `main.cpp`, `cluster.cpp`, `socket.cpp` returns **no hits**. The RTL comment
in `VX_socket.sv` likewise notes the socket MMU counters are "computed but not consumed".

So gate **V-X2** ("assert `unit_mmu_` shows non-zero `tlb_reads()` after a DXA gather") and gate
**V8** are not implementable as written, and §7's Hypothesis-B metric set (`tlb_*`, `walks_started`,
`walk_latency`, `walker_busy`, `tlb_stall_cycles`, `mshr_occ_tlb`) has no data source. `tlb_stall_cycles`
— the metric v2 designates as B's *primary* engagement signal and the acceptance test for V10 — does
not exist in either the RTL or SimX `Mmu`.

**Impact on validity.** The B branch is unmeasurable until counters are plumbed, and V10 (the gate
that prevents misattributing a cycle win) cannot run at all.

### F8 — [V] κ is defined at the wrong granularity to produce a coalescing contrast. **Severity: high.**

v2 §6.2: κ = "fraction of elements whose index is a compile-time affine function of the loop
counter". Coalescing in Vortex is a property of a *memory instruction*: the LSU forms one request per
warp per instruction and merges lanes that fall in the same line (`hw/rtl/libs/VX_mem_coalescer.sv`;
   `sim/simx/mem/mem_coalescer.cpp`). With `NUM_THREADS = 4` and `SIMD_WIDTH = 4` (`VX_config.toml:59-60,72`), a single load
instruction carries 4 element indices. If κ is applied *per element* inside that group, then at
κ = 0.5 essentially every instruction has ≥1 irregular lane and **no instruction coalesces** — so
the contrast between κ = 1 and κ = 0.5 is nearly the same as between κ = 1 and κ = 0, and κ carries
no dose-response information.

**Impact on validity.** κ is described as "the single most important control in the study" and is the
one knob that distinguishes a genuine indirect-access result from a scheduling artifact
(pre-registered: "effect vanishes as κ→1; effect present at κ=1 ⇒ bug"). At element granularity it
would not work, and the bug-check would fire spuriously.

**Required correction.** Define κ **per warp-instruction**: with probability κ, the whole 4-lane
group's indices are affine (`base + lane·stride`, a coalescible pattern); otherwise all 4 lanes are
indirect. Then κ is exactly the fraction of *requests* that are regular.

### F9 — [V] The walk-cache hit rate is computed against the wrong index space. **Severity: medium-high.**

v2 §6.3: "a **16-entry** walk cache whose hit chance on random VPNs is ≈ 0.1 %" (16/16384). The walk
cache is not indexed on the VPN; it caches the *last-level table* and its tag is
`vpn >> TLB_VPN_LEVEL_BITS` (`sim/simx/mem/ptw.cpp:127`), i.e. one entry per **2 MB** region, and the
RTL header describes exactly that ("skip the interior fetches when a spatially-adjacent walk already
resolved its last-level table", `VX_ptw.sv:17-19`). At `f = 512`, a 64 MiB footprint spans **32**
such regions — so the relevant space is 32 tags in a 16-entry direct-mapped cache, not 16 384.

The correct statement is that the walk cache covers roughly half the granules and, with 2:1 tag
aliasing, saves the two interior fetches on perhaps 25–40 % of walks — reducing the average from 3.0
to ≈2.4 dependent fetches. So "each walk ≥ 3 dependent PTE fetches" is an upper bound, and v2's
"0.1 %" is wrong by ~300×.

**Impact on validity.** Feeds directly into F3's traffic figure and the walk-latency assumption.

### F10 — [V] A4/A5 do not traverse the L1 dcache; A0–A3 do. **Severity: medium-high.**

The DXA gmem port is wired into the socket *memory arbiter*, which is the L2-facing arbiter that the
icache and dcache feed as peers (`VX_socket.sv:960` for the DXA input, `:950-951` for
icache/dcache). The unit MMU output therefore lands **below** both L1s; the DXA never probes an L1.
Confirmed on the SimX side by the same topology (`sim/simx/socket.cpp:266-290`, `bind_unit_port`
binding the DXA to `sock_arb`).

Consequences v2 does not state:
- Any footprint that fits the 16 KB L1 dcache (`VX_config.toml:272`) is systematically favourable to
  A0–A3 by construction. v2's `f = 8` level is exactly that (|A| = 1 MiB is L2-resident, but the
  *index array* `B` and hot rows are L1-resident).
- §7's `hit_L1` metric is specified "demand and engine requests separately"; for engine requests it
  is identically zero/undefined, which will read as a bug.

**Required correction.** Either restrict Hypothesis A to `f ≥ 64` and say why, or add an explicit
"engine has no L1" note to §6.1 R2 with the bias direction stated (favours baseline), and drop
`hit_L1` for engine requests.

### F11 — [V] B1 and B4 scale only the L1 TLB, so they are not fair capacity controls. **Severity: medium.**

B1 (`S_vm+big`) raises the per-core DTLB to 256; B4 raises the UTLB to 256. But the **L2 TLB is 512
entries for the whole cluster** (`VX_config.toml:219`) with 8 MSHRs, and only 2 walkers. Once the
footprint exceeds 512 pages — which is the entire interesting band after F4 — the L2 TLB is the
binding lookup resource and enlarging an L1 TLB cannot reduce walks. So B1 will show "bigger TLB does
not help" for a reason unrelated to the hypothesis, and the `B3 > B1` conclusion ("B survives the
just-buy-entries control") would be too easy.

**Required correction.** Add `B1′` = L2 TLB scaled alongside the DTLB (or state that B1 alone cannot
falsify the capacity explanation and that the capacity control is `B1′`).

### F12 — [V] `d` and `P` are coupled: raising the duplicate rate lowers the distinct page count. **Severity: medium.**

`P` is defined as pages *touched*; `d` reduces the number of distinct targets, hence the number of
distinct pages. v2 varies `d` in the dedup ablation (Stage 4 is A-only, but W1 is shared) and `P` in
B, with no statement that the two must be orthogonalized. A dedup win at high `d` is partly a
*translation* win (fewer distinct pages ⇒ fewer walks), which would be misattributed.

**Required correction.** Define `P` as **distinct pages touched**, measured per run and reported;
when varying `d`, hold the *distinct-target set* fixed (duplicate by re-drawing from the same realized
distinct set) so `P` is invariant.

### F13 — [V→R] SimX omits the two levels of TLB-bus arbitration latency that the RTL has. **Severity: low.**

RTL registers both TLB arbiters: `OUT_BUF(3)` at the socket level over 3 MMUs
(`VX_socket.sv:420-430`, `SOCKET_NUM_MMUS = 3` at `VX_gpu_pkg.sv:2360`) and `OUT_BUF(3)` at the
cluster level (`VX_cluster.sv:366-376`). SimX instead gives each L1 MMU its own channel into the
`L2Tlb` and serves **one lookup per tick round-robin** (`sim/simx/mem/tlb_l2.cpp:238`,
`rr_client_`). Throughput contention is therefore modelled, but ~2–6 cycles of arbitration latency
per TLB transaction are not. This does not affect any qualitative conclusion; it makes absolute walk
latency optimistic.

### F14 — [V] Unit and rounding errors. **Severity: low.**

`16384(1−e⁻¹)` is **10 356.7** (v2: "≈ 10 360"); "64 MB" is actually 64 MiB = 67.1 MB (§3 C6);
"1.05 MB" is correct as 1 MiB expressed in decimal MB (§3 C8) and is *not* an inconsistency —
see §3.1.

---

## 3. Detailed arithmetic verification

### 3.1 The apparent 8 B / 64 B inconsistency (the specific question asked)

**Verdict: not an arithmetic error, but an undefended and unlabelled assumption.** The two figures
measure different quantities:

| Figure in v2 | Quantity | Equation | Inputs | Result | Exact? |
|---|---|---|---|---|---|
| "512 elements per 4 KB page" | element addresses per page | `4096 / w` | `w = 8 B` element width | **512** | exact *given* `w = 8`; `w = 4` ⇒ 1024 |
| "16 K elements ≈ 1.05 MB" | **cache-line** traffic, not element bytes | `N × 64 B = 16384 × 64` | 64 B line (`VX_config.toml:78`) | **1 048 576 B = 1 MiB = 1.05 MB** | exact |

They coincide only because `w = 8`. The document never states the element width anywhere — not in
§5.1, not in §6, not in the workload definitions — so the reader is forced to infer it twice, from
two different quantities. **The width is a real experimental input and it changes results
partition-wise**: at `w = 4` the elements per page double (1024), the tile is 16 pages not 32, and
`|A|` for `f = 512` is 8192 pages not 16384, which moves every threshold in F4 by 2×.

**Required fix.** Declare `w` as a §6.2 knob with default 8 B (double-precision accumulator, as in
W3's SpMV), and re-express every page count as a function of `w`.

### 3.2 Table of every v2 calculation

Legend: **E** exact, **A** approximate (closed-form expectation), **D** distribution-dependent,
**X** wrong.

| # | Quantity | Equation | Inputs | v2 value | Audited value | Kind |
|---|---|---|---|---|---|---|
| C1 | Bytes per element | — | not stated | (8 B implied) | 8 B declared | **input** |
| C2 | Elements per 4 KB page | `4096/w` | `w=8` | 512 | **512** | E |
| C3 | Elements per 64 B line | `64/w` | `w=8` | — | **8** | E |
| C4 | Tile bytes | `N·w` | `N=16384` | 128 KB | **131 072 B = 128 KiB** | E |
| C5 | Pages per tile | `N·w/4096` | — | 32 | **32** | E |
| C6 | `\|A\|` at `f=512` | `f·N·w` | `f=512` | "8.39 M words = 64 MB" | **8 388 608 el = 67 108 864 B = 64 MiB** | E (unit: MiB) |
| C7 | Pages of `A` | `f·N·w/4096` | — | 16 384 | **16 384** | E |
| C8 | Data traffic | `N × 64 B` | `N=16384` | 1.05 MB | **1 048 576 B = 1 MiB** | E |
| C9 | Distinct pages touched | `P₀(1−e^{−N/P₀})` | `P₀=16384`, `N=16384` | ≈10 360 | **10 356.7 ≈ 10 357** | A |
| C10 | UTLB (8) hit probability | `8/P₀` | — | 0.05 % | **0.0488 %** | E |
| C11 | L2 TLB (512) coverage | `512/P₀` | — | 3 % | **3.125 %** | E |
| C12 | Walk-cache hit chance | `16/16384` | wrong index space | 0.1 % | **see F9: ≈25–40 %** | **X** |
| C13 | PTE traffic | `walks × fetches × 64 B` | 3 fetches assumed | ≈1.9 MB | **≈136 KiB** (128 KiB leaf + 8 KiB interior) | **X** |
| C14 | Walk count per tile | `P₀(1−e^{−N/P₀})` | L2 3 % hit | ≈10 000 | **≈10 050** | A |
| C15 | Walk throughput | `walkers / walk_latency` | 2, ~305 cy | not stated | **0.00656 walks/cy = 152.5 cy/walk** | A (latency assumed) |
| C16 | Walk time per tile | `walks × 152.5` | C14, C15 | "order 10⁶" | **1.53×10⁶ cy** | A |
| C17 | Data time per tile | `1 MiB / 256 B/cy` | 102.4 GB/s, 400 MHz | "~4×10³" | **4 096 cy** | E |
| C18 | Ratio | `C16/C17` | — | not stated | **≈374×** | A |
| C19 | Distinct fraction at `d` | `(1/(1+d))(1−e^{−(1+d)})` | — | `1/(1+d)` | **0.632 / 0.432 / 0.199** | **X** |
| C20 | `P` from `f` | `f·N·w/4096 = 32f` | `N=16384,w=8` | independent | **{256, 2048, 16384}** only | **X** |
| C21 | Peak bandwidth | `ch × 64 B × 2 × 400 MHz` | 2 ch (`:92`) | 102.4 GB/s | **102.4 GB/s = 256 B/cy** | E |
| C22 | Engine in-flight vs budget | `L2_MSHR × banks` | 64 × 2 | 128 | **128** | E |

### 3.3 C13 in full — why the 1.9 MB figure is wrong

Workload: `N = 16 K` uniform draws over `|A| = 64 MiB`, `w = 8 B`, 4 KB pages, 3-level Sv39.

1. **Fetch count.** A walk that misses the walk cache performs 3 dependent fetches (root, mid, leaf).
   With the C12 correction (hit chance ~30 %) the average is `3 − 2×0.30 ≈ 2.4`, so
   `10 050 × 2.4 ≈ 24 000` fetch *requests*. v2's 30 000 is the h = 0 bound.
2. **Distinct leaf lines.** One 64 B leaf-PTE line holds 8 PTEs × 4 KB = 32 KB of VA; a 4 KB leaf
   page covers 512 × 4 KB = 2 MB. The 64 MiB footprint therefore has `2048` leaf-PTE lines total, of
   which the expected touched count is `2048(1−(1−8/16384)^16384) ≈ 2047`. **≈128 KiB.**
3. **Interior tables.** One mid page (covers 512 × 2 MB = 1 GB) + one root page = **8 KiB**, all
   resident in L1/L2 after the first walk of the launch.
4. **Total distinct PTE bytes ≈ 136 KiB vs 1 MiB of data = 13 %.**

The general identity behind this: for a densely-traversed region, page-table bytes scale as
`data × (8 / 4096 × levels_touched)` ≈ `data / 512`. PTE *bytes* can only exceed data bytes when the
pattern is sparse relative to page size — i.e. when `N ≪ P`, which is not this configuration.

**What actually dominates, correctly derived.** Not bytes but *serialization*: 10 050 walks through
2 walkers at ~305 cycles each = 1.53×10⁶ cycles, versus 4 096 cycles of data transfer
(C16 ÷ C17 ≈ 374×). **This** is the defensible basis for "translation-dominated at large `P`", and it
is assumption-dependent on the ~305-cycle walk latency, which itself follows from the chosen
`VX_CFG_PLATFORM_MEMORY_LATENCY` (v2 sets 20/60/150). The design must present C15–C18, not C13.

### 3.4 C15 and the actual binding constraint (Audit 2, second half)

Both candidates were checked, because v2 claims the walkers bind:

| Resource | Count | Service time | Throughput | Cycles per request |
|---|---|---|---|---|
| UTLB miss station | 4 entries (`VX_CFG_L1_TLB_MSHR_SIZE`) | ≈ walk latency | `4/305 = 0.0131/cy` | 76 |
| Cluster walkers | 2 (`VX_CFG_PTW_NUM_WALKERS`) | ≈ walk latency | `2/305 = 0.00656/cy` | 152.5 |

**Walkers bind** (0.00656 < 0.0131), so v2's claim is correct **[V]** — but only once
`P > L2_TLB_SIZE`, because below that the L2 TLB answers without a walk.

And in the "no walks" regime the single translation lane binds instead: a hit occupies the lane for 1
cycle, a parked miss for 2 (park + replay), so at `P = 32` with 8 UTLB entries the miss rate is
`1 − 8/32 = 75 %` and the ummu sustains `1/(1 + 0.75) ≈ 0.57 requests/cycle`. That is the honest
statement of what `P = 32` costs the engine — 1.75 cycles per request, not "nil". It is small
against a 60-cycle memory latency, which is why B is *effectively* nil there, but the design should
say so rather than assert "everything fits".

---

## 4. RTL versus SimX translation fidelity

Method: for each mechanism the RTL path was traced from `VX_socket.sv` / `VX_mmu.sv` / `VX_tlb_*` /
`VX_ptw.sv`, and the SimX path from `sim/simx/socket.cpp` / `mem/mmu.cpp` / `mem/mmu_tlb.cpp` /
`mem/tlb_l2.cpp` / `mem/ptw.cpp` / `cluster.cpp`. A mechanism was only called "absent" after
tracing the full path, not from a single file's silence.

| Mechanism | RTL behavior | SimX behavior | Equivalent? | Evidence | Consequence for experiment |
|---|---|---|---|---|---|
| **Distinct DXA translation client** | `ummu` is a third `VX_mmu` instance; DXA wired into `unit_mem_bus_if[UMMU_DXA_IDX]` | `unit_mmu_ = Mmu::Create(...)`, DXA bound via `bind_unit_port(kDxaArbIdx, UMMU_DXA_IDX, …)` | **Yes** | `VX_socket.sv:855-880`; `sim/simx/socket.cpp:220-222, 290` | B is measurable at all |
| **UTLB capacity & associativity** | `TLB_SIZE = VX_CFG_UTLB_SIZE` (8), fully associative, MRU | `tlb_(tlb_size)` = 8, linear-scan fully-associative, MRU victim | **Yes** | `VX_socket.sv:893`; `VX_tlb_cam.sv:16-22,53-57`; `sim/simx/mem/mmu.cpp:39`; `mem/mmu_tlb.cpp:23-58` | Thresholds keyed to 8 entries are faithful |
| **Superpage levels** | CAM masks `level×TLB_LEVEL_BITS` VPN bits | `shift = e.level * VPN_LEVEL_BITS`, low bits spliced | **Yes** | `VX_tlb_cam.sv:18-20`; `mem/mmu_tlb.cpp:31-42` | 2 MB diagnostic is faithful |
| **4 TLB MSHRs** | `MSHR_SIZE = VX_CFG_L1_TLB_MSHR_SIZE` (4) | `mshr_(VX_CFG_L1_TLB_MSHR_SIZE)` = 4 | **Yes** | `VX_config.toml:207`; `VX_mmu.sv:28`; `mem/mmu.cpp:41` | 76-cycle/request bound is faithful |
| **VPN-dedup miss station** | MSHR matches on VPN; a repeat joins the entry | `mshr_find(vpn)` then append to `parked` | **Yes** | `VX_tlb_mshr.sv:21,44`; `mem/mmu.cpp:134-152` | Dedup claims are testable |
| **Per-entry parked depth** | `REPLAY_DEPTH = 2` reap queue | `MAX_PARKED_PER_ENTRY = 2` | **Yes** | `VX_tlb_l1.sv:28,143`; `mem/mmu.h:147` | Faithful |
| **Request lanes** | `NUM_REQS = UMMU_NUM_REQS` = 1 port for the ummu; dMMU gets `SOCKET_SIZE × DCACHE_NUM_REQS` | `num_ports_` from `UMMU_NUM_REQS`; dMMU from the same product | **Yes** | `VX_gpu_pkg.sv:2287,2073`; `VX_socket.sv:344`; `sim/simx/socket.cpp:221` | The 1-lane vs ≥S-lane asymmetry (F10-adjacent) is real in both |
| **One park per cycle, global** | priority-encoder grants one park/cycle across lanes | single `stage_` optional, `stage_free` gating | **Yes** (for 1 port, identical) | `VX_mmu.sv:304-320`; `mem/mmu.cpp:393-395, 465-470` | Faithful |
| **Same-page hold while staged** | `stage_hold[l] = stage_valid_r && (vpn == stage_vpn)` | `hold_vpn` comparison, `continue` | **Yes** | `VX_mmu.sv:408`; `mem/mmu.cpp:427-428` | Faithful |
| **Hit joins live MSHR (ordering)** | `mshr_order` forces park on a hit to a page with a live MSHR | `ordered = !exec_side && (park_reg || port_parked)`; parks if `mshr_find(vpn) >= 0` | **Yes** | `VX_mmu.sv:296`; `mem/mmu.cpp:436-443` | Faithful |
| **Hit-under-miss** | a parked page does not block other lanes/pages | per-port replay queues drain ahead of new input | **Yes** | `VX_mmu.sv:17-21`; `mem/mmu.cpp:402-406` | Faithful |
| **Read-only client** | `WRITE_ENABLE = 0`; parked fields drop data/byteen | `write_enable = false`; `PFIELDS_W` drops them | **Yes** | `VX_socket.sv:895`; `sim/simx/socket.cpp:222`; `mem/mmu.cpp:32` | Faithful |
| **Registered park + rsp stage** | `PARK_REG = 1`, `RSP_OUT_BUF = 2` | `park_reg = true`, `rsp_reg = true` (`rsp_delay = 1`) | **Yes** | `VX_socket.sv:897-898`; `sim/simx/socket.cpp:222`; `mem/mmu.cpp:292` | Faithful |
| **Shared L2 TLB geometry** | 512 entries, 4-way, 8 mega, 8 MSHR, 4-cycle | `num_sets_ = 512/4`, `mega_(8)`, `mshr_(8)`, `pipe_` with `VX_CFG_L2_TLB_LATENCY` | **Yes** | `VX_config.toml:219-223`; `mem/tlb_l2.cpp:24-27,238` | Faithful |
| **L2 MSHR VPN dedup** | MSHR dedups, answers every attached requester | `mshr_dedups` counter, `requesters` vector | **Yes** | `VX_tlb_l2.sv:20,34`; `mem/tlb_l2.h:37-43` | Faithful |
| **Walkers** | `VX_CFG_PTW_NUM_WALKERS` = 2, cluster-shared, device-level instance | `walkers_(VX_CFG_PTW_NUM_WALKERS)` = 2, one device `Ptw` behind `PtwMux` | **Yes** | `VX_ptw.sv:24`; `Vortex.sv:356`; `mem/ptw.cpp:25`; `mem/ptw.h:117-148` | The binding constraint is modelled |
| **Walk cache** | 16-entry, direct-mapped, keyed `vpn >> TLB_LEVEL_BITS`, interior fetch only | `walk_cache_(16)`, `tag % size`, filled at `level == 0` | **Yes** | `VX_ptw.sv:17-19,66`; `mem/ptw.cpp:127-133` | Faithful; invalidates v2's "0.1 %" (F9) |
| **PTE fetch traffic** | real `VX_mem_bus_if` to the cache, tagged per walker | real `MemReq` on `MemReqOut` into the cluster cache | **Yes** | `VX_ptw.sv:27-35`; `mem/ptw.h:44-48` | PTE traffic is chargeable — but see F3 |
| **TLB-bus arbitration latency** | 2 registered levels, `OUT_BUF(3)` each, 3 MMUs → 1 cluster bus | one channel per client into `L2Tlb`, 1 lookup/tick round-robin; **no arbitration delay** | **Partial** | `VX_socket.sv:420-430`; `VX_cluster.sv:366-376`; `mem/tlb_l2.cpp:238` | Absolute walk latency optimistic by ~2–6 cy; qualitative results unaffected (F13) |
| **Per-L2-MSHR requester cap** | MSHR stores requesting client id (`ID_WIDTH = 6`); header says "answer every attached requester"; no explicit cap found | `MAX_REQUESTERS_PER_ENTRY = 4` | **Non-issue here** | `VX_tlb_l2.sv:29,34`; `mem/tlb_l2.h:62`; `tlb_l2.cpp:203` | Cap 4 vs ≤3 clients/socket (dMMU+immu+ummu) ⇒ never binds at 1 socket |
| **VAs preserved to the TLB** | DXA emits `VX_CFG_MEM_ADDR_WIDTH` VA, line-granular | `desc.base_addr` (64-bit) → `MemReq.addr` → `Mmu` translates before `ReqOut` | **Yes** | `VX_dxa_setup.sv:512-513`; `sim/simx/dxa/dxa_core.cpp:177-178,473` | Faithful |
| **Bypass conditions** | `~vm_active` or FLUSH/IO/OM attr ⇒ `req_bypass` | `needs_translation()` false when no SATP or BARE; also io/flush flags | **Equivalent in effect** | `VX_mmu.sv:187-190`; `mem/mmu.cpp:100-115` | Build N (VM off) is a true bypass in both |
| **Identity-mapped TLB entries** | not modelled separately (BARE covers it) | runtime installs identity PTEs at boot "for every PA-addressed region" | **Divergence in kind** | `mem/mmu.cpp:100-108` | A PA-region access appears as a *translation* in SimX; can inflate `tlb_reads` on kernel/descriptor traffic |
| **Counters: TLB** | `mmu_perf` ports exist; socket comment says "computed but not consumed" | accessors exist (`mmu.h:76-79`, `tlb_l2.h:37-43`, `ptw.h:33-37`); **nothing consumes them**; `Socket::perf_stats()` omits all MMUs | **No — absent output path** | `sim/simx/socket.cpp:451-465`; grep for consumers returns none | **Blocks V-X2, V8, V10 and all §7 B-metrics (F7)** |
| **Counter: `tlb_stall_cycles`** | n/a | n/a | **Absent in both** | — | B's designated primary signal has no source |

**Net assessment.** SimX's translation model is *much* closer to the RTL than v2 assumed — 21 of 24
mechanisms are equivalent, including all the ones that matter for the arithmetic (4 MSHRs, VPN dedup,
8-entry fully-associative UTLB, 512/4-way/8-mega L2 TLB with 8 MSHRs, 2 walkers, walk cache, real
PTE traffic). The two real gaps are **counter plumbing** (F7, blocking) and **arbitration latency**
(F13, cosmetic). v2's §2.4 uncertainties U1–U3 are therefore *narrower* than stated: U1 (miss-dedup
scope) and U3 (walker concurrency) are verified equivalent by inspection; only the counter gap
remains.

---

## 5. Configuration and fairness review

### 5.1 Per-comparison causal interpretability

| Comparison | Intended single variable | Actually differs by | Interpretable? |
|---|---|---|---|
| A0 → A1 | in-flight capacity | capacity only | **Yes** |
| A1 → A2 | + realistic window (visibility) | capacity **and** visibility (F1) | **No** until F1 is fixed |
| A1 → A3 | + oracle window | capacity (unbounded) + visibility + clairvoyance | **No** until F1 |
| A2 → A3 | realism of window | clairvoyance only (both predicted? no — A3 clairvoyant, A2 predicted) | **Partly**; state it as "clairvoyance + unbounded" |
| A1 → A4 | who generates | generation **and** L1 bypass (F10) | **No** for `f = 8`; **yes** for `f ≥ 64` |
| **A3 → A5** | **generation independence** | generation **and** prediction-vs-clairvoyance (F2) **and** L1 bypass | **No** — needs A5* |
| A4 → A5 | + ordering within the batch | ordering only | **Yes** |
| B0 → B2 | SIMT vs engine under VM | generation + L1 bypass + translation client (8 vs 16/core) | Interpretable as "engine vs SIMT", **not** as B |
| **B2 → B3** | **bulk translation** | translation only | **Yes** — this is the cleanest pair in the document |
| B0 → B1 | "just buy TLB entries" | L1 DTLB only; L2 TLB unchanged (F11) | **No** — under-tests capacity |
| B3 → B4 | capacity substitution | UTLB size only; L2 TLB unchanged (F11) | **Partly** |
| B0 → B5 | conventional ceiling under VM | DTLB + oracle window (F1) | **No** until F1 |
| C1 (=A5 in V) → C0 | + bulk translation | both mechanisms | Yes, as a 2×2 cell |
| B2 → C0 | both | both | Yes |

**The good news.** The C hypothesis has a proper 2×2 factorial already: `B2` (neither), `C1`
(A only), `B3` (B only), `C0` (both). Additivity is therefore identifiable, and v2's three-way
interpretation (sub-additive / additive / super-additive) is well-posed. **No change needed to the
C logic** beyond inheriting the F1/F2 fixes.

**The bad news.** Both of Hypothesis A's decisive comparisons (A2/A3 against A1, and A5 against A3)
are currently confounded, and the two cleanest pairs in the whole design are A0→A1 (capacity) and
B2→B3 (bulk translation). That is a striking asymmetry: the design is strongest exactly where it
claims the least.

### 5.2 VM-build discipline

v2 §5.1 says "Nothing is compared across builds" **[V] — correct and sufficient.** Checked for
leakage:
- A0–A3 are build N only; B0–B5, C0 and the "A5(V)" cell are build V only.
- The only cross-build pair used is A5(N) vs A5(V) = C1, which is introduced in §12 Stage 5 solely
  as the C addend and is correctly kept inside build V. **No confound.**
- The cost of enabling VM is *not* separately measured, but it is recoverable as A0 (N) vs B0 (V) for
  the SIMT arm. **Recommend** stating that explicitly so a reader can audit it.
- One gap: `SOCKET_UNIT_MMU_ENABLE` requires `VM_ENABLE && (TEX || RTU || DXA)`
  (`VX_define.vh:91-101`), so build V is a *three-way* flag combination that v2's U4 flags as possibly
  not building. That remains an open risk, not a fairness issue.

### 5.3 Capacity, bandwidth, latency, cache

All controlled by §6.1 R1–R6 **[V]** with the exceptions already listed (F1 window, F10 L1 bypass,
F11 L2 TLB). Two further points:

- **The engine's 128 KB tile buffer is on-chip storage the baseline does not have.** This is inherent
  to Hypothesis A (it *is* the mechanism) and v2 counts it in §9.1.8, but it is worth stating in §8
  R2 that the baseline's counterfactual is not "the same SRAM" but "nothing", because no SIMT
  configuration can hold 16 K indices. Otherwise a reviewer will call it an unfair advantage.
- **`result_delivery` and the LMEM drain (1 word/cycle)** are charged to the engine **[V]** and are
  genuinely absent from the baseline, which is a real cost. Good.

### 5.4 The "perfect-knowledge baseline must not see ungenerated addresses" requirement

v2's rule is textually correct and, for A2, checkable by gate V6. For A3 the gate is stated as "at
every cycle, the A3 window's request set ⊆ the SIMT-generated set" **[V] — this is the right check**
and it is implementable. The defect is not the rule but the *capacity* consequence (F1). One
additional check is needed: **the window must not be able to hold a request whose warp has since been
scheduled out or blocked**; otherwise the window is a prefetch buffer for a stalled warp, which is
still legal under the enumeration rule but changes the meaning. Recommend V6 be strengthened to
"⊆ generated ∧ the set is bounded by 128 total".

---

## 6. Workload validity review

### 6.1 Does the matrix distinguish the six required access classes?

| Required class | Distinguishing knob | Verdict |
|---|---|---|
| 1. Ordinary coalesced SIMT | `κ = 1`, W0 | **Yes** (W0 is exact; κ needs F8 fix) |
| 2. Irregular, many independent requests | `κ = 0`, `f = 512`, W1 | **Yes** |
| 3. Duplicate-heavy indirect | `d` | **Yes, once F5 fixed** (currently mislabelled) |
| 4. Page-diverse indirect | `P` | **Only after F6** — the stated `P` grid is unreachable |
| 5. Translation-limited | `P ≥ 512` | **Yes, once F4 moves the band** |
| 6. Locality-rich (caches/TLB work) | `f ≤ 8`, `P ≤ 256` | **Partially** — `f = 8` is L1-resident and therefore hits F10 |

### 6.2 Missing controls and hazards

1. **Number of independent index streams.** Audit item not present in v2. Two concurrent gathers from
   two different `B` arrays (`out[j] += A1[B1[i]] + A2[B2[i]]`) double the translation pressure per
   instruction without changing `P`. This is the single cheapest way to separate "translation
   throughput" from "address-generation independence" and it is *missing*. **Recommend adding one
   level: 1 vs 4 interleaved streams on W1.**

2. **The line-versus-element over-fetch is invisible to the metrics (new finding).** v2 §7 defines
   `useful_bytes = 64 × (distinct lines)`, which is *derived from* the same line count as
   `transferred_bytes`. So `efficiency` is 100 % by construction and can only ever measure duplicate
   *line* suppression — it cannot see that a random 8 B gather uses 8 B of each 64 B line, i.e. that
   12.5 % element-level efficiency is the dominant inefficiency in W1 and is **not** fixable by any
   arm in this study. A reviewer will read `efficiency = 100 %` as "no waste". **Recommend** renaming
   it `line_efficiency` and adding `elem_efficiency = 8 × N / transferred_bytes`.

3. **`P` is a *measured* outcome, not a set-point (F6/F12).** The design should report the realized
   `distinct_vpn_per_launch` for every cell and treat the nominal `P` as a target to be verified,
   not assumed.

4. **W2 is unfaithful to the engine arm (already flagged by v2 as U2).** v2 is honest about this; the
   audit confirms it should stay flagged, and recommends W2 be SIMT-only unless the engine's range
   form is modelled, because a modelled extension that the RTL cannot express weakens every W2
   conclusion.

5. **W0's role as the ceiling is weak for the engine (F10).** The engine cannot hit an L1, so for
   contiguous data it must be ≥ the SIMT arm; `V2` ("no arm beats W0") is then satisfied trivially
   and the gate proves nothing about the engine. It remains a valid gate for A0–A3.

6. **The `d` generator's interaction with `ρ` (row locality) is unstated.** Reordering `B` to sort a
   fraction ρ of consecutive pairs into the same row (§6.3 W1's ρ) changes the duplicate structure if
   implemented by swapping, so ρ and d are also coupled. **Recommend** the generator report realized
   (d, ρ, P) per run, and that the analytic values be checked against them (this is what V5 is trying
   to do, but F5 breaks it).

---

## 7. Exact corrections required in v2

Ordered by whether they block implementation. Section references are to v2.

| # | v2 location | Required change | Blocking? |
|---|---|---|---|
| 1 | §5.2 table + §6.1 R3 | Replace "unbounded" window and the "Occupancy beyond budget: no" assertion with the invariant `in_flight + window ≤ 128`; give explicit window/MSHR splits per arm | **Yes** |
| 2 | §5.3, §5.2 | Add **A5\*** (engine + clairvoyant ordering within its own batch) and make `A5* vs A3` the primary Hypothesis-A comparison; retitle `A5 vs A2` as the realistic one | **Yes** |
| 3 | §6.2 | Declare element width `w` (default 8 B) as an input; state `τ = 4096/w` elements per page | **Yes** |
| 4 | §6.2 | Collapse `f`/`P` into one knob (`P = 32f` at `N=16K, w=8`); if both are wanted, specify the page-sparsification method and hold `f` fixed | **Yes** |
| 5 | §6.2, §4/W1 | κ defined **per warp-instruction** (whole 4-lane group affine or not) | **Yes** |
| 6 | §4/W1 + §6.2 | `d = 0` ⇒ permutation (without replacement); `d > 0` ⇒ replacement from `M = N/(1+d)`, with realized duplicate rate measured, not assumed; correct the closed form to `(1/(1+d))(1−e^{−(1+d)})` in any text that quotes it | **Yes** |
| 7 | §6.3 | Replace the "PTE traffic 1.9 MB" bullet with the C13–C18 derivation: ≈136 KiB distinct PTE bytes (13 %), ≈10 050 walks, 152.5 cy/walk, 1.53×10⁶ cy vs 4 096 cy, ≈374×. State that the walk latency is an assumption | **Yes** |
| 8 | §6.3 | Change the band to `P ∈ [512, 8192]` and derive both edges from capacity (`VX_CFG_L2_TLB_SIZE = 512`) and throughput (`2 / walk_latency`), not from "a modest TLB" | **Yes** |
| 9 | §7, §11 | Add the counter-plumbing requirement and a `tlb_stall_cycles` definition (§8 below); restate V-X2/V8/V10 in terms of the deliverables | **Yes** |
| 10 | §6.1 R2, §5.3 | State that the engine bypasses the L1 dcache; restrict Hypothesis A to `f ≥ 64` or mark `f = 8` as baseline-favouring; drop `hit_L1` for engine requests | No |
| 11 | §5.3 | Add `B1′` scaling the L2 TLB alongside the DTLB, or state that `B1` alone cannot falsify the capacity explanation | No |
| 12 | §5.3, §6.2 | Add the index-stream-count knob (1 vs 4) | No |
| 13 | §7 | Rename `efficiency` → `line_efficiency`; add `elem_efficiency = 8N / transferred_bytes` | No |
| 14 | §6.3 | Fix rounding/units: 10 357 not 10 360; 64 MiB not 64 MB | No |
| 15 | §2.4 | Narrow U1/U3: miss-dedup scope and walker concurrency are verified equivalent; only arbitration latency (F13) and counter export (F7) differ | No |
| 16 | §12.1 | Re-derive Stage 4's run count after the `P` band and grid change; Stage 0 must include the counter check | No |
| 17 | §12.1 | Add the "VM cost is A0(N) vs B0(V)" note | No |

---

## 8. Minimum additional simulator work before the experiment

All items are instrumentation or model-configuration; **none** changes behaviour. No RTL change is
required, and no RTL change is recommended (the design's whole point is to evaluate the existing
pipeline).

| # | Work | Why | Where | Size |
|---|---|---|---|---|
| **W1** | Add `tlb_stall_cycles` to `Mmu`: count cycles in which a port's request is present but refused **because** `stage_` is occupied (with `park_reg_`) or the miss station is full | It is B's designated primary engagement signal and V10's acceptance test; it exists nowhere | `sim/simx/mem/mmu.cpp` (`on_tick`, park branch) | ~10 lines |
| **W2** | Export per-client TLB counters: extend `Socket::PerfStats` with `dmmu`, `immu`, `ummu` (reads/hits/misses/evictions), and `Cluster::PerfStats` with L2 TLB (incl. `mshr_dedups`) and PTW (`walks`, `walk_latency`, `wc_hits`) | V-X2, V8, V10 and every §7 B-metric are unmeasurable without it; the accessors already exist | `sim/simx/socket.cpp:451-465`, `sim/simx/cluster.cpp`, `perf_stats` path | ~30 lines |
| **W3** | Add a per-cycle `mshr_occ_tlb` sampler (or an integrated occupancy counter) for the ummu and L2 TLB | §7 asks for `mshr_occ_tlb` over time; no sampler exists | along the [N3] tracer | ~15 lines |
| **W4** | Add the `P`-forcing workload generator (fixed `f`, page-sparsified) and a realized-(d, ρ, P) reporter | Required by F6/F12; without it `P` cannot be set to the design's levels at all | workload harness | ~40 lines |
| **W5** | Add the A2/A3 window as a budgeted holding stage with an occupancy counter that feeds the in-flight total | Required by F1; the counter is what makes V6 checkable | dcache request path, config-gated | ~60 lines |
| **W6** | Add the A5\* clairvoyant-ordering variant of the engine | Required by F2; reuses A5's ordering stage with the predictor replaced by MC state | engine issue stage, config-gated | ~20 lines |

Items **W1–W3** are prerequisites for *any* Hypothesis-B result and should be done first: they are
~55 lines and unblock the entire B branch. Items **W4–W6** are prerequisites for the corrected A/B
arms but not for the fidelity probe in §9.

**Not required:** a bank-group DRAM model (v2 already drops that claim); a coherence model (AS4);
energy/area (out of scope); any change to `VX_tlb_*` or `VX_ptw.sv`.

---

## 9. Recommendation: go/no-go, and the minimal first experiment

**Recommendation: GO, conditional on fixes 1–9 of §7 (design text only) and instrumentation W1–W3.**

Rationale: the architecture is verified, the simulator is far more faithful than feared (21/24
mechanisms equivalent), and the two central mechanisms are separable. Nothing found requires new
hardware research or a model rewrite. The defects are in the *specification* of the comparison, not in
the platform.

**Minimal first experiment (do this before committing to anything larger).** Two smoke campaigns,
~16 runs total, one per build, both with a single seed. Their purpose is *fidelity*, not evidence: they
either confirm that the model reproduces the derivations in §3 or they stop the study cheaply.

**E1 — Translation-path fidelity probe (build V, 6 runs).**

Cell: `W1`, `κ = 0`, `N = 16 K`, `f = 512`, 4 KB pages, `S = 4`, `W = 16`, DRAM latency 60.
Arms: `B0`, `B2`, `B3` × `P ∈ {512, 8192}`.

Collect only: `tlb_reads/hits/misses` per client (needs W2), `tlb_stall_cycles` (needs W1),
`walks`, `walk_latency`, `pte_fetches`, `cycles`, `transferred_bytes`.

Pass criteria, all pre-registered:
- **E1a** ummu misses > 0 on the DXA path (V-X2 becomes real). If zero, the DXA is not exercising
  translation and the whole B branch is void *empirically*, not merely unbuilt.
- **E1b** measured walks at `P = 8192` within 2× of C14 (≈10 050). A large deviation means the
  footprint or the TLB model differs from the design's assumption.
- **E1c** measured walk throughput ≤ 2 concurrent walkers (E1 confirms C15's binding resource).
- **E1d** at `P = 512`, walks per launch ≪ 512 (the L2 TLB is absorbing), confirming F4's lower edge.
- **E1e** in `B3`, `tlb_stall_cycles` and `pte_fetches` move **before** `cycles` does. If `cycles`
  moves alone, the change is not from translation and the cell is invalid (this is v2's V10, run for
  the first time).

**E2 — Generation-independence smoke (build N, 10 runs).**

Cell: `W1`, `f = 512`, `ρ = 0`, `N = 16 K`, `S = 8`, latency 60, `κ ∈ {0, 1}`.
Arms: `A0`, `A1`, `A3`, `A4`, `A5*`.

Pass criteria:
- **E2a** `κ = 1` behaves like W0 within 15 % in A0 (the affine path really coalesces — this is the
  bug-check that F8 would otherwise break).
- **E2b** in A3, the recorded window occupancy is ≤ 128 at every sample (F1's invariant).
- **E2c** `A3 ≥ A1` and `A4/A5* ≥ A1` — i.e. the ordering arms are monotone; a violation means the
  window or the engine is leaking capacity.
- **E2d** `A5*` and `A3` produce *different* request orders (sanity that the two arms are distinct).

Only if **E1a–E1e** and **E2a–E2d** all pass should the ≈620-run plan be committed. If E1a fails, cancel
Hypothesis B entirely — that would leave a clean, single-hypothesis study of A, which §14 of v2
already names as an acceptable outcome.

---

## Appendix — audit provenance

**Files inspected (all under `DX100_MEM/ref/vortex`, read-only):**

RTL: `hw/rtl/VX_socket.sv`, `hw/rtl/VX_cluster.sv`, `hw/rtl/VX_gpu_pkg.sv`, `hw/rtl/VX_define.vh`,
`hw/rtl/vm/VX_mmu.sv`, `VX_tlb_l1.sv`, `VX_tlb_l2.sv`, `VX_tlb_cam.sv`, `VX_tlb_mshr.sv`,
`VX_ptw.sv`, `hw/rtl/dxa/VX_dxa_gmem_req.sv`, `VX_dxa_addr_gen.sv`, `VX_dxa_setup.sv`,
`VX_dxa_core.sv`, `VX_dxa_unit.sv`, `VX_dxa_worker.sv`, `VX_dxa_pkg.sv`.

SimX: `sim/simx/mem/mmu.cpp`, `mmu.h`, `mmu_tlb.cpp`, `mmu_tlb.h`, `tlb_l2.cpp`, `tlb_l2.h`,
`ptw.cpp`, `ptw.h`, `sim/simx/socket.cpp`, `socket.h`, `cluster.cpp`, `cluster.h`,
`sim/simx/dxa/dxa_core.cpp`, `sim/common/dram_bank_model.h`.

Config: `VX_config.toml`, `VX_types.toml`.

Design documents: `docs/vortex-memory-recon.md`, `docs/dx100-vortex-gap-analysis.md`,
`docs/dx100-experiment-design.md`, `docs/dx100-experiment-design-v2.md`.

**Commands run:** line-scoped `grep`/`sed` over the paths above (symbols including
`SOCKET_UNIT_MMU_ENABLE`, `UMMU_*`, `SOCKET_NUM_MMUS`, `DCACHE_NUM_REQS`, `MAX_PARKED_PER_ENTRY`,
`MAX_REQUESTERS_PER_ENTRY`, `REPLAY_DEPTH`, `mshr_find`, `walk_cache_`, `perf_stats`, and all TLB
counter names); `wc -l` on the audited files; `git status --porcelain` in `ref/vortex`; and one
`python3` arithmetic pass reproducing C2–C21 exactly (element/page/lines, `P₀(1−e^{−N/P₀})`,
distinct-line occupancy, the duplicate-rate expectation, and the throughput bounds).

**Not done:** no build, no elaboration, no simulation, no synthesis, no modification of any file
other than the creation of this audit.

**Working tree:** `ref/vortex` unmodified (0 changed paths excluding `.DS_Store`).
