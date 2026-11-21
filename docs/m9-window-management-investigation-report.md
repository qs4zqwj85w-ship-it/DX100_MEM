# M9 window management investigation — E0–E4

**Scope.** This report investigates the interaction between M9's shared request-slot pool
and the Bulk engine's partial-window sealing policy. It is a diagnostic investigation:
the dispatch policy, the Missed architecture, the index-acquisition mechanism, the
scheduler, the request ordering and every published configuration are unchanged. The
only new code is (i) a selectable window-management policy behind a diagnostic config
key whose default reproduces the published machine exactly, (ii) read-only
instrumentation, and (iii) a workload-regularity switch (`bulk_stream`, default off) used
to realise W0 at the published κ grid.

**Headline.** The 64/96/128-slot performance difference is *not* caused by slot capacity.
It is caused by the 4096-cycle partial-seal deadline truncating a window that is still
making progress: at 128 slots the first window reaches only 4 736 of 8 192 elements in
its 4096 cycles, so a 12 376-element operation is cut into **three** windows. The manager
owns only **two** buffers, so windows 1 and 2 are both sealed from cycle 8 779 to 87 823
and no buffer can accept elements: the address generator stops for 79 043 cycles, the
index ring backs up (66 ready lines on average), and the run loses 27 894 cycles
(18.2 %) to an idle DRAM and an idle generator. Making the deadline measure *waiting*
rather than *filling* — policy E3 — removes the effect entirely at the **published**
4096-cycle bound, on every workload and every capacity tested, and leaves only a
≤1.1 % residual capacity effect that is also present in runs with a single window.

The same change **costs** 5.8 % on the streaming workload W0 at κ = 0.25 (4/4 seeds), so
larger windows are not inherently better; there is a real window-size/latency trade-off
and this report measures it rather than assuming a side.

---

## 1. What is measured, what is not changed

| | |
|---|---|
| Published M9 configuration | `R = 128`, `batch_window = 8192`, `batch_deadline = 4096`, `org_overlap = 1`, `idx_mode = 2`, `idx_reserve = 8`, `page_sched = 1`, `ag_lanes = 8`, `eng_issue = 4` — **all unchanged** |
| New config keys | `win_policy` (0–3, default 0), `win_abs_cap` (default 4), `bulk_stream` (default 0) |
| Default behaviour | `win_policy = 0` executes the pre-existing decision path byte for byte; §5.1 shows the published cycle counts reproduce exactly |
| Instrumentation | read-only; one record per window at the four events that define a window (allocation, admission, seal, release) plus per-cycle flags already present from the §9.18 capacity study |
| Not changed | dispatch, classification, compaction, descriptor handling, index acquisition, page scheduling, arbitration, slot pool, caches, TLB, DRAM scheduler, address map, memory hierarchy |
| Not done | no tuning per κ or per workload, no change to defaults, no new allocation policy proposed as the winner |

All runs are `experiment = m9diag`, which runs three arms over one materialised trace:
**A** = SIMT only, **B** = bulk only over the whole trace (the M8 anchor), **C** = the
hybrid dispatched by the class map. The subject of this investigation is arm **C**,
because that is where SIMT and Bulk share the slot pool.

---

## 2. The implementation, as it stands

Everything below is the pre-existing M6/M7/M8 window manager. Line numbers refer to
`DX100_MEM/sim/src/` at the time of this report.

### 2.1 Storage and cursors

```cpp
struct Batch {                     // sim.hpp:904
  u8 state;                        // 0 empty, 1 filling, 2 issuing
  std::vector<WinEnt> ent;         // admitted elements (line, element, offset, width)
  std::vector<u32>  sorted;        // entry ids ordered by (va_line, id)
  std::vector<LineReq> lines;      // one request per distinct line (dedup)
  std::vector<u32>  order;         // indices into `lines`, in issue order
  std::vector<u64>  dir_vpn, dir_mask;   // page directory (D entries)
  std::vector<Cohort> coh;         // M7 page runs
  u32 issued_n, n_ent, n_pages, pending, retired;
  u64 fill_start;                  // cycle the buffer was adopted for filling
};

std::array<Batch, 2> bat_;         // sim.hpp:963  -- exactly two buffers
int fill_ = -1, iss_ = -1;         // one filling cursor, one issuing cursor
bool seal_pending_ = false;
```

Two buffers, one `fill_` cursor, one `iss_` cursor. `state` never takes the value 3 in the
current code; a sealed window goes 2 → (released) → 0.

### 2.2 Allocation — and where the deadline clock actually starts

`BulkEngine::win_tick` (`bulk.cpp:1347`) performs, in this order: organiser progress (1);
release fully retired windows (2); adopt a sealed window for issue (3); **adopt a buffer
for filling (4)**; submit queued stream requests, issue the adopted window, fan out (5);
keep the index stream moving and admit into the window (6); **seal** (6, same block);
occupancy accounting (8); descriptor completion (10).

Step (4), `bulk.cpp:1375`:

```cpp
if (fill_ < 0 && active_ && run_.phase == 1 && run_.gen_done < run_.d.count) {
  const bool no_overlap = !cfg_.org_overlap || cfg_.win_policy == 1;
  for (int i = 0; i < 2; ++i) {
    if (i == iss_) continue;                        // never the issuing buffer
    if (bat_[i].state != 0) continue;               // only an empty buffer
    if (no_overlap && iss_ >= 0) break;             // double buffering off
    fill_ = i;
    nb.state = 1;
    nb.fill_start = cycle;      // <-- THE DEADLINE CLOCK STARTS HERE
    nb.last_admit = cycle;      // <-- and, for E3/E4, here
    break;
  }
}
```

**The deadline is anchored at buffer allocation, not at the first element, and not at any
element-arrival event.** The buffer is adopted on the first tick in which the generator is
runnable (descriptor fetched, `phase == 1`, elements left) and no other buffer is
available, whether or not a single element can be admitted at that instant. On W1 at κ = 0.25 / 128
slots the three windows of the control are adopted at 586, 4683 and 87 823 and their mean
`allocation → first admission` delay is 393 cycles (max 1 181) — the last window waits
longest, because the generator is blocked while both buffers are sealed. A window that
receives nothing is sealed by the descriptor-end rule, not by the deadline.

### 2.3 Admission

`BulkEngine::win_admit` (`bulk.cpp:918`) runs at most `ag_lanes = 8` admissions per cycle
into the buffer named by `fill_`, provided `bat_[fill_].state == 1`. For each element it
calls `idx_take` (`bulk.cpp:712`), which returns the value only when its index line has
actually arrived; on failure it increments `idx_wait_stall` and **breaks out of the loop**
(the generator has no earlier ready element), leaving the window partially filled. On
success it computes `addr = data_base + v * data_bytes` and calls `win_admit_one`
(`bulk.cpp:902`), which appends the entry and fails when `n_ent >= batch_window`, setting
`seal_pending_`.

So *window filling is gated only by index readiness, by the element budget W, and by the
organiser* (§2.6) — never directly by slot availability. Slots gate **issue**, and issue
gates **release**, which is the indirect link (§2.5).

Step (6) admits only if `org_backlog <= 2 * org_lanes`, i.e. when the single organiser
engine (4 entries/cycle) has not fallen behind; otherwise the cycle is counted in
`win_full_stall` and admission is skipped. The organiser is charged one visit per admitted
entry and one more per entry at seal (the line-ordered pass in `win_seal`).

### 2.4 Sealing, and the precedence between reasons

`bulk.cpp:1441`:

```cpp
const bool full  = b.n_ent >= cfg_.batch_window;
const bool ended = active_ && run_.phase == 1 && run_.gen_done >= run_.d.count;
const u64  D     = cfg_.batch_deadline;
const bool age   = D && b.n_ent > 0 && cycle - b.fill_start >= D;     // E0/E1/E2
const bool idle  = D && b.n_ent > 0 && cycle - b.last_admit  >= D;    // E3/E4
const bool cap   = D && b.n_ent > 0 && cycle - b.fill_start >= cfg_.win_abs_cap * D; // E4
if (seal_pending_ || full) win_seal(cycle, 0);   // full
else if (ended)            win_seal(cycle, 1);   // descriptor exhausted
else if (late)             win_seal(cycle, 2);   // partial (deadline / policy)
```

Precedence is **full > descriptor end > partial** and is identical in every policy. Note
`n_ent > 0` in all three deadline terms: an empty window is never sealed *by the deadline*,
but an empty window **is** sealed by `ended` — this path existed before this investigation
and was reached every cycle after the generator was exhausted (§4.1). `win_seal`
(`bulk.cpp:997`) discards an empty buffer, otherwise it sorts the window by line, dedups
into `LineReq`, decides the issue order (page order unless `page_order = 0`), builds the
page directory and page-run cohorts, charges the organiser, and sets `state = 2`.

### 2.5 After sealing: issue, drain, release

Step (3) adopts the first sealed buffer into `iss_`. `win_issue` (`bulk.cpp:1266`) or its
M7 cohort form `win_issue_sched` (`bulk.cpp:1217`) issues up to `eng_issue = 4` lines per
cycle from that one buffer, in sealed order, stopping on the first residency refusal
(`win_emit`, `bulk.cpp:1119`) or dTLB refusal. A fill (`on_line`) retires the dependent
elements through the fan-out queue at `fanout_lanes = 4`/cycle, comparing each reconstructed
address against the index image (`addr_mismatch`).

Step (2), `bulk.cpp:1361`, releases a buffer only when

```cpp
b.state == 2 && b.issued_n >= b.order.size() && b.pending == 0 && b.retired >= b.n_ent
```

i.e. every line issued, every fill returned, every element retired. Until then the buffer
is **occupied**: it is not available for filling.

**A sealed window does drain while another fills.** Step (4) skips only `iss_`, so with
`org_overlap = 1` (the default and the published value) a second buffer fills while the
first is issuing. Measured: 105 006 of 153 475 cycles (68 %) have two non-empty buffers at
κ = 0.25, `win_policy = 0`. What the manager does **not** do is decouple "sealed" from
"occupying a buffer" — a sealed window holds its buffer until fully retired, including its
fan-out tail.

### 2.6 How shared-slot refusals affect window progress

`win_emit` refuses with `free_count() <= tgt_reserve()` (`idx_reserve = 8` while the tiled
index unit is active) and the issue loop breaks for that cycle. This does not touch
admission. The chain is: refusal → slower issue → later release → later buffer availability
→ later adoption of the next window. It is a *second-order* path and is quantified in
§5.8 (7 693 pool-empty refusals at κ = 0.25 in the control, 5 275 under E3).

### 2.7 Can multiple incomplete windows exist?

No. Two independent structural facts:

1. `fill_` is a single integer cursor. Step (4) assigns it only when `fill_ < 0`, and
   step (6) seals the window `fill_` names (setting `fill_ = -1`) before the next tick can
   adopt another. No tick can therefore admit into two buffers.
2. The 2-buffer array bounds *live* windows at 2, one filling and at most one sealed.

Measured on all 150 cells: `peak simultaneously unsealed windows = 1` everywhere, and the
per-window records (Appendix A) show windows in strict allocation order with no
interleaving of admissions.

### 2.8 E2 and E3 as specified are not operationally distinct — and what was implemented

* **E2** asks for at most one incomplete window, with a sealed window free to drain while
  the current one fills. The first clause is already an invariant (§2.7); the second clause
  is the existing `org_overlap = 1` behaviour. Literal E2 ≡ E0.
* **E3** asks to fill the oldest incomplete window first and not to start a new one until
  the current is full or sealed. Both are already the implementation: there is exactly one
  incomplete window and it is the oldest by construction. Literal E3 ≡ E0.

Running E2/E3 as literally specified would have produced three identical columns and no
information. Two refinements were therefore implemented, each the *only* remaining degree
of freedom in the direction the policy names, and each selectable separately:

* **E2 (implemented) = one live window.** Since the number of *incomplete* windows cannot
  be reduced, the remaining way to concentrate elements into fewer windows is to forbid a
  second *live* window: no buffer is adopted until the previous one has fully retired
  (`no_overlap`, `bulk.cpp:1387`). This removes the fill/drain overlap that E2's own text
  permits, and it halves the window SRAM because one buffer suffices
  (`bulk_window_bytes`: 147 584 → 73 792 B). It is reported as E2 with that deviation
  stated explicitly.
* **E3 (implemented) = sequential filling with an admission-anchored deadline.** The
  sequentiality needs no code; what the policy can still decide is **which cycles the
  anti-starvation bound counts**. E0 counts from allocation, so a window is truncated
  *while it is still filling at full rate*. E3 counts from the last admission
  (`b.last_admit`, re-armed by every admission), so the bound measures **waiting**, not
  filling. Fill/drain overlap stays enabled, exactly as E3's text requires ("once a window
  is sealed, allow it to drain while the next window begins filling").
* **E4** = E3 plus an absolute ceiling of `win_abs_cap × D = 4 × 16384 = 65 536` cycles on
  a window's total age, so that "never wait indefinitely for a full window" is structural
  even for a trickling generator.

`win_policy` therefore means: **0** = E0 published · **1** = E2 one live window · **2** =
E3 admission-anchored deadline · **3** = E4 (E3 + ceiling). **E1** needs no code: it is
`win_policy = 0` with a different `batch_deadline`.

---

## 3. Policies as run

| Policy | `win_policy` | `batch_deadline` | Fill/drain overlap | Deadline counts from | Buffers | Window SRAM |
|---|---:|---:|---|---|---:|---:|
| **E0** control | 0 | 4096 | yes | allocation | 2 | 147 584 B |
| **E1** | 0 | 8192 / 16384 / 65536 / 100 000 000 | yes | allocation | 2 | 147 584 B |
| **E2** | 1 | 4096 | **no** | allocation | **1** | **73 792 B** |
| **E3** | 2 | **4096 (unchanged)** | yes | **last admission** | 2 | 147 584 B |
| **E4** | 3 | 16384 | yes | last admission + 4×D total-age ceiling | 2 | 147 584 B |

E1's control point (`4096`) is E0 itself; E4's deadline choice is justified in §5.2 —
beyond 8192 no deadline value changes anything in the W1 matrix, so 16384 is the first
point at which the bound is provably inactive and the ceiling is the only remaining
difference.

---

## 4. Method

### 4.1 One genuine bug found by the new records (and fixed inertly)

The first instrumented run reported **33 864 "discarded empty" windows** per cell. Cause:
step (4) adopted a buffer whenever the generator was *runnable*, without checking that it
still had elements. After the last element was generated, `ended` was immediately true, so
step (6) discarded the empty window and the next tick adopted another — a buffer churn that
had always existed and was invisible because it changed no decision. Guarding adoption with
`run_.gen_done < run_.d.count` removes it, and is inert: an empty window accepts nothing and
is discarded in the same tick either way. The published cycle counts are identical before
and after the guard (§5.1), and `windows adopted = windows sealed` now holds.

### 4.2 Configuration

Identical for every cell except the policy columns above:

```
configs/ab_controlled.cfg
N=16384 Lambda=16384 P=512 rho_dup=0 elem_bytes=4 seed=1
bulk_wl=E0 bulk_index_base=8388608 bulk_desc_line=98304 bulk_data_base=0
batch_window=8192 page_dir=512 org_lanes=4 fanout_lanes=4 org_overlap=1 dedup=1 page_order=1
page_sched=1 page_cohorts=2 park_retry=1 idx_mode=2 idx_buf_lines=64 idx_mshr=64 idx_reserve=8
ag_lanes=8 eng_issue=4 mem_path=l1 R=128 pool_slots=<64|96|128> max_cycles=200000000
experiment=m9diag
```

Workloads: κ ∈ {0.25, 0.50, 0.75} is the class mix **and** the regularity of the trace in
this generator (a "regular" instruction is a contiguous run). Because the request asks for
κ and W0/W1 as independent axes, W0/W1 are realised as **trace regularity at fixed κ**:
`bulk_stream = 0` is the published W1 (primary-class elements are a permutation of the line
set) and `bulk_stream = 1` is W0 (the same line set, same element count, same seed, walked
in order). With `bulk_stream = 0` — every published run — the generator is unchanged.

Secondary capacity runs use `pool_slots` ∈ {64, 96, 128}. Seeds 1–4 are run for the four
policies that matter (E0, E1@8192, E3, E2).

### 4.3 Reproducibility

```bash
cd DX100_MEM/sim
make -j4                                    # no warnings
./bin/dx100_tests                           # 4032 checks, 0 failures
bash  build/wm/run_matrix.sh                # 81 cells, ~1 min
bash  build/wm/run_seeds.sh                 # 72 cells, seeds 2-4
python3 build/wm/extract.py 'build/wm/*_k*_p*.txt' > build/wm/matrix.csv
```

150 cells were run in total; **0 gate failures** (`M9 GATES PASSED` in every cell).

---

## 5. Results

All cycles below are arm **C** (hybrid) unless stated. `A` (SIMT-only) is 220 123 /
169 826 / 120 110 cycles at κ = 0.25/0.50/0.75 and **identical in every policy** — the
policy cannot affect the SIMT-only arm, which is one check that the new code is inert
outside the window manager.

### 5.1 E0 reproduces the published M9 results exactly

| κ | A | B | C | published C |
|---:|---:|---:|---:|---:|
| 0.25 | 220 123 | 145 032 | **153 475** | 153 475 |
| 0.50 | 169 826 | 131 199 | **119 798** | 119 798 |
| 0.75 | 120 110 | 118 689 | **95 194** | 95 194 |

Bit-identical, before and after the empty-adoption guard, with all instrumentation
compiled in. `dx100_tests`: 4032 checks, 0 failures. The A-arm completion hash is
3510628737 in all four policies.

### 5.2 E1 — longer partial-seal deadline (W1, 128 slots)

| `batch_deadline` | κ = 0.25 | κ = 0.50 | κ = 0.75 |
|---:|---:|---:|---:|
| **4096 (E0)** | **153 475** | **119 798** | **95 194** |
| 8192 | 125 581 (−18.2 %) | 94 035 (−21.5 %) | 95 194 (0) |
| 16384 | 125 581 | 94 035 | 95 194 |
| 65536 | 125 581 | 94 035 | 95 194 |
| 100 000 000 | 125 581 | 94 035 | 95 194 |

Two facts: the whole no-deadline benefit is recovered at **8192**, and **nothing** changes
beyond it. The deadline is not a tuning knob with a smooth trade-off here — it is a cliff
at the point where the bound stops firing before the window fills. W1's first window needs
7 614 cycles to reach 8 192 elements at 128 slots, so any bound ≥ that is inactive; a bound
of 4 096 truncates it at 4 736.

The pure-bulk arm shows the same effect, damped: B = 145 032 → 142 076 (8192) → 138 924
(16384) at κ = 0.25. B still truncates at 8192 because it fills more slowly without SIMT
competition; the hybrid is where the effect is large.

### 5.3 E2 — one live window (W1, 128 slots)

| κ | E0 | E2 | Δ | E2 live windows | fill blocked by policy |
|---:|---:|---:|---:|---:|---:|
| 0.25 | 153 475 | 147 616 | **−5 859 (−3.8 %)** | 1 | 81 045 cy |
| 0.50 | 119 798 | 120 505 | **+707 (+0.6 %)** | 1 | 86 314 cy |
| 0.75 | 95 194 | 95 194 | 0 | 1 | 0 |

E2 recovers about a fifth of the available gain at κ = 0.25 and is a small **regression** at
κ = 0.50, despite using one buffer instead of two. The mechanism is measured, not inferred:
`win_fill_blocked` (cycles in which the generator had elements and a buffer was free but the
one-live-window rule forbade adoption) is 81 045 / 86 314 — the policy *recreates* the
symptom it was meant to remove, at the other end of the window's life. Average window size
does rise (4 125 → 6 188 at κ = 0.25) because the second window no longer competes for slots
while the first drains, but the serialization delays the next window's start until the
previous one has fully retired (κ = 0.50: second window adopted at 90 997 instead of 4 683),
which moves the same work into a longer tail.

### 5.4 E3 and E4 — sequential filling with a progress-aware bound (W1, 128 slots)

| κ | E0 | E3 (D = 4096) | E4 (D = 16384) | E1 @ 8192 |
|---:|---:|---:|---:|---:|
| 0.25 | 153 475 | **125 581 (−18.2 %)** | 125 581 | 125 581 |
| 0.50 | 119 798 | **94 035 (−21.5 %)** | 94 035 | 94 035 |
| 0.75 | 95 194 | 95 194 | 95 194 | 95 194 |

**E3 with the published 4096-cycle bound reaches the no-deadline result exactly**, and its
per-window records are byte-for-byte those of E1@8192 (Appendix A). E4 is **identical to E3
in all 18 cells**: the 65 536-cycle absolute ceiling never binds, so its extra bound is
measurably inert and E4 offers no better balance than E3 — there is no latency to trade
against, on these workloads, once the deadline measures waiting.

### 5.5 Window behaviour (W1, 128 slots, seed 1)

| | E0 κ.25 | E1@8192 κ.25 | E2 κ.25 | E3 κ.25 | E0 κ.50 | E1/E3 κ.50 |
|---|---:|---:|---:|---:|---:|---:|
| windows allocated / sealed | 3 / 3 | 2 / 2 | 2 / 2 | 2 / 2 | 3 / 3 | 2 / 2 |
| sealed full (8 192) | 0 | 1 | 0 | 1 | 0 | 1 |
| sealed partial (deadline) | 2 | 0 | 1 | 0 | 2 | 0 |
| sealed at descriptor end | 1 | 1 | 1 | 1 | 1 | 1 |
| elements: mean / median | 4 125 / 3 968 | 6 188 / 4 184 | 6 188 / 4 736 | 6 188 / 4 184 | 2 753 / 3 504 | 4 130 / 68 |
| elements: min / max | 3 672 / 4 736 | 4 184 / 8 192 | 4 736 / 7 640 | 4 184 / 8 192 | 20 / 4 736 | 68 / 8 192 |
| % windows reaching full | 0 % | 50 % | 0 % | 50 % | 0 % | 50 % |
| allocation → seal: mean | 3 572 | 6 529 | 3 569 | 6 529 | 2 731 | 4 852 |
| allocation → first admission: mean | 393 | 1 024 | 0 | 1 024 | 393 | 1 024 |
| first → last admission: mean span | 3 161 | 5 505 | 3 567 | 5 505 | 2 288 | 3 828 |
| peak simultaneously unsealed | 1 | 1 | 1 | 1 | 1 | 1 |
| peak simultaneously live (any state) | 2 | 2 | **1** | 2 | 2 | 2 |
| cycles with two live windows | 115 553 | 79 856 | **0** | 79 856 | 113 661 | 82 836 |
| **cycles with work but no filling window** | **79 043** | **0** | **81 045** | **0** | **82 887** | **0** |
| model window SRAM | 147 584 | 147 584 | 73 792 | 147 584 | 147 584 | 147 584 |

The last row is the whole result. The window manager is not short of *elements* (the
generator always has work) and not short of *slots*; it is short of a **buffer that is
allowed to accept** them, and that shortage is created by the seal, not by capacity.

Full distribution (fill-size histogram, 1 024-wide buckets, W1 128 slots):

| bucket | E0 κ.25 | E1/E3 κ.25 | E2 κ.25 | E0 κ.50 | E1/E3 κ.50 |
|---|---:|---:|---:|---:|---:|
| [0,1024) | 0 | 0 | 0 | 0 | 0 |
| [1024,2048) | 0 | 0 | 0 | 0 | 0 |
| [2048,3072) | 0 | 0 | 0 | 0 | 0 |
| [3072,4096) | 1 | 0 | 0 | 1 | 0 |
| [4096,5120) | 1 | 1 | 1 | 1 | 0 |
| [5120,6144) | 0 | 0 | 0 | 0 | 0 |
| [6144,7168) | 0 | 0 | 0 | 1 | 0 |
| [7168,8192) | 0 | 0 | 1 | 0 | 0 |
| [8192 full] | 0 | 1 | 0 | 0 | 1 |
| (68-element tail) | – | – | – | – | 1 |

### 5.6 Cycle-state distribution (C arm, κ = 0.25, engine-active cycles, mutually exclusive)

States are the §9.18 classification: **A** capacity free and no ready index, **B** ready
index but target capacity refused, **C** capacity free with a sealed line waiting and no
issue (C1 dTLB, C2 no filling window, C3 other), **D** target issued, **E** index moved with
targets outstanding and none issued, **F** neither moved.

| state | E0 | E1@8192 | E2 | E3 |
|---|---:|---:|---:|---:|
| A | 0 | 0 | 1 462 | 0 |
| B | 63 494 | 36 614 | 54 157 | 36 614 |
| C | 75 503 | 73 333 | 76 175 | 73 333 |
|   C1 dTLB | 72 790 | 72 193 | 75 160 | 72 193 |
|   C2 generator had work, no window | 663 | 1 | 509 | 1 |
|   C3 other | 2 050 | 1 139 | 506 | 1 139 |
| D | 8 701 | 6 345 | 8 467 | 6 345 |
| E | 0 | 0 | 0 | 0 |
| F | 5 776 | 9 288 | 7 354 | 9 288 |
| generator had elements, no window accepting | **79 045** | **1** | **81 046** | **1** |

E1 and E3 produce identical state distributions, cycle for cycle — the two policies converge
on the same execution here. Note that A = 0 and E = 0 in every policy: index *acquisition*
never runs out of target capacity, so the improvement is not an index-pipeline effect.
(`win_fill_wait` and the per-cycle C2/no-fill count differ by 1–2 cycles because one is
counted inside the adoption step and the other at the cycle boundary.)

### 5.7 Index acquisition (C, W1, 128 slots)

| metric | E0 κ.25 | E1/E3 κ.25 | E2 κ.25 | E0 κ.50 | E1/E3 κ.50 |
|---|---:|---:|---:|---:|---:|
| index lines submitted | 774 | 774 | 774 | 517 | 517 |
| index elements consumed | 12 376 | 12 376 | 12 376 | 8 260 | 8 260 |
| AG stalls (all) | 7 333 | 8 971 | 4 791 | 5 615 | 6 420 |
| AG stalls with target capacity free | 0 | 0 | 1 462 | 0 | 0 |
| mean ready index elements | 1 049.7 | 20.4 | 1 028.4 | 22.5 | 2.9 |
| mean index lines ready | 65.6 | 1.3 | 64.3 | 1.9 | 0.2 |
| mean index lines in the ring | 70.9 | 7.8 | 73.7 | 7.1 | 5.9 |
| peak index lines ready | 128 | 123 | 118 | 72 | 33 |
| page walks | 539 | 526 | 555 | 522 | 522 |
| last index line completed (cycle) | 90 347 | 13 645 | 88 769 | 8 812 | 8 527 |

The index stream is never slower under E0 — at κ = 0.25 it is **ahead of the consumer**: the
ring sits at 70.9 of its 128 lines with 1 050 ready elements, because address generation has
stopped and acquisition is flow-controlled by consumption. E3 turns it into a pass-through
(7.8 lines, 20 elements), and the last index line then completes at 13 645 instead of
90 347 — a *consequence* of the generator no longer stalling, not of faster fetches: the
index line count is identical (774) and page walks move 539 → 526.

At κ = 0.50 the same table looks flat (ring 7.1 vs 5.9 lines, last index 8 812 vs 8 527)
because there the operation is only 8 260 elements, so the generator finishes consuming the
index array at cycle 8 812 before the window shortage can back the ring up. The shortage is
still there — 82 887 cycles with work and no window — but it shows up as an idle generator
with an empty ring rather than as a full one. This is why the index columns must not be read
as the cause at one κ and as irrelevant at another: they are both consequences of the same
buffer shortage, and their sign depends on where the peak of accumulated unconsumed index
data happens to sit.

### 5.8 Shared request pool (C, W1, 128 slots)

| metric | E0 κ.25 | E1/E3 κ.25 | E2 κ.25 | E0 κ.50 | E1/E3 κ.50 |
|---|---:|---:|---:|---:|---:|
| pool slots | 128 | 128 | 128 | 128 | 128 |
| peak SIMT residency | 128 | 128 | 128 | 128 | 128 |
| peak bulk residency | **128** | 120 | 120 | 120 | 116 |
| peak total outstanding | 128 | 128 | 128 | 128 | 128 |
| target refusals: pool empty | 7 693 | 5 275 | 5 714 | 15 226 | 12 200 |
| target refusals: index reserve | 63 059 | 35 561 | 55 393 | 29 129 | 2 619 |
| conservation violations | 0 | 0 | 0 | 0 | 0 |

E0 needs *more* of the pool to finish later: it saturates all 128 slots while producing
fewer completed requests per cycle. Slot pressure is a consequence of the stall (targets
queue behind a sealed window that cannot be replaced), not its cause — E2 shows the
independence directly: it removes slot competition between windows (`refusals: index
reserve` stays high at 55 393) and gains almost nothing.

### 5.9 Memory scheduling (C, W1, 128 slots)

| metric | E0 κ.25 | E1/E3 κ.25 | E2 κ.25 | E0 κ.50 | E1/E3 κ.50 |
|---|---:|---:|---:|---:|---:|
| target lines issued | 12 376 | 12 376 | 12 376 | 8 260 | 8 260 |
| DRAM reads | 13 609 | 13 589 | 13 475 | 9 900 | 9 876 |
| DRAM row hits | 10 470 | 11 383 | 11 351 | 7 842 | 8 667 |
| row-hit rate | 76.9 % | 83.8 % | 84.2 % | 79.2 % | 87.8 % |
| DRAM starved cycles | **33 353** | **11 665** | 34 435 | 30 990 | 10 763 |
| mean / peak queue depth | 9 / 16 | 11 / 16 | 9 / 16 | 8 / 16 | 10 / 16 |
| mean candidates per decision | 6 | 7 | 6 | 6 | 6 |
| mean targets in flight | 55 | 46 | 52 | 34 | 16 |
| dedup merged elements | 0 | 0 | 0 | 0 | 0 |
| sealed lines unissued at end | 0 | 0 | 0 | 0 | 0 |

Row locality improves (76.9 → 83.8 %), but it is **downstream**: fewer targets are in
flight (55 → 46), which is exactly what reduces row conflicts, and the run gets faster
because 21 688 DRAM-starved cycles disappear, not because the row-hit rate rose. The W0
counter-example makes this unambiguous: there the *faster* policy has the *lower* row-hit
rate (90.2 % → 89.3 %, §5.10). No row-hit, queue-occupancy or blocked-cycle figure in this
table is offered as a cause.

### 5.10 W0 (streaming, dedup-heavy) versus W1 at 128 slots

W0 keeps every parameter and the element count but makes the primary-class stream walk the
line set in order, so a window is almost entirely intra-line reuse: 11 550 of 12 376
elements are merged and only 770 target lines are ever issued.

| κ | E0 | E1@8192 | E2 | E3 | E4 |
|---:|---:|---:|---:|---:|---:|
| 0.25 | **17 160** | 18 142 (+5.7 %) | 17 564 (+2.4 %) | 18 142 (+5.7 %) | 18 142 |
| 0.50 | **24 804** | 24 832 (+0.1 %) | 24 997 (+0.8 %) | 24 832 (+0.1 %) | 24 832 |
| 0.75 | 31 594 | 31 594 | 31 594 | 31 594 | 31 594 |

W0 is where larger windows hurt. Records (κ = 0.25):

| policy | windows (n / alloc / seal / why / retire) |
|---|---|
| E0 | [4736/586/4682/partial/15251] [3968/4683/8779/partial/16248] [3616/15251/16194/end/17160] |
| E1/E3 | [8192/586/8200/full/17105] [4128/8201/13325/end/18142] |
| E2 | [4736/586/4682/partial/12863] [7584/12863/15662/end/17564] |

With only 770 target lines, window size buys no throughput; it only delays visibility.
Measured: first target issue at 13 448 (E0) vs 14 328 (E1/E3) vs 11 371 (E2), and the last
window's lines cannot issue until it seals, 8 200 instead of 4 682. In a 17 000-cycle run an
880-cycle delay to first issue is 5 % — the entire regression. The row-hit rate *falls*
under the faster-looking policy (90.2 → 89.3 %) and DRAM starved cycles *rise*
(2 197 → 3 034), which is the signature of a latency-bound, not bandwidth-bound, run.

### 5.11 Capacity interaction (C cycles, W1)

| κ | slots | E0 | E2 | E3 | E1@1e8 | E4 |
|---:|---:|---:|---:|---:|---:|---:|
| 0.25 | 64 | 125 778 | 126 242 | 125 778 | 125 778 | 125 778 |
| 0.25 | 96 | 145 468 | 140 696 | **124 418** | 124 418 | 124 418 |
| 0.25 | 128 | 153 475 | 147 616 | 125 581 | 125 581 | 125 581 |
| | **spread** | **27 697 (22.0 %)** | 21 374 (16.9 %) | **1 163 (0.9 %)** | 1 163 | 1 163 |
| 0.50 | 64 | 94 763 | 94 778 | 94 763 | 94 763 | 94 763 |
| 0.50 | 96 | 113 233 | 114 079 | 94 263 | 94 263 | 94 263 |
| 0.50 | 128 | 119 798 | 120 505 | 94 035 | 94 035 | 94 035 |
| | **spread** | **25 035 (26.4 %)** | 25 727 (27.1 %) | **728 (0.8 %)** | 728 | 728 |
| 0.75 | 64 | 95 899 | 95 899 | 95 899 | 95 899 | 95 899 |
| 0.75 | 96 | 95 605 | 95 605 | 95 605 | 95 605 | 95 605 |
| 0.75 | 128 | 95 194 | 95 194 | 95 194 | 95 194 | 95 194 |
| | **spread** | **705 (0.7 %)** | 705 | **705 (0.7 %)** | 705 | 705 |

The large capacity sensitivity is a property of the **deadline**, not of capacity: under E1/
E3/E4 it collapses from 22–26 % to ≤0.9 %, and E2 leaves 17–27 % of it in place. What
remains is a ≤1.1 % residual that also appears at κ = 0.75, where the operation is smaller
than one window and **no seal ever happens** (one window, `sealed at descriptor end`) — so
it cannot be a window-management effect at all.

The p = 64 record shows the underlying asymmetry the §9.18 study reported: at 64 slots the
first window reaches 8 192 elements at cycle 4 114 (full), at 96 slots it reaches 5 840 by
4 682 (partial), at 128 slots 4 736 by 4 682 (partial). Fewer outstanding targets mean fewer
conflicting DRAM rows, a faster index stream and therefore faster admission. Under E0 that
asymmetry decides *whether a third window exists*, which is why it shows up as a 22 %
capacity effect. Under E3 the third window never exists at any capacity, so the asymmetry
only shifts a few hundred cycles.

### 5.12 Seeds (128 slots, C cycles; seeds 2–4 added to seed 1)

| W | κ | seeds | E0 | E1@8192 | E2 | E3 | E3 − E0 (per seed) |
|---|---:|---:|---:|---:|---:|---:|---|
| W1 | 0.25 | 4 | 153 475 / 151 875 / 153 868 / 151 978 | 125 581 / 123 404 / 125 463 / 123 125 | 147 616 / 146 422 / 148 243 / 146 392 | = E1 | **−27 894 / −28 471 / −28 405 / −28 853** |
| W1 | 0.50 | 4 | 119 798 / 119 543 / 118 973 / 119 924 | 94 035 / 93 295 / 93 344 / 93 336 | 120 505 / 120 330 / 119 690 / 120 484 | = E1 | **−25 763 / −26 248 / −25 629 / −26 588** |
| W1 | 0.75 | 4 | 95 194 / 95 156 / 94 985 / 95 148 | identical | identical | identical | **0 / 0 / 0 / 0** |
| W0 | 0.25 | 4 | 17 160 / 16 983 / 17 137 / 17 315 | 18 142 / 18 078 / 18 150 / 18 242 | 17 564 / 17 457 / 17 542 / 17 741 | = E1 | **+982 / +1 095 / +1 013 / +927** |
| W0 | 0.50 | 4 | 24 804 / 24 314 / 24 613 / 24 869 | 24 832 / 24 405 / 24 658 / 24 888 | 24 997 / 24 577 / 24 822 / 25 060 | = E1 | **+28 / +91 / +45 / +19** |
| W0 | 0.75 | 4 | 31 594 / 31 337 / 31 580 / 31 833 | identical | identical | identical | **0 / 0 / 0 / 0** |

E3 and E1@8192 agree in **every** cell and every seed. The W1 gain (−18.2 % × 4 seeds,
residual spread ±0.5 %) and the W0 κ = 0.25 regression (+5.8 %, ±0.4 %) are both systematic,
not seed noise.

---

## 6. Correctness and experimental integrity

| Check | Result |
|---|---|
| `dx100_tests` | 4032 checks, **0 failures** (slot pool, M8 index acquisition, conservation, M4–M8 anchors) |
| M9 gates (`M9 GATES PASSED`) | **150 / 150 cells**, 0 violations |
| E0 = published results | exact: 153 475 / 119 798 / 95 194 |
| SIMT-only arm A under every policy | 220 123 / 169 826 / 120 110, completion hash 3510628737 — unchanged |
| Address / value verification | `addr_mismatch = 0`, output hash vs expectation equal in every cell |
| Conservation | `generated == issued == completed`; window admitted every element; `lines issued + merged == elements`; residency invariant held; pool empty at end |
| Window records | `windows adopted == windows sealed`; `Σ elements per sealed window == elements admitted`; one record per allocation |
| Window-manager invariants | `peak unsealed windows ≤ 1` in every policy (single `fill_` cursor) and `peak live windows == 1` for E2 — both checked, both hold in all 150 cells |
| E2 memory accounting | one buffer: `bulk_window_bytes = 73 792 B` |
| Instrumentation | read-only; every new counter is a load of existing state at an existing event, and E0's cycle counts are identical with the instrumentation compiled in |
| Defaults | `win_policy = 0`, `win_abs_cap = 4`, `bulk_stream = 0`: the published configuration is untouched |
| Determinism | reruns are bit-identical; the same cell in two batches gives the same cycles, hashes and window records |

Structural changes and their implications: E2 changes *when a buffer may be adopted*
(one live window) and therefore the modelled SRAM (two buffers → one). E3/E4 change only
*which cycle counter the deadline compares against* and, for E4, add one extra comparison.
None of them touches admission, sealing order, dedup, page order, cohorts, issue order,
arbitration or the pool, and none can affect correctness: every element is still admitted
exactly once, issued once per distinct line, and retired once, which the per-run checks
confirm in all 150 cells.

---

## 7. Analysis

### 7.1 Directly measured

1. The deadline clock is anchored at **buffer allocation** (`nb.fill_start = cycle`), not at
   first arrival; the seal condition additionally requires `n_ent > 0`.
2. The manager has **two buffers and one filling cursor**: at most one *incomplete* window
   exists at all times (`peak unsealed = 1` in all 150 cells), and at most two *live* ones.
3. A sealed window keeps its buffer until every line has issued, every fill has returned
   and every element has retired.
4. Under E0 at 128 slots / κ = 0.25 a 12 376-element operation is sealed into three windows
   (4 736, 3 968, 3 672); windows 1 and 2 are simultaneously sealed from cycle 8 779 to
   87 823, and for 79 043 cycles the generator has elements and no buffer may accept them.
5. E1 saturates at `batch_deadline = 8192`; 16 384, 65 536 and 1e8 give identical results in
   all three κ.
6. E3 at the published 4 096 bound gives exactly the E1@8192 result in all 18 W1 cells and
   all 4 seeds; E4 is identical to E3 everywhere (the 4×D ceiling never binds).
7. E2 halves the live-window count and the window SRAM, raises mean window size by 50 %,
   and yet gains only 3.8 % at κ = 0.25 and loses 0.6 % at κ = 0.50 — with 81 045 / 86 314
   cycles of generator-work held back by the policy itself.
8. The large capacity sensitivity (22–26 %) collapses to ≤0.9 % under E1/E3/E4, is retained
   under E2, and a ≤1.1 % residual remains even at κ = 0.75 where no window is ever sealed.
9. On W0 the same policies cost 5.7 % (κ = 0.25, 4/4 seeds) while the row-hit rate falls and
   DRAM starved cycles rise; time to first target issue moves from 13 448 to 14 328.

### 7.2 Answers to the questions asked

**1. Does increasing the partial-seal deadline improve performance?** Yes on W1, massively
and with saturation: −18.2 % at κ = 0.25 and −21.5 % at κ = 0.50 the moment the bound stops
firing before a window fills (8192), and no further effect above that. No effect at κ = 0.75,
where the operation is smaller than one window. On W0 it is a small *regression* (+5.7 % at
κ = 0.25).

**2. Does limiting incomplete windows improve average window size?** The number of incomplete
windows is already 1, so the literal policy is a no-op (§2.8). Its implementable form
(E2, one live window) does raise mean window size from 4 125 to 6 188 elements (+50 %) at
κ = 0.25 — and does not pay, because the serialization it introduces blocks the generator
for 81 045 cycles and moves the work into a longer tail.

**3. Does sequential filling improve global visibility or reduce fragmentation?** Filling is
already sequential; what the measurements show is that *fragmentation* was the problem, and
it is caused by truncation, not by ordering. At κ = 0.50 the control cuts 8 260 elements into
three windows ending in a 20-element tail, while E3 produces one full 8 192-element window
plus a 68-element tail; at κ = 0.25 the control makes three windows (4 736 / 3 968 / 3 672)
and E3 one full window plus 4 184. The mechanism that converts fragmentation into lost
cycles is the
two-buffer limit, which is measured directly: `cycles with work but no filling window`
79 043 → 0.

**4. Does E4 provide a better balance between window size and latency?** No — E4 is
measurably identical to E3 in all 18 cells. Once the bound measures waiting rather than
filling, the 16 384-cycle deadline and the 4×D ceiling never bind, so there is nothing to
trade. E4 is not worse, it is simply not a distinct policy on these workloads.

**5. Are the performance changes explained by window size, index acquisition, shared-slot
availability, translation, or DRAM scheduling?** By **window size only through buffer
availability**. Index acquisition is neither faster nor slower: the same 774 index lines are
fetched, page walks move 539 → 526, and the ring simply drains from 66 buffered ready lines
to 1.3 because consumption no longer stalls (§5.7). Slot residency *falls* (peak bulk
128 → 120), translation events are unchanged (72 790 → 72 193 C1 cycles, within seed noise),
and DRAM starvation is a *symptom* (33 353 → 11 665 starved cycles, row-hit rising only
because concurrency falls). The causal chain is: seal at 4 096 while
still filling → three windows → two-buffer occupancy → generator blocked → index ring backs
up → issue starves → DRAM idles.

**6. Does the best policy differ between κ = 0.25, 0.50 and 0.75?** No: E1@8192 = E3 = E4
is best wherever a policy can matter, and at κ = 0.75 no policy can matter at all (single
window, descriptor-end seal, all five policies identical to the cycle). E2 is never best.

**7. Does the policy improve W0 and W1, or shift the trade-off?** It shifts the trade-off.
W1 gains 18–21 %; W0 loses up to 5.8 % at κ = 0.25 and ~0.2 % at κ = 0.50. Because W0 is
latency-bound (770 target lines, 11 550 dedup merges) and W1 is throughput-bound, larger
windows help the second and hurt the first. This is the finding that forbids a blanket
"bigger windows are better" conclusion, and it is the reason no default was changed.

**8. Does the 64/96/128-slot interaction persist under the alternative policies?** The large
interaction does not: under E1/E3/E4 the spread falls from 22.0 %/26.4 % to 0.9 %/0.8 %. A
≤1.1 % residual persists at every κ — including κ = 0.75, where nothing is ever sealed — so
it belongs to the shared pool, not to window management. Under E2 the interaction is
retained in full (16.9 %/27.1 %).

**9. What is the smallest policy change that produces a repeatable benefit?** **E3: make the
existing 4 096-cycle bound count cycles since the last admission instead of cycles since
allocation.** No parameter changes, no new storage, no change to admission, sealing order,
issue order or arbitration; one comparison in the seal test. It reproduces the no-deadline
result on all 18 W1 cells and all 4 seeds at all three capacities, and it keeps a finite
anti-starvation bound. E1 at 8192 is an equally small *constant* change with identical
results in this matrix, but its equivalence is a fitted coincidence of these workloads,
whereas E3's is structural: it cannot truncate a window that is still making progress at any
admission rate.

### 7.3 Hypotheses (not established here)

* Why admission is faster with fewer slots (the §9.18 open question) is still not resolved at
  the level of a single mechanism. This experiment adds one measurement: at 64 slots the
  first window reaches 8 192 elements by cycle 4 114, at 96 slots 5 840 by 4 682, at 128 slots
  4 736 by 4 682. The most economical explanation is that fewer outstanding targets produce
  fewer DRAM row conflicts, which shortens index-line latency and therefore admission — the
  index stream and the target stream share the DRAM — but no per-request attribution of
  row conflicts to stream exists in this simulator (§8).
* The systematic W0 regression is consistent with a pure latency effect (time-to-first-issue
  and the last window's seal time), and the numbers support it, but it was not demonstrated
  by a controlled isolation of the two components. A variant that seals the last window early
  while keeping full windows elsewhere would separate them.

### 7.4 Unresolved

* One footprint (`P = 512`, `N = 16 384`) and one seed family. Whether the trade-off
  reverses at other footprints, at other W sizes, or under `page_cohorts`/`park_retry`
  variation is not measured.
* `bulk_stream` realises W0 by making the primary-class stream sequential. It is a faithful
  stand-in for "regular and coalescible" but not the literal work of any published W0 run.
* There is no per-cycle attribution of DRAM row conflicts to the index vs target stream, so
  the admission-rate asymmetry cannot be closed from these logs.
* E2 was measured with the fill/drain overlap it permits *removed* (§2.8). An E2 that keeps
  overlap is E0 and was not run separately, because it would be the same binary path.

---

## 8. Limitations

1. **Single simulator, single workload family.** Every number is from the standalone
   `dx100sim` model; no RTL or silicon correlation is offered or implied.
2. **One footprint per κ.** `P = 512`, `N = Λ = 16 384`, `elem_bytes = 4`. The absolute
   window counts (2–3) are a property of 16 384 elements over an 8 192-element window; a
   longer descriptor would have more windows and more chances to be fragmented, and a shorter
   one fewer (at κ = 0.75 it already fits in one window and no policy matters).
3. **Seeds test repeatability, not generality.** Four seeds confirm the sign and rough size
   of every effect; they do not substitute for more footprints.
4. **The deadline values tested are powers of two around the observed fill time.** The
   saturation at 8192 is a property of this footprint's fill rate (7 614 cycles to 8 192
   elements at 128 slots).
5. **W0 is a constructed workload** (`bulk_stream`), not a published trace.
6. **Not every metric the request lists is separately meaningful here**: because the runs
   are a few thousand to 150 000 cycles with 2–3 windows, "average, median, min, max" of
   window size are computed over 1–3 observations per cell, and the distribution tables are
   exhaustive rather than statistical. Medians of one or two values are reported as such.
7. `replays` in the memory system counts accumulated per-cycle replay state rather than
   discrete replay events and is not used in this analysis.

---

## 9. Recommended next step

**For authorisation, not for implementation here:** the evidence supports considering
**E3** (`win_policy = 2` semantics: the partial-seal bound counts cycles since the last
admission) as the candidate successor to the published deadline rule, because it is the
smallest change that removes a 22–26 % capacity artefact and an 18–21 % loss on the
throughput-bound workload at the *existing* 4 096-cycle constant, at every capacity tested
and every seed.

It should **not** be adopted on this evidence alone, for one measured reason: it costs 5.8 %
on W0 at κ = 0.25, systematically. Before any default changes, the next step should be the
experiment that this one cannot answer — whether the two effects can be separated, e.g. a
policy that seals a window early only when the generator is near the end of the descriptor
(W0's case is a 770-line operation that finishes inside one window), with the guard that it
must not reintroduce the three-window fragmentation at κ ≤ 0.50. That is a new hypothesis
test, not a consequence of these results, and it needs its own control.

E2 and E4 are **not** recommended: E2 retains most of the pathology and halves the buffer
budget for a 3.8 %/0 % gain; E4 is indistinguishable from E3.

The published M9 configuration is unchanged in this work.

---

## Appendix A — per-window records

`[elements / allocation cycle / seal cycle / why / release cycle]`, why = full | partial |
descriptor-end. C arm, seed 1, 128 slots unless noted.

| W | κ | policy | windows |
|---|---:|---|---|
| W1 | 0.25 | E0 | [4736/586/4682/partial/87823] [3968/4683/8779/partial/121417] [3672/87823/90347/end/153475] |
| W1 | 0.25 | E1@8192, E3, E4 | [8192/586/8200/full/90105] [4184/8201/13646/end/125581] |
| W1 | 0.25 | E2 | [4736/586/4682/partial/85728] [7640/85728/88770/end/147616] |
| W1 | 0.50 | E0 | [4736/586/4682/partial/91667] [3504/4683/8779/partial/119525] [20/91667/91669/end/119798] |
| W1 | 0.50 | E1@8192, E3, E4 | [8192/586/8226/full/93111] [68/8227/10291/end/94035] |
| W1 | 0.50 | E2 | [4736/586/4682/partial/90997] [3524/90997/92150/end/120505] |
| W1 | 0.75 | all five | [4052/586/3898/end/95194] |
| W0 | 0.25 | E0 | [4736/586/4682/partial/15251] [3968/4683/8779/partial/16248] [3616/15251/16194/end/17160] |
| W0 | 0.25 | E1@8192, E3, E4 | [8192/586/8200/full/17105] [4128/8201/13325/end/18142] |
| W0 | 0.25 | E2 | [4736/586/4682/partial/12863] [7584/12863/15662/end/17564] |
| W0 | 0.50 | E0 | [4736/586/4682/partial/23969] [3320/4683/8442/end/24804] |
| W0 | 0.50 | E1@8192, E3, E4 | [8056/586/8112/end/24832] |
| W0 | 0.75 | all five | [4072/586/3911/end/31594] |
| W1 | 0.25 | E0 @ 64 slots | [8192/337/4114/full/90411] [4184/4115/7947/end/125778] |
| W1 | 0.25 | E0 @ 96 slots | [5840/586/4682/partial/88812] [4448/4683/8779/partial/125124] [2088/88812/89634/end/145468] |
| W1 | 0.25 | E3 @ 64 slots | [8192/337/4114/full/90411] [4184/4115/7947/end/125778] |
| W1 | 0.25 | E3 @ 96 slots | [8192/586/6347/full/89012] [4184/6348/10757/end/124418] |

At 64 slots the first window fills to 8 192 *before* the 4 096-cycle deadline, so the E0
record and the E3 record are identical — which is exactly why the 64-slot configuration was
the one that "filled full windows more consistently" in the earlier capacity study. The
difference was never the slot count: it was whether the deadline happened to fire first.

## Appendix B — new configuration keys

| Key | Default | Values | Meaning |
|---|---:|---|---|
| `win_policy` | 0 | 0–3 | 0 E0 published · 1 E2 one live window · 2 E3 admission-anchored deadline · 3 E4 (+4×D age ceiling) |
| `win_abs_cap` | 4 | ≥1 | E4's absolute age ceiling, in multiples of `batch_deadline` |
| `bulk_stream` | 0 | 0/1 | 0 = W1 (permuted primary class, published) · 1 = W0 (in-order primary class, same footprint/κ/seed) |

E1 uses existing keys only (`batch_deadline`).

## Appendix C — code index

| Site | File:line | Role |
|---|---|---|
| `Config` policy fields | `sim.hpp:153-155` | `win_policy`, `win_abs_cap` |
| `Batch::last_admit` | `sim.hpp:981` | E3/E4 deadline anchor |
| `BulkStats::WinRec`, aggregates | `sim.hpp:804-822` | per-window records and live/blocked counters |
| `win_rec()` | `bulk.cpp:472` | buffer → record |
| `win_admit` | `bulk.cpp:918` | admission; re-arms `last_admit`; records first/last |
| `win_seal` | `bulk.cpp:997` | records size/reason; discards empty windows |
| `win_emit` | `bulk.cpp:1119` | residency refusal (unchanged) |
| `win_issue` / `win_issue_sched` | `bulk.cpp:1266` / `1217` | issue (unchanged) |
| `win_tick` step 2 | `bulk.cpp:1361` | release; records retire |
| `win_tick` step 3 | `bulk.cpp:1370` | adopt sealed window (unchanged) |
| `win_tick` step 4 | `bulk.cpp:1375-1407` | allocation, E2 gate, wait/blocked counters |
| `win_tick` step 6 seal | `bulk.cpp:1441-1466` | reason precedence and the three deadline variants |
| `win_tick` occupancy | `bulk.cpp:1478-1491` | live / unsealed / sealed peaks |
| `export_win_records` | `bulk.cpp:37-118` | aggregation at export (no simulation effect) |
| diag sections `[16]`, `[16b]` | `ab.cpp:2306-2400` | printed results |
| W0 generator branch | `workload.cpp:115` | `bulk_stream` (default off) |
