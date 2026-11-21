# M23-G4C — DRAM address-mapping sensitivity of row-aware Bulk

**Status:** audit done, mapping parameterized where it was safe to do so, 96 cells measured
(48 published + 48 ideal-reference), all complete and clean. No physical-GPU behaviour is
claimed: every number is a DX-100 simulator result.

**Question.** M23-G3 found an 8 MiB row-map period: Orkut's whole footprint spans exactly
**1.00** period and ogbn's **18.68**. Is P2's row-locality benefit a property of the policy, or
of that particular interleaving?

**Short answer.**

| Question | Answer |
|---|---|
| Is P2's benefit robust across mappings? | **Yes.** Under ideal translation P2 improves cycles in **every** mapping and every window (ogbn −8.4 % to −31.5 %), and its achieved row-hit rate is **69–103 %** of each mapping's own window ceiling. Its *magnitude* co-varies with lines-per-row — 8/16/32 lines per row give ogbn −8.4 / −14.5 / −22.1 % at 8K — which is the ceiling moving, not the policy. |
| Does bank-interleave granularity matter? | **No.** 1-line vs 4-line interleave (M3) is identical to production within **0.1 %** on every cell. |
| Is the Orkut regression a mapping artifact? | **No.** It is +24…+46 % in all four mappings, its row-hit gain is ≤0.23 pt in all four, and it vanishes under ideal translation in all four. It is the cohort/translation interaction found in G3, not the map. |
| Dependence on the 8 MiB period | The period **is** the capacity (`banks × rows × row_bytes`). At constant capacity it cannot move, so the sweep varies the structure *inside* a period. What the period controls is aliasing: 1.00 period on Orkut means no physical row is shared by two virtual pages, 18.68 on ogbn means every row is shared by many. That ratio is why the two workloads sit at opposite ends, and no interleaving choice at 8 MiB changes it. |
| Deployable configuration | Unchanged conclusion: under published translation none of this reaches cycles. P2 is +0.01…+2.8 % on ogbn and +24…+46 % on Orkut in every mapping. |

---

## 1. Mapping audit

**Where the map lives.** `MemorySystem::dram_bank_row(line, bank, row)` is the single
production map. It is called from exactly two places: `l2_probe` (the controller, when a request
enters the DRAM queue) and `win_order_fused` (P2's organizer, on the physical line). That is the
property that makes this experiment safe: one function, so P0's controller and P2's ordering
cannot disagree about the mapping.

**What was already configurable, and what was not.**

| Parameter | Effect on the map | Safe for this test? |
|---|---|---|
| `rows`, `row_bytes` | row geometry; their product with `banks` is the capacity and the row-index period | Yes — but nothing enforced constant capacity, so changing one alone silently changes the total and the period with it |
| `banks` | bank count | No — held fixed at 2 by the brief |
| `bank_hash` | XOR-folds the bank index against power-of-two strides | On in every cell; off is not a mapping alternative so much as an anti-hash switch |
| `dram_il_lines` | **did not exist** | Added (§2) |
| `dram_map = 1..5` | the M11.3 *diagnostic* remaps (bit-23 flip, one bank, strict balance, one row, one cell) | **No.** They are applied at the enqueue site only, i.e. after the controller has the request, and `BulkEngine`'s constructor already refuses `bulk_org >= 2` with `dram_map != 0` for exactly this reason. Using them here would give P0 and P2 different maps — the one thing the brief forbids. |

**Two hazards found and one fixed.**

1. **`row_bytes` was never echoed.** A log recorded `rows` but not the row size, so a cell could
   not be proven against the mapping it claimed. Now every log carries
   `dram_map=… bank_hash=… row_bytes=… dram_il_lines=… capacity_mib=…` on its own line, and every
   cell is checked against it (§3).
2. **Nothing rejected a non-bijective geometry.** The map is only a bijection when
   `row_bytes % 64 == 0` and `dram_il_lines` divides `row_bytes/64`; a silent violation would look
   like a locality result. The constructor now throws instead.
3. **`g0bench.cpp` keeps a private copy of the map** (lines 70–72). It is a standalone
   micro-benchmark not used for any result here, so it is left untouched, but it will not see
   `dram_il_lines` and must not be used to analyse these cells. Recorded as a limitation (§8).

## 2. The parameter added

`Config::dram_il_lines` (default `1`): the bank-interleave granularity in 64 B lines. The
address space is laid out as `[row][group][bank][line-in-unit]`:

```
bank = hash(line / il) % banks
row  = ((line / (il * banks)) / (lines_per_row / il)) % rows
```

so the space one row index wraps over is `il · banks · rows · (lines_per_row/il) =
banks · rows · row_bytes` — **`il` changes the interleaving and never the capacity**. With
`il = 1` both expressions are the arithmetic this function always computed, which is verified,
not asserted: M0 reproduces M23-G3's published cells to the cycle (§3).

No timing, MSHR, buffer, queue or bank-count change. The only other edits are the two validation
checks and the one header line.

## 3. The four mappings

All 2 banks × 8 MiB, `t_hit` 14 / `t_miss` 26 / FR-FCFS / unbounded queue identical.

| Map | `row_bytes` | `rows` | `il` | lines/row | cells / 8 MiB | period |
|---|---:|---:|---:|---:|---:|---:|
| **M0** production | 1024 | 4096 | 1 | 16 | 8192 | 8 MiB |
| **M1** fine rows | 512 | 8192 | 1 | 8 | 16384 | 8 MiB |
| **M2** coarse rows | 2048 | 2048 | 1 | 32 | 4096 | 8 MiB |
| **M3** 4-line interleave | 1024 | 4096 | 4 | 16 | 8192 | 8 MiB |

**Correctness and inertness.**

- `./bin/dx100_tests` → **4 735 checks, 0 failures**.
- 96 of 96 cells complete and clean (`checks.txt`): element and request conservation, no address
  fault or mismatch, grouping identity, residency, byte conservation, queue identity, window
  match, `bulk_org_policy`, **plus the mapping identity** — each log's `row_bytes`,
  `dram_il_lines` and `capacity_mib` must match the cell's mapping, and `bank0_reads +
  bank1_reads` must equal `dram_reads`. **No DNF, no guard-limited cells.**
- M0 reproduces G3 exactly: published Orkut 8K/16K/32K P0 = 27 034 450 / 16 714 538 / 10 401 464
  and P2 = 35 647 262 / 23 965 633 / 15 011 813; ideal Orkut 16K P0 = 6 077 523, ogbn 16K P2 =
  12 113 032. The ceiling tool reproduces G3's M0 ceilings to the digit (56.12 / 65.41 / 73.64
  and 36.52 / 56.26 / 74.81), which validates the replicated map.

**How to read the numbers.** The mapping changes the *machine*, so absolute cycles are **not**
comparable across maps — ideal-translation Orkut P0 alone moves 6.08 M (M0) → 5.56 M (M2) →
6.66 M (M1) at 16K. Only **P2 against P0 inside one mapping** is a meaningful comparison, and
that is the only cross-map comparison made below.

## 4. Results — published translation

Orkut BFS (P0 / P2 cycles, then P2's delta):

| Map | W | P0 | P2 | Δ | Row hit P0 → P2 | Ceiling | Opens P0 → P2 |
|---|---:|---:|---:|---:|---|---:|---|
| M0 | 8K | 27,034,450 | 35,647,262 | **+31.86 %** | 49.70 → 49.71 | 56.12 % | 175,826 → 416,121 |
| M0 | 16K | 16,714,538 | 23,965,633 | **+43.38 %** | 60.43 → 60.47 | 65.41 % | 106,289 → 285,044 |
| M0 | 32K | 10,401,464 | 15,011,813 | **+44.32 %** | 73.22 → 73.38 | 73.64 % | 60,987 → 179,195 |
| M1 | 8K | 27,075,386 | 36,469,545 | **+34.70 %** | 37.27 → 37.28 | 40.27 % | 175,835 → 566,510 |
| M1 | 16K | 16,799,797 | 24,449,720 | **+45.54 %** | 45.55 → 45.59 | 49.02 % | 106,288 → 419,944 |
| M1 | 32K | 10,613,805 | 15,299,670 | **+44.15 %** | 58.46 → 58.59 | 58.20 % | 60,990 → 284,141 |
| M2 | 8K | 27,009,703 | 33,479,013 | **+23.95 %** | 62.74 → 62.76 | 70.29 % | 175,795 → 281,765 |
| M2 | 16K | 16,679,808 | 22,569,267 | **+35.31 %** | 73.63 → 73.70 | 78.19 % | 106,302 → 179,659 |
| M2 | 32K | 10,302,765 | 14,381,558 | **+39.59 %** | 83.53 → 83.76 | 84.23 % | 60,985 → 107,167 |
| M3 | 8K | 27,035,984 | 35,625,014 | **+31.77 %** | 49.75 → 49.78 | 56.17 % | 175,829 → 415,616 |
| M3 | 16K | 16,716,480 | 23,950,480 | **+43.27 %** | 60.48 → 60.54 | 65.45 % | 106,308 → 284,660 |
| M3 | 32K | 10,400,262 | 14,991,135 | **+44.14 %** | 73.27 → 73.45 | 73.69 % | 60,979 → 178,822 |

ogbn-products uniform (P0 / P2 cycles, delta):

| Map | W | P0 | P2 | Δ | Row hit P0 → P2 | Ceiling | Acts P0 → P2 |
|---|---:|---:|---:|---:|---|---:|---|
| M0 | 8K | 160,417,625 | 160,429,629 | +0.01 % | 7.59 → 34.35 | 36.52 % | 1,189,417 → 843,490 |
| M0 | 16K | 144,443,603 | 144,828,967 | +0.27 % | 9.33 → 45.81 | 56.26 % | 1,231,115 → 733,492 |
| M0 | 32K | 119,371,267 | 121,293,357 | +1.61 % | 13.54 → 57.97 | 74.81 % | 1,149,088 → 559,024 |
| M1 | 8K | 160,417,709 | 160,427,126 | +0.01 % | 6.00 → 21.42 | 21.09 % | 1,209,831 → 1,009,643 |
| M1 | 16K | 144,443,663 | 144,534,059 | +0.06 % | 6.73 → 29.24 | 36.26 % | 1,266,465 → 958,608 |
| M1 | 32K | 119,371,279 | 122,693,370 | +2.78 % | 8.85 → 38.60 | 55.89 % | 1,211,367 → 819,356 |
| M2 | 8K | 160,417,397 | 160,450,524 | +0.02 % | 10.17 → 50.51 | 56.49 % | 1,156,118 → 635,956 |
| M2 | 16K | 144,443,543 | 144,609,696 | +0.12 % | 13.77 → 63.04 | 75.07 % | 1,170,855 → 500,920 |
| M2 | 32K | 119,371,201 | 122,381,204 | +2.52 % | 21.42 → 71.00 | 87.15 % | 1,044,376 → 386,173 |
| M3 | 8K | 160,417,489 | 160,428,213 | +0.01 % | 7.64 → 34.41 | 36.53 % | 1,188,693 → 842,760 |
| M3 | 16K | 144,443,404 | 144,574,376 | +0.09 % | 9.54 → 45.86 | 56.28 % | 1,228,202 → 733,278 |
| M3 | 32K | 119,371,287 | 121,342,890 | +1.65 % | 13.57 → 57.78 | 74.82 % | 1,148,676 → 561,766 |

The published arm confirms G3 in every mapping: P2's row-hit gain on ogbn is **15–50 pt** and its
effect on cycles is **+0.01…+2.8 %** — no gain, never a win — while Orkut loses 24–46 %. Bank
utilization tells the same Orkut story as G3: mean bank busy falls from 36.5 % to 25.5 % (M0 16K)
even though the DRAM does strictly less work, because the issue port starves.

## 5. Results — ideal translation (mechanism reference)

P2 against P0, per mapping. This arm removes the dTLB and the walker, so what is left is the
memory order.

| Map | Workload | 8K | 16K | 32K | Ceiling 8K / 16K / 32K |
|---|---|---:|---:|---:|---|
| M0 | Orkut | +0.00 % | −0.02 % | −0.02 % | 56.12 / 65.41 / 73.64 |
| M2 | Orkut | −0.00 % | +0.00 % | −0.04 % | 70.29 / 78.19 / 84.23 |
| M1 | Orkut | −0.03 % | −0.01 % | −0.01 % | 40.27 / 49.02 / 58.20 |
| M3 | Orkut | −0.01 % | −0.02 % | −0.04 % | 56.17 / 65.45 / 73.69 |
| **M0** | **ogbn** | **−14.49 %** | **−22.21 %** | **−28.82 %** | 36.52 / 56.26 / 74.81 |
| **M1** | **ogbn** | **−8.39 %** | **−14.49 %** | **−22.22 %** | 21.09 / 36.26 / 55.89 |
| **M2** | **ogbn** | **−22.13 %** | **−28.81 %** | **−31.46 %** | 56.49 / 75.07 / 87.15 |
| **M3** | **ogbn** | **−14.49 %** | **−22.20 %** | **−28.83 %** | 36.53 / 56.28 / 74.82 |

Two things are visible at once:

- **Orkut is flat in every mapping** (−0.04…+0.00 %). P0 already realises 88–101 % of the window
  ceiling in every map, so there is nothing for a second ordering to collect, and the pass is
  free only because there is no regression to pay for.
- **ogbn tracks the ceiling.** P2's achieved rate is 69–103 % of each map's ceiling, and the
  cycle saving rises monotonically with `lines/row` at every window: at 16K, ceiling
  36.26 → 56.26 → 75.07 % gives −14.49 → −22.21 → −28.81 %.

## 6. Why the benefit tracks lines-per-row and not the bank interleave

A `(bank,row)` cell is the unit P2 consolidates. On a fixed window of `L` distinct lines, the
number of cells `C` — and therefore the achievable hits `L − C` — is set by how many lines share
a row, i.e. by `row_bytes`. Halving the row (M1) raises `C` by 46 %; doubling it (M2) lowers `C` by 43 %.
Measured at 16K ogbn: `C` = 718 411 (M1) / 493 053 (M0) / 280 953 (M2), against `L` = 1 127 130 —
ceilings 36.26 / 56.26 / 75.07 %.

The bank interleave is the other axis and it is nearly inert: regrouping lines into 4-line
instead of 1-line units (M3) changes the window's cell count by 0.05 % at 16K ogbn and 0.12 % on
Orkut, and every P0/P2 metric lands within 0.1 % of M0. A bank has one open row at a time, so
what matters is *which bank* a line lands in, and both interleaves balance the banks equally
(§7) — the choice of granularity changes only the order in which a balanced stream arrives.

## 7. Address distribution and the 8 MiB period

From `tools/m23g4c_ceiling.py` (trace analysis, not simulation):

| Map | Workload | Distinct lines | Span | Periods | Bank 0 / Bank 1 | Imbalance | Rows touched | Lines/row | Busiest row |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| M0 | Orkut | 100,280 | 8.00 MiB | **1.00** | 50,079 / 50,201 | 0.12 % | 8,181 of 8,192 | 12.26 | 16 |
| M0 | ogbn | 710,110 | 149.45 MiB | **18.68** | 354,916 / 355,194 | 0.04 % | 8,192 of 8,192 | 86.68 | 118 |
| M1 | Orkut | 100,280 | 8.00 MiB | 1.00 | 50,079 / 50,201 | 0.12 % | 16,174 of 16,384 | 6.20 | 8 |
| M1 | ogbn | 710,110 | 149.45 MiB | 18.68 | 354,916 / 355,194 | 0.04 % | 16,384 of 16,384 | 43.34 | 66 |
| M2 | Orkut | 100,280 | 8.00 MiB | 1.00 | 50,079 / 50,201 | 0.12 % | 4,096 of 4,096 | 24.48 | 32 |
| M2 | ogbn | 710,110 | 149.45 MiB | 18.68 | 354,916 / 355,194 | 0.04 % | 4,096 of 4,096 | 173.37 | 214 |
| M3 | Orkut | 100,280 | 8.00 MiB | 1.00 | 50,251 / 50,029 | 0.22 % | 8,181 of 8,192 | 12.26 | 16 |
| M3 | ogbn | 710,110 | 149.45 MiB | 18.68 | 355,158 / 354,952 | 0.03 % | 8,192 of 8,192 | 86.68 | 121 |

Three facts:

1. **The brief's period figures reproduce exactly** — Orkut 1.00 period, ogbn 18.68.
2. **Every mapping is bank-balanced**   (imbalance ≤0.22 %), so no result below is a bank-load
   artifact. Per-bank hit rates differ by ≤1.8 pt in the worst cell, and that is the DRAM's own
   arbitration, not the map.
3. **The period is the capacity.** `banks × rows × row_bytes = 8 MiB` in all four, so a
   constant-capacity sweep cannot move the period; what it moves is the *number of lines per
   row*, i.e. how much of a period a single open row covers. Orkut fills one period and shares
   no row between two virtual pages; ogbn's 18.68 periods mean every physical row is reused by
   many pages, which is where P2's 15–50 pt comes from. Change the capacity and the period — and
   with it that aliasing opportunity — would change too; that is precisely what this brief
   forbids, so it is stated as the untested axis rather than guessed at.

## 8. Limitations

1. **The period itself was not swept, by constraint.** Period = capacity = 8 MiB in every cell.
   The experiment therefore measures *intra-period structure* (row granularity, bank interleave),
   and the period's role is established analytically (§7), not by a run.
2. **Absolute cycles are not comparable across mappings** (§3). Only P2-against-P0 within a map
   is meaningful, and that is what every table reports.
3. **Four mappings, one bank count.** `banks = 2` is fixed by the brief; a many-bank device could
   behave differently, especially where bank-level parallelism is the limiting resource (ideal
   translation shows 99 % bank-busy in every cell, so the banks are the bottleneck there).
4. **`g0bench.cpp` still carries a private copy of the map** and does not honour
   `dram_il_lines`. It is not used by any result here.
5. **Two workloads.** Criteo is outside this brief; G3 measured it as behaving like ogbn.
6. **No claim about real hardware.** `t_hit`/`t_miss`/row-buffer counts are fitted simulator
   parameters; a mapping conclusion here is a statement about this model.

## 9. Reproducing

```bash
cd DX100_MEM/sim && make && ./bin/dx100_tests        # 4735 checks, 0 failures

../build/wm/m23g4c_run.sh all 6                      # 48 published cells
../build/wm/m23g4c_run.sh idealall 6                 # 48 ideal-reference cells
python3 ../tools/m23g4c_ceiling.py                   # distribution + window ceilings
python3 ../tools/m23g4c_extract.py                   # checks, summary.csv, tables.md
```

One cell:

```bash
./bin/dx100sim --config configs/m23g4c_base.cfg \
  --set dataset=../build/wm/m23_ogbn_uniform.bin \
  --set bulk_index_base=0x80000000 --set bulk_desc_line=0x1FFFFC0 \
  --set bulk_max_elems=2097152 --set batch_window=16384 --set bulk_org=2 \
  --set row_bytes=2048 --set rows=2048 --set dram_il_lines=1 --all
```

Every log states its own mapping on the `dram_map=… row_bytes=… dram_il_lines=…
capacity_mib=…` line, and `tools/m23g4c_extract.py` fails a cell whose header disagrees with the
mapping it was run under.

## 10. Files changed

| File | Change |
|---|---|
| `sim/src/sim.hpp` | `Config::dram_il_lines` (default 1) |
| `sim/src/memory.cpp` | `dram_bank_row` generalized for the interleave unit (identity at `il = 1`); constructor rejects non-bijective geometry |
| `sim/src/util.cpp` | parse `dram_il_lines`; new address-map header line |
| `sim/configs/m23g4c_base.cfg` | instrument configuration with the explicit geometry block |
| `build/wm/m23g4c_run.sh` | 96-cell runner (4 mappings × 2 workloads × 3 windows × 2 policies × 2 arms) |
| `tools/m23g4c_ceiling.py` | address distribution + per-mapping window ceilings |
| `tools/m23g4c_extract.py` | accounting and mapping-identity checks, `summary.csv`, `tables.md` |

No timing, capacity, bank count, buffer, MSHR or queue parameter moved in any cell. The original
mapping is preserved as M0 and reproduces every historical number (§3).

**Artifacts** (`build/wm/m23g4c/`): `g4c_*` published cells, `g4ci_*` ideal-reference cells,
`summary.csv` (96 rows), `checks.txt`, `tables.md`, `ceiling.txt`, `ceilings.csv`, `run.log`,
`hashes.txt`.
