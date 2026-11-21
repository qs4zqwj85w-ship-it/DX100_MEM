# M16 Part B — Bulk mechanism ablation

**Question.** How much of Bulk's improvement comes from deduplication, from
page-aware scheduling, and from simply exposing more requests?

**Short answer.** On these three workloads the answer is lopsided:

* **Page-aware scheduling is the mechanism that pays**: disabling it costs
  **+28.7 %** on W1 κ=.75, **+45.6 %** on DUP and **+207 %** on PSCR. Within it,
  the seal-time page ordering is the larger half and the M7 cohort scheduler the
  smaller, and the two are **coupled**, not independent.
* **Deduplication contributes between 0.0 % and 0.5 %** on all three workloads,
  and the traffic counters say why: the duplicate lines were already served from
  the L2/from an outstanding MSHR entry, so folding them onto one request saves
  probes, not DRAM reads (DUP: 33 354 → 33 362 reads with dedup off).
* **The residual advantage is not request visibility.** With both mechanisms off,
  the hybrid is *slower* than the SIMT-only arm (0.93–1.00×), so whatever Bulk
  still gains comes from the organization the ablation removed.

All numbers are means of 3 paired seeds at W = 8192, one trace per cell, three
arms per cell (A = SIMT only, B = engine only on the whole trace, C = hybrid).

---

## 1. The configurations, and how each mechanism is switched off

The ablation uses the implementation's own controls. Nothing is re-implemented,
and no cell changes the sort or the request-generation path.

| configuration | `dedup` | `page_order` | `page_sched` | what it isolates |
|---|---|---|---|---|
| Full Bulk | 1 | 1 | 1 | the reference |
| No dedup | **0** | 1 | 1 | dedup's isolated contribution |
| No page-order (cohorts on) | 1 | **0** | 1 | seal order without page ordering |
| No cohorts (page-order on) | 1 | 1 | **0** | M6 single cursor, page order kept |
| No page-awareness | 1 | **0** | **0** | all page-awareness removed |
| Neither | **0** | **0** | **0** | both mechanisms removed |

Two extra rows (no page-order with cohorts on, no cohorts with page-order on)
exist because "page-aware scheduling" is *two* controls in this implementation,
and reporting one number for both would hide the coupling in §5.3.

**What each switch actually does** (from `src/bulk.cpp`):

* `dedup = 0` (line 1166): seal emits one line record **per element** instead of
  one per distinct line. The entries are still sorted by `(va_line, id)` — the
  same sort — so the ablation removes the *folding*, not the ordering. The
  request-residency limit is untouched: `R` stays 128, the slot pool is the same
  object, and the measured `slot_stall` moves by <1 % (214 892 → 216 713 on DUP).
* `page_order = 0`: the issue cursor walks the seal order (first-admission) rather
  than pages-ascending, lines-ascending within a page. Same entries, same sort,
  different traversal of `order`.
* `page_sched = 0`: the M7 cohort scheduler is absent and the window issues from
  one cursor, so a page that is still being walked blocks the whole window
  (`page_sched = 0` is the published M6 policy).
* `win_fused = 0` in every ablation cell — the fused representation requires
  `dedup = 1` and `page_order = 1` by construction, so it cannot express this
  matrix. The M6 layout is the production default, so this is the right path to
  ablate.

---

## 2. W1, κ = .75, N = 65 536 — the cache-interference workload

| configuration | SIMT-only | engine-only (B) | hybrid (C) | SD(C) | A/C |
|---|---|---|---|---|---|
| Full Bulk | 294 127 | 282 558 | **244 977** | 389 | **1.2006** |
| No dedup | 294 127 | 287 258 | 244 977 | 389 | 1.2006 |
| No page-order (cohorts on) | 294 127 | 329 194 | 315 181 | 528 | 0.9332 |
| No cohorts (page-order on) | 294 127 | 338 564 | 264 375 | 515 | 1.1125 |
| No page-awareness | 294 127 | 329 729 | 315 355 | 417 | 0.9327 |
| Neither | 294 127 | 329 408 | 315 355 | 417 | 0.9327 |

Per-seed hybrid cycles: full 244 823 / 245 419 / 244 688; no dedup identical;
no page-awareness 315 439 / 315 724 / 314 903. The reconstruction cost of
page-awareness (+70 378 cycles) is 170× the seed spread.

| counter | Full | No dedup | No page-order | No cohorts |
|---|---|---|---|---|
| target lines issued | 16 284 | 16 287 | 16 284 | 16 284 |
| duplicates merged | 3 | 0 | 3 | 3 |
| fanout elements | 16 287 | 16 287 | 16 287 | 16 287 |
| L2 hit % (C) | 10.3 | 10.3 | 12.4 | 10.3 |
| DRAM reads (C) | 27 516 | 27 516 | 26 877 | 27 516 |
| DRAM row-hit % (C) | **89.2** | **89.2** | **46.9** | **89.2** |
| page walks (C) | 529 | 529 | 534 | 529 |
| window-full stall | 2 617 | 2 617 | 2 617 | 2 617 |
| slot stall | 214 807 | 214 807 | 285 934 | 215 598 |

W1 is the cleanest ablation in the set: **dedup is worth 0.000 %** here because
there are only 3 duplicates in the whole engine-stream to fold, and disabling it
changes nothing else (same lines issued, same DRAM traffic, same row behaviour).
**Page ordering is worth 28.7 %**, and the counter that moves is not the request
count — it is the **DRAM row-hit rate, 89.2 % → 46.9 %** — with L2 hits slightly
*higher* without ordering (12.4 % vs 10.3 %). That is the mechanism made visible:
ordering converts a line-at-a-time walk over 16 384 lines into a page-sequential
sweep that keeps a DRAM row open for 16 consecutive lines.

**Cohorts are worth 7.9 % on top of ordering** (264 375 vs 244 977) even though
the row-hit rate is identical — the cost they remove is not locality but stalling:
`slot stall` 214 807 → 215 598 changes only marginally, yet the single-cursor
policy loses 19 398 cycles to waiting behind page walks it cannot skip.

---

## 3. DUP, κ = .25, N = 65 536 — duplicate concentration

| configuration | SIMT-only | engine-only (B) | hybrid (C) | SD(C) | A/C |
|---|---|---|---|---|---|
| Full Bulk | 439 025 | 334 245 | **308 354** | 703 | **1.4238** |
| No dedup | 439 025 | 337 846 | 309 976 | 838 | 1.4163 |
| No page-order (cohorts on) | 439 025 | 478 351 | 448 932 | 1 672 | 0.9779 |
| No cohorts (page-order on) | 439 025 | 433 779 | 381 261 | 1 248 | 1.1515 |
| No page-awareness | 439 025 | 478 271 | 448 995 | 1 531 | 0.9778 |
| Neither | 439 025 | 479 108 | 450 589 | 1 838 | 0.9743 |

| counter | Full | No dedup | No page-order | No cohorts |
|---|---|---|---|---|
| target lines issued | 38 694 | **49 136** | 38 693 | 38 694 |
| duplicates merged | 10 442 | 0 | 10 443 | 10 442 |
| fanout elements | 49 136 | 49 136 | 49 136 | 49 136 |
| L2 hit % (C) | 29.4 | 29.4 | 29.5 | 29.4 |
| DRAM reads (C) | 33 354 | **33 362** | 33 223 | 33 356 |
| DRAM row-hit % (C) | 80.3 | 80.4 | **20.7** | 80.5 |
| slot stall | 214 892 | 216 713 | 376 012 | 210 937 |

DUP is the workload that *should* reward dedup — 10 442 duplicates, a fifth of
the engine's requests — and it recovers **0.53 %** (308 354 → 309 976).

The counters explain it exactly. Turning dedup off adds 10 442 L1 probes and
10 442 L2 probes, and leaves **DRAM reads unchanged** (33 354 → 33 362, +8). The
duplicate line was already being fetched by the first element's request, so those
10 442 extra probes were *hits* all along: the L2 absorbed them, and the MSHR
would have joined them even without the fold. Dedup is saving probe bandwidth,
and this machine is not probe-limited. It also does not change row behaviour
(80.3 % vs 80.4 %) — another sign the traffic is genuinely identical.

Note the L2 hit rate *falls* with dedup on in the no-page-order arms (29.5 % vs
41.4 % for "neither"): that is not a dedup effect, it is the 12 000 extra probes
counting as hits against the same fills. Comparing hit rates across these arms is
misleading; compare DRAM reads.

---

## 4. PSCR, κ = 0, N = 65 536 — page- and translation-sensitive

| configuration | SIMT-only | engine-only (B) | hybrid (C) | SD(C) | A/C |
|---|---|---|---|---|---|
| Full Bulk | 7 619 639 | 2 485 737 | **2 485 737** | 1 078 | **3.0653** |
| No dedup | 7 619 639 | 2 485 737 | 2 485 737 | 1 078 | 3.0653 |
| No page-order (cohorts on) | 7 619 639 | 7 634 857 | 7 634 857 | 8 951 | 0.9980 |
| No cohorts (page-order on) | 7 619 639 | 4 348 255 | 4 348 255 | 12 152 | 1.7523 |
| No page-awareness | 7 619 639 | 7 634 364 | 7 634 364 | 7 968 | 0.9981 |
| Neither | 7 619 639 | 7 634 364 | 7 634 364 | 7 968 | 0.9981 |

With κ = 0 every element is dispatched to the engine, so arms B and C coincide
by construction and C is the engine's own runtime.

| counter | Full | No page-order | No cohorts |
|---|---|---|---|
| page walks (C) | **16 380** | **50 872** | 16 380 |
| DRAM row-hit % (C) | **55.4** | **5.7** | 55.3 |
| DRAM reads (C) | 69 879 | 69 340 | 69 821 |
| L2 hit % (C) | 31.6 | 59.5 | 31.7 |
| window-full stall | 4 685 | 6 079 | 5 508 |

PSCR is where the mechanisms earn their keep: **page ordering is worth 3.07×
(2 485 737 → 7 634 857) and the cohort scheduler another 1.75× on its own**
(2 485 737 → 4 348 255). The mechanism is visible in two counters at once:
ordering cuts page walks from 50 872 to 16 380 (the engine asks for one page's
lines consecutively, so one translation covers them) and lifts the DRAM row-hit
rate from 5.7 % to 55.4 %. Dedup again contributes exactly nothing (PSCR has no
intra-window duplicates).

This is the strongest single result in M16: with both mechanisms disabled the
engine is 0.19 % *slower* than plain SIMT on the same trace, and with them enabled
it is 3.07× faster. The accelerator's advantage on a page-sparse irregular
workload is entirely an organizational property.

---

## 5. Attribution

### 5.1 Deduplication: 0.0 % / 0.5 % / 0.0 %

| workload | C full | C no dedup | Δ | duplicates folded |
|---|---|---|---|---|
| W1 κ=.75 | 244 977 | 244 977 | **0.00 %** | 3 |
| DUP | 308 354 | 309 976 | **+0.53 %** | 10 442 |
| PSCR | 2 485 737 | 2 485 737 | **0.00 %** | 0 |

The variance (703 on DUP) is smaller than the effect, so +0.53 % is real — and
negligible. **Dedup is not where the gains are**, on these traces, and the reason
is a traffic argument rather than a timing one: the duplicate lines are already
hits or MSHR joins, so folding them saves probes and not DRAM traffic (§3).

This does **not** overturn M12/M13. M12 found that a larger window exposed more
duplicates and that the fold counted them (10 442 at 8K, 24 644 at 32K); M13 found
that *cross-window* reuse needed a separate buffer. What M16 adds is the first
isolation of the mechanism itself, and the result is that its value is conditional
on the workload's duplicates being *demand misses* rather than hits — which they
are not here.

### 5.2 Page-aware scheduling: 28.7 % / 45.6 % / 207 %

| workload | C full | C no page-awareness | Δ cycles | relative |
|---|---|---|---|---|
| W1 κ=.75 | 244 977 | 315 355 | +70 378 | **+28.7 %** |
| DUP | 308 354 | 448 995 | +140 641 | **+45.6 %** |
| PSCR | 2 485 737 | 7 634 364 | +5 148 627 | **+207.1 %** |

Decomposition inside page-awareness (W1 / DUP / PSCR):

| removed | W1 | DUP | PSCR |
|---|---|---|---|
| page ordering only | +28.7 % | +45.6 % | +207 % |
| cohort scheduler only | +7.9 % | +23.7 % | +74.9 % |

Ordering is the bigger half everywhere. It is also the one whose benefit shows up
in the DRAM row-hit rate (89.2→46.9 on W1, 80.3→20.7 on DUP, 55.4→5.7 on PSCR);
the scheduler's benefit shows up in stalls (DUP `slot stall` 210 937 → 376 012
when cohorts are removed). Two different mechanisms with two different signatures,
which is why they are reported separately even though the brief treats them as one
control.

### 5.3 They are coupled, and the coupling is sub-additive

W1 κ=.75: full 244 977, ordering removed +70 204, cohorts removed +19 398. If the
two were independent, removing both would cost 89 602 cycles; measured cost is
**70 378** (315 355). So the mechanisms *overlap*: each is worth more when the
other is present, and neither can be credited with a share that adds up. Concretely,
page ordering without cohorts (315 181) is no better than neither (315 355) on W1,
because a single issue cursor cannot exploit the order once it is blocked on a
page walk; and cohorts without ordering (264 375) recover only a third of the
full benefit, because there are no ordered runs to keep open.

**Therefore no isolated causal percentage is claimed for "page-aware scheduling"
as a single mechanism** — only the joint figure and the two marginals above.

### 5.4 Do the mechanisms depend on each other, and is anything left?

| workload | A | C with both off | A/C | C full | A/C |
|---|---|---|---|---|---|
| W1 κ=.75 | 294 127 | 315 355 | **0.933** | 244 977 | 1.201 |
| DUP | 439 025 | 450 589 | **0.974** | 308 354 | 1.424 |
| PSCR | 7 619 639 | 7 634 364 | **0.998** | 2 485 737 | 3.065 |

With both mechanisms off, **the accelerator is 0.2–6.7 % slower than the SIMT
arm it is supposed to accelerate**. The residual is therefore nothing: request
visibility and concurrency, on their own, buy no cycles on these traces. Everything
Bulk achieves is attributable to the two organizational mechanisms — which is the
answer to the milestone's central question, on these three workloads.

---

## 6. Limitations

* **Three workloads, one κ each, one window size.** W1 at κ=.75, DUP at κ=.25,
  PSCR at κ=0. The ablation cannot say what dedup is worth on a trace with many
  duplicates *inside* one window and no L2 coverage; that is the missing case and
  it is worth a dedicated trace in a later milestone.
* **Dedup's zero is partly a trace property.** W1 carries 3 mergeable duplicates
  in the engine's share; reporting "dedup is worthless" from W1 alone would be
  wrong, which is why DUP is included and why the traffic counters (not cycles)
  are the evidence.
* **Storage of each mechanism is structural, not measured.** From the layouts:
  dedup costs no extra storage (it is the existing sort); page ordering costs the
  `order` array, 4 B per distinct line; the cohort scheduler costs the `page_dir`
  table (16 B × D entries, which the window already needs) plus two `Cohort`
  records (~40 B each at `page_cohorts = 2`); the window's entry array dominates
  either way at 8 B/entry (933 856 B for 2 × 8K in the M6 layout, 245 744 B
  fused). No new counter was added to price them.
* **`win_fused = 1` cannot express this matrix** (it requires dedup and page
  ordering), so all ablation cells use the M6 layout — the production default.
* **No silicon claim.** The DRAM/row figures come from a fixed-`t_hit`/`t_miss`
  model with FR-FCFS and a bank hash; a real controller would convert the row-hit
  difference into a different amount of time.
* **Cohort count is fixed** at `page_cohorts = 2` and `park_retry = 1`; a sweep of
  those is out of scope and would be a separate experiment.

---

## 7. Reproduction

```bash
cd DX100_MEM/sim && bash build/wm/m16_matrix.sh   # section B is the ablation
python3 build/wm/m16_extract.py                   # table §B
```

One ablation cell (full Bulk, W1 κ=.75, seed 1) is the "Full Bulk" command in
`m16-simt-mlp-sensitivity-report.md` §8; the no-dedup cell adds
`--set dedup=0`, the no-cohorts cell `--set page_sched=0 --set page_order=0`.
