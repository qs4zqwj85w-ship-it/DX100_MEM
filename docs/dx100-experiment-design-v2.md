# Task 3 (revised) — Three separable hypotheses: bulk address generation, bulk-aware translation, and their combination

**Status: design only. No simulator code was written, no RTL modified. Submitted for review.**

Snapshot: `DX100_MEM/ref/vortex` at `7300bc84981c34331b6247cc14500d8e07644988` (VORTEX_VERSION 3.0).
Every claim marked *(verified)* below was read from that tree while writing this document, with the
line numbers given; claims marked *(recon)* come from `vortex-memory-recon.md` (Task 1) and
*(gap)* from `dx100-vortex-gap-analysis.md` (Task 2). DX100 section references are to
arXiv:2505.23073v2.

This document supersedes `dx100-experiment-design.md` (v1). §1 states exactly what changed and why,
and every v1 experiment is carried forward somewhere — nothing is silently dropped.

---

## 1. What changed from v1, and why

### 1.1 Corrections of record

The critical architectural investigation (§2) invalidated three structural assumptions in v1.
They are corrected here and the affected v1 material is re-derived, not patched.

| # | v1 said | Source says | Consequence |
|---|---|---|---|
| **X1** | DXA gmem reads are translated by the socket **dMMU** (§2.1 diagram; §3.2 row 2; §6 R2; §12 R5) | They are translated by a **separate socket unit MMU** (`ummu`), a third `VX_mmu` instance with its own TLB (`VX_CFG_UTLB_SIZE = 8`) and its own tlb_bus into the shared cluster L2 TLB (`VX_socket.sv:855-927`, `:880`, `:909`; `VX_gpu_pkg.sv:2286`, `:2360`) | Translation is a *distinct resource per client*, not a shared one. Hypothesis B becomes a real experiment instead of a footnote. |
| **X2** | "Engine TLB 256 entries, software-preloaded … **disabled** in C4–C6; keep the shared dMMU for all arms" (§5.2, §12 R5) | The **implemented** Vortex engine path has an 8-entry UTLB, not 256. 256 is DX100's aspiration, not the Vortex baseline | The engine is *worse* off than the baseline on translation (8 entries vs 16 per core). Both the stock-8 and the 256 arms must exist, and the 256 arm is a capacity claim that must be matched on the conventional side. |
| **X3** | The whole DXA↔translation question is "unresolved" (Task 2 §6, §7 evidence gap) | Resolved: requests are **virtual**, translation is on the path, misses ride the **same** cluster walker complex | Task 2's one surviving hypothesis is now testable, and it is testable *cheaply*. |

Two further v1 defects, independent of the RTL finding:

- **X4** — v1's C3 ("16K-entry reorder buffer … perfect knowledge of all pending line addresses")
  never said what "pending" meant. Read loosely it grants the baseline knowledge of addresses SIMT
  has not yet produced, which is the exact confound the task forbids. §5.2 replaces it with an
  explicit capability ladder and an enumeration rule.
- **X5** — v1 compared six configs on one axis (engine vs SIMT), so "the engine won" and "the
  engine's *translation* won" were indistinguishable. §3 splits the axis in two.

### 1.2 v1 → v2 mapping (nothing discarded)

| v1 config | v2 arm | Change |
|---|---|---|
| C1 baseline | **A0** | unchanged (now stated as VM-off) |
| C2 larger queues | **A1** | unchanged |
| C3 oracle window | **A3** | **redefined** by the enumeration rule (§5.2); the v1 wording is withdrawn |
| — | **A2** (new) | 256-entry *realistic* reorder window — v1 §14 checkpoint 2's open question, now mandatory |
| C4 engine, no order | **A4** | unchanged |
| C5 engine + row order | **A5** | unchanged |
| C6 full engine (+dedup) | **A5+D** | dedup kept, but demoted to an ablation (§5.3), not an arm of the main comparison |
| — | **B0–B5** (new) | Hypothesis B: the translation axis, absent from v1 |
| — | **C0** (new) | Hypothesis C: combination and additivity |

v1's experimental stages 1–5, validation gates V0–V8, metric definitions, workload W0–W3, and the
pre-registered interpretation table are all carried forward (§6 workloads, §7 metrics, §11 gates
V0–V13, §12 stages and §12.3 interpretation), with the config names rewritten and v1's V6/V7
renumbered V12/V13.

---

## 2. Critical architectural investigation: does a DXA request pass through the dMMU?

### 2.1 Chain of custody, source-verified

Traced from the DXA opcode to a physical address at the L2. Each step is the *actual* RTL.

| # | Step | Evidence |
|---|---|---|
| 1 | A warp's `WGATHER` opcode packs (`smem_addr`, `meta`, 5 × `coords`) into `dxa_req_data_t`; the DXA has **no address operand of its own** — the base lives in a descriptor in memory | `VX_dxa_unit.sv:33-105` (`:60-80` packing, `:92-105` buffer) |
| 2 | Request → socket `dxa_req_arb` → elastic queue (`VX_CFG_DXA_QUEUE_SIZE = 16`) → descriptor fetch FSM (registered-read BRAM, +1 cycle) → dispatch to workers | `VX_dxa_core.sv:49-140` (`:61` arb, `:76` queue, `:134` dispatch); `VX_config.toml:429` |
| 3 | `setup` stages `desc_data.base_addr + coord0 × elem_size` as the seed address, **full `VX_CFG_MEM_ADDR_WIDTH` width, no truncation** | `VX_dxa_setup.sv:512-513` |
| 4 | `addr_gen` odometers that seed through an N-dimensional cursor and emits `out_cl_addr` = base >> `CL_OFF_BITS` (a **line-granular address**) | `VX_dxa_addr_gen.sv:166, 206, 249-252` |
| 5 | `gmem_req` drives the bus with `rw = 1'b0` (**read-only**) and `addr = ag_cl_addr`, 8 in-flight slots (`VX_CFG_DXA_MAX_INFLIGHT = 8`) | `VX_dxa_gmem_req.sv:200-201`; `VX_config.toml:430` |
| 6 | Worker gmem arbiter → `gmem_bus_if` → socket port; **registered whenever `SOCKET_UNIT_MMU_ENABLE`**, with the comment "the unit MMU's TLB lookup under VM" | `VX_dxa_core.sv:183-196` |
| 7 | Socket declares `dxa_gmem_bus_if[1]` off the DXA core | `VX_socket.sv:782, 813` |
| 8 | **DXA gmem is wired into `unit_mem_bus_if[UMMU_DXA_IDX]`, the input of the unit MMU** | `VX_socket.sv:880` |
| 9 | `ummu` = a third `VX_mmu`: `NUM_REQS = UMMU_NUM_REQS`, `TLB_SIZE = VX_CFG_UTLB_SIZE` (8), `EXEC_SIDE = 0`, `WRITE_ENABLE = 0`, `PARK_REG = 1`, `RSP_OUT_BUF = 2`, `tlb_bus_if = socket_tlb_bus_if[2]` | `VX_socket.sv:855-927` (`:893-909`) |
| 10 | All three socket MMUs' tlb_buses arbitrate onto one `cluster_tlb_bus_if` | `VX_socket.sv:420-430`; `SOCKET_NUM_MMUS = 3` (`VX_gpu_pkg.sv:2360`) |
| 11 | Cluster: L2 TLB 512 entries / 4-way / 8 mega / **8 MSHRs** / 4-cycle latency, miss bus exported to the device-level walker | `VX_cluster.sv:381-392`; `VX_config.toml:219-223` |
| 12 | Walker complex: **2 independent walkers** + a **16-entry direct-mapped walk cache** | `VX_ptw.sv:24-25`; `VX_config.toml:224-225` |
| 13 | Response: ummu `mem_bus_if` → `skid_if` → `core_bus_if.rsp_*` → `gmem_rsp_in` → worker gmem arb → `sw_*` → `smem_wr` → LMEM | `VX_mmu.sv:101-115, 566`; `VX_dxa_worker.sv:245-300` |

### 2.2 The answer

**DXA gmem requests carry virtual addresses and are translated before they reach the L1/L2.**

- **Address representation.** Virtual, at `VX_CFG_MEM_ADDR_WIDTH` (48 bits if `XLEN_64`, else 32;
  `VX_config.toml:79`), line-granular. The DXA has no notion of a physical address anywhere in its
  datapath, and the descriptor's `base_addr` is written whole (`VX_dxa_setup.sv:512`).
  The kernel supplies a `vx_mem_alloc`-style VA; nothing in the DXA or its driver converts it.
- **Translation interface.** A single request lane per socket: `UMMU_NUM_REQS = EXT_TEX + EXT_RTU +
  EXT_DXA` = **1** in the DXA-only configuration (`VX_gpu_pkg.sv:2287`, `:2286`). One lookup per
  cycle, one park per cycle.
- **TLB.** A *separate* TLB of **8 entries** (`VX_CFG_UTLB_SIZE`, `VX_config.toml:212`), fully
  associative, MRU, and **not shared with the cores' dTLBs** (which are 16 entries per core,
  `:205`). The stated reason is isolation: "keeps texture, BVH and tensor streams from evicting the
  dTLB's entries" (`VX_socket.sv:858-859`).
- **Page-walk behaviour.** A miss parks in a **4-entry VPN-deduplicated miss station**
  (`VX_CFG_L1_TLB_MSHR_SIZE`, `:207`; `VX_mmu.sv:296-300`; `VX_tlb_mshr.sv:44`), issues onto the
  socket tlb_bus, arbitrates against the dMMU and iMMU, probes the L2 TLB (8 MSHRs, VPN-dedup,
  `VX_tlb_l2.sv:26`), and only then reaches the **2 shared walkers**. Miss handling is
  hit-under-miss **within a lane**: a parked page's later requests are held (`stage_hold`,
  `VX_mmu.sv:408`) while other pages can still park, but the single lane means the socket cannot
  present more than one new address per cycle and cannot park two misses in the same cycle
  (`VX_mmu.sv:304-320`).
- **Response path.** Identical to the L1 path; translation only rewrites the address bits, then the
  request goes to the socket memory arbiter as `socket_arb_in_if[DXA_MEM_ARB_IDX]`
  (`VX_socket.sv:960`) — i.e. the DXA is a *peer client* of the L2, arbitrated **last** so it
  cannot starve core traffic   (`VX_socket.sv:935-937`).

### 2.3 Three consequences that reshape the experiment

**(a) The whole DXA-translation path exists only under VM, and both features are off by default.**
`VX_CFG_VM_ENABLE = false` (`VX_config.toml:22`) and `VX_CFG_EXT_DXA_ENABLE = false` (`:42`);
`SOCKET_UNIT_MMU_ENABLE` is only defined when VM *and* one of TEX/RTU/DXA is on
(`VX_define.vh:91-101`). With VM off, `vm_active = 0` forces `req_bypass` in every MMU
(`VX_mmu.sv:187-190`) and the DXA's VA is used verbatim as a physical address.
So the experiment has **two distinct builds**, and Hypothesis B is only testable in the VM-on one.
This is a hard branch in the design, not a knob.

**(b) The baseline gets *more* translation capacity than the engine, not less.**
Per socket: conventional side = `S × 16` DTLB entries + 512-entry L2 TLB shared; engine side = 8.
At 4 SMs that is 64 : 8. Any Hypothesis B result must therefore be read as "the bulk mechanism beat
a *better-provisioned* conventional side", which is the direction that makes a positive result
credible and a negative result strong.

**(c) The translation ceiling is 2 walkers, cluster-wide, and it is shared.**
`VX_CFG_PTW_NUM_WALKERS = 2` (`:224`). No amount of address visibility in the engine raises this;
the only legitimate bulk mechanisms are the ones that reduce *walks per distinct page* and *walks
issued ahead of need* — dedup, VPN-ordered walk issue (walk-cache reuse), and pre-walking the next
tile. That is a narrow, enumerable mechanism set, which is exactly what §3's Hypothesis B contains.

### 2.4 What remains uncertain, and the targeted verification test

| ID | Uncertainty | Why it matters | Test |
|---|---|---|---|
| **U1** | Whether SimX's `ummu` is *equivalent* to the RTL on the points that matter (miss dedup scope, park serialization, replay order). SimX builds it from `Mmu::Create(name, UMMU_NUM_REQS, VX_CFG_UTLB_SIZE, …, write_enable=false, park_reg=true)` (`sim/simx/socket.cpp:220-222`) with the miss station sized by `VX_CFG_L1_TLB_MSHR_SIZE` (`sim/simx/mem/mmu.cpp:41`) — matching the RTL parameters, but SimX and RTL have diverged before (Task 2 §2.7) | A Hypothesis B result inherits the model's fidelity | **V-X1**: microbenchmark that touches 4 distinct pages in a known order through the DXA, compared against the RTL DXA pipeline trace (`DBG_TRACE_DXA`), counting parks and replays per page |
| **U2** | Whether the simulator ever *exercises* the ummu on a DXA path in practice (an unexercised path is an unvalidated path) | If `ummu` is dead code in SimX, Hypothesis B cannot be run at all | **V-X2**: assert `unit_mmu_` shows non-zero `tlb_reads()` after a DXA gather under VM; fail the campaign if zero |
| **U3** | Whether the device-level PTW in SimX honours `VX_CFG_PTW_NUM_WALKERS` concurrency (it constructs `walkers_(VX_CFG_PTW_NUM_WALKERS)`, `sim/simx/mem/ptw.cpp:25`) and whether the walk cache is consulted the way `VX_ptw.sv:66` implies | The 2-walker ceiling is a load-bearing constraint | **V-X3**: a 512-page streaming touch must show walk throughput saturating at 2 concurrent walks in both models |

If U1–U3 cannot be settled, Hypothesis B is reported as *modelled but unvalidated* and no B
conclusion is published. Hypothesis A is unaffected by all three (VM off ⇒ no ummu).

---

## 3. The three hypotheses

Stated so that each can fail.

### Hypothesis A — bulk indirect-address generation

> An independent GPU-side engine that fetches index arrays, computes indirect addresses, and
> issues the resulting requests can expose independent memory operations **earlier** than
> ordinary SIMT execution can, at equal memory capacity and equal scheduling information.

**Fails if** the engine's request stream, once the engines's own cost is charged, is no better
than a SIMT baseline that has (i) the same number of outstanding line requests, (ii) a realistic
large reorder window, and (iii) an idealized reorder window over the requests it has already
generated. The claim is about **generation independence**, not queue depth, and not ordering.

### Hypothesis B — bulk-aware address translation

> Visibility into a large batch of future indirect addresses lets a bulk engine translate them
> better than demand translation: dedup to distinct pages, amortize interior page-table fetches
> by ordering walks, and start walks before the data requests need them.

**Fails if** the effect is (i) nil, (ii) fully reproduced by giving the conventional side a bigger
TLB, or (iii) fully reproduced by the walk cache and MSHR dedup that Vortex already has at 4 and 8
entries. Note the honest prior: **every mechanism in this hypothesis already exists in some form in
Vortex or in the literature** (`mshr_find` VPN dedup, `sim/simx/mem/mmu.cpp:134`; the 16-entry walk
cache, `VX_ptw.sv:66`; PTW scheduling, neighbourhood translation, Valkyrie, IGS-TLB, DUCATI,
LATPC). What is *absent* is the **free, descriptor-derived lookahead**, and that is the only
defensible novelty claim. §9 states this explicitly rather than leaving it implied.

### Hypothesis C — combination

> Address-generation independence and translation lookahead are **complementary**: the pair is
> worth more than the sum of the parts, because earlier requests also mean earlier translation
> demand, which the bulk mechanism can only exploit if the addresses exist early.

**Fails if** the combination is sub-additive (one mechanism subsumes the other) — which is the more
likely outcome and must be reported as such.

**Why separate them at all.** They have different costs (A needs a 128 KB tile buffer and an address
ALU; B needs a VPN CAM and a walk-issue queue), different prior art, and different failure modes.
Conflating them is how a translation win gets written up as a memory-level-parallelism win.

---

## 4. Updated architecture

```
                    ┌──────────────────────── one Vortex socket (VM-enabled build) ────────────────────────┐
                    │                                                                                      │
 ┌───────────┐      │  ┌──────────────────────┐   ┌───────────────────────────┐   ┌──────────────────┐    │
 │ SM 0      │      │  │ [N2] WINDOW (A2/A3)  │   │ [N1] BULK ENGINE (A4/A5)  │   │ [N4] BULK XLATE  │    │
 │ sched/LSU │      │  │ 256-entry real |     │   │ desc RAM → S1 fetch       │   │ (B3/B4)          │    │
 │ coalesce  │      │  │ oracle over already- │   │ S2 index fill (charged)   │   │ VPN dedup CAM    │    │
 │ MSHR      │      │  │ generated requests   │   │ S3 addr ALU (16 lanes)    │   │ VPN-sorted queue │    │
 └─────┬─────┘      │  │ (before MSHR alloc)  │   │ S4 issue (1/cycle)        │   │ walk pre-issue   │    │
       │            │  └──────────┬───────────┘   └─────────────┬─────────────┘   └────────┬─────────┘    │
       ▼            │             ▼                             ▼                          │              │
 ┌──────────────────────────────────────────────────────────────────────┐                │              │
 │ [E] socket memory arbiter — icache > dcache > (tex/rtu) > DXA (last) │                │              │
 └──────┬───────────────────────────────┬───────────────────────────────┘                │              │
        │                               │                                                │              │
        ▼                               ▼                                                ▼              │
 ┌─────────────────┐   ┌──────────────────────────────┐   ┌──────────────────────────────────────┐      │
 │ [F0] dMMU       │   │ [F1] unit MMU (ummu)         │   │ [F2] cluster L2 TLB 512/4-way        │      │
 │ per core        │   │ NUM_REQS = 1                 │   │  + 8 mega, 8 MSHR (VPN-dedup)        │      │
 │ DTLB 16, MSHR 4 │   │ UTLB  8, MSHR 4, RO, PARK_REG│   │  latency 4                           │      │
 └────────┬────────┘   └──────────────┬───────────────┘   └───────────────┬──────────────────────┘      │
          │  (dMMU/ummu/iMMU arbitrate onto one cluster tlb_bus)          │                              │
          └──────────────────────────────┴───────────────┬───────────────┘                              │
                                                          ▼                                              │
                                       ┌────────────────────────────────────┐                            │
                                       │ [F3] PTW: 2 walkers + 16-entry     │  ← ceiling: 2 concurrent   │
                                       │ direct-mapped walk cache           │    walks per cluster       │
                                       └────────────────┬───────────────────┘                            │
                                                        ▼                                               │
 ┌─────────────────────────────────────────────────────────────────────────┐                            │
 │ [G] L1 dcache (16 KB, 4-way, MSHR 16) · L2 (1 MB, 8-way, L2 MSHR 16)     │                            │
 └──────────────────────────────────┬──────────────────────────────────────┘                            │
                                    ▼                                                                    │
 ┌─────────────────────────────────────────────────────────────────────────┐                            │
 │ [H] memory interface, static channel interleave                          │                            │
 └──────────────────────────────────┬──────────────────────────────────────┘                            │
              ┌────────────────────┴────────────────────┐
              ▼                                          ▼
      ┌───────────────────┐                      ┌───────────────────┐
      │ [I] channel 0     │                      │ [I] channel 1     │  ← sim/common/dram_bank_model.h
      │ read q 32 / wr 32 │                      │ (identical)       │
      └───────────────────┘                      └───────────────────┘
```

**New blocks: [N1] engine, [N2] window, [N4] bulk translation.**
**[E]–[I] are existing SimX code that the experiment parameterizes, not replaces.**
The diagram's key correction over v1: **there are two translation paths** ([F0] for SIMT, [F1] for
the engine), they arbitrate at [F2], and they share the 2-walker ceiling at [F3]. v1 merged them
into one "dMMU" box, which is what made X1 possible.

Attachment points are unchanged from v1 §2.2 except:

| Block | Attaches as | Note |
|---|---|---|
| [N4] bulk translation | A filter/scheduler **inside** the engine's issue stage (S4), *not* a new MMU instance | It must not add TLB entries or walkers — those are the capacity budget (§8 R2) |
| [F1] ummu instrumentation | Passive taps on the existing `Mmu` (`sim/simx/mem/mmu.cpp`) | SimX already exposes `tlb_reads/hits/misses/evictions` (`sim/simx/mem/mmu.h:76-79`) |

---

## 5. Revised comparison configurations

### 5.1 The two builds

| Build | Flags | Arms | What it can measure |
|---|---|---|---|
| **N (no VM)** | `VM_ENABLE=false`, `EXT_DXA_ENABLE=true` | A0–A5 | Hypothesis A only. DXA VA = PA (`VX_mmu.sv:187-190` bypass) |
| **V (VM)** | `VM_ENABLE=true`, `EXT_DXA_ENABLE=true`, SV39, 4 KB pages | B0–B5, C0, A5(V) | Hypotheses B and C, and A re-run under translation |

Nothing is compared across builds. Every claim is made inside one build.

### 5.2 The perfect-knowledge baseline, defined exactly

The task requires this to be defined carefully, and v1 was not careful enough. The rule:

> **Enumeration rule.** At cycle *t*, a window may hold only requests that the SIMT front end has
> **already formed** by *t* (post-coalescing, `MemReq`-shaped) and that have **not yet been
> accepted by memory**. It may reorder and delay them. It may not create a request, compute an
> address that no lane has computed, or hold a request beyond the capacity budget of §8 R2.

Three permitted capabilities, one forbidden one:

| Capability | A0 | A1 | A2 (realistic) | A3 (oracle) | A4/A5 (engine) |
|---|---|---|---|---|---|
| Requests in flight to memory | 32 | 128 | 128 | 128 | 128 |
| Reorder window size | — | — | 256 entries | unbounded (= all generated, not yet issued) | = its own batch (16K) |
| Knows MC bank/row **state** | no | no | predicted from address only | **yes, clairvoyant** | A4 no, A5 yes |
| Charges reorder cost | — | — | 1 cyc insert + 1 cyc select per issue | **0** | 1 elem/cyc/slice table update |
| **Knows addresses SIMT has not generated** | no | no | **no** | **no** | **yes — by construction, after paying to fetch `B[i]`** |
| Occupancy beyond budget | no | no | no | no | no |

A3 is therefore the honest ceiling of "give the ordinary execution engine a very large, very smart
scheduler". A5's advantage over A3, if any, is Hypothesis A's entire content, because A5's ordering
knowledge is *identical in kind* to A3's — only its provenance differs.

### 5.3 The arms

**Hypothesis A (build N).**

| ID | Name | Generator | In-flight | Window / order | Purpose |
|---|---|---|---|---|---|
| **A0** | stock SIMT | SIMT | 32 | none | Reference. Stock MSHR/L2-MSHR/MC queue |
| **A1** | `S+q` | SIMT | 128 | none | **Strong conventional baseline #1**: absorbs pure capacity |
| **A2** | `S+q+w256` | SIMT | 128 | 256-entry realistic reorder, address-predicted row order | **Strong conventional baseline #2**: absorbs "a realistic bigger window" |
| **A3** | `S+q+W` | SIMT | 128 | clairvoyant reorder over generated requests, free | **The ceiling**: absorbs "an idealized scheduler" |
| **A4** | `E` | engine | 128 | in-order, no dedup, no row order | Isolates **generation independence** vs A1 |
| **A5** | `E+ord` | engine | 128 | row-aware reorder of its own batch | Isolates generation **at equal ordering freedom** vs A3 |
| **A5+D** | ablation | engine | 128 | + dup suppression | v1's C6, demoted: the claim it tests is work elimination, which is prior art (IRU, ICC) *(gap §5)* |

**Hypothesis B (build V, 4 KB pages).**

| ID | Name | Client | TLB entries | Bulk translation | Purpose |
|---|---|---|---|---|---|
| **B0** | `S_vm` | SIMT | DTLB 16/core (64 at S=4) | — | Reference under translation |
| **B1** | `S_vm+big` | SIMT | **256/core** | — | **Capacity control**: "just make the TLB bigger", at DX100's own engine-TLB size |
| **B2** | `E_vm` | engine | UTLB 8 | **off** | Control: the engine without the mechanism |
| **B3** | `E_vm+bt` | engine | UTLB 8 | **on** | **Hypothesis B's mechanism** at *identical* capacity to B2 |
| **B4** | `E_vm+bt+big` | engine | UTLB 256 | on | Substitution test: does bulk translation still pay when capacity is granted? |
| **B5** | `S_vm+big+W` | SIMT | 256/core | — | The conventional ceiling under VM: big TLB **and** the oracle window |

**Hypothesis C (build V).**

| ID | Name | = | Purpose |
|---|---|---|---|
| **C0** | `E+ord+bt` | A5(V) ∪ B3 | The pair |
| **C1** | `E+ord` | A5 run in build V | The addend |
| **C2** | `E_vm+bt` | B2→B3 delta | The other addend |

Additivity test (§12): is `C0 − B2` ≈ `(C1 − B2) + (B3 − B2)`? Sub-additive ⇒ one subsumes the
other; super-additive ⇒ complementarity (the interesting case); `C0 ≈ max(C1, B3)` ⇒ one makes the
other unnecessary.

**Total: 13 distinct configurations**, each a flag set on one model. Two are ablations that need not
run in every stage.

---

## 6. Updated workload matrix

### 6.1 The four workloads (carried from v1, plus the independence knob)

| ID | Workload | Shape | Tests |
|---|---|---|---|
| **W0** | Contiguous control | `out[k] += A[i·S + k]`, `i` sequential | The streaming ceiling. No arm may beat it (gate V2) |
| **W1** | Random indirect gather | `out[j] += A[B[i]]` | Hypothesis A's core case; `κ = 0` |
| **W2** | Graph traversal | `for j in H[K[i]]..H[K[i]+1]: A[B[j]]` — variable-length ranges | DX100's *indirect range* (`§3.1`), which the DXA cannot express *(gap §2.2)* |
| **W3** | Sparse matrix (CSR SpMV) | `y[r] += A_val[p]·x[A_idx[p]]`, plus an RMW variant | Index reuse, writes, duplicate indices |

### 6.2 Knob matrix

| Knob | Symbol | Levels | What it isolates |
|---|---|---|---|
| **Address independence** | `κ` | 1.0, 0.75, 0.5, 0.25, 0 | Fraction of elements whose index is a compile-time affine function of the loop counter. **`κ=1` is fully coalescible (a bulk engine should be worthless); `κ=0` is fully indirect.** This is the "different degrees of address independence" requirement, and it is the single most important control in the study: it predicts that the engine's value must vanish as `κ→1` |
| Elements per launch | `N` | 1 K, 16 K, 256 K | 16 K = one DX100 tile; 1 K = under-filled |
| Data footprint | `f = |A|/N` | 8, 64, 512 | L1-resident → DRAM-only |
| Duplicate rate | `d` | 0, 1, 4 | Work-elimination ablation only |
| Row locality | `ρ` | 0, 0.5, 0.9 | Hypothesis A's ordering arm |
| **Pages touched per launch** | `P` | 4, 32, 512, 8 192, 16 384 | **Hypothesis B's independent variable**: TLB working set. Derived from `f` and the page size, or forced independently by scattering the footprint |
| **Page size** | `m` | 4 KB (all arms), 2 MB (diagnostic only) | DX100's escape hatch (mega entries, `VX_CFG_L2_TLB_MEGA_SIZE = 8`). Never a fair arm — reported as a separate diagnostic |
| Warps | `W` | 4, 16, 64 per SM | Occupancy sweep |
| SMs | `S` | 4, 8 | "Multiple SMs"; stock default is 1 (`VX_config.toml:4-6`) |

### 6.3 Why `P` has a narrow interesting band (computed before running)

Derived from `VX_types.toml:71-76` (4 KB, Sv39, 3 levels), `VX_config.toml:205-225`, and an 8 B
element. This is an a-priori bound, not a result; V-X2/V-X3 confirm the model agrees.

- A 4 KB page holds **512** 8 B elements. A 16 K-element tile is 128 KB ⇒ **32 pages** if contiguous.
- `f = 512`, `N = 16 K` ⇒ `|A| = 8.39 M` words = **64 MB = 16 384 pages**.
- 16 K uniform draws from 16 384 pages touch ≈ `16384(1−e⁻¹)` ≈ **10 360 distinct pages**.
- Stock UTLB 8 entries ⇒ steady-state hit chance ≈ `8/16384` ≈ **0.05 %**. The 512-entry L2 TLB
  covers 3 %. So ≈ 10 000 walks per tile, each ≥ 3 dependent PTE fetches, against a **16-entry**
  walk cache whose hit chance on random VPNs is ≈ 0.1 %.
- PTE traffic ≈ `10 000 × 3 × 64 B` ≈ **1.9 MB** for **1.05 MB** of data.
- Two walkers, ~3 serialized DRAM-dependent fetches each ⇒ order `10⁶` cycles per tile, against
  ~`4 × 10³` cycles of data time at the derived 102 GB/s peak (§7 V0).

**Conclusion.** At `P ≫ 10³` the workload is a page-walk benchmark, not a memory-parallelism
benchmark: *every* arm collapses and the comparison is uninformative. At `P ≤ 32` everything fits in
a modest TLB and Hypothesis B is trivially nil. **Hypothesis B's honest window is `P ∈ [32, 8192]`**,
and the design concentrates there. This narrowness is a feature: it is why the study stays small,
and it is the main reason B is recommended second (§14).

---

## 7. Updated metrics

Primary outcome is `cycles`; everything else supports attribution. Tracer samples every 64 cycles.

**Common (all hypotheses).**

| Metric | Definition | Collector |
|---|---|---|
| `cycles` | launch → last barrier release | SimX stats |
| `useful_bytes` | 64 × distinct lines the kernel must consume | offline oracle |
| `transferred_bytes` | Σ 64 B responses at the MC | MC stats |
| `efficiency` | `useful / transferred` | derived |
| `bw_achieved`, `bw_util` | GB/s, and vs `channels × 64 B × data_rate` = 102.4 GB/s at 2 ch | derived |
| `outstanding[t]` | lines in flight; mean, p50/p95, integral | [N3] tracer |
| `stall_mshr`, `stall_pending`, `stall_mc` | cycles a source is refused, by cause | tracer |
| `row_hits`, `row_conflicts`, `row_hit_rate` | per channel, from bank-model reservation events | MC stats (`dram_bank_model.h:145-146`) |
| `hit_L1`, `hit_L2` | per level, demand and engine requests **separately** | cache stats |
| `dep_wait` | cycles a warp stalls on a register produced by a pending load | [N3] on the scheduler |
| `result_delivery` | last response → barrier release, plus LMEM beats | engine model |

**Hypothesis A specific.**

| Metric | Definition |
|---|---|
| `gen_lead` | for W1: cycles between "index element `i` available in the engine's tile buffer" and "the corresponding data request issued", minus the SIMT baseline's equivalent. **This is the quantity the hypothesis is about** |
| `addr_gen_cycles` | total S3 cycles, reported separately and included in `cycles` |
| `t_dependency_chain` | index-fill start → last data request issued |
| `dyn_insts` | per-SM dynamic instruction count (the other face of off-core generation) |
| `dups_eliminated` | requests suppressed (ablation only), validated by V5 |

**Hypothesis B specific.** SimX already exposes most of these (`sim/simx/mem/mmu.h:76-79`,
`sim/simx/mem/ptw.h:37`).

| Metric | Definition |
|---|---|
| `tlb_reads/hits/misses/evictions` | per MMU instance: dMMU, **ummu**, L2 TLB. Reported per client, never aggregated |
| `walks_started`, `walk_latency`, `walker_busy` | PTW complex; `walker_busy / 2` is the utilisation of the ceiling |
| `pte_fetches`, `walk_cache_hits` | PTE memory traffic — the number that dominates at large `P` |
| `tlb_stall_cycles` | cycles a lane is refused because the park stage or miss station is full |
| `mshr_occ_tlb` | occupied L1/L2 TLB MSHR entries over time |
| `distinct_vpn_per_launch`, `vpn_dedup_ratio` | `distinct_vpn / accesses` — what a bulk mechanism can exploit |
| `walks_per_fill` | `walks_started / L2 TLB fills` — the amortization the walk cache already gives |
| `prefetch_lead` | cycles a bulk pre-walk completes before the first demand request to that page (mechanism on/off) |

**Primary:** `cycles`. **For A:** `gen_lead` and `outstanding`. **For B:** `tlb_stall_cycles` and
`pte_fetches` (these must move *before* `cycles` does, otherwise the mechanism is not engaged).

---

## 8. Fairness and correctness requirements

Each task requirement, and the rule that enforces it. These are binding on every arm.

| # | Requirement | Enforcement |
|---|---|---|
| **R1** | Same memory bandwidth | Identical channels, data rate, block size, declared peak in every arm. Peak is **derived** (`channels × 64 B × data_rate`), never taken from `VX_CFG_PLATFORM_MEMORY_PEAK_BW = 460 000` (`VX_config.toml:96`), because with `LATENCY = 0` (`:107`) and `QUEUE_SIZE = 0` (`:108`) that number is non-binding (§11 V0) |
| **R2** | Same underlying memory resources | Same dcache, L2, sectoring, MSHR counts, **same TLB sizes within an arm's pair**, same page size, same 2 walkers. Concretely: B2 and B3 differ *only* in the bulk-translation switch; A4 and A5 differ *only* in ordering; A1 and A2 differ *only* in the window. No arm gets a private cache, extra walkers, or extra TLB entries unless its pair-mate gets them too |
| **R3** | No free additional outstanding capacity | Total lines in flight below the LLC set to `L2_MSHR × L2_banks` = `16 × 2 = 32` (A0) or `64 × 2 = 128` (A1–A5, B*, C0). The engine's request table (=128) is *drawn from* that budget, not added to it. Gate V4 checks the leak |
| **R4** | Explicit address-generation accounting | Engine: S1 descriptor fetch, S2 index fill (occupies real channel bandwidth — **the index array is read, not conjured**), S3 `ceil(N / LANES_ADDR)` cycles contending for tile-buffer ports, S4 table update, S5 drain. All charged into `cycles` and reported as `addr_gen_cycles` |
| **R5** | Explicit translation accounting | Bulk translation charges: 1 cycle per VPN CAM probe, 1 per dedup insert, and **every walk it issues is a real walk** competing for the same 2 walkers and the same L2 TLB MSHRs as demand. It gets no private walker and no priority |
| **R6** | No free prefetching or free index fetching | The engine's lookahead exists **only** because it paid R4-S2 to stream `B[i]` into its buffer. The window arms (A2/A3) may not read `B` ahead of the lanes; V6 asserts the A3 request set is a subset of the SIMT-generated set at every cycle |
| **R7** | Correct handling of true data dependencies | W2 (variable ranges) and W3-RMW carry real RAW chains (`H[K[i]]` → loop bound; `A_val[p]` → multiply). The engine must not start element `i+1`'s data request before `i`'s dependency resolves where one exists. W2's range form makes this unavoidable, which is why it is in the matrix |
| **R8** | No violation of memory ordering | Per-lane program order is preserved at the MMU by construction (`VX_mmu.sv:296`, `mshr_order`); the engine issues reads only (`VX_dxa_gmem_req.sv:200`), so RMW variants must order write-after-read through the same barrier the stock path uses. Gate V7 checks final memory state hash against single-threaded reference |
| **R9** | No assumed perfect cache behaviour | Hit rates are *measured*, never set. The engine's scratchpad is an LMEM client that never serves a core request (AS3, from v1, retained). Cold-cache runs reported for W1/W2 |

---

## 9. Per-hypothesis specification

### 9.1 Hypothesis A

1. **Baseline.** A0: stock Vortex, build N, 4–8 SMs, 4 KB pages irrelevant (VM off).
2. **Strong conventional baseline.** A1 (capacity) **and** A2 (realistic 256-entry window) **and**
   A3 (oracle window). Reporting against A1 alone would repeat v1's error.
3. **Proposed mechanism.** S1–S5 with in-order issue (A4); plus row-aware ordering of the engine's
   own batch (A5).
4. **Required model components.** Descriptor RAM (16 slots, mirroring `VX_DCR_DXA_DESC_COUNT`);
   index tile buffer 16 K × 8 B = 128 KB; 16-lane address ALU; request table 128 (from the R3
   budget); per-channel row table 64 entries; LMEM drain at 1 word/cycle.
5. **Independent variables.** `κ` (primary), `ρ`, `N`, `f`, `W`, `S`, address-lane count, table
   update rate.
6. **Metrics.** `cycles`, `gen_lead`, `outstanding`, `bw_util`, `row_hit_rate`, `dep_wait`,
   `dyn_insts`, `addr_gen_cycles` (§7).
7. **Correctness requirements.** R7/R8; output hash equality across all arms for every workload;
   V2 (nothing beats W0); V6 (A2/A3 never see an ungenerated address).
8. **Expected hardware overhead.** Dominated by the tile buffer: 128 KB of SRAM per engine for
   16 K × 8 B, plus ~2.5 KB request table and ~3.5 KB descriptor RAM; the address ALU is small.
   For scale, DX100 reports **4.061 mm² and 777 mW** for the whole accelerator including a 2 MB
   scratchpad (`§5`), which is the number to quote *and* to attribute, not to reuse as an estimate.
   The reason this matters: the tile buffer alone is larger than the entire L1 dcache (16 KB,
   `VX_config.toml:272`), so A's cost is not marginal.

### 9.2 Hypothesis B

1. **Baseline.** B0: build V, SIMT, stock DTLB 16/core, L2 TLB 512, 8 MSHR, 2 walkers.
2. **Strong conventional baselines.** B1 (DTLB 256/core — the "just buy entries" fix at DX100's own
   engine-TLB number) **and** B5 (B1 + oracle window), so that neither capacity nor scheduling can
   be mistaken for the mechanism.
3. **Proposed mechanism.** [N4], three parts, each individually ablatable: (i) **VPN dedup** across
   the tile before issue — one walk per distinct page; (ii) **VPN-ordered walk issue** so
   consecutive walks share interior tables and hit the 16-entry walk cache; (iii) **pre-walk** of
   the next tile's pages while the current tile drains.
4. **Required model components.** A 64-entry VPN CAM + dedup counter; a sorted walk-issue queue;
   a one-tile-ahead page list; taps on the existing `Mmu`/`Ptw` counters. No TLB resize, no extra
   walkers (R5).
5. **Independent variables.** `P` (primary), `m` (4 KB / 2 MB diagnostic), `κ`, `N`, dedup on/off,
   pre-walk on/off, sorted vs FIFO walk issue.
6. **Metrics.** `tlb_*` per instance, `walks_started`, `pte_fetches`, `walk_cache_hits`,
   `tlb_stall_cycles`, `distinct_vpn_per_launch`, `walks_per_fill`, `prefetch_lead`, plus `cycles`.
7. **Correctness requirements.** Identical translated results to B0 (V7 hash); no permission-fault
   behaviour change; a pre-walk for a page that is never touched must not change counted traffic
   beyond its own PTE fetch (it *is* charged); V-X1–V-X3 before any B number is reported.
8. **Expected hardware overhead.** Small: a 64-entry VPN CAM at ~110 bits/entry ≈ 7 kbit, a sorted
   queue of ~64 entries, and a walk-issue FSM — order **1 %** of Hypothesis A's tile buffer. This
   asymmetry is the strongest practical argument for B, and the strongest argument *against*
   building it alone: it is cheap only if the addresses are free, and they are free only because A
   already paid for them.

### 9.3 Hypothesis C

1. **Baseline.** B2 (`E_vm`, engine without bulk translation) — the pair-mate for both mechanisms.
2. **Strong conventional baseline.** B5 (big TLB + oracle window) — the ceiling that any combined
   claim must beat to be about *bulk* anything.
3. **Proposed mechanism.** A5's ordering plus B3's translation, sharing the tile buffer (the
   page list is derived from the same tile the addresses come from — the actual point of the
   hypothesis).
4. **Required model components.** Union of §9.1.4 and §9.2.4, with **one** shared tile buffer.
5. **Independent variables.** Ablation `{A only, B only, both}` × `P` × `κ`.
6. **Metrics.** As A and B, plus the additivity residual `C0 − [C1 + B3 − B2]`.
7. **Correctness requirements.** Same as A and B combined; the shared buffer must not let the page
   list outrun `B[i]` actually being fetched (R6).
8. **Expected hardware overhead.** A's cost plus B's; **not** additive in area if the tile buffer is
   shared, which is the whole engineering argument for combining.

---

## 10. Assumptions and unresolved questions

| # | Assumption / question | Bias | Handling |
|---|---|---|---|
| **AS1** | The engine's per-element table update costs 1 elem/cycle/slice (DX100 does not publish it) | Favours engine | Sweep 0.25–4 in Stage 4 |
| **AS2** | Engine control fetch is nearly free (3 MMIO stores per launch) | Favours engine | Charge S1 explicitly |
| **AS3** | Engine scratchpad never serves a core request | Neutral | Enforced by R2 |
| **AS4** | Engine regions are exclusive; no coherence traffic | Favours engine | Report as an upper bound; DX100 §3.6 needs a coherence agent |
| **AS5** | One LMEM drain port at 1 word/cycle | Favours baseline | Report `result_delivery` separately |
| **AS6** | No bank groups in `dram_bank_model.h` | Favours baseline | Drop DX100's channel+BG interleave claim from the write-up (v1 R2, retained) |
| **AS7** | The A2 window and the A3 oracle are free or cheap to build | **Favours baseline** (a free oracle is unreachable hardware) | Intentional: it makes A falsifiable. A2 is the realistic companion |
| **AS8** | Hand-written kernels, no compiler pass (Vortex has none, *(gap §2.15)*) | Neutral | Same skeleton per arm |
| **AS9** | 4 KB pages everywhere except the diagnostic | Favours baseline in cycles | R2 |
| **AS10** | Warm caches at launch, cold DRAM | Small, arm-dependent | Cold-cache sensitivity on W1/W2 |
| **U1** | **DX100's own evaluation cannot be re-derived** — its baseline is a 32-entry FR-FCFS controller, not this model | Unknown | Do not compare absolute numbers to DX100; compare *mechanisms* |
| **U2** | **Whether a descriptors-only engine can express W2 at all.** DX100 needs `IRMW` (indirect range); Vortex's DXA has no equivalent *(gap §2.2)* | Favours baseline on W2 | W2's engine arm is a *modelled extension*, explicitly flagged; if reviewers object, W2 runs SIMT-only and the claim narrows to W1/W3 |
| **U3** | Whether the ummu path is faithfully modelled (U1–U3 in §2.4) | Unknown | V-X1–V-X3 gate all B results |
| **U4** | Whether `EXT_DXA_ENABLE` can be turned on together with `VM_ENABLE` and PGSLVL 3 without other paths breaking | Unknown | Build check before Stage 0; if it fails, that is a reportable finding, not a workaround |

**Explicitly out of scope:** energy and area (needs synthesis; DX100's 777 mW is quoted, not
simulated), compiler support, multi-cluster scaling, coherence protocol changes, and any claim about
whether DX100's own results are correct.

---

## 11. Validation gates

A failure stops the campaign; results from a model that fails a gate are not reported.

| Gate | Check | Applies to |
|---|---|---|
| **V0** | Make the platform binding: `bw_achieved ≤ 102.4 GB/s` at 2 channels, and W0 latency-sensitive when `LATENCY` goes 20 → 150 | all |
| **V1** | W0 within 5 % of derived peak | all |
| **V2** | Little's law: `mean_outstanding × latency ≈ bytes/cycle` within 15 %; **no arm beats W0** | all |
| **V3** | Ordering sanity: `ρ=0` ⇒ random-row expectation; `ρ=0.9` ⇒ high row hits even in A0 | A |
| **V4** | Invariance: raising the MC queue beyond the request table changes `cycles` < 1 % ⇒ no capacity leak | A, C |
| **V5** | Dedup oracle: `dups_eliminated == N − |unique(B[i])|` exactly | A5+D |
| **V6** | **Enumeration check:** at every cycle, the A3 window's request set ⊆ the SIMT-generated set, and its in-flight count ≤ the R3 budget | A2, A3 |
| **V7** | Correctness: final-memory hash equals the single-threaded reference for every workload × arm | all |
| **V8** | Translation engagement: `tlb_reads()` and `walks_started` are non-zero on the DXA path under VM (U2) | B, C |
| **V9** | Walker ceiling: with `P = 512` streaming, `walker_busy ≤ 2` concurrent, in both models (U3) | B, C |
| **V10** | **Mechanism-first ordering:** in B3, `tlb_stall_cycles` and `pte_fetches` must fall *before* `cycles` does. If `cycles` moves without them, the change came from somewhere else and the cell is invalid | B, C |
| **V11** | Cross-model spot check: one cell in rtlsim vs SimX; disagreements beyond configured link/latency constants are recorded and all absolute numbers become model-relative | all |
| **V12** | Address-map randomization (v1 V6): each cell run with 5 seeds that permute the channel/bank/row map. If the spread exceeds the effect, the result is a map artifact | all |
| **V13** | Positive control (v1 V7): A5 must show a `row_hit_rate` gain over A1 on `ρ=0` data, of the same sign as DX100's reported reordering gain. A model that cannot reproduce the effect qualitatively cannot be used to argue its absence in another arm. (2.7× is DX100's number on a 32-entry FR-FCFS controller, not a prediction here) | A |

---

## 12. Experiment plan (narrow) and pre-registered interpretation

### 12.1 Stages

Deliberately smaller than v1's ≈1 000 runs, because the hypotheses are now separable and each can be
stopped early.

| Stage | Content | Runs |
|---|---|---|
| **0 — Instrumentation validity** | V0–V3, V6, V8, V9 on a single workload each. **No hypothesis is tested here** | ≈ 8 |
| **1 — Hypothesis A go/no-go** | A0–A5 × {W0, W1, W3} × `κ ∈ {0, 0.5, 1}` at nominal `ρ`, 5 seeds | 6 × 3 × 3 × 5 = **270** |
| **2 — A dose-response** | A1, A2, A3, A5 × W1 × `ρ ∈ {0, 0.5, 0.9}` × `N ∈ {1 K, 16 K}` , 3 seeds | 4 × 1 × 3 × 2 × 3 = **72** |
| **3 — A cost sensitivity** | A4, A5 × W1 × lanes {4,16,64} × table rate {0.25,1,4}, 3 seeds | **54** |
| **B-probe (cheap, decides whether B is worth running)** | B0, B2 on W1 at `P ∈ {32, 512, 8192}` — measure `tlb_misses`, `pte_fetches`, `cycles` only | **18** |
| **4 — Hypothesis B** (only if the probe shows translation is material, i.e. `tlb_stall_cycles > 10 %` of `cycles` somewhere in the band) | B0–B4 × {W1, W2} × `P ∈ {32, 512, 8192}` × `κ ∈ {0, 0.5}`, 3 seeds | 5 × 2 × 3 × 2 × 3 = **180** |
| **5 — Hypothesis C** (only if both 1 and 4 produced a non-null effect) | C0, C1, C2, B5 × W1 × `P ∈ {512, 8192}`, 3 seeds | 4 × 1 × 2 × 3 = **24** |

**Total ≈ 620 runs**, of which Stage 0–1 (≈ 280) is the go/no-go. At 1–5 min per SimX run for 8 SMs,
Stage 1 is 5–25 CPU-hours, parallelizable.

### 12.2 Analysis

Paired by seed within a cell (same index array, same address map). Per-workload Δ % and a geometric
mean across workloads, never a bare mean. Batch-means CI over seeds. The only inferential tests that
matter are the pre-registered pairs below.

### 12.3 Pre-registered interpretation

**Hypothesis A.**

| Observation | Conclusion |
|---|---|
| **A5 ≤ A3** (within CI) at every `ρ` | **A refuted.** Independent generation buys nothing that a large clairvoyant window over generated requests does not. Report as a negative result — a strong one, because A3 is unreachable hardware |
| **A4 > A1** but **A5 ≈ A3** | Generation helps only by adding ordering luck; ordering is the real lever. Pivot to the window |
| **A2 ≈ A3** | A realistic 256-entry window already captures the ceiling ⇒ no exotic scheduler needed, and A5 must beat A2 to matter |
| **A5 > A3** and holds across `ρ` and seeds | **A survives.** Investigate the mechanism: is it `gen_lead` (earlier requests) or `dep_wait` (fewer core stalls)? The two imply different papers |
| **Effect vanishes as `κ→1`** | Confirms the mechanism is indirect-address generation (required for a credible A claim) |
| **Effect present at `κ=1`** | Bug signal: at `κ=1` the addresses are affine and the baseline coalesces perfectly. Investigate before claiming |

**Hypothesis B.**

| Observation | Conclusion |
|---|---|
| **B3 ≈ B2** | **B refuted** at stock capacity: bulk visibility does not improve translation when dedup/walk-cache already exist |
| **B1 ≈ B3 > B2** | Translation capacity is the whole story; a bigger TLB is a cheaper answer than a bulk engine |
| **B3 > B1** | **B survives** against the "just buy entries" control. Then S5 must show it also survives against B5 |
| **B4 ≈ B3** | Bulk translation is *not* a capacity substitute — it is orthogonal to TLB size (the interesting, quotable outcome) |
| **B4 > B3** | The mechanisms are partly redundant; report the overlap honestly |
| **Effect only at `P ≫ 10³`** | Suspect: that is the page-walk-collapse regime where every arm is broken (§6.3). Do not report as a translation win |
| **`pte_fetches` unchanged in B3** | Mechanism not engaged; cell invalid (V10) |

**Hypothesis C.**

| Observation | Conclusion |
|---|---|
| `C0 ≈ max(C1, B3)` | **One makes the other unnecessary.** Report which dominates — this is the most useful negative result in the study |
| `C0 ≈ C1 + B3 − B2` (additive) | Independent, composable wins. Modest but clean |
| `C0 >` additive | **Complementarity** — the only outcome that justifies building both. Requires Stage 4 and 5 to both have survived |

---

## 13. Recommendation: test Hypothesis A first

**Order: A → (probe) → B → C.** Three reasons, in order of weight.

1. **A is testable on the default build; B is not.** `EXT_DXA_ENABLE` and `VM_ENABLE` are both
   false by default (`VX_config.toml:22, 42`) and `SOCKET_UNIT_MMU_ENABLE` exists only when both are
   on (`VX_define.vh:91-101`). Build V is therefore the less-exercised combination, and its
   translation path inherits the modelling risk in §2.4 (U1–U3) *before* any B number is
   interpretable. A needs none of that.
2. **If A fails, B cannot rescue the engine's case.** B changes only *when* translations complete.
   If knowing a batch of addresses earlier does not let the engine beat a clairvoyant window over
   *generated* requests (A3), then translating those same addresses earlier cannot either — the
   binding constraint was never translation. The converse is not true: A can win while B is nil
   (the footprint fits the TLB), so A is the strictly wider gate.
3. **B's novelty is thinner and its window narrower.** Every B mechanism already exists in Vortex
   (VPN-dedup miss station, `VX_tlb_mshr.sv:44`; 16-entry walk cache, `VX_ptw.sv:66`) or in the
   literature (PTW scheduling ISCA'18, neighbourhood translation MICRO'18, Valkyrie PACT'20,
   IGS-TLB ICPP'23, DUCATI TACO'19, LATPC MICRO'25). Its only defensible claim is the *free,
   descriptor-derived lookahead*, which is a support section, not a paper. And §6.3 shows the
   mechanism can only pay inside `P ∈ [32, 8192]`, outside which it is either unnecessary or
   swamped.

**Why the probe is nonetheless mandatory.** The one scenario that inverts this ordering is a machine
whose *untranslated* baseline is bandwidth-bound at 102 GB/s while the real target is
translation-bound — the regime the DMP/SoftWalker line of work argues is common. The B-probe
(Stage 3b, 18 runs, measuring only `tlb_misses` / `pte_fetches` / `cycles` on B0 and B2) settles that
in a day. **If the probe shows translation consuming more than 10 % of `cycles` at `P = 512` on
random gather, promote B to run in parallel with A** rather than after it.

---

## 14. Risks and review checkpoints

| # | Risk | Mitigation |
|---|---|---|
| **Q1** | SimX's ummu is unvalidated (U1–U3) ⇒ B uninterpretable | V-X1–V-X3; publish nothing from B until they pass |
| **Q2** | `VM_ENABLE` + `EXT_DXA_ENABLE` + PGSLVL 3 may not build cleanly together (U4) | Build check in Stage 0; a failure is a finding |
| **Q3** | The oracle window (A3) is unreachable hardware, so beating it proves little practically | A2 is co-primary; report A5-vs-A2 alongside A5-vs-A3, and state plainly that beating A3 is the harder claim |
| **Q4** | Engine conservatism (AS1/AS5) may be insufficient for a negative result to be credible | Stage 3 bounds the table cost; if the engine loses, it must lose *despite* 1 elem/cycle/slice |
| **Q5** | The additivity test (C) needs two non-null effects to be meaningful | Run C only if both parents survived (Stage 5 gate) |
| **Q6** | DX100's absolute numbers are not comparable (its controller is a 32-entry FR-FCFS) | Compare mechanisms only; never quote 2.6×/2.0×/3.9×/2.7× as targets |
| **Q7** | W2's engine arm is a modelled extension the DXA cannot express (U2) | Flagged; W2 falls back to SIMT-only if reviewers object |
| **Q8** | Effect could be a DRAM address-map artifact | 5-seed map permutation (V12); report spread |

**Review checkpoints before implementation.**

1. **Platform admissibility** — are the derived 102.4 GB/s, latency 20/60/150 and 32/64-entry MC
   queues accepted as the baseline (V0)?
2. **Is the §5.2 capability ladder the right ceiling?** Specifically: is A2 (256-entry realistic)
   the correct realism point, and is A3's zero-cost clairvoyance acceptable as a bound?
3. **Is `P ∈ [32, 8192]` the right band for B** (§6.3)? If a reviewer believes real indirect gathers
   sit at `P ≪ 32`, B is pre-falsified and should be cut entirely — which would leave a clean,
   single-hypothesis study. That is an acceptable outcome and worth stating.
4. **Is 13 configurations still "narrow enough"?** The minimum defensible set is A0, A1, A3, A4, A5
   (5 arms, Hypothesis A) plus B0, B2, B3 (3 arms, Hypothesis B) = 8. A2, B1, B4, B5, C0 and the
   dedup ablation are the controls that make the other 8 interpretable, and each can be deferred to
   a later stage.

---

*End of revised design. No simulator code written; no RTL modified; `ref/vortex` remains unmodified.
Awaiting review before any implementation.*
