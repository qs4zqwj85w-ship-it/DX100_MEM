# M9 E3 window-sealing policy: implementation report

Status: implemented, validated, benchmarked. E3 is the default sealing policy for
M9 runs; E0 remains selectable and still reproduces every earlier published
number.

Companions:

- `docs/m9-window-management-investigation-report.md` - defines E0-E4, measures
  them, and is where the premature-seal behaviour was identified.
- `docs/m9-double-buffer-balance-diagnostic.md` - producer/consumer timings that
  motivate E3.

---

## 1. What E3 changes, in one paragraph

Under E0 the partial-seal deadline (`batch_deadline`) starts counting when a
buffer is adopted for filling, so a window that is still actively receiving
elements is sealed anyway when the count expires. Under E3 the same
`batch_deadline` value counts cycles since the **most recent successful
admission** (and since allocation before the first one), so the bound measures
how long a window has been *waiting* rather than how long it has been *filling*.
A window that keeps admitting is never truncated; a window that stops admitting
is still sealed after `batch_deadline`.

Everything else about the window manager is untouched: same two buffers, same
`fill_` cursor, same adoption rule, same issue scheduler, same capacities.

## 2. Exact source changes

### 2.1 `sim/src/sim.hpp` - policy default and default resolution

`win_policy` no longer ships as `0`. It ships **unset** (`0xff`), and a new
`Config::effective_win_policy()` resolves it per run:

```cpp
  // 0xff = unset, which is the shipped state: `effective_win_policy()` then picks
  // E3 for an M9 run and E0 for every earlier milestone configuration, so the M6,
  // M7 and M8 results (and their regression anchors) keep the policy they were
  // measured with. An explicit --set win_policy=<0..3> overrides either default.
  u32 win_policy = 0xff;
  // The policy this run actually uses. M9 = the compiler-directed dispatch, which
  // is the configuration this engine is published as since M9; a bare dispatched
  // run counts as M9 too, so `--set dispatch=1` alone gets the M9 default.
  u32 effective_win_policy() const {
    if (win_policy != 0xffu) return win_policy;
    const bool m9 = dispatch != 0 || experiment == "m9" || experiment == "m9diag";
    return m9 ? 2u : 0u;  // E3 for M9, E0 for the M6-M8 configurations
  }
```

The comment block above the field was rewritten to state that **2 = E3 is the
default**, that **0 = E0 is the retained control** (and why it must stay
reproducible), and that 1 (E2) and 3 (E4) are never defaults.

The policy numbers themselves are unchanged (0 = E0, 1 = E2, 2 = E3, 3 = E4),
so `docs/m9-window-management-investigation-report.md` and the `build/wm`
scripts keep their meaning.

### 2.2 Why the default is resolved per run instead of flipped globally

Flipping the global default to `2` was tried first and measured. It fails 11
existing checks, all of them published M6/M7 anchors:

```
FAIL tests/tests.cpp:1886: r.b[0].cycles == 39322          (m6_w0_diagnostic)
FAIL tests/tests.cpp:1887: r.b[1].cycles == 38985
FAIL tests/tests.cpp:1894: r.b[0].tlb_refuse_parked == 20577
FAIL tests/tests.cpp:1922: r.b[0].cycles == 233475         (m6_w1_diagnostic)
FAIL tests/tests.cpp:1923: r.b[1].cycles == 233138
FAIL tests/tests.cpp:1933: r.b[0].win_refuse == 155501
FAIL tests/tests.cpp:2094: a_legacy == 233475              (m7_baselines)
FAIL tests/tests.cpp:2095: a_c2 == 156912
FAIL tests/tests.cpp:2096: a_c4 == 154807
FAIL tests/tests.cpp:2111: b_legacy == 39322
FAIL tests/tests.cpp:2112: b_c2 == 39530
4032 checks, 11 failures
```

That is requirement 6 ("do not change previously published M6/M7/M8 behavior")
being violated in the plainest possible way: those milestones were measured and
published under the E0 anchor, and their regression anchors assert it. A direct
driver measurement agrees - the M6 experiment at `batch_window = 8192`:

| M6 experiment run | cycles |
| --- | --- |
| default (E0 for `experiment=m6`) | 233 528 |
| `--set win_policy=2` (E3 forced) | 227 293 (-6 235, -2.7%) |

So the default is scoped: M9 runs get E3, the M6-M8 configurations keep E0, and
`--set win_policy=<n>` overrides both. The trigger is the *experiment/dispatch*,
not the \(dispatch\) flag alone, because `run_m9` builds arm A and arm B with
`dispatch = 0` (`ab.cpp:1186`, `ab.cpp:1202`) and arm C with `dispatch = 1`
(`ab.cpp:1221`); keying on `dispatch` alone would have given the bulk-only arm
inside an M9 run a different sealing policy from the dispatched arm.

### 2.3 `sim/src/bulk.cpp` - every decision reads the resolved policy

Nine read sites now use `effective_win_policy()`; the seal gate was made explicit
instead of being a chain of `cfg_.win_policy` comparisons:

Before (`win_tick`, step 6):

```cpp
    bool late = false;
    if (cfg_.win_policy <= 1) late = age;
    else if (cfg_.win_policy == 2) late = idle;
    else late = idle || cap;
```

After:

```cpp
    const u32 pol = cfg_.effective_win_policy();
    bool late = false;
    if (pol <= 1) late = age;        // E0/E1 (control), E2
    else if (pol == 2) late = idle;  // E3 (default for M9)
    else late = idle || cap;         // E4
```

The rest are one-line substitutions in `win_rec`/`bulk_stream` export
(`bulk.cpp:92`, `:94`), the E2 overlap gate (`:1424`), the E2 live-window check
(`:2027`), the buffered-entries bound (`:2120`), the buffer-count metric
(`:2386`) and the one-live-window gate (`:2475`). No window state, no buffer
count, no scheduler parameter changed.

### 2.4 `sim/src/util.cpp` - the echo says which policy actually ran

`Config::to_string()` now prints the effective value plus whether it was set:

```cpp
     << " win_policy=" << effective_win_policy()
     << (win_policy == 0xffu ? "(default)" : "(set)")
```

`bulk_win_policy` in the stats export likewise reports the effective value, so
every archived log says which sealing rule produced it.

### 2.5 Not changed

No new buffers, pools or capacities. `batch_window` (8 192), `batch_deadline`
(4 096), `bulk_max_elems`, `org_overlap`, `R`, `idx_reserve`, `ag_lanes`,
`eng_issue` are all untouched, and no tuning of any of them was performed for
this task. The SIMT engine, the index path, the descriptor path and the
dispatch/compaction logic are untouched.

## 3. Policy semantics as implemented

| Situation | Behaviour |
| --- | --- |
| Window reaches `batch_window` | seals immediately (all policies) |
| Window stops admitting for `batch_deadline` cycles | E3 seals it (E0: after `batch_deadline` from adoption) |
| Window only 4 184 of 8 192 elements when the element stream ends | seals at descriptor end (all policies) |
| No work available at all | no window is adopted, so nothing is sealed or discarded |
| `org_overlap = 0` | unchanged: no fill/drain overlap, independent of policy |

`batch_window` and `batch_deadline` keep their names, defaults and units; only
the *reference point* of the partial-seal comparison moved, which is precisely
the E3 definition.

Empty-window churn cannot be introduced by E3 because adoption is guarded on
`run_.gen_done < run_.d.count` (a window is only adopted when there is an element
to put in it), and the counters confirm it in every run below: `windows adopted
for filling` equals `windows sealed (non-empty)` and `discarded empty = 0`.

Forward progress at end of operation is preserved by the descriptor-end rule: the
final window is short and still seals, which is visible as
`sealed at descriptor end = 1` in every cell.

## 4. Validation

### 4.1 Test suite

```
cd sim && make -j4 && ./bin/dx100_tests
4032 checks, 0 failures
ALL CHECKS PASSED
```

Run after the final source state. This includes the M6/M7 regression anchors
listed in section 2.2, the M5/M5.1 cycle anchors (`280432`, `278034`, `270753`,
all window-off and therefore policy-independent), and the SIMT-only checks.

### 4.2 Default actually selects E3

No `--set win_policy` anywhere in the validation commands. The `[16]` block of
the M9 diagnostic prints the policy per arm:

```
  metric                                              B          C
  policy (E0=0, E2=1, E3=2, E4=3)             2          2
  partial-seal deadline (cycles)           4096       4096
  live-window limit                           2          2
```

Both B (`bulk only`, `dispatch = 0`, `experiment = m9diag`) and C (`dispatched`)
resolve to 2. The resolved value is also exported as `bulk_win_policy`.

### 4.3 E0 control reproduces the reference exactly

`--set win_policy=0`, 128 slots, seed 1:

| cell | A (SIMT) | B (bulk only) | C (dispatched) | published reference |
| --- | --- | --- | --- | --- |
| W1 \(\kappa=0.25\) | 220 123 | 145 032 | **153 475** | 153 475 |
| W1 \(\kappa=0.50\) | 169 826 | 131 199 | **119 798** | 119 798 |
| W1 \(\kappa=0.75\) | 120 110 | 118 689 | **95 194** | 95 194 |
| W0 \(\kappa=0.25\) | 11 052 | 17 522 | **17 160** | 17 160 |
| W0 \(\kappa=0.50\) | 21 291 | 26 299 | **24 804** | 24 804 |
| W0 \(\kappa=0.75\) | 30 581 | 34 075 | **31 594** | 31 594 |

Every E0 cell is bit-identical to the published M9 reference and to the
investigation report's E0 row. E0 still seals 3 windows with 2 partial seals at
\(\kappa = 0.25\) (`f0/p2/e1`), i.e. the premature-seal behaviour is still
reachable and still measurable.

### 4.4 Default (E3) reproduces the measured E3 results exactly

Same command with no `--set win_policy`:

| cell | A (SIMT) | B (bulk only) | C (dispatched) | vs E0 (C) | investigation report |
| --- | --- | --- | --- | --- | --- |
| W1 \(\kappa=0.25\) | 220 123 | 138 924 | **125 581** | **-27 894 (-18.2%)** | 125 581 |
| W1 \(\kappa=0.50\) | 169 826 | 127 138 | **94 035** | **-25 763 (-21.5%)** | 94 035 |
| W1 \(\kappa=0.75\) | 120 110 | 116 804 | **95 194** | 0 | 95 194 |
| W0 \(\kappa=0.25\) | 11 052 | 17 267 | **18 142** | **+982 (+5.7%)** | 18 142 |
| W0 \(\kappa=0.50\) | 21 291 | 26 311 | **24 832** | +28 (+0.1%) | 24 832 |
| W0 \(\kappa=0.75\) | 30 581 | 34 284 | **31 594** | 0 | 31 594 |

All six match the previously measured E3 numbers to the cycle.

### 4.5 SIMT-only is unchanged

The A column is identical under both policies in every cell (220 123 / 169 826 /
120 110 for W1; 11 052 / 21 291 / 30 581 for W0), and the completion hashes are
unchanged (`3510628737`, `2645928636`, `1417164640`). The value hash check
(`value hash 2377403185915931891 vs expectation 2377403185915931891, mismatched
addresses 0`) passes in every cell, so element conservation is independent of the
sealing policy.

### 4.6 Accounting, conservation, progress, churn

Per-cell window accounting from the same runs:

| cell | policy | adopted | sealed (non-empty) | full | partial | end | discarded empty | cycles with work but no filling window | cycles sealed but nothing adopting |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| W1 0.25 | E0 | 3 | 3 | 0 | 2 | 1 | 0 | 79 043 | 1 |
| W1 0.25 | E3 | 2 | 2 | 1 | 0 | 1 | 0 | 0 | 1 |
| W1 0.50 | E0 | 3 | 3 | 0 | 2 | 1 | 0 | 82 887 | 1 |
| W1 0.50 | E3 | 2 | 2 | 1 | 0 | 1 | 0 | 0 | 1 |
| W1 0.75 | E0 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | 1 |
| W1 0.75 | E3 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | 1 |
| W0 0.25 | E0 | 3 | 3 | 0 | 2 | 1 | 0 | 6 471 | 1 |
| W0 0.25 | E3 | 2 | 2 | 1 | 0 | 1 | 0 | 0 | 1 |
| W0 0.50 | E0 | 2 | 2 | 0 | 1 | 1 | 0 | 0 | 1 |
| W0 0.50 | E3 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | 1 |
| W0 0.75 | E0 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | 1 |
| W0 0.75 | E3 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | 1 |

- **Buffer accounting**: `adopted == sealed` in all 12 cells, `discarded empty =
  0`, `buffered entries <= bufs*W` and `window occupancy <= W` gates pass.
- **Element conservation**: the bulk gates `window admitted every element`,
  `window retired every element`, `lines issued + merged == elements`, `window
  records account for every admitted element` and `elements total == done +
  faults` pass in all 12 cells, as does the end-to-end value hash.
- **Forward progress**: `cycles with work but no filling window` is **0** under E3
  in every cell (it was 79 043 / 82 887 / 6 471 under E0). Removing the premature
  seal removes the periods in which elements were queued while no window was
  filling.
- **No churn**: no window is ever adopted without an element, none is discarded
  empty, and `cycles sealed but nothing adopting` is 1 (the cold start) in every
  cell.
- **Gates**: `M9 GATES PASSED` in all 12 cells.

## 5. The W0 \(\kappa = 0.25\) regression (+982, +5.7%)

### 5.1 What it is

| | E0 | E3 | delta |
| --- | --- | --- | --- |
| C cycles | 17 160 | 18 142 | +982 (+5.7%) |
| windows | 3 (2 partial + 1 end) | 2 (1 full + 1 end) | -1 |
| window records | `[4736/586/4682]` `[3968/4683/8779]` `[3616/15251/16194]` | `[8192/586/8200]` `[4128/8201/13325]` | |
| first target issue | 13 448 | 14 328 | +880 |
| last target issue | 16 316 | 17 182 | +866 |
| run end | 17 160 | 18 142 | +982 |

(n/alloc/seal per window; full records in the archived logs.)

### 5.2 It follows directly from the new deadline reference, and it is not tunable

E3 makes `batch_deadline` an *inactivity* bound. W0's index stream is fast enough
that admission never pauses for a full `batch_deadline`, so under E3 the deadline
never fires and the window always fills to `batch_window`. On W0
\(\kappa = 0.25\) that means the first target issue moves from cycle 4 682 (when
E0 truncates at 4 736 elements) to cycle 8 200 (when the window is full at 8 192
elements), i.e. 3 518 cycles later, and the remainder then issues 880 cycles
later than under E0.

The direct test of "is this the deadline, or something else" is to shrink the
deadline under E3. It changes nothing:

| `batch_deadline` under E3 | C cycles | window records |
| --- | --- | --- |
| 512 | 18 142 | identical |
| 1024 | 18 142 | identical |
| 2048 | 18 142 | identical |
| 3072 | 18 142 | identical |
| 4096 | 18 142 | identical |

Five different deadlines, byte-identical window timelines. That is the expected
behaviour of an inactivity bound: admission is continuous, so no value of
`batch_deadline` can trigger a seal while the window is filling. The W0 loss is
therefore not a threshold effect that could be recovered by retuning
`batch_deadline`; recovering it would require a *different policy* - a size or
total-age cap (that is what E4's `win_abs_cap` is) - which the investigation
already measured and which is out of scope here.

### 5.3 Why the trade is still the right way round

W0 is a constructed workload (`bulk_stream = 1`) in which the bulk stream never
stalls, i.e. the one regime where E3's bound is not doing any work. The cost is
+5.7% at \(\kappa = 0.25\), +0.1% at \(\kappa = 0.50\), 0 at \(\kappa = 0.75\), and
it is a consequence of *delaying* the first target issue on a stream that is
already saturating memory. W1 - the workload the engine is published on - gains
18.2% / 21.5% / 0% precisely because the premature seal was discarding 3 456 of
8 192 elements at \(\kappa = 0.25\) (`[8192/...]` under E3 versus a 4 736-element
first window under E0).

No other parameter was touched to offset the W0 number, and the W0 gap is
reported here rather than tuned away.

## 6. Preservation checklist

| Requirement | Evidence |
| --- | --- |
| E3 is the default sealing policy | `[16]` policy row = 2 with no `--set win_policy`; section 4.2 |
| E0 remains selectable and exact | `--set win_policy=0` reproduces 153 475 / 119 798 / 95 194 and 17 160 / 24 804 / 31 594 (section 4.3) |
| Two-buffer architecture kept | `live-window limit = 2`, `win_policy=1` still the only single-buffer mode; `model window SRAM = 147 584 B` under both E0 and E3 |
| No new buffers/pools/capacities | only the policy comparison changed; `batch_window`, `batch_deadline`, `bulk_max_elems`, `R` untouched |
| `batch_window`/`batch_deadline` semantics kept | same keys, defaults and units; only the partial-seal reference moved (section 3) |
| Full windows seal immediately | `sealed full = 1` at \(\kappa = 0.25\) under E3 (2 on the B arm) |
| End-of-operation partials still seal | `sealed at descriptor end = 1` in all 12 cells |
| Empty windows never sealed/discarded | `discarded empty = 0`, `adopted == sealed`, adoption guarded on remaining elements |
| SIMT baseline unchanged | identical A column and hashes (section 4.5) |
| M6/M7/M8 published behaviour unchanged | `dx100_tests` 4032/0 including the M6/M7 anchors; default resolution keeps E0 for those configurations (section 2.2) |
| Unrelated scheduling unchanged | no diff outside the policy comparison, its resolution and the log line |

## 7. Reproduction

Default policy (E3) - no `--set win_policy`:

```
cd DX100_MEM/sim
./bin/dx100sim --config configs/ab_controlled.cfg --set N=16384 --set Lambda=16384 \
  --set P=512 --set rho_dup=0 --set elem_bytes=4 --set bulk_wl=E0 --set seed=1 \
  --set batch_window=8192 --set bulk_max_elems=262144 --set max_cycles=200000000 \
  --set bulk_index_base=8388608 --set bulk_desc_line=98304 --set page_sched=1 \
  --set idx_mode=2 --set experiment=m9diag --set pool_slots=128 \
  --set kappa=0.25 --set bulk_stream=0
```

E0 control: append `--set win_policy=0`.

Regressions: `make -j4 && ./bin/dx100_tests`.

Archived logs for this report: `sim/build/wm/e3d_*.txt` (default/E3) and
`sim/build/wm/e0c_*.txt` (E0 control), plus `w0sw_*.txt` (W0 deadline sweep) and
`m6_def.txt` / `m6_e3.txt` (scoping measurement).

## 8. Limitations

- Single footprint (16 384 elements, `P = 512`), 128 slots, seed 1 for the tables;
  elements are 4 B and `rho_dup = 0`. The investigation's earlier multi-seed W1
  runs showed the same direction and are not re-measured here.
- W0 is a constructed workload; `bulk_stream = 1` is not a traced access pattern,
  so the +5.7% should be read as "on a bulk stream that never stalls, E3 gives up
  5.7% at \(\kappa = 0.25\)", not as a prediction for a real trace.
- The M6/M7 scoping is deliberate and measured (section 2.2), but it does mean the
  simulation has two default policies depending on the configuration. A user
  replaying an M6 command from `m6-bulk-window-implementation-report.md` gets E0
  (matching that report); a user running M9 gets E3. `--set win_policy=<n>` makes
  the choice explicit either way.
