// SIMT front-end + driver: coalescing, slot residency (R), conservation,
// Vortex's LSU outstanding pool, and the M4a/M4b comparison harnesses.
#include "sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace dx100 {

namespace {

// FNV-1a: stable completion-sequence hash for the M4a exact test.
inline u64 fnv(u64 h, u64 v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xff;
    h *= 1099511628211ull;
  }
  return h;
}

struct SimtCore {
  u32 id = 0;
  std::vector<u32> instrs;
  size_t next = 0;
  std::deque<Req> held;
  std::vector<u32> free_slots;
  std::vector<u64> cur_lines;
  size_t cur_pos = 0;
  u64 completed = 0, issued = 0;

  bool has_work() const {
    return next < instrs.size() || !held.empty() || cur_pos < cur_lines.size();
  }
};

// Independent issuer used by M4a: plain per-core FIFO, one request per cycle,
// no per-warp skip. It must behave identically to the baseline front-end when
// the baseline is placed in the same neutral profile.
struct NeutralCore {
  u32 id = 0;
  std::vector<u64> lines;   // flattened, in trace order
  size_t head = 0;
  std::vector<Req> q;
  size_t qhead = 0;
  std::vector<u32> free_slots;
  u64 completed = 0;

  bool has_work() const { return head < lines.size() || qhead < q.size(); }
};

}  // namespace

Sim::Sim(const Config& cfg)
    : cfg_(cfg), tr_(std::make_shared<const Trace>(build_trace(cfg))) {
  mem_ = std::make_unique<MemorySystem>(cfg_);
}

// One materialized trace shared by every arm (M5.1 §2/§8): the workload is built
// once and neither arm can regenerate it differently.
Sim::Sim(const Config& cfg, std::shared_ptr<const Trace> trace)
    : cfg_(cfg), tr_(std::move(trace)) {
  mem_ = std::make_unique<MemorySystem>(cfg_);
}

RunResult Sim::run() {
  // M9: the only line this file gains. `dispatch = 0` -- every published
  // configuration, and the default -- takes exactly the old branch, so the SIMT
  // path below is untouched.
  if (cfg_.dispatch) return run_hybrid();
  return cfg_.mode == Mode::Bulk ? run_bulk() : run(cfg_.mode);
}

RunResult Sim::run(Mode mode) {
  if (mode == Mode::Bulk) return run_bulk();
  RunResult res;
  Stats& s = res.s;

  // Instruction -> trace range.
  std::vector<u32> begin(tr_->n_instr + 1, 0);
  for (const auto& a : tr_->acc) begin[a.instr + 1]++;
  for (u32 i = 0; i < tr_->n_instr; ++i) begin[i + 1] += begin[i];

  // Residency pool. M5.1 §8.1: the SIMT coalescer holds up to `lanes` line
  // addresses of the instruction it is expanding, outside the slot count. With
  // ab_charge_latch those four are charged against the budget, so the arm's peak
  // outstanding is exactly R — the same as the engine's. The flag is off by
  // default so the published M5.1 numbers reproduce unchanged.
  const u32 latch_charge = cfg_.ab_charge_latch ? cfg_.lanes : 0;
  const u32 per_core =
      std::max<u32>(1, cfg_.R / std::max<u32>(1, cfg_.cores) - latch_charge);
  const bool blocked = cfg_.core_map == "blocked";
  const u32 chunk = tr_->n_instr ? (tr_->n_instr + cfg_.cores - 1) / cfg_.cores : 0;

  std::vector<SimtCore> cores(cfg_.cores);
  std::vector<NeutralCore> ncores(cfg_.cores);
  for (u32 c = 0; c < cfg_.cores; ++c) {
    cores[c].id = c;
    cores[c].free_slots.resize(per_core);
    for (u32 i = 0; i < per_core; ++i) cores[c].free_slots[i] = i;
    ncores[c].id = c;
    ncores[c].free_slots = cores[c].free_slots;
    for (u32 i = 0; i < tr_->n_instr; ++i) {
      const bool mine = blocked ? (i / std::max<u32>(1, chunk) == c) : (i % cfg_.cores == c);
      if (!mine) continue;
      cores[c].instrs.push_back(i);
      for (u32 k = begin[i]; k < begin[i + 1]; ++k) {
        const u64 line = tr_->acc[k].line;
        auto& v = ncores[c].lines;
        if (std::find(v.begin() + static_cast<long>(ncores[c].head), v.end(), line) == v.end())
          ncores[c].lines.push_back(line);  // coalesced, same rule as the baseline
      }
    }
  }

  s.set("cores", cfg_.cores);
  s.set("slots_total", static_cast<u64>(cfg_.cores) * per_core + static_cast<u64>(latch_charge) * cfg_.cores);
  s.set("slot_pool_per_core", per_core);
  s.set("latch_charge", latch_charge);
  s.set("trace_accesses", tr_->acc.size());
  s.set("trace_instr", tr_->n_instr);
  s.set("trace_lines", tr_->lines_real);
  s.set("trace_pages", tr_->pages_real);
  s.set("lsu_pending", cfg_.lsu_pending);

  u64 cycle = 0;
  const u64 limit = cfg_.max_cycles;
  u64 hash = 1469598103934665603ull;
  std::vector<u64> in_sys(cfg_.cores, 0);

  // M5.1 residency accounting. `slot` is what the R budget bounds: requests that
  // have been formed and hold a slot (unissued + issued-not-filled). `latched`
  // is the SIMT coalescer's input latch for the instruction currently being
  // expanded — it is bounded by `lanes`, is not a queue, and is reported
  // separately so neither front-end can hide capacity.
  u64 peak_slot = 0, peak_outstanding = 0, staging_max = 0, slot_full = 0;
  bool latch_reported = false;

  for (;; ++cycle) {
    std::vector<u64> done;
    mem_->tick(cycle, done);
    for (u64 p : done) {
      const u32 core = static_cast<u32>(p >> 32);
      const u32 slot = static_cast<u32>(p & 0xffffffffu);
      hash = fnv(hash, slot);
      hash = fnv(hash, cycle);
      s.inc("completed");
      if (in_sys[core] > 0) in_sys[core]--;
      if (mode == Mode::Baseline) {
        cores[core].free_slots.push_back(slot);
        cores[core].completed++;
      } else {
        ncores[core].free_slots.push_back(slot);
        ncores[core].completed++;
      }
    }

    bool any_work = false;

    if (mode == Mode::Baseline) {
      for (auto& c : cores) {
        if (c.has_work()) any_work = true;
        if (cfg_.lsu_pending && in_sys[c.id] >= cfg_.lsu_pending) continue;

        // Issue: up to one request per warp per cycle; a request blocked on its
        // own translation must not block the other warps.
        u32 attempts = 0;
        const u32 cap = std::max<u32>(1, cfg_.warps_per_core);
        for (size_t i = 0; i < c.held.size() && attempts < cap;) {
          Req r = c.held[i];
          if (mem_->submit(r, cycle)) {
            c.held.erase(c.held.begin() + static_cast<long>(i));
            s.inc("issued");
            in_sys[c.id]++;
          } else {
            ++i;
            s.inc("simt_issue_stalls");  // memory backpressure, retried next cycle
          }
          ++attempts;
        }

        // Coalesce the next instruction into distinct line requests.
        if (c.cur_pos >= c.cur_lines.size() && c.next < c.instrs.size()) {
          const u32 id = c.instrs[c.next];
          c.cur_lines.clear();
          c.cur_pos = 0;
          for (u32 i = begin[id]; i < begin[id + 1]; ++i) {
            const u64 line = tr_->acc[i].line;
            if (std::find(c.cur_lines.begin(), c.cur_lines.end(), line) == c.cur_lines.end())
              c.cur_lines.push_back(line);
          }
          c.next++;
          s.inc("instr_issued");
          s.inc("requests_generated", c.cur_lines.size());
        }

        while (c.cur_pos < c.cur_lines.size() && !c.free_slots.empty()) {
          Req r;
          r.va_line = c.cur_lines[c.cur_pos++];
          r.vpn = r.va_line >> mem_->page_line_bits();
          r.core = c.id;
          r.slot = c.free_slots.back();
          c.free_slots.pop_back();
          r.seq = s.get("requests_generated");
          s.inc("slot_allocs");
          c.held.push_back(r);
        }
        s.inc("unissued_cycles", c.held.size());
        if (c.held.size() > s.get("unissued_max")) s.set("unissued_max", c.held.size());
      }
    } else {
      for (auto& c : ncores) {
        if (c.has_work()) any_work = true;
        if (cfg_.lsu_pending && in_sys[c.id] >= cfg_.lsu_pending) continue;

        if (c.qhead < c.q.size()) {
          Req r = c.q[c.qhead];
          if (mem_->submit(r, cycle)) { c.qhead++; s.inc("issued"); in_sys[c.id]++; }
          else s.inc("simt_issue_stalls");
        }
        while (c.head < c.lines.size() && !c.free_slots.empty()) {
          Req r;
          r.va_line = c.lines[c.head++];
          r.vpn = r.va_line >> mem_->page_line_bits();
          r.core = c.id;
          r.slot = c.free_slots.back();
          c.free_slots.pop_back();
          s.inc("requests_generated");
          s.inc("slot_allocs");
          c.q.push_back(r);
        }
        s.inc("unissued_cycles", c.q.size() - c.qhead);
      }
    }

    {
      u64 slot = 0, latched = 0, full = 0, latch_core_max = 0;
      if (mode == Mode::Baseline) {
        for (const auto& c : cores) {
          slot += c.held.size() + in_sys[c.id];
          const u64 mine = c.cur_pos < c.cur_lines.size() ? c.cur_lines.size() - c.cur_pos : 0;
          latched += mine;
          latch_core_max = std::max(latch_core_max, mine);
          if (mine > 0 && c.free_slots.empty()) ++full;
        }
      } else {
        for (const auto& c : ncores) slot += (c.q.size() - c.qhead) + in_sys[c.id];
      }
      peak_slot = std::max(peak_slot, slot);
      peak_outstanding = std::max(peak_outstanding, slot + latched);
      staging_max = std::max(staging_max, latched);
      slot_full += full;
      if (cyc_) {  // M9 diagnostic: pure observation, no state touched
        CycleSample q;
        q.simt_done = static_cast<u32>(s.get("completed"));
        q.pool_simt = static_cast<u32>(slot);
        const u64 cap = static_cast<u64>(cfg_.cores) * per_core;
        q.pool_free = static_cast<u32>(cap > slot ? cap - slot : 0);
        q.simt_latch = static_cast<u32>(latched);
        q.mem_inflight = static_cast<u32>(mem_->inflight_count());
        q.issued = static_cast<u32>(s.get("issued"));
        q.simt_stalls = static_cast<u32>(s.get("simt_issue_stalls"));
        cyc_->v.push_back(q);
      }
      if (mode == Mode::Baseline && latch_core_max > cfg_.lanes && !latch_reported) {
        res.ok = false;
        latch_reported = true;
        res.violations.push_back("coalescer latch " + std::to_string(latch_core_max) +
                                 " exceeds lanes " + std::to_string(cfg_.lanes));
      }
    }
    if (!any_work && mem_->idle()) break;
    // M27-P2: liveness is a correctness invariant, not a harness switch. A run
    // that still holds work while nothing has been admitted or has retired for
    // longer than the model's own worst-case service time cannot finish, even
    // though every conservation counter still balances. `stall_abort` (M26
    // harness, default 0) only tightens the bound for tests.
    {
      const u64 thr = cfg_.stall_abort ? cfg_.stall_abort : mem_->live_limit();
      if ((any_work || !mem_->idle()) && mem_->liveness_tripped(cycle, thr)) {
        res.ok = false;
        res.violations.push_back("liveness: no admission or retirement for " +
                                 std::to_string(cycle - mem_->last_progress_cycle()) +
                                 " cycles (bound " + std::to_string(thr) + "); " +
                                 mem_->outstanding_report());
        break;
      }
    }
    if (cycle >= limit) {
      res.ok = false;
      res.violations.push_back("cycle limit exceeded (deadlock guard)");
      break;
    }
  }

  mem_->finish(cycle);
  res.cycles = cycle;
  res.completion_hash = hash;
  const double clk_hz = 400e6;
  res.lines_per_cycle = cycle ? double(tr_->lines_real) / double(cycle) : 0.0;
  res.gbs = cycle ? double(mem_->dram.bytes) / double(cycle) * clk_hz / 1e9 : 0.0;

  s.set("cycles", cycle);
  s.set("requests_accepted", s.get("issued"));
  s.set("completion_hash", hash & 0xffffffffull);

  for (u32 c = 0; c < cfg_.cores; ++c)
    s.set("core" + std::to_string(c) + "_completed",
          mode == Mode::Baseline ? cores[c].completed : ncores[c].completed);
  collect_mem_stats(*mem_, cfg_, s);

  // ---- completion behaviour (M4b family 4) --------------------------------
  collect_lat_stats(*mem_, res);

  u64 held_end = 0;
  if (mode == Mode::Baseline) {
    for (const auto& c : cores) held_end += c.held.size() + (c.cur_lines.size() - c.cur_pos);
  } else {
    for (const auto& c : ncores) held_end += (c.lines.size() - c.head) + (c.q.size() - c.qhead);
  }
  s.set("requests_unissued_end", held_end);
  s.set("simt_peak_slot", peak_slot);
  s.set("simt_peak_outstanding", peak_outstanding);
  s.set("simt_staging_max", staging_max);
  s.set("simt_slot_full_cycles", slot_full);
  s.set("mem_path", cfg_.mem_path == "l2" ? 2 : (cfg_.mem_path == "l1" ? 1 : 0));

  auto check = [&](const char* name, u64 lhs, u64 rhs) {
    if (lhs != rhs) {
      res.ok = false;
      res.violations.push_back(std::string(name) + ": " + std::to_string(lhs) +
                               " != " + std::to_string(rhs));
    }
  };
  check("generated == issued + unissued", s.get("requests_generated"),
        s.get("issued") + held_end);
  check("issued == completed + inflight", s.get("issued"),
        s.get("completed") + mem_->inflight_count());
  check("tlb0 reads == hits + misses", mem_->tlb[0].reads,
        mem_->tlb[0].hits + mem_->tlb[0].misses);
  check("l1 reads == hits + misses", mem_->l1.reads, mem_->l1.hits + mem_->l1.misses);
  check("l2 reads == hits + misses", mem_->l2.reads, mem_->l2.hits + mem_->l2.misses);
  check("l2tlb reads == hits + misses", mem_->l2tlb.reads,
        mem_->l2tlb.hits + mem_->l2tlb.misses);
  check("completed == requests_generated", s.get("completed"), s.get("requests_generated"));
  check("peak slot residency <= R", peak_slot <= cfg_.R ? 0 : 1, 0);
  // With the latch charged there is no capacity outside the budget, so this is
  // the arm-level statement that both front-ends are equal (M5.1 §4).
  if (cfg_.ab_charge_latch)
    check("peak outstanding <= R (latch charged)", peak_outstanding <= cfg_.R ? 0 : 1, 0);
  s.set("conservation_ok", res.ok ? 1 : 0);
  s.inc("conservation_violations", res.violations.size());
  return res;
}

// One definition of the shared downstream counters, imported by every
// front-end so a bulk run reports the hierarchy exactly as a baseline run does.
void collect_lat_stats(const MemorySystem& mem, RunResult& res) {
  Stats& s = res.s;
  s.set("lat_sum", mem.lat_sum);
  s.set("lat_tlb_sum", mem.tlb_lat_sum);
  s.set("lat_cache_sum", mem.cache_lat_sum);
  if (mem.lat_all.empty()) return;
  std::vector<u32> v = mem.lat_all;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  res.lat_mean = double(mem.lat_sum) / double(n);
  res.lat_p50 = v[n / 2];
  res.lat_p95 = v[(n * 95) / 100];
  s.set("lat_mean_x100", static_cast<u64>(res.lat_mean * 100));
  s.set("lat_p50", res.lat_p50);
  s.set("lat_p95", res.lat_p95);
  s.set("lat_tlb_mean", mem.tlb_lat_sum / n);
  s.set("lat_cache_mean", mem.cache_lat_sum / n);
}

void collect_mem_stats(const MemorySystem& mem, const Config& cfg, Stats& s) {
  // Per-core is not the same as per-client: a bulk run has one client but still
  // reports core 0's client-side TLB, so both arms' numbers are comparable.
  for (u32 c = 0; c < cfg.cores; ++c) {
    const std::string p = "tlb" + std::to_string(c) + "_";
    s.set(p + "reads", mem.tlb[c].reads);
    s.set(p + "hits", mem.tlb[c].hits);
    s.set(p + "misses", mem.tlb[c].misses);
    s.set(p + "mshr_stalls", mem.tlb[c].stall_cycles);
    s.set(p + "mshr_max", mem.tlb[c].mshr_max);
    s.set(p + "mshr_occ", mem.tlb[c].mshr_occ);
    for (u32 hi = 0; hi < 17; ++hi)
      s.set(p + "mshr_h" + std::to_string(hi), mem.tlb[c].mshr_hist[hi]);
  }
  s.set("l1_reads", mem.l1.reads);
  s.set("l1_hits", mem.l1.hits);
  s.set("l1_misses", mem.l1.misses);
  s.set("l1_evict", mem.l1.evict);
  s.set("l1_mshr_allocs", mem.l1.mshr_allocs);
  s.set("l1_mshr_joins", mem.l1.mshr_joins);
  s.set("l1_mshr_occ", mem.l1.occ);
  s.set("l1_mshr_max", mem.l1.max_occ);
  s.set("l1_stall_cycles", mem.l1.stall_cycles);
  s.set("l2_reads", mem.l2.reads);
  s.set("l2_hits", mem.l2.hits);
  s.set("l2_misses", mem.l2.misses);
  s.set("l2_evict", mem.l2.evict);
  s.set("l2_mshr_allocs", mem.l2.mshr_allocs);
  s.set("l2_mshr_joins", mem.l2.mshr_joins);
  s.set("l2_mshr_occ", mem.l2.occ);
  s.set("l2_mshr_max", mem.l2.max_occ);
  s.set("l2tlb_reads", mem.l2tlb.reads);
  s.set("l2tlb_hits", mem.l2tlb.hits);
  s.set("l2tlb_misses", mem.l2tlb.misses);
  s.set("l2tlb_evict", mem.l2tlb.evict);
  s.set("l2tlb_allocs", mem.l2tlb.allocs);
  s.set("l2tlb_dedups", mem.l2tlb.dedups);
  s.set("l2tlb_occ", mem.l2tlb.occ);
  s.set("l2tlb_max_occ", mem.l2tlb.max_occ);
  s.set("l2tlb_walk_stalls", mem.l2tlb.walk_stalls);
  for (u32 hi = 0; hi < 17; ++hi)
    s.set("l2tlb_occ_h" + std::to_string(hi), mem.l2tlb.occ_hist[hi]);
  s.set("walks", mem.ptw.walks);
  s.set("walk_wc_hits", mem.ptw.wc_hits);
  s.set("walk_latency", mem.ptw.walk_latency);
  s.set("pte_fetches", mem.ptw.pte_fetches);
  s.set("walker_busy", mem.ptw.walker_busy);
  s.set("walker_max", mem.ptw.walker_max);
  mem.dump_hbm(s);
  s.set("dram_reads", mem.dram.reads);
  s.set("dram_row_hits", mem.dram.row_hits);
  s.set("dram_row_conflicts", mem.dram.row_conflicts);
  s.set("dram_q_full", mem.dram.q_full);
  s.set("dram_bytes", mem.dram.bytes);
  s.set("dram_max_q", mem.dram.max_q);
  s.set("dram_bank_mask", mem.dram.bank_mask);
  s.set("inflight_occ", mem.inflight_occ);
  s.set("inflight_max", mem.inflight_max);
  s.set("dram_blocked_cycles", mem.dram.blocked_cycles);
  s.set("dram_starved_cycles", mem.dram.starved_cycles);
  // M23-G2C: queue arrivals by cycle, and the eligible-but-unissued guard.
  // All four stay 0 unless `dram_diag` is set.
  s.set("dram_arrivals", mem.dram.arrivals);
  s.set("dram_arrive_cycles", mem.dram.arrive_cycles);
  s.set("dram_arrive_multi_cycles", mem.dram.arrive_multi_cycles);
  s.set("dram_elig_wait_cycles", mem.dram.elig_wait_cycles);
  // M9 capacity diagnostic: DRAM scheduling candidates and queue depth.
  s.set("dram_svc_events", mem.dram.svc_events);
  s.set("dram_elig_sum", mem.dram.elig_sum);
  s.set("dram_elig_max", mem.dram.elig_max);
  s.set("dram_q_occ_sum", mem.dram.q_occ_sum);
  // Bank-busy time and MSHR occupancy: the two resources that decide whether a
  // DRAM row-hit improvement can turn into a cycle improvement at all.
  s.set("dram_busy_cycles", mem.dram.busy_cycles);
  // ---- M11.2 scheduler diagnostic -----------------------------------------
  // All zero unless the run set `dram_diag`. Nothing above or below reads them.
  s.set("dram_diag_decisions", mem.dram.diag_decisions);
  s.set("dram_hit_cand_sum", mem.dram.hit_cand_sum);
  s.set("dram_hit_busy_sum", mem.dram.hit_busy_sum);
  s.set("dram_hit_elig_sum", mem.dram.hit_elig_sum);
  s.set("dram_missed_hit", mem.dram.missed_hit);
  s.set("dram_stall_case_b", mem.dram.stall_case_b);
  s.set("dram_stall_case_c", mem.dram.stall_case_c);
  s.set("dram_wait_sum", mem.dram.wait_sum);
  s.set("dram_wait_max", mem.dram.wait_max);
  s.set("dram_svc_hit_cycles", mem.dram.svc_hit_cycles);
  s.set("dram_svc_miss_cycles", mem.dram.svc_miss_cycles);
  s.set("dram_rows_used", mem.dram.rows_used);
  s.set("dram_row_acts_sum", mem.dram.row_acts_sum);
  // M23-G6E visibility audit (zero unless dram_diag).
  s.set("dram_vis_rows_sum", mem.dram.vis_rows_sum);
  s.set("dram_vis_rows_max", mem.dram.vis_rows_max);
  s.set("dram_vis_banks_sum", mem.dram.vis_banks_sum);
  for (u32 k = 0; k < 16; ++k)
    s.set("dram_vis_rows_h" + std::to_string(k), mem.dram.vis_rows_hist[k]);
  for (u32 k = 0; k < 17; ++k)
    s.set("dram_hit_cand_h" + std::to_string(k), mem.dram.hit_cand_hist[k]);
  s.set("dram_row_acts_max", mem.dram.row_acts_max);
  for (u32 k = 0; k < 16; ++k) {
    s.set("dram_q_h" + std::to_string(k), mem.dram.q_occ_hist[k]);
    s.set("dram_wait_h" + std::to_string(k), mem.dram.wait_hist[k]);
  }
  for (u32 k = 0; k < 4; ++k) {
    const std::string p = "dram_bank" + std::to_string(k);
    s.set(p + "_reads", mem.dram.bank_reads[k]);
    s.set(p + "_hits", mem.dram.bank_hits[k]);
    s.set(p + "_conf", mem.dram.bank_conf[k]);
    s.set(p + "_busy", mem.dram.bank_busy[k]);
    s.set(p + "_qsum", mem.dram.bank_q_sum[k]);
    s.set(p + "_qmax", mem.dram.bank_q_max[k]);
  }
  for (u32 k = 0; k < 4; ++k) {
    const std::string p = "dram_client" + std::to_string(k);
    s.set(p + "_reads", mem.dram.client_reads[k]);
    s.set(p + "_hits", mem.dram.client_hits[k]);
    s.set(p + "_wait", mem.dram.client_wait[k]);
    s.set(p + "_conf", mem.dram.client_conf[k]);
    s.set(p + "_qsum", mem.dram.client_q_sum[k]);
    s.set(p + "_qmax", mem.dram.client_q_max[k]);
    s.set(p + "_interf", mem.dram.client_interf[k]);
  }
  // ---- M15: per-requester attribution --------------------------------------
  // Zero unless the run set `cache_diag`. The four tag values are the M11.3
  // client index the hierarchy already carries: 0 SIMT, 1 engine index stream,
  // 2 page-walker PTE fetch, 3 engine target stream.
  for (u32 k = 0; k < 4; ++k) {
    const std::string p = "cc" + std::to_string(k);
    s.set(p + "_l1_reads", mem.cc[k].l1_reads);
    s.set(p + "_l1_hits", mem.cc[k].l1_hits);
    s.set(p + "_l1_misses", mem.cc[k].l1_misses);
    s.set(p + "_l1_joins", mem.cc[k].l1_joins);
    s.set(p + "_l1_allocs", mem.cc[k].l1_allocs);
    s.set(p + "_l1_evict", mem.cc[k].l1_evict);
    s.set(p + "_l1_stalls", mem.cc[k].l1_stalls);
    s.set(p + "_l2_reads", mem.cc[k].l2_reads);
    s.set(p + "_l2_hits", mem.cc[k].l2_hits);
    s.set(p + "_l2_misses", mem.cc[k].l2_misses);
    s.set(p + "_l2_joins", mem.cc[k].l2_joins);
    s.set(p + "_l2_allocs", mem.cc[k].l2_allocs);
    s.set(p + "_l2_evict", mem.cc[k].l2_evict);
    s.set(p + "_l2_stalls", mem.cc[k].l2_stalls);
    s.set(p + "_l2_hit_self", mem.cc[k].l2_hit_self);
    s.set(p + "_l2_hit_cross", mem.cc[k].l2_hit_cross);
    s.set(p + "_l2_evict_self", mem.cc[k].l2_evict_self);
    s.set(p + "_l2_evict_cross", mem.cc[k].l2_evict_cross);
    s.set(p + "_issue", mem.cc[k].issue);
    s.set(p + "_done", mem.cc[k].done);
    s.set(p + "_lat_sum", mem.cc[k].lat_sum);
    s.set(p + "_first_issue", mem.cc[k].first_issue == NONE ? 0 : mem.cc[k].first_issue);
    s.set(p + "_last_issue", mem.cc[k].last_issue);
    s.set(p + "_first_done", mem.cc[k].first_done == NONE ? 0 : mem.cc[k].first_done);
    s.set(p + "_last_done", mem.cc[k].last_done);
  }
  s.set("l1_mshr_occ", mem.l1.occ);
  s.set("l2_mshr_occ", mem.l2.occ);
  s.set("replays", mem.replays);
  s.set("tlb_refuse_parked", mem.refuse_parked);
  s.set("tlb_refuse_full", mem.refuse_full);
  // M7: largest set of requests seen parked on one dTLB MSHR (per-page waiters).
  s.set("tlb_max_parked", mem.max_parked);
  // M26-B1: lifecycle counters (and, when `life_dump` names a file, the detailed
  // per-request report). Renames nothing: `dump_life` returns immediately unless
  // Config::life_diag was set.
  mem.dump_life(s);
}

}  // namespace dx100
