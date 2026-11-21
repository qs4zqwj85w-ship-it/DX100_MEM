# Baseline (SIMT) implementation and validation report

Status: **baseline implemented and validated; engine deliberately not implemented.**
Implements design `standalone-simulator-design.md` §2–§7 up to milestone M4's
prerequisite. Implements: `DX100_MEM/sim/`. Build: `make` (native clang, C++17,
no Vortex, no Docker, no VM). Tests: `./bin/dx100_tests` → **47 checks, 0 failures**.

---

## 1. What is modelled

One shared memory system serves both front-ends from the same trace, so the
future engine comparison is A/B over identical downstream state.

| Component | Structure | Cycle behaviour | Origin |
|---|---|---|---|
| L1 TLB (per core) | 16 entries, 4-way, LRU | 1-cycle lookup | [V] `VX_config.toml:205` |
| L1 TLB MSHR | 4 entries, dedup by VPN, ≤2 parked/entry | fill on walk completion | [V] `:207`, `REPLAY_DEPTH` |
| L2 TLB (shared) | 512 entries, 4-way, LRU, 8 MSHRs, ≤8 reqs/entry | 4-cycle pipeline | [V] `:219-223` |
| Walker | 2 walkers, 16-entry walk cache, Sv32 (2 levels) | 150 cy/level, 300 cy/walk | [A] `:224-225` |
| L1 cache (per core) | 256 lines, 4-way, LRU, 16 MSHRs | 2 cycles | [V] `:272,273,299,290` |
| L2 cache (shared) | 16 384 lines, 8-way, LRU, 128 MSHRs | 12 cycles | [V] `:272,299` |
| DRAM | 16 banks × 1024 rows, 1 KiB rows, FR-FCFS, queue 32 | t_hit 24 / t_miss 56 / link 4 | [A] |
| Front-end | 4 cores × 8 warps × 32 lanes; coalescing; R = 128 slots | ≤8 requests/cycle/core | [A] |

Sv32 is reproduced as designed: 4 B PTEs, 2 levels, PTE lines at
`ROOT_BASE = 0x00200000` and `LEAF_BASE = 0x00400000`, 16 PTEs per 64 B line, PA ≠ VA
(so translation bugs cannot pass silently).

**Counters** (`--all`) cover every quantity §6 requires: requests
generated/issued/completed/unissued, slot residency, per-core TLB
reads/hits/misses/MSHR occupancy, L2 TLB lookups/allocs/dedups/walk stalls, walks,
walk-cache hits, PTE fetches, L1/L2 hits/misses/evictions/MSHR occupancy, DRAM
reads/row hits/row conflicts/queue depth/bytes, plus per-core completed counts.

**Conservation identities** are checked on every run and the process exits non-zero
on violation:

```
generated   == issued + unissued_end            (unissued_end must be 0)
issued      == completed + inflight             (inflight must be 0)
tlb reads   == hits + misses          (per core)
l1/l2/l2tlb reads == hits + misses
completed   == generated
```

A refused probe (MSHR full) is rolled back out of `reads`, so backpressure is
counted in `*_stall_cycles`, never as a translation or cache event.

## 2. Test suite (47 checks)

| Test | Property |
|---|---|
| `config` | key/value parsing, unknown key rejected |
| `solve_M` | round-trips v3 C24: `f(solve_M(N,ρ)) = ρ` to 1e-6 |
| `trace_permutation` | ρ=0 is a true permutation: Λ distinct lines, no duplicates |
| `trace_regular` | κ=1 emits contiguous runs (`kappa_real = 1`) |
| `trace_duplicates` | sampled ρ=0.5 realizes 0.5 ± 0.05 |
| `tlb_capacity` | 2-entry TLB: 2 misses, 2 hits, then eviction forces a re-walk |
| `l1_reuse` | 16 lines fill the L1; the second pass is 16/16 hits |
| `dram_locality` | a bank-strided sweep has strictly more row conflicts than a sequential one |
| `conservation` | all identities hold on a full 4-core run |
| `determinism` | same seed ⇒ identical cycles, walks, DRAM reads |
| `streaming_ceiling` | TLB-resident stream: ≥80 % DRAM row hits, one walk per page |
| `stream_beats_gather` | 4096-line footprint: stream 94 % row hits, gather 15 %; stream strictly faster |
| `walks_analytic` | working set resident in the L2 TLB ⇒ `walks == distinct pages` (5 %), `pte_fetches == 2·walks` |

## 3. Baseline results

W1 (random gather, ρ_dup = 0, κ = 0), N = 16 384 accesses, Λ = 16 384 lines,
4 cores, seed 1:

| `P` | pages | walks | PTE fetches | walk-bound `W/2×300` | measured cycles | ratio |
|---|---|---|---|---|---|---|
| 512 | 512 | 512 | 1 024 | 76 800 | 191 560 | 2.49× |
| 2 048 | 2 048 | 12 684 | 25 368 | 1 902 600 | 1 902 740 | **1.000×** |
| 8 192 | 8 192 | 15 840 | 31 680 | 2 376 000 | 2 376 140 | **1.000×** |
| 16 384 | 16 384 | 16 384 | 32 768 | 2 457 600 | 2 457 740 | **1.000×** |

W1 statistics: L1 hit rate 0 % (as predicted: the footprint exceeds 16 KiB), L2 TLB
max occupancy 8/8, DRAM row-hit rate 1–5 %, and conservation OK in all runs.

**The headline agreement.** For `P ≥ 2048` the measured cycle count equals
`walks / walkers × (levels × L_pt)` to within 140 cycles — the pipeline drain. The
model is *walk-bound*, which is precisely the regime the design's Hypothesis B
depends on. This is the strongest validation available without RTL: an analytic
quantity computed before the simulator ran, met to three significant figures by a
simulation that never knew the target.

**Second finding.** Walks exceed distinct pages by 1.55× at `P = 2048` and 1.93× at
`P = 8192` but are exactly 1.00× at `P = 512` and `P = 16384`. The 512-entry shared
L2 TLB cannot cover a 2 048–8 192-page working set, so pages are re-walked; at
`P = 16384` every page is touched exactly once by the permutation, so no re-walk is
possible. This is a real mechanism, not an artefact, and it changes the B-family
comparison: an engine that reduces *requests* but not *pages* will still pay these
walks.

W0 reference (κ = 1, N = 32 768, Λ = 4 096, `P` = 64): 19 662 cycles for 4 098 lines
(0.21 lines/cycle), **94 % DRAM row hits**, 64 walks (one per page).

## 4. Known gaps (to close before any engine result is reported)

1. **Data-path throughput is below the DRAM bound.** At `P = 512` the run moves
   0.086 lines/cycle against a 0.29 lines/cycle bound for 16 banks at t_miss = 56.
   The limiter is the in-flight population (~18 lines), not bank capacity. Absolute
   cycle counts in the non-walk-bound regime are therefore pessimistic; the *ratios*
   between arms remain valid because both arms share this path, but the cause must be
   found and fixed during M4 calibration rather than explained away.
2. **DRAM is a reservation model, not a full controller.** Row state and bank busy
   time are tracked per request with FR-FCFS ordering inside a 32-entry queue, but
   there is no burst/column modelling and no refresh. Row hits are ~1 per 16 lines in
   a sweep, consistent with the chosen 1 KiB row and 64 B interleave.
3. **Front-end issue is modelled at 8 requests/cycle/core** (one per warp), with
   head-of-line freedom. Real warp scheduling, scoreboarding and register pressure
   are absent.
4. **ρ_dup > 0 is clamped to the Λ-line footprint** when the inverted C24 solution
   asks for more than Λ distinct targets; realized ρ is measured and reported, never
   assumed (v3 F7).
5. **No writes, no atomics, no prefetch** — the baseline gathers only. v3's arms are
   read-only, so this is in scope, but it must be stated in any results table.

## 5. Defects found and fixed during bring-up

Recorded because each one would have silently biased the engine comparison:

| Defect | Effect | Fix |
|---|---|---|
| L2 TLB entry filled only the first requesting core | other cores' MSHRs never retired → permanent deadlock at 4 cores | per-entry core mask; every waiter is filled |
| `regular` workload branch drew a random line per instruction | κ=1 was a scattered pattern, not a stream (97 % DRAM row conflicts) | linear sweep cursor |
| One request/cycle/core | 7 of 8 warps idle, MLP capped at 4 | up to one request per warp per cycle, no head-of-line blocking |
| Single shared L1 | 16 MSHRs for the whole socket instead of per core | private L1 per core (arrays indexed by core) |
| Request polled an MSHR after it had retired | missed completion → permanent stall | completion handed to waiters before the MSHR is recycled |
| Probe refusals counted as reads/misses | `reads == hits + misses` broke | rollback on refusal; stalls counted separately |
| DRAM row mapping | consecutive lines never shared a row | 1 KiB rows, 64 B bank interleave |

## 6. How to run

```sh
cd DX100_MEM/sim
make                 # bin/dx100sim, bin/dx100_tests
./bin/dx100_tests    # 47 checks
./bin/dx100sim --set wl=W1 --set N=16384 --set Lambda=16384 --set P=2048 \
               --set cores=4 --set seed=1 --all
```

Every parameter in `Config` is settable from the command line (`--set k=v`) or a
`key=value` file (`--config`), so the §4.1 sweeps need no rebuild.
