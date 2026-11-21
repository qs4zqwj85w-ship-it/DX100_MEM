# M23-B — Real Recommendation-System Embedding Lookup (Criteo)

First real-workload experiment under M23. Architecture, baseline and memory
system are exactly those frozen by M23-A; nothing was retuned.

---

## 1. Dataset and exact subset

| item | value |
|---|---|
| dataset | Criteo 1TB Click Logs (the public Criteo Terabyte dataset) |
| mirror | `huggingface.co/datasets/criteo/CriteoClickLogs` |
| file | `data/day=2015-02-15/part-00079-99c339d5-….c000.snappy.parquet` |
| file SHA-256 | `d0733ed6b4925d0d…` (full value in the trace JSON) |
| rows in file | 766 864 |
| **rows used** | **16 384** — the file's prefix, in file order |
| columns used | 26 categorical (32-bit hashed IDs) + 13 integer features |
| trace SHA-256 | `5a5c76b35ea19e50…` |

This is a **prefix of one shard of one day**, not the whole day and not the
24-day dataset. No record was resampled, permuted or replaced: the ID multiset,
the per-field cardinalities and the per-field frequency skews are the dataset's
own. The 1 TB corpus is not installable in a standalone simulator, and no claim
is made that this subset represents it.

Download was live and the range/shape was verified before use; the raw file is
kept at `data/criteo/` so the experiment is reproducible offline.

## 2. Workload construction

A recommendation model performs, per record, one embedding lookup per
categorical field. The 26 fields are **26 separate tables**, as the model
specification requires.

```
slot_f = id_f & (V_f − 1)                      hashing trick, V_f a power of two
V_f    = clamp(2^ceil(log2(8 · D_f)), 16, 2^20)  table size from the field's own
                                                 cardinality D_f in this prefix
addr   = table_base[f] + slot_f · 64           16 × float32 = 64 B = one line
```

Instruction shape: one **warp** (lanes = 4) looks up one field for four
consecutive records — four independent lines inside one table. Field identity is
therefore preserved at instruction granularity, which is what makes a
per-operation compiler decision expressible.

Program order per warp group: 26 embedding instructions (one per field), then one
**dense** instruction reading the 13 integer features of its four records
(contiguous, coalesced).

| quantity | value |
|---|---:|
| records | 16 384 |
| warp groups | 4 096 |
| instructions | 110 592 |
| **bulk-class instructions** | **73 728** (18 fields) |
| **simt-class instructions** | **36 864** (8 fields + dense) |
| total accesses | 638 976 |
| bulk elements | 294 912 |
| SIMT elements | 344 064 (131 072 table + 212 992 dense) |
| distinct lines / working set | 64 252 / **4.11 MiB** (4.1 × L2) |
| distinct pages | 10 670 |
| line reuse (duplicate-line fraction) | **0.899** |
| table region span | 64.0 MiB (26 tables, 2 MiB allocation granularity) |

### 2.1 Address layout

| region | base | size |
|---|---|---:|
| dense integer features | `0x0000_0000` | 832 KiB |
| 26 embedding tables | `0x0100_0000` | 64 MiB |
| descriptor | `0x7FFF_F000` (line 33 554 368) | 64 B |
| index array | `0x8000_0000` | 1.18 MB (294 912 × 4 B) |
| `data_base` | 0 | — |

`elem_bytes = 4`, `index_bytes = 4`, so the engine's index value is exactly
`address / 4` and no address is synthesised — every generated address is an
address the trace itself issues.

## 3. Compiler selection (reported per operation class)

The annotation rule uses only what a compiler and the model configuration can
see — the field's declared table size and the loop's independence — never runtime
behaviour:

* **SIMT** when `V_f · 64 ≤ 16 KiB` (one L1): the table is L1-resident after
  warm-up and Bulk could only add descriptor, index and fanout overhead.
* **Bulk** otherwise: independent 512 KiB–4 MiB tables, no spatial locality
  between consecutive records.

| decision | operations | fields |
|---|---:|---|
| SIMT | 8 fields + dense | 6 (16 rows), 9 (256), 13 (128), 16 (512), 17 (32), 19 (128), 25 (512), 26 (256), plus the dense block |
| Bulk | 18 fields | 1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 14, 15, 18, 20, 21, 22, 23, 24 |

Nothing is forced to Bulk and nothing is forced to SIMT: 18 of 27 operation
kinds per record go to the accelerator, 9 stay on the SIMT path. The
`m23_select` audit trail records every decision with its class, element count,
metadata source (`compiler-annotation`) and rule
(`independent-irregular-gather` / `regular-or-localised-coalesced`).

## 4. Four-configuration results

Identical trace, identical layout, identical memory system. Only the context
capacity differs.

| | M0 | DX 2×8K | DX 2×16K | DX 2×32K |
|---|---:|---:|---:|---:|
| **cycles** | **28 511 774** | **15 778 298** | **12 273 324** | **8 942 139** |
| **speedup vs M0** | 1.000 | **1.81×** | **2.32×** | **3.19×** |
| page walks | 190 076 | 104 877 | 81 493 | 59 300 |
| pte fetches | 380 152 | 209 754 | 162 986 | 118 600 |
| L2-TLB reads / hits | 322 090 / 132 014 | 124 539 / 9 189 | 101 389 / 9 238 | 79 575 / 9 392 |
| walker busy (2 walkers) | 57 022 800 | 31 473 573 | 24 458 558 | 17 800 883 |
| L1 reads / hits (all) | 360 326 / 125 866 | 241 582 / 81 000 | 224 877 / 80 859 | 209 361 / 80 577 |
| L2 reads / hits (all) | 614 590 / 527 697 | 368 043 / 264 595 | 304 688 / 200 687 | 244 970 / 137 495 |
| L2 evictions | 70 502 | 86 798 | 87 233 | 90 291 |
| DRAM reads | 86 886 | 103 182 | 103 617 | 106 675 |
| DRAM row hits | 5 190 | 30 477 | 32 120 | 35 433 |
| DRAM row conflicts | 81 694 | 72 703 | 71 495 | 71 240 |
| DRAM row activations | 81 696 | 72 705 | 71 497 | 71 242 |
| DRAM busy cycles | 2 196 756 | 2 317 008 | 2 308 602 | 2 348 354 |
| peak resident (of 128) | 128 | 128 | 128 | 128 |
| `conservation` | OK | OK | OK | OK |

### 4.1 Per-requester traffic (M15 attribution, `cache_diag`)

| requester | M0 | 8K | 16K | 32K |
|---|---:|---:|---:|---:|
| SIMT L1 reads / hits | 360 326 / 125 866 | 98 312 / 81 000 | 98 312 / 80 859 | 98 312 / 80 577 |
| SIMT L2 reads / hits | 234 438 / 148 261 | 15 019 / 1 548 | 15 142 / 1 673 | 15 332 / 1 874 |
| index L1 reads (hits) | — | 18 432 (0) | 18 432 (0) | 18 432 (0) |
| target L1 reads | — | 124 838 | 108 133 | 92 617 |
| **target L2 reads / hits** | — | 124 838 / **54 323** | 108 133 / **37 373** | 92 617 / **19 769** |
| DRAM reads: SIMT / target | 86 177 / 0 | 13 471 / 70 515 | 13 469 / 70 760 | 13 458 / 72 848 |

### 4.2 DX-100 internal cost accounting

| counter | 8K | 16K | 32K |
|---|---:|---:|---:|
| elements admitted / done | 294 912 | 294 912 | 294 912 |
| distinct lines issued | 124 837 | 108 132 | 92 616 |
| elements merged onto an existing line | 170 075 (57.7 %) | 186 780 (63.3 %) | 202 296 (68.6 %) |
| window batches (seals) | 36 | 18 | 9 |
| index lines requested | 18 432 | 18 432 | 18 432 |
| target-issue dTLB refusals | 0 | 0 | 0 |
| index-stream dTLB refusals | 242 445 | 238 593 | 214 717 |
| organizer cycles | 147 456 | 147 456 | 147 456 |
| fanout cycles | 150 155 | 142 039 | 132 777 |
| request-buffer peak (of 128) | 72 | 73 | 75 |
| fused window bytes (measured) | 194 900 | 368 888 | 713 886 |
| modelled charge (`win_bytes`) | 147 584 | 278 656 | 540 800 |

**Context-buffer occupancy** (mean entries held across both buffers, from
`bulk_win_occ_sum / cycles`):

| | 8K | 16K | 32K |
|---|---:|---:|---:|
| mean occupancy | 16 113 | 31 740 | 61 559 |
| capacity | 16 384 | 32 768 | 65 536 |
| **utilisation** | **98.3 %** | **96.9 %** | **93.9 %** |

All three configurations run their context buffers essentially full for the
whole run. The engine is capacity-bound, not merely ordering-bound — which is the
structural reason the larger windows keep paying (§Q6) rather than saturating.

**Dispatch / metadata overhead.** The simulator charges the selection for its
metadata in the only places it models it: one descriptor fetch (1 line) and the
index array traffic (18 432 lines, 294 912 × 4 B). It has **no separate
per-instruction dispatch cost**, so the cycles a real core would spend branching
on the annotation are not charged to DX-100. That omission favours DX-100 by a
small, unquantified amount and is listed in §8.

## 5. Per-field results

Trace-level, computed from the generated trace (the simulator's cache and DRAM
counters are aggregate — see §8).

| fld | class | V rows | table KiB | accesses | distinct lines | line reuse | pages |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | bulk | 65 536 | 4 096 | 16 384 | 5 312 | 0.676 | 1 019 |
| 2 | bulk | 32 768 | 2 048 | 16 384 | 2 391 | 0.854 | 509 |
| 3 | bulk | 32 768 | 2 048 | 16 384 | 3 460 | 0.789 | 511 |
| 4 | bulk | 16 384 | 1 024 | 16 384 | 1 055 | 0.936 | 252 |
| 5 | bulk | 32 768 | 2 048 | 16 384 | 2 119 | 0.871 | 502 |
| 6 | simt | 16 | 1 | 16 384 | 2 | 1.000 | 1 |
| 7 | bulk | 32 768 | 2 048 | 16 384 | 2 297 | 0.860 | 504 |
| 8 | bulk | 8 192 | 512 | 16 384 | 580 | 0.965 | 125 |
| 9 | simt | 256 | 16 | 16 384 | 21 | 0.999 | 4 |
| 10 | bulk | 65 536 | 4 096 | 16 384 | 4 912 | 0.700 | 1 016 |
| 11 | bulk | 32 768 | 2 048 | 16 384 | 2 611 | 0.841 | 510 |
| 12 | bulk | 65 536 | 4 096 | 16 384 | 4 296 | 0.738 | 1 009 |
| 13 | simt | 128 | 8 | 16 384 | 9 | 0.999 | 2 |
| 14 | bulk | 8 192 | 512 | 16 384 | 539 | 0.967 | 128 |
| 15 | bulk | 16 384 | 1 024 | 16 384 | 1 400 | 0.915 | 254 |
| 16 | simt | 512 | 32 | 16 384 | 34 | 0.998 | 8 |
| 17 | simt | 32 | 2 | 16 384 | 4 | 1.000 | 1 |
| 18 | bulk | 2 048 | 128 | 16 384 | 219 | 0.987 | 32 |
| 19 | simt | 128 | 8 | 16 384 | 13 | 0.999 | 2 |
| 20 | bulk | 65 536 | 4 096 | 16 384 | 5 419 | 0.669 | 1 024 |
| 21 | bulk | 65 536 | 4 096 | 16 384 | 4 036 | 0.754 | 1 002 |
| 22 | bulk | 65 536 | 4 096 | 16 384 | 5 193 | 0.683 | 1 020 |
| 23 | bulk | 32 768 | 2 048 | 16 384 | 2 425 | 0.852 | 505 |
| 24 | bulk | 32 768 | 2 048 | 16 384 | 2 536 | 0.845 | 510 |
| 25 | simt | 512 | 32 | 16 384 | 36 | 0.998 | 8 |
| 26 | simt | 256 | 16 | 16 384 | 21 | 0.999 | 4 |
| dense | simt | — | 0.9 | 212 992 | 13 312 | 0.938 | 208 |

Bulk half: 50 800 distinct lines (3.25 MiB). SIMT half: 13 452 lines
(0.86 MiB). The two halves behave completely differently — the SIMT half is
almost all reuse inside a few KiB, the bulk half is 83 % line reuse inside
3 MiB — which is why one aggregate number would be misleading.

## 6. Analysis

### Q1. Does the larger context window expose useful locality?

**Yes, monotonically, with no sign of saturation.** Distinct lines issued fall
124 837 → 108 132 → 92 616 (−26 % from 8K to 32K) and merged elements rise
170 075 (57.7 %) → 202 296 (68.6 %) of all bulk elements. Batches fall 36 → 18 → 9,
so each window covers 2× / 4× more records. The 32K point is the best measured in
every one of these counters, so no upper bound has been found at 32K.

### Q2. Does page-aware organisation reduce translation overhead?

**Yes, and this is the dominant effect.** Page walks fall
190 076 → 104 877 → 81 493 → 59 300 (−69 % versus M0), L2-TLB reads fall
322 090 → 79 575 (−75 %), and `walker_busy` falls 57.0 M → 17.8 M cycles.

The mechanism is measurable, not inferred: issuing a window in page order turns a
program order that alternates between 26 tables on every record into runs of
same-page requests, so the dTLB/L2-TLB amortise a translation over a run instead
of re-missing it. Note the L2-TLB *hit rate* actually falls (41 % → 7.5 %) while
the *walk count* falls — the engine needs far fewer translation probes in total,
so most of the probes it does make are compulsory misses. Reporting the hit rate
alone would invert the conclusion.

### Q3. Does it improve cache-line reuse or cache hit rates?

**Line reuse, yes; hit rates, no — and the distinction matters.**

* Line reuse: 124 837 requests serve 294 912 elements at 8K, 92 616 at 32K.
* Target-stream L2 hit rate *falls* with window size: 43.5 % (8K) → 34.6 % (16K)
  → 21.3 % (32K), because a larger window merges duplicates *before* issue, so
  fewer later lookups survive to become L2 hits.
* SIMT L1 hit rate rises 34.9 % (M0, whole program) → **82.4 %** (DX), because the
  18 large tables have left the SIMT cache footprint: the SIMT half's entire
  working set is 0.86 MiB of small tables plus the dense block.

So the engine's benefit is "issue fewer requests", not "hit the cache more".

### Q4. Does it improve DRAM scheduling or row locality?

**Row locality yes, DRAM traffic no.**

| | M0 | 8K | 16K | 32K |
|---|---:|---:|---:|---:|
| row hits | 5 190 | 30 477 | 32 120 | 35 433 |
| row hits / reads | 6.0 % | 29.5 % | 31.0 % | 33.2 % |
| row activations | 81 696 | 72 705 | 71 497 | 71 242 |
| DRAM reads | 86 886 | 103 182 | 103 617 | 106 675 |
| DRAM busy cycles | 2 196 756 | 2 317 008 | 2 308 602 | 2 348 354 |

Row hits improve 5.9× and activations fall 13 %, but **DRAM reads rise 19–23 %**
because target requests bypass L1 by construction and each lookup fetches a line
that the SIMT path might have hit. The trade is favourable here only because DRAM
is 8 % of the run at M0 (2.2 M of 28.5 M cycles) and translation is the binding
resource. A bandwidth-limited machine would see this differently.

### Q5. Which benefits are responsible for the measured cycle change?

Measured, and unusually clean — **page-walk service is the entire difference**:

| | M0 | 8K | 16K | 32K |
|---|---:|---:|---:|---:|
| walks × 150 cycles | 28 511 400 | 15 731 550 | 12 223 950 | 8 895 000 |
| measured cycles | 28 511 774 | 15 778 298 | 12 273 324 | 8 942 139 |
| residual | 374 | 46 748 | 49 374 | 47 139 |

A walk costs 2 levels × 150 cycles, two walkers run in parallel, `walker_busy` is
2 × cycles in all four configurations (both walkers are busy essentially
always), and the predicted cycle count from walks alone matches the measurement
to within 0.3 %. **In this workload the machine is a page-walk throughput
machine**, so every cycle the engine saves is a walk it avoided.

Contributions, in measured order:

1. **fewer walks** (190 076 → 59 300): the whole 3.19×.
2. **fewer target requests** (294 912 elements → 92 616 lines, 68.6 % merged):
   reduces DRAM and L2 work, but those are secondary resources here.
3. **row locality** (6.0 % → 33.2 % row hits): visible in DRAM counters,
   immaterial to the critical path.
4. **SIMT L1 hit rate** (34.9 % → 82.4 %): helps the SIMT half's own traffic.

### Q6. Is 16K or 32K worth the extra storage?

Measured on this workload, **yes for 32K and marginal for 16K**:

| step | cycles | gain | fused storage | storage added | cycles saved per KiB |
|---|---:|---:|---:|---:|---:|
| M0 → 8K | 28.51 M → 15.78 M | 1.81× | 194 900 B | +194 900 B | 66 900 /KiB |
| 8K → 16K | 15.78 M → 12.27 M | 1.29× | 368 888 B | +173 988 B | 20 600 /KiB |
| 16K → 32K | 12.27 M → 8.94 M | 1.37× | 713 886 B | +344 998 B | 9 700 /KiB |

Both steps still pay, but with decreasing efficiency: the second doubling buys
27 % of cycles for twice the storage of the first. That is a real
diminishing-return curve, not a saturation — and it is workload-specific
(M23-A's synthetic `W1` showed 16K ≡ 32K because one buffer held the whole run).
**M23 must not conclude from this that 32K is the preferred capacity**; C and D
have to agree.

### Q7. Does compiler-directed selection keep unsuitable operations on SIMT?

**Yes, and the measurement supports the rule.** The eight SIMT-classified fields
have tables of 16–512 rows (1–32 KiB): together with the dense block they touch
13 452 lines, comfortably L1-resident, and the SIMT half hits L1 82.4 % of the
time. Sending those 131 072 lookups to the engine would have added them to a
294 912-element descriptor — no line could be merged (reuse 0.998 means the lines
are already cached, not absent), and the window would have spent capacity holding
requests that hit in L1. The dense block is contiguous and coalesced; the
accelerator has nothing to organise.

This is also why the SIMT side is not idle: it executes 54 % of all accesses and
does so efficiently.

### Q8. Are there tables where Bulk is slower than M0?

**Cannot be measured per table** with the current simulator: the engine executes
one descriptor covering all 294 912 bulk elements, and the cache/DRAM counters
are aggregate (§8). What can be said:

* The eight SIMT-classified fields are exactly the ones where Bulk would lose —
  L1-resident, 99.8–100 % line reuse, 2–36 distinct lines for 16 384 lookups. M0
  serves them at L1 latency; a descriptor would add index traffic and fanout for
  no reduction in requests.
* For all 18 bulk fields the aggregate is faster at every window size, but which
  of them individually recovers more than its share is not observable here. That
  is an open question for M23-E, not a result.

## 7. Correctness and accounting

| check | result |
|---|---|
| identical trace in all four | same file, same SHA-256, `accesses = 638 976` in every run |
| every bulk address equals its trace address | `bulk_addr_mismatch = 0` (all three DX) |
| partition covers every element | `simt_elems + bulk_elems = 638 976`; `dispatch_unaccounted = 0` |
| every request completes | `conservation = OK`, `conservation_violations = 0` |
| residency budget | `simt_peak_slot = 128` in all four; `slot_pool_total = 128` in DX |
| index traffic charged | 18 432 index lines requested (294 912 × 4 B / 64 B) in every DX run |
| descriptor fetch charged | 1 descriptor, `desc_fetch = 1` |
| determinism | rerunning each configuration reproduces its cycle count exactly (the `dram_diag` pass reproduced all four numbers) |
| M0 unchanged | `l1_mshr = 128`, no engine, `dispatch = 0` |

The completion *hashes* differ between configurations because they accumulate in
completion order, which is exactly what scheduling changes; element-level
equivalence is established by `bulk_addr_mismatch = 0` plus conservation, not by
a hash.

## 8. Limitations

1. **Per-table timing is not observable.** Cache, TLB and DRAM counters are
   aggregate; per-field numbers here are trace-level (accesses, distinct lines,
   pages, reuse). A per-table time split needs per-request tagging that M23-B did
   not add.
2. **One descriptor per run.** All 294 912 bulk elements execute as a single
   gather spanning 36/18/9 window batches. The engine's own batching is the unit
   of scheduling; a real program would submit many small descriptors. Record-level
   sequencing between descriptors is not modelled.
3. **One scalar read per lookup.** A lookup reads one 4-byte element of a 64 B
   embedding row, i.e. the *line* traffic of a real lookup is right but the payload
   is 16× smaller. Only line traffic and translation are affected by this.
4. **`bulk_max_elems` was raised to 1 048 576** for these runs (the descriptor
   must hold 294 912 elements). It is a programming limit, not a memory-system
   resource, and it is identical in all three DX-100 configurations. The config
   files themselves are unchanged.
5. **DRAM has no capacity model** (M23-A): the row index wraps at 8 MiB while the
   tables span 64 MiB, so several tables alias onto the same rows. Row-hit
   numbers are therefore optimistic for the larger tables.
6. **Walk cost is calibrated**, not measured (150 cycles/level, 2 walkers). Since
   the whole result is walk-bound, the absolute speedups scale with that
   calibration; the *relative* walk reduction is a structural result and does not.
7. **L2-TLB reach is 2 MiB against a 4.11 MiB working set and 64 MiB of tables.**
   This is why walks dominate. A machine with a larger reach would compress the
   speedups — that boundary is the honest scope of this result.
8. **High line reuse (0.899) is a Criteo property, not a modelling choice.** It
   flatters dedup/merging relative to a low-reuse workload; M23-C/D are the test
   of whether the benefit survives weaker reuse.

## 9. Reproducibility

```bash
cd DX100_MEM

# 1. dataset (48 MB parquet shard, kept in data/criteo/)
curl -sL -o data/criteo/criteo_day0_part00079.parquet \
  "https://huggingface.co/datasets/criteo/CriteoClickLogs/resolve/main/data/day%3D2015-02-15/part-00079-99c339d5-fbac-4110-9dcf-75453a61a5c1.c000.snappy.parquet"

# 2. trace (16 384 records, 26 tables, fused row = one cache line)
python3 tools/m23_build_criteo.py \
  --parquet data/criteo/criteo_day0_part00079.parquet \
  --records 16384 --out build/wm/m23_criteo_16k.bin

# 3. the four configurations
cd sim
W="--set dataset=../build/wm/m23_criteo_16k.bin --set bulk_index_base=0x80000000 \
   --set bulk_desc_line=0x1FFFFC0 --set bulk_max_elems=1048576"
for c in m0 dx8k dx16k dx32k; do
  ./bin/dx100sim --config configs/m23_$c.cfg $W --all \
    | tee ../build/wm/m23b_$c.txt
done

# 4. selection audit trail (any DX-100 configuration)
../build/wm/m23_run.sh dx8k $W    # writes build/wm/m23_sel_dx8k.tsv
```

Logs: `build/wm/m23b_{m0,dx8k,dx16k,dx32k}.txt` and the `_dramdiag` variants.
Trace and metadata: `build/wm/m23_criteo_16k.bin{,.json}`.

## 10. What this does and does not establish

**Establishes:** on a real recommendation workload with real categorical IDs, the
fused DX-100 window converts a page-walk-bound execution into a less
walk-bound one, 1.81× / 2.32× / 3.19× faster at 2×8K / 2×16K / 2×32K, with every
address accounted and no change to the memory system or to M0.

**Does not establish:** that the win is a cache or DRAM win (it is not, on this
workload); that 32K is the right capacity (one workload, and its storage cost is
2× the 16K point); that any individual table benefits more than another
(unobservable here); that the result survives weaker reuse or a larger TLB — that
is M23-C and M23-D.
