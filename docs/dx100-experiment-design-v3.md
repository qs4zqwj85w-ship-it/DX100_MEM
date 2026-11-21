# Task 5 — Experiment design v3: bulk indirect address generation and bulk-aware translation on Vortex DXA

**Design revision only.** No RTL, SimX, configuration, workload or prior document was modified; no
experiment was run. Everything marked **[V]** is verified against `DX100_MEM/ref/vortex` at
`7300bc84981c34331b6247cc14500d8e07644988` with the line reference given; **[C]** is an explicit
calculation reproduced in §6 and §13; **[A]** is an assumption the design depends on and defends;
**[R]** is a recommendation. Supersedes `dx100-experiment-design-v2.md`; the audit that drove the
changes is `dx100-experiment-audit.md` (findings F1–F14, all mapped in §12).

---

## 1. Research question and hypotheses

### 1.1 The question (unchanged)

> Can extending the Vortex DXA with **bulk indirect address generation** and **bulk request
> management** improve irregular GPU memory access beyond what conventional SIMT execution,
> coalescing, caching and request scheduling already achieve?

Four mechanisms stay strictly separated throughout, because they have different costs, different
prior art and different failure modes:

| # | Mechanism | Arm family |
|---|---|---|
| M1 | **Bulk indirect address generation** — fetch `B[i]` off-core, compute `A[B[i]]` | A |
| M2 | **Request organization and reordering** — choose the issue order among held requests | A (ordering dimension) |
| M3 | **Duplicate elimination** — collapse repeated target words/lines | ablation `+D` |
| M4 | **Translation optimization** — dedup, order and pre-issue walks from bulk visibility | B |

M2 is a *dimension* of A, not a separate hypothesis: it exists so that M1 can be measured at equal
scheduling freedom. M3 is an ablation because it is prior art (IRU, ICC) and because its effect is
work elimination, not MLP. M4 is Hypothesis B. No new hardware beyond these four is proposed.

### 1.2 Hypotheses, each with an explicit failure condition

**Hypothesis A (M1).** An off-core engine that fetches index arrays, computes indirect addresses and
issues the resulting requests exposes independent memory operations earlier than SIMT can, at
*identical* outstanding-request capacity and *identical* scheduling knowledge.

> **Fails if** `A5*` does not beat `A3g` (§3) beyond the seed spread at any row locality.

**Hypothesis B (M4).** Bulk visibility into a large batch of future indirect addresses lets a bulk
engine translate better than demand translation: dedup to distinct pages, order walks for page-table
locality, and start walks before the data requests need them.

> **Fails if** `B3 ≈ B2`, or if the effect is fully reproduced by enlarging the TLB (`B1`), or if it
> only appears where translation has already collapsed (`P ≥ 8192`, §6.4).

**Hypothesis C (A ∩ B).** The two are complementary because earlier requests mean earlier translation
demand, which only a bulk mechanism can exploit.

> **Fails if** the combination is sub-additive, i.e. one mechanism subsumes the other.

---

## 2. Architecture, and what the platform actually provides

### 2.1 Verified structure (unchanged from v2 §2, re-verified)

Two *distinct* translation clients per socket:

- **SIMT path.** Per-core dMMU (`VX_mmu`, `NUM_REQS = SOCKET_SIZE × DCACHE_NUM_REQS`,
  `VX_socket.sv:344`; `DCACHE_NUM_REQS = NUM_LSU_BLOCKS × DCACHE_CHANNELS`,
  `VX_gpu_pkg.sv:2073`), TLB 16 entries per core (`VX_config.toml:205`), 4 TLB MSHRs (`:207`).
- **Engine path.** DXA → socket unit MMU `ummu` (`VX_socket.sv:855-880`), one request lane
  (`UMMU_NUM_REQS`, `VX_gpu_pkg.sv:2287`), TLB **8** entries (`VX_CFG_UTLB_SIZE`, `:212`),
  4 TLB MSHRs (same global constant), read-only, `PARK_REG=1` (`VX_socket.sv:893-899`).
- **Shared below them.** Cluster L2 TLB 512 / 4-way / 8 mega / **8 MSHRs** / 4 cycles
  (`VX_config.toml:219-223`), and **2 page-table walkers** plus a 16-entry walk cache (`:224-225`).
  Both clients arbitrate onto one cluster tlb_bus (`VX_socket.sv:420-430`, `VX_cluster.sv:366-376`).
- **The engine has no L1.** The DXA enters the socket memory arbiter directly (`VX_socket.sv:960`),
  below both L1s. This is stated once here and governs §4.3 and §10.
- **Both features are off by default** (`VM_ENABLE=false`, `EXT_DXA_ENABLE=false`,
  `VX_config.toml:22,42`) and `SOCKET_UNIT_MMU_ENABLE` needs VM ∧ DXA (`VX_define.vh:91-101`).
  Hence two builds, §2.2.

### 2.2 The two builds

| Build | Flags | Arms | Note |
|---|---|---|---|
| **N** | `VM_ENABLE=false`, `EXT_DXA_ENABLE=true` | A0–A5*, D-ablation | DXA VA = PA via `req_bypass` (`VX_mmu.sv:187-190`) |
| **V** | `VM_ENABLE=true`, `EXT_DXA_ENABLE=true`, Sv39, 4 KB pages | B0–B5, C0–C2, and A5*/A3g re-run | Adds translation |

**No claim is made or derived across builds.** The only cross-build quantity reported is the cost of
enabling VM, `A0(N) vs B0(V)`, and it is reported as a platform cost, never as an engine result.

### 2.3 The capacity budget, and why it is derived rather than chosen

The single most important fairness mechanism in v3 is that **the outstanding-request budget is a
derived configuration quantity, not a hand-set constant.** Vortex already computes MSHR depths from
Little's law:

```
mem_bytes_in_flight = (PEAK_BW // CLOCK) * MEMORY_LATENCY                      [VX_config.toml:113]
DCACHE_MSHR_SIZE    = clamp(pow2(clog2(l1_mem_bytes_in_flight / dcache_inflight_bytes)), 16, 64)   [:290]
L2_MSHR_SIZE        = clamp(pow2(clog2(mem_bytes_in_flight    / l2_inflight_bytes)),   16, 256)  [:337]
```

With the stock `MEMORY_LATENCY = 0` (`:107`) these collapse to their floors (16 and 16), which is the
real reason stock Vortex is not latency-sensitive. **v3 sets the budget by fixing
`PEAK_BW`, `CLOCK`, `MEMORY_LATENCY` and the MSHR overrides identically in every arm, then printing
the derived values and asserting them at build time (gate G1).** The design does not assert a number
it has not read back.

The chosen budget for every non-stock arm is:

```
R = 128 line-requests outstanding per socket, counted at the client→L2 request boundary
```

with `L2_MSHR = 64` per bank × 2 L2 banks = 128 (**[C]**, and within the `≤256` clamp at `:337`).

**Where requests reside (this is the F1 fix; see §5.1):**

| Population | Lives in | Consumes budget? | Released when |
|---|---|---|---|
| *unissued* — formed, holding an address, not yet presented to memory | one of the client's `R` slots | **yes** | it is selected for issue (moves to *issued*) |
| *issued* — accepted by the L1/L2, awaiting fill | the same `R` slots, plus a downstream MSHR for tracking | **no double count**: the slot stays occupied until the fill returns | fill accepted by the client |
| *in flight in the memory system* | L2 MSHR / MC queue | not separately budgeted; bounded by construction because ≤ R can ever be issued per socket | response |

Invariant, checked every cycle by gate G4: `unissued + issued_not_filled ≤ R` **for every arm**.

---

## 3. The A-family: capacity-matched, knowledge-matched comparison

### 3.1 The F1 fix — A3's advantage is *selection*, not *storage*

v2 let A3 accumulate "all generated, not yet issued" requests before MSHR allocation, i.e. it added
unbounded storage on top of the 128-slot budget. v3 removes the extra storage and keeps the
advantage, as follows.

**The `R` slots are shared between the unissued and issued populations.** A reorder capability does not
require more slots than a non-reordering client; it requires the freedom to *choose which of its own
slots to present next*. So:

- **A0/A1** — `R` slots; the selection policy is **FIFO** (arrival order). With FIFO and no
  backpressure the unissued population is ~empty, which is exactly what "no window" means.
- **A2/A3/A3g** — the *same* `R` slots, but the policy may hold k requests unissued and pick the best
  of them. Storage is identical; only the selection function differs.
- **A4/A5/A5\*** — the engine's request table is the same `R` slots, allocated when S4 forms a
  request and released when the LMEM write completes.

Backpressure arises naturally: when the L2 MSHR is full, requests accumulate unissued in the client's
`R` slots — which is precisely when a reorder policy has something to work on. **No arm may hold more
than `R` requests in any state.** The v2 table row "occupancy beyond budget: no" is now a *consequence*
of the invariant rather than an assertion.

**What A3 keeps.** Full visibility over its `R` slots and perfect knowledge of MC bank/row state.
Its scheduling advantage is: *of the up-to-`R` requests it already holds, it issues the one that hits
an open row first*. That is a large advantage and it is deliberately preserved.

### 3.2 The remaining asymmetry, disclosed and then removed by one more arm

A `R = 128` budget shared across `S` cores is **32 slots per core** at S=4, **16** at S=8
(**[C]**, §13 C22). But a SIMT reorder window is naturally *per-SM*: core *i* cannot see core *j*'s
requests without a new cross-SM path. The engine, by contrast, is a *single global client* holding
all 128. That is a 4–8× visibility advantage that has nothing to do with address generation.

**This would confound A5\* vs A3, so v3 adds `A3g`:** the *same* 128-slot budget, one **global**
(cluster-level) issue scheduler, clairvoyant about MC state, drawing only on requests that SIMT has
already generated. `A3g` requires a modelled global request path — exactly the same hardware class
as the DXA's own port, and no more speculative — so it is a fair ceiling, not a strawman.

`A3` (per-SM, 32 slots) is retained as the **realistic** ceiling; `A3g` is the **matched** ceiling.

### 3.3 The A-arm table

| ID | Name | Generator | Slots | Selection policy | MC state | Sees ungenerated addresses? | Status |
|---|---|---|---|---|---|---|---|
| **A0** | stock | SIMT | 32 total (stock MSHRs) | FIFO | — | no | stock hardware |
| **A1** | `S+q` | SIMT | 128, 32/SM | FIFO | — | no | config only |
| **A2** | `S+w` | SIMT | 128, 32/SM | row-hit **predicted from the address** | no | no | modelled window |
| **A3** | `S+W` | SIMT | 128, 32/SM | row-hit **clairvoyant** | yes | no | modelled window |
| **A3g** | `S+Wg` | SIMT | 128, **global** | row-hit **clairvoyant**, same algorithm as A3 | yes | no | modelled global issue path |
| **A4** | `E` | engine | 128, global | FIFO | — | **yes — after paying to fetch `B[i]`** | modelled engine |
| **A5** | `E+p` | engine | 128, global | predicted from the address (**same function as A2**) | no | yes (paid) | modelled engine |
| **A5\*** | `E+W` | engine | 128, global | clairvoyant (**same function as A3/A3g**) | yes | yes (paid) | modelled engine |
| **A5\*+D** | ablation | engine | 128, global | as A5\* + duplicate-word suppression | yes | yes (paid) | ablation only |

### 3.4 The comparisons, and which one is primary

| Comparison | Generation differs? | Slots | Selection | MC state | Reading |
|---|---|---|---|---|---|
| A0 → A1 | no | 32 → 128 | FIFO | — | pure capacity |
| A1 → A2 | no | 128 | FIFO → predicted | — | realistic window |
| A1 → A3 | no | 128 | FIFO → clairvoyant | — | idealised window, per-SM |
| A1 → A3g | no | 128, per-SM → global | FIFO → clairvoyant | — | idealised window, global |
| **A3g → A5\*** | **yes** | **128 = 128, both global** | **identical (clairvoyant)** | **identical** | **PRIMARY TEST OF HYPOTHESIS A** |
| A2 → A5 | yes | 128 = 128 | identical (predicted) | identical | realistic test |
| **A1 → A4** | **yes** | **128 = 128** | **identical (FIFO)** | **identical** | **cleanest generation test** |
| A4 → A5 → A5\* | no | 128 | FIFO → predicted → clairvoyant | — | value of ordering *inside* the engine |

**Primary test: `A5*` vs `A3g`.** Reasoning: capacity is identical by construction (§3.1); visibility
granularity is identical (both global over 128 slots); the ordering algorithm is the *same function*
(rank by open-row/column), differing only in whether the row state is known or predicted — and in the
primary pair both are *known*; and the only remaining difference is whether the addresses existed
before SIMT computed them. That is Hypothesis A and nothing else.

**Secondary, and more conservative: `A1 → A4`.** Both FIFO, both 128 slots, no scheduler at all. If
the engine cannot beat plain SIMT with the same capacity and no ordering intelligence, then no part of
any engine advantage can come from M2 and the result is very easy to defend.

**What this comparison cannot establish.** It cannot separate "the addresses exist earlier" from "a
single global client with 128 slots is a better shape than four 32-slot clients" *within the
realistic arms* (A2/A3 vs A5). `A3g` equalises that, at the cost of being a modelled global SIMT
issuer that no shipped GPU has. That limitation is stated rather than hidden: the primary claim is
"A + global visibility from one client", and A3g is the control that removes the visibility term.

---

## 4. The B-family, the C-family, and the capacity/visibility rules for every baseline

### 4.1 B-arms (build V)

| ID | Name | Client | L1 TLB | L2 TLB | Bulk translation | Purpose |
|---|---|---|---|---|---|---|
| **B0** | `S_vm` | SIMT | 16/core | 512 | — | reference under translation |
| **B1** | `S_vm+big` | SIMT | **256/core** | **4096** (scaled with it) | — | capacity control (F11 fix) |
| **B2** | `E_vm` | engine | 8 | 512 | **off** | control |
| **B3** | `E_vm+bt` | engine | 8 | 512 | **on** | Hypothesis B |
| **B4** | `E_vm+bt+big` | engine | **256** | **4096** | on | substitution test |
| **B5** | `S_vm+big+W` | SIMT | 256/core | 4096 | — | conventional ceiling under VM (uses A3g's global window) |

**F11 fix.** v2 scaled only the L1 DTLB, so the 512-entry L2 TLB bound the result and the "just buy
entries" control could not succeed. v3 scales **both** levels in the same ratio (16→256 is 16×, so
512→4096 is 16×), and states that 4096 exceeds `VX_CFG_L2_TLB_MEGA_SIZE`-class practical sizing — it is
a *bound*, not a proposal.

B3's three mechanisms, individually ablatable:
(i) **VPN dedup** across the tile before issue — one walk per distinct page;
(ii) **walk ordering** by VPN so consecutive walks share the 16-entry walk cache's tag space;
(iii) **pre-walk** of the next tile's pages while the current tile drains.
All three are charged: each issued walk is a real walk competing for the same 2 walkers and the same
8 L2-TLB MSHRs (§7, F5 of v2). **No extra TLB entries and no extra walkers in B3** — capacity is
identical to B2, which is what makes B2→B3 the cleanest pair in the study.

### 4.2 C-arms (build V)

The 2×2 factorial already exists and needs no new hardware model:

| ID | M1 (A) | M4 (B) | Cell |
|---|---|---|---|
| **B2** | off | off | (0,0) |
| **C1** | **on** (`A5*` under VM) | off | (1,0) |
| **B3** | off | on | (0,1) |
| **C0** | on | on | (1,1) |

Additivity is then identifiable as `C0 − B2` vs `(C1 − B2) + (B3 − B2)`, with the three pre-registered
readings of v2 §12.3 retained: sub-additive ⇒ one subsumes the other; additive ⇒ composable; super-
additive ⇒ complementarity. Because all four cells share one build, one workload, one `P`, and one
budget, no cross-build term enters.

### 4.3 Capacity and visibility rules, stated per arm

| Rule | A0 | A1–A3 | A3g | A4–A5\* | B0–B1, B5 | B2–B4 |
|---|---|---|---|---|---|---|
| Request slots | 32 | 128 (32/SM) | 128 global | 128 (1 client) | 128 (32/SM) | 128 (1 client) |
| Slots shared with unissued population | — | **yes** | **yes** | **yes** | yes | yes |
| Sees only SIMT-generated addresses | — | yes | yes | **no** (pays for `B[i]`) | yes | no (pays) |
| Global visibility | no | no | **yes** | yes | no | yes |
| Translation client | none (VM off) | none | none | none | dMMU 16/core | ummu 8 |
| L1 dcache on path | **yes** | **yes** | **yes** | **no** | yes | **no** |
| Private scratchpad | no | no | no | 128 KB tile | no | 128 KB tile |
| Extra MSHRs/walkers/TLB over its pair | — | = A1 | = A3 | = A4 | = B0 (+ its control) | = B2 |

**The L1 asymmetry is now a rule, not an omission:** the engine bypasses the L1 (§2.1), so all A-family
workloads use `Λ ≥ 4096` lines (256 KiB), which is 16× the 16 KB L1 (`VX_config.toml:272`). The
L1-resident regime is deliberately excluded from Hypothesis A because there the baseline would win by
construction and the comparison would teach nothing; it is retained only in the control cells of
Stage 2 and is labelled baseline-favouring.

---

## 5. Workload generation (rewritten)

### 5.1 Declared constants (used everywhere, never mixed)

| Symbol | Meaning | Value | Source |
|---|---|---|---|
| `w` | element width (words **and** index entries) | **8 B** | declared; W3's SpMV accumulator is a double |
| `ℓ` | cache line / memory block | **64 B** | `VX_config.toml:78` |
| `κ_line` | elements per line | `ℓ/w = 8` | **[C]** |
| `π` | elements per 4 KB page | `4096/w = 512` | **[C]** |
| `L` | lanes per memory instruction | `NUM_THREADS` = 4 stock, 32 recommended | `:59,72,128` |
| `N` | element accesses per launch | 16 384 | design choice |

**v3 never states a byte figure without naming whether it is element bytes or line bytes.** Every
table below carries units.

### 5.2 The two independent footprint knobs (F4 + F6 fix)

v2 had `f` and `P` as separate knobs, but `P = f·N·w/4096 = 32f`, so they were the same knob and the
stated `P` levels were unreachable. v3 replaces both with a pair that is genuinely independent:

- **`Λ`** — the number of **distinct 64 B lines touched**. This is the cache and DRAM footprint and,
with `N` fixed, the traffic.
- **`P`** — the number of **distinct 4 KB pages** those lines live in. This is the TLB footprint.
- **`λ = Λ/P`** — **lines per page**, the within-page density. Integer, `1 ≤ λ ≤ 64`.

The hard constraint is `P ≥ Λ/64` (a page holds 64 lines) and `P ≤ Λ` (a touched page holds ≥1
touched line). Fixing `Λ` and varying `P` therefore varies the TLB working set **while holding traffic,
L1/L2 pressure and DRAM footprint constant** — which is exactly the separation the task asks for and
which no `(f, P)` pair could provide.

**Generator.** Lay out the region as `P` consecutive 4 KB pages. Within each page touch exactly `λ`
lines at offsets `j·(64/λ)` for `j = 0..λ−1` (a fixed page-local stride, so the pattern is a page-stride
walk — reproducible, no RNG needed for placement). The `Λ` touched lines are the target set; the `N`
accesses map onto them per §5.3. `|A|`, the containing region, is `P × 4 KiB` and is reported, not
chosen.

**Lattice at `Λ = 16 384` lines (1 MiB of data), [C] §13 C23:**

| `P` pages | region | `λ` lines/page | L1 (256 lines) | L2 (16 384 lines) | L2 TLB (512) | walker cost |
|---|---|---|---|---|---|---|
| 256 | 1 MiB | 64 (dense) | miss | resident | resident | ~256 walks |
| 512 | 2 MiB | 32 | miss | resident | **at capacity** | ~512 |
| 1024 | 4 MiB | 16 | miss | resident | 2× over | ~1024 |
| 2048 | 8 MiB | 8 | miss | resident | 4× over | ~2047 |
| 4096 | 16 MiB | 4 | miss | resident | 8× over | ~4021 |
| 8192 | 32 MiB | 2 | miss | resident | 16× over | ~7083 |
| 16 384 | 64 MiB | 1 | miss | resident | 32× over | ~10 357 |

**Tested levels: `P ∈ {512, 2048, 8192}`** — the three points that sit at, 4×, and 16× the L2 TLB
capacity, with `λ ∈ {32, 8, 2}`. `Λ = 16 384` throughout, so traffic is constant across the sweep.
`P = 256` and `P = 16 384` are retained as the two endpoints of Stage 2 only.

### 5.3 Duplicate modelling (F5 fix)

The audit found v2's `1/(1+d)` wrong: drawing `N` from `M` with replacement gives

```
E[distinct] = M·(1 − e^(−N/M))          distinct fraction = (M/N)(1 − e^(−N/M))
duplicate fraction = 1 − (M/N)(1 − e^(−N/M))
```

which is **36.8 %** duplicates at `M = N`, not 0 %.

v3 defines duplicates by a **target measured rate**, with two generators:

- **`ρ_dup = 0` ⇒ permutation.** `N` distinct targets sampled **without** replacement from `Λ`
  (requires `Λ ≥ N`; satisfied since `Λ = 16 384 = N`). Duplicates are **exactly zero** by
  construction, which is the control v2 lacked.
- **`ρ_dup > 0` ⇒ replacement from `M` solved numerically** so that the *expected* rate equals the
  target. **[C]** §13 C24:

| target `ρ_dup` | `M` (distinct targets) | `M/N` | achieved |
|---|---|---|---|
| 0.00 | permutation, `M = Λ` | 1.000 | **0.0 %** |
| 0.10 | 76 362 | 4.661 | 10.0 % |
| 0.25 | 27 043 | 1.651 | 25.0 % |
| 0.50 | 10 281 | 0.628 | 50.0 % |
| 0.75 | 4 179 | 0.255 | 74.9 % |
| 0.80 | 3 300 | 0.201 | 80.1 % |
| 0.90 | 1 638 | 0.100 | 90.0 % |

Tested levels: **`ρ_dup ∈ {0, 0.25, 0.50, 0.75}`**, ablation only.

**Three distinct duplicate types, all measured and reported (never conflated):**

| Type | Definition | Who can remove it | Expected relation |
|---|---|---|---|
| `dup_idx` | repeated `B[i]` **value** | nobody below the client (it is the generator's parameter) | the knob |
| `dup_word` | repeated 8 B target **address** (`dup_idx` ⇒ `dup_word`) | the engine's word table (M3) | `dup_word ≥ dup_idx` |
| `dup_line` | repeated 64 B **line request** (`dup_word` ⇒ `dup_line`; also 8 distinct words in one line) | request-level dedup / MSHR merge | `dup_line ≥ dup_word` |

The design's M3 ablation measures **`dup_line` reduction**, because that is the only one that reduces
memory traffic. `dup_word` is measured to bound what the word table can win (it saves LMEM writes and
result remap, not bandwidth). **The realized values of all three are recorded per run and are the
ground truth; the analytic `M` above is only a generator input** (this is the F5 fix — the audit's
objection was that v2 compared a model against a distribution it did not generate).

### 5.4 κ redefined (F8 fix)

**Definition.** `κ` = the fraction of **warp memory instructions** (lane-groups of size `L`) whose `L`
lane addresses fall inside **one** 64 B line, i.e. are fully coalescible.

**Generator, per instruction `j`:**
- with probability `κ`: emit `addr[l] = base_j + l·w` with `base_j` aligned to `L·w`, so the `L` lanes
  occupy `max(1, L·w/ℓ)` **consecutive** lines (`= 1` for `L ≤ 8`, `= 4` for `L = 32`) — a
  *regular* instruction;
- otherwise: emit `L` independent addresses drawn from `Λ` **without regard to adjacency** — a
  *divergent* instruction.

**Effect on coalescing (exact).** Lines requested per instruction:

```
L_req = κ·max(1, L·w/ℓ) + (1−κ)·L
```

which evaluates to `4 − 3κ` at `L = 4`, `8 − 7κ` at `L = 8` (both `= L − κ(L−1)` because a group
fits in one line), and `32 − 28κ` at `L = 32` (a contiguous 32-lane group spans 4 lines, not 1).
Total line requests over
`N/L` instructions: `(N/L)·L_req`. Line utilization: `u = (κ·min(L·w,ℓ) + (1−κ)·w)/ℓ`.

**Both endpoints are meaningful controls [C] §13 C25:**

| κ | lines/instr (`L=4`) | total lines | useful B/line | line utilization |
|---|---|---|---|---|
| 0.00 | 4.00 | 16 384 | 8.0 | 12.5 % |
| 0.25 | 3.25 | 13 312 | 14.0 | 21.9 % |
| 0.50 | 2.50 | 10 240 | 20.0 | 31.2 % |
| 0.75 | 1.75 | 7 168 | 26.0 | 40.6 % |
| 1.00 | 1.00 | 4 096 | 32.0 | **50.0 %** |

At `L = 32` the same definition gives lines/instr `{32, 25, 18, 11, 4}`, totals
`{16 384, 12 800, 9 216, 5 632, 2 048}` and utilization `{12.5, 34.4, 56.2, 78.1, 100} %`.
**Both endpoints are meaningful controls in both settings:** κ = 0 is the fully divergent
irregular control (every lane its own line, 8 B of every 64 B used) and κ = 1 is the fully coalesced
control (one line per group at `L = 4`, four at `L = 32`, 100 % line utilization at `L = 32`).

**Residual caveat, stated rather than hidden (F8 partial):** at stock `L = 4`
(`VX_CFG_NUM_THREADS = 4`, `:60`; `NUM_LSU_LANES = SIMD_WIDTH = NUM_THREADS`, `:128`), a fully
coalesced instruction consumes only 32 B of a 64 B line, so **κ can reduce requests by at most 4×
and utilization never exceeds 50 %.** That caps the observable benefit of coalescing and therefore
compresses the κ dose-response. v3 therefore makes `NUM_THREADS ∈ {4, 32}` a **config knob applied
identically to every arm**, with `L = 32` as the primary setting for the κ sweep and `L = 4` retained
to show the cap. Raising `NUM_THREADS` also raises `SIMD_WIDTH` and `NUM_LSU_LANES` (`:72,128`), so
it must be applied uniformly or not at all; gate G3 checks this.

### 5.5 Remaining knobs

| Knob | Symbol | Levels | Role |
|---|---|---|---|
| Row/DRAM locality | `ρ` | 0, 0.5, 0.9 | fraction of consecutive targets forced into the same DRAM row (address-map driven) |
| Independent index streams | `J` | 1, 4 | concurrent gathers from `J` distinct index arrays → multiplies translation pressure at constant `Λ, P` (**new in v3**; the audit's missing control) |
| Warps / SM | `W` | 4, 16, 64 | occupancy |
| SMs | `S` | 4, 8 | stock is 1 (`:5-6`) |
| Page size | `m` | 4 KB (all arms), 2 MB (diagnostic only) | DX100's escape hatch; never a fair arm |
| Launch size | `N` | 16 384 (Stage 3 only varies) | — |

### 5.6 The actual tested combinations

**A family (build N), primary cell:** `N = 16 384`, `Λ = 16 384` lines, `P = 2048`, `λ = 8`,
`ρ = 0`, `ρ_dup = 0`, `J = 1`, `W = 16`, `S = 8`, `L = 32`, latency 60.

**A sweeps:** `κ ∈ {0, 0.5, 1}` × `P ∈ {512, 2048, 8192}` × `ρ ∈ {0, 0.5}`.

**B family (build V), primary cell:** identical to A's but with VM on, 4 KB pages, `P ∈ {512, 2048,
8192}`, `κ = 0`.

**Reference table of every combination actually run** is generated mechanically from the stage
definitions in §11 and attached to each run's manifest; the design deliberately does not enumerate
288 cells here because the manifest is the authority (and gate G7 verifies the manifest matches the
generator's realized `Λ, P, λ, ρ_dup, κ`).

---

## 6. Corrected arithmetic

All figures below use `w = 8 B`, `ℓ = 64 B`, `N = 16 384`, `Λ = 16 384` lines, and are **[C]**
reproduced in §13. The v2 numbers they replace are shown for comparison.

### 6.1 Footprint and traffic (no translation)

| Quantity | Equation | Result | v2 | Status |
|---|---|---|---|---|
| Elements per line | `ℓ/w` | 8 | — | exact |
| Elements per page | `4096/w` | 512 | 512 | exact |
| Distinct lines `Λ` | given | 16 384 | — | exact |
| Data bytes | `Λ·ℓ` | **1 048 576 B = 1 MiB = 1.05 MB** | 1.05 MB | exact |
| Distinct pages `P` | given | 512 / 2048 / 8192 | 4…16 384 (unreachable) | exact |
| Region | `P·4096` | 2 / 8 / 32 MiB | "64 MB" | exact |
| Lines per page `λ` | `Λ/P` | 32 / 8 / 2 | — | exact |
| Distinct pages for a *uniform permutation* over `Λ` | `P` (chosen) | same | — | exact by construction |

The v2 confusion is now impossible: the 1.05 MB figure is `Λ × 64 B` (line bytes) and the
512-elements-per-page figure is `4096/8` (element bytes); the two are labelled and neither is derived
from the other.

### 6.2 Page walks, PTEs, lines and traffic — four separate quantities

For `P` pages with `N` uniform accesses (**[C]** §13 C26):

| Quantity | Equation | `P = 512` | `P = 2048` | `P = 8192` | `P = 16 384` |
|---|---|---|---|---|---|
| (a) distinct pages = **walks** (after VPN dedup) | `P(1−e^(−N/P))` | 512 | 2 047 | 7 083 | 10 357 |
| (b) **PTE accesses** | `3 ×` walks (Sv39, 3 levels) | 1 536 | 6 141 | 21 249 | 31 071 |
| (c) **distinct PTE lines** | leaf lines + interior | 64+128 = 192 | 256+128 = 384 | 1 024+128 = 1 152 | 2 047+128 = 2 175 |
| (d) **PTE bytes** = (c) × 64 B | | **12 KiB** | **24 KiB** | **72 KiB** | **136 KiB** |
| (e) data bytes | `Λ·ℓ` | 1 MiB | 1 MiB | 1 MiB | 1 MiB |
| (d)/(e) | | **1.2 %** | **2.3 %** | **7.0 %** | **13.3 %** |

Notes on (c): the leaf-PTE level holds 512 PTEs per 4 KiB page and 8 PTEs per 64 B line, so each leaf
line maps 32 KiB; interior tables are one mid page (covers 1 GiB) plus one root page = 128 lines
= 8 KiB, cold and then L2-resident.

**v2's "≈1.9 MB" is wrong by ≈14×; the correct value is 136 KiB = 13.3 % of data.** v2 multiplied the
fetch-request count (b) by 64 B, which double-counts the interior fetches that hit in cache and
ignores that the page table is 1/512 the size of what it maps.

### 6.3 Why a small PTE byte count does *not* mean translation is unimportant

Bytes are the wrong cost model here. A walk is a **serialised, latency-bearing, countably-limited**
operation, not a bulk transfer:

- Byte traffic is inherently small: with 4 KiB pages and 8 B PTEs the page table is `w/4096 = 1/512`
  of the data. For any densely-traversed region, PTE bytes ≈ 1–13 % of data bytes and can never
  dominate. v2's premise was therefore not just numerically wrong but structurally impossible.
- The cost is **throughput**: only **2 walkers** exist cluster-wide (`VX_CFG_PTW_NUM_WALKERS = 2`,
  `:224`), shared with the dMMU, iMMU and ummu through one arbiter (`VX_socket.sv:420-430`).

**Translation-dominated argument, re-derived ([C] §13 C27, analytical estimate — not measured):**

```
walk service rate   = n_walkers / walk_latency = 2 / 305 = 0.00656 walks/cycle
                      (i.e. 152.5 cycles per walk, cluster-wide)
walks at P = 8192   = 7 083
walk service time   = 7 083 × 152.5 ≈ 1.08 × 10^6 cycles
data time           = 1 MiB / 256 B/cycle = 4 096 cycles      (at the derived 102.4 GB/s peak)
ratio               ≈ 264×
```

with the walk latency sweeping `{150, 305, 450}` cycles → ratios `{190×, 386×, 569×}` for
`P = 16 384` (10 357 walks). **The conclusion is robust to the walk-latency assumption by more than an order of
magnitude, but it is an analytic estimate:** the walk latency depends on the configured
`VX_CFG_PLATFORM_MEMORY_LATENCY` and on how many of the three fetches hit in L1/L2. It is a prediction
to be checked by probe P1 (gate G8c), not a result.

**Also corrected:** the walk cache is not useless. Its tag is `vpn >> TLB_VPN_LEVEL_BITS`
(`sim/simx/mem/ptw.cpp:127`), i.e. one entry per 2 MB region, so at `P = 16 384` (64 MiB) the relevant
space is 32 tags in a 16-entry direct-mapped cache — an expected hit rate of roughly 30–50 %, saving
the two interior fetches. v2's "0.1 %" used the wrong index space by ~500×. The corrected estimate is
used in (b): average fetches `≈ 2.4`, not 3.0, and the measured `wc_hits` counter will replace the
estimate (gate G8d).

### 6.4 The translation-dominated band (F4 fix)

| `P` | L2 TLB (512) coverage | expected walks | walker time | regime |
|---|---|---|---|---|
| ≤ 512 | 100 % | ≤ 512 | ≤ 78 k cy | **translation-trivial** — B has nothing to recover |
| 1024 | 50 % | 1 024 | 156 k cy | transition |
| 2048 | 25 % | 2 047 | 312 k cy | **useful range** |
| 8192 | 6 % | 7 083 | 1.08 M cy | **useful range**, near collapse |
| 16 384 | 3 % | 10 357 | 1.58 M cy | collapsed — every arm is broken |

So the useful band is **`P ∈ [512, 8192]`**, derived from the 512-entry L2 TLB at the bottom and from
2-walker saturation at the top — not from "a modest TLB" (v2's 32) nor by assertion. The tested levels
`{512, 2048, 8192}` bracket it. Below 512 the honest conclusion is *a priori* "B is nil"; above 8192
the measurement is uninformative.

---

## 7. Metrics, in three buckets

### 7.1 Already available (no code change)

| Metric | Source |
|---|---|
| `cycles`, per-SM utilization | SimX stats |
| L1/L2 hit, miss, MSHR occupancy | cache stats (`sim/simx/mem/cache.h`) |
| DRAM per-channel reads/writes/hits/conflicts, deferred | `dram_bank_model.h:145-146` (`Stats`) |
| DXA transfers, `gmem_reads`, `lmem_writes`, `gmem_latency`, `noslot_stalls` | `Socket::perf_stats().dxa` (`sim/simx/socket.cpp:456-458`) |
| dynamic instruction count | existing perf model |

### 7.2 Requiring new instrumentation (small, behaviour-preserving)

The audit's F7: **no TLB counter is currently exported.** `Mmu::tlb_reads/hits/misses/evictions`
exist (`sim/simx/mem/mmu.h:76-79`), `L2Tlb::PerfStats` (`tlb_l2.h:37-43`) and `Ptw::PerfStats`
(`ptw.h:33-37`) exist, but **nothing consumes them**: `Socket::perf_stats()` aggregates only
icache/dcache/dxa/tex/rtu (`sim/simx/socket.cpp:452-461`), and a repo-wide search for
`tlb_reads|tlb_misses|mmu_perf|walk_latency|mshr_dedups` in the perf path returns no hits.

| Metric | Status | Minimum change |
|---|---|---|
| `dmmu/ummu` reads, hits, misses, evictions | accessor exists, not exported | add to `Socket::PerfStats`; plumb to output |
| L2 TLB reads/hits/misses/evictions/`mshr_dedups` | `PerfStats` exists, not exported | add to `Cluster::PerfStats` |
| `walks`, `walk_latency`, `wc_hits` | `Ptw::PerfStats` exists, not exported | same |
| **`tlb_stall_cycles`** | **does not exist in either model** | add to `Mmu::on_tick`: count cycles a port offers a request and is refused because the park stage is occupied or the miss station is full |
| `mshr_occ_tlb` (time series) | no sampler | sample in the [N3] tracer |
| window occupancy (A2/A3/A3g) | no such stage | counter on the modelled window |
| `gen_lead` | no such counter | timestamp at tile-buffer insert and at request issue |

Estimated size: **≈55 lines** for the first four rows (the audit's W1–W3), plus ≈60 lines for the
window stage (W5) and ≈20 for `A5*` (W6). **No RTL change is proposed or required.**

### 7.3 Requiring substantial model work (not claimed to exist)

| Metric | Why substantial |
|---|---|
| `A3g` global SIMT issue path | a new cluster-level request fabric across SMs — modelled extension |
| `A2/A3` window with budget-neutral residency | a holding stage in front of the MSHR array, with exact occupancy accounting |
| `B3` bulk translation stage | VPN CAM + dedup + ordered walk issue + next-tile pre-walk |
| `A5*` clairvoyant ordering | requires exposing MC bank/row state to the issue stage |
| energy / area | out of scope (needs synthesis) |

### 7.4 Full metric list, and the two derived ratios

Retained from v2 §7: `cycles`, `transferred_bytes`, `bw_achieved`, `bw_util` (vs the **derived**
102.4 GB/s, never the declared 460 000), `outstanding[t]` mean/p95/integral, `stall_*`, `row_hits`,
`row_conflicts`, `row_hit_rate`, `hit_L1`, `hit_L2`, `dep_wait`, `result_delivery`, `dyn_insts`,
`addr_gen_cycles`, `t_dependency_chain`, `gen_lead`, `dups_eliminated`.

Three corrections to v2's metric definitions:

- **`line_efficiency`** = `useful_lines / transferred_lines` (renamed from `efficiency`, which was
  circular: `useful_bytes` was itself defined as `64 × distinct lines`).
- **`elem_efficiency`** = `8·N_distinct_words / transferred_bytes` (**new**). At `κ = 0`,
  `w = 8 B`, `ℓ = 64 B` a random gather uses 8 B of every 64 B line, i.e. 12.5 % element
  efficiency — the dominant inefficiency in W1, invisible to v2's metrics, and **not fixable by any
  arm in this study**. Reporting it prevents a reader from reading `line_efficiency ≈ 100 %` as
  "no waste".
- **`hit_L1` is reported for SIMT arms only.** The engine has no L1 (§2.1); reporting `hit_L1 = 0`
  for engine requests would read as a bug.

---

## 8. Workload definitions (final)

All are hand-written kernels over pre-materialized arrays, `N = 16 384` elements, so every arm
executes byte-identical work and the compiler cannot be a confound.

| ID | Kernel | Shape | Exercises |
|---|---|---|---|
| **W0** | contiguous control | `out[k] += A[i·S + k]` | streaming ceiling; no arm may beat it |
| **W1** | random indirect gather | `out[j] += A[B[i]]` | κ, Λ, P, ρ, ρ_dup, J |
| **W2** | graph traversal | `for j in H[K[i]]..H[K[i]+1]: A[B[j]]` | variable-length ranges; the DXA cannot express this (v2 U2 retained) |
| **W3** | CSR SpMV (+RMW) | `y[r] += A_val[p]·x[A_idx[p]]` | index reuse, writes, duplicate words |

`W2`'s engine arm remains a **modelled extension** and is flagged as such; the design runs W2 for the
SIMT arms and for the engine only if the extension is accepted at review checkpoint 3.

---

## 9. Validation gates and positive controls

Every v2 gate was re-examined. Three were **removed or rewritten** because they depended on unavailable
metrics or invalid comparisons: v2 V5 (dedup oracle equality — depended on the broken `d` model), v2
V6 (enumeration check — rewritten as G5 to include the capacity invariant), v2 V10 (kept, but its
metric did not exist → now depends on `tlb_stall_cycles`, which G2 requires).

| Gate | Check | Fail action |
|---|---|---|
| **G1** | **Derived-configuration readback.** Print and assert `DCACHE_MSHR_SIZE`, `L2_MSHR_SIZE`, `L2_TLB_*`, `PTW_NUM_WALKERS`, `UTLB_SIZE`, `DTLB_SIZE`, `NUM_LSU_LANES` for every arm. The budget `R = L2_MSHR × banks = 128` must be the *derived* value, not an assumption (`VX_config.toml:290,337`) | stop; config not as designed |
| **G2** | **Instrumentation self-test.** All counters in §7.2 are non-zero where they must be after a known-access microbenchmark; `tlb_stall_cycles` exists and is non-zero | stop; B branch unmeasurable |
| **G3** | **Uniform-config check.** `NUM_THREADS`, `S`, `W`, `W_MSHR`, `R` identical across all arms of one stage | stop |
| **G4** | **Capacity invariant.** At every cycle and in every arm, `unissued + issued_not_filled ≤ R`. A3/A3g/A5\* in particular may never exceed 128 | invalidate the cell |
| **G5** | **Enumeration.** At every cycle, the A2/A3/A3g request set ⊆ the set SIMT has already generated (post-coalesce, `MemReq`-shaped) | invalidate the cell |
| **G6** | **Correctness.** Final memory hash equals a single-threaded reference, for every workload × arm | stop |
| **G7** | **Generator conformance.** Realized `Λ`, `P`, `λ`, `ρ_dup`, `dup_idx/word/line`, `κ`, `L_req` within tolerance of the manifest | reject the run |
| **G8** | **Translation engagement and equivalence.** (a) ummu `tlb_reads > 0` on the DXA path under VM; (b) walks within 2× of §6.4; (c) walk throughput ≤ 2 concurrent walkers; (d) `wc_hits` consistent with the 30–50 % estimate; (e) **SimX vs RTL**: one DXA-translation cell run in rtlsim matches SimX on misses, walks and replay order | B results void if any fail |
| **G9** | **Platform admissibility.** `bw_achieved ≤ 102.4 GB/s`; W0 latency-sensitive as `LATENCY` sweeps 20/60/150 | stop |
| **G10** | **Streaming ceiling.** W0 within 5 % of derived peak; Little's law within 15 % | stop |
| **G11** | **Mechanism-first ordering.** In B3, `tlb_stall_cycles` and PTE fetches must fall *before* `cycles`; in A5\*, `gen_lead` must move before `cycles` | invalidate the cell |
| **G12** | **Positive control.** A5\* must show a `row_hit_rate` gain over A1 on `ρ = 0` of the same sign as DX100's reordering result. A model that cannot reproduce the effect qualitatively cannot argue its absence elsewhere | stop; model cannot support the claim |
| **G13** | **Address-map randomness.** 5 seeds permuting channel/bank/row; if the spread exceeds the effect, the result is a map artefact | report spread, not the mean |
| **G14** | **Cross-model spot check.** One non-translation cell (A1, W0) in rtlsim vs SimX | record; all absolute numbers become model-relative |

---

## 10. Minimal first probe

Purpose: **to falsify the platform assumptions cheaply**, not to produce evidence for any hypothesis.
No hypothesis is tested here and no result from it is reportable except as a gate outcome.

**P0 — instrumentation self-test (0 runs).** Compile both builds with all §7.2 counters; run the G2
microbenchmark; confirm `tlb_stall_cycles`, ummu counters, L2/PTW counters appear in the output.
*If this fails, stop — nothing downstream is measurable.*

**P1 — translation engagement (6 SimX runs).** Build V, `W1`, `κ = 0`, `Λ = 16 384`, `L = 32`,
`S = 4`, `W = 16`, latency 60, one seed. Arms `B0, B2, B3` × `P ∈ {2048, 8192}`. Collect only ummu/L2/
PTW counters, `cycles`, `transferred_bytes`.

Pass criteria: G8a (ummu engaged), G8b (walks within 2× of 2 047 / 7 083), G8c (≤2 concurrent
walkers), G11 (in B3, `tlb_stall_cycles` and PTE fetches move before `cycles`), G4 (capacity).
*If G8a fails — the DXA never translates — Hypothesis B is void empirically and the study continues
as a single-hypothesis study of A.*

**P2 — RTL/SimX equivalence, one cell (2 runs).** The same DXA gather with 4 distinct pages in
rtlsim and in SimX; compare `tlb_misses`, `walks`, and the replay order of parked requests
(`VX_tlb_mshr.sv` ordering vs `Mmu::stage_drain`). Pass: identical miss count and identical replay
order. *If this fails, no B result may be published and B3 becomes "modelled but unvalidated".*

**P3 — generation smoke (10 SimX runs).** Build N, `W1`, `Λ = 16 384`, `P = 2048`, `ρ = 0`, `S = 8`,
`W = 16`, `L = 32`, one seed; arms `A0, A1, A3g, A4, A5*` × `κ ∈ {0, 1}`.
Pass: G5 (enumeration), G4 (capacity), G6 (correctness), G7 (κ realized as the manifest's `L_req`, i.e.
4→1 lines/instr at `L = 4`, 32→4 at `L = 32`), and the
monotonicity check `A3g ≥ A1`, `A4 ≥ A1`, `A5* ≥ A1`. A violation means the window or engine is
leaking capacity and the model must be fixed before Stage 1.

**Total probe cost: 18 runs.** Commit to §11 only if P0–P3 all pass.

---

## 11. Full experiment plan, with staged go/no-go

| Stage | Content | Runs | Go/no-go |
|---|---|---|---|
| **0** | P0: instrumentation + G1–G3, G9–G10 on W0 | 4 | counters exist; platform binding |
| **1** | P1, P2, P3 | 18 | as §10 |
| **2 — A headline** | `A0, A1, A2, A3, A3g, A4, A5, A5*` × W1 × `κ ∈ {0, 1}` × 5 seeds, `P = 2048` | 80 | **go/no-go for the study** |
| **3 — A dose-response** | `A1, A3g, A4, A5*` × W1 × `κ ∈ {0, 0.5, 1}` × `P ∈ {512, 2048, 8192}` × 3 seeds | 108 | does the effect scale with irregularity? |
| **4 — A cost** | `A4, A5*` × W1 × lanes `{4,16,64}` × table rate `{0.25,1,4}` × 3 seeds | 54 | is the engine's cost charged fairly? |
| **5 — M3 ablation** | `A5*, A5*+D` × W1 × `ρ_dup ∈ {0, 0.25, 0.5, 0.75}` × 3 seeds | 24 | work elimination separate from MLP |
| **6 — B** | `B0, B1, B2, B3, B4` × W1 × `P ∈ {512, 2048, 8192}` × 3 seeds, `κ = 0` | 45 | B2→B3 non-null, and B3 > B1 |
| **7 — C** | `B2, C1, B3, C0` + `B5` × W1 × `P ∈ {2048, 8192}` × 3 seeds | 30 | requires 3 and 6 both non-null |
| **8 — Breadth** | surviving arms on W2, W3 at one `κ`, `P`, 3 seeds | ~36 | external validity |
| **Total** | | **≈ 400** | |

**Analysis.** Paired by seed within a cell. Per-workload Δ % and a geometric mean across workloads,
never a bare mean. Batch-means CI over seeds. The only inferential tests are the pre-registered pairs
of §3.4, §4.2, §11-stage boundaries. **Stage 2 is the study's go/no-go:** if `A5* ≈ A3g` there,
Hypothesis A is refuted and Stages 3–5 are cancelled (Stage 6 may still run, since B does not depend on
A's outcome).

**Pre-registered interpretation.** Retained from v2 §12.3 with the arm names updated:
`A5* ≤ A3g` at all `ρ` ⇒ A refuted (a strong negative, since A3g is unreachable hardware);
`A5* > A3g` and monotone in `κ` ⇒ A survives; effect present at `κ = 1` ⇒ bug signal, investigate;
`B3 ≈ B2` ⇒ B refuted; `B3 > B1` ⇒ B survives the capacity control; `C0 ≈ max(C1, B3)` ⇒ one subsumes
the other (the most useful negative result in the study).

---

## 12. Change log: every audit finding → resolution

| Finding | Severity | Resolution | Where | Status |
|---|---|---|---|---|
| **F1** A3 oracle adds unbounded capacity | critical | `R = 128` is a single pool of slots shared by the unissued and issued populations; the window is a *selection policy* over the client's own slots, not extra storage; invariant `unissued + issued_not_filled ≤ R` enforced by G4. A3 loses no visibility and no clairvoyance | §2.3, §3.1, §4.3, G4 | **Resolved** |
| **F2** A5 vs A3 also swaps clairvoyance for prediction | critical | added `A5*` (engine, clairvoyant, same ranking function as A3/A3g) and `A3g` (global SIMT issuer, 128 slots, clairvoyant) so both sides match on slots *and* visibility; primary test is `A5*` vs `A3g`; the residual limitation (A3g is a modelled global fabric) is stated | §3.2–§3.4 | **Resolved** |
| **F3** PTE traffic wrong by ≈14× | high | four quantities separated (walks / PTE accesses / distinct PTE lines / bytes); 136 KiB = 13.3 % replacing 1.9 MB; translation-dominance re-derived from walker throughput (2/305 → 152.5 cy/walk → 264–386× over `P` 8192→16384, 190–569× over walk latency), labelled analytical | §6.2–§6.3, §13 C26–C27 | **Resolved** |
| **F4** useful band's lower edge is 512, not 32 | high | band re-derived from L2 TLB capacity (512) and 2-walker saturation; tested levels `{512, 2048, 8192}` | §6.4 | **Resolved** |
| **F5** duplicate-rate formula wrong, contradicts W1 | high | `ρ_dup = 0` is a permutation (exactly zero duplicates); `ρ_dup > 0` solves `M` numerically; realized rates measured, never assumed; `dup_idx`/`dup_word`/`dup_line` separated | §5.3, §13 C24 | **Resolved** |
| **F6** `P` unreachable, `P = 32f` | high | `f` removed; replaced by `Λ` (lines, footprint) and `P` (pages, TLB footprint) with `λ = Λ/P`; constraint `P ≥ Λ/64`; lattice tabulated; only realizable combinations tested | §5.2, §13 C23 | **Resolved** |
| **F7** no TLB counters exported; `tlb_stall_cycles` absent | high | documented as an actual gap; metrics split into available / small-instrumentation / substantial; minimum change list with sizes (≈55 lines + W5/W6); no claim that the counters exist | §7, §9 G2 | **Resolved in specification; implementation required before Stage 6** |
| **F8** κ defined per element | high | κ redefined per warp *instruction*; exact `L_req = κ·max(1, L·w/ℓ) + (1−κ)·L`; both endpoints tabulated; `NUM_THREADS ∈ {4,32}` added because at `L = 4` a coalesced instruction cannot fill a line (κ caps at 4×, utilization at 50 %) | §5.4, §13 C25 | **Resolved, with a residual external-validity caveat** (the `L = 4` cap remains unless `L = 32` is used) |
| **F9** walk-cache hit rate computed on the wrong space | medium-high | corrected index space (2 MB tags, 16 entries, 32 tags at `P = 16 384` → 30–50 %); average fetches 2.4 not 3.0; measured `wc_hits` replaces the estimate under G8d | §6.3 | **Resolved** |
| **F10** engine bypasses the L1 | medium-high | stated as a rule; A-family restricted to `Λ ≥ 4096` (16× L1); L1-resident cells labelled baseline-favouring and confined to controls; `hit_L1` reported for SIMT only | §4.3, §7.4 | **Resolved** |
| **F11** B1/B4 scale only the L1 TLB | medium | B1 and B4 scale L2 TLB 512→4096 as well (same 16× ratio); the 4096 figure labelled a bound, not a proposal | §4.1 | **Resolved** |
| **F12** `d` and `P` coupled | medium | `P` is defined as *distinct pages* and held fixed by construction: duplicates draw from the `Λ` touched lines, so raising `ρ_dup` cannot add pages; G7 checks realized `P` per run | §5.2–§5.3, G7 | **Resolved** |
| **F13** SimX omits TLB-bus arbitration latency | low | accepted deviation, recorded; no model change (throughput contention *is* modelled; only ~2–6 cycles of latency are missing) | §12 (this row) | **Partially resolved — accepted deviation** |
| **F14** rounding/units | low | 10 357 not 10 360; MiB/MB labelled; element bytes vs line bytes never mixed (§5.1) | §5.1, §6.2 | **Resolved** |

**Non-blocking v2 corrections from audit §7 also folded in:** v2 correction 1→§3.1; 2→§3.3–3.4;
3→§5.1; 4→§5.2; 5→§5.4; 6→§5.3; 7→§6.2–6.3; 8→§6.4; 9→§7, §9; 10→§4.3, §7.4; 11→§4.1;
12→§5.5 (`J`); 13→§7.4; 14→§5.1; 15→§2.1; 16→§11; 17→§2.2.

**Unresolved items carried forward, stated as limitations (not blockers):**

| Item | Why unresolved | Impact |
|---|---|---|
| **U-a** `A3g` is a modelled global SIMT issue fabric; no shipped GPU has one | It is the only way to equalise visibility between a per-SM SIMT machine and a single-client engine | If a reviewer rejects A3g, the primary test reverts to `A3` vs `A5*`, which confounds generation with visibility — the design then reports that limitation rather than a causal claim |
| **U-b** `L = 4` caps κ's request reduction at 4× and line utilization at 50 % | stock `NUM_THREADS`; raising it is a config knob, not a property of the DXA | κ's dose-response is compressed at `L = 4`; `L = 32` is primary for the κ sweep |
| **U-c** W2 (variable ranges) is not expressible by the DXA | requires `IRMW`-class hardware that Vortex lacks | engine arm on W2 is a modelled extension, flagged; W2 otherwise SIMT-only |
| **U-d** no bank groups in the DRAM model | `dram_bank_model.h` models channel→bank→row only | DX100's channel+bank-group interleave claim stays out of scope (unchanged from v2) |
| **U-e** `VM_ENABLE ∧ EXT_DXA_ENABLE ∧ 3-level` may not build cleanly | not yet tested in this project | G1 catches it in Stage 0; a failure is a reportable finding, not a workaround |

**Honest novelty statement (unchanged):** M1 (off-core address generation) is absent from Vortex but
is the DMP/IRU/ICC lineage; M2 (reordering) is PTW-scheduling-adjacent and well studied; **M3 (dedup)
is prior art** (IRU, ICC) and is therefore an ablation, not a hypothesis; **M4's individual mechanisms
all already exist** in Vortex (4-entry VPN-dedup miss station, 16-entry walk cache) or in the
literature (PTW scheduling ISCA'18, neighbourhood translation MICRO'18, Valkyrie PACT'20, IGS-TLB
ICPP'23, DUCATI TACO'19, LATPC MICRO'25). The only defensible novelty in this study is the
**combination**: bulk indirect lookahead that is *free* (paid for once, when the index tile is
fetched) and that a translation mechanism can therefore exploit ahead of demand.

---

## 13. Appendix — every calculation, reproducible

Inputs: `N = 16 384`, `w = 8`, `ℓ = 64`, `Λ = 16 384`, Sv39 (3 levels), 4 KiB pages, 512 PTEs/page,
8 PTEs/64 B leaf line.

**C22 — slots per client.**
`R = L2_MSHR × L2_banks = 64 × 2 = 128`; per SM at `S = 4`: `128/4 = 32`; at `S = 8`: `128/8 = 16`.
Stock: `L2_MSHR = 16` at `LATENCY = 0` (`:107,113,337`) ⇒ `R₀ = 32`; stock L1 MSHR 16 × 2 banks ×
1 socket = 32. Consistent.

**C23 — footprint lattice.**
`λ = Λ/P`; `region = P × 4 KiB`; constraint `Λ/64 ≤ P ≤ Λ`.
`P ∈ {256, 512, 1024, 2048, 4096, 8192, 16384}` ⇒ `λ ∈ {64, 32, 16, 8, 4, 2, 1}`;
regions `{1, 2, 4, 8, 16, 32, 64} MiB`.

**C24 — duplicates.** `E[distinct] = M(1−e^{−N/M})`; duplicate fraction `= 1 − (M/N)(1−e^{−N/M})`.
Solved to target: 10 %→`M = 76 362`; 25 %→`27 043`; 50 %→`10 281`; 75 %→`4 179`; 80 %→`3 300`;
90 %→`1 638`. `ρ_dup = 0` is a permutation (without replacement), duplicates exactly 0.

**C25 — κ.** `L_req = κ·max(1, L·w/ℓ) + (1−κ)·L`; total lines `= (N/L)·L_req`;
utilization `= (κ·min(L·w, ℓ) + (1−κ)·w)/ℓ`.
`L = 4` (`max(1,·) = 1`): `{4, 3.25, 2.5, 1.75, 1}` lines/instr at `κ = {0, .25, .5, .75, 1}`;
totals `{16384, 13312, 10240, 7168, 4096}`; utilization `{12.5, 21.9, 31.2, 40.6, 50} %`.
`L = 32` (`max(1, 256/64) = 4`): `{32, 25, 18, 11, 4}`; totals
`{16384, 12800, 9216, 5632, 2048}`; utilization `{12.5, 34.4, 56.2, 78.1, 100} %`.

**C26 — translation accounting.** walks `= P(1−e^{−N/P})`; PTE accesses `= 3 × walks`;
leaf pages `= P/512`; leaf lines `= 64 × P/512`; distinct leaf lines touched
`= (P/8)(1−(1−8/P)^N)`; interior `= 128` lines; bytes `= 64 × lines`.

| `P` | walks | PTE accesses | leaf lines | interior | PTE bytes | % of 1 MiB |
|---|---|---|---|---|---|---|
| 512 | 512 | 1 536 | 64 | 128 | 12 KiB | 1.2 % |
| 2 048 | 2 047 | 6 141 | 256 | 128 | 24 KiB | 2.3 % |
| 8 192 | 7 083 | 21 249 | 1 024 | 128 | 72 KiB | 7.0 % |
| 16 384 | 10 357 | 31 071 | 2 047 | 128 | 136 KiB | 13.3 % |

**C27 — walk service time.** throughput `= 2 / walk_latency`;
`P = 8 192`: `7 083 × 152.5 = 1.08×10⁶ cy` (at 305 cy) vs data `1 MiB / 256 B/cy = 4 096 cy`
⇒ **264×**. Sweep at `P = 16 384` (10 357 walks): 150 cy ⇒ `10 357×75 = 7.77×10⁵` ⇒ **190×**;
305 cy ⇒ `1.58×10⁶` ⇒ **386×**; 450 cy ⇒ `2.33×10⁶` ⇒ **569×**.

**C28 — peak bandwidth.** `channels × ℓ × data_rate × clock = 2 × 64 B × 2 × 400 MHz = 102.4 GB/s`
= 256 B/cycle. The declared `460 000` (`:96`) is non-binding at `LATENCY = 0`.

---

*End of v3. No source, RTL, configuration or prior document was modified. Awaiting review before any
implementation; the first implementation step is instrumentation P0, not the experiment.*
