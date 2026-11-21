# M28 architecture baseline — frozen

**Status: FROZEN at M28-F.** This document records the state the architecture was frozen at, so that any
later work can be checked against it rather than against memory. The frozen artefact is the source tree +
test suite + configuration + the behaviour and storage figures below. Nothing here is a proposal; it is a
snapshot with hashes.

Frozen on 2026-11-21, at the end of M28-F (`docs/m28-f-timestamp-storage-consolidation.md`).

---

## 1. What is frozen

| item | path | sha256 (first 16) |
|---|---|---|
| engine + hierarchy | `sim/src/sim.hpp` | `34488c4a8f9f3f51` |
| Bulk engine | `sim/src/bulk.cpp` | `30d57cb73b1d1773` |
| memory system | `sim/src/memory.cpp` | `6f7c45ac0436b7dd` |
| system / dispatch / metrics export | `sim/src/system.cpp` | `8f727ac35b1be0f1` |
| DRAM model | `sim/src/hbm.cpp` / `hbm.hpp` | `f3ecbc1e7ae823c3` / `f89025e80d6f172d` |
| driver | `sim/src/main.cpp` | `29868141d2f6bfe2` |
| workload generator | `sim/src/workload.cpp` | `670c7ca9933bf8f2` |
| A/B harness | `sim/src/ab.cpp` | `b8ffacf991824d9c` |
| unit + integration suite | `sim/tests/tests.cpp` | `68bdfe8e2bfff658` |
| build | `sim/Makefile` | `30381d26cd9b8248` |
| production config | `configs/m27_bulk.cfg` | `0848700987f2f6eb` |

Full manifest (all files, full digests):
`build/wm/m28_baseline/checksums.sha256`.

Verify with:

```
cd DX100_MEM && sha256sum -c build/wm/m28_baseline/checksums.sha256
```

(`util.cpp`, `workload.cpp`, `g0bench.cpp` and the five anchor logs are in the manifest too: 19 entries,
all `OK`.)

### 1.1 Frozen behaviour anchors

| log | sha256 (first 16) | meaning |
|---|---|---|
| `build/wm/m27/wl1/m27bulk_orkut.txt` | `9977895135b3b09f` | Bulk Orkut BFS anchor — unchanged since M27 |
| `build/wm/m27/wl1/m0rm_orkut.txt` | `eeda3265a7c6e1b8` | M0-RM Orkut BFS anchor |
| `build/wm/m28f/m28f_bulk_orkut.txt` | `9977895135b3b09f` | M28-F reproduction — **same digest as the anchor** |
| `build/wm/m28f/m28f_m0rm_orkut.txt` | `eeda3265a7c6e1b8` | M28-F reproduction — **same digest as the anchor** |
| `build/wm/m28f/m28f_bulk_org1.txt` | `34f932b073acc292` | sorter-enabled (`m24_dram_org = 1`) compatibility log |

The anchor and the M28-F reproduction sharing a digest is the whole behavioural claim in one line: the
consolidations from M28-D and M28-F changed no observable output, on 814 Bulk lines and 394 M0-RM lines.

## 2. Frozen configuration

`configs/m27_bulk.cfg`, production arm (`bulk_org = 8`, G6C = G6B driver + P0 admission):

```
window=16384 page_dir=512 page_cohorts=8 g6b_pa_capacity=4096 g6a_pa_capacity=65536
R=128 cores=1 warps_per_core=4 lanes=4 elem_bytes=4 eng_issue=4 walkers=8
dtlb=16/4 dtlb_parked=2 l2tlb=512/4 l1=256/4 l2=16384/8 l2_mshr=64
banks=2 rows=4096 row_bytes=1024 dram_q=0 frfcfs=1 t_hit=14 t_miss=26
m24_dram_org=0 m24_map=0 m24_perm_seed=1 bulk_org=8 win_fused=1 org_overlap=1
page_sched=1 dedup=1 page_order=1 sched_vis=0 sched_group=0 g6i_diag=0
cache_diag=1 dram_diag=1 idx_mode=2 idx_buf_lines=64
(reuse_dir not present in the file -> default 0, so the M13 reuse buffer is not allocated)
```

## 3. Frozen behaviour

Orkut BFS, `build/wm/m23_orkut_bfs.bin`, `--all`, measured boundaries as in M28-D:

```
optimized Bulk    6 131 444 cycles
M0-RM            17 904 629 cycles
speedup           2.9201x
completion_hash   1 284 131 032
conservation      OK
lat_tlb_sum       58 715 056      lat_tlb_mean 65
lat_sum           13 263 390 135  lat_cache_sum 13 204 675 079
bulk_g6b_tr_wait_dram 12 551 332 265   _max 39 034
bulk_g6c_dup_submit_blocked 0
suite             4 967 checks, 0 failures
```

## 4. Frozen storage

| figure | bytes |
|---|---:|
| functional Bulk storage (M28-D consolidation) | **677 536** |
| translation timestamps, one engine copy each (M28-F) | 262 144 |
| **allocated Bulk total** | **939 680** |
| window state / release path / engine control | 475 840 / 442 368 / 21 472 |
| shared hierarchy (excluded, M28-C §2.1) | 1 220 224 |

Excluded from every `bulk_*` term and from the functional total: uncharged window state 196 800 B,
`row_acts_`/`row_stamp_` 65 536 B (`dram_diag = 1`, diagnostic), `win_rec` 432 B. These are declared
functional structure, not peak heap and not silicon area — no synthesis figure is claimed anywhere in
M28-C/D/E/F.

## 5. Reproduction (one shot)

```
cd DX100_MEM/sim && make -j4 && ./bin/dx100_tests
./bin/dx100sim --config ../configs/m27_bulk.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin --all > /tmp/bulk.txt
./bin/dx100sim --config ../build/wm/m24d/m24d_simt.cfg \
  --set dataset=../build/wm/m23_orkut_bfs.bin --all > /tmp/m0.txt
diff /tmp/bulk.txt ../build/wm/m27/wl1/m27bulk_orkut.txt   # must be empty
diff /tmp/m0.txt   ../build/wm/m27/wl1/m0rm_orkut.txt      # must be empty
```

## 6. Change policy from here

* The hashes in §1 and the anchors in §1.1 are the reference. A change that alters any anchor is a
  **new architecture**, not a continuation of M28, and needs its own milestone and explicit authorization.
* Timing parameters, capacities, field widths, admission/release policy, DRAM scheduling and the
  statistics export are part of the frozen surface. Nothing may be narrowed, widened or re-timed silently.
* `build/wm/m27/`, `build/wm/m24d/`, `build/wm/m28d/` and `build/wm/m28f/` are history. They are read-only
  evidence; new results go in a new directory.
* Explicitly **not** part of this baseline and not authorised by the freeze: Candidate B (packed field
  widths), Candidate C, the E2 ready-pool depth curve, and any further storage consolidation.
* If a later milestone must move the baseline, write a new baseline document next to this one and leave
  this one intact — the point of a freeze is that it stays checkable.

No source, configuration or log was modified to produce this document; it records the state M28-F left.
