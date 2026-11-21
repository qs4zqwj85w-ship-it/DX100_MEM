# Fused window representation — prototype report

**Scope.** The hardware-cost audit found the window's C++ representation to be
its second-largest storage term (after the L2) and, per buffer, ~3.2× larger than
the 8 B/entry the design charges. This report answers the question the audit
could only pose: can the window be stored in a genuinely compact form **without
changing architectural behaviour**? It can, it is implemented behind
`win_fused=1`, and it is measured here.

No official default changed. `win_fused` defaults to 0, no published
configuration sets it, and with it at 0 the entire M5–M14 path compiles and runs
exactly as before (verified: the M10 anchors reproduce to the cycle,
§4.3).

---

## 1. The original storage layout

### 1.1 What is allocated

Per window buffer (`BulkEngine::Batch`, `sim/src/sim.hpp`), with `W =
batch_window` elements and `L` distinct target lines:

| array | element | size | role |
|---|---|---|---|
| `ent` | `WinEnt` | **16 B** (measured) | one per admitted element: `va_line` (8), `elem` (4), `off` (2), `width` (1), pad (1) |
| `sorted` | `u32` | 4 B | entry ids ordered by `(va_line, entry id)` — the permutation that makes equal lines adjacent |
| `lines` | `LineReq` | **32 B** (measured) | one per distinct line: `va_line` (8), `first` (4), `count` (4), `seq` (4), `slot` (4), `issued` (1)+pad, `reuse_ent` (4) |
| `order` | `u32` | 4 B | indices into `lines`, in issue order |
| `dir_vpn`, `dir_mask` | `u64` | 16 B × `D` | page directory: `page_dir` = 512 entries, 8 KiB, per buffer |
| `coh` | `Cohort` | 32 B × `page_cohorts` | page runs (2 by default → 64 B) |
| `fmeta/fslot/fflag` | — | — | absent in this layout |

Design footprint per buffer:

```
M6(W, L) = 16·W + 4·W + 32·L + 4·L + 16·D + 64        [D = page_dir = 512]
```

At W = L = 8 192 this is 467 008 B/buffer; two buffers are 934 016 B, which
matches the audit's measured `bulk_win_vec_bytes` of 933 856 B (capacity-based,
±0.02 %).

### 1.2 What is actually *information*

Reading every consumer (`win_admit`, `win_seal`, `win_issue`, `win_issue_sched`,
`win_fanout`, `win_reuse_probe`, `on_line`, `win_tick`) gives this classification:

**Must be stored per element**
* the target line identity (a 64 B physical line never straddles a page, so the
  VA line *is* the identity — no translation is needed to group);
* the in-line offset (which word of the 64 B line the element reads);
* which output element the value belongs to.

**Must be stored per distinct line**
* the range of elements that depend on it (fanout needs it at fill time);
* the residency slot while its request is outstanding;
* an "issued" bit, so the issue cursor does not re-request it.

**Can be reconstructed**
* the element id: it is `elem_base + position`, and `elem_base` is a descriptor
  field (constant for the run);
* the data width: a descriptor field, identical for every element of the run;
* the issue order: with `page_order = 1` it is the line array itself, in
  ascending line order — `order` is the identity permutation at `sim/…`;
* the line→element grouping: equal lines are adjacent in the sorted array, so a
  run is `[start, start+count)` and needs no permutation array.

**Temporary**
* the 64 B copy one fanout job holds (`FanJob::bytes`), one line in flight;
* the sort's working set — in place, so none beyond the entry array.

**Diagnostic only**
* `val_` and `addr_`: the model's output store (`std::unordered_map`, ~100 B per
  element realized). They exist so the simulator can verify
  `output[i] = data[index[i]]` independently; hardware writes the output array to
  memory instead (8 elements per 64 B store). They are **not** window storage and
  must not be charged to it;
* `WinRec` per window (72 B, 8 exported fields) — M9's accounting record;
* `m13_dump` — an optional file of per-window line sets;
* `dir_collisions`, `n_pages` — counters derived from the directory;
* the two `std::set` scoreboards (`idx_pending_`, `tgt_pending_`) — issue-order
  bookkeeping for the model, not window state.

The audit's recommendation to discard `val_`/`addr_` (≈ 1.6–2.0 MB at 32K) and
`WinRec`/`m13_dump` stands and is unaffected by this prototype: they remain model
state.

### 1.3 The redundancy the audit measured

The layout stores the element→line mapping **twice** (`ent[i].va_line` and,
implicitly, the run in `lines` that `sorted` indexes into), and it stores the
grouping as an explicit permutation (`sorted`, 4 B per element) plus a per-line
record (`first`, `count`, 8 B of the 32 B). Of the 20 B/entry in `ent + sorted`,
8 B is the same line number `lines` already holds, and 4 B is a permutation that
a sorted array does not need.

---

## 2. The proposed compact layout

### 2.1 The fused entry

One 64-bit word per element, **sorted in place** by `(line, position)`:

```
 63        38 37                    6 5      0
+------------+----------------------+--------+
| va_line:26 | position: 32         | off: 6 |
+------------+----------------------+--------+
```

* 26 bits: `VA_LIMIT / 64 = 2^26` lines, i.e. the whole 4 GiB VA space at 64 B
  granularity. This is the address identity the grouping already relies on.
* 32 bits: the element's position in the descriptor (`count ≤ 2^32`; the
  configured `bulk_max_elems` is 2.6 × 10^5, and the window bound is 2^16).
* 6 bits: the byte offset inside the 64 B line.

The field order is deliberate. Sorting the word numerically orders by
`(va_line, position, offset)`, which is the order the M6 comparator
`(va_line, entry id)` produces for an in-order window, so the fanout sequence
within a line is preserved.

**Does 64 bits hold it?** For *this* design, yes with 17 bits to spare if the
position field is narrowed. `count ≤ 2^16` inside a window and `off` is a
multiple of the element width (3 bits for 8 B elements, 4 for 4 B), so
`26 + 16 + 4 = 46` bits suffices; a 48-bit entry (6 B) is the natural RTL width
(§6). The simulator uses 64 bits because a 64-bit word keeps the sort and the
decode trivial and the extra 2 B change none of the conclusions below.

### 2.2 The per-line state

```
fmeta[li]  = start:16 | count:16        (4 B)   the element run for line li
fslot[li]  = residency slot, 255 = none  (1 B)
fflag[li]  = bit 0 issued / served       (1 B)
```

`start` and `count` are 16 bits because a window holds at most 2^16 elements —
enforced, not assumed (§2.4). The line address is *not* stored per line: it is
`fent[start] >> 38`. The issue order is *not* stored at all: it is
`0 … fmeta.size()-1`, which is ascending line order because the array is sorted.

### 2.3 What replaces what

| M6 | fused | note |
|---|---|---|
| `ent` (16 B/el) | `fent` (8 B/el) | `elem` → `elem_base + pos`; `width` → descriptor field |
| `sorted` (4 B/el) | — | the sort is applied to `fent` in place |
| `lines` (32 B/line) | `fmeta` (4 B/line) | `va_line` decoded from `fent[start]`; `seq`/`reuse_ent` unused under `page_order = 1` |
| `order` (4 B/line) | — | the line array is the issue order |
| — | `fslot`+`fflag` (2 B/line) | the two fields `lines` held for free |
| `dir_vpn`/`dir_mask` | unchanged | `D` is enforced *during admission*, before the array is sorted, so the table cannot be replaced by a scan without changing when admission stops |

Design footprint per buffer:

```
fused(W, L) = 8·W + 6·L + 16·D + 64
```

### 2.4 Preconditions, enforced at construction

Each follows from the representation, so each is a hard error rather than a
silent fallback (`BulkEngine::BulkEngine`):

* `batch_window > 0` — a window is what gets sealed;
* `dedup = 1` — a line *is* a run;
* `page_order = 1` — the sorted array is the issue order;
* `batch_window ≤ 65 536` — `(start, count)` are 16-bit;
* `R ≤ 255` — the residency slot is 8-bit;
* `reuse_dir = 0` — M13's buffer keeps a per-line record the fused array does
  not carry (and M13 is not recommended anyway).

Two are tested directly (`m15_fused_window` asserts the throw for `dedup = 0`,
`page_order = 0` and `batch_window = 131 072`).

### 2.5 Implementation

`win_seal_fused` sorts and builds the run words; `win_emit_fused`,
`win_open_cohort_fused`, `win_issue_sched_fused`, `win_fanout_fused` mirror their
M6 counterparts line for line, as separate functions so the published path
compiles to exactly what it compiled to before. `win_admit_one` writes the fused
word instead of a `WinEnt`; `win_tick`, `on_line`, `push_fan` and the page
directory take a two-way branch on `fused()`.

Six functions plus a two-way branch at the five call sites that touch window
state, all of it off the default path.

---

## 3. Actual storage comparison

### 3.1 Design footprint (formula, `D = 512`, both buffers)

| W = L | M6 per buffer | fused per buffer | ratio | M6 both | fused both | saved |
|---|---|---|---|---|---|---|
| 8 192 | 467 008 B | 122 944 B | **3.80×** | 934 016 B | 245 888 B | 688 128 B |
| 16 384 | 925 760 B | 237 632 B | **3.90×** | 1 851 520 B | 475 264 B | 1 376 256 B |
| 32 768 | 1 843 264 B | 467 008 B | **3.95×** | 3 686 528 B | 934 016 B | 2 752 512 B |

Without the page directory and cohorts — i.e. the arrays this milestone actually
replaces — the ratio is **exactly 4.00×** at every window size, because the fused
entry costs a quarter of `ent + sorted + lines + order` per element-and-line
(14 B vs 56 B, since `L = W` on these workloads).

Two consequences worth stating plainly:

* **The fused 32K window occupies exactly what the M6 8K window occupies**
  (467 008 B/buffer). The representation buys one capacity step for free.
* **At 32K, 2.75 MB per engine disappears** (both buffers), which is more than
  the entire 1 MiB L2 and is what the audit called the second-largest storage
  term after the cache.

### 3.2 Realized simulator storage (measured, peak over the run)

From the 6 matrix cells (`bulk_win_vec_bytes` vs `bulk_win_fused_vec_bytes`,
`std::vector::capacity()` sums, peak of the cycle):

| W | seed | M6 realized | fused realized | ratio |
|---|---|---|---|---|
| 8 192 | 1 | 933 856 B | 245 744 B | 3.80× |
| 8 192 | 2 | 932 896 B | 245 512 B | 3.80× |
| 8 192 | 3 | 932 192 B | 245 336 B | 3.80× |
| 32 768 | 1 | 1 188 016 B | 368 640 B | 3.22× |
| 32 768 | 2 | 924 704 B | 237 320 B | 3.90× |
| 32 768 | 3 | 924 000 B | 237 144 B | 3.90× |

The 32K seed-1 row is larger in both layouts for an occupancy reason, not a
layout reason: that descriptor (16 428 elements) fits in **one** 32K window, so
only one buffer ever holds entries, while the M6 vectors' geometric growth
rounds `ent` and `sorted` up to 32 768 slots. Where both layouts hold the same
content (seeds 2–3, 16 172–16 260 elements) the measured ratio is 3.90×, matching
the formula.

The realized numbers include the 8 KiB page directory in both, which is why they
are 3.2–3.9× and not 4.0×: at these window sizes the directory is 6.7 % of the
M6 buffer and 30 % of the fused one. **The directory is now the largest single
array in the window** — which is the audit's open question about whether `D` is
sized correctly, and this prototype makes it urgent rather than theoretical.

---

## 4. Correctness and performance

### 4.1 What is preserved

Every architectural obligation named in the brief, and where each is checked:

| obligation | how it is checked |
|---|---|
| exact target addresses | independent re-derivation from the index image at every fanout element (`addr_mismatch`), plus `engine_hash`/`truth_hash` over `(element, address, value)` |
| cache-line identity | the entry *is* the VA line; grouping is by exact equality of the 26-bit field, the same identity the M6 layout used |
| element offsets | `off` is stored, and the fanout loads at `bytes + off` with the descriptor width |
| duplicate detection | identical `dedup_merged` in every paired cell (8 vs 8, 0 vs 0, 44 vs 44) |
| response fanout | one fill retires exactly its run; `retired == n_ent` releases the buffer, and the run check is `issued == done + faults` |
| out-of-order completion | a fill matches its `Pending` by slot; the run index travels in `Pending::win_line`; nothing depends on arrival order |
| page scheduling | the cohort scheduler is ported (`win_open_cohort_fused`, `win_issue_sched_fused`) and produces the same issue order |
| window ordering | `(va_line, position)` sort; `page_order = 1` required |
| retirement semantics | `run_.done`, `finished` and buffer release driven by the same counters |
| output values | value hash equal to the expectation in all 6 cells, and to the M6 run's value in all 3 pairs |

### 4.2 The measurement

Six cells, three seeds × two capacities, fused against M6 on the *same* trace
(κ = .75, N = 65 536 — the anomaly configuration, so the comparison is at the
hard point):

| W | seed | C cycles M6 | C cycles fused | delta | lines issued | dedup | addr mismatch |
|---|---|---|---|---|---|---|---|
| 8 192 | 1 | 244 823 | 244 823 | +0.000 % | 16 420 / 16 420 | 8 / 8 | 0 |
| 8 192 | 2 | 245 419 | 245 419 | +0.000 % | 16 260 / 16 260 | 0 / 0 | 0 |
| 8 192 | 3 | 244 688 | 244 688 | +0.000 % | 16 172 / 16 172 | 0 / 0 | 0 |
| 32 768 | 1 | 264 541 | 264 541 | +0.000 % | 16 384 / 16 384 | 44 / 44 | 0 |
| 32 768 | 2 | 262 402 | 262 402 | +0.000 % | 16 260 / 16 260 | 0 / 0 | 0 |
| 32 768 | 3 | 260 978 | 260 978 | +0.000 % | 16 172 / 16 172 | 0 / 0 | 0 |

Value hash, all four of {fused, M6} × {8 192, 32 768}, seed 1:
`1 791 610 298 472 719 864` — equal to the independent expectation.

**Why the cycles are identical rather than merely close:** the fused path issues
the same lines in the same order (the sort produces the same line sequence), takes
and releases the same number of slots, performs the same fanout at the same
`fanout_lanes`, and charges the organizer the same one visit per entry at seal.
The representation changed; no decision did.

### 4.3 Regression suite

* `./bin/dx100_tests`: **4 470 checks, 0 failures** (4 338 before this milestone,
  +132 from two new tests).
* New: `m15_fused_window` — 32 elements over 5 lines, window 8, cohorts on: same
  cycles, lines, dedup, values and addresses as M6; fused storage smaller;
  preconditions enforced. `m15_client_attribution` — the new cache counters are
  exact (per-requester rows sum to the aggregate) and inert (cycles and every
  aggregate counter unchanged with the flag on).
* Historical anchors with `win_fused` at its default: κ = .25/.50/.75, N = 16 384,
  W = 8 192, seed 1 → `220 123/138 924/125 581`, `169 826/127 138/94 035`,
  `120 110/116 804/95 194`. Unchanged, to the cycle.
* All 202 matrix cells report `M9 GATES PASSED`, including the 6 fused cells.

---

## 5. Additional logic and timing implications

### 5.1 What the model can and cannot charge

The simulator's organizer is a single abstract unit charged **one visit per
entry** at admission and one per entry at seal (`org_backlog`, `org_lanes`). Both
layouts perform exactly one pass per entry at seal, so both are charged the same,
which is why the cycles are identical. That is a real property of the
*algorithm* (sort then scan, once each) — but it is **not** a measurement of the
sort's cost, and this is the main limitation of the prototype:

* **The fused sort moves twice the bytes.** M6 sorts `W` 32-bit indices (4 B
  swaps, and the comparator reads `ent[].va_line`, a second array); the fused path
  sorts `W` 64-bit keys in place. Same asymptotics, same pass count, ~2× the
  swap bandwidth. The model charges neither, so the honest statement is: *the
  fused layout's seal costs more bandwidth than the number above shows, and the
  model cannot price it.* §6 proposes the fix (a radix/bucket sort on the line
  field, which is cheaper than either).
* **The fanout reconstructs two fields** (`elem = elem_base + pos`,
  `width = descriptor[41]`) instead of loading them. In the model that is free;
  in hardware it is an adder and a register read, i.e. strictly cheaper than the
  5 B/entry the reconstruction replaces.
* **The issue cursor loses an indirection.** M6 reads `order[pos]` → `lines[i]` →
  `lines[i].va_line`, a dependent load chain; the fused path reads
  `fent[fmeta[pos] >> 16]` — one level. The model gives memory the same cost
  either way, so this benefit is also unpriced.

### 5.2 Decode logic

Trivial and constant-time:

```
line = w >> 38        pos = (w >> 6) & 0xffffffff        off = w & 0x3f
```

No comparator chain, no content-addressable lookup, no second array. The M6
layout's fanout reads `b.ent[eid].va_line`, `.elem`, `.off`, `.width` — four
fields across two arrays, one of them an indexed indirection through `sorted[]`.

### 5.3 The sort

The seal already sorts today (M6 sorts `W` entry indices with a comparator over
`ent[].va_line`), so fusion does not introduce a sort — it changes its key from
`(32-bit index, indirect compare)` to `(64-bit key, direct compare)` and applies
it in place instead of to a permutation. In an RTL implementation the realistic
structure is not a comparison sort at all: the entry's top 26 bits are the line,
so a **bucket sort by page** (the `page_dir` table already exists) followed by an
insertion pass within each page gives ascending line order in O(W) with `D`
buckets. That is what §6 recommends, and it is the one place where the fused
layout is *much* better than a direct translation of the C++: bucketing needs the
entry and nothing else, while M6's permutation would have to be bucketed
alongside.

### 5.4 What the fused layout does not fix

The table above is only the window. The four other window-adjacent costs the
audit identified remain exactly as they were and are not addressed here: `val_`
and `addr_` (model-only output state, ~1.6–2.0 MB at 32K), `WinRec`, the
`m13_dump` path, and the two `std::set` scoreboards. They are implementation
scaffolding, and the audit's recommendation to drop them stands.

---

## 6. RTL implementation considerations

1. **Use 48 bits, not 64.** `va_line:26 | position:16 | offset:4` = 46 bits, held
   in a 48-bit (6 B) entry. That takes the core ratio from 4.00× to **4.67×**
   (6 B entry + 4 B run + 2 B slot/flag = 12 B per element-and-line, against
   56 B for `ent + sorted + lines + order` when `L = W`) and makes a 32K window
   cost 401 472 B instead of 467 008 B. The condition is `count ≤ 2^16` inside a
   window, which the 16-bit run fields already require.
2. **Do not copy the object layout.** The arrays above are *logical* stores. A
   real buffer wants one width-parameterised entry SRAM (not three arrays), `L`
   deep by 6 B, plus a small `L`-deep slot/flag SRAM, and a separate run SRAM.
   The current C++ splits `ent`/`sorted`/`lines`/`order` only because they are
   convenient to write in four passes.
3. **Sort by bucket, not by comparison.** 26 bits of line + a 512-entry page
   directory is a radix pass. A comparison network at W = 32 768 would need
   `O(W log²W)` comparators (~10^7) and is not buildable; a bucket pass is
   `W` writes plus `D` scans. This is the single most important RTL change and it
   is *enabled* by fusion (the key is in the entry), not required by it.
4. **Bank the entry array.** Fanout reads a contiguous run and the organizer
   writes sequentially, so 8 banks of 8 B sustain one element per cycle with no
   conflict; the M6 layout's four arrays need four separate ports to do the same.
5. **The run word is the fanout's address.** A fill presents `line`; the fanout
   needs `(start, count)` for that line. With the array sorted, a binary search on
   the 6 B entry (15 steps at 32K) recovers it without `fmeta`; keeping `fmeta`
   (4 B/line) turns it into a direct read. Keep `fmeta` — 4 B/line is 0.3 % of
   the buffer and the search would otherwise be on the response path.
6. **The page directory stays.** `D` must be enforced while the window is being
   filled, and at that point the array is unsorted, so the count cannot be
   derived. Replacing the 8 KiB table with a counter would change *when*
   admission stops, which is a behaviour change, not a representation change.
   (If `D` is resized — the audit's open question — the fused layout makes the
   trade visible: the directory is now 30 % of a 32K buffer.)
7. **Keep the entry write on the admission path, not the seal path.** The current
   prototype admits a 64-bit word and sorts at seal. An RTL design can insert
   directly into a per-page bucket at admission, which removes the sort from the
   critical path entirely — but that changes *when* dedup becomes visible, so it
   would need its own milestone and its own equivalence gate.
8. **Do not carry `val_`/`addr_` into hardware.** They are the model's output
   store; hardware writes the output array. Charging them to the window would
   triple its size and is a modelling artefact, not a design cost.

### 6.1 Recommended adoption path

1. Adopt the **fused entry** (48-bit) with the `fmeta` run words and the two
   per-line byte arrays. It is a pure storage change with measured zero cycle
   cost and a 4.67× core reduction.
2. Keep `page_order = 1` and `dedup = 1` as the conditions — they already hold in
   every published configuration, so this constrains nothing today.
3. Treat the bucket-sort-at-admission idea as a **separate** milestone: it is
   where the remaining organizer cost lives, and it changes the dedup visibility
   point.
4. Re-examine `D` next, but do not assume it can shrink. After fusion it is the
   largest non-entry array in the window (8 KiB per buffer at 32K). The measured
   resident page count (`dir_occ_max`, the max of `n_pages` over windows) reaches
   **512 in every W1 cell** — the directory is saturated, not under-used — and
   reaches 25 534 in the widest PSCR cell, which is above the configured bound and
   which this study did not chase. The honest position is that the `D` question is
   open in both directions; what fusion adds is that the directory is now a visible
   fraction of the buffer instead of a rounding error.

---

## 7. Verdict

The fused representation is feasible, is implemented, and is exact: identical
cycles, identical issued lines, identical dedup, identical values and addresses,
in all six measured cells and in the unit test. It reduces the window's core
storage by exactly 4.00× (3.80–3.95× including the unchanged page directory), and
it makes a 32K window occupy what an 8K window occupies today. It is not a
performance mechanism — it buys no cycles — and it should be adopted for what it
is: the removal of 1.3 MiB of redundancy per engine at 32K — more than the 1 MiB
shared L2 those two buffers compete with.

Its two honest costs are that the model does not price the sort's bandwidth
(§5.1) and that it constrains the configuration to `dedup = 1`, `page_order = 1`,
`W ≤ 65 536`, `R ≤ 255`, `reuse_dir = 0`. Those are the conditions to carry into
an RTL review, and §6.1 states the order in which to adopt it.
