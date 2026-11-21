// DX100 standalone memory-system simulator — core types.
//
// Design: DX100_MEM/docs/standalone-simulator-design.md
// Baseline (SIMT) only; the bulk engine is deliberately absent.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <functional>
#include <queue>
#include <ostream>
#include <set>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>

namespace dx100 {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 LINE_BYTES = 64;
constexpr u64 NONE = ~0ull;
// Sv32 user VA space (4 GiB, 4 KiB pages). Every address check below is against
// this limit, so an index value cannot produce a wrapped address silently.
constexpr u64 VA_LIMIT = 1ull << 32;

// Front-end under test.
//   Baseline — the calibrated SIMT front-end (M1-M4).
//   Neutral  — the M4a reference issuer: same rules, an independently written
//              implementation, used to prove the baseline front-end introduces
//              no hidden asymmetry.
//   Bulk     — the M5 bulk indirect-access engine.
enum class Mode { Baseline, Neutral, Bulk };

// ---------------------------------------------------------------------------
struct Config {
  // topology
  u32 cores = 4;
  u32 warps_per_core = 8;
  u32 lanes = 32;
  u32 R = 128;  // total outstanding line requests (v3 §2.3)

  // caches
  u32 l1_lines = 256, l1_ways = 4, l1_latency = 2, l1_mshr = 16;
  u32 l2_lines = 16384, l2_ways = 8, l2_latency = 12, l2_mshr = 128;

  // translation
  u32 dtlb_entries = 16, dtlb_ways = 4, dtlb_mshr = 4, dtlb_parked = 2;
  u32 l2tlb_entries = 512, l2tlb_ways = 4, l2tlb_mshr = 8;
  u32 l2tlb_req_per_entry = 8, l2tlb_latency = 4;
  u32 walkers = 2, walk_cache = 16, walk_latency_per_level = 150, levels = 2;
  // M23-F1 diagnostic: ideal translation. When set, every access is translated
  // in the cycle it is submitted, using exactly the mapping tlb_fill installs
  // (ppn = vpn + PPN_OFFSET). No dTLB/L2-TLB lookup, no translation MSHR, no
  // walker, no walk latency and no PTE traffic. A theoretical bound for
  // separating translation cost from every other mechanism -- never a hardware
  // configuration. Off by default, so every historical run is byte-identical.
  bool ideal_translation = false;

  // dram
  u32 banks = 16, rows = 1024, row_bytes = 1024, t_hit = 24, t_miss = 56, link = 4, dram_q = 32;
  // M23-G4C: bank-interleave granularity, in 64 B lines. Lines are grouped into
  // `dram_il_lines`-line units and consecutive units rotate across banks, so a
  // row of `row_bytes` holds its lines in `row_bytes/64/il` groups per bank. 1
  // (the default and the production setting) reduces dram_bank_row to exactly
  // the arithmetic it had before this parameter existed.
  u32 dram_il_lines = 1;
  bool frfcfs = true;
  // M11.2: scheduler diagnostic only. When set, dram_tick accumulates the
  // per-decision evidence (row hits available / bank-blocked, wait times,
  // per-bank and per-row spread) and nothing else changes. Off by default, so a
  // run without --set dram_diag=1 executes exactly the code it executed before.
  bool dram_diag = false;
  // M11.3 diagnostic address map. 0 = production (unchanged). 1 = high-bit
  // decoupling, 2 = single bank, 3 = strict round-robin banks, 4 = single row
  // per bank, 5 = single bank and single row. Never set by a published config.
  u32 dram_map = 0;
  // XOR-fold the address into bank/row (Vortex VX_CFG_L2_BANK_HASH = 1). Without
  // it, a stride equal to the bank count aliases every request onto one bank.
  bool bank_hash = true;
  // M24-A: the controlled physical-mapping experiments. One function
  // (`dram_bank_row`) decodes every agent -- the controller, the row-open probe
  // and the engine's organization key -- and, for M3, one function
  // (`pa_page_of_vpn`) builds every physical address, so every arm sees one map.
  //   0  M0 historical (default): the production map, byte for byte
  //   1  M1 line-interleaved, no XOR fold:  bank = line % banks
  //   2  M2 XOR fold over four address bits:
  //        bank = (line ^ line>>5 ^ line>>11 ^ line>>17) % banks
  //   3  M3 seeded physical-page permutation, offsets preserved, decoded by M0
  // 0 is the default, so every historical run is unchanged.
  u32 m24_map = 0;
  u32 m24_perm_seed = 1;  // M3's permutation seed

  // Vortex LSU outstanding-load pool (VX_CFG_LSU_PENDING_SIZE, per core).
  // 0 = unlimited (the design's R budget alone bounds residency).
  u32 lsu_pending = 0;
  std::string core_map = "round_robin";  // round_robin | blocked
  Mode mode = Mode::Baseline;

  // ---- memory access path (M5.1) -------------------------------------------
  // Which downstream path a request takes. "native" is the historical
  // per-front-end behaviour (SIMT through its L1, engine straight to L2);
  // "l1" and "l2" force BOTH front-ends onto the same path, which is what a
  // controlled A/B comparison requires. Reported in every table.
  std::string mem_path = "native";  // native | l1 | l2

  // Experiment driver: "none" | "ab" | "ab_sweep" (m5.1 §8).
  std::string experiment = "none";
  // M18: skip the "bulk only over the whole trace" arm. Every published path
  // runs it, so the default keeps them byte for byte. A real application trace
  // is several streams at once, and gathering the sequential ones as if they
  // were irregular targets is not a configuration M18 compares; the three arms
  // under test are SIMT, hybrid 2x8K and hybrid 2x16K.
  bool skip_b = false;

  // ---- bulk engine (M5, design §3.2 / §4.4) --------------------------------
  // M27-P5: the SM whose L1 and dTLB the engine's requests enter. 0xffffffff
  // means "unset": resolved to 0 when cores == 1, and refused for cores > 1
  // rather than silently routing every engine request through core 0's L1.
  u32 eng_core = 0xffffffffu;
  // Addresses generated per cycle (design: AG_LANES = 8).
  u32 ag_lanes = 8;
  // Requests the engine may hand to the MemorySystem per cycle. The design's
  // slot-formation rate is 1/cycle; it is a knob so the limit can be shown to
  // be non-binding rather than assumed.
  u32 eng_issue = 1;
  // Outstanding index lines the address generator may keep in flight. 0 = the
  // whole R budget (default). The default is a measurement, not a preference:
  // a bounded window starts the target stream earlier but stretches the index
  // tail, and cost ~1% cycles on the bulk gather (report §6.3). A window is
  // still worth setting when the index array is much larger than R.
  u32 idx_window = 0;
  // Charge one line read for the descriptor itself (design §4.4). Turning this
  // off is an ablation, never the default.
  bool desc_fetch = true;
  // Diagnostic only (docs/m6-w0-diagnostic-report.md §4): the index values are
  // supplied from a side buffer, so the engine pays neither the index array's
  // traffic nor the dependency on its fills. This is not a realizable
  // configuration; it exists to separate the cost of *acquiring* indices from
  // the cost of *organizing* bulk accesses. Never a default.
  bool idx_oracle = false;
  // Descriptor validation limit on `count`.
  u32 bulk_max_elems = 65536;
  // "none" = no descriptor from the config (tests submit their own);
  // "E0" = trace gather with 4 B indices; "E1" = same with 8 B indices.
  std::string bulk_wl = "none";
  // M9 window-management investigation: trace *regularity* at fixed κ. Off (the
  // default, and every published run) keeps W1: the primary-class elements are a
  // random permutation of the line set. On gives W0: the same footprint, element
  // count, seed and κ, but the primary-class elements walk the line set in order,
  // so a window's elements are consecutive lines -- the coalescible case. κ still
  // selects the class mix, so W0 and W1 differ only in regularity.
  bool bulk_stream = false;

  // ---- M6: bulk visibility window (docs/m6-architecture-investigation.md) --
  // W: logical visibility, i.e. element entries a batch may hold. 0 = the M5
  //    FIFO path, byte for byte. W, R and page_dir are three independent
  //    resources: W bounds *pending elements*, R bounds *outstanding line
  //    requests*, page_dir bounds *distinct pages in one window*.
  u32 batch_window = 0;
  // ---- M24-B: page size and the selectable HBM hierarchy -------------------
  // `page_bytes` changes the whole translation path consistently (VPN
  // decomposition, PTE indexing, TLB tags and reach, page-walk bookkeeping,
  // cohort grouping, physical allocation), not just a constant.
  u32 page_bytes = 4096;            // 4096 (historical), 65536 or 2097152
  u32 page_shift() const;           // log2(page_bytes): 12, 16 or 21
  u32 page_line_bits() const;       // log2(page_bytes / 64): 6, 10 or 15
  u32 page_lines() const { return 1u << page_line_bits(); }
  // "ddr" is the historical two-bank model, unchanged. "hbm" selects the
  // M24-B hierarchy in hbm.cpp (hbm.hpp, design in build/wm/m24b/design.md).
  std::string dram_kind = "ddr";
  u32 hbm_stacks = 2, hbm_pcs = 16, hbm_bank_groups = 4, hbm_banks_pg = 4;
  u64 hbm_capacity = 4ull << 30;    // total bytes; rows per bank are derived
  u32 hbm_row_bytes = 1024;
  u32 hbm_map = 1, hbm_base_map = 1, hbm_perm_seed = 1, hbm_perm_page = 4096;
  u32 hbm_il_lines = 4;   // 256 B interleave granule for maps B/C
  u32 hbm_t_rcd = 12, hbm_t_rp = 12, hbm_t_ras = 28, hbm_t_cas = 12;
  u32 hbm_t_ccd_s = 2, hbm_t_ccd_l = 4, hbm_t_rrd_s = 4, hbm_t_rrd_l = 6;
  u32 hbm_t_burst = 4, hbm_t_faw = 0, hbm_t_refi = 0, hbm_t_rfc = 0;
  // D: page-directory capacity (distinct pages resident in one window).
  u32 page_dir = 512;
  // Organization bandwidth: entries visited per cycle (admission and sealing).
  u32 org_lanes = 4;
  // Elements reconstructed per cycle from a line fill. 1 makes fanout the
  // bottleneck on any workload with intra-line reuse (report §7.2).
  u32 fanout_lanes = 4;
  // Max cycles a batch may stay open before it is sealed (anti-starvation).
  u32 batch_deadline = 4096;
  // 1 = fill the next window while the current one issues (double buffered).
  bool org_overlap = true;
  // ---- Window-sealing policy (docs/m9-e3-window-policy-implementation-report.md,
  // docs/m9-window-management-investigation-report.md) ----------------------
  // `batch_deadline` is the bound in every policy; what differs is *which cycles
  // it counts* and whether a second window may be live. The window manager is
  // structurally sequential (one `fill_` cursor over two buffers), so "fill the
  // oldest incomplete window first" and "at most one incomplete window" need no
  // code: the policies below are the only distinctions the implementation admits.
  //
  //   2 = E3 admission-anchored deadline -- THE DEFAULT.
  //       The partial-seal bound counts cycles since the most recent successful
  //       admission (since allocation before the first), so it bounds *waiting*
  //       rather than *filling*: a window that is still receiving elements is not
  //       truncated. Full windows still seal immediately, descriptor-end windows
  //       still seal, and a stalled window is still sealed after `batch_deadline`.
  //       Fill/drain overlap is unchanged.
  //   0 = E0 CONTROL: the deadline counts cycles since the buffer was adopted for
  //       filling. This is the policy every M6/M7/M8 result and the published M9
  //       reference were measured with, so it is retained and selectable:
  //         --set win_policy=0
  //       It truncates a window that is still filling (measured: 4 736 of 8 192
  //       elements at 128 slots, W1 kappa = 0.25), which is what E3 fixes.
  //   1 = E2 one live window: no fill/drain overlap -- a new window is adopted
  //       only when no other buffer is live. Halves the window SRAM (one buffer).
  //       Never a default; retained for the investigation's comparison.
  //   3 = E4 = E3 plus an absolute ceiling of `win_abs_cap` x D on a window's
  //       total age. Never a default; measured identical to E3.
  //
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
  // Multiplier for E4's absolute age ceiling. Diagnostic; never read by E0-E3.
  u32 win_abs_cap = 4;
  // The two mechanisms are switchable independently so their contributions can
  // be measured separately (M6 §6, §10).
  bool dedup = true;       // fold elements sharing a line onto one request
  bool page_order = true;  // issue pages ascending, lines ascending within page

  // ---- M7: translation-aware page scheduling --------------------------------
  // page_sched = 0 keeps the M6 policy byte for byte: one cursor over the
  // sealed order, which stops at the first line the dTLB refuses, so the whole
  // target-issue port idles for the length of a page walk. page_sched = 1 keeps
  // `page_cohorts` page runs open and issues from whichever run the translation
  // layer will accept. This is a scheduling change only: no queue depth, TLB,
  // MSHR, walker, cache or DRAM capacity moves, and a refused line is simply
  // retried later instead of blocking the port.
  bool page_sched = false;
  // Page runs open at once. Each costs one 32 B descriptor (vpn, span, cursor,
  // retry deadline) per window buffer, reported as `bulk_sched_bytes` and
  // included in `bulk_win_bytes`. Bounded at 16.
  // Default 2 = dtlb_mshr / dtlb_parked: the number of pages whose translations
  // can be in flight at once, since each may park `dtlb_parked` requests. It is
  // derived from translation capacity, not fitted to W1 (report §6.3).
  u32 page_cohorts = 2;
  // Cycles a refused page run waits before it is re-probed. 1 = retry every
  // cycle (what the M6 policy does for its single page); larger values trade
  // retry traffic for a slower reaction to a completed translation. Never 0, so
  // a run can not be parked permanently: forward progress is structural.
  u32 park_retry = 1;

  // ---- M8: pipelined index acquisition --------------------------------------
  // 0 = the M6/M7 path (one implicit stream, values looked up in a completed-line
  //     map, strictly element order, index lines compete in the shared FIFO);
  // 1 = two bounded index tiles with explicit ownership (double buffering);
  // 2 = 1 + per-line readiness, so a ready line is consumed while an earlier one
  //     is still outstanding;
  // 3 = 2 + a dedicated index issue port with its own bound and arbitration.
  // ---- M23-G3: request organization inside the fused window ----------------
  // Which order the sealed window's line runs are issued in. The grouping (one
  // request per distinct line, all its elements served by that fill) is a
  // property of the fused representation and is not selectable; see
  // docs/m23-g3-bulk-locality-organization.md §2.
  //   0 = P0, the sealed order: ascending VA line (the published policy)
  //   1 = P1, cache-line reuse first: descending elements-per-line
  //   2 = P2, DRAM row first: ascending (row, bank, line) from the real map
  //   3 = P3, combined: (row, bank) then descending elements-per-line
  //   4 = diagnostic only, NOT a policy: grouping disabled (one request per
  //       element), the counterfactual P1 is measured against
  //   6 = M23-G6A, translation first and DRAM second: phase 1 translates every
  //       run of the sealed window through the real dTLB/L2 TLB/walker path and
  //       buffers its physical line; phase 2 releases the runs in the P2 order.
  //   7 = M23-G6B, streaming: whatever has translated is released in P2 order
  //       while translation continues; the ready buffer is the only coupling.
  //   8 = M23-G6C, M23-G6B with P0's admission policy: the same streaming
  //       pipeline, ready buffer and release path, but the window's page runs are
  //       admitted through P0's cohorts, which defer a refused page and try
  //       another instead of stopping admission on it.
  u32 bulk_org = 0;
  // M23-G6A physical-address buffer: one entry per run of the window being
  // translated, holding {physical line, translation cycle}. `g6a_pa_capacity`
  // is the size the experiment provisions; a window that needs more entries is
  // refused with an error rather than silently truncated, so a run can never
  // report a barrier it did not actually hold.
  u32 g6a_pa_capacity = 65536;
  // M23-G6B: streaming decoupling. `bulk_org = 7` translates a window while
  // releasing whatever has already been translated, so translation and DRAM
  // overlap. The ready buffer holds translated-but-unissued runs -- one entry
  // per run -- and its capacity is what bounds the translation/DRAM distance:
  // when it is full, translation admission stops until the DRAM scheduler
  // drains an entry. Declared here, never silently enlarged.
  u32 g6b_pa_capacity = 4096;
  // M23-G6F: scheduler candidate visibility. 0 keeps M23-G6E behaviour exactly:
  // the release pops the smallest-key translated run, so the DRAM scheduler only
  // ever sees what the residency budget admitted (~61 requests). N > 0 lets the
  // release compare up to min(N, |ready|) candidates and hand the DRAM's
  // preferred one (an open row first, then the smallest key) to the *same*
  // admission gate: one request is submitted, through L1, L2 and the L2 MSHR
  // file, exactly as before. Candidate visibility is not transaction
  // concurrency -- no outstanding limit is raised by this knob.
  u32 sched_vis = 0;
  // M23-G6G: release by physical (bank,row) group. 0 keeps the existing release
  // path exactly. 1 = the ready buffer is grouped by destination cell and the
  // cell with the most pending runs is drained in ascending line order before
  // another cell is chosen (one persistent group at a time). 2 = the same
  // grouping with one open group per bank, released alternately, so both banks
  // keep a row open (cross-bank interleaving). The group key is the physical
  // (bank,row) the DRAM will use -- never a virtual page. Read only by the G6B
  // streaming release (bulk_org 7/8); every other policy ignores it.
  // 3 = M23-G6H, G3 P2's own selection rule over the same complete population:
  // the cell holding the smallest physical (row,bank) key is drained, then the
  // next smallest -- P2's ascending (row, bank, line) applied to the translated
  // window instead of the sealed one. The difference between 1 and 3 is the
  // selection rule alone.
  u32 sched_group = 0;
  // M23-G6I: continuous cross-page pipeline diagnostic. 0 = off, which is the
  // state every earlier report was measured in: no scan, no storage, no counter.
  // 1 = sample the ready pool at most once per `g6i_sample_period` cycles and
  // record how many distinct virtual pages and physical (bank,row) cells it holds
  // at that instant, plus per-release page-switch and same-cell gap statistics.
  // Observation only -- no decision reads any of it, so `cycles` is identical with
  // the knob on and off (measured in the report).
  u32 g6i_diag = 0;
  u32 g6i_sample_period = 512;
  // M24-A: the explicit DRAM-organization factor. 1 (the default, and every
  // historical cell) means the sealed window's runs are ordered by physical
  // (row, bank, line), so the release order is the controller's own locality
  // order. 0 removes exactly that and nothing else: the sealed order -- ascending
  // line, P0's documented issue order -- becomes the release order, while the
  // window, the pool, the page cohorts, the address map, the caches, FR-FCFS and
  // the queue all stay as they are. Defined only for the streaming arms
  // (bulk_org = 7 or 8), enforced in the engine's constructor so no historical
  // policy can be silently reinterpreted.
  // M27-P4: the production Bulk path performs page organization only. `false`
  // means the sealed (ascending-line) order is the release order -- no physical
  // row/bank sort -- which is the shipped default from M27 on. `true` keeps the
  // M24-A physical-order organizer available for historical comparisons.
  bool m24_dram_org = false;

  // Modes 1-3 need a window (`batch_window > 0`); mode 0 is the published
  // M6/M7 configuration and is what every earlier report was measured with.
  u32 idx_mode = 2;
  // Index lines per tile (one tile = one buffer). Bounded at 64.
  u32 idx_buf_lines = 64;
  // Outstanding index lines the acquisition unit may keep in flight. These are
  // drawn from the same R pool as target requests: the bound rations the stream,
  // it does not add outstanding-request capacity.
  u32 idx_mshr = 64;
  // Slots of the R budget that target requests may not take while the tiled
  // acquisition unit is active: the index stream's guaranteed share, so its
  // claims are never starved by the target stream (measured: without it the
  // claims stall for 32 700 cycles on W1 and the run loses 4.8 %). This is a
  // partition of the existing budget, not extra capacity.
  u32 idx_reserve = 8;

  // ---- M13: bounded cross-window reuse buffer ------------------------------
  // A small set-associative store of *completed target lines*: tag = the VA line
  // the window groups on, data = the 64 B the fill returned. It is consulted
  // once per distinct line when a window is sealed and filled when that line's
  // fill arrives, so a hit retires a window's elements from bytes the engine
  // already holds instead of forming a request. That is the only form of reuse
  // that can be exploited here: an entry holding a tag and nothing else cannot
  // serve `output[i] = data[index[i]]`, because the data would have to be
  // fetched again -- and a fetch is exactly what reuse is supposed to remove.
  //
  // OFF BY DEFAULT. M10's published configuration is two 8K windows and nothing
  // else; with `reuse_dir = 0` none of the M13 code runs (the guards are `if
  // (reuse_on())`), so every earlier log and anchor is byte-identical.
  bool reuse_dir = false;
  // Sets x ways entries. The set index is the VA line modulo `reuse_sets`, which
  // is exact: the model's VA -> PA map is injective, so two elements on one VA
  // line are necessarily on one physical line (the same argument the window's
  // grouping already relies on).
  u32 reuse_sets = 256;
  u32 reuse_ways = 4;
  // Fold the tag into the set index (`line ^ line>>11 ^ line>>22`), exactly the
  // reason this model's DRAM bank index is hashed: a stride that is a multiple
  // of the set count would otherwise alias every line of the footprint onto a
  // fraction of the sets. 0 = plain `line % sets`, kept as an ablation because
  // the aliasing it causes is measurable (report §9).
  bool reuse_hash = true;
  // 1 = a lookup that finds the line still outstanding (requested by an earlier,
  // still-draining window) attaches to that request instead of issuing another;
  // 0 = completed-data hits only, the strictly simpler mechanism.
  bool reuse_merge = true;
  // Charge one organizer visit per lookup and one per insertion. 0 removes the
  // mechanism's modelled control cost: an ablation, never a default.
  bool reuse_charge = true;
  // Diagnostic only: append one record per sealed window (its distinct target
  // lines) to this file. Empty -- the default -- writes nothing, and no decision
  // ever reads the file.
  std::string m13_dump;

  // ---- M23: the selection audit trail -------------------------------------
  // Diagnostic only. When non-empty, `run_hybrid` writes one row per memory
  // instruction after the class map is fixed and before the first cycle: the
  // instruction, the class it was dispatched to, its element count and the
  // metadata source that produced the class. It is written once, never read,
  // and empty by default, so no historical run changes by a cycle or a byte.
  // This exists because a compiler-directed dispatch is only reviewable if the
  // decision that was taken is visible: `simt_instr`/`bulk_instr` are totals.
  std::string m23_select;

  // ---- M9: compiler-directed dispatch --------------------------------------
  // 0 = exactly one front-end, which is every published M4-M8 configuration and
  // leaves those code paths byte for byte as they were. 1 = both front-ends run
  // concurrently, each serving the operations the class map assigns to it.
  u32 dispatch = 0;
  // Source of the per-instruction class metadata. "gen" hands the simulator the
  // generator's own regularity decision, recorded during trace building -- the
  // stand-in for a compiler annotation. "pattern" recomputes the class from the
  // addresses with the rule kappa_real already uses; it exists to cross-check
  // the first, never to classify at run time.
  std::string op_class = "gen";
  // Which client gets the first submission opportunity in a contended cycle:
  // 0 = engine, then SIMT (default); 1 = the reverse. A documented tie-break,
  // not a policy: see docs/m9-dispatch-design.md §9.2.
  u32 dispatch_order = 0;
  // Diagnostic only (experiment=m9diag / capacity sensitivity). 0 = the pool is
  // `R - latch_charge`, exactly as published. Non-zero overrides only the shared
  // pool's capacity in `run_hybrid`; no published configuration sets it, and
  // arms A and B never read it. Values above R allow more than R line requests
  // outstanding, which is a capacity experiment, not a free improvement.
  u32 pool_slots = 0;
  // ---- M15: requester attribution and the fused window layout (diagnostic) --
  // `cache_diag` makes the MemorySystem accumulate per-requester L1/L2 counters.
  // It is read only by the instrumentation itself: with it clear, every probe,
  // MSHR allocation and replacement decision is byte for byte the one the
  // published runs take, and every new counter stays zero.
  bool cache_diag = false;
  // EXPERIMENTAL (M15 Part D). One fused 64-bit word per window element instead
  // of WinEnt + sorted + LineReq + order. Requires a window with dedup and
  // page_order; BulkEngine's constructor states and enforces the conditions.
  // Never a default, and no published configuration sets it.
  bool win_fused = false;

  // M5.1 §8.1: charge the SIMT coalescer latch (<= lanes per core) against A's
  // slot pool, so peak_outstanding == R in both arms. Off by default so the
  // published M5.1 configuration reproduces exactly; the M6 sweep turns it on.
  bool ab_charge_latch = false;
  u64 bulk_index_base = 0x00800000ull;  // 8 MiB: clear of the trace's 1 MiB
  u64 bulk_data_base = 0x01000000ull;   // 16 MiB: clear of the index array
  u64 bulk_desc_line = 98304ull;        // 6 MiB, expressed as a line number

  // workload
  std::string wl = "W1";
  // M18: file-driven access trace (the "M18TRC1" binary written by
  // tools/build_trace.py). Empty = the synthetic W0/W1 generator, so every
  // published path keeps building exactly the trace it built before. A file
  // trace supplies both the accesses and the per-instruction class metadata;
  // `N`, `Lambda`, `P`, `kappa`, `rho_dup` and `seed` are not consulted.
  std::string dataset;
  u32 N = 16384;
  u32 Lambda = 16384;
  u32 P = 2048;
  double kappa = 0.0;
  double rho_dup = 0.0;
  u64 seed = 1;
  u32 elem_bytes = 8;
  u64 max_cycles = 4000000;  // deadlock guard
  // ---- M26-B1: lifecycle diagnostics (diagnostic only, never a decision) ---
  // `life_diag` posts the request-lifecycle trace of docs/m26-stage-b1-...:
  // one bounded record per outstanding request, per-transition counters, a
  // per-VPN walk/fill history and a capped log of the transitions of any
  // request that goes stale. 0 (the default, and every published cell) runs
  // exactly the code it ran before: every new branch is guarded by `life_`,
  // which is latched from this key, so no counter can move.
  bool life_diag = false;
  // Where `life_diag` writes its detailed report. Empty = standard error.
  std::string life_dump;
  // M26-B1 test harness only: stop a run once no request has left `inflight_`
  // for this many cycles while some are still held. 0 = disabled, which is
  // every cell and every published configuration. The cycle guard itself is
  // never touched by this key: a test that trips it is a test that has already
  // observed a stall the guard would have reported 100x later.
  u64 stall_abort = 0;

  static Config load(const std::string& path);
  void set(const std::string& key, const std::string& value);
  std::string to_string() const;
};

// ---------------------------------------------------------------------------
struct Stats {
  std::map<std::string, u64> c;
  void inc(const std::string& k, u64 v = 1) { c[k] += v; }
  void set(const std::string& k, u64 v) { c[k] = v; }
  u64  get(const std::string& k) const {
    auto it = c.find(k);
    return it == c.end() ? 0 : it->second;
  }
  void dump(std::ostream& os, const std::string& prefix) const;
};

// ---------------------------------------------------------------------------
struct Access {
  u64 addr;
  u64 line;
  u32 elem;
  u32 instr;
  u32 lane;
};

struct Trace {
  std::vector<Access> acc;
  u32 lanes = 32;
  u32 n_instr = 0;
  // M9: the class metadata, one entry per instruction, recorded while the trace
  // is generated. Consumed as if a compiler had supplied it (see ClassMap).
  std::vector<u8> instr_class;
  u64 lines_real = 0;
  u32 pages_real = 0;
  double kappa_real = 0.0;
  double rho_line_real = 0.0;
  double rho_word_real = 0.0;
  u64 bytes_real = 0;
};

Trace build_trace(const Config& cfg);
// M18 file-trace loader. Returns an empty trace and sets `err` on failure; only
// reachable when cfg.dataset is non-empty.
Trace build_trace_file(const Config& cfg, std::string& err);
double solve_M(u32 N, double target);

// ---------------------------------------------------------------------------
// M9: compiler-provided operation metadata (docs/m9-dispatch-design.md §2)
// ---------------------------------------------------------------------------
// Exactly two classes. No score, no confidence, no third category, and nothing
// here is ever re-evaluated once execution starts.
enum class OpClass : u8 { Simt = 0, Bulk = 1 };

// One class per memory instruction, in instruction order. Built once from the
// metadata source named by Config::op_class, before any request or descriptor
// exists.
struct ClassMap {
  std::vector<u8> cls;
  u8 at(u32 instr) const {
    return instr < cls.size() ? cls[instr] : static_cast<u8>(OpClass::Simt);
  }
  u64 count(u8 k) const {
    u64 n = 0;
    for (u8 c : cls) if (c == k) ++n;
    return n;
  }
  bool empty() const { return cls.empty(); }
};
ClassMap build_op_classes(const Config& cfg, const Trace& tr);

// ---------------------------------------------------------------------------
struct TlbStats {
  u64 reads = 0, hits = 0, misses = 0, evict = 0;
  u64 mshr_allocs = 0, mshr_joins = 0;
  u64 mshr_occ = 0, mshr_max = 0;
  u64 stall_cycles = 0, stall_lanes = 0;
  // M23-G6D diagnostic: cycles at each MSHR occupancy, index = min(occ, 16), so
  // the top bin means "16 or more". Sampled in the same per-cycle loop that
  // already accumulates `mshr_occ`; never read by a decision.
  u64 mshr_hist[17] = {};
};

struct L2TlbStats {
  u64 reads = 0, hits = 0, misses = 0, evict = 0;
  u64 allocs = 0, dedups = 0, occ = 0, max_occ = 0;
  u64 stall_cycles = 0, walk_stalls = 0;
  // M23-G6D diagnostic: cycles at each MSHR occupancy, index = min(occ, 16).
  u64 occ_hist[17] = {};
};

struct PtwStats {
  u64 walks = 0, walk_latency = 0, wc_hits = 0;
  u64 pte_fetches = 0, walker_busy = 0, walker_max = 0;
};

struct CacheStats {
  u64 reads = 0, hits = 0, misses = 0, evict = 0;
  u64 mshr_allocs = 0, mshr_joins = 0, occ = 0, max_occ = 0;
  u64 stall_cycles = 0;
};

// ---------------------------------------------------------------------------
// M15: per-requester attribution through the shared hierarchy
// ---------------------------------------------------------------------------
// The tag is the M11.3 client index that already rides on Req -> DQ, so no new
// identifier is introduced and no request changes shape:
//   0 = SIMT, 1 = engine index stream, 2 = page-walker PTE fetch,
//   3 = engine target stream.
// A refused probe is a stall for that requester and is *not* a read, exactly the
// convention the aggregate l1/l2 counters use, so `reads == hits + misses` holds
// per requester as well as in aggregate. Every field is written only when
// Config::cache_diag is set, and none is read by a probe, an arbitration, a
// replacement or a cycle decision.
struct ClientCacheStats {
  u64 l1_reads = 0, l1_hits = 0, l1_misses = 0, l1_joins = 0, l1_allocs = 0;
  u64 l1_evict = 0, l1_stalls = 0;
  u64 l2_reads = 0, l2_hits = 0, l2_misses = 0, l2_joins = 0, l2_allocs = 0;
  u64 l2_evict = 0, l2_stalls = 0;
  // An L2 hit whose line was last touched by this requester / by another one.
  u64 l2_hit_self = 0, l2_hit_cross = 0;
  // Evictions this requester's own fills caused, split by who last used the
  // victim. Cross-requester eviction is the direct measure of interference.
  u64 l2_evict_self = 0, l2_evict_cross = 0;
  // Requests submitted and completions returned, the cycle span they covered,
  // and the summed latency of the completions.
  u64 issue = 0, done = 0, lat_sum = 0;
  u64 first_issue = NONE, last_issue = 0, first_done = NONE, last_done = 0;
};

struct DramStats {
  u64 reads = 0, row_hits = 0, row_conflicts = 0, q_full = 0;
  u64 bytes = 0, busy_cycles = 0, max_q = 0;
  u64 blocked_cycles = 0;   // queue non-empty, every bank busy
  u64 starved_cycles = 0;   // queue empty
  u64 bank_mask = 0;        // banks that ever served a request
  // M9 capacity diagnostic (read-only): how many candidates each scheduling
  // decision could choose between, and how deep the queue was. `elig_last` is
  // this cycle's decision, sampled by the cycle trace; the others are totals.
  u64 svc_events = 0;       // cycles a command was issued
  u64 elig_sum = 0;         // eligible candidates summed over those decisions
  u64 elig_max = 0;
  u64 elig_last = 0;
  u64 q_occ_sum = 0;        // queue depth summed over every cycle
  // ---- M11.2 scheduler diagnostic (written only when Config::dram_diag) ----
  // None of these is ever read by the arbitration or by any front-end: they are
  // accumulated after each decision has been made. With `dram_diag` off the
  // whole block stays zero and no extra code runs.
  u64 diag_decisions = 0;   // cycles with a non-empty queue = one decision each
  u64 hit_cand_sum = 0;     // row-hit entries in the queue, summed per decision
  u64 hit_busy_sum = 0;     // ... of those, bank-blocked (the requested metric)
  u64 hit_elig_sum = 0;     // row-hit entries among the eligible candidates
  u64 missed_hit = 0;       // took a non-hit while an eligible hit existed (A)
  u64 stall_case_b = 0;     // non-hit taken, hits existed but were bank-blocked (B)
  u64 stall_case_c = 0;     // non-hit taken, no row hit in the queue at all (C)
  u64 wait_sum = 0, wait_max = 0;   // arrive -> issue, in cycles
  u64 wait_hist[16] = {};   // log2 buckets of the above (b covers [2^b, 2^(b+1)))
  u64 q_occ_hist[16] = {};  // log2 buckets of the per-cycle queue depth
  // ---- M23-G2C queue-arrival instrumentation (only when Config::dram_diag) --
  // Counted at the enqueue site, in the cycle the request actually enters the
  // DRAM queue, so these are true per-cycle arrival statistics: `q_occ_hist`
  // samples depth at the decision, which cannot show whether the queue was fed
  // at all. None of these is read by the arbitration or by any front-end; with
  // the flag off nothing is written and no extra code runs.
  u64 arrivals = 0;              // queue pushes over the run (== reads)
  u64 arrive_cycles = 0;         // cycles with at least one push
  u64 arrive_multi_cycles = 0;   // cycles with two or more pushes
  u64 arrive_this_cycle = 0;     // scratch: pushes seen in the current cycle
  u64 last_arrive_cycle = NONE;  // scratch: cycle of the current arrival batch
  u64 elig_wait_cycles = 0;      // non-empty queue, eligible candidate, no issue
  u64 svc_hit_cycles = 0, svc_miss_cycles = 0;
  u64 bank_reads[4] = {}, bank_hits[4] = {}, bank_conf[4] = {};
  u64 bank_busy[4] = {}, bank_q_sum[4] = {}, bank_q_max[4] = {};
  // Client index: 0 = SIMT, 1 = engine index fetch, 2 = page-walker PTE fetch,
  //                3 = engine target fetch.
  u64 client_reads[4] = {}, client_hits[4] = {}, client_wait[4] = {};
  u64 client_conf[4] = {};    // activations caused by this requester
  u64 client_q_sum[4] = {};   // per-cycle queue occupancy, summed, per requester
  u64 client_q_max[4] = {};   // peak queue occupancy per requester
  u64 client_interf[4] = {};  // activations by this requester whose bank was last
                              // touched by a different one (cross-requester)
  u64 rows_used = 0, row_acts_max = 0, row_acts_sum = 0;
  // ---- M23-G6E scheduler-visibility audit (written only when dram_diag) ----
  // The unit is a *distinct (bank,row) cell present in `dram_wait_` at the
  // moment of the decision*, i.e. the diversity the arbitration could actually
  // choose between. None of these is read by the arbitration.
  u64 vis_rows_sum = 0;    // distinct cells, summed over decisions
  u64 vis_rows_max = 0;
  u64 vis_banks_sum = 0;   // distinct banks represented, summed over decisions
  u64 vis_rows_hist[16] = {};   // log2 buckets of vis_rows, one per decision
  u64 hit_cand_hist[17] = {};   // row-hit entries per decision, index = min(hits, 16)
};

// One line request as seen by the front-end.
//   stage 0: not yet accepted (front-end holds it)
//   stage 1: accepted, waiting for translation
//   stage 2: accepted, at the cache hierarchy (ready==NONE while unknown)
//   done:    removed from the in-flight list
struct Req {
  u64 va_line = 0;
  u64 pa_line = NONE;
  u64 vpn = 0;
  u32 core = 0;
  u32 slot = 0;
  u64 seq = 0;
  u64 id = 0;         // M26-B1: stable identity for the lifecycle trace only
  u64 ready = NONE;
  u64 gen = 0;        // cycle accepted into the system
  u64 tlb_at = 0;     // cycle translation completed
  // M27-P1: the PPN a dTLB hit produced at admission. Kept with the request so
  // the later stage-1 step consumes a value it owns, instead of re-reading an
  // array another client can evict in the meantime.
  u64 ppn = NONE;
  bool ppn_ok = false;
  u8  stage = 0;
  bool probed = false;
  // Engine client: no private L1 (design §4.4 — the DXA enters the socket
  // arbiter directly). Such a request probes the shared L2 instead of the
  // per-core L1. The baseline never sets this.
  bool bypass_l1 = false;
  // M11.2 diagnostic tag, carried to the DRAM queue so the scheduler counters
  // can attribute commands to a requester. 0 = SIMT, 1 = engine, 2 = PTE. It is
  // never read by any decision.
  u8 client = 0;
  // M23-G6A. `xlate_only` is a phase-1 probe: it translates and stops, so no
  // cache or DRAM access can follow it. `pretranslated` is a phase-2 request
  // whose physical line already sits in the engine's PA buffer: `submit` then
  // installs it at the hierarchy directly, with no probe, MSHR or walk.
  bool xlate_only = false;
  bool pretranslated = false;
};

// ---------------------------------------------------------------------------
// M9: one residency budget, two clients (docs/m9-dispatch-design.md §9)
// ---------------------------------------------------------------------------
// A slot is a line request's claim on the global R budget, held from the cycle
// the request is *formed* until the cycle its fill returns -- the lifetime both
// front-ends already used separately (SIMT from coalescing, the engine from
// admission or line claim). Sharing thus changes only who may claim a slot,
// never when a claim is made or released.
//
// `owner[s]` makes a completion record self-describing. `MemorySystem` packs
// `(core << 32) | slot` and treats `slot` as opaque payload (memory.cpp:554), so
// with one pool and cores == 1 the slot id already identifies the request; the
// owner table routes it to the front-end that formed it without changing the
// identifier format or touching the hierarchy at all.
//
// The pure paths never construct a pool: `Sim::run` keeps its per-core vectors
// and `BulkEngine` keeps its own free list when no pool is supplied, so no
// published M4-M8 result can depend on this class (design §9.4, E1/E2).
struct SlotAges;

struct SlotPool {
  static constexpr u8 FREE = 0xff;
  std::vector<u32> free;  // LIFO, the idiom both front-ends already used
  std::vector<u8> owner;  // slot -> client id, or FREE
  u64 held[2] = {0, 0};
  // M9 diagnostic only (all three null/zero unless a cycle trace is attached):
  // `now` is the current cycle, and when `born` is set every claim is stamped
  // and every release is tallied into `ages`. Nothing here is read by a
  // decision, and an owner-table diff would mis-measure lifetimes whenever a
  // released slot is re-claimed inside the same cycle -- which is the normal
  // case for a client holding nearly the whole pool.
  u64 now = 0;
  std::vector<u64>* born = nullptr;
  SlotAges* ages = nullptr;

  // `ascending` reproduces Sim::run's fill (0..n-1, taken from the back, so
  // n-1 is handed out first); `descending` reproduces BulkEngine's (n-1..0, so
  // 0 first). SIMT slot ids enter completion_hash while engine ids enter
  // nothing, so a hybrid must use the ascending fill to match arm A exactly.
  SlotPool(u32 slots, bool ascending) : owner(slots, FREE) {
    free.reserve(slots);
    if (ascending) {
      for (u32 i = 0; i < slots; ++i) free.push_back(i);
    } else {
      for (u32 i = slots; i-- > 0;) free.push_back(i);
    }
  }

  u32 take(u8 who) {
    if (who > 1 || free.empty()) return static_cast<u32>(NONE);
    const u32 s = free.back();
    free.pop_back();
    owner[s] = who;
    ++held[who];
    if (born) (*born)[s] = now;
    return s;
  }
  // False on a double release, a phantom completion, or an id outside the pool.
  bool give(u32 s);  // defined after SlotAges, which it tallies into
  u64 residency() const { return held[0] + held[1]; }
  u64 held_by(u8 who) const { return who > 1 ? 0 : held[who]; }
  size_t free_count() const { return free.size(); }
  bool has_free() const { return !free.empty(); }
  // Structural invariant: every slot is in exactly one set, after every op.
  bool consistent() const { return free.size() + held[0] + held[1] == owner.size(); }
};

// M24-B: one data-ready completion from the HBM controller. Declared here so
// MemorySystem can own the buffer without a header cycle; hbm.hpp includes this
// file and defines the controller itself.
struct HbmServed {
  u64 line = 0, lat = 0;
  u32 ch = 0, bank = 0, row = 0, client = 0;
};
class HbmController;
struct Stats;

// ---------------------------------------------------------------------------
// M26-B1: request-lifecycle diagnostic, off unless Config::life_diag
// ---------------------------------------------------------------------------
// One bounded record per request that is *currently* outstanding; the record is
// erased the cycle the request retires, so the map's size is the in-flight
// population and not the number of requests ever issued. Each record keeps a
// per-transition count (exact, never truncated) and an 8-entry ring of the most
// recent transitions. A request that goes *stale* -- its expected fill cycle has
// passed and its dTLB lookup still misses -- additionally has every subsequent
// transition appended to one capped log, so the stranded request's history is
// complete from the moment it became suspicious, which is the only part of it
// that matters. Requests that retire normally are summarised and dropped.
enum LifeEv : u16 {
  LE_ENTER = 0, LE_PROBE_HIT, LE_PROBE_JOIN, LE_PROBE_ALLOC, LE_PROBE_REFUSED,
  LE_WALK_ISSUE, LE_WALK_DONE, LE_FILL_PUSH, LE_FILL_INSTALL, LE_FILL_EVICT,
  LE_S1_WAIT, LE_S1_MISS, LE_S1_STALE, LE_S1_HIT, LE_XL_ENQ, LE_XL_REJ,
  LE_XL_DEQ, LE_S2_PROBE, LE_S2_JOIN, LE_RETIRE, LE_L2TLB_REQUEUE, LE_N
};
const char* life_ev_name(u16 k);

struct LifeEvRec {
  u64 cycle = 0, a = 0, b = 0;
  u16 kind = 0;
};
struct ReqLife {
  u64 id = 0, vpn = 0, gen = 0;
  u32 core = 0;
  u64 count[LE_N] = {};   // exact per-transition totals
  u64 stale_at = NONE;    // first cycle the expected fill had not arrived
  u16 last = 0;           // last transition seen, and when
  u64 last_cycle = 0;
  LifeEvRec ev[8];        // ring of the most recent transitions
  u8 nev = 0, head = 0;
  // Complete post-onset history, one entry per *state-changing* transition
  // (retries are counted exactly in `count`, never logged, or a per-cycle miss
  // would fill the buffer in a few cycles). Bounded per request, so one early
  // suspect can never crowd out the terminal one.
  LifeEvRec hist[32];
  u8 hist_n = 0;
  u32 hist_drop = 0;
  bool log_state = false;
  void push(u16 k, u64 cyc, u64 a, u64 b) {
    ev[head] = LifeEvRec{cyc, a, b, k};
    head = static_cast<u8>((head + 1) & 7u);
    if (nev < 8) ++nev;
    last = k;
    last_cycle = cyc;
    ++count[k];
    if (log_state && k != LE_S1_MISS && k != LE_S1_WAIT) {
      if (hist_n < 32) hist[hist_n++] = LifeEvRec{cyc, a, b, k};
      else ++hist_drop;
    }
  }
};
// Per-(VPN, core) translation history: transitions that belong to a page rather
// than to one request. This is what distinguishes "the fill was never pushed"
// from "the fill was pushed and the waiter never saw it".
struct VpnLife {
  u32 probes = 0, joins = 0, allocs = 0, walk_done = 0;
  u32 fills = 0, installs = 0, seen_by_waiter = 0;
  u64 last_cycle = 0;
  LifeEvRec ev[8];
  u8 nev = 0, head = 0;
  void push(u16 k, u64 cyc, u64 a, u64 b) {
    ev[head] = LifeEvRec{cyc, a, b, k};
    head = static_cast<u8>((head + 1) & 7u);
    if (nev < 8) ++nev;
    last_cycle = cyc;
  }
};

class MemorySystem {
public:
  explicit MemorySystem(const Config& cfg);
  ~MemorySystem();

  // Resumes/advances one request. Returns false only when the hierarchy cannot
  // accept the request *this cycle* (front-end keeps it held, no state change).
  bool submit(Req& req, u64 cycle);
  void tick(u64 cycle, std::vector<u64>& done);
  void finish(u64 cycle);

  bool idle() const { return inflight_.empty(); }
  // ---- M23-G6A: phase-1 translation results --------------------------------
  // A translation-only request (`Req::xlate_only`) leaves the pipeline the
  // cycle its dTLB lookup succeeds; its physical line is queued here for the
  // engine that asked for it. The queue is drained every cycle by that engine,
  // and its depth is bounded by the dTLB's own MSHR count (submit refuses
  // beyond it), so it is a hand-off, not a buffer.
  struct Xlate {
    u64 seq = 0, pa = 0, cycle = 0;
  };
  bool take_xlate(Xlate& out) {
    if (xlate_head_ >= xlate_done_.size()) {
      xlate_done_.clear();
      xlate_head_ = 0;
      return false;
    }
    if (life_) ++life_xl_deq;
    out = xlate_done_[xlate_head_++];
    if (xlate_head_ == xlate_done_.size()) {
      xlate_done_.clear();
      xlate_head_ = 0;
    }
    return true;
  }
  std::vector<Xlate> xlate_done_;
  size_t xlate_head_ = 0;
  u64 xlate_overflow = 0;  // completions dropped because nobody drained the queue
  size_t inflight_count() const { return inflight_.size(); }

  // ---- M26-B1 lifecycle diagnostics ----------------------------------------
  // All of this is inert unless Config::life_diag was set at construction:
  // `life_` is latched once, no decision reads any of it, and every write is
  // inside an `if (life_)`. Public because the harness's stall detector and the
  // dump both need it; nothing in the model reads it.
  bool life_on() const { return life_; }
  u64 life_last_retire = 0;      // last cycle a request left inflight_
  u64 life_stall_seen = 0;       // cycles since then at the last observation
  u64 life_enter = 0, life_leave = 0;
  u64 life_xl_enq = 0, life_xl_rej = 0, life_xl_deq = 0, life_xl_occ_max = 0;
  u64 life_fill_push = 0, life_fill_install = 0, life_fill_evict = 0;
  u64 life_walk_alloc = 0, life_walk_done = 0, life_walk_release = 0;
  u64 life_parked_sum = 0, life_parked_max = 0;
  u64 life_s1_wait = 0, life_s1_miss = 0, life_s1_stale = 0, life_s1_hit = 0;
  u64 life_reprobe = 0;          // always 0 until a fix exists; counted anyway
  u64 life_l2tlb_requeue = 0, life_l2tlb_requeue_nocore = 0;
  ReqLife* life_at(u64 id);
  void life_ev(u64 id, u16 kind, u64 cycle, u64 a = 0, u64 b = 0);
  VpnLife& vpn_life(u64 vpn, u32 core);
  void dump_life(Stats& s) const;
  // Diagnostic: the DRAM queue as seen at the end of a cycle, and the number of
  // candidates the last FR-FCFS decision could pick from (0 = no command).
  size_t dram_queue() const;   // dispatches to HBM (defined in memory.cpp)
  u64 dram_elig_now() const;
  // M24-B: the HBM counters, written into the same Stats the legacy model uses.
  // A no-op when `dram_kind=ddr`, so every historical run reports exactly what
  // it reported before.
  void dump_hbm(Stats& s) const;
  // M24-B: the configured page geometry, for the front-ends that build a VPN.
  u32 page_line_bits() const { return cfg_.page_line_bits(); }
  u32 page_shift() const { return cfg_.page_shift(); }

  // ---- physical memory image (M5) -----------------------------------------
  // The model translates VA -> PA as ppn = vpn + PPN_OFFSET (the mapping
  // tlb_fill installs); this exposes that same function so a front-end can
  // locate the bytes of a line it has just fetched.
  u64 pa_line_of_va(u64 va) const;
  // M24-A: the physical page frame a virtual page maps to. With m24_map = 3 this
  // is the seeded permutation; otherwise it is exactly `vpn + PPN_OFFSET`, the
  // mapping tlb_fill installs. Every producer and consumer of a physical address
  // goes through this one function, so a remap cannot make two agents disagree.
  u64 pa_page_of_vpn(u64 vpn) const;
  // M23-G3: the production address map (defined in memory.cpp), shared with the
  // Bulk engine so a (bank,row)-grouping policy cannot drift from the
  // controller's own mapping.
  void dram_bank_row(u64 line, u32& bank, u32& row) const;
  // M23-G6F: read-only visibility probe used by the release path -- is `line` in
  // a row the addressed bank currently holds open? It creates no request and
  // touches no state; the caller still submits through the normal gate. Bank
  // *readiness* is deliberately not part of the test: a released run reaches the
  // queue ~L cycles later, so the open row, not the instantaneous ready time, is
  // what predicts a hit on arrival.
  bool dram_row_open(u64 line, u32& bank, u32& row) const;  // dispatches to HBM
  // Same mapping, addressed by VA line number (the unit requests carry).
  u64 pa_line_of_va_line(u64 va_line) const { return pa_line_of_va(va_line * LINE_BYTES); }
  // Order-independent hash of every line the host has written into the image.
  // Two arms that apply the same workload must report the same value.
  u64 image_hash() const;
  // True when this request bypasses the private L1 under the configured path.
  bool path_bypasses_l1(bool req_wants_bypass) const {
    return path_mode_ == 2 || (path_mode_ == 0 && req_wants_bypass);
  }
  // Image writes are how the host places an index array or a descriptor in
  // memory before a run. Only written lines are stored; unwritten lines read
  // back a deterministic synthetic pattern, so no run depends on allocation.
  void write_va(u64 va, const void* src, size_t n);
  // 64 B of a physical line. The pointer is valid until the next call for an
  // unwritten line (synthetic fill), so copy what you need immediately.
  const u8* line_bytes(u64 pa_line) const;

  std::vector<TlbStats> tlb;
  L2TlbStats l2tlb;
  PtwStats   ptw;
  CacheStats l1, l2;
  // M15: requester split of the two caches, the DRAM read stream and the
  // request lifetimes. Zero unless Config::cache_diag is set.
  ClientCacheStats cc[4];
  DramStats  dram;
  u64 cycles = 0;
  u64 replays = 0;   // requests that had to be re-submitted
  // Diagnostic: the only way `submit` refuses is `tlb_probe`, and it refuses
  // for exactly two reasons. Counted so a refusal can be attributed instead of
  // guessed at (report §2.3).
  u64 refuse_parked = 0;  // matching dTLB MSHR already at dtlb_parked waiters
  // M7: peak requests parked on one dTLB MSHR (i.e. the largest per-page
  // waiting set actually reached). Reported, never used for control.
  u64 max_parked = 0;
  u64 refuse_full = 0;    // every dTLB MSHR slot occupied by another VPN

  // Completion behaviour: full latency and its split across the two waits.
  u64 lat_sum = 0, tlb_lat_sum = 0, cache_lat_sum = 0;
  u64 inflight_occ = 0, inflight_max = 0;   // direct request-level occupancy
  std::vector<u32> lat_all;
  u64 issued_per_core(u32 c) const { return c < issued_.size() ? issued_[c] : 0; }
  std::vector<u64> issued_;  // per-core issued-but-not-complete

private:
  struct TlbEntry { bool valid = false; u64 vpn = 0, ppn = 0, lru = 0; };
  struct TlbMshr  { bool valid = false; u64 vpn = 0; u32 parked = 0; };
  struct L2Entry  { bool valid = false; u64 vpn = 0, ppn = 0, lru = 0; };
  // core_mask: every core waiting on this VPN must be filled, not just the
  // first one — otherwise the others' TLB MSHRs never retire.
  struct L2Mshr   { bool valid = false; u64 vpn = 0; u32 core_mask = 0, reqs = 0; u64 ready = 0; };
  // `last_client` is M15 diagnostic state: the requester that last hit or
  // filled the line. Only the attribution counters read it, and it is written
  // only when Config::cache_diag is set, so a normal run's cache content is
  // exactly what it was before the field existed.
  struct CEntry   { bool valid = false; u64 line = 0, lru = 0; u8 last_client = 0xff; };
  // `client` is the requester that allocated the fill, carried so an eviction
  // can be attributed to the request that displaced the victim.
  struct CMshr    { bool valid = false; u64 line = 0, ready = 0; u8 client = 0xff; };
  struct PipeEnt  { u64 ready = 0, vpn = 0; u32 core = 0; };
  // `client` (M11.2) tags the requester for the diagnostic counters only; it is
  // never consulted by the arbitration.
  struct DQ       { u64 line = 0, seq = 0, arrive = 0; u32 bank = 0, row = 0; u8 client = 2; };

  // translation
  bool tlb_probe(u32 core, u64 vpn, u64 cycle, u64& ready, u64* ppn_out = nullptr);
  void tlb_fill(u32 core, u64 vpn);
  void l2tlb_tick(u64 cycle);
  void ptw_tick(u64 cycle);

  // data path
  // `client` is a diagnostic tag (0 SIMT, 1 engine, 2 PTE); it rides along to
  // DQ and changes nothing.
  int  l1_probe(u32 core, u64 line, u64 cycle, u64& ready, u8 client = 2);  // 0 hit, 1 wait, 2 stall
  int  l2_probe(u64 line, u64 cycle, u64& ready, u8 client = 2);
  void cache_tick(u64 cycle);
  void dram_tick(u64 cycle);

  Config cfg_;
  // M24-B: null unless `dram_kind=hbm`. Owning it here keeps the legacy path
  // byte-identical: every branch that uses it is guarded by `hbm_`.
  std::unique_ptr<HbmController> hbm_;
  std::vector<HbmServed> hbm_out_;
  void hbm_tick_(u64 cycle);
  std::vector<TlbEntry> tlb_e_;
  std::vector<TlbMshr>  tlb_m_;
  std::vector<L2Entry>  l2tlb_e_;
  std::vector<L2Mshr>   l2tlb_m_;
  std::deque<PipeEnt>   l2tlb_pipe_;
  std::deque<u64>       walk_cache_;
  std::vector<u64>      walk_busy_;
  std::vector<std::pair<u64, u32>> pending_fill_;  // (vpn, requesting core)

  std::vector<CEntry> l1_e_, l2_e_;
  std::vector<CMshr>  l1_m_, l2_m_;
  std::vector<DQ>     dram_wait_;
  // Per-(bank,row) activation counts. Allocated only when Config::dram_diag is
  // set, so a normal run allocates nothing new.
  std::vector<u32>    row_acts_;
  // M23-G6E: per-(bank,row) stamp for counting distinct cells in the queue.
  // Allocated only when Config::dram_diag is set; `stamp_` is a generation
  // counter, so no array is cleared between cycles.
  std::vector<u32>    row_stamp_;
  u32                 stamp_ = 0;
  std::vector<u64>    bank_ready_, bank_row_;
  std::vector<bool>   bank_open_;
  // M11.3 diagnostic: requester of the last access to each bank (4 = none yet),
  // used only to attribute cross-requester interference.
  u32 bank_last_client_[4] = {4, 4, 4, 4};
  u64 lru_clock_ = 0;
  u64 seq_ = 0;

  // M26-B1 lifecycle state (allocated only when life_ is set).
  bool life_ = false;
  u64 life_id_ = 0;
  u16 life_probe_ = LE_ENTER;   // outcome of the most recent tlb_probe attempt
  std::unordered_map<u64, ReqLife> life_rec_;
  std::unordered_map<u64, VpnLife> vpn_life_;
  u32 life_parked_core_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  std::string life_dump_;

  std::vector<Req> inflight_;
  std::unordered_map<u64, std::array<u8, LINE_BYTES>> img_;
  mutable std::array<u8, LINE_BYTES> scratch_{};
  u8 path_mode_ = 0;  // 0 native, 1 force-L1, 2 force-L2 (from Config::mem_path)
  // M27-P2: liveness. `last_progress` is stamped on every admission and every
  // retirement; a run that still holds work while nothing has progressed for
  // `live_limit_` cycles cannot finish. The bound is derived from the model's
  // own worst-case service times, not tuned to a workload.
 public:
  u64 live_limit() const { return live_limit_; }
  u64 last_progress_cycle() const { return last_progress; }
  bool liveness_tripped(u64 cycle, u64 thr) const { return cycle > last_progress + thr; }
  std::string outstanding_report() const;
 private:
  u64 last_progress = 0;
  u64 live_limit_ = 0;
  bool diag_ = false;  // M15: Config::cache_diag, latched at construction
};

// ---------------------------------------------------------------------------
struct RunResult {
  Stats s;
  u64 cycles = 0;
  double lines_per_cycle = 0.0;
  double gbs = 0.0;
  double lat_mean = 0.0;
  u64 lat_p50 = 0;
  u64 lat_p95 = 0;
  u64 completion_hash = 0;
  bool ok = true;
  std::vector<std::string> violations;
};

// Shared counter export: one definition used by both front-ends, so a bulk run
// reports L1/L2/TLB/DRAM exactly as a baseline run does.
void collect_mem_stats(const MemorySystem& mem, const Config& cfg, Stats& s);

// M9 diagnostic: one read-only observation per simulated cycle. Nothing in the
// drivers reads these fields, so enabling a trace cannot change a decision, a
// counter or a cycle count -- the anchors are re-verified with it compiled in.
struct CycleSample {
  u32 simt_done = 0;      // cumulative SIMT-path completions
  u32 bulk_done = 0;      // cumulative engine elements completed
  u32 pool_simt = 0;      // slots held by the SIMT client (formed + in flight)
  u32 pool_bulk = 0;      // slots held by the engine
  u32 pool_free = 0;
  u32 simt_latch = 0;     // coalescer staging for the current instruction (not a slot)
  u32 bulk_res = 0;       // engine residency under its own accounting
  u32 mem_inflight = 0;   // requests outstanding in the hierarchy
  u32 issued = 0;         // cumulative requests submitted to the hierarchy
  u32 simt_stalls = 0;    // cumulative SIMT issue refusals (memory backpressure)
  u32 bulk_slot_stall = 0, bulk_idx_wait = 0, bulk_buf_full = 0, bulk_win_full = 0;
  // ---- contention detail (all cumulative, read-only) ----------------------
  u32 simt_form = 0;        // SIMT requests formed (a slot taken)
  u32 simt_issue = 0;       // SIMT requests submitted to the hierarchy
  u32 simt_form_refuse = 0; // cycles a coalesced line was ready but the pool had no slot
  u32 simt_ref_simt = 0;    // sum of slots held by SIMT at each such refusal
  u32 simt_ref_bulk = 0;    // sum of slots held by the engine at each refusal
  u32 simt_ref_free = 0;    // sum of free slots at each refusal
  u32 bulk_alloc = 0;       // engine slots taken (pool or own list)
  u32 bulk_release = 0;     // engine slots given back
  u32 bulk_refuse_noslot = 0;   // target refusals with free == 0
  u32 bulk_refuse_reserve = 0;  // target refusals with 0 < free <= idx_reserve
  u32 bulk_ref_simt = 0;    // sum of slots held by SIMT at each target refusal
  u32 bulk_ref_bulk = 0;    // sum of slots held by the engine at each target refusal
  u32 bulk_ref_free = 0;    // sum of free slots at each target refusal
  u32 bulk_tgt_issue = 0, bulk_tgt_done = 0, bulk_idx_done = 0, bulk_elems = 0;
  // ---- M9 capacity diagnostic: index pipeline, target pipeline, DRAM ------
  // Cumulative unless marked instantaneous. The printer differences consecutive
  // samples to obtain per-cycle events, so no diff state is kept in the driver.
  u32 idx_sub = 0;         // index lines submitted to the hierarchy
  u32 idx_elems = 0;       // index elements taken for address generation
  u32 addr_gen = 0;        // target addresses generated (elements admitted)
  u32 idx_stall_cap = 0;   // AG stalls that happened with target capacity free
  u32 idx_ready = 0;       // instantaneous: ready, unconsumed index elements
  u32 idx_ready_lines = 0; // instantaneous: index lines ready with elements left
  u32 idx_ring = 0;        // instantaneous: index lines occupying the ring
  u32 idx_res = 0;         // instantaneous: index lines holding a residency slot
  u32 tgt_backlog = 0;     // instantaneous: sealed target lines not yet issued
  u32 tgt_unissued = 0;    // instantaneous: formed requests awaiting submission
  u32 dram_q = 0;          // instantaneous: DRAM queue depth at the cycle boundary
  u32 dram_elig = 0;       // FR-FCFS candidates at this cycle's decision
  // Did the target stream wait on the feeder, or the feeder on the target
  // stream? `ag_remain`/`win_fill` answer the second question directly: the
  // generator has elements left but no window to admit them into.
  u32 ag_remain = 0;       // instantaneous: elements not yet address-generated
  u32 win_fill = 0;        // instantaneous: a window is accepting elements
  u32 bulk_defer = 0;      // cumulative: page runs deferred after a dTLB refusal
  u32 winrefuse = 0;       // cumulative: M6 policy refusals at the issue port
  // Per-cycle readiness and movement, for the overlap matrix. A flag is set for
  // the cycle only; it is derived from the counters above, never used by them.
  u32 simt_active = 0;   // the SIMT client still had work (formed, held or unformed)
  u32 simt_blocked = 0;  // ... and could not form its next line for lack of a slot
  u32 simt_progress = 0; // formed, issued or completed a request this cycle
  u32 bulk_active = 0;   // the engine was not idle
  u32 bulk_blocked = 0;  // ... and was refused residency for a target or index line
  u32 bulk_progress = 0; // issued a target, or completed a line / element / index line
};

// Allocation-to-release latency histogram, one per client. Bucket b covers
// [2^b, 2^(b+1)-1] cycles for b < NB-1, and [2^(NB-1), inf) for the last.
struct SlotAges {
  static constexpr u32 NB = 12;
  u64 n[2][NB] = {};
  u64 sum[2] = {0, 0};
  u64 max[2] = {0, 0};
  u64 zero[2] = {0, 0};  // claims released in the cycle they were made
  void add(u8 who, u64 age) {
    if (who > 1) return;
    if (age == 0) ++zero[who];
    u32 b = 0;
    u64 lim = 2;
    while (b + 1 < NB && age >= lim) {
      ++b;
      lim <<= 1;
    }
    ++n[who][b];
    sum[who] += age;
    if (age > max[who]) max[who] = age;
  }
  static u64 lo(u32 b) { return b ? (1ull << b) : 0ull; }
  static u64 hi(u32 b) { return b + 1 < NB ? ((1ull << (b + 1)) - 1) : ~0ull; }
};

struct CycleTrace {
  std::vector<CycleSample> v;
  SlotAges ages;  // filled only while this trace is attached
};

inline bool SlotPool::give(u32 s) {
  if (s >= owner.size() || owner[s] == FREE) return false;
  if (born && ages) ages->add(owner[s], now - (*born)[s]);
  --held[owner[s]];
  owner[s] = FREE;
  free.push_back(s);
  return true;
}
// Latency split and percentiles; one definition for every front-end.
void collect_lat_stats(const MemorySystem& mem, RunResult& res);

// ---------------------------------------------------------------------------
// M5: bulk indirect-access engine
//
//   output[i] = data[index[i]]
//
// A descriptor is fetched from memory, the index array is streamed through the
// shared MemorySystem, each index value produces one target address, and each
// target line request holds one of the R residency slots from generation until
// its fill returns. No dedup, no reordering, no L1 bypass of translation — this
// milestone implements generation and coordination only.
// ---------------------------------------------------------------------------

// On-memory descriptor layout, 64 B (offsets are byte offsets in the line):
//   0  u32 magic ('DX01')
//   4  u32 version (1)
//   8  u64 desc_line      (VA line holding this descriptor)
//   16 u64 index_base     (VA byte address of index[0])
//   24 u64 data_base      (VA byte address of data[0])
//   32 u32 count          (elements)
//   36 u32 elem_base      (element id of element 0)
//   40 u8  index_bytes    (4 or 8)
//   41 u8  data_bytes     (1, 2, 4 or 8)
//   42 u16 reserved (0)
struct BulkDesc {
  u64 desc_line = 0;
  u64 index_base = 0;
  u64 data_base = 0;
  u32 count = 0;
  u32 elem_base = 0;
  u8 index_bytes = 4;
  u8 data_bytes = 8;

  void encode(u8* line64) const;
  static bool decode(const u8* line64, BulkDesc& out);
  // "" when valid, otherwise the reason. `count == 0` is valid: an empty
  // descriptor still costs its fetch and then completes immediately.
  std::string validate(u32 max_elems) const;
};

enum class ReqKind : u8 { Desc = 0, Index = 1, Target = 2, Xlate = 3 };

struct BulkStats {
  // descriptors
  u64 desc_submitted = 0, desc_completed = 0, desc_rejected = 0;
  u64 desc_cycles_sum = 0, desc_cycles_max = 0;
  u64 desc_fetch_cycles_sum = 0;
  // index stream
  u64 idx_lines_req = 0, idx_lines_done = 0, idx_lines_ooo = 0, idx_out_max = 0;
  u64 idx_elems_req = 0, idx_elems_done = 0;
  u64 idx_first_done = NONE;
  // address generation and target requests
  u64 tgt_addr_gen = 0, tgt_req_gen = 0, tgt_req_issued = 0, tgt_req_done = 0, tgt_ooo = 0;
  u64 tgt_first_gen = NONE;
  u64 elems_total = 0, elems_done = 0, addr_faults = 0;
  // request buffer (unissued + issued-not-filled)
  u64 buf_occ = 0, buf_max = 0, unissued_max = 0;
  u64 buf_full_stall = 0, idx_wait_stall = 0, issue_stall = 0;
  // M6.1: `issue_stall` counts memory-system refusals (tlb_probe), which is a
  // different event from having no free residency slot. Kept apart so neither
  // is quoted as the other (report §2.3).
  u64 slot_stall = 0;
  // M27-P3: translation admissions refused because the shared residency budget
  // had no slot for a phase-1 probe (the index-stream reserve is honoured).
  u64 tr_slot_stall = 0;
  // M6.2: the refusal above, split by which port was blocked. Both are counted
  // once per cycle (each site breaks out of its issue loop on refusal), so each
  // is a *cycle* count: cycles where that port had a line ready and a free slot
  // and the dTLB refused it.
  u64 win_refuse = 0;   // target-issue port (`win_issue`)
  u64 idx_refuse = 0;   // shared stream (`issue`): index lines and descriptor
  // ---- M6 window ----
  u64 win_entries = 0;        // elements admitted to windows
  u64 win_occ_max = 0, win_occ_sum = 0, win_total_max = 0;
  u64 dir_occ_max = 0;        // peak distinct pages in one window
  u64 batches = 0, seals_full = 0, seals_end = 0, seals_deadline = 0;
  u64 lines_issued = 0;       // unique line requests issued from windows
  u64 dedup_merged = 0;       // elements folded onto an already-requested line
  u64 org_cycles = 0;         // cycles the organizer was busy
  u64 org_runs = 0;           // M23-G3: line runs placed by an order policy
  u64 org_backlog = 0;        // outstanding organization work (entries)
  u64 fanout_cycles = 0, fanout_elems = 0, fanout_max_backlog = 0;
  // ---- M23-G6A: translation-first, DRAM-second ------------------------------
  // Phase 1 is measured in cycles: a window is either translating or releasing,
  // and `g6a_tr_cycles + g6a_dr_cycles` is the time it spent doing both, which
  // is compared with the same window's lifetime measured from adoption.
  u64 g6a_windows = 0;        // windows that ran both phases
  u64 g6a_tr_reqs = 0;        // phase-1 translation probes submitted
  u64 g6a_tr_done = 0;        // phase-1 translation probes completed
  u64 g6a_tr_cycles = 0;      // cycles windows spent in phase 1
  u64 g6a_dr_cycles = 0;      // cycles windows spent in phase 2
  u64 g6a_tr_stall = 0;       // cycles phase 1 had a run ready and the dTLB refused
  u64 g6a_barrier_cycles = 0; // cycles between the last translation and phase 2
  u64 g6a_pa_occ_max = 0;     // peak PA-buffer entries held by one window
  u64 g6a_pa_need_max = 0;    // peak entries a window required (its run count)
  u64 g6a_pa_capacity = 0;    // provisioned entries
  u64 g6a_pa_bytes = 0;       // modelled SRAM: entries x 16 B {pa, cycle}
  u64 g6a_dram_idle_tr = 0;   // phase-1 cycles with no target of this window out
  u64 g6a_tr_wait_dram = 0;   // sum over runs of (release cycle - translated cycle)
  // ---- M23-G6B: streaming translation/DRAM decoupling -----------------------
  u64 g6b_windows = 0;
  u64 g6b_tr_reqs = 0, g6b_tr_done = 0;
  u64 g6b_capacity = 0, g6b_ready_peak = 0, g6b_ready_occ_sum = 0;
  // M23-G6D diagnostic: cycles at each ready-buffer occupancy, log2 buckets:
  // 0 = empty, 1 = 1, 2 = 2-3, 3 = 4-7, ... 12 = 2048+ (the buffer is 4096).
  u64 g6b_ready_hist[13] = {};
  u64 g6b_backpressure = 0;   // cycles translation had work but the buffer was full
  u64 g6b_full_events = 0;
  u64 g6b_tr_stall = 0;       // cycles the dTLB refused a probe that was ready
  u64 g6b_no_ready = 0;       // cycles the DRAM scheduler found nothing translated
  u64 g6b_overlap = 0;        // cycles with a probe in flight AND a target outstanding
  u64 g6b_tr_active = 0;      // cycles with at least one probe in flight
  u64 g6b_tgt_out = 0;        // cycles with at least one target outstanding
  u64 g6b_runs_released = 0;
  // M23-G6E release-port audit: pops actually handed to `win_emit_fused`, cycles
  // where translated work existed but nothing could be released, and cycles where
  // the release stopped on the residency budget (`win_emit_fused` == 1).
  u64 g6b_rel_pops = 0, g6b_rel_blocked = 0, g6b_rel_slot_stall = 0;
  // M23-G6F candidate visibility (written only when Config::sched_vis != 0).
  // `scan_*` describe the population the release *compared*; `vis_*` describe
  // how many of those candidates were distinct (bank,row) cells and how many sat
  // on an already-open row; `pick_*` say which class the chosen candidate fell
  // in. None of these is read by a decision.
  u64 g6f_scan_calls = 0;      // release decisions that compared candidates
  u64 g6f_scan_entries = 0;    // candidates inspected, summed over decisions
  u64 g6f_vis_cells_sum = 0;   // distinct (bank,row) cells among them, summed
  u64 g6f_vis_cells_max = 0;
  u64 g6f_vis_hit_sum = 0;     // open-row candidates among them, summed
  u64 g6f_pick_hit = 0;        // decisions whose pick was an open-row candidate
  u64 g6f_pick_frontier = 0;   // decisions that fell back to the smallest key
  // M23-G6G physical (bank,row) group release. A *group* is opened when the
  // release claims a cell, and closed when the buffer no longer holds any pending
  // run of that cell (so a group never waits: closing is immediate). `size` is
  // measured at open, `drain` at close -- the two differ when the cell keeps
  // receiving runs while it is being drained. None of these is read by a
  // decision.
  u64 g6g_groups_opened = 0, g6g_groups_closed = 0;
  u64 g6g_group_runs = 0;          // runs released while their cell was open
  u64 g6g_group_size_sum = 0, g6g_group_size_max = 0;
  u64 g6g_group_drain_sum = 0, g6g_group_drain_max = 0;
  // open-size histogram: 1, 2, 3-4, 5-8, 9-15, 16+ (a cell holds <= 16 lines
  // per bank in the production map)
  u64 g6g_group_hist[6] = {};
  u64 g6g_same_cell = 0;      // release stayed in the previous release's cell
  u64 g6g_switch_cell = 0;    // release moved to a different cell
  u64 g6g_bank_alt = 0;       // consecutive releases served different banks
  u64 g6g_revisit = 0;        // a cell opened again after being drained once
  u64 g6g_sel_mismatch = 0;   // picked cell != released run's cell (must stay 0)
  u64 g6g_scan_calls = 0, g6g_scan_entries = 0;  // grouping passes / entries read
  // M23-G6H complete-eligible-window audit. A *decision* is one complete
  // enumeration of `ready` in the group-choice pass; every decision reads the
  // whole array, so `exam_max` rises to the buffer's occupancy and nothing caps
  // it below the 4096-entry capacity. `exam` counts what the scan read, `elig` the
  // live (not yet released) entries, `stale` the released heap entries it skipped,
  // `untr` the window's runs still in translation (`fmeta.size() - tr_done`).
  // `inv_viol` must stay 0: one count per decision where
  // exam != elig + stale, or elig != the sum of the per-cell counts.
  u64 g6h_decisions = 0;
  u64 g6h_exam_sum = 0, g6h_exam_max = 0;
  u64 g6h_elig_sum = 0, g6h_elig_max = 0;
  u64 g6h_stale_sum = 0;
  u64 g6h_untr_sum = 0, g6h_untr_max = 0;
  u64 g6h_groups_sum = 0, g6h_groups_max = 0;  // distinct cells per decision
  u64 g6h_considered_sum = 0;                  // cells the choice weighed
  u64 g6h_deferred_sum = 0;                    // eligible runs left for later
  u64 g6h_inv_viol = 0;
  // M23-G6I: continuous translation/DRAM pipeline (written only when
  // Config::g6i_diag). The sampled fields describe the ready pool's *contents* at
  // a sample instant; the exact fields count events in the release loop. None is
  // read by a decision.
  u64 g6i_samples = 0;                 // pool samples taken
  u64 g6i_samp_pool_sum = 0, g6i_samp_pool_max = 0;
  u64 g6i_vpn_sum = 0, g6i_vpn_max = 0;          // distinct virtual pages in pool
  u64 g6i_cells_sum = 0, g6i_cells_max = 0;      // distinct (bank,row) in pool
  u64 g6i_xpage_cells_sum = 0, g6i_xpage_cells_max = 0;  // cells fed by >=2 pages
  u64 g6i_xpage_entries_sum = 0;                 // pool entries in such cells
  u64 g6i_untr_sum = 0, g6i_untr_max = 0;        // window runs not yet translated
  u64 g6i_pages_open_sum = 0, g6i_pages_open_max = 0;  // cohorts with work left
  u64 g6i_probe_max = 0;               // longest VPN-table probe (hash health)
  u64 g6i_rel_cycles = 0;              // cycles that released >= 1 run
  u64 g6i_rel_with_tr = 0;             // ... while a probe was still in flight
  u64 g6i_pool_empty = 0;              // cycles ending with an empty pool
  // M24-A: release-order evidence. `pairs` counts consecutive releases inside one
  // window, `inversions` those whose position in the chosen order decreased. The
  // neutral (sealed) arm can only produce 0 inversions; the physical arm releases
  // across destinations, so both numbers make the two orders comparable.
  u64 m24_rel_pairs = 0, m24_rel_inversions = 0;
  u64 g6i_pool_over = 0;               // cycles above capacity (must be 0)
  u64 g6i_gap_n = 0, g6i_gap_sum = 0, g6i_gap_max = 0;  // same-cell release gap
  u64 g6i_rel_vpn_switch = 0;          // consecutive releases from different pages
  u64 g6i_rel_same_vpn = 0;            // consecutive releases from the same page
  u64 g6b_tr_wait_dram = 0, g6b_tr_wait_max = 0;
  u64 g6b_pa_bytes = 0;
  // ---- M23-G6C: P0-style admission deferral on the G6B pipeline -------------
  // An *attempt* is one probe submission; a refusal is the dTLB's, classified by
  // cause. A *deferral* is a refusal turned into a bounded postponement of that
  // page run. Depth is the number of runs deferred at the moment of an event;
  // age is the cycles between a run's first deferral and the admission that
  // ended it.
  u64 g6c_windows = 0;
  u64 g6c_admit_attempts = 0, g6c_admit_ok = 0, g6c_refused = 0;
  u64 g6c_refuse_parked = 0;  // this page's own dTLB MSHR is at dtlb_parked
  u64 g6c_refuse_full = 0;    // no dTLB MSHR free for another page
  u64 g6c_refuse_other = 0;   // must stay 0: a probe has no other refusal source
  u64 g6c_defer_events = 0, g6c_defer_readmit = 0;
  u64 g6c_defer_other_admit = 0;  // refusal, then another page admitted, same cycle
  u64 g6c_defer_age_sum = 0, g6c_defer_age_max = 0;
  u64 g6c_defer_depth_sum = 0, g6c_defer_depth_n = 0, g6c_defer_depth_max = 0;
  u64 g6c_cohorts_opened = 0, g6c_admit_idle = 0, g6c_dup_submit = 0;
  u64 g6a_tr_wait_max = 0;    // longest wait of one translated run for DRAM
  u64 g6a_runs_released = 0;  // runs released in phase 2 with a buffered address
  u64 win_full_stall = 0;     // could not admit: window or directory full
  u64 dir_collisions = 0;
  u64 win_bytes = 0;          // modelled SRAM: W*8 + D*16 per buffer
  // M15 diagnostic: realized storage of the fused prototype's own vectors. The
  // design footprint is reported separately and derived, not measured here.
  u64 win_fused = 0, win_fused_vec_bytes = 0;
  // M12 diagnostic: peak realized storage of both batch buffers in the simulator
  // (vector capacities, measured in the occupancy block). Never read by a
  // decision; `win_bytes` stays the design's modelled charge.
  u64 win_vec_bytes = 0;
  // utilisation and invariants
  u64 ag_cycles = 0, ag_util_x1000 = 0;
  u64 addr_mismatch = 0, residency_violations = 0, issue_monotonic_violations = 0;
  u64 completion_hash = 0;
  // ---- M6.1: event timeline -------------------------------------------------
  // Recorded unconditionally (a handful of scalars; no control flow depends on
  // them), so cycles can be split into startup / issue span / drain instead of
  // only summed per mechanism. See docs/m6-w0-diagnostic-report.md §3.
  u64 first_desc_done = NONE, first_idx_issue = NONE, first_admit = NONE;
  u64 last_idx_issue = NONE, last_idx_done = NONE;
  u64 first_seal = NONE, last_seal = NONE;
  u64 first_tgt_issue = NONE, last_tgt_issue = NONE, last_fill = NONE;
  // DRAM service already completed when an event happened: bank-idle time per
  // phase is (2 * phase_cycles - phase_busy), with two banks configured.
  u64 busy_at_first_tgt = 0, busy_at_last_tgt = 0, busy_at_last_fill = 0;
  u64 starved_at_first_tgt = 0;
  u64 oracle_elems = 0;  // elements whose index value came from the oracle
  // ---- M7: translation-aware scheduling -------------------------------------
  u64 sched_opens = 0;     // page runs opened
  u64 sched_switches = 0;  // times issue moved to a different page run
  u64 sched_defer = 0;     // refusals turned into a deferral (events, not cycles)
  u64 sched_idle = 0;      // cycles lost: work pending, free slot, every run deferred
  u64 sched_probes = 0;    // translation probes attempted by the scheduler
  u64 sched_cycles = 0;    // cycles charged for scheduler work
  u64 sched_scan = 0;      // line steps spent delimiting page runs
  u64 sched_bytes = 0;     // modelled metadata, per window buffer
  // ---- M8: index acquisition ------------------------------------------------
  u64 idx_tile_swaps = 0;  // buffer A -> B handovers
  u64 idx_buf_stall = 0;   // cycles the acquisition unit had a line ready but no slot
  u64 idx_ooo_gen = 0;     // elements generated out of order (fine-grained readiness)
  u64 idx_ready_max = 0;   // peak ready lines across both tiles
  u64 idx_bytes = 0;       // modelled tile SRAM (descriptors + ready/used bits)
  // ---- M13: bounded cross-window reuse buffer -------------------------------
  // `reuse_lines_planned` is the denominator every other counter here is stated
  // against: the distinct target lines of every sealed window. Each one is
  // either issued as a request (`reuse_miss`), served from retained data
  // (`reuse_hit_ready`), or attached to a request that is still outstanding
  // (`reuse_hit_inflight`). The three are exhaustive and disjoint by
  // construction, and the run checks that they add up.
  u64 reuse_lines_planned = 0;
  u64 reuse_probes = 0;        // lookups performed (one per planned line)
  u64 reuse_hit_ready = 0;     // served from retained data: no request formed
  u64 reuse_hit_inflight = 0;  // merged into an outstanding request
  u64 reuse_miss = 0;          // had to issue a request (== lines_issued)
  u64 reuse_full = 0;          // no evictable entry: no record kept, request as usual
  u64 reuse_sub_full = 0;      // in-flight entry already had its full fanout group
  u64 reuse_evict = 0;         // completed entries replaced
  u64 reuse_inserts = 0;       // entries filled with data
  u64 reuse_dropped = 0;       // outstanding records dropped at a descriptor abort
  u64 reuse_elems_ready = 0;   // elements retired from retained data
  u64 reuse_elems_merged = 0;  // elements retired by a merged fill
  u64 reuse_sub_max = 0;       // peak dependants attached to one entry
  u64 reuse_occ_max = 0;       // peak valid entries
  u64 reuse_occ_sum = 0;       // valid entries summed per cycle
  u64 reuse_bytes = 0;         // modelled SRAM charge (entries x 88 B)
  u64 reuse_vec_bytes = 0;     // realized simulator storage (vector capacity)
  // ---- M9 diagnostic: shared-pool accounting (read-only) --------------------
  // `slot_take`/`slot_give` are every residency claim and release the engine
  // makes, on its own list or on the shared pool. The refusal attribution is
  // sampled at the refusal site (win_emit), so it says who actually held the
  // slots at that instant instead of inferring it from an occupancy snapshot.
  u64 slot_take = 0, slot_give = 0;
  u64 tgt_refuse_noslot = 0;   // free_count() == 0
  u64 tgt_refuse_reserve = 0;  // 0 < free_count() <= idx_reserve (index share)
  u64 tgt_refuse_simt_held = 0;
  u64 tgt_refuse_bulk_held = 0;
  u64 tgt_refuse_free = 0;
  u64 win_backlog = 0;  // sealed target lines not yet issued (last cycle)
  // ---- M9 window management: one record per window, appended at allocation ----
  // Recorded at the events themselves (allocation, admission, seal, release), so
  // the sizes, ages and overlap below are observations, not reconstructions.
  struct WinRec {
    u32 n = 0;         // elements sealed into this window
    u64 alloc = 0;     // cycle the buffer was adopted for filling
    u64 first = NONE;  // first admission
    u64 last = NONE;   // last admission
    u64 seal = 0;      // cycle sealed (0 = never: only on an aborted descriptor)
    u64 retire = 0;    // cycle the buffer was released
    u8 why = 0;        // 0 full, 1 descriptor end, 2 partial (deadline), 3 discarded empty
    // M9 double-buffer balance: the consumer's own two events. `issue` is the
    // cycle the buffer was adopted for issue (target processing starts here);
    // `last_issue` is the cycle its last target line was submitted, so
    // issue..last_issue is the issuing phase and last_issue..retire is the drain
    // (fills + fan-out). Read-only; nothing looks at either before export.
    u64 issue = 0;
    u64 last_issue = 0;
  };
  std::vector<WinRec> win_recs;
  u64 win_alloc = 0;        // windows adopted for filling
  u64 win_live_max = 0;        // peak simultaneously non-empty buffers (any state)
  u64 win_incomplete_max = 0;  // peak simultaneously unsealed buffers (structurally 1)
  u64 win_drain_max = 0;    // peak simultaneously sealed-but-not-released buffers
  u64 win_live_cycles = 0;  // cycles with two non-empty buffers
  u64 win_fill_wait = 0;    // cycles with generator work and no window accepting
  u64 win_fill_blocked = 0; // ... because the policy would not let another start
  // Cycles that end with a sealed window present and no window adopted for issue.
  // A cold start costs one; any larger count is a real hand-off delay.
  u64 win_issue_idle = 0;
  // ---- M9 capacity diagnostic: the index pipeline --------------------------
  // `idx_sub` is not `idx_lines_req`: the first counts index lines handed to the
  // hierarchy, the second index lines claimed into the acquisition ring. The
  // gap between them is the submit backlog, which is what a smaller pool can
  // lengthen. `idx_consumed` counts the index elements taken for address
  // generation (one element -> one address here; reported separately anyway).
  u64 idx_sub = 0;
  u64 idx_consumed = 0;
  // Readiness attribution, sampled at the AG stall itself: how many of those
  // stalls happened while the target port still had issue capacity free.
  u64 idx_wait_cap = 0;
  u64 idx_wait_free_sum = 0;
  // Free slots summed at target-capacity refusals (pool path only).
  u64 slot_stall_free_sum = 0;
};

class BulkEngine {
public:
  // `pool` null (default) = own free list of R slots, the published behaviour;
  // non-null = the M9 driver owns the budget and this engine is one client.
  BulkEngine(const Config& cfg, MemorySystem& mem, SlotPool* pool = nullptr);

  // Queue a descriptor. Only encodability and the descriptor's own address are
  // checked here; field validation happens after the descriptor line is fetched,
  // so a bad descriptor is rejected by the engine, not by the caller.
  bool submit(const BulkDesc& d);
  void tick(u64 cycle);
  // M9: `tick` split into its two halves, so a driver that also runs the SIMT
  // front-end can advance the hierarchy once and hand this engine only its own
  // completion records. `tick` remains their composition, in this order.
  void attribute_done(u64 cycle, const std::vector<u64>& done);
  void progress(u64 cycle);
  bool idle() const;

  const BulkStats& stats() const { return st_; }
  const std::vector<std::string>& errors() const { return err_; }
  // Diagnostic path (Config::idx_oracle): the index values, in element order.
  // The engine still re-derives every address from the index image at
  // completion, so an oracle value that disagreed with memory would show up in
  // `addr_mismatch` rather than pass silently.
  void set_index_oracle(const std::vector<u64>* v) { oracle_ = v; }
  // Slots this engine holds: `unissued + idx_q + inflight` on its own free
  // list, or `pool->held_by(1)` when the M9 driver owns the budget.
  size_t residency() const;
  u64 engine_residency() const;
  size_t free_count() const;  // slots available to this engine right now
  // M9 capacity diagnostic: index readiness as the acquisition ring holds it.
  // Read-only; empty/zero unless the tiled path is active.
  u64 idx_ready_elems() const;   // ready, unconsumed index elements
  u64 idx_ready_lines() const;   // index lines ready with elements left
  u64 idx_ring_lines() const;    // index lines occupying the ring
  u64 idx_resident_lines() const { return idx_resident_; }
  size_t tgt_unissued() const { return unissued_.size(); }
  u64 ag_remaining() const;      // elements not yet address-generated
  bool win_filling() const { return fill_ >= 0; }  // a window accepts elements
  bool slot_space() const;    // == (residency() < R) on the pure path
  bool has_value(u32 elem) const { return val_.count(elem) != 0; }
  u64 value_of(u32 elem) const;
  u64 addr_of(u32 elem) const;

private:
  // Slots this engine may hold: R on its own list, the pool's size when the M9
  // driver owns the budget (equal to R unless a capacity experiment overrides it).
  u32 slot_cap_ = 0;
  struct Pending {
    ReqKind kind = ReqKind::Desc;
    u64 va_line = 0;
    u32 elem = 0;
    u16 off = 0;
    u8 width = 0;
    u64 seq = 0;
    u64 first_elem = 0;  // index lines: first element covered by this line
    // M6: a windowed target request names the batch line it will fan out.
    u32 win_batch = 0xffffffffu, win_line = 0xffffffffu;
    // M13: the reuse-buffer entry this request will publish into, if any.
    u32 reuse_ent = 0xffffffffu;
  };
  // M8: one bounded tile of index lines. It holds *descriptors* (which line, how
  // many elements, how many consumed) and readiness, not the index values, which
  // stay in the hierarchy exactly as they did in M6 — the tile is the buffer the
  // earlier design kept implicitly in a map. Two tiles give A/B ownership.
  // Two bounded halves (A and B) over one ring of descriptors. A line occupies a
  // slot from the claim until its last element has been generated, so capacity is
  // released **per line**: the far half fills while the near half drains and the
  // ring wraps, which is double buffering at line granularity instead of at
  // whole-buffer granularity. Release-per-buffer was measured and is 15 % slower
  // (report §5.4) because a 32-line buffer is drained in 64 cycles but filled in
  // hundreds, so a per-buffer barrier stalls the address engine every time.
  static constexpr u32 IDX_SLOTS = 256;
  struct IdxRing {
    std::array<u64, IDX_SLOTS> elem_base{};  // first descriptor element of the line
    std::array<u64, IDX_SLOTS> va{};         // the index line itself
    std::array<u16, IDX_SLOTS> cnt{};        // elements the line covers
    std::array<u16, IDX_SLOTS> used{};       // elements already generated
    std::array<u8, IDX_SLOTS> ready{};       // line data has arrived
    u32 head = 0;  // oldest slot still holding elements
    u32 n = 0;     // slots in use
    u64 next_elem = 0;  // next descriptor element whose line is unclaimed
  };
  struct Run {
    BulkDesc d;
    u32 phase = 0;  // 0 = descriptor fetch, 1 = stream/generate, 2 = done
    u64 next_line_elem = 0;
    u64 gen_cursor = 0;
    u64 gen_done = 0;  // elements generated (out-of-order safe)
    u32 done = 0;
    u64 start = 0;
    // M8 index acquisition state
    IdxRing ring;
    u32 idx_cap = 0;  // slots per half, fixed at seal of the run
  };

  void reject(const std::string& why);
  void start_next(u64 cycle);

  // ---- M6: windowed path --------------------------------------------------
  // A window is a page-indexed store of pending elements. It is NOT residency:
  // elements wait here holding no slot; a slot is taken by a *unique line* when
  // it is issued and released when the fill arrives.
  struct WinEnt {
    u64 va_line = 0;
    u32 elem = 0;
    u16 off = 0;
    u8 width = 0;
  };
  struct LineReq {
    u64 va_line = 0;
    u32 first = 0;   // start in `sorted`
    u32 count = 0;   // elements depending on this line
    u32 seq = 0;     // first-admission order (entry id), for page_order = 0
    u32 slot = 0xffffffffu;
    bool issued = false;  // a request was issued, or M13 served it from an entry
    // M13: the reuse-buffer entry this line registered in (0xffffffff = none).
    u32 reuse_ent = 0xffffffffu;
  };
  struct Batch {
    u8 state = 0;  // 0 empty, 1 filling, 2 issuing, 3 draining
    std::vector<WinEnt> ent;
    std::vector<u32> sorted;    // entry ids ordered by (va_line, id)
    std::vector<LineReq> lines;
    std::vector<u32> order;     // indices into `lines`, in issue order
    std::vector<u64> dir_vpn;   // page-directory tags (D entries)
    std::vector<u64> dir_mask;  // exact line mask per resident page
    // M7: one page run of the sealed order. `pos..end` index into `order`, so a
    // run owns lines and nothing else; `resume_at` is the first cycle the run
    // may be re-probed after the dTLB refused it.
    struct Cohort {
      u32 pos = 0, end = 0;
      u64 vpn = NONE;
      u64 resume_at = 0;
      bool open = false;
    };
    std::vector<Cohort> coh;  // empty unless Config::page_sched
    u32 open_pos = 0;         // first order position not yet claimed by a run
    u32 rr = 0;               // round-robin cursor over `coh`
    u32 last_coh = 0xffffffffu;
    // ---- M15 fused layout (Config::win_fused) ----------------------------
    // One 64-bit word per element, sorted by (va_line, element position), so a
    // distinct line is a contiguous run and the grouping needs no permutation:
    //   bits 63..38 va_line (26) | 37..6 position in the descriptor (32)
    //   | 5..0 in-line offset (6)
    // Per distinct line: start|count in `fmeta`, residency slot and issued bit
    // in `fslot`/`fflag`. `ent`, `sorted`, `lines` and `order` stay empty in
    // this mode -- that is the entire storage argument for the prototype.
    std::vector<u64> fent;
    std::vector<u32> fmeta;  // start:16 | count:16, both bounded by batch_window
    std::vector<u8> fslot;   // residency slot, 255 = none (requires R <= 255)
    std::vector<u8> fflag;   // bit 0 = issued, or served from the reuse buffer
    // M23-G3: issue order over `fmeta` (a permutation of run ids). Empty when
    // bulk_org = 0, so the published policy allocates nothing extra: an empty
    // `ford` means "identity", i.e. the sealed order.
    std::vector<u32> ford;
    // ---- M23-G6A: two-phase issue -----------------------------------------
    // M28-D: the physical line phase 1 produced (`fpa`) and the translated bit
    // (`ftr`) are per-run arrays owned by the *engine*, not by a window: they are
    // written and read only by the window that holds the single issue cursor
    // `iss_` (see the `rel_*` block in BulkEngine). M28-F extended that to the
    // two per-run timestamps, `ftr_gen`/`ftr_done` -- the cycles the probe was
    // submitted and completed -- because every write and read of them lies in the
    // same blocks as `fpa`/`ftr`; the engine holds one copy each as
    // `rel_gen_`/`rel_done_`.
    u32 g6a_phase = 0;        // 0 not started, 1 translating, 2 releasing
    u32 tr_next = 0, tr_done = 0;
    u64 tr_start = 0, tr_last_done = 0, dr_start = 0;
    u64 busy_tr_start = 0;    // DRAM bank-cycles already spent when phase 1 began
    // ---- M23-G6B: streaming decoupling -----------------------------------
    // `ready` is the set of translated-but-unissued runs, keyed by their
    // position in `ford` -- i.e. by physical (row, bank, line), which is the P2
    // order. The DRAM scheduler therefore always releases the most local
    // translated line available while translation continues behind it.
    // M23-G6F: a min-heap over those positions, held in a vector instead of a
    // std::priority_queue so the release path can read the whole candidate
    // population. The heap operations are the ones priority_queue<u64, vector,
    // greater<u64>> performs, so the release sequence is unchanged when the
    // visibility knob is off.
    //
    // M28-D: the heap itself (`rel_ready_`) and the rank array it is keyed by
    // (`rel_pos_`) are engine state, not window state -- like `fpa`/`ftr` above
    // they are written and read only through the single `iss_` cursor, so the
    // engine owns one copy of each instead of one per window.
    // ---- M23-G6G: physical (bank,row) group release ----------------------
    // `g6g_cell` is the cell G1 is draining; G2 keeps one per bank in
    // `g6g_cell_b`, indexed by bank (the table is sized for 8 and `pick_group`
    // refuses a wider DRAM). 0xffffffff = no group. `g6g_grp_n` counts the runs
    // actually released into a group, so a group's realized size is recorded when
    // it closes. `g6g_rr` is G2's bank cursor: it advances once per release, so
    // consecutive releases alternate banks.
    u32 g6g_cell = 0xffffffffu;
    u32 g6g_cell_b[8] = {};
    u32 g6g_grp_n[8] = {};
    u32 g6g_rr = 0;
    u32 g6g_sel_cell = 0xffffffffu, g6g_sel_slot = 0;
    u32 g6g_last_cell = 0xffffffffu, g6g_last_bank = 0xffffffffu;
    u8 g6g_any = 0;
    u32 g6b_init = 0, g6b_tr_next = 0, g6b_tr_done = 0;
    u64 g6b_tr_start = 0;
    // ---- M23-G6C: P0's page-run cohorts, applied to translation admission ---
    // The table is P0's own `coh` (installed by `win_seal` whenever page_sched is
    // on, and unused by G6B's release path), so the resource is exactly the one
    // M7 charges: page_cohorts descriptors of 32 B. `g6c_coh_def`/`g6c_coh_age`
    // are the per-cohort deferral episode: refusals since the last admission, and
    // the cycle the episode began.
    u32 g6c_init = 0, g6c_open_pos = 0, g6c_submitted = 0;
    std::vector<u32> g6c_coh_def;
    std::vector<u64> g6c_coh_age;
    u8 g6b_cell_valid = 0;    // last cell the DRAM scheduler released from
    u32 g6b_bk = 0, g6b_rw = 0;
    u32 issued_n = 0;         // lines issued from this window (both policies)
    u32 n_ent = 0, n_pages = 0;
    size_t issue_pos = 0;
    u32 pending = 0, retired = 0;
    u64 fill_start = 0;   // cycle the buffer was adopted for filling
    // M9 window management: cycle of the most recent admission. E0/E1 read the
    // deadline off `fill_start`; E3/E4 read it off this, so that a window which
    // is still making progress is not truncated.
    u64 last_admit = 0;
  };
  struct FanJob {
    u32 batch = 0, line = 0, pos = 0;
    std::array<u8, LINE_BYTES> bytes{};
  };

  // ---- M13: bounded cross-window reuse buffer ------------------------------
  // One entry is one target line the engine has either already requested (and
  // will publish when the fill lands) or already holds. `subs` is the bounded
  // fanout group: the window line that owned the request plus any window line
  // that arrived while it was outstanding. Everything a served line needs is in
  // `data`, so a hit is a copy + fanout, exactly like a fill -- there is no
  // second code path that could extract a value differently.
  //
  // 88 B per entry, itemized: 64 B data + 4 B tag (VA line fits in 26 bits) +
  // 4 B LRU stamp + 1 B state + 1 B subscriber count + 3 x 4 B subscribers.
  static constexpr u32 REUSE_SUBS = 3;
  struct Sub {
    u32 batch = 0, line = 0;
  };
  struct ReuseEnt {
    u32 tag = 0xffffffffu;
    u32 lru = 0;
    u8 state = 0;  // 0 invalid, 1 outstanding, 2 data valid
    u8 nsub = 0;
    std::array<Sub, REUSE_SUBS> subs{};
    std::array<u8, LINE_BYTES> data{};
  };
  bool reuse_on() const { return cfg_.reuse_dir && windowed(); }
  // A mixing hash (two multiplies, high bits folded down), for the same reason
  // the DRAM bank index is hashed: the footprints this simulator generates are
  // strided, so `line % sets` aliases whole pages of lines onto a fraction of
  // the sets. Measured: the weak `%`/XOR forms left half the array unused on W1
  // (report §9). Correctness never depends on the choice -- the tag comparison
  // is exact either way, the index only decides where a line may live.
  u32 reuse_set_of(u64 va_line) const {
    u64 h = va_line;
    if (cfg_.reuse_hash) {
      h *= 0x9E3779B97F4A7C15ull;
      h ^= h >> 29;
      h *= 0xBF58476D1CE4E5B9ull;
      h ^= h >> 32;
    }
    return static_cast<u32>(h % reuse_sets_);
  }
  void reuse_init();
  // One lookup per distinct line of a freshly sealed window.
  void win_reuse_probe(Batch& b, u32 bi, u64 cycle);
  // Publish a completed line into its entry and fan it out to every dependant.
  void reuse_publish(u32 ent, const u8* bytes, u64 cycle);
  // Drop outstanding records (descriptor abort): their fills will never land.
  void reuse_abort();
  // One fanout job for (batch, line) from `bytes`.
  void push_fan(u32 bi, u32 li, const u8* bytes);
  void reuse_tick();
  std::vector<ReuseEnt> reuse_;
  u32 reuse_ways_ = 0, reuse_sets_ = 0;
  u32 reuse_occ_ = 0;     // valid entries, maintained at alloc/evict/abort
  u64 reuse_clock_ = 0;

  bool windowed() const { return cfg_.batch_window > 0; }
  // M15: the fused layout is a window-only, dedup-only representation.
  bool fused() const { return cfg_.win_fused && windowed(); }
  bool win_idle() const;
  // `pos` is the element's position in the descriptor, which is what the fused
  // entry stores in place of an explicit element id.
  bool win_admit_one(u64 va_line, u32 elem, u16 off, u8 width, u32 pos);
  void stream_index();
  void win_admit(u64 cycle);
  void win_seal(u64 cycle, u8 why);
  void win_issue(u64 cycle);
  // M7: issue from whichever open page run the dTLB will accept.
  void win_issue_sched(u64 cycle, Batch& b);
  // ---- M15: the same window in the fused layout ---------------------------
  // These mirror their M6 counterparts line for line; they exist as separate
  // functions so the published path compiles to exactly what it compiled to
  // before `win_fused` existed.
  void win_seal_fused(Batch& b);
  bool win_open_cohort_fused(Batch& b);
  void win_issue_sched_fused(u64 cycle, Batch& b);
  // M23-G6A: the two-phase driver. Phase 1 translates every run of the window
  // and nothing issues; phase 2 releases the buffered addresses in P2 order.
  void win_issue_g6a(u64 cycle, Batch& b);
  // M23-G6B: the streaming driver. Translation admission and DRAM release run
  // in the same cycle; the ready buffer is the only coupling between them.
  void win_issue_g6b(u64 cycle, Batch& b);
  // M23-G6C: P0's admission policy on top of the G6B pipeline. `g6c_admit` is
  // the single place the arm is selected; the ready buffer, the release path and
  // the capacity rule are G6B's unchanged.
  bool g6c_admit() const { return cfg_.bulk_org == 8; }
  void win_admit_g6c(u64 cycle, Batch& b);
  bool g6c_open_cohort(Batch& b);
  bool g6c_work_left(const Batch& b) const;
  int win_emit_fused(Batch& b, u32 li, u64 cycle);
  void win_fanout_fused(u64 cycle);
  // ---- M23-G3: run organization -------------------------------------------
  // Build `b.ford` for Config::bulk_org. Charges one organizer visit per run.
  void win_order_fused(Batch& b);
  // Run id at issue position `pos` (identity when the window is unordered).
  u32 run_at(const Batch& b, u32 pos) const {
    return b.ford.empty() ? pos : b.ford[pos];
  }
  // M28-D: the ready pool's key for run `li` -- its rank in `ford` when the
  // organizer built an order, and the run id itself when it did not. With
  // `m24_dram_org = 0` (the production default) `win_order_fused` leaves `ford`
  // empty, so the inverse permutation would be the identity and no array is
  // allocated for it. When the organizer is on, `rel_pos_` holds exactly the
  // `fpos` this replaces.
  u64 run_key(u32 li) const {
    return rel_pos_.empty() ? static_cast<u64>(li) : static_cast<u64>(rel_pos_[li]);
  }
  // ---- M23-G6F: candidate visibility ---------------------------------------
  // `rel_ready_` is a min-heap over run positions held in a vector (a
  // std::priority_queue hides its container, and the release scan has to read the
  // whole candidate population). These are the operations std::priority_queue
  // performs, so with `sched_vis == 0` the release sequence is identical.
  static void ready_push(std::vector<u64>& ready, u64 key) {
    ready.push_back(key);
    std::push_heap(ready.begin(), ready.end(), std::greater<u64>());
  }
  static void ready_pop(std::vector<u64>& ready) {
    std::pop_heap(ready.begin(), ready.end(), std::greater<u64>());
    ready.pop_back();
  }
  // Remove an arbitrary entry by key (positions are unique) and restore the
  // invariant in O(log n).
  static void ready_erase_val(std::vector<u64>& ready, u64 key) {
    if (!ready.empty() && ready.front() == key) { ready_pop(ready); return; }
    for (size_t i = 0; i < ready.size(); ++i) {
      if (ready[i] != key) continue;
      const size_t last = ready.size() - 1;
      if (i == last) { ready.pop_back(); return; }
      ready[i] = ready[last];
      ready.pop_back();
      size_t k = i;
      while (k > 0) {                       // sift up
        const size_t p = (k - 1) / 2;
        if (ready[p] <= ready[k]) break;
        std::swap(ready[p], ready[k]);
        k = p;
      }
      if (k == i) {                         // it did not rise: sift down
        for (;;) {
          const size_t l = 2 * k + 1, r = l + 1;
          size_t m = k;
          if (l < ready.size() && ready[l] < ready[m]) m = l;
          if (r < ready.size() && ready[r] < ready[m]) m = r;
          if (m == k) break;
          std::swap(ready[m], ready[k]);
          k = m;
        }
      }
      return;
    }
  }
  // The candidate the DRAM scheduler would prefer, out of the part of the buffer
  // it is allowed to see. Returns the chosen run position's key.
  u64 pick_candidate(Batch& b);
  // ---- M23-G6G: physical (bank,row) group release ---------------------------
  // Returns the position of the run to release: the open group's smallest
  // pending key, or -- when the open group is exhausted or none is open -- the
  // smallest key of the cell with the most pending runs. Metadata only; the
  // chosen run still goes through `win_emit_fused`.
  u64 pick_group(Batch& b);
  // M23-G6I: per-release cross-page bookkeeping and the pool sample. Both are
  // no-ops unless `Config::g6i_diag` is set, and neither can change a decision.
  void note_g6i_release(u64 cycle, const Batch& b, u32 li, u32 bank, u32 row);
  void g6i_sample(u64 cycle, const Batch& b);
  // Bookkeeping for a release `pick_group` chose, using the (bank,row) the
  // release loop already computed for its own cell-change counter.
  void note_group_release(Batch& b, u32 bank, u32 row);
  // Defined in bulk.cpp, where the fused word's field layout lives.
  u64 run_line(const Batch& b, u32 li) const;
  u32 run_count(const Batch& b, u32 li) const { return b.fmeta[li] & 0xffffu; }
  // Does run `li` belong to the same organization unit as the run opened first?
  bool same_run_unit(const Batch& b, u32 li, bool by_cell, u64 vpn, u32 bank, u32 row) const;
  void win_reuse_probe_fused(Batch& b, u32 bi, u64 cycle);
  // M7 audit: cycles the issue port must spend installing a page run. Charged,
  // not merely counted (docs/m8-pipelined-index-acquisition-report.md §9).
  u32 sched_busy_ = 0;
  // M23-G6C: the admission port, charged one cycle per installed page run on the
  // same rule `sched_busy_` charges the DRAM issue port in P0.
  u32 g6c_busy_ = 0;
  // Claim the next page run for a free cohort. False when `order` is exhausted.
  bool win_open_cohort(Batch& b);
  // Slots the target port may not take while the tiled acquisition unit is
  // active. One expression, shared by the issue decision and the readiness
  // attribution, so the two cannot drift apart.
  u32 tgt_reserve() const;
  // M27-P5: the SM whose L1/dTLB this engine's requests enter.
  u32 ecore() const { return cfg_.eng_core == 0xffffffffu ? 0u : cfg_.eng_core; }
  // Submit one target line: 0 issued, 1 no residency slot, 2 refused.
  int win_emit(Batch& b, u32 li, u64 cycle);
  // M8: index acquisition unit.
  void idx_stream(u64 cycle);          // claim lines into the filling tile, issue
  void idx_issue(u64 cycle);           // dedicated index issue port (mode 3)
  bool idx_take(u64& pos, const u8** line, u32& off);  // next consumable element
  void idx_consume(u64 pos);           // retire one consumed element
  bool idx_windowed() const { return windowed() && cfg_.idx_mode > 0; }
  void win_fanout(u64 cycle);
  void win_tick(u64 cycle);
  void win_reset();
  // M9 window management: the record of a buffer's current window, or null.
  BulkStats::WinRec* win_rec(int buf);
  void on_line(u64 cycle, const Pending& p);

  std::array<Batch, 2> bat_{};
  // ---- M28-D: the release path's per-run arrays, owned once ----------------
  // `fpa`/`ftr` (M23-G6A) and the `ready` heap with its `fpos` rank (M23-G6B)
  // are written and read only by the window that holds the single issue cursor
  // `iss_`: translation completion and the release pop both run inside
  // `win_issue_g6a`/`win_issue_g6b`, which `win_issue` calls for `bat_[iss_]`
  // only; `win_emit_fused` and the release-ordering helpers are reached from
  // those two paths alone; and a window retires (`iss_ = -1`) only once every
  // run has been released, at which point `issued_n >= |runs|` guarantees the
  // heap is empty. So the engine allocates these once, and the per-window
  // copies -- which the source never touched -- no longer exist.
  // `rel_pos_` is the inverse of `Batch::ford`, materialized only when `ford` is
  // non-empty; with the organizer off it stays empty and `run_key` returns the
  // run id, which is that identity.
  std::vector<u64> rel_pa_;     // physical line per run (was Batch::fpa)
  std::vector<u8> rel_tr_;      // translation landed per run (was Batch::ftr)
  std::vector<u64> rel_ready_;  // translated-but-unissued runs, min-heap of keys
  std::vector<u32> rel_pos_;    // run -> rank in `ford` (was Batch::fpos)
  // M28-F: the two per-run translation timestamps, on the same ownership
  // argument as the four above. A window `assign`s them in its own init block
  // (`g6a_phase == 0`, `!g6b_init`), before any submission or drain, so an
  // adopted window finds exactly the state a fresh one had; every write happens
  // after an accepted probe and every read at release, both inside the `iss_`
  // chain; and a window retires only once every run has been released, so no
  // stamp is still needed when `iss_` clears. Types, capacities, indexing and
  // timestamp meaning are unchanged -- only the number of copies.
  std::vector<u64> rel_gen_;    // probe submission cycle per run (was Batch::ftr_gen)
  std::vector<u64> rel_done_;   // probe completion cycle per run (was Batch::ftr_done)
  std::deque<FanJob> fan_;
  int fill_ = -1, iss_ = -1;   // buffer indices
  bool seal_pending_ = false;
  // buffer -> index in `st_.win_recs`, 0xffffffff when the buffer holds no window
  std::array<u32, 2> rec_ix_{{0xffffffffu, 0xffffffffu}};
  u64 win_occ_ = 0;            // entries held across both buffers
  u64 fan_backlog_ = 0;
  void attribute(u64 cycle, const std::vector<u64>& done);
  void on_desc(u64 cycle, const Pending& p);
  void on_index(u64 cycle, const Pending& p);
  void on_target(u64 cycle, const Pending& p);
  void generate(u64 cycle);
  void issue(u64 cycle);
  void finish_desc(u64 cycle);
  void abort_desc();
  u32 alloc_and_enqueue(ReqKind k, u64 va_line, u32 elem, u64 first_elem);
  // M9 slot plumbing: the only two places a slot changes hands in this file.
  u32 alloc_slot();
  void free_slot(u32 s);

  Config cfg_;
  MemorySystem& mem_;
  BulkStats st_;
  std::vector<std::string> err_;

  std::deque<BulkDesc> queue_;
  Run run_;
  bool active_ = false;
  u64 seq_ = 0;

  std::vector<Pending> info_;   // slot id -> request record
  std::deque<u32> unissued_;
  // M8: slots of index requests waiting on the dedicated issue port. They hold
  // residency exactly like `unissued_`, and `residency()` counts them.
  std::deque<u32> idx_q_;    // FIFO of allocated slots awaiting submission
  std::vector<u32> free_;
  // Non-null only when the driver owns the residency budget (M9 hybrid); the
  // engine then takes and returns slots through it and never touches `free_`.
  SlotPool* pool_ = nullptr;
  // ---- M23-G6F: candidate-visibility scratch --------------------------------
  // `g6f_stamp_` counts distinct (bank,row) cells among the inspected candidates
  // with one generation stamp per cell, so a scan costs O(candidates) and clears
  // nothing; `g6f_scratch_` holds the bounded reorder window when the configured
  // visibility is below the buffer size. Both stay empty unless `sched_vis` is
  // set, so the control allocates nothing new.
  std::vector<u32> g6f_stamp_;
  std::vector<u64> g6f_scratch_;
  u32 g6f_gen_ = 0;
  // ---- M23-G6G: group-release scratch ---------------------------------------
  // One word per physical cell (banks x rows, 16 384 in the production map):
  // `g6g_stamp_` generation-stamps a cell as "counted in this pass", `g6g_cnt_`
  // holds its pending-run count, `g6g_min_` its smallest key. `g6g_seen_`
  // stamps the window in which a cell last drained, which is how a reopened cell
  // is counted as a revisit. All empty unless `sched_group` is set.
  std::vector<u32> g6g_stamp_;
  std::vector<u32> g6g_cnt_;
  std::vector<u64> g6g_min_;
  std::vector<u32> g6g_seen_;
  std::vector<u32> g6g_cells_;
  u32 g6g_gen_ = 0;   // per-pass generation
  u32 g6g_wgen_ = 0;  // per-window generation for the revisit test
  // ---- M23-G6I: cross-page pipeline sample scratch -------------------------
  // `g6i_vpn_*` is an open-addressed, generation-stamped set of the virtual pages
  // present in one pool sample (14-bit, load <= 50 % at the 4 k baseline);
  // `g6i_cell_*` tracks, per physical cell, the first page seen this sample, how
  // many entries it holds and whether a second page also feeds it. Simulator-only
  // diagnostic storage: it is not part of any modelled SRAM charge and allocates
  // nothing unless `g6i_diag` is set.
  std::vector<u32> g6i_vpn_key_, g6i_vpn_gen_;
  std::vector<u32> g6i_cell_gen_, g6i_cell_vpn_, g6i_cell_n_;
  std::vector<u8> g6i_cell_multi_;
  std::vector<u64> g6i_last_rel_;   // last release cycle per physical cell
  u32 g6i_gen_ = 0;
  u64 g6i_last_sample_ = NONE, g6i_last_pub_vpn_ = NONE;
  u64 m24_prev_pos_ = NONE;  // M24-A: previous release position, per window
  // Diagnostic only: when set, index values come from here instead of memory.
  const std::vector<u64>* oracle_ = nullptr;

  std::map<u64, u64> idx_done_;   // first_elem -> va_line, filled out of order
  std::set<u64> idx_pending_;     // seqs of requested index lines not yet done
  std::set<u64> tgt_pending_;     // seqs of generated target requests not yet done
  std::unordered_map<u32, u64> val_, addr_;

  u64 last_tgt_elem_ = NONE;
  u64 idx_inflight_ = 0;   // index lines issued and not yet completed
  u64 idx_resident_ = 0;   // index lines holding a residency slot
};

// ---------------------------------------------------------------------------
// M5.1: one immutable workload, applied identically to every arm
// ---------------------------------------------------------------------------
struct BulkWorkload {
  BulkDesc desc;
  bool armed = false;  // false = no descriptor (engine idle)
  // (VA, bytes) writes that constitute the initial memory image. Built once,
  // replayed into each arm's fresh MemorySystem.
  std::vector<std::pair<u64, std::vector<u8>>> writes;
  // The index values themselves, in element order. No normal run reads them;
  // the `idx_oracle` diagnostic supplies them to the engine in place of the
  // index array, and the diagnostic driver uses them as the independent
  // expectation for every target address and output value.
  std::vector<u64> index_values;
  // M9: original element identity, position -> index into Trace::acc. Empty on
  // the legacy (unfiltered) workload, where identity is the position itself.
  // Element `j` of this descriptor is trace access `elem_ids[j]`.
  std::vector<u32> elem_ids;
  void apply(MemorySystem& mem) const;
};
// `cm` null = the whole trace, exactly as M5-M8 built it. Non-null = only the
// elements whose instruction the class map marks Bulk, compacted in trace order;
// when none qualify the workload stays unarmed, so no descriptor is ever written
// or submitted (docs/m9-dispatch-design.md §4.4, G3).
BulkWorkload build_bulk_workload(const Config& cfg, const Trace& tr, std::string& err,
                                 const ClassMap* cm = nullptr);

// Every configuration value that can influence memory performance. Both arms
// are checked against this list; a difference is a hard failure.
std::vector<std::pair<std::string, std::string>> environment_fields(const Config& cfg);
std::string environment_signature(const Config& cfg);

class Sim {
public:
  explicit Sim(const Config& cfg);
  // Shares one materialized trace instead of regenerating it (M5.1 §2).
  Sim(const Config& cfg, std::shared_ptr<const Trace> trace);
  RunResult run();
  RunResult run(Mode mode);
  // M9: both front-ends over one materialised trace, one MemorySystem and one
  // residency budget, dispatching per the class map (docs/m9-dispatch-design.md
  // §4.3). Reads Config::dispatch_order; never re-classifies anything.
  RunResult run_hybrid();
  RunResult run_bulk();
  RunResult run_bulk(const BulkWorkload& w);
  const Trace& trace() const { return *tr_; }
  const MemorySystem& mem() const { return *mem_; }
  MemorySystem& memory() { return *mem_; }
  const BulkEngine& engine() const { return *eng_; }
  // M9 diagnostic hook. Null unless `experiment=m9diag` asks for it; when null
  // every driver executes exactly the statements it executed before.
  void observe(CycleTrace* t) { cyc_ = t; }
  CycleTrace* cyc_ = nullptr;
  bool has_engine() const { return eng_ != nullptr; }

private:
  Config cfg_;
  std::shared_ptr<const Trace> tr_;
  std::unique_ptr<MemorySystem> mem_;
  std::unique_ptr<BulkEngine> eng_;
};

// ---------------------------------------------------------------------------
// M5.1: paired A/B experiment
// ---------------------------------------------------------------------------
// One arm's reportable numbers. `a` = accelerator OFF (SIMT), `b` = ON (bulk).
struct AbArm {
  const char* name = "";
  bool ok = true;
  u64 image_hash = 0;          // hash of the materialized memory image
  u64 cycles = 0;
  u64 requests = 0;            // line requests generated by the front-end
  u64 desc_requests = 0, index_requests = 0, target_requests = 0;
  u64 elements = 0;            // elements the workload contains (A: trace accesses)
  u64 l1_reads = 0, l1_hits = 0, l1_misses = 0, l1_mshr_joins = 0;
  u64 l2_reads = 0, l2_hits = 0, l2_misses = 0;
  u64 dram_reads = 0, dram_bytes = 0, dram_row_hits = 0, dram_row_conflicts = 0;
  u64 dram_busy_cycles = 0, dram_blocked_cycles = 0, dram_starved_cycles = 0;
  u64 l1_mshr_allocs = 0, l2_mshr_joins = 0, l2_mshr_allocs = 0;
  u64 l1_mshr_max = 0, l2_mshr_max = 0, replays = 0;
  u64 refuse_parked = 0, refuse_full = 0;  // why the dTLB refused a submission
  u64 tlb_reads = 0, tlb_hits = 0, tlb_misses = 0, tlb_mshr_stalls = 0;
  u64 l2tlb_reads = 0, l2tlb_hits = 0, l2tlb_dedups = 0;
  u64 walks = 0, pte_fetches = 0;
  u64 peak_outstanding = 0;    // peak total unissued + issued-not-filled
  u64 peak_slot = 0;           // peak resident in the shared R slots
  u64 staging_peak = 0;        // A only: coalescer latch outside the slots
  // M6 window resources and mechanism counters
  u64 win_entries = 0, win_occ_max = 0, dir_occ_max = 0, win_bytes = 0;
  u64 lines_issued = 0, dedup_merged = 0, org_cycles = 0, fanout_cycles = 0;
  u64 batches = 0, fanout_max_backlog = 0, win_full_stalls = 0;
  u64 dir_collisions = 0, fanout_elems = 0;
  u64 seals_full = 0, seals_end = 0, seals_deadline = 0;
  u64 buffer_full_stalls = 0;  // cycles the request buffer was full
  u64 issue_stalls = 0;        // refused submissions (memory backpressure)
  u64 l1_stall_cycles = 0, l2_stall_cycles = 0, tlb_stall_cycles = 0;
  u64 lat_mean_x100 = 0, lat_p50 = 0, lat_p95 = 0;
  u64 completion_hash = 0;
  // M6.1 timeline: phases of the run, not totals (report §3).
  u64 first_tgt_issue = 0, last_tgt_issue = 0, last_fill = 0;
};

struct AbResult {
  bool ok = false;
  std::vector<std::string> violations;
  Config cfg;
  std::string signature;
  u64 trace_hash = 0;   // the materialized trace both arms consumed
  BulkWorkload workload;
  AbArm a, b;
  double speedup = 0.0;                    // cycles(A) / cycles(B)
  double traffic_ratio = 0.0;              // dram_reads(B) / dram_reads(A)
};

// Runs both arms on one materialized workload with one configuration and checks
// that the environment is identical. Any mismatch sets ok = false and appears in
// `violations` — configuration problems are never warnings.
AbResult run_ab(const Config& cfg);
// Same experiment at R = 8, 16, 32, 64, 128 (§3 sensitivity sweep).
std::vector<AbResult> run_ab_sweep(const Config& base);
// M6: W x {dedup, page_order}, every cell a full paired A/B run.
std::vector<AbResult> run_m6_sweep(const Config& base);
void print_ab(const AbResult& r, std::ostream& os);

// ---------------------------------------------------------------------------
// M6.1: index-acquisition diagnostic (docs/m6-w0-diagnostic-report.md §4)
//
// One SIMT reference row and four engine variants of the *same* workload, the
// same output, the same memory hierarchy and the same R budget:
//
//   B0  windowed, exactly as measured (descriptor + index array fetched)
//   B1  descriptor supplied on the engine's descriptor port (desc_fetch = 0)
//   B2  index values supplied out of band (idx_oracle), descriptor fetched
//   B3  both acquisition costs removed
//
// B2/B3 are an oracle, not a hardware proposal: they keep every target request,
// every target latency and every residency rule, and remove only the traffic
// and dependency of *finding out which elements to touch*.
struct DiagRow {
  std::string name;
  bool ok = true;
  u64 cycles = 0;
  u64 desc_req = 0, index_req = 0, target_req = 0, total_req = 0;
  u64 dram_reads = 0, dram_bytes = 0, dram_row_hits = 0, dram_row_conflicts = 0;
  u64 dram_busy = 0, dram_starved = 0, dram_blocked = 0;
  u64 l1_reads = 0, l1_hits = 0, l1_misses = 0, l1_joins = 0, l1_mshr_max = 0;
  u64 l2_reads = 0, l2_hits = 0, l2_misses = 0, l2_joins = 0, l2_mshr_max = 0;
  u64 tlb_reads = 0, tlb_hits = 0, tlb_misses = 0, tlb_mshr_stalls = 0;
  u64 tlb_refuse_parked = 0, tlb_refuse_full = 0;
  u64 l2tlb_reads = 0, walks = 0, pte_fetches = 0;
  u64 peak_slots = 0, slot_occ_sum = 0, replays = 0;
  u64 first_tgt_issue = 0, last_tgt_issue = 0, last_fill = 0;
  u64 busy_at_first_tgt = 0, busy_at_last_tgt = 0;
  u64 idx_wait_stall = 0, issue_stall = 0, buf_full_stall = 0, slot_stall = 0;
  u64 org_cycles = 0, fanout_cycles = 0, win_entries = 0;
  u64 last_idx_issue = 0, last_idx_done = 0;
  u64 win_refuse = 0, idx_refuse = 0;
  u64 lat_mean_x100 = 0, lat_p50 = 0, lat_p95 = 0;
  u64 walker_busy = 0, l2tlb_hits = 0, l2tlb_misses = 0;
  u64 tlb_mshr_occ = 0, l1_mshr_occ = 0, l2_mshr_occ = 0;
  u64 addr_set_hash = 0;   // order-independent hash of every target address
  u64 oracle_elems = 0;
  u64 completion_hash = 0, value_hash = 0, expected_hash = 0;
  u64 elems = 0, addr_mismatch = 0;
  // M7 scheduling: lost cycles, deferral events, switching and metadata.
  u64 sched_idle = 0, sched_defer = 0, sched_opens = 0, sched_switches = 0;
  u64 sched_probes = 0, sched_cycles = 0, sched_bytes = 0, max_parked = 0;
  // M8 index acquisition.
  u64 idx_wait = 0, idx_tile_swaps = 0, idx_buf_stall = 0, idx_ooo = 0;
  u64 idx_ready_max = 0, idx_res_max = 0, idx_bytes = 0;
  u64 idx_first_issue = 0, idx_last_done = 0;
  std::vector<std::string> violations;
};

struct DiagResult {
  bool ok = true;
  std::vector<std::string> violations;
  // Diagnostics that were not applicable to this run, with the reason. Never a
  // pass and never a failure: they record why an identity was not evaluated, so
  // a skipped check is visible instead of silently omitted (M8 audit §2).
  std::vector<std::string> notes;
  Config cfg;
  u64 trace_hash = 0;
  u64 image_hash = 0;
  DiagRow a;                 // SIMT reference
  std::vector<DiagRow> b;    // B0..B3
  u64 bank_idle_prefix = 0, bank_idle_span = 0, bank_idle_drain = 0;
  u64 prefix_cycles = 0, span_cycles = 0, drain_cycles = 0;
};
DiagResult run_index_oracle_diag(const Config& cfg);
void print_index_oracle_diag(const DiagResult& r, std::ostream& os);
void print_ab_sweep(const std::vector<AbResult>& rs, std::ostream& os);
void print_m6_sweep(const std::vector<AbResult>& rs, std::ostream& os);

// ---------------------------------------------------------------------------
// M9: the three-arm dispatch experiment (docs/m9-dispatch-design.md §7)
// ---------------------------------------------------------------------------
// A = SIMT only, B = bulk only (the M8 path, one descriptor, whole trace),
// C = hybrid per the class map. All three share one materialised trace and one
// environment; each gets a fresh MemorySystem.
struct M9Result {
  bool ok = true;
  std::vector<std::string> violations;
  std::vector<std::string> notes;
  Config cfg;
  u64 trace_hash = 0, image_hash = 0;
  u64 instr_simt = 0, instr_bulk = 0, elems_simt = 0, elems_bulk = 0;
  BulkWorkload workload;   // C: compacted to the bulk-class elements
  BulkWorkload reference;  // A/B: the whole trace
  RunResult a, b, c;
  // Output verification, computed against the memory image independently of
  // both engines' own bookkeeping (ab.cpp truth_hash / engine_hash).
  u64 b_value_hash = 0, b_expected_hash = 0, c_value_hash = 0, c_expected_hash = 0;
  u64 b_mismatch = 0, c_mismatch = 0;
  bool all_simt() const { return instr_bulk == 0; }
  bool all_bulk() const { return instr_simt == 0; }
};
M9Result run_m9(const Config& cfg, CycleTrace* ta = nullptr, CycleTrace* tb = nullptr,
                CycleTrace* tc = nullptr);
void print_m9(const M9Result& r, std::ostream& os);
void print_m9_diag(const M9Result& r, const CycleTrace& ta, const CycleTrace& tb,
                   const CycleTrace& tc, std::ostream& os);

}  // namespace dx100
