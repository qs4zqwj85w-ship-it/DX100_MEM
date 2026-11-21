# M20 — Mechanism Analysis

Why the M18 Bulk advantage shrinks as SIMT widens, measured rather than asserted.
Every number here is from an M20 cell (`build/wm/m20_<matrix>_<variant>_w<W>_l<L>.txt`),
2×8K window unless stated, and every claim is checked against the four-arm
decomposition `S_f = 1 + (ΔA_idx − ΔC_eng)/C(noidx)` that M19 validated.

---

## 1. What the width knob does, and what it does not

`lanes` enters the model in exactly one place: **instruction formation**. It sets
how many logical accesses are presented together, and therefore the instruction
count, the coalescer's grouping, and the resulting line requests. It does **not**
model a wider memory pipe, per-lane MSHRs, wider LSU ports, or a larger per-warp
address-generation width. The full accounting is in
`m20-simt-width-model-validation.md` §2; the practical consequence is that a
32-lane cell here answers "what if SIMT formed 32-lane accesses?" and *not* "what
would a 32-lane GPU do?".

| what stays fixed across widths | what changes |
|---|---|
| DRAM state machine, banks, row buffer | instructions issued per row segment |
| L1/L2 capacity, geometry, MSHR counts | accesses per instruction |
| residency pool (128 slots), sharing policy | line requests per instruction |
| address mappings, page placement | intra-instruction dedup of lines |
| Bulk window (8K or 16K), policy, engine | *nothing* |

The last row is the control that makes the comparison legitimate: the engine's
request count, window occupancy, and DRAM order are identical in all four widths
(`bulk_idx_lines_req` = 15 766 / 28 960 / 1 588 / 99 347 and `bulk_tgt_req_gen` =
3 431 / 2 212 / 560 / 111 288, invariant), so any change in the hybrid's cycles is
a change in how the *SIMT half* interacts with the shared machine.

## 2. Where the 4-lane advantage comes from

Two separate structural wins, and it matters that they are separate:

**(a) The index stream is request-amplified in SIMT.** The engine reads the index
array as a dense line stream: one request per 64 B = one request per 16 indices,
252 241/16 = 15 766 requests for bcsstk25, all of them compulsory misses. SIMT
reads the same bytes through a coalescer that groups *consecutive accesses in the
row's access stream*: with 4 lanes a group is 16 B, so four requests per line, and
even a perfect 4-lane coalescer could not do better than 4× the engine's count.
Measured at 4 lanes: 79 573 requests, i.e. **5.05× the engine**.

**(b) The gather is deduplicated by the window, which SIMT cannot do at all.**
bcsstk25 issues 252 241 gathers of `x[j]`. SIMT forms them into 136 747 line
requests at 4 lanes. The engine's 8K window holds the target indices, merges
duplicates *and* sorts by line, and issues **3 431** requests — a 40× reduction,
and it is a different axis from (a): it does not improve with lane width at all.

The 4-lane cycle saving decomposes accordingly (bcsstk25):

| component | cycles | share |
|---|---:|---:|
| index stream removed from the SIMT path (`ΔA_idx`) | 152 984 | 100 % |
| engine's own index acquisition + merge + gather (`ΔC_eng`) | 81 791 | 53 % |
| **net surplus** | **71 193** | **47 %** |

So at the calibrated baseline, roughly half of the engine's win is the index
stream and half survives the engine's own overhead. Both halves are attackable by
wider warps — but only one of them actually is, which is the next section.

## 3. Why wider SIMT erodes the advantage

### 3.1 The prize: index request amplification falls

| dataset | engine index req | SIMT colidx req l4 | l8 | l16 | l32 | amplification l4 → l32 |
|---|---:|---:|---:|---:|---:|---|
| bcsstk25 | 15 766 | 79 573 | 52 011 | 38 073 | 30 570 | 5.05× → 1.94× |
| qc2534 | 28 960 | 137 199 | 83 080 | 57 208 | 43 084 | 4.74× → 1.49× |
| lnsp3937 | 1 588 | 9 095 | 6 569 | 5 280 | 5 280 | 5.73× → 3.32× |
| dubcova3¹ | 99 347 | 495 868 | 319 097 | 217 058 | 158 439 | 4.99× → 1.59× |

Wider warps recover most of the index-stream penalty: 16 lanes × 4 B = exactly one
64 B line, so a perfectly aligned 16-lane index group costs the same one line the
engine pays. The measured floor is not 1.0× because SIMT groups are formed over the
*access stream*, which restarts at every row boundary and starts at arbitrary
alignment: a group that begins at byte offset 4·j with j ≢ 0 (mod 16) straddles two
lines. That fragmentation is why amplification settles at 1.5–3.3× instead of 1×,
and it is a genuine property of row-structured SpMV, not a model artifact.

This is the effect the brief asked about, and it is large: `ΔA_idx` for qc2534
falls 315 903 → 248 407 (−21 %) between 4 and 8 lanes alone.

### 3.2 The price: the engine's overhead does not fall, it rises

`ΔC_eng` (bcsstk25: 81 791 → 111 376, +36 %; qc2534: 281 462 → 290 724, +3 %) is
made of the engine's own index acquisition, its merge, and its stalls. None of
those get cheaper when SIMT widens — and two of them get worse:

| counter (qc2534, 2×8K) | l4 | l8 | l16 | l32 |
|---|---:|---:|---:|---:|
| `bulk_idx_wait_stall` (engine waiting for index data) | 140 615 | 108 263 | 83 461 | 84 252 |
| `bulk_slot_stall` (engine waiting for residency slots) | 348 704 | **696 816** | 660 554 | 660 373 |
| `bulk_win_full_stall` (window ready but cannot merge) | 86 303 | 94 176 | 151 441 | 151 609 |
| `bulk_ag_cycles` (cycles the engine is active) | 484 276 | 861 007 | 950 360 | 951 187 |
| C(full) cycles | 923 790 | 861 007 | 950 360 | 951 187 |
| index phase (first index req → last index response) | 477 226 | 859 345 | 949 200 | 950 027 |

The engine's slot stall *doubles* at 8 lanes. The mechanism is that residency
slots are a single shared budget: a 4-lane instruction hands the machine one
16 B request at a time, while a 16- or 32-lane instruction must have all of its
lines admitted before the latch releases, so the SIMT client holds more slots
per instruction. The coalescer staging peak tracks this exactly — 4, 8, 13, 18
entries for 4, 8, 16, 32 lanes — and the engine, which needs slots to hold its
window's outstanding lines, is squeezed.

The visible consequence is the index phase. At 4 lanes the engine completes its
index acquisition and merge at cycle 484 276 of a 923 790-cycle run and then
idles behind the SIMT client; at 8 lanes and above `bulk_ag_cycles` equals
`C(full)` to the cycle — **the engine's own index acquisition becomes the critical
path**, so widening SIMT cannot shorten the hybrid at all. That is the direct
explanation of the flat `C(noidx)` column in §3 of the results report.

### 3.3 Why the crossing happens on qc2534 and not elsewhere

Two conditions must both hold for a crossing:

1. the index share of the baseline's runtime must be large enough that removing it
   was worth the engine's overhead, and
2. widening must destroy most of that share.

| dataset | index share of A at l4 | ΔA_idx loss l4→l8 | ΔC_eng change l4→l8 | outcome |
|---|---:|---:|---:|---|
| qc2534 | 41 % | −67 496 (−21 %) | +9 226 (+3 %) | **inverts at l8** |
| bcsstk25 | 33 % | −16 733 (−11 %) | +16 028 (+20 %) | survives, halved |
| lnsp3937 | 34 % | −896 (−5 %) | +1 439 (+17 %) | survives, −32 % |
| dubcova3¹ | 30 % | −89 296 (−8 %) | +46 501 (+9 %) | survives, −31 % |

¹ row-prefix experiment.

qc2534 has the largest index array (1.85 MB vs 101 KB / 123 KB), so its index
stream is the biggest single component of the baseline's time, and its long rows
(183 nnz/row) let 8-lane groups coalesce very effectively. It is the worst case
for Bulk on *both* axes, and that is where the sign flips. lnsp3937 has the
smallest working set (fits in L2), so its baseline barely improves with width and
Bulk keeps its win.

## 4. Index stream versus target gather, separated

The brief requires the two improvements to be reported separately. They are
separable in this model because the three-arm design measures each stream's cost
directly: A(full) − A(noidx) is the index stream's cost *in the baseline*;
C(full) − C(noidx) is the same work's cost *in the hybrid engine*; and the
difference between the two arms' noidx runs is the engine's gather-side behaviour.

| dataset | width | index cost, SIMT | index cost, engine | index saving | gather: SIMT req | gather: engine req | gather req ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| bcsstk25 | 4 | 152 984 | 101 954 | +51 030 | 136 747 | 3 431 | 40× |
| bcsstk25 | 8 | 136 251 | 12 577† | +123 674 | 121 010 | 3 431 | 35× |
| bcsstk25 | 16 | 132 543 | 38 717 | +93 826 | 110 689 | 3 431 | 32× |
| bcsstk25 | 32 | 133 710 | 37 603 | +96 107 | 105 572 | 3 430 | 31× |
| qc2534 | 4 | 315 903 | 195 728 | +120 175 | 157 917 | 2 212 | 71× |
| qc2534 | 8 | 248 407 | 135 770 | +112 637 | 107 961 | 2 212 | 49× |
| qc2534 | 16 | 240 027 | 225 099 | +14 928 | 86 060 | 2 212 | 39× |
| qc2534 | 32 | 240 217 | 225 986 | +14 231 | 74 114 | 2 212 | 34× |
| dubcova3¹ | 4 | 1 056 991 | 1 022 579 | +34 412 | 822 435 | 111 288 | 7.4× |
| dubcova3¹ | 8 | 967 695 | 803 379 | +164 316 | 722 093 | 111 288 | 6.5× |
| dubcova3¹ | 16 | 944 027 | 813 277 | +130 750 | 677 602 | 111 288 | 6.1× |
| dubcova3¹ | 32 | 944 348 | 827 583 | +116 765 | 643 051 | 111 288 | 5.8× |

† at 8 lanes the index stream costs the hybrid only 12 577 cycles because the engine
overlaps its acquisition with the (long) merge almost perfectly — `bulk_ag_cycles`
equals `C(full)` exactly at that width. At 16 lanes the slot stalls separate the two
phases again and the cost returns to ~38 000 cycles. The non-monotonicity is
measured, not smoothed over.

Reading it:

* **The index-stream advantage survives wide SIMT** — it is still 96 000 cycles on
  bcsstk25 at 32 lanes — because it is a *request-count* advantage and the engine's
  request count cannot change.
* **The gather advantage never shrinks in kind**: the engine issues 3 431 requests
  where SIMT issues 105 572 at 32 lanes, still 31×. Width improves the SIMT gather
  (136 747 → 105 572, −23 %) far less than it improves the SIMT index stream
  (−62 %), because gather addresses are data-dependent and a wider window cannot
  make them contiguous.
* **The merge's request advantage tracks how many windows' worth of gathers fit
in the column space.** An 8K window holds 8 192 gathers; with 2 534 columns
(qc2534) that is 3.2 windows' worth, so duplicates are abundant and the engine
merges 71×; with 15 439 columns (bcsstk25) it is 0.53 windows' worth, 40×; with
3 937 (lnsp3937), 2.1 windows' worth, 40×. On the dubcova3 prefix the column space
is 146 689, so 8 192 gathers cover 5.6 % of it and duplicate suppression nearly
disappears: **7.4× at 4 lanes, 5.8× at 32**. The merge is a locality win, not a
gather-rate win, and it degrades exactly where the matrix gets wide.

| dataset | columns | gathers per window ÷ columns | merged-request reduction |
|---|---:|---:|---:|
| qc2534 | 2 534 | 3.23 | 71× |
| lnsp3937 | 3 937 | 2.08 | 40× |
| bcsstk25 | 15 439 | 0.53 | 40× |
| dubcova3¹ | 146 689 | 0.056 | 7.4× |
* Yet the *cycle* advantage still disappears on qc2534. That is the whole finding:
the gather's request advantage is not worth many cycles when the gather is
L2-resident and cheap per request, while the index stream's requests are all
compulsory DRAM misses and the engine pays for them too.

So the advantage is not "Bulk is a better memory pipe". It is "Bulk converts an
expensive, DRAM-missing, request-amplified index walk into a dense stream, and
pays for it with a merge that a wide SIMT front end can partially match". Widen
SIMT and the first half of that trade keeps its value while the second half
becomes redundant.

## 5. What this analysis cannot support

1. **The shared slot pool is a modelling choice, not a GPU property.** The doubling
   of `bulk_slot_stall` at 8 lanes is a real, measured interaction *inside this
   model*, but a production accelerator would have its own request queue. The AB
   comparison deliberately gives both clients one residency budget so that neither
   is handed a private advantage; the side effect is that wide SIMT crowds out the
   engine. §6 bounds how much of the qc2534 loss this could account for: it is
   12–13 % of `C(full)` in absolute cycles and **zero of the verdict**.
2. **Nothing here is a 32-lane GPU prediction.** The 16- and 32-lane columns model
   formation only. A real 32-lane SM would also have wider LSU issue, more MSHRs,
   and likely a better coalescer than a strict 32-access window; all of those
   would *help the SIMT baseline*, i.e. push the same direction as the measured
   trend, but the magnitude is unvalidated.
3. **The engine's index phase equality with total cycles (l8+)** is a saturation
   state, not a prediction of how much a real design would overlap. It means the
   engine is never idle; it does not mean it is optimally scheduled.
4. **Dubcova3 is a row prefix.** Its absolute costs are not the full matrix's, and
   its index share is inflated by the prefix's row distribution (see
   `m20-combined-findings.md` §4).

## 6. Diagnostic bound on the pool coupling

The concern raised in §5.1 is that the loss at wide lanes is an artifact of both
clients sharing one residency budget. To bound it, the budget was doubled
(`pool_slots` 128 → 256) for qc2534 at 4 and 8 lanes, 2×8K, nothing else changed.
This is a **diagnostic, not part of the primary study**: no Bulk parameter, window,
or policy is touched, and it appears nowhere in the sweep tables.

| L | arm | @128 slots | @256 slots | Δ |
|---:|---|---:|---:|---:|
| 4 | A(full) | 762 503 | 762 503 | 0 |
| 4 | C(full) | 923 790 | 805 824 | −12.8 % |
| 4 | C(noidx) | 728 062 | 725 267 | −0.4 % |
| 4 | `bulk_slot_stall` | 348 704 | 299 592 | −14 % |
| 4 | **S_f** | **1.047** | **1.051** | +0.004 |
| 8 | A(full) | 682 956 | 682 956 | 0 |
| 8 | C(full) | 861 007 | 759 918 | −11.7 % |
| 8 | C(noidx) | 725 237 | 725 273 | +0.005 % |
| 8 | `bulk_slot_stall` | 696 816 | 470 498 | −32 % |
| 8 | **S_f** | **0.942** | **0.942** | **0.000** |

Doubling the budget does unblock the engine's index acquisition: `C(full)` falls
12–13 % and the index phase shortens from 859 345 to 626 774 cycles at 8 lanes. It
changes the verdict not at all, because the *noidx* path — the `S_f` denominator —
is untouched by the pool size (±0.4 %), and it is that path that the baseline
crosses at 8 lanes. The qc2534 inversion is therefore **structural**, not a
contention artifact: the hybrid's floor is ~725 000 cycles on this matrix whether
the pool is 128 or 256 slots, and 8-lane SIMT reaches 682 956.

What the probe does show is that the *price* half of §4 is inflated by the shared
pool: 12 % of the hybrid's `C(full)` at 8 lanes is contention that a private-queue
design would not pay. It does not rescue the index-stream half, which is the half
that mattered.
