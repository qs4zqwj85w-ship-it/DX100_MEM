# M26 Stage B1 — translation liveness instrumentation and DRAM mapping verification

**Diagnostic stage. No architectural fix was implemented, no timing parameter was changed, and no
cycle guard was altered.** The only source changes are instrumentation, two regression tests and one
test-harness switch; they are listed in §10. Everything below is either a measurement from
`build/wm/m26/b1/` or an enumeration from `tools/m26_b1_pagemap.py`; nothing is inferred from an
aggregate counter alone.

---

## 1. Executive summary

**One defect, one mechanism, both triggers.** In both known Bulk liveness failures the run dies
because a request enters `inflight_` and can never leave it. The trace shows the same transition
missing in every case:

> a request probes the dTLB, **hits** (`ready = cycle + 1`), waits that one cycle, and finds the
> entry **gone**. From then on it re-runs a *lookup* every cycle and never re-issues a probe, so
> nothing in the model can ever retry its translation.

The only writer that can remove a dTLB entry is `tlb_fill` (`memory.cpp:270-290`), on LRU
replacement of the 4-way set. The trace therefore establishes **H2 — the translation is evicted
between the probe's hit and the very next lookup** — without needing a per-set event log: "hit at
C, absent at C+1, no other mechanism can remove an entry" is a proof, not a correlation.

| | trigger 1 (`map3_s3`) | trigger 2 (`cohorts=1`) |
|---|---|---|
| stranded requests | **1** (`id=37937`, vpn 64, client 0) | **3** (`id=8805` vpn 12 client 0; `id=10979`/`10980` vpn 4188 client 2) |
| probe outcome | `probe_hit` at 224 044 | `probe_hit` at 23 596 / 29 853 |
| first missing lookup | 224 045 (one cycle later) | 23 597 / 29 854 |
| failed lookups afterwards | 8 068 933 | 5 340 462 / 5 334 205 |
| any further transition | **none** (`hist n=1`) | **none** (`hist n=1` × 3) |
| run state | workload complete (`desc_completed=1`, 294 912/294 912 elements) | window 1 stalled at 16 382/34 816 |
| ready pool | peak 4 096, empty at the end | **peak 335** |

**The ready pool is not the cause.** Trigger 2 finishes 47 % of its elements with an 8 %-full pool,
and trigger 1's pool is empty when the run stops. The M25 statement that both failures are "a
ready-pool liveness failure" is refuted by the instrumentation; the pool is a victim when its runs'
lines never return, and a bystander when they do.

**Three hypotheses are rejected with numbers, not arguments.** H1: every MSHR retirement pushed a
fill (`life_fill_push = life_walk_done = 94 782`, and `installs ≥ push` per VPN) — no MSHR was
cleared without delivering. H3: `life_xl_rej = 0`, `xlate_overflow = 0`, `xl_enq = xl_deq`
(108 909/108 909 and 6 120/6 120) — no completed translation was dropped, so trigger 2's
`tr_requests − tr_completed = 2` is *not* a hand-off loss; it is two stage-1 requests that never
translated at all. H4: `life_l2tlb_requeue = 0` — the L2 TLB never requeued a probe in either run.

**The defect is not Bulk-specific.** One stranded request in trigger 1 and one in trigger 2 carry
client tag 0 (the conventional SIMT front-end); the other two are PTE probes. The fix belongs in the
shared memory system and must be validated for both arms.

**The DRAM mapping question is settled in M24-A's favour, and M25's correction was wrong.** On
Orkut BFS the identity map places the 2 048 accessed pages on 2 048 distinct frames and **0 shared
cells**; the M3 permutation leaves them 2 048 distinct frames (a bijection cannot collide them) but
scatters them over 2^20 frames, so they alias modulo the 2 048-frame model and produce **4 140
shared page pairs** (max 5 pages per row-cell). M24-A's own sentence — "two pages share a row only if
their page numbers differ by 2048 (8 MiB)" — is exactly what the decoder does. What M25 checked (a
page never shares with its neighbour) is true and irrelevant.

---

## 2. Provenance

| item | value |
|---|---|
| source revision | not a git repository; per-file SHA-256 (first 16 hex) in §10.2 |
| binary | `sim/bin/dx100sim`, SHA-256 `a64cb0255345194b…` |
| configuration | `build/wm/m24d/m24d_bulk.cfg` (M24-D pinned Bulk template) |
| trigger 1 overrides | `dataset=../build/wm/m23_criteo_16k.bin`, `m24_map=3`, `m24_perm_seed=3` |
| trigger 2 overrides | `dataset=../build/wm/m23_criteo_2k.bin`, `page_cohorts=1` |
| diagnostics | `life_diag=1`, `life_dump=build/wm/m26/b1/<cell>_life.txt`, `stall_abort=5000000` |
| cycle guard | **`max_cycles = 500 000 000`, unchanged** in both config and command line |
| traces | `m23_criteo_16k.bin` (294 912 elements), `m23_criteo_2k.bin` (34 816 elements) |
| hardware model | 1 core, 4 warps × 4 lanes, R = 128, L1 16 KiB/4-way, L2 1 MiB/8-way (mshr 64), dTLB 16×4-way (mshr 8, parked 2), L2 TLB 512×4-way, 8 walkers at 150×2 cycles, 2 banks × 4 096 rows × 1 KiB, FR-FCFS, `dram_q = 0` |

**Reproduction check.** The instrumented runs report the same terminal `issued`/`completed` as the
preserved logs: 225 654/225 653 (`m24d_bulk_criteo_map3_s3.txt`) and 21 875/21 873
(`criteo_2k__cohorts_1__bulk.txt`). The historical artifacts themselves were not modified (§11.3).

**Why the runs stop early.** At ~90 k simulated cycles/s a 500 M-cycle run takes about 90 minutes.
`stall_abort` (new key, default **0** = off in every real cell) stops a run once nothing has left
`inflight_` for N cycles while requests are held — a state the model provably cannot leave. Both
diagnostic runs and both tests use it; **the guard is untouched and still applies**. The abort only
shortens the observation window after the stall is already terminal: in trigger 1 the last
retirement is cycle 3 292 976 and the abort fires at 8 292 977, i.e. after 5 000 001 cycles of
nothing.

---

## 3. Instrumentation

### 3.1 Design

Every write is inside `if (life_)`, latched once from `Config::life_diag` in the constructor. With the
key absent, the binary executes the pre-B1 code path; the proof is §3.3.

* **One record per outstanding request** (`ReqLife`), created when the request enters `inflight_`,
  erased when it retires. Its size is the in-flight population (≤ ~200), not the request count, so
  memory is bounded by R plus the translation probes.
* **21 transition kinds** (`LifeEv`) covering the brief's list: `enter`, `probe_hit/join/alloc/
  refused`, `walk_issue`, `walk_done`, `fill_push`, `fill_install`, `fill_evict`, `s1_wait`,
  `s1_miss`, `s1_stale`, `s1_hit`, `xl_enq/xl_rej/xl_deq`, `s2_probe/s2_join`, `retire`,
  `l2tlb_requeue`.
* **Exact per-transition counts** per request (64-bit, never truncated) plus an **8-entry ring** of
  the most recent transitions.
* **Post-onset history**: from the first `s1_stale` cycle, every *state-changing* transition of that
  request is appended to a 32-entry per-request buffer. Retries (`s1_miss`, `s1_wait`) are excluded
  from the buffer but counted exactly — otherwise a per-cycle miss would fill any buffer in 32
  cycles and say nothing new.
* **Per-VPN records** (`VpnLife`, keyed by `(vpn, core)`): probes, joins, allocs, walk completions,
  fills pushed, installs, and how often a waiter actually saw the entry.
* **Aggregate counters** (`life_*`, 29 of them) for every transition, plus `xlate_done_` occupancy
  (current, peak, enqueue/dequeue/reject), parked waiters per core (sum and peak), and
  `requests entering/leaving inflight_`.
* **Termination dump**: with `life_dump` set, one block per outstanding request (id, vpn, core, gen,
  stage, ready, client, seq, stale cycle, counts, ring, post-onset history, per-VPN record), then
  the pages probed but never installed. Ordered by id, so it is deterministic.

### 3.2 Required counters, and where each is produced

| requirement | counter | producer |
|---|---|---|
| `xlate_done_` occupancy / peak | `life_xl_occ_end`, `life_xl_occ_max` | `tick` (each cycle) |
| enqueue / dequeue / reject / overflow | `life_xl_enq`, `life_xl_deq`, `life_xl_rej`, `life_xlate_overflow` | stage-1 xlate branch, `take_xlate`, `xlate_overflow` site |
| walk / MSHR alloc, completion, release | `life_walk_alloc`, `life_walk_done`, `life_walk_release` | `ptw_tick` issue and retire loops |
| fill generation / install / eviction | `life_fill_push`, `life_fill_install`, `life_fill_evict` | `ptw_tick` retire loop, `tlb_fill` |
| per-core parked waiters | `life_parked_sum`, `life_parked_max`, `life_parked_last` | `tick`, summing `TlbMshr::parked` |
| per-(VPN,core) fill/waiter accounting | `VpnLife` fields | `tlb_probe`, `ptw_tick`, `tlb_fill`, stage-1 hit |
| stage-1 retries / stale / re-probes | `life_s1_miss_cycles`, `life_s1_stale_events`, `life_reprobe` | `tick` stage 1 (re-probe is structurally 0: none exists) |
| requests entering / leaving `inflight_` | `life_enter`, `life_leave`, `life_inflight_end` | `submit`, both retire sites |
| outstanding identities at termination | `life_records_end`, the dump | `dump_life` |

### 3.3 Limitations, stated not hidden

1. **Retries are counted, not logged.** After onset the only event is `s1_miss`; the buffer excludes
   it so the one entry that matters (`s1_stale`) is never crowded out.
2. **`VpnLife::fills` counts only fills pushed by an MSHR retirement.** `l2tlb_tick` also pushes a
   fill directly on an L2-TLB hit (`memory.cpp:305`) and that path is not counted. Use `installs`
   (from `tlb_fill`) as the reliable count; the two disagree by exactly that path (vpn 64 shows
   `fills=1`, `installs=14`).
3. **Records of retired requests are dropped.** The dump describes the *outstanding* set, which is
   what a liveness question needs; a request that completed normally leaves only its aggregate
   contribution.
4. **Per-VPN keys carry `core < 8`.** With more cores the key would alias; both runs are single-core.
5. **`cc*.done` still under-counts `xlate_only` requests** (they retire before the stage-2 diagnostic
   site). That is pre-existing behaviour, not introduced here; it is why M25's attribution table had
   62–74 % under client 0.
6. **The dump is a terminal snapshot plus per-request history, not a cycle-by-cycle trace of every
   request.** Nothing in the evidence below needs more: the missing transition is identified exactly.

### 3.4 Inertness proof

`sim/bin/dx100sim` with the instrumentation compiled in and `life_diag` absent reproduces
`build/wm/m24d/m24d_bulk_orkut.txt` **byte for byte** (`diff` empty, 6 130 484 cycles, every one of
the ~840 counters). This was re-checked after the last instrumentation edit.

---

## 4. Test results

| test | config | checks | failures |
|---|---|---|---|
| `m26_liveness_map3_s3` | trigger 1 | 8 | **2** — `r.ok`, `inflight == 0` |
| `m26_liveness_cohorts1` | trigger 2 | 8 | **4** — `r.ok`, `inflight == 0`, `tr_reqs == tr_done`, `desc_completed == 1` |
| pre-existing suite | — | 4 924 | 0 |
| **total** | | **4 940** | **6** |

Both tests reproduce the historical failure exactly; the assertions were **not** weakened and the
guard was **not** raised. What passes in both tests is instructive: `life_enter == life_leave +
inflight` (the trace's own accounting closes), `win_incomplete_max ≤ 1`, `life_xl_rej == 0` and
`life_xl_enq == life_xl_deq`. The failure is precisely the missing retirement, not a bookkeeping
error.

---

## 5. Lifecycle reconstruction

### 5.1 Trigger 1 — `bulk_criteo_map3_s3`

```
REQ id=37937 vpn=64 core=0 gen=224044 stage=1 ready=224045 client=0 seq=30561
    stale_at=224045 last=s1_miss last_cycle=8292977
  counts: enter=1 probe_hit=1 s1_miss=8068933 s1_stale=1
  hist n=1:  224045 s1_stale a=224044 b=64
  vpn probes=51 joins=14 allocs=14 walk_done=1 fills=1 installs=14 seen=78 last=223033
  vpn_ring: 222733:walk_issue(300,1)  223033:fill_push(64,0)
```

| transition | observed |
|---|---|
| 1. enters `inflight_` | cycle 224 044 (`enter`) |
| 2. dTLB probe | **hit** — `ready = 224 045`, a one-cycle lookup (`probe_hit`) |
| 3–5. translation enqueue / MSHR / walk | **none** — it was a hit, so no walk was needed |
| 6. fill generation and installation | the VPN's last fill was pushed at 223 033 and installed (14 installs over the run) |
| 7. `xlate_done_` | not applicable (this is a data request, not `xlate_only`) |
| 8. stage-1 retry | one cycle of waiting, then 8 068 933 failed lookups; **no re-probe exists** |
| 9. retirement | **never** |

**Last successful transition:** the probe hit at 224 044. **Missing transition:** any way back into
the translation path — a re-probe, or a fill for vpn 64 after the eviction. The engine had already
finished: `desc_completed = 1`, `bulk_elems_done = 294 912/294 912`, all 19 windows sealed, last
retirement at cycle 3 292 976. The run then ticks with exactly one request held.

### 5.2 Trigger 2 — `criteo_2k`, `page_cohorts = 1`

Three requests, same shape:

```
REQ id=8805  vpn=12   client=0  gen=23596  ready=23597  hist: 23597 s1_stale
REQ id=10979 vpn=4188 client=2  gen=29853  ready=29854  hist: 29854 s1_stale
REQ id=10980 vpn=4188 client=2  gen=29853  ready=29854  hist: 29854 s1_stale
```

All three `probe_hit`, all three miss one cycle later, none ever moves again. The two client-2
requests are Bulk's own phase-1 translation probes (`seq` 298 and 299 within window 1);
`g6b_tr_reqs − g6b_tr_done = 2` is exactly these two. Their runs therefore never reach the ready
buffer, window 1 can never retire, the second buffer is never freed, and `g6c_admit_idle_cycles =
499 985 362` counts the rest of the run with admission having nothing admissible.

**Why the pool is not the cause:** `peak_ready = 335` of 4 096. The pool looks "full of stuck work"
only in the sense that the work that is there is waiting for translations that will never arrive.

**Why both triggers share one cause:** vpn 4188's own record shows `probes=2 joins=1 allocs=1
fills=1 installs=1 seen=2` — its last install was immediately followed by the two requests that
needed it, both of which found it gone. Recovery from this eviction is possible only if *another*
request for the same VPN re-installs the entry; these are the last two requests for vpn 4188 in the
run, so nothing does.

---

## 6. Hypothesis-by-hypothesis evidence

| hypothesis | verdict | evidence |
|---|---|---|
| **H1** an MSHR is cleared without delivering a fill to every parked core | **rejected** | `life_fill_push = life_walk_done = 94 782` (one fill or more per retirement); `life_walk_release = life_walk_done`; every stranded request's probe was a *hit*, so it never had a parked MSHR at all |
| **H2** a fill is evicted before its waiters observe it | **supported** | all four stranded requests: `probe_hit` at cycle C, first lookup at C+1 misses, and the only writer that can remove a dTLB entry (`tlb_fill`) is LRU replacement; `probe_hit`/`s1_miss` on consecutive cycles is the eviction, directly observed |
| **H3** `xlate_done_` overflows or drops a completed translation | **rejected** | `life_xlate_overflow = 0`, `life_xl_rej = 0`, `life_xl_enq = life_xl_deq` in both runs; the two missing translations in trigger 2 are stage-1 requests that never translated, not completed results that were lost |
| **H4** an L2-TLB requeue fails to preserve the core bit | **rejected** | `life_l2tlb_requeue = 0` and `life_l2tlb_requeue_nocore = 0`; no requeue occurred in either run |
| a fifth mechanism (request dropped from the done list, slot double-free, counter bug) | **not supported** | `life_enter = life_leave + inflight` exactly (334 563 = 334 562 + 1; 27 997 = 27 994 + 3); `issued`/`completed` equal the preserved logs; no request retires twice |

---

## 7. Root cause

**A stage-1 request has no path back to the translation mechanism.** `MemorySystem::tick`
(`memory.cpp:1013-1045`) performs a dTLB *lookup* on every cycle after `ready` and re-queues the
request when the entry is absent; the probe that could start a walk is issued only once, in
`submit`. A dTLB entry is 4-way in 4 sets against 10 670 pages in the Criteo traces, so an entry can
be replaced within the single cycle between a probe hit and the next lookup. When that happens the
request waits forever, holding a residency slot and (for a translation probe) an engine run. Nothing
else in the model can retire it.

Consequences, and why the two triggers look so different:

* **The pool** only appears involved. A window with a stranded run cannot retire, so its buffer is
  never released and the pool drains into it. Trigger 2 shows this with an 8 %-full pool; trigger 1
  shows the end state with an empty one.
* **The longer a VPN's requests run out, the likelier the last one is stranded** — recovery needs a
  *later* install of the same VPN by someone else. That is why the count is 1–3 in 28 000–335 000
  requests rather than zero or thousands.
* **Both arms are exposed.** Client 0 (SIMT) is stranded in both cells. Any M26 measurement taken
  before this is fixed can be distorted in either direction.

**Not established here (and not needed for the conclusion):** *which* fills performed the eviction
in that cycle. If confirmation is wanted, the smallest next step is a per-set fill log
(`set → filling vpn` for the one cycle) — a 16-entry array, not a new experiment. It would not change
§7, which rests on "hit at C, absent at C+1".

**No fix is proposed as implemented.** The shape the evidence supports — re-probing at stage 1 once
the expected fill has passed, which is idempotent in `tlb_probe` and does not move any latency
parameter — is a hypothesis for B2 and is not in the tree.

---

## 8. DRAM mapping: enumeration, and the M24-A / M25 reconciliation

### 8.1 What the source implements

```
pa_page_of_vpn : ppn = 16384 + f(vpn),  f = identity (M0) | 4-round Feistel over 20 bits (M3, seeded)
pa_line_of     : line = (ppn << 6) | (offset_in_page & 63)
dram_bank_row  : bank = (line/il) ^ (line/il >> 6) ^ (line/il >> 12)  % banks
                 row  = ((line/(il*banks)) / ((row_bytes/64)/il))  % rows
```

With `banks = 2`, `rows = 4096`, `row_bytes = 1024`, `il = 1`: `row = (line / 32) % 4096`, so one
row index spans **32 lines = 2 KiB** of address space, a 4 KiB page occupies **2 row indices**
(4 `(bank,row)` cells), and the model's capacity is `2 × 4096 × 1024 = 8 MiB = 2048 pages`.
Because `16384` pages of offset is a multiple of 2048, it cancels modulo the row count:
`row = (2·f + hi) mod 4096`, so **frames `f` and `f + 2048` share both of their rows** — for *any*
bijection `f`, identity included.

### 8.2 Enumeration of the M24-A workload (Orkut BFS, `m23_orkut_bfs.bin`)

1 200 003 accesses, **2 048 distinct pages, spanning pages 0–2 047 = exactly 8.0 MiB = exactly the
model's capacity.** `tools/m26_b1_pagemap.py`, `row_bytes = 1024`:

| mapping | distinct frames | distinct row-cells | cells shared by ≥2 pages | page pairs sharing | max pages per cell |
|---|---:|---:|---:|---:|---:|
| **M0** identity | 2 048 | **8 192** | **0** | **0** | 1 |
| M3 seed 1 | 2 048 | 5 136 | 2 180 | **4 140** | 5 |
| M3 seed 2 | 2 048 | 5 236 | 2 168 | 3 924 | 5 |
| M3 seed 3 | 2 048 | 5 152 | 2 212 | 4 140 | 7 |
| M0 + engine index array (3 073 pages) | 3 073 | 8 192 | 4 100 | 4 100 | 2 |
| M3 s1 + index array | 3 073 | 6 404 | 3 644 | 9 032 | 6 |

A bijection keeps the frames distinct (2 048 → 2 048 in both cases) and *still* changes sharing from
0 to 4 140 pairs, because the permutation pushes frames past 2 048 and the row index wraps.

### 8.3 Row geometry, and why the answer is not universal

Same trace, `row_bytes = 4096` (one row index = 128 lines = 8 KiB; capacity 32 MiB):

| mapping | cells | shared cells | page pairs | max/cell |
|---|---:|---:|---:|---:|
| M0 | 2 048 | 2 048 | 2 048 | 2 |
| M3 s1 | 3 228 | 728 | 1 020 | 4 |

Here identity has **geometric** sharing (two consecutive pages per 8 KiB row), and M3 *reduces* it by
scattering the frames. So "M3 increases sharing" is a property of the 1 KiB-row geometry at 8 MiB
capacity, not of the permutation.

### 8.4 Reconciliation

1. **What the source implements.** As in §8.1: pages are permuted in the *virtual page number*, the
   row index is taken modulo `rows`, and a 2 048-page working set exactly fills the 8 MiB model, so
   the identity map has zero sharing by construction and the permutation converts the frame range
   from `[0, 2048)` into `[0, 2^20)`, which aliases.
2. **What M24-A claimed.** Two statements, both in
   `docs/m24-a-four-way-organization-evaluation.md`: M3 is "the one map that makes two virtual pages
   share a DRAM row" (§1) and "two pages share a row only if their page numbers differ by 2048
   (8 MiB)" (§4). **The second is exactly the decoder's rule and is correct.** The first is correct
   in effect (0 → 4 140 pairs) — but the phrase "makes two virtual pages share a DRAM row" is true
   only because of the modulo, not because the map creates a new geometric relationship.
3. **What M25 concluded.** `docs/m25-dram-organization-audit.md` stated that a page covers whole rows
   "so *two pages can never share a row*" and that M24-A's claim was "not reproducible from the
   decode". **That is wrong**, and this report corrects it: M25 verified adjacency (page *p* never
   shares with *p+1*, still true — §8.2) and then generalised it to all pairs, overlooking the row
   index's modular reduction. The correct statement is: *at 1 KiB rows a page never shares a row with
   an adjacent page; two pages share a row exactly when their frames agree modulo 2 048, which the
   identity map never produces for a 2 048-page set and the permutation produces for 4 140 pairs.*
4. **Is the historical M3 performance result reproducible?** The **measurements** are preserved and
   stand (they are numbers in preserved logs; nothing here changes them). What is now precise is what
   they *mean*: M3 is not "a more realistic physical map", it is a **sharing generator at model
   capacity**. Under M0 the Orkut working set exactly fills the modelled 8 MiB, so each page owns its
   cells and no ordering can help; under M3 the pages interleave on 5 136 cells instead of 8 192,
   which is why the physical-order organizer finds something to do (−1.51 % on Orkut M3) while it
   does nothing under M0 (+0.0002 %). That is an artefact of an 8 MiB DRAM model with an 8 MiB
   working set, **not** evidence about a real GPU's row sharing.

**Classification.** Not an implementation defect (the decoder and the permutation do what they say);
not a reporting error in M24-A (its stated rule is right); **a M25 analytical error** (mine), now
corrected. The M3 experiment is *reproducible* but *not representative*: it should not be cited as
evidence about hardware page-to-row aliasing, and M26's organizer evaluation must use the production
map and the 4 KiB-row geometry (§8.3) rather than M3-at-capacity.

---

## 9. Files changed

| file | change | architectural effect |
|---|---|---|
| `sim/src/sim.hpp` | `Config::life_diag`, `Config::life_dump`, `Config::stall_abort`; `Req::id`; `LifeEv`/`LifeEvRec`/`ReqLife`/`VpnLife`; 29 counters; `life_at`/`life_ev`/`vpn_life`/`dump_life`; `++life_xl_deq` in `take_xlate` | **none** — every write is behind `if (life_)`; the key defaults off |
| `sim/src/memory.cpp` | event posting at the nine lifecycle sites; per-VPN counters; `life_ev`, `vpn_life`, `dump_life`; `<cstdio>` include | **none** |
| `sim/src/util.cpp` | parse `life_diag`, `life_dump`, `stall_abort` (not echoed by `to_string`) | **none** |
| `sim/src/bulk.cpp` | stall-abort check in the two run loops (key default 0) | **none** |
| `sim/src/system.cpp` | stall-abort check in the SIMT loop; one `mem.dump_life(s)` call in `collect_mem_stats` | **none** |
| `sim/tests/tests.cpp` | `m26_liveness_map3_s3`, `m26_liveness_cohorts1` (+ 16 checks) | the two tests fail by design until B2 |
| `tools/m26_b1_pagemap.py` | new: decoder-faithful page enumeration | analysis only |

Not touched: any `.cfg`, any `build/wm/m24*` or `build/wm/m25` artifact, any `docs/m24-*` or
`docs/m25-*`, the cycle guard, any latency, capacity or arbitration parameter, any Vortex file.

---

## 10. Verification

### 10.1 Suite

`./bin/dx100_tests`: **4 940 checks, 6 failures** — the 4 924 pre-existing checks pass unchanged;
the 6 failures are the two new tests' assertions (§4). `FAILURES PRESENT` is the correct status for a
stage that has documented a defect and not fixed it.

### 10.2 Source hashes (first 16 hex of SHA-256)

```
ab.cpp     b8ffacf991824d9c        memory.cpp  6c9b6017f15fe6f6
bulk.cpp   8d8390deea79c0e0        system.cpp  29363fd39a8b290f
sim.hpp    1519f9673ac2f01f        util.cpp    693844d488a78892
workload.cpp 670c7ca9933bf8f2      binary      a64cb0255345194b
```

### 10.3 Unchanged artifacts

`build/wm/m24d/m24d_bulk_criteo_map3_s3.txt` (21 Nov 02:26),
`build/wm/m24g/log/criteo_2k__cohorts_1__bulk.txt` (03:40),
`docs/m24-a-four-way-organization-evaluation.md` (20 Nov 22:38),
`docs/m25-dram-organization-audit.md` (21 Nov 10:55) — all predate this stage and are unmodified.
New outputs exist only under `build/wm/m26/b1/`.

---

## 11. Confirmation

* **No architectural fix was implemented.** No re-probe, no backpressure, no retirement change, no
  ready-pool change, no selector/cache/DRAM change. `life_reprobe` is structurally zero because the
  mechanism does not exist.
* **No cycle guard or timing model changed.** `max_cycles = 500 000 000` in both configs and both
  command lines; `stall_abort` is a separate, default-off harness switch and is not the guard.
* **No Vortex RTL was modified or built.**
* **All instrumentation and test changes are listed in §9** and gated so that a normal run is
  byte-identical to the pre-B1 binary (§3.4).

Logs: `build/wm/m26/b1/{map3s3,cohorts1}_{run,life}.txt`.
