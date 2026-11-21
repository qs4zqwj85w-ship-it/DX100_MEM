// M5.1: controlled A/B experiment driver.
//
// One configuration, one materialized workload, two arms:
//   A — accelerator OFF: the conventional SIMT front-end.
//   B — accelerator ON:  the bulk indirect-access engine.
//
// Both arms run on a freshly constructed MemorySystem from the same Config, share
// the same Trace object and the same memory image, and are bounded by the same
// R-slot residency budget. Any environmental difference is a hard failure
// (`ok = false` + a violation string), never a warning.
#include "sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dx100 {

namespace {

std::string num(u64 v) { return std::to_string(v); }

// Reads one arm's numbers out of its Stats map. The two arms export the shared
// hierarchy counters under the same names; the front-end-specific volume
// counters are read from the keys each front-end actually sets.
AbArm collect(const char* name, const RunResult& r, bool engine_arm, u64 image_hash,
              u64 elements) {
  AbArm arm;
  arm.name = name;
  arm.ok = r.ok;
  arm.image_hash = image_hash;
  arm.cycles = r.cycles;
  arm.elements = elements;
  const Stats& s = r.s;

  arm.requests = s.get("requests_generated");
  if (engine_arm) {
    arm.desc_requests = s.get("bulk_desc_submitted");
    arm.index_requests = s.get("bulk_idx_lines_req");
    arm.target_requests = s.get("bulk_tgt_req_gen");
    arm.peak_outstanding = s.get("bulk_buf_max");
    arm.peak_slot = s.get("bulk_buf_max");
    arm.staging_peak = 0;
    arm.buffer_full_stalls = s.get("bulk_buf_full_stall");
    arm.issue_stalls = s.get("bulk_issue_stall");
    arm.win_entries = s.get("bulk_win_entries");
    arm.win_occ_max = s.get("bulk_win_occ_max");
    arm.dir_occ_max = s.get("bulk_dir_occ_max");
    arm.win_bytes = s.get("bulk_win_bytes");
    arm.lines_issued = s.get("bulk_lines_issued");
    arm.dedup_merged = s.get("bulk_dedup_merged");
    arm.org_cycles = s.get("bulk_org_cycles");
    arm.fanout_cycles = s.get("bulk_fanout_cycles");
    arm.batches = s.get("bulk_batches");
    arm.fanout_max_backlog = s.get("bulk_fanout_max_backlog");
    arm.fanout_elems = s.get("bulk_fanout_elems");
    arm.dir_collisions = s.get("bulk_dir_collisions");
    arm.win_full_stalls = s.get("bulk_win_full_stall");
    arm.seals_full = s.get("bulk_seals_full");
    arm.seals_end = s.get("bulk_seals_end");
    arm.seals_deadline = s.get("bulk_seals_deadline");
  } else {
    arm.desc_requests = arm.index_requests = arm.target_requests = 0;
    arm.peak_outstanding = s.get("simt_peak_outstanding");
    arm.peak_slot = s.get("simt_peak_slot");
    arm.staging_peak = s.get("simt_staging_max");
    arm.buffer_full_stalls = s.get("simt_slot_full_cycles");
    arm.issue_stalls = s.get("simt_issue_stalls");
  }

  arm.l1_reads = s.get("l1_reads");
  arm.l1_hits = s.get("l1_hits");
  arm.l1_misses = s.get("l1_misses");
  arm.l1_mshr_joins = s.get("l1_mshr_joins");
  arm.l2_reads = s.get("l2_reads");
  arm.l2_hits = s.get("l2_hits");
  arm.l2_misses = s.get("l2_misses");
  arm.dram_reads = s.get("dram_reads");
  arm.dram_bytes = s.get("dram_bytes");
  arm.dram_row_hits = s.get("dram_row_hits");
  arm.dram_row_conflicts = s.get("dram_row_conflicts");
  arm.dram_busy_cycles = s.get("dram_busy_cycles");
  arm.dram_blocked_cycles = s.get("dram_blocked_cycles");
  arm.dram_starved_cycles = s.get("dram_starved_cycles");
  arm.l1_mshr_allocs = s.get("l1_mshr_allocs");
  arm.l1_mshr_max = s.get("l1_mshr_max");
  arm.l2_mshr_joins = s.get("l2_mshr_joins");
  arm.l2_mshr_allocs = s.get("l2_mshr_allocs");
  arm.l2_mshr_max = s.get("l2_mshr_max");
  arm.replays = s.get("replays");
  arm.refuse_parked = s.get("tlb_refuse_parked");
  arm.refuse_full = s.get("tlb_refuse_full");
  // Engine-only timeline keys; absent (0) for arm A, which has no engine.
  arm.first_tgt_issue = s.get("bulk_first_tgt_issue");
  arm.last_tgt_issue = s.get("bulk_last_tgt_issue");
  arm.last_fill = s.get("bulk_last_fill");
  arm.tlb_reads = s.get("tlb0_reads");
  arm.tlb_hits = s.get("tlb0_hits");
  arm.tlb_misses = s.get("tlb0_misses");
  arm.tlb_mshr_stalls = s.get("tlb0_mshr_stalls");
  arm.l2tlb_reads = s.get("l2tlb_reads");
  arm.l2tlb_hits = s.get("l2tlb_hits");
  arm.l2tlb_dedups = s.get("l2tlb_dedups");
  arm.walks = s.get("walks");
  arm.pte_fetches = s.get("pte_fetches");
  arm.l1_stall_cycles = s.get("l1_stall_cycles");
  arm.l2_stall_cycles = s.get("l2_stall_cycles");
  arm.tlb_stall_cycles = s.get("tlb0_mshr_stalls");
  arm.lat_mean_x100 = s.get("lat_mean_x100");
  arm.lat_p50 = s.get("lat_p50");
  arm.lat_p95 = s.get("lat_p95");
  arm.completion_hash = s.get("completion_hash");
  return arm;
}

// Runtime proof that both arms observed the same capacity limits: every peak
// occupancy reported must sit inside the configured capacity.
void check_capacity(const char* arm, const RunResult& r, const Config& cfg,
                    std::vector<std::string>& v) {
  auto over = [&](const char* what, u64 seen, u64 cap) {
    if (cap && seen > cap)
      v.push_back(std::string(arm) + ": " + what + " reached " + num(seen) + " > capacity " +
                  num(cap));
  };
  const Stats& s = r.s;
  over("L1 MSHRs", s.get("l1_mshr_max"), cfg.l1_mshr);
  over("L2 MSHRs", s.get("l2_mshr_max"), cfg.l2_mshr);
  over("L2 TLB MSHRs", s.get("l2tlb_max_occ"), cfg.l2tlb_mshr);
  over("DTLB MSHRs (core 0)", s.get("tlb0_mshr_max"), cfg.dtlb_mshr);
  over("page walkers", s.get("walker_max"), cfg.walkers);
  over("DRAM queue", s.get("dram_max_q"), cfg.dram_q);
  over("residency slots", s.get("slots_total"), cfg.R);
  if (s.get("slots_total") != cfg.R)
    v.push_back(std::string(arm) + ": slots_total " + num(s.get("slots_total")) + " != R " +
                num(cfg.R));
  const u64 mask = s.get("dram_bank_mask");
  for (u32 b = cfg.banks; b < 32; ++b)
    if (mask & (1ull << b))
      v.push_back(std::string(arm) + ": used DRAM bank " + num(b) + " above the configured " +
                  num(cfg.banks));
}

}  // namespace

// ---------------------------------------------------------------------------
std::vector<std::pair<std::string, std::string>> environment_fields(const Config& c) {
  std::vector<std::pair<std::string, std::string>> f;
  auto add = [&](const char* k, u64 v) { f.push_back({k, num(v)}); };
  add("cores", c.cores);
  add("warps_per_core", c.warps_per_core);
  add("lanes", c.lanes);
  add("R", c.R);
  add("l1_lines", c.l1_lines);
  add("l1_ways", c.l1_ways);
  add("l1_latency", c.l1_latency);
  add("l1_mshr", c.l1_mshr);
  add("l2_lines", c.l2_lines);
  add("l2_ways", c.l2_ways);
  add("l2_latency", c.l2_latency);
  add("l2_mshr", c.l2_mshr);
  add("dtlb_entries", c.dtlb_entries);
  add("dtlb_ways", c.dtlb_ways);
  add("dtlb_mshr", c.dtlb_mshr);
  add("dtlb_parked", c.dtlb_parked);
  add("l2tlb_entries", c.l2tlb_entries);
  add("l2tlb_ways", c.l2tlb_ways);
  add("l2tlb_mshr", c.l2tlb_mshr);
  add("l2tlb_req_per_entry", c.l2tlb_req_per_entry);
  add("l2tlb_latency", c.l2tlb_latency);
  add("walkers", c.walkers);
  add("walk_cache", c.walk_cache);
  add("walk_latency_per_level", c.walk_latency_per_level);
  add("levels", c.levels);
  add("banks", c.banks);
  add("rows", c.rows);
  add("row_bytes", c.row_bytes);
  add("t_hit", c.t_hit);
  add("t_miss", c.t_miss);
  add("link", c.link);
  add("dram_q", c.dram_q);
  add("frfcfs", c.frfcfs ? 1 : 0);
  add("bank_hash", c.bank_hash ? 1 : 0);
  add("elem_bytes", c.elem_bytes);
  add("N", c.N);
  add("Lambda", c.Lambda);
  add("P", c.P);
  add("seed", c.seed);
  add("kappa_x1000", static_cast<u64>(c.kappa * 1000.0));
  add("rho_dup_x1000", static_cast<u64>(c.rho_dup * 1000.0));
  add("bulk_index_base", c.bulk_index_base);
  add("bulk_data_base", c.bulk_data_base);
  add("bulk_desc_line", c.bulk_desc_line);
  add("bulk_max_elems", c.bulk_max_elems);
  add("ag_lanes", c.ag_lanes);
  add("eng_issue", c.eng_issue);
  add("idx_window", c.idx_window);
  add("desc_fetch", c.desc_fetch ? 1 : 0);
  add("idx_oracle", c.idx_oracle ? 1 : 0);
  add("ab_charge_latch", c.ab_charge_latch ? 1 : 0);
  // M13: the reuse buffer is part of the environment both arms are measured in
  // when it is on, so a mis-set arm is a violation rather than a silent
  // difference between the two arms' numbers.
  add("reuse_dir", c.reuse_dir ? 1 : 0);
  add("reuse_sets", c.reuse_dir ? c.reuse_sets : 0);
  add("reuse_ways", c.reuse_dir ? c.reuse_ways : 0);
  add("reuse_merge", c.reuse_merge ? 1 : 0);
  add("reuse_charge", c.reuse_charge ? 1 : 0);
  f.push_back({"mem_path", c.mem_path});
  return f;
}

std::string environment_signature(const Config& cfg) {
  std::string s;
  for (const auto& kv : environment_fields(cfg)) {
    s += kv.first;
    s += '=';
    s += kv.second;
    s += ';';
  }
  return s;
}

// ---------------------------------------------------------------------------
AbResult run_ab(const Config& cfg) {
  AbResult r;
  r.cfg = cfg;
  auto fail = [&](const std::string& m) {
    r.ok = false;
    r.violations.push_back(m);
  };

  // ---- 1. the environment must be one the comparison can be drawn from -----
  if (cfg.mem_path == "native")
    fail("mem_path must be l1 or l2 for a controlled A/B run: 'native' gives the two arms "
         "different cache paths");
  if (cfg.lsu_pending != 0)
    fail("lsu_pending must be 0: the SIMT arm would be capped at " + num(cfg.lsu_pending) +
         " outstanding requests instead of R=" + num(cfg.R));
  if (cfg.cores != 1)
    fail("cores must be 1: with S>1 the SIMT arm gets S private dTLBs and S private L1s that "
         "the single engine client cannot use");
  if (cfg.dispatch)
    fail("dispatch must be 0 for the paired A/B driver: the two-arm comparison has no third "
         "arm; use experiment=m9 for the dispatched experiment");
  if (cfg.R == 0) fail("R must be greater than 0");

  // ---- 2. materialize the workload exactly once ---------------------------
  auto trace = std::make_shared<const Trace>(build_trace(cfg));
  std::string err;
  r.workload = build_bulk_workload(cfg, *trace, err);
  if (!err.empty()) fail("workload: " + err);
  if (!r.workload.armed) fail("workload: no descriptor was built (bulk_wl = none)");

  r.signature = environment_signature(cfg);
  {
    u64 h = 1469598103934665603ull;
    for (const auto& a : trace->acc) {
      const u64 vals[5] = {a.addr, a.line, static_cast<u64>(a.elem),
                           static_cast<u64>(a.instr), static_cast<u64>(a.lane)};
      for (u64 v : vals) {
        for (int i = 0; i < 8; ++i) {
          h ^= (v >> (8 * i)) & 0xff;
          h *= 1099511628211ull;
        }
      }
    }
    r.trace_hash = h;
  }

  // ---- 3. arm A: accelerator OFF ------------------------------------------
  {
    Config ca = cfg;
    ca.mode = Mode::Baseline;
    Sim sim(ca, trace);
    if (sim.has_engine()) fail("arm A: an accelerator engine was constructed in OFF mode");
    r.workload.apply(sim.memory());
    const u64 img = sim.memory().image_hash();
    const RunResult ra = sim.run(Mode::Baseline);
    r.a = collect("A: accelerator OFF (SIMT)", ra, false, img, trace->acc.size());
    if (!ra.ok) {
      r.ok = false;
      for (const auto& v : ra.violations) r.violations.push_back("arm A: " + v);
    }
    check_capacity("arm A", ra, cfg, r.violations);
  }

  // ---- 4. arm B: accelerator ON (fresh Sim => fresh memory, caches, TLBs) --
  {
    Config cb = cfg;
    cb.mode = Mode::Bulk;
    Sim sim(cb, trace);
    r.workload.apply(sim.memory());
    const u64 img = sim.memory().image_hash();
    const RunResult rb = sim.run_bulk(r.workload);
    r.b = collect("B: accelerator ON (bulk)", rb, true, img, sim.engine().stats().elems_total);
    if (!sim.has_engine()) fail("arm B: no engine was constructed in ON mode");
    if (!rb.ok) {
      r.ok = false;
      for (const auto& v : rb.violations) r.violations.push_back("arm B: " + v);
    }
    check_capacity("arm B", rb, cfg, r.violations);
    for (const auto& e : sim.engine().errors()) r.violations.push_back("arm B: " + e);
  }

  // ---- 5. the arms must have received the same work -----------------------
  if (r.a.image_hash != r.b.image_hash)
    fail("the arms did not start from the same memory image");
  if (r.a.elements != trace->acc.size())
    fail("arm A processed " + num(r.a.elements) + " elements, trace has " + num(trace->acc.size()));
  if (r.b.elements != trace->acc.size())
    fail("arm B issued " + num(r.b.elements) + " elements, trace has " + num(trace->acc.size()));

  // Accelerator OFF: no accelerator traffic at all.
  if (r.a.desc_requests || r.a.index_requests || r.a.target_requests)
    fail("arm A produced accelerator traffic");

  // Accelerator ON: exactly the traffic the operation requires.
  const u64 ib = (cfg.bulk_wl == "E1") ? 8 : 4;
  const u64 per_line = LINE_BYTES / ib;
  const u64 expect_idx = (r.workload.desc.count + per_line - 1) / per_line;
  if (r.b.desc_requests != 1)
    fail("arm B issued " + num(r.b.desc_requests) + " descriptor requests, expected 1");
  if (r.b.index_requests != expect_idx)
    fail("arm B issued " + num(r.b.index_requests) + " index requests, expected " + num(expect_idx));
  // With a window the target stream is one request per *distinct line*; the
  // element count is recovered by the fanout. Without one it is one per element.
  if (cfg.batch_window) {
    if (r.b.lines_issued + r.b.dedup_merged != r.workload.desc.count)
      fail("arm B: lines issued " + num(r.b.lines_issued) + " + merged " +
           num(r.b.dedup_merged) + " != elements " + num(r.workload.desc.count));
    if (r.b.target_requests != r.b.lines_issued)
      fail("arm B: target requests " + num(r.b.target_requests) +
           " != lines issued " + num(r.b.lines_issued));
  } else if (r.b.target_requests != r.workload.desc.count) {
    fail("arm B formed " + num(r.b.target_requests) + " target requests for " +
         num(r.workload.desc.count) + " elements");
  }
  if (r.b.requests != r.b.desc_requests + r.b.index_requests + r.b.target_requests)
    fail("arm B total requests != descriptor + index + target");

  // ---- 6. equal residency budget -----------------------------------------
  if (r.a.peak_slot > cfg.R)
    fail("arm A peak slot residency " + num(r.a.peak_slot) + " > R " + num(cfg.R));
  if (r.b.peak_slot > cfg.R)
    fail("arm B peak slot residency " + num(r.b.peak_slot) + " > R " + num(cfg.R));
  if (r.a.staging_peak > cfg.lanes)
    fail("arm A coalescer latch " + num(r.a.staging_peak) + " > lanes " + num(cfg.lanes));
  if (r.b.staging_peak != 0)
    fail("arm B reported staging capacity outside the budget");

  // ---- 7. results ---------------------------------------------------------
  r.speedup = r.b.cycles ? double(r.a.cycles) / double(r.b.cycles) : 0.0;
  r.traffic_ratio = r.a.dram_reads ? double(r.b.dram_reads) / double(r.a.dram_reads) : 0.0;
  if (r.violations.empty()) r.ok = true;
  return r;
}

std::vector<AbResult> run_ab_sweep(const Config& base) {
  std::vector<AbResult> out;
  for (u64 R : {8ull, 16ull, 32ull, 64ull, 128ull}) {
    Config c = base;
    c.R = static_cast<u32>(R);
    out.push_back(run_ab(c));
  }
  return out;
}

// ---------------------------------------------------------------------------
void print_ab(const AbResult& r, std::ostream& os) {
  char line[256];
  auto row = [&](const char* what, u64 a, u64 b) {
    std::snprintf(line, sizeof line, "  %-26s %12llu %12llu", what,
                  (unsigned long long)a, (unsigned long long)b);
    os << line;
    if (a) {
      std::snprintf(line, sizeof line, "   %7.2f%%", 100.0 * (double)b / (double)a);
      os << line;
    }
    os << "\n";
  };

  os << "== paired A/B experiment ==\n";
  os << "  environment: " << r.cfg.mem_path << " path, R=" << r.cfg.R
     << ", cores=" << r.cfg.cores << ", W=" << r.cfg.warps_per_core
     << ", L=" << r.cfg.lanes << ", workload=" << r.cfg.wl
     << (r.cfg.bulk_wl == "E1" ? " (8 B indices)" : " (4 B indices)")
     << ", N=" << r.cfg.N << ", P=" << r.cfg.P << ", seed=" << r.cfg.seed << "\n";
  std::snprintf(line, sizeof line, "  %-26s %12s %12s   %8s\n", "metric", "A: OFF", "B: ON", "B/A");
  os << line;
  row("cycles", r.a.cycles, r.b.cycles);
  row("line requests", r.a.requests, r.b.requests);
  row("  descriptor requests", r.a.desc_requests, r.b.desc_requests);
  row("  index requests", r.a.index_requests, r.b.index_requests);
  row("  target requests", r.a.target_requests, r.b.target_requests);
  row("elements", r.a.elements, r.b.elements);
  row("L1 reads", r.a.l1_reads, r.b.l1_reads);
  row("L1 hits", r.a.l1_hits, r.b.l1_hits);
  row("L1 MSHR joins", r.a.l1_mshr_joins, r.b.l1_mshr_joins);
  row("L2 reads", r.a.l2_reads, r.b.l2_reads);
  row("L2 hits", r.a.l2_hits, r.b.l2_hits);
  row("L2 MSHR joins", r.a.l2_mshr_joins, r.b.l2_mshr_joins);
  row("DRAM reads", r.a.dram_reads, r.b.dram_reads);
  row("DRAM bytes", r.a.dram_bytes, r.b.dram_bytes);
  row("DRAM row hits", r.a.dram_row_hits, r.b.dram_row_hits);
  row("DRAM row conflicts", r.a.dram_row_conflicts, r.b.dram_row_conflicts);
  row("DRAM bank-busy cycles", r.a.dram_busy_cycles, r.b.dram_busy_cycles);
  row("TLB reads (core 0)", r.a.tlb_reads, r.b.tlb_reads);
  row("TLB misses", r.a.tlb_misses, r.b.tlb_misses);
  row("L2 TLB reads", r.a.l2tlb_reads, r.b.l2tlb_reads);
  row("page walks", r.a.walks, r.b.walks);
  row("PTE fetches", r.a.pte_fetches, r.b.pte_fetches);
  row("peak outstanding", r.a.peak_outstanding, r.b.peak_outstanding);
  row("peak slot residency", r.a.peak_slot, r.b.peak_slot);
  row("staging latch peak", r.a.staging_peak, r.b.staging_peak);
  row("buffer-full stalls", r.a.buffer_full_stalls, r.b.buffer_full_stalls);
  row("issue stalls", r.a.issue_stalls, r.b.issue_stalls);
  row("L1 stall cycles", r.a.l1_stall_cycles, r.b.l1_stall_cycles);
  row("L2 stall cycles", r.a.l2_stall_cycles, r.b.l2_stall_cycles);
  row("TLB MSHR stalls", r.a.tlb_stall_cycles, r.b.tlb_stall_cycles);
  row("TLB refusals (parked)", r.a.refuse_parked, r.b.refuse_parked);
  row("TLB refusals (full)", r.a.refuse_full, r.b.refuse_full);
  row("mean latency (cy)", r.a.lat_mean_x100 / 100, r.b.lat_mean_x100 / 100);
  row("p50 latency (cy)", r.a.lat_p50, r.b.lat_p50);
  row("p95 latency (cy)", r.a.lat_p95, r.b.lat_p95);

  if (r.cfg.batch_window) {
    os << "  -- M6 window (arm B only; A has no window) --\n";
    row("window entries admitted", r.a.win_entries, r.b.win_entries);
    row("peak window occupancy", r.a.win_occ_max, r.b.win_occ_max);
    row("peak pages resident", r.a.dir_occ_max, r.b.dir_occ_max);
    row("windows sealed", r.a.batches, r.b.batches);
    row("unique lines issued", r.a.lines_issued, r.b.lines_issued);
    row("elements merged (dedup)", r.a.dedup_merged, r.b.dedup_merged);
    row("organizer cycles", r.a.org_cycles, r.b.org_cycles);
    row("fanout cycles", r.a.fanout_cycles, r.b.fanout_cycles);
    row("fanout peak backlog", r.a.fanout_max_backlog, r.b.fanout_max_backlog);
    row("admission stalls", r.a.win_full_stalls, r.b.win_full_stalls);
    std::snprintf(line, sizeof line,
                  "  seals B: full=%llu end=%llu deadline=%llu | window SRAM=%llu B "
                  "(W*8 + D*16 per buffer)\n",
                  (unsigned long long)r.b.seals_full, (unsigned long long)r.b.seals_end,
                  (unsigned long long)r.b.seals_deadline, (unsigned long long)r.b.win_bytes);
    os << line;
  }

  std::snprintf(line, sizeof line, "  speedup cycles(A)/cycles(B) = %.4f\n", r.speedup);
  os << line;
  std::snprintf(line, sizeof line, "  DRAM traffic ratio B/A        = %.4f\n", r.traffic_ratio);
  os << line;
  std::snprintf(line, sizeof line, "  status: %s\n", r.ok ? "OK" : "VIOLATIONS");
  os << line;
  for (const auto& v : r.violations) os << "    ! " << v << "\n";
}

void print_ab_sweep(const std::vector<AbResult>& rs, std::ostream& os) {
  char line[256];
  os << "== residency sweep (paired A/B) ==\n";
  std::snprintf(line, sizeof line,
                "  %5s %10s %10s %8s %8s %10s %10s %8s %8s %6s\n", "R", "A cyc", "B cyc",
                "A dram", "B dram", "A peak", "B peak", "A walks", "B walks", "B/A");
  os << line;
  for (const auto& r : rs) {
    std::snprintf(line, sizeof line,
                  "  %5u %10llu %10llu %8llu %8llu %10llu %10llu %8llu %8llu %6.3f%s\n",
                  r.cfg.R, (unsigned long long)r.a.cycles, (unsigned long long)r.b.cycles,
                  (unsigned long long)r.a.dram_reads, (unsigned long long)r.b.dram_reads,
                  (unsigned long long)r.a.peak_outstanding,
                  (unsigned long long)r.b.peak_outstanding, (unsigned long long)r.a.walks,
                  (unsigned long long)r.b.walks, r.speedup, r.ok ? "" : "  VIOLATIONS");
    os << line;
    for (const auto& v : r.violations) os << "      ! " << v << "\n";
  }
}

// ---------------------------------------------------------------------------
// M6 sweep: W x {dedup, page_order}, holding R and every shared memory-system
// parameter fixed. Each cell is a full paired A/B run, so every row is a genuine
// controlled comparison rather than a re-used baseline.
std::vector<AbResult> run_m6_sweep(const Config& base) {
  std::vector<AbResult> out;
  for (u64 W : {128ull, 512ull, 1024ull, 2048ull, 4096ull, 8192ull, 16384ull}) {
    for (int cell = 0; cell < 4; ++cell) {
      Config c = base;
      c.batch_window = static_cast<u32>(W);
      c.dedup = (cell & 1) != 0;
      c.page_order = (cell & 2) != 0;
      out.push_back(run_ab(c));
    }
  }
  return out;
}

void print_m6_sweep(const std::vector<AbResult>& rs, std::ostream& os) {
  char line[512];
  os << "== M6 sweep: window W x {dedup, page order}, R and shared memory fixed ==\n";
  os << "   workload " << (rs.empty() ? std::string("?") : rs.front().cfg.wl)
     << ", N=" << (rs.empty() ? 0 : rs.front().cfg.N)
     << ", elem_bytes=" << (rs.empty() ? 0 : rs.front().cfg.elem_bytes)
     << ", R=" << (rs.empty() ? 0 : rs.front().cfg.R)
     << (rs.empty() ? "" : (rs.front().cfg.ab_charge_latch ? ", latch charged" : ", latch free"))
     << "\n";
  std::snprintf(line, sizeof line,
                "  %6s %9s %9s %8s %9s %8s %9s %8s %9s %8s\n", "W", "A cyc", "none",
                "x", "dedup", "x", "porder", "x", "both", "x");
  os << line;
  for (size_t i = 0; i + 3 < rs.size(); i += 4) {
    const AbResult& none = rs[i];
    const AbResult& ded = rs[i + 1];
    const AbResult& po = rs[i + 2];
    const AbResult& both = rs[i + 3];
    std::snprintf(line, sizeof line,
                  "  %6u %9llu %9llu %8.3f %9llu %8.3f %9llu %8.3f %9llu %8.3f%s\n",
                  none.cfg.batch_window, (unsigned long long)none.a.cycles,
                  (unsigned long long)none.b.cycles, none.speedup,
                  (unsigned long long)ded.b.cycles, ded.speedup,
                  (unsigned long long)po.b.cycles, po.speedup,
                  (unsigned long long)both.b.cycles, both.speedup,
                  (none.ok && ded.ok && po.ok && both.ok) ? "" : "  VIOLATIONS");
    os << line;
  }
  os << "\n  what the window bought (both mechanisms on): lines, row behaviour, translation,\n"
        "  organization cost, latency and issue pressure.\n";
  std::snprintf(line, sizeof line,
                "  %6s %7s %9s %9s %8s %8s %7s %7s %7s %7s %9s %9s\n", "W", "lines",
                "row hits", "row conf", "dtlb ms", "walks", "org cy", "fan cy", "lat", "p95",
                "issue st", "win B");
  os << line;
  for (size_t i = 3; i < rs.size(); i += 4) {
    const AbArm& b = rs[i].b;
    std::snprintf(line, sizeof line,
                  "  %6u %7llu %9llu %9llu %8llu %8llu %7llu %7llu %7llu %7llu %9llu %9llu\n",
                  rs[i].cfg.batch_window, (unsigned long long)b.lines_issued,
                  (unsigned long long)b.dram_row_hits, (unsigned long long)b.dram_row_conflicts,
                  (unsigned long long)b.tlb_misses, (unsigned long long)b.walks,
                  (unsigned long long)b.org_cycles, (unsigned long long)b.fanout_cycles,
                  (unsigned long long)(b.lat_mean_x100 / 100), (unsigned long long)b.lat_p95,
                  (unsigned long long)b.issue_stalls, (unsigned long long)b.win_bytes);
    os << line;
  }
  os << "\n  arm A for the same workload: "
     << (rs.empty() ? 0ull : (unsigned long long)rs.front().a.cycles) << " cycles, "
     << (rs.empty() ? 0ull : (unsigned long long)rs.front().a.dram_row_hits) << " row hits / "
     << (rs.empty() ? 0ull : (unsigned long long)rs.front().a.dram_reads) << " reads, "
     << (rs.empty() ? 0ull : (unsigned long long)rs.front().a.tlb_misses) << " dtlb misses\n";
  for (const auto& r : rs)
    for (const auto& v : r.violations) os << "    ! W=" << r.cfg.batch_window << ": " << v << "\n";
}

// ---------------------------------------------------------------------------
// M6.1: index-acquisition diagnostic (docs/m6-w0-diagnostic-report.md §4)
// ---------------------------------------------------------------------------
namespace {

inline u64 fnv1a64(u64 h, u64 v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xff;
    h *= 1099511628211ull;
  }
  return h;
}

inline u64 load_le_n(const u8* p, u32 n) {
  u64 v = 0;
  for (u32 i = 0; i < n; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}

// Ground truth from the index values and the memory image alone: the address
// element i must touch, and the value that line holds there. Every arm is
// compared against this, so "same output" is checked rather than assumed.
u64 truth_hash(MemorySystem& mem, const BulkWorkload& w) {
  const BulkDesc& d = w.desc;
  u64 h = 1469598103934665603ull;
  for (u32 i = 0; i < d.count; ++i) {
    const u32 elem = d.elem_base + i;
    // M9: hash the *original* element identity, so a compacted workload is
    // verified against the elements it was built from. On the legacy workload
    // elem_ids is empty and this is elem, i.e. the published behaviour.
    const u32 id = w.elem_ids.empty() ? elem : w.elem_ids[i];
    const u64 addr = d.data_base + w.index_values[i] * d.data_bytes;
    const u8* b = mem.line_bytes(mem.pa_line_of_va(addr));
    h = fnv1a64(h, id);
    h = fnv1a64(h, addr);
    h = fnv1a64(h, load_le_n(b + (addr & (LINE_BYTES - 1)), d.data_bytes));
  }
  return h;
}

// Order-independent digest of a set of target addresses: two arms agree only if
// they touched exactly the same lines, with nothing missing and nothing doubled.
u64 addr_set_hash(std::vector<u64> v) {
  std::sort(v.begin(), v.end());
  u64 h = 1469598103934665603ull;
  for (u64 x : v) h = fnv1a64(h, x);
  return h ^ (static_cast<u64>(v.size()) * 0x9E3779B97F4A7C15ull);
}

// What the engine actually produced, read back element by element and compared
// with the address the index value names. Any missing or wrong element is
// counted, not silently hashed away.
u64 engine_hash(const Sim& sim, const BulkWorkload& w, u64& mismatches) {
  const BulkDesc& d = w.desc;
  u64 h = 1469598103934665603ull;
  mismatches = 0;
  for (u32 i = 0; i < d.count; ++i) {
    const u32 elem = d.elem_base + i;  // the engine's key: compaction position
    const u32 id = w.elem_ids.empty() ? elem : w.elem_ids[i];  // original identity
    const u64 want = d.data_base + w.index_values[i] * d.data_bytes;
    const u64 addr = sim.engine().addr_of(elem);
    const u64 val = sim.engine().value_of(elem);
    if (addr != want) ++mismatches;
    h = fnv1a64(h, id);
    h = fnv1a64(h, addr);
    h = fnv1a64(h, val);
  }
  return h;
}

void fill_mem(DiagRow& row, const Stats& s) {
  row.dram_reads = s.get("dram_reads");
  row.dram_bytes = s.get("dram_bytes");
  row.dram_row_hits = s.get("dram_row_hits");
  row.dram_row_conflicts = s.get("dram_row_conflicts");
  row.dram_busy = s.get("dram_busy_cycles");
  row.dram_starved = s.get("dram_starved_cycles");
  row.dram_blocked = s.get("dram_blocked_cycles");
  row.l1_reads = s.get("l1_reads");
  row.l1_hits = s.get("l1_hits");
  row.l1_misses = s.get("l1_misses");
  row.l1_joins = s.get("l1_mshr_joins");
  row.l1_mshr_max = s.get("l1_mshr_max");
  row.l2_reads = s.get("l2_reads");
  row.l2_hits = s.get("l2_hits");
  row.l2_misses = s.get("l2_misses");
  row.l2_joins = s.get("l2_mshr_joins");
  row.l2_mshr_max = s.get("l2_mshr_max");
  row.tlb_reads = s.get("tlb0_reads");
  row.tlb_hits = s.get("tlb0_hits");
  row.tlb_misses = s.get("tlb0_misses");
  row.tlb_mshr_stalls = s.get("tlb0_mshr_stalls");
  row.tlb_refuse_parked = s.get("tlb_refuse_parked");
  row.tlb_refuse_full = s.get("tlb_refuse_full");
  row.l2tlb_reads = s.get("l2tlb_reads");
  row.walks = s.get("walks");
  row.pte_fetches = s.get("pte_fetches");
  row.replays = s.get("replays");
  row.lat_mean_x100 = s.get("lat_mean_x100");
  row.lat_p50 = s.get("lat_p50");
  row.lat_p95 = s.get("lat_p95");
  row.walker_busy = s.get("walker_busy");
  row.l2tlb_hits = s.get("l2tlb_hits");
  row.l2tlb_misses = s.get("l2tlb_misses");
  row.tlb_mshr_occ = s.get("tlb0_mshr_occ");
  row.l1_mshr_occ = s.get("l1_mshr_occ");
  row.l2_mshr_occ = s.get("l2_mshr_occ");
  row.win_refuse = s.get("bulk_win_refuse");
  row.idx_refuse = s.get("bulk_idx_refuse");
  row.sched_idle = s.get("bulk_sched_idle");
  row.sched_defer = s.get("bulk_sched_defer");
  row.sched_opens = s.get("bulk_sched_opens");
  row.sched_switches = s.get("bulk_sched_switches");
  row.sched_probes = s.get("bulk_sched_probes");
  row.sched_cycles = s.get("bulk_sched_cycles");
  row.sched_bytes = s.get("bulk_sched_bytes");
  row.max_parked = s.get("tlb_max_parked");
  row.idx_wait = s.get("bulk_idx_wait_stall");
  row.idx_tile_swaps = s.get("bulk_idx_tile_swaps");
  row.idx_buf_stall = s.get("bulk_idx_buf_stall");
  row.idx_ooo = s.get("bulk_idx_ooo_gen");
  row.idx_ready_max = s.get("bulk_idx_ready_max");
  row.idx_res_max = s.get("bulk_idx_resident_max");
  row.idx_bytes = s.get("bulk_idx_bytes");
  row.idx_first_issue = s.get("bulk_first_idx_issue");
  row.idx_last_done = s.get("bulk_last_idx_done");
}

// Bank-idle cycle-equivalents of one phase: with two banks, a phase of `cycles`
// cycles offers 2*cycles bank-cycles and `busy` were used.
long long bank_idle(u64 cycles, u64 busy) {
  const long long x = 2 * static_cast<long long>(cycles);
  return x > static_cast<long long>(busy) ? x - static_cast<long long>(busy) : 0;
}

}  // namespace

DiagResult run_index_oracle_diag(const Config& cfg) {
  DiagResult r;
  r.cfg = cfg;
  auto fail = [&](const std::string& m) {
    r.ok = false;
    r.violations.push_back(m);
  };

  auto trace = std::make_shared<const Trace>(build_trace(cfg));
  std::string err;
  BulkWorkload w = build_bulk_workload(cfg, *trace, err);
  if (!err.empty() || !w.armed) {
    fail(err.empty() ? "no workload was built" : err);
    return r;
  }
  {
    u64 h = 1469598103934665603ull;
    for (const auto& a : trace->acc) {
      h = fnv1a64(h, a.addr);
      h = fnv1a64(h, a.line);
      h = fnv1a64(h, a.elem);
    }
    r.trace_hash = h;
  }

  // ---- A: the SIMT reference. Its output *is* the ground truth. -------------
  {
    Config ca = cfg;
    ca.mode = Mode::Baseline;
    ca.idx_oracle = false;
    Sim sim(ca, trace);
    w.apply(sim.memory());
    r.image_hash = sim.memory().image_hash();
    const RunResult ra = sim.run(Mode::Baseline);
    DiagRow& row = r.a;
    row.name = "A: SIMT (accelerator off)";
    row.ok = ra.ok;
    row.cycles = ra.cycles;
    row.total_req = ra.s.get("requests_generated");
    row.target_req = row.total_req;
    row.elems = trace->acc.size();
    row.peak_slots = ra.s.get("simt_peak_slot");
    row.slot_occ_sum = ra.s.get("inflight_occ");
    row.completion_hash = ra.s.get("completion_hash");
    row.expected_hash = truth_hash(sim.memory(), w);
    row.value_hash = row.expected_hash;
    {
      std::vector<u64> addrs;
      addrs.reserve(w.desc.count);
      for (u32 i = 0; i < w.desc.count; ++i)
        addrs.push_back(w.desc.data_base + w.index_values[i] * w.desc.data_bytes);
      row.addr_set_hash = addr_set_hash(std::move(addrs));
    }
    fill_mem(row, ra.s);
    if (!ra.ok) {
      r.ok = false;
      for (const auto& v : ra.violations) r.violations.push_back("A: " + v);
    }
  }

  // ---- B: four engine variants on the same workload -------------------------
  struct Variant { const char* name; bool oracle; bool desc; };
  const Variant variants[4] = {
      {"B0 windowed (as measured)", false, true},
      {"B1 no descriptor fetch", false, false},
      {"B2 index oracle", true, true},
      {"B3 oracle + no descriptor", true, false},
  };
  for (const auto& v : variants) {
    Config cb = cfg;
    cb.mode = Mode::Bulk;
    cb.idx_oracle = v.oracle;
    cb.desc_fetch = v.desc;
    Sim sim(cb, trace);
    w.apply(sim.memory());
    const u64 img = sim.memory().image_hash();
    const RunResult rb = sim.run_bulk(w);
    DiagRow row;
    row.name = v.name;
    row.ok = rb.ok;
    row.cycles = rb.cycles;
    const BulkStats& b = sim.engine().stats();
    // `desc_submitted` counts the submission, not a line request: with
    // desc_fetch = 0 no line is ever read for it, so the request volume is 0.
    row.desc_req = v.desc ? b.desc_submitted : 0;
    row.index_req = b.idx_lines_req;
    row.target_req = b.tgt_req_gen;
    row.total_req = row.desc_req + row.index_req + row.target_req;
    row.elems = b.elems_total;
    row.peak_slots = b.buf_max;
    row.slot_occ_sum = b.buf_occ;
    row.first_tgt_issue = b.first_tgt_issue == NONE ? 0 : b.first_tgt_issue;
    row.last_tgt_issue = b.last_tgt_issue == NONE ? 0 : b.last_tgt_issue;
    row.last_fill = b.last_fill == NONE ? 0 : b.last_fill;
    row.last_idx_issue = b.last_idx_issue == NONE ? 0 : b.last_idx_issue;
    row.last_idx_done = b.last_idx_done == NONE ? 0 : b.last_idx_done;
    row.busy_at_first_tgt = b.busy_at_first_tgt;
    row.busy_at_last_tgt = b.busy_at_last_tgt;
    row.idx_wait_stall = b.idx_wait_stall;
    row.issue_stall = b.issue_stall;
    row.slot_stall = b.slot_stall;
    row.buf_full_stall = b.buf_full_stall;
    row.org_cycles = b.org_cycles;
    row.fanout_cycles = b.fanout_cycles;
    row.win_entries = b.win_entries;
    row.oracle_elems = b.oracle_elems;
    row.addr_mismatch = b.addr_mismatch;
    row.completion_hash = rb.s.get("completion_hash");
    fill_mem(row, rb.s);
    row.expected_hash = r.a.expected_hash;
    row.value_hash = engine_hash(sim, w, row.addr_mismatch);
    {
      std::vector<u64> addrs;
      addrs.reserve(w.desc.count);
      for (u32 i = 0; i < w.desc.count; ++i)
        addrs.push_back(sim.engine().addr_of(w.desc.elem_base + i));
      row.addr_set_hash = addr_set_hash(std::move(addrs));
      if (row.addr_set_hash != r.a.addr_set_hash)
        fail(std::string(v.name) + ": target-address multiset differs from SIMT");
    }
    if (img != r.image_hash) fail(std::string(v.name) + ": different memory image");
    if (!rb.ok) {
      r.ok = false;
      for (const auto& x : rb.violations) r.violations.push_back(std::string(v.name) + ": " + x);
    }
    if (b.elems_total != w.desc.count)
      fail(std::string(v.name) + ": elements " + num(b.elems_total) + " != " + num(w.desc.count));
    if (row.value_hash != r.a.expected_hash)
      fail(std::string(v.name) + ": output values differ from the SIMT reference");
    if (row.addr_mismatch != 0)
      fail(std::string(v.name) + ": " + num(row.addr_mismatch) +
           " elements resolved to an address the index array does not name");
    if (row.peak_slots > cfg.R)
      fail(std::string(v.name) + ": peak residency " + num(row.peak_slots) + " > R");
    r.b.push_back(row);
  }

  // ---- invariants specific to this diagnostic -------------------------------
  const DiagRow& b0 = r.b[0];
  const DiagRow& b1 = r.b[1];
  const DiagRow& b2 = r.b[2];
  const DiagRow& b3 = r.b[3];
  const u64 per_line = LINE_BYTES / w.desc.index_bytes;
  const u64 expect_idx = (w.desc.count + per_line - 1) / per_line;
  if (b0.index_req != expect_idx) fail("B0 index requests != ceil(count / per line)");
  if (b2.index_req != 0) fail("B2 fetched index lines despite the oracle");
  if (b2.oracle_elems != w.desc.count) fail("B2 did not take every index from the oracle");
  if (b0.desc_req != 1 || b2.desc_req != 1)
    fail("B0/B2 must fetch exactly one descriptor line");
  if (b1.desc_req != 0 || b3.desc_req != 0)
    fail("B1/B3 are defined as not fetching the descriptor at all");

  // ---- cross-arm identities: compare the arm-controlled component -----------
  // The oracle removes exactly the index array, and `desc_fetch = 0` removes
  // exactly the descriptor line. Both are statements about the *non-target*
  // request volume of an arm, and that is the form which is always true. The
  // number of target requests is a function of when a window was composed, so
  // it legitimately differs between arms whenever duplicate target lines make
  // deduplication timing-sensitive (m8-diagnostic-capacity-audit.md §2.3).
  // The original total-based forms therefore mixed a per-arm volume invariant
  // with an unstated cross-arm assumption about target counts. Both are checked
  // below on their non-target component unconditionally, and the originals are
  // kept and still run in full whenever target counts happen to agree.
  const u64 b0_side = b0.total_req - b0.target_req;
  const u64 b1_side = b1.total_req - b1.target_req;
  const u64 b2_side = b2.total_req - b2.target_req;
  if (b0_side != b2_side + expect_idx)
    fail("the index array is not exactly " + num(expect_idx) + " non-target requests of B0");
  if (b0_side != b1_side + 1)
    fail("the descriptor is not exactly one non-target request of B0");
  {
    const u64 gap = b0.target_req > b2.target_req ? b0.target_req - b2.target_req
                                                  : b2.target_req - b0.target_req;
    if (gap == 0) {
      if (b0.total_req != b2.total_req + expect_idx)
        fail("the index array is not exactly " + num(expect_idx) + " requests of B0");
    } else {
      r.notes.push_back("B0 vs B2: original total-request identity not applicable - target "
                        "requests differ by " + num(gap) +
                        " (deduplication is timing-sensitive); non-target volume checked");
    }
  }
  {
    const u64 gap = b0.target_req > b1.target_req ? b0.target_req - b1.target_req
                                                  : b1.target_req - b0.target_req;
    if (gap == 0) {
      if (b0.total_req != b1.total_req + 1)
        fail("the descriptor is not exactly one request of B0");
    } else {
      r.notes.push_back("B0 vs B1: original descriptor identity not applicable - target "
                        "requests differ by " + num(gap) +
                        "; non-target volume checked");
    }
  }

  // ---- per-arm accounting: DRAM traffic ------------------------------------
  // DRAM reads are the lines the L2 really missed on. A request reaches
  // `l2_probe` only when it allocates at the L1: an L1 hit or an L1 MSHR join is
  // served above the L2 and never probes it (memory.cpp, l1_probe). The
  // original identity omitted both terms, so it is exact only while nothing is
  // absorbed at the L1 - true for W0/W1 at their published sizes, false for any
  // distribution with reuse (HOT, DUP, PGLO, STRID). The L1-aware identity is
  // checked for every row; the original is kept verbatim and still runs in full
  // whenever its assumption holds.
  {
    std::vector<const DiagRow*> all{&r.a};
    for (const auto& row : r.b) all.push_back(&row);
    for (const DiagRow* d : all) {
      const u64 absorbed = d->l1_hits + d->l1_joins;
      const long long want = static_cast<long long>(d->total_req) -
                             static_cast<long long>(absorbed) +
                             static_cast<long long>(d->pte_fetches) -
                             static_cast<long long>(d->l2_hits) -
                             static_cast<long long>(d->l2_joins);
      if (static_cast<long long>(d->dram_reads) != want)
        fail(d->name +
             ": DRAM reads != requests - L1 hits - L1 joins + PTE fetches - L2 hits - L2 joins");
      if (absorbed == 0) {
        const long long orig = static_cast<long long>(d->total_req) +
                               static_cast<long long>(d->pte_fetches) -
                               static_cast<long long>(d->l2_hits) -
                               static_cast<long long>(d->l2_joins);
        if (static_cast<long long>(d->dram_reads) != orig)
          fail(d->name + ": DRAM reads != requests + PTE fetches - L2 hits - L2 joins");
      } else {
        r.notes.push_back(d->name + ": original DRAM identity not applicable - " + num(absorbed) +
                          " requests were absorbed by the L1 (hits + MSHR joins); "
                          "L1-aware identity checked");
      }
    }
  }
  // The completion hash is order-dependent (it is folded as lines retire), so it
  // is reported but not compared: the oracle changes when a window drains, not
  // what it produces. Output identity is checked through `value_hash` instead.

  // ---- phase attribution of the measured run -------------------------------
  r.prefix_cycles = b0.first_tgt_issue + 1;                  // cycles 0..first issue
  r.span_cycles = b0.last_tgt_issue - b0.first_tgt_issue;    // issue span
  r.drain_cycles = b0.cycles - (b0.last_tgt_issue + 1);      // after the last issue
  r.bank_idle_prefix = static_cast<u64>(
      bank_idle(r.prefix_cycles, b0.busy_at_first_tgt));
  r.bank_idle_span = static_cast<u64>(
      bank_idle(r.span_cycles, b0.busy_at_last_tgt - b0.busy_at_first_tgt));
  r.bank_idle_drain =
      static_cast<u64>(bank_idle(r.drain_cycles, b0.dram_busy - b0.busy_at_last_tgt));

  r.ok = r.violations.empty();
  return r;
}

void print_index_oracle_diag(const DiagResult& r, std::ostream& os) {
  char line[512];
  os << "== index-acquisition diagnostic ==\n";
  os << "  workload " << r.cfg.wl << " (kappa=" << r.cfg.kappa << ", P=" << r.cfg.P
     << ", N=" << r.cfg.N << ", " << r.cfg.elem_bytes << " B/element), R=" << r.cfg.R
     << ", W=" << r.cfg.batch_window << ", mem_path=" << r.cfg.mem_path
     << (r.cfg.ab_charge_latch ? ", latch charged" : ", latch free") << "\n";
  os << "  B2/B3 are an ORACLE: index values arrive from a side buffer. No target request,\n"
        "  target latency, residency rule or memory-system parameter is changed.\n\n";

  std::snprintf(line, sizeof line,
                "  %-28s %8s %6s %6s %6s %6s %6s %6s %6s %7s %6s\n", "row", "cycles",
                "reqs", "desc", "idx", "tgt", "dram", "rowhit", "rconf", "bank%", "avgR");
  os << line;
  auto row1 = [&](const DiagRow& d) {
    const double util = d.cycles ? 100.0 * double(d.dram_busy) / (2.0 * double(d.cycles)) : 0.0;
    const double avg = d.cycles ? double(d.slot_occ_sum) / double(d.cycles) : 0.0;
    std::snprintf(line, sizeof line,
                  "  %-28s %8llu %6llu %6llu %6llu %6llu %6llu %6llu %6llu %7.2f %6.1f\n",
                  d.name.c_str(), (unsigned long long)d.cycles,
                  (unsigned long long)d.total_req, (unsigned long long)d.desc_req,
                  (unsigned long long)d.index_req, (unsigned long long)d.target_req,
                  (unsigned long long)d.dram_reads, (unsigned long long)d.dram_row_hits,
                  (unsigned long long)d.dram_row_conflicts, util, avg);
    os << line;
  };
  row1(r.a);
  for (const auto& d : r.b) row1(d);

  os << "\n";
  std::snprintf(line, sizeof line,
                "  %-28s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", "row",
                "peakR", "l1rd", "l1hit", "l1join", "l2rd", "l2hit", "l2join", "dtlbm",
                "walks", "pte", "refP", "refF", "hashOK");
  os << line;
  auto row2 = [&](const DiagRow& d) {
    std::snprintf(line, sizeof line,
                  "  %-28s %6llu %6llu %6llu %6llu %6llu %6llu %6llu %6llu %6llu %6llu %6llu %6llu %6s\n",
                  d.name.c_str(), (unsigned long long)d.peak_slots,
                  (unsigned long long)d.l1_reads, (unsigned long long)d.l1_hits,
                  (unsigned long long)d.l1_joins, (unsigned long long)d.l2_reads,
                  (unsigned long long)d.l2_hits, (unsigned long long)d.l2_joins,
                  (unsigned long long)d.tlb_misses, (unsigned long long)d.walks,
                  (unsigned long long)d.pte_fetches, (unsigned long long)d.tlb_refuse_parked,
                  (unsigned long long)d.tlb_refuse_full,
                  d.value_hash == r.a.expected_hash ? "yes" : "NO");
    os << line;
  };
  row2(r.a);
  for (const auto& d : r.b) row2(d);

  os << "\n  pipeline: latency, average structure occupancy, and the cycles an issue port\n"
        "  spent blocked on a translation it could not submit into\n";
  std::snprintf(line, sizeof line,
                "  %-28s %6s %6s %6s %6s %6s %6s %6s %6s %9s %9s %9s %6s\n", "row", "lat",
                "p50", "p95", "dtlbM", "l1M", "walk", "l2tlH", "l2tlM", "tgtBlock",
                "idxBlock", "slotStall", "blk%");
  os << line;
  auto row3 = [&](const DiagRow& d) {
    auto avg = [&](u64 sum) { return d.cycles ? double(sum) / double(d.cycles) : 0.0; };
    std::snprintf(line, sizeof line,
                  "  %-28s %6llu %6llu %6llu %6.2f %6.2f %6llu %6llu %6llu %9llu %9llu %9llu %5.1f%%\n",
                  d.name.c_str(), (unsigned long long)(d.lat_mean_x100 / 100),
                  (unsigned long long)d.lat_p50, (unsigned long long)d.lat_p95,
                  avg(d.tlb_mshr_occ), avg(d.l1_mshr_occ),
                  (unsigned long long)d.walker_busy, (unsigned long long)d.l2tlb_hits,
                  (unsigned long long)d.l2tlb_misses, (unsigned long long)d.win_refuse,
                  (unsigned long long)d.idx_refuse, (unsigned long long)d.slot_stall,
                  d.cycles ? 100.0 * double(d.win_refuse) / double(d.cycles) : 0.0);
    os << line;
  };
  row3(r.a);
  for (const auto& d : r.b) row3(d);

  os << "\n  engine-side detail (B rows): stalls, organization cost and timeline\n";
  std::snprintf(line, sizeof line,
                "  %-28s %9s %9s %9s %9s %8s %8s %9s %9s %9s %9s %9s\n", "row", "idxwait",
                "tlbref", "slotstall", "bufstall", "orgcy", "fancy", "firstTgt", "lastTgt",
                "lastFill", "lastIdxIs", "lastIdxDn");
  os << line;
  for (const auto& d : r.b) {
    std::snprintf(line, sizeof line,
                  "  %-28s %9llu %9llu %9llu %9llu %8llu %8llu %9llu %9llu %9llu %9llu %9llu\n",
                  d.name.c_str(), (unsigned long long)d.idx_wait_stall,
                  (unsigned long long)d.issue_stall, (unsigned long long)d.slot_stall,
                  (unsigned long long)d.buf_full_stall, (unsigned long long)d.org_cycles,
                  (unsigned long long)d.fanout_cycles, (unsigned long long)d.first_tgt_issue,
                  (unsigned long long)d.last_tgt_issue, (unsigned long long)d.last_fill,
                  (unsigned long long)d.last_idx_issue, (unsigned long long)d.last_idx_done);
    os << line;
  }

  os << "\n  where the engine's cycles went: startup (submission -> first target issue),\n"
        "  issue span, drain. `idle` is bank-idle cycle-equivalents (2 banks).\n";
  std::snprintf(line, sizeof line,
                "  %-28s %8s %8s %8s %8s %8s %8s %8s\n", "row", "startup", "s.idle",
                "span", "p.idle", "drain", "d.idle", "bank%");
  os << line;
  for (const auto& d : r.b) {
    const u64 pre = d.first_tgt_issue + 1;
    const u64 span = d.last_tgt_issue > d.first_tgt_issue ? d.last_tgt_issue - d.first_tgt_issue : 0;
    const u64 drain = d.cycles > d.last_tgt_issue + 1 ? d.cycles - (d.last_tgt_issue + 1) : 0;
    const u64 ip = static_cast<u64>(bank_idle(pre, d.busy_at_first_tgt));
    const u64 is = static_cast<u64>(bank_idle(
        span, d.busy_at_last_tgt > d.busy_at_first_tgt
                  ? d.busy_at_last_tgt - d.busy_at_first_tgt
                  : 0));
    const u64 id = static_cast<u64>(bank_idle(
        drain, d.dram_busy > d.busy_at_last_tgt ? d.dram_busy - d.busy_at_last_tgt : 0));
    const double util = d.cycles ? 100.0 * double(d.dram_busy) / (2.0 * double(d.cycles)) : 0.0;
    std::snprintf(line, sizeof line,
                  "  %-28s %8llu %8llu %8llu %8llu %8llu %8llu %8.2f\n", d.name.c_str(),
                  (unsigned long long)pre, (unsigned long long)ip, (unsigned long long)span,
                  (unsigned long long)is, (unsigned long long)drain, (unsigned long long)id,
                  util);
    os << line;
  }
  std::snprintf(line, sizeof line,
                "  %-28s %8llu %8s %8llu %8s %8llu %8s %8.2f\n", "A: SIMT (no engine)",
                (unsigned long long)r.a.cycles, "-", (unsigned long long)0, "-",
                (unsigned long long)0, "-",
                r.a.cycles ? 100.0 * double(r.a.dram_busy) / (2.0 * double(r.a.cycles)) : 0.0);
  os << line;

  os << "\n  scheduler (M7): `idle` is cycles lost with work pending and a free slot, `defer`\n"
        "  is refusal *events* (not cycles), `opens` page runs claimed, `switch` times the\n"
        "  port moved pages, `maxPark` the largest per-page waiting set reached.\n";
  std::snprintf(line, sizeof line,
                "  %-28s %9s %6s %7s %6s %6s %7s %6s %7s\n", "row", "schedIdle",
                "idle%", "defer", "opens", "switch", "probes", "maxPark", "metaB");
  os << line;
  for (const auto& d : r.b) {
    const double ip = d.cycles ? 100.0 * double(d.sched_idle) / double(d.cycles) : 0.0;
    std::snprintf(line, sizeof line,
                  "  %-28s %9llu %5.1f%% %7llu %6llu %6llu %7llu %6llu %7llu\n",
                  d.name.c_str(), (unsigned long long)d.sched_idle, ip,
                  (unsigned long long)d.sched_defer, (unsigned long long)d.sched_opens,
                  (unsigned long long)d.sched_switches, (unsigned long long)d.sched_probes,
                  (unsigned long long)d.max_parked, (unsigned long long)d.sched_bytes);
    os << line;
  }

  os << "\n  index acquisition (M8): `wait` = cycles the address engine had no consumable\n"
        "  index line, `bufStall` = cycles a line was ready but no residency slot,\n"
        "  `swaps` = buffer handovers, `ooo` = elements generated out of order,\n"
        "  `ready` = peak ready lines over both tiles, `resMax` = peak index lines in flight.\n";
  std::snprintf(line, sizeof line,
                "  %-28s %8s %6s %8s %6s %6s %6s %6s %7s\n", "row", "wait", "wait%",
                "bufStall", "swaps", "ooo", "ready", "resMax", "metaB");
  os << line;
  for (const auto& d : r.b) {
    const double wp = d.cycles ? 100.0 * double(d.idx_wait) / double(d.cycles) : 0.0;
    std::snprintf(line, sizeof line,
                  "  %-28s %8llu %5.1f%% %8llu %6llu %6llu %6llu %6llu %7llu\n",
                  d.name.c_str(), (unsigned long long)d.idx_wait, wp,
                  (unsigned long long)d.idx_buf_stall, (unsigned long long)d.idx_tile_swaps,
                  (unsigned long long)d.idx_ooo, (unsigned long long)d.idx_ready_max,
                  (unsigned long long)d.idx_res_max, (unsigned long long)d.idx_bytes);
    os << line;
  }

  os << "\n  deltas against B0 (measured):\n";
  for (size_t i = 1; i < r.b.size(); ++i) {
    const DiagRow& d = r.b[i];
    const long long dcyc = static_cast<long long>(r.b[0].cycles) -
                           static_cast<long long>(d.cycles);
    const long long ddram = static_cast<long long>(r.b[0].dram_reads) -
                            static_cast<long long>(d.dram_reads);
    std::snprintf(line, sizeof line,
                  "  %-28s %+8lld cycles, %+6lld DRAM reads, first tgt %llu -> %llu, "
                  "idxwait %llu -> %llu\n",
                  d.name.c_str(), dcyc, ddram,
                  (unsigned long long)r.b[0].first_tgt_issue,
                  (unsigned long long)d.first_tgt_issue,
                  (unsigned long long)r.b[0].idx_wait_stall,
                  (unsigned long long)d.idx_wait_stall);
    os << line;
  }

  std::snprintf(line, sizeof line,
                "\n  image hash %llx, trace hash %llx, target-address set %llx (identical in every row)\n",
                (unsigned long long)r.image_hash, (unsigned long long)r.trace_hash,
                (unsigned long long)r.a.addr_set_hash);
  os << line;
  std::snprintf(line, sizeof line, "  status: %s\n", r.ok ? "OK" : "VIOLATIONS");
  os << line;
  for (const auto& v : r.violations) os << "    ! " << v << "\n";
  for (const auto& n : r.notes) os << "    ~ " << n << "\n";
}

// ---------------------------------------------------------------------------
// M9: three-arm dispatch experiment (docs/m9-dispatch-design.md §7)
// ---------------------------------------------------------------------------
namespace {
// Compare two arms key for key. A difference is a gate failure with the offending
// counter named, never a warning.
void m9_same(M9Result& r, const char* gate, const char* what, const RunResult& x,
             const RunResult& y, std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    const u64 a = x.s.get(k), b = y.s.get(k);
    if (a != b) {
      r.ok = false;
      r.violations.push_back(std::string(gate) + ": " + what + " " + k + " " + num(a) +
                             " != " + num(b));
    }
  }
}
}  // namespace

M9Result run_m9(const Config& cfg0, CycleTrace* ta, CycleTrace* tb, CycleTrace* tc) {
  M9Result r;
  r.cfg = cfg0;
  auto fail = [&](const std::string& m) {
    r.ok = false;
    r.violations.push_back(m);
  };

  if (cfg0.mem_path == "native")
    fail("mem_path must be l1 or l2: the three arms have to share one cache path");
  if (cfg0.cores != 1) fail("cores must be 1 for a dispatched run");

  // ---- one workload, one environment -------------------------------------
  auto trace = std::make_shared<const Trace>(build_trace(cfg0));
  {
    u64 h = 1469598103934665603ull;
    for (const auto& a : trace->acc) {
      h = fnv1a64(h, a.addr);
      h = fnv1a64(h, a.line);
      h = fnv1a64(h, a.elem);
      h = fnv1a64(h, a.instr);
      h = fnv1a64(h, a.lane);
    }
    r.trace_hash = h;
  }

  // Metadata first, then the two workloads it implies: the compacted one for C,
  // and the whole-trace one that A and B share so their images are identical.
  const ClassMap cm = build_op_classes(cfg0, *trace);
  std::string err, err2;
  r.workload = build_bulk_workload(cfg0, *trace, err, &cm);
  // M18: arm A needs no descriptor of its own (the reference gather is only
  // built for arm B). When B is skipped the reference is never built, so a
  // whole-trace descriptor that would not fit is not an error: the arm it
  // belongs to does not run.
  if (!cfg0.skip_b) {
    r.reference = build_bulk_workload(cfg0, *trace, err2, nullptr);
    if (!err2.empty()) fail("reference workload: " + err2);
  }
  if (!err.empty()) fail("hybrid workload: " + err);

  for (u32 i = 0; i < trace->n_instr; ++i) {
    if (cm.at(i) == static_cast<u8>(OpClass::Bulk)) ++r.instr_bulk;
    else ++r.instr_simt;
  }
  r.elems_bulk = r.workload.armed ? r.workload.desc.count : 0;
  r.elems_simt = trace->acc.size() - r.elems_bulk;

  // ---- A: SIMT only -------------------------------------------------------
  {
    Config c = cfg0;
    c.mode = Mode::Baseline;
    c.dispatch = 0;
    Sim sim(c, trace);
    r.reference.apply(sim.memory());
    r.image_hash = sim.memory().image_hash();
    if (ta) sim.observe(ta);
    r.a = sim.run(Mode::Baseline);
    if (!r.a.ok) {
      fail("arm A: conservation violated");
      for (const auto& v : r.a.violations) r.violations.push_back("A: " + v);
    }
  }

  // ---- B: bulk only, the M8 path over the whole trace --------------------
  if (!cfg0.skip_b) {
    Config c = cfg0;
    c.mode = Mode::Bulk;
    c.dispatch = 0;
    Sim sim(c, trace);
    r.reference.apply(sim.memory());
    if (sim.memory().image_hash() != r.image_hash) fail("arm B: different memory image");
    if (tb) sim.observe(tb);
    r.b = sim.run_bulk(r.reference);
    if (sim.has_engine()) {
      r.b_value_hash = engine_hash(sim, r.reference, r.b_mismatch);
      r.b_expected_hash = truth_hash(sim.memory(), r.reference);
    }
    if (!r.b.ok) {
      fail("arm B: conservation violated");
      for (const auto& v : r.b.violations) r.violations.push_back("B: " + v);
    }
  }

  // ---- C: the dispatched run ---------------------------------------------
  {
    Config c = cfg0;
    c.dispatch = 1;
    Sim sim(c, trace);
    if (tc) sim.observe(tc);
    r.c = sim.run_hybrid();
    if (sim.has_engine()) {
      r.c_value_hash = engine_hash(sim, r.workload, r.c_mismatch);
      r.c_expected_hash = truth_hash(sim.memory(), r.workload);
    }
    if (r.all_bulk() && sim.memory().image_hash() != r.image_hash)
      fail("arm C: an all-bulk class map must produce arm B's image");
    if (!r.c.ok) {
      fail("arm C: conservation violated");
      for (const auto& v : r.c.violations) r.violations.push_back("C: " + v);
    }
  }

  // ---- gates --------------------------------------------------------------
  if (r.all_simt()) {
    // G1: the dispatched run is the SIMT baseline, to the cycle and to the hash.
    m9_same(r, "G1", "C vs A", r.a, r.c,
            {"cycles", "requests_generated", "slot_allocs", "completed", "completion_hash",
             "l1_reads", "l1_hits", "l1_misses", "l1_mshr_joins", "l1_mshr_allocs",
             "l2_reads", "l2_hits", "l2_misses", "l2_mshr_joins", "l2_mshr_allocs",
             "dram_reads", "dram_bytes", "dram_row_hits", "dram_row_conflicts",
             "dram_busy_cycles", "tlb0_reads", "tlb0_hits", "tlb0_misses", "l2tlb_reads",
             "l2tlb_hits", "walks", "pte_fetches", "replays", "inflight_max",
             "simt_peak_slot", "simt_staging_max"});
    if (r.c.s.get("bulk_desc_submitted") || r.c.s.get("bulk_idx_lines_req") ||
        r.c.s.get("bulk_tgt_req_gen") || r.c.s.get("bulk_workload_armed"))
      fail("G1: bulk acquisition on an all-SIMT workload");
    r.notes.push_back("G1: C writes no index array, so its image hashes differently from A's; "
                      "the comparable state is the hierarchy, which the gate checks in full");
  } else if (r.all_bulk()) {
    // G2: the dispatched bulk side is the M8 machine.
    m9_same(r, "G2", "C vs B", r.b, r.c,
            {"cycles", "requests_generated", "issued", "completed", "completion_hash",
             "bulk_desc_submitted", "bulk_desc_completed", "bulk_desc_cycles_sum",
             "bulk_desc_fetch_cycles_mean", "bulk_idx_lines_req", "bulk_idx_lines_done",
             "bulk_idx_lines_ooo", "bulk_idx_out_max", "bulk_idx_elems_req",
             "bulk_idx_elems_done", "bulk_idx_first_done", "bulk_tgt_addr_gen",
             "bulk_tgt_req_gen", "bulk_tgt_req_issued", "bulk_tgt_req_done", "bulk_tgt_ooo",
             "bulk_tgt_first_gen", "bulk_elems_total", "bulk_elems_done", "bulk_addr_faults",
             "bulk_addr_mismatch", "bulk_buf_occ", "bulk_buf_max", "bulk_unissued_max",
             "bulk_buf_full_stall", "bulk_idx_wait_stall", "bulk_issue_stall",
             "bulk_slot_stall", "bulk_win_refuse", "bulk_idx_refuse", "bulk_sched_opens",
             "bulk_sched_switches", "bulk_sched_defer", "bulk_sched_idle",
             "bulk_sched_probes", "bulk_sched_cycles", "bulk_sched_scan",
             "bulk_idx_tile_swaps", "bulk_idx_buf_stall", "bulk_idx_ooo_gen",
             "bulk_idx_ready_max", "bulk_ag_cycles", "bulk_win_entries", "bulk_win_occ_max",
             "bulk_win_total_max", "bulk_dir_occ_max", "bulk_dir_collisions",
             "bulk_batches", "bulk_seals_full", "bulk_seals_end", "bulk_seals_deadline",
             "bulk_lines_issued", "bulk_dedup_merged", "bulk_org_cycles",
             "bulk_fanout_cycles", "bulk_fanout_elems", "bulk_fanout_max_backlog",
             "bulk_win_full_stall", "bulk_first_desc_done", "bulk_first_idx_issue",
             "bulk_last_idx_done", "bulk_first_admit", "bulk_first_seal", "bulk_last_seal",
             "bulk_first_tgt_issue", "bulk_last_tgt_issue", "bulk_last_fill",
             "bulk_residency_violations", "dram_reads", "dram_bytes", "dram_row_hits",
             "dram_row_conflicts", "l1_reads", "l1_hits", "l1_misses", "l1_mshr_joins",
             "l2_reads", "l2_hits", "l2_mshr_joins", "walks", "pte_fetches",
             "inflight_max", "replays", "lat_mean_x100", "lat_p50", "lat_p95"});
    if (r.c_mismatch) fail("G2: C's addresses disagree with the index image");
    if (r.c_value_hash != r.b_value_hash)
      fail("G2: C's (element, address, value) hash differs from B's");
    if (r.c_expected_hash != r.b_expected_hash) fail("G2: expectation differs between arms");
    r.notes.push_back("G2: no M4-M8 report recorded a bulk completion_hash, so C is compared "
                      "with B re-run in this build, not with a historical anchor");
  } else {
    // G3: mixed classes.
    if (r.elems_simt + r.elems_bulk != trace->acc.size())
      fail("G3: the element partition is not exhaustive");
    if (r.c.s.get("bulk_desc_submitted") != 1) fail("G3: expected exactly one descriptor");
    if (r.c.s.get("bulk_elems_done") + r.c.s.get("bulk_addr_faults") != r.elems_bulk)
      fail("G3: bulk elements were not all completed exactly once");
    const u64 per = LINE_BYTES / r.workload.desc.index_bytes;
    if (r.c.s.get("bulk_idx_lines_req") != (r.elems_bulk + per - 1) / per)
      fail("G3: index lines do not match the bulk element count");
    if (r.c.s.get("simt_completed") != r.c.s.get("simt_requests_generated"))
      fail("G3: SIMT requests were lost");
    if (r.c.s.get("peak_total_outstanding") > r.c.s.get("slot_pool_total"))
      fail("G3: slot residency exceeded the shared pool");
    if (r.c_mismatch) fail("G3: C's addresses disagree with the index image");
    if (r.c_value_hash != r.c_expected_hash)
      fail("G3: C's output does not match the image-derived expectation");
  }

  if (r.violations.empty()) r.ok = true;
  return r;
}

// ---------------------------------------------------------------------------
// M11.2: DRAM scheduler diagnostic (experiment=m9diag + --set dram_diag=1)
// ---------------------------------------------------------------------------
// Every number here is a counter written *after* the arbitration decision. None
// of them is read by the scheduler, so turning the flag on cannot reorder, delay
// or duplicate a single command. The block is printed only when the flag is set,
// which keeps every previously published log byte-identical.
void print_dram_diag(const M9Result& r, std::ostream& os) {
  char line[512];
  const RunResult* arm[2] = {&r.b, &r.c};
  auto get = [&](int i, const char* k) { return arm[i]->s.get(k); };
  auto ratio = [&](int i, const char* k) {
    const u64 d = get(i, "dram_diag_decisions");
    return d ? double(get(i, k)) / double(d) : 0.0;
  };
  auto per_read = [&](int i, const char* k) {
    const u64 d = get(i, "dram_reads");
    return d ? double(get(i, k)) / double(d) : 0.0;
  };
  auto f2 = [&](const char* label, double b, double c) {
    std::snprintf(line, sizeof line, "  %-44s %9.2f %9.2f\n", label, b, c);
    os << line;
  };
  auto u2 = [&](const char* label, u64 b, u64 c) {
    std::snprintf(line, sizeof line, "  %-44s %9llu %9llu\n", label,
                  (unsigned long long)b, (unsigned long long)c);
    os << line;
  };

  os << "\n[17] DRAM scheduler diagnostic (dram_diag)\n";
  std::snprintf(line, sizeof line, "  %-44s %9s %9s\n", "metric", "B", "C");
  os << line;
  u2("scheduling decisions (non-empty queue)", get(0, "dram_diag_decisions"),
     get(1, "dram_diag_decisions"));
  u2("commands issued", get(0, "dram_svc_events"), get(1, "dram_svc_events"));
  u2("row hits chosen", get(0, "dram_row_hits"), get(1, "dram_row_hits"));
  f2("row-hit candidates available per decision", ratio(0, "dram_hit_cand_sum"),
     ratio(1, "dram_hit_cand_sum"));
  f2("  of them eligible this cycle (mean)", ratio(0, "dram_hit_elig_sum"),
     ratio(1, "dram_hit_elig_sum"));
  f2("  of them bank-blocked (mean, hit_busy_sum)", ratio(0, "dram_hit_busy_sum"),
     ratio(1, "dram_hit_busy_sum"));
  u2("case A: eligible hit existed, not chosen", get(0, "dram_missed_hit"),
     get(1, "dram_missed_hit"));
  u2("case B: non-hit taken, hits bank-blocked", get(0, "dram_stall_case_b"),
     get(1, "dram_stall_case_b"));
  u2("case C: non-hit taken, no hit queued", get(0, "dram_stall_case_c"),
     get(1, "dram_stall_case_c"));
  f2("queue depth mean",
     double(get(0, "dram_q_occ_sum")) / double(r.b.cycles + 1),
     double(get(1, "dram_q_occ_sum")) / double(r.c.cycles + 1));
  u2("queue depth peak", get(0, "dram_max_q"), get(1, "dram_max_q"));
  for (u32 k = 0; k < 16; ++k) {
    char lab[64];
    std::snprintf(lab, sizeof lab, "  queue depth in [%u,%u)", 1u << k,
                  k == 15 ? 0u : (1u << (k + 1)));
    u2(k == 15 ? "  queue depth >= 32768" : lab, get(0, ("dram_q_h" + std::to_string(k)).c_str()),
       get(1, ("dram_q_h" + std::to_string(k)).c_str()));
  }
  for (u32 b = 0; b < r.cfg.banks && b < 4; ++b) {
    char lab[80];
    const std::string p = "dram_bank" + std::to_string(b);
    std::snprintf(lab, sizeof lab, "bank %u: reads", b);
    u2(lab, get(0, (p + "_reads").c_str()), get(1, (p + "_reads").c_str()));
    std::snprintf(lab, sizeof lab, "        hits");
    u2(lab, get(0, (p + "_hits").c_str()), get(1, (p + "_hits").c_str()));
    std::snprintf(lab, sizeof lab, "        conflicts");
    u2(lab, get(0, (p + "_conf").c_str()), get(1, (p + "_conf").c_str()));
    std::snprintf(lab, sizeof lab, "        mean queued requests");
    f2(lab,
       double(get(0, (p + "_qsum").c_str())) / double(r.b.cycles + 1),
       double(get(1, (p + "_qsum").c_str())) / double(r.c.cycles + 1));
    std::snprintf(lab, sizeof lab, "        peak queued requests");
    u2(lab, get(0, (p + "_qmax").c_str()), get(1, (p + "_qmax").c_str()));
  }
  u2("row activations (total)", get(0, "dram_row_acts_sum"), get(1, "dram_row_acts_sum"));
  u2("distinct (bank,row) pairs activated", get(0, "dram_rows_used"), get(1, "dram_rows_used"));
  f2("activations per activated row",
     get(0, "dram_rows_used") ? double(get(0, "dram_row_acts_sum")) / double(get(0, "dram_rows_used")) : 0.0,
     get(1, "dram_rows_used") ? double(get(1, "dram_row_acts_sum")) / double(get(1, "dram_rows_used")) : 0.0);
  u2("max activations on one (bank,row)", get(0, "dram_row_acts_max"), get(1, "dram_row_acts_max"));
  f2("queue wait mean (cycles)", per_read(0, "dram_wait_sum"), per_read(1, "dram_wait_sum"));
  u2("queue wait max (cycles)", get(0, "dram_wait_max"), get(1, "dram_wait_max"));
  for (u32 k = 0; k < 8; ++k) {
    char lab[64];
    std::snprintf(lab, sizeof lab, "  wait in [%u,%u)", 1u << k,
                  k == 7 ? 0u : (1u << (k + 1)));
    u2(k == 7 ? "  wait >= 128" : lab, get(0, ("dram_wait_h" + std::to_string(k)).c_str()),
       get(1, ("dram_wait_h" + std::to_string(k)).c_str()));
  }
  // ---- M11.3: per-requester block (0 SIMT, 1 engine index, 2 PTE, 3 engine target)
  {
    const char* cname[4] = {"SIMT", "engine index", "PTE fetch", "engine target"};
    auto cl = [](const char* f, u32 k) {
      return "dram_client" + std::to_string(k) + f;
    };
    for (u32 k = 0; k < 4; ++k) {
      char lab[80];
      std::snprintf(lab, sizeof lab, "reads: %s", cname[k]);
      u2(lab, get(0, cl("_reads", k).c_str()), get(1, cl("_reads", k).c_str()));
      std::snprintf(lab, sizeof lab, "hits: %s", cname[k]);
      u2(lab, get(0, cl("_hits", k).c_str()), get(1, cl("_hits", k).c_str()));
      std::snprintf(lab, sizeof lab, "  row-hit rate (%%): %s", cname[k]);
      f2(lab,
         get(0, cl("_reads", k).c_str())
             ? 100.0 * double(get(0, cl("_hits", k).c_str())) /
                   double(get(0, cl("_reads", k).c_str()))
             : 0.0,
         get(1, cl("_reads", k).c_str())
             ? 100.0 * double(get(1, cl("_hits", k).c_str())) /
                   double(get(1, cl("_reads", k).c_str()))
             : 0.0);
      std::snprintf(lab, sizeof lab, "activations: %s", cname[k]);
      u2(lab, get(0, cl("_conf", k).c_str()), get(1, cl("_conf", k).c_str()));
      std::snprintf(lab, sizeof lab, "  of them cross-requester: %s", cname[k]);
      u2(lab, get(0, cl("_interf", k).c_str()), get(1, cl("_interf", k).c_str()));
      std::snprintf(lab, sizeof lab, "queue mean: %s", cname[k]);
      f2(lab, double(get(0, cl("_qsum", k).c_str())) / double(r.b.cycles + 1),
         double(get(1, cl("_qsum", k).c_str())) / double(r.c.cycles + 1));
      std::snprintf(lab, sizeof lab, "queue peak: %s", cname[k]);
      u2(lab, get(0, cl("_qmax", k).c_str()), get(1, cl("_qmax", k).c_str()));
    }
  }
  u2("service cycles: row hits", get(0, "dram_svc_hit_cycles"), get(1, "dram_svc_hit_cycles"));
  u2("service cycles: activations", get(0, "dram_svc_miss_cycles"), get(1, "dram_svc_miss_cycles"));
}

// ---------------------------------------------------------------------------
// M15: per-requester attribution of the hierarchy traffic (Config::cache_diag)
// ---------------------------------------------------------------------------
// Read straight out of the run's exported counters, so the table cannot disagree
// with the log it was produced from. `ev x` counts evictions this requester's own
// fills caused on a line the *other* requester had used, and `hit x` counts L2
// hits whose line the other requester had touched last: the two direct measures
// of cross-requester interference.
namespace {
void print_client_caches(const Stats& s, std::ostream& os) {
  char line[512];
  static const char* nm[4] = {"simt", "e-idx", "walk", "e-tgt"};
  os << "\n  per-requester hierarchy attribution (cache_diag):\n";
  std::snprintf(line, sizeof line,
                "  %-6s %9s %9s %9s %9s %9s %9s %8s %6s %6s %9s %9s %9s %9s\n", "req",
                "L1reads", "L1hits", "L1miss", "L2reads", "L2hits", "L2miss", "L2ev",
                "ev x", "hit x", "DRAMrd", "issued", "done", "lastdone");
  os << line;
  for (u32 k = 0; k < 4; ++k) {
    const std::string p = "cc" + std::to_string(k);
    const std::string d = "dram_client" + std::to_string(k);
    std::snprintf(line, sizeof line,
                  "  %-6s %9llu %9llu %9llu %9llu %9llu %9llu %8llu %6llu %6llu %9llu %9llu %9llu %9llu\n",
                  nm[k], (unsigned long long)s.get(p + "_l1_reads"),
                  (unsigned long long)s.get(p + "_l1_hits"),
                  (unsigned long long)s.get(p + "_l1_misses"),
                  (unsigned long long)s.get(p + "_l2_reads"),
                  (unsigned long long)s.get(p + "_l2_hits"),
                  (unsigned long long)s.get(p + "_l2_misses"),
                  (unsigned long long)s.get(p + "_l2_evict"),
                  (unsigned long long)s.get(p + "_l2_evict_cross"),
                  (unsigned long long)s.get(p + "_l2_hit_cross"),
                  (unsigned long long)s.get(d + "_reads"),
                  (unsigned long long)s.get(p + "_issue"),
                  (unsigned long long)s.get(p + "_done"),
                  (unsigned long long)s.get(p + "_last_done"));
    os << line;
  }
  // The per-requester identity, stated rather than assumed: the split must add
  // back up to the aggregate the same run reports.
  u64 r1 = 0, h1 = 0, m1 = 0, r2 = 0, h2 = 0, m2 = 0;
  for (u32 k = 0; k < 4; ++k) {
    const std::string p = "cc" + std::to_string(k);
    r1 += s.get(p + "_l1_reads");
    h1 += s.get(p + "_l1_hits");
    m1 += s.get(p + "_l1_misses");
    r2 += s.get(p + "_l2_reads");
    h2 += s.get(p + "_l2_hits");
    m2 += s.get(p + "_l2_misses");
  }
  std::snprintf(line, sizeof line,
                "  split check: L1 %llu/%llu/%llu vs %llu, L2 %llu/%llu/%llu vs %llu "
                "(reads/hits/misses)\n",
                (unsigned long long)r1, (unsigned long long)h1, (unsigned long long)m1,
                (unsigned long long)s.get("l1_reads"), (unsigned long long)r2,
                (unsigned long long)h2, (unsigned long long)m2,
                (unsigned long long)s.get("l2_reads"));
  os << line;
}
}  // namespace

void print_m9(const M9Result& r, std::ostream& os) {
  char line[512];
  os << "== M9: compiler-directed SIMT/bulk dispatch ==\n";
  os << "  workload " << r.cfg.wl << " N=" << r.cfg.N << " Lambda=" << r.cfg.Lambda
     << " P=" << r.cfg.P << " kappa=" << r.cfg.kappa << " seed=" << r.cfg.seed
     << "\n  op_class=" << r.cfg.op_class << " R=" << r.cfg.R
     << (r.cfg.ab_charge_latch ? ", latch charged" : ", latch free")
     << (r.cfg.pool_slots ? ", pool " + std::to_string(r.cfg.pool_slots) + " (diagnostic)" : "")
     << " dispatch_order=" << r.cfg.dispatch_order
     << " mem_path=" << r.cfg.mem_path << "\n";
  os << "  class map: " << r.instr_simt << " SIMT + " << r.instr_bulk << " bulk instructions -> "
     << r.elems_simt << " + " << r.elems_bulk << " elements"
     << (r.all_simt() ? "   [all SIMT]" : (r.all_bulk() ? "   [all bulk]" : "   [mixed]"))
     << "\n\n";

  if (r.cfg.skip_b)
    os << "  arm B skipped (skip_b=1): a whole-trace gather of a multi-stream application\n"
          "  is not one of the M18 configurations; the comparison is A vs C.\n";
  os << "  A = SIMT only, B = bulk only (M8), C = dispatched\n";
  std::snprintf(line, sizeof line, "  %-24s %12s %12s %12s\n", "metric", "A", "B", "C");
  os << line;
  auto row = [&](const char* k, const char* name) {
    std::snprintf(line, sizeof line, "  %-24s %12llu %12llu %12llu\n", name,
                  static_cast<unsigned long long>(r.a.s.get(k)),
                  static_cast<unsigned long long>(r.b.s.get(k)),
                  static_cast<unsigned long long>(r.c.s.get(k)));
    os << line;
  };
  row("cycles", "cycles");
  row("requests_generated", "line requests");
  row("bulk_desc_submitted", "descriptors");
  row("bulk_idx_lines_req", "index lines");
  row("bulk_tgt_req_gen", "target requests");
  row("dram_reads", "DRAM reads");
  row("l1_reads", "L1 reads");
  row("walks", "page walks");
  row("completion_hash", "completion hash");

  os << "\n  residency and staging (separate, not summed into one budget claim):\n";
  std::snprintf(line, sizeof line, "  %-24s %12llu %12llu %12llu\n", "peak slot-resident",
                static_cast<unsigned long long>(r.a.s.get("simt_peak_slot")),
                static_cast<unsigned long long>(r.b.s.get("bulk_buf_max")),
                static_cast<unsigned long long>(r.c.s.get("peak_total_outstanding")));
  os << line;
  std::snprintf(line, sizeof line, "  %-24s %12llu %12llu %12llu\n", "  of which SIMT",
                static_cast<unsigned long long>(r.a.s.get("simt_peak_slot")), 0ull,
                static_cast<unsigned long long>(r.c.s.get("simt_peak_slot")));
  os << line;
  std::snprintf(line, sizeof line, "  %-24s %12llu %12llu %12llu\n", "  of which bulk", 0ull,
                static_cast<unsigned long long>(r.b.s.get("bulk_buf_max")),
                static_cast<unsigned long long>(r.c.s.get("bulk_peak_slot")));
  os << line;
  std::snprintf(line, sizeof line, "  %-24s %12llu %12llu %12llu\n", "coalescer staging (peak)",
                static_cast<unsigned long long>(r.a.s.get("simt_staging_max")), 0ull,
                static_cast<unsigned long long>(r.c.s.get("simt_staging_max")));
  os << line;
  std::snprintf(line, sizeof line, "  %-24s %12llu %12llu %12llu\n", "slot pool (R - charge)",
                static_cast<unsigned long long>(r.a.s.get("slots_total")),
                static_cast<unsigned long long>(r.b.s.get("slots_total")),
                static_cast<unsigned long long>(r.c.s.get("slot_pool_total")));
  os << line;

  if (!r.all_simt()) {
    // G4: the ordering is visible in the run, not just in the source. The class
    // map and the compacted index array exist before the Sim is built, and the
    // descriptor is submitted before the first mem_->tick, so every index and
    // target request below necessarily follows them.
    os << "\n  G4 ordering (C): class map and compaction completed before cycle 0; descriptor "
          "submitted before cycle 0.\n     first index request "
       << r.c.s.get("bulk_first_idx_issue") << ", descriptor fetch done "
       << r.c.s.get("bulk_first_desc_done") << ", first admit "
       << r.c.s.get("bulk_first_admit") << ", first target issue "
       << r.c.s.get("bulk_first_tgt_issue") << "\n     compaction: " << r.elems_bulk << " of "
       << (r.elems_simt + r.elems_bulk) << " elements entered the index array; " << r.elems_simt
       << " SIMT elements were never indexed\n";
  }
  if (!r.all_simt() && !r.all_bulk()) {
    os << "\n  output check (C): value hash " << r.c_value_hash << " vs expectation "
       << r.c_expected_hash << ", mismatched addresses " << r.c_mismatch << "\n";
  }
  if (r.cfg.cache_diag) {
    os << "\n  C arm:";
    print_client_caches(r.c.s, os);
    // One `m15 key=value` line per counter, so a matrix log can be reduced
    // without re-running anything and without a second parser for the tables.
    static const char* keys[] = {
        "trace_lines", "trace_pages", "simt_instr", "bulk_instr", "simt_elems",
        "bulk_elems", "simt_requests_generated", "simt_issued", "simt_completed",
        "bulk_elems_total", "bulk_elems_done", "bulk_lines_issued", "bulk_dedup_merged",
        "bulk_tgt_req_gen", "bulk_tgt_req_done", "bulk_idx_lines_req", "bulk_win_entries",
        "bulk_win_sealed", "bulk_win_partial", "bulk_win_size_mean", "bulk_win_occ_max",
        "bulk_win_fill_wait", "bulk_win_full_stall", "bulk_org_cycles", "bulk_fanout_cycles",
        "bulk_fanout_elems", "bulk_buf_full_stall", "bulk_idx_wait_stall", "bulk_slot_stall",
        "bulk_dir_occ_max", "bulk_first_seal", "bulk_last_seal", "bulk_win_issue_idle",
        "simt_form_refuse", "simt_issue_stalls", "peak_total_outstanding", "replays",
        "dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_rows_used", "walks",
        "pte_fetches", "tlb0_misses", "l2tlb_misses", "l2_evict", "bulk_addr_mismatch",
        "N", "bulk_win_fused", "bulk_win_vec_bytes", "bulk_win_fused_vec_bytes",
        "bulk_win_size_max", "bulk_win_issue_idle", "simt_last_done_cycle",
        "bulk_last_done_cycle", "dram_svc_events", "l2_mshr_max", "inflight_max",
        // M18A: the index-accounting audit needs the acquisition work in the log
        // itself. Read-only: these are Stats values, no decision consults them.
        "bulk_desc_submitted", "bulk_desc_completed", "bulk_desc_rejected",
        "bulk_desc_cycles_sum", "bulk_idx_lines_req", "bulk_idx_lines_done",
        "bulk_idx_elems_done", "bulk_idx_bytes", "bulk_idx_consumed",
        "bulk_tgt_req_gen", "bulk_tgt_req_issued", "bulk_tgt_req_done",
        "bulk_ag_cycles", "bulk_first_tgt_issue", "bulk_last_fill",
        "bulk_idx_buf_stall", "bulk_idx_wait_stall", "bulk_win_bytes",
        "bulk_sched_idle"};
    for (const char* k : keys) {
      os << "  m15 " << k << "=" << r.c.s.get(k) << "\n";
    }
    // M16: per-arm memory counters. Parts A and B compare the same structure
    // across arms (SIMT-only vs bulk-only vs hybrid) and across MSHR capacities,
    // so every arm's occupancy has to be readable without parsing the width-
    // formatted tables. Diagnostic only: the counters are the ones the tables
    // above already print, and nothing here feeds back into the model.
    static const char* arm_keys[] = {
        "l1_reads", "l1_hits", "l1_misses", "l1_mshr_allocs", "l1_mshr_max",
        "l2_reads", "l2_hits", "l2_misses", "l2_mshr_allocs", "l2_mshr_max",
        "l2_evict", "l1_stall_cycles", "l2_stall_cycles", "replays",
        "dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_rows_used",
        "dram_svc_events", "dram_q_occ_sum", "dram_elig_sum", "dram_elig_max",
        "dram_busy_cycles", "dram_starved_cycles", "dram_blocked_cycles",
        "inflight_occ", "inflight_max", "walks", "pte_fetches", "tlb0_misses",
        "tlb0_mshr_stalls", "l2tlb_misses", "l2tlb_hits", "simt_completed",
        "simt_requests_generated", "bulk_tgt_req_issued", "bulk_tgt_req_done",
        "bulk_lines_issued", "bulk_dedup_merged", "bulk_fanout_elems",
        "bulk_org_cycles", "bulk_fanout_cycles", "bulk_sched_idle",
        "bulk_sched_defer", "bulk_sched_switches", "bulk_win_full_stall",
        "bulk_idx_wait_stall", "bulk_slot_stall", "simt_form_refuse",
        "simt_issue_stalls", "bulk_win_fused_vec_bytes", "bulk_win_vec_bytes"};
    struct { const char* n; const Stats* s; } arms[] = {
        {"A", &r.a.s}, {"B", &r.b.s}, {"C", &r.c.s}};
    for (const auto& a : arms) {
      for (const char* k : arm_keys)
        os << "  m16 " << a.n << "_" << k << "=" << a.s->get(k) << "\n";
    }
    os << "  dispatch: " << r.c.s.get("simt_instr") << " SIMT + " << r.c.s.get("bulk_instr")
       << " bulk instructions, " << r.c.s.get("simt_elems") << " + " << r.c.s.get("bulk_elems")
       << " of " << r.c.s.get("trace_accesses") << " elements; unaccounted "
       << r.c.s.get("dispatch_unaccounted") << "; SIMT done by cycle "
       << r.c.s.get("simt_last_done_cycle") << ", bulk done by "
       << r.c.s.get("bulk_last_done_cycle") << "\n";
  }
  for (const auto& n : r.notes) os << "  note: " << n << "\n";
  for (const auto& v : r.violations) os << "  VIOLATION: " << v << "\n";
  os << "\n  " << (r.ok ? "M9 GATES PASSED" : "M9 GATES FAILED") << "\n";
  if (r.cfg.dram_diag) print_dram_diag(r, os);
}

// ---------------------------------------------------------------------------
// M9 diagnostic: the same three arms, read cycle by cycle (experiment=m9diag)
// ---------------------------------------------------------------------------
// Every number below is either a counter the run already exported or an
// observation of one of those counters taken once per cycle. Nothing here is
// reconstructed, fitted or estimated; the interpretations live in the report.
void print_m9_diag(const M9Result& r, const CycleTrace& ta, const CycleTrace& tb,
                   const CycleTrace& tc, std::ostream& os) {
  char line[512];
  auto mean = [](const CycleTrace& t, u32 CycleSample::*f) {
    if (t.v.empty()) return 0.0;
    double sum = 0;
    for (const auto& q : t.v) sum += static_cast<double>(q.*f);
    return sum / static_cast<double>(t.v.size());
  };
  auto peak = [](const CycleTrace& t, u32 CycleSample::*f) {
    u32 p = 0;
    for (const auto& q : t.v) p = std::max(p, q.*f);
    return p;
  };
  auto first_inc = [](const CycleTrace& t, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    for (size_t i = 0; i < t.v.size(); ++i) {
      const u32 v = t.v[i].*f;
      if (v > prev) return i;
      prev = v;
    }
    return t.v.size();
  };
  auto last_inc = [](const CycleTrace& t, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    u64 last = 0;
    for (size_t i = 0; i < t.v.size(); ++i) {
      const u32 v = t.v[i].*f;
      if (v > prev) last = i;
      prev = v;
    }
    return last;
  };
  auto inc_cycles = [](const CycleTrace& t, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    u64 c = 0;
    for (const auto& q : t.v) {
      const u32 v = q.*f;
      if (v > prev) ++c;
      prev = v;
    }
    return c;
  };
  auto inc_in = [](const CycleTrace& t, u64 lo, u64 hi, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    u64 c = 0;
    for (size_t i = 0; i < t.v.size(); ++i) {
      const u32 v = t.v[i].*f;
      if (i >= lo && i <= hi && v > prev) ++c;
      prev = v;
    }
    return c;
  };
  auto at = [](const CycleTrace& t, u64 c, u32 CycleSample::*f) -> u64 {
    if (t.v.empty()) return 0;
    if (c >= t.v.size()) c = t.v.size() - 1;
    return t.v[c].*f;
  };
  auto both_inc = [](const CycleTrace& t) -> u64 {
    u32 pd = 0, pb = 0;
    u64 c = 0;
    for (const auto& q : t.v) {
      if (q.simt_done > pd && q.bulk_done > pb) ++c;
      pd = q.simt_done;
      pb = q.bulk_done;
    }
    return c;
  };
  auto idle_mem = [](const CycleTrace& t) -> u64 {
    u64 c = 0;
    for (const auto& q : t.v)
      if (q.mem_inflight == 0) ++c;
    return c;
  };
  auto row3 = [&](const char* name, const char* k) {
    std::snprintf(line, sizeof line, "  %-34s %10llu %10llu %10llu\n", name,
                  static_cast<unsigned long long>(r.a.s.get(k)),
                  static_cast<unsigned long long>(r.b.s.get(k)),
                  static_cast<unsigned long long>(r.c.s.get(k)));
    os << line;
  };
  auto row3v = [&](const char* name, u64 a, u64 b, u64 c) {
    std::snprintf(line, sizeof line, "  %-34s %10llu %10llu %10llu\n", name,
                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                  static_cast<unsigned long long>(c));
    os << line;
  };
  const u64 ca = r.a.cycles, cb = r.b.cycles, cc = r.c.cycles;
  const u64 la = last_inc(ta, &CycleSample::simt_done);
  const u64 lb = last_inc(tb, &CycleSample::bulk_done);
  const u64 c_simt_fin = last_inc(tc, &CycleSample::simt_done);
  const u64 c_bulk_fin = last_inc(tc, &CycleSample::bulk_done);

  os << "\n== M9 diagnostic: kappa " << r.cfg.kappa << " (Lambda " << r.cfg.Lambda << ", P "
     << r.cfg.P << ", R " << r.cfg.R << (r.cfg.ab_charge_latch ? ", latch charged" : ", latch free")
     << ", pool " << r.c.s.get("slot_pool_slots") << " slots)" << " ==\n  A = SIMT only, B = bulk only (M8), C = dispatched; cycles A="
     << ca << " B=" << cb << " C=" << cc << "\n";

  os << "\n[1] timeline (cycles, directly observed)\n";
  std::snprintf(line, sizeof line, "  %-34s %10s %10s %10s\n", "metric", "A", "B", "C");
  os << line;
  row3v("first request submitted", first_inc(ta, &CycleSample::issued),
        first_inc(tb, &CycleSample::issued), first_inc(tc, &CycleSample::issued));
  row3v("first SIMT completion", first_inc(ta, &CycleSample::simt_done), 0,
        first_inc(tc, &CycleSample::simt_done));
  row3v("last SIMT completion", la, 0, c_simt_fin);
  row3v("first bulk element done", 0, first_inc(tb, &CycleSample::bulk_done),
        first_inc(tc, &CycleSample::bulk_done));
  row3v("last bulk element done", 0, lb, c_bulk_fin);
  row3v("run end", ca, cb, cc);
  row3("first index issue", "bulk_first_idx_issue");
  row3("descriptor fetch done", "bulk_first_desc_done");
  row3("index acquisition done", "bulk_last_idx_done");
  row3("first target issue", "bulk_first_tgt_issue");
  row3("last target issue", "bulk_last_tgt_issue");
  row3("last fill (bulk)", "bulk_last_fill");
  row3("first element admitted", "bulk_first_admit");
  row3("first window sealed", "bulk_first_seal");
  row3("last window sealed", "bulk_last_seal");

  os << "\n[2] index acquisition vs SIMT execution\n";
  for (int arm = 0; arm < 2; ++arm) {
    const RunResult& x = arm ? r.c : r.b;
    const CycleTrace& t = arm ? tc : tb;
    const u64 lo = x.s.get("bulk_first_idx_issue"), hi = x.s.get("bulk_last_idx_done");
    const u64 tgt = x.s.get("bulk_first_tgt_issue");
    os << "  " << (arm ? "C" : "B") << ": index window " << lo << ".." << hi << " ("
       << (hi - lo) << " cycles), first target " << tgt
       << ", gap index->target " << (tgt > hi ? tgt - hi : 0) << "\n";
    if (arm) {
      os << "     SIMT requests completed before the window opens " << at(t, lo, &CycleSample::simt_done)
         << " of " << r.c.s.get("simt_requests_generated") << "\n"
         << "     SIMT completions inside the window "
         << inc_in(t, lo, hi, &CycleSample::simt_done) << " cycles with progress\n"
         << "     bulk elements completed inside the window "
         << inc_in(t, lo, hi, &CycleSample::bulk_done) << " cycles with progress\n";
    }
  }

  u64 only_simt = 0, only_bulk = 0, neither = 0;
  {
    u32 pd = 0, pb = 0;
    for (const auto& q : tc.v) {
      const bool ds = q.simt_done > pd, db = q.bulk_done > pb;
      if (ds && db) {
      } else if (ds) {
        ++only_simt;
      } else if (db) {
        ++only_bulk;
      } else {
        ++neither;
      }
      pd = q.simt_done;
      pb = q.bulk_done;
    }
  }

  os << "\n[3] completion split (C)\n";
  std::snprintf(line, sizeof line, "  %-34s %10llu\n", "SIMT last completion",
                static_cast<unsigned long long>(c_simt_fin));
  os << line;
  std::snprintf(line, sizeof line, "  %-34s %10llu\n", "bulk last element",
                static_cast<unsigned long long>(c_bulk_fin));
  os << line;
  std::snprintf(line, sizeof line, "  %-34s %10llu\n", "cycles both paths completed",
                static_cast<unsigned long long>(both_inc(tc)));
  os << line;
  std::snprintf(line, sizeof line, "  %-34s %10llu\n", "cycles only SIMT completed",
                static_cast<unsigned long long>(only_simt));
  os << line;
  std::snprintf(line, sizeof line, "  %-34s %10llu\n", "cycles only bulk completed",
                static_cast<unsigned long long>(only_bulk));
  os << line;
  std::snprintf(line, sizeof line, "  %-34s %10llu\n", "cycles neither path completed",
                static_cast<unsigned long long>(neither));
  os << line;
  {
    u64 both_hold = 0;
    for (const auto& q : tc.v)
      if (q.pool_simt > 0 && q.pool_bulk > 0) ++both_hold;
    std::snprintf(line, sizeof line, "  %-34s %10llu\n", "cycles both paths hold slots",
                  static_cast<unsigned long long>(both_hold));
    os << line;
  }

  os << "\n[4] slot residency, 128-slot pool (peak in C / mean over cycles per arm)\n";
  std::snprintf(line, sizeof line, "  %-30s %6s %9s %9s %9s\n", "owner", "peak", "A mean",
                "B mean", "C mean");
  os << line;
  std::snprintf(line, sizeof line, "  %-30s %6u %9.1f %9.1f %9.1f\n", "SIMT client",
                peak(tc, &CycleSample::pool_simt), mean(ta, &CycleSample::pool_simt),
                mean(tb, &CycleSample::pool_simt), mean(tc, &CycleSample::pool_simt));
  os << line;
  std::snprintf(line, sizeof line, "  %-30s %6u %9.1f %9.1f %9.1f\n", "engine",
                peak(tc, &CycleSample::pool_bulk), mean(ta, &CycleSample::pool_bulk),
                mean(tb, &CycleSample::pool_bulk), mean(tc, &CycleSample::pool_bulk));
  os << line;
  std::snprintf(line, sizeof line, "  %-30s %6u %9.1f %9.1f %9.1f\n", "coalescer latch (not a slot)",
                peak(tc, &CycleSample::simt_latch), mean(ta, &CycleSample::simt_latch),
                mean(tb, &CycleSample::simt_latch), mean(tc, &CycleSample::simt_latch));
  os << line;
  std::snprintf(line, sizeof line, "  %-30s %6u %9.1f %9.1f %9.1f\n", "hierarchy inflight",
                peak(tc, &CycleSample::mem_inflight), mean(ta, &CycleSample::mem_inflight),
                mean(tb, &CycleSample::mem_inflight), mean(tc, &CycleSample::mem_inflight));
  os << line;

  os << "\n[5] C occupancy snapshot (cycle: slots SIMT/engine/free, hierarchy inflight)\n";
  for (int i = 0; i <= 10; ++i) {
    const u64 c = (tc.v.empty() ? 0 : (tc.v.size() - 1)) * i / 10;
    std::snprintf(line, sizeof line, "     %7llu: %4llu %4llu %4llu   %4llu\n",
                  static_cast<unsigned long long>(c),
                  static_cast<unsigned long long>(at(tc, c, &CycleSample::pool_simt)),
                  static_cast<unsigned long long>(at(tc, c, &CycleSample::pool_bulk)),
                  static_cast<unsigned long long>(at(tc, c, &CycleSample::pool_free)),
                  static_cast<unsigned long long>(at(tc, c, &CycleSample::mem_inflight)));
    os << line;
  }

  os << "\n[6] translation\n";
  std::snprintf(line, sizeof line, "  %-34s %10s %10s %10s\n", "metric", "A", "B", "C");
  os << line;
  row3("dTLB reads", "tlb0_reads");
  row3("dTLB misses", "tlb0_misses");
  row3("dTLB MSHR stalls (cycles)", "tlb0_mshr_stalls");
  row3("refusals: parked", "tlb_refuse_parked");
  row3("refusals: MSHR full", "tlb_refuse_full");
  row3("max parked on one entry", "tlb_max_parked");
  row3("L2 TLB misses", "l2tlb_misses");
  row3("L2 TLB walk stalls", "l2tlb_walk_stalls");
  row3("walks", "walks");
  row3("walker busy (cycles)", "walker_busy");
  row3("walker peak", "walker_max");
  row3("PTE fetches", "pte_fetches");

  os << "\n[7] hierarchy\n";
  std::snprintf(line, sizeof line, "  %-34s %10s %10s %10s\n", "metric", "A", "B", "C");
  os << line;
  row3("L1 requests", "l1_reads");
  row3("L1 hits", "l1_hits");
  row3("L1 MSHR joins", "l1_mshr_joins");
  row3("L1 MSHR allocs", "l1_mshr_allocs");
  row3("L1 stall cycles", "l1_stall_cycles");
  row3("L2 requests", "l2_reads");
  row3("L2 hits", "l2_hits");
  row3("L2 MSHR joins", "l2_mshr_joins");
  row3("DRAM reads", "dram_reads");
  row3("DRAM row hits", "dram_row_hits");
  row3("DRAM row conflicts", "dram_row_conflicts");
  row3("DRAM bank busy (cycles)", "dram_busy_cycles");
  row3("DRAM blocked (cycles)", "dram_blocked_cycles");
  row3("DRAM starved (cycles)", "dram_starved_cycles");
  row3("replays", "replays");

  os << "\n[7b] engine organization (B vs C, same configuration)\n";
  std::snprintf(line, sizeof line, "  %-34s %10s %10s\n", "metric", "B", "C");
  os << line;
  auto row2 = [&](const char* name, const char* k) {
    std::snprintf(line, sizeof line, "  %-34s %10llu %10llu\n", name,
                  static_cast<unsigned long long>(r.b.s.get(k)),
                  static_cast<unsigned long long>(r.c.s.get(k)));
    os << line;
  };
  row2("elements in the descriptor", "bulk_elems_total");
  row2("index lines requested", "bulk_idx_lines_req");
  row2("target requests generated", "bulk_tgt_req_gen");
  row2("lines issued (deduplicated)", "bulk_lines_issued");
  row2("dedup merges", "bulk_dedup_merged");
  row2("batches", "bulk_batches");
  row2("seals: full / end / deadline", "bulk_seals_full");
  row2("  seals end", "bulk_seals_end");
  row2("  seals deadline", "bulk_seals_deadline");
  row2("window entries", "bulk_win_entries");
  row2("window occupancy peak", "bulk_win_occ_max");
  row2("scheduler opens / switches", "bulk_sched_opens");
  row2("  switches", "bulk_sched_switches");
  row2("  deferrals", "bulk_sched_defer");
  row2("  probes", "bulk_sched_probes");
  row2("address-generation cycles", "bulk_ag_cycles");
  row2("targets address-generated", "bulk_tgt_addr_gen");

  os << "\n[8] traffic (64 B lines)\n";
  std::snprintf(line, sizeof line, "  %-34s %10s %10s %10s\n", "metric", "A", "B", "C");
  os << line;
  row3("line requests", "requests_generated");
  row3("DRAM bytes", "dram_bytes");
  row3("descriptor lines", "bulk_desc_line_bytes");
  row3("index lines", "bulk_idx_line_bytes");
  row3("target lines", "bulk_tgt_line_bytes");

  os << "\n[9] cycles without progress (counted from cumulative counters)\n";
  std::snprintf(line, sizeof line, "  %-34s %10s %10s %10s\n", "metric", "A", "B", "C");
  os << line;
  row3v("hierarchy empty (nothing inflight)", idle_mem(ta), idle_mem(tb), idle_mem(tc));
  row3v("SIMT issue refused (>=1/window)", inc_cycles(ta, &CycleSample::simt_stalls),
        inc_cycles(tb, &CycleSample::simt_stalls), inc_cycles(tc, &CycleSample::simt_stalls));
  // `bulk_slot_stall` is the engine's own guard: it declines to issue a target
  // while the pool has <= idx_reserve (8) free slots, so it is a direct
  // contention counter -- it rises whenever the other client holds the pool.
  row3v("engine slot stall (free <= 8)", 0, inc_cycles(tb, &CycleSample::bulk_slot_stall),
        inc_cycles(tc, &CycleSample::bulk_slot_stall));
  row3v("engine index wait", 0, inc_cycles(tb, &CycleSample::bulk_idx_wait),
        inc_cycles(tc, &CycleSample::bulk_idx_wait));
  row3v("engine buffer full", 0, inc_cycles(tb, &CycleSample::bulk_buf_full),
        inc_cycles(tc, &CycleSample::bulk_buf_full));
  row3v("engine window full", 0, inc_cycles(tb, &CycleSample::bulk_win_full),
        inc_cycles(tc, &CycleSample::bulk_win_full));
  std::snprintf(line, sizeof line, "  %-34s %9.1f%% %9.1f%% %9.1f%%\n", "hierarchy busy fraction",
                100.0 * double(ca + 1 - idle_mem(ta)) / double(ca + 1),
                100.0 * double(cb + 1 - idle_mem(tb)) / double(cb + 1),
                100.0 * double(cc + 1 - idle_mem(tc)) / double(cc + 1));
  os << line;

  // ---- [10] allocation, refusal, release -----------------------------------
  os << "\n[10] allocation, refusal and release (whole run)\n";
  std::snprintf(line, sizeof line, "  %-38s %10s %10s %10s\n", "metric", "A", "B", "C");
  os << line;
  row3v("SIMT slots taken", r.a.s.get("slot_allocs"), 0, r.c.s.get("slot_allocs"));
  row3v("SIMT formation refusals (cycles)", 0, 0, r.c.s.get("simt_form_refuse"));
  row3v("engine slots taken", 0, r.b.s.get("bulk_slot_take"), r.c.s.get("bulk_slot_take"));
  row3v("engine slots released", 0, r.b.s.get("bulk_slot_give"), r.c.s.get("bulk_slot_give"));
  row3v("engine target refusals (total)", 0, r.b.s.get("bulk_slot_stall"),
        r.c.s.get("bulk_slot_stall"));
  row3v("  free == 0 (pool empty)", 0, 0, r.c.s.get("bulk_tgt_refuse_noslot"));
  row3v("  0 < free <= idx_reserve", 0, 0, r.c.s.get("bulk_tgt_refuse_reserve"));
  {
    const u64 sr = r.c.s.get("simt_form_refuse");
    const u64 br = r.c.s.get("bulk_slot_stall");
    auto avg = [](u64 sum, u64 n) { return n ? double(sum) / double(n) : 0.0; };
    std::snprintf(line, sizeof line,
                  "  at SIMT refusals (%llu): mean slots held  SIMT %5.1f  engine %5.1f  free %5.1f\n",
                  static_cast<unsigned long long>(sr), avg(r.c.s.get("simt_ref_simt_sum"), sr),
                  avg(r.c.s.get("simt_ref_bulk_sum"), sr),
                  avg(r.c.s.get("simt_ref_free_sum"), sr));
    os << line;
    std::snprintf(line, sizeof line,
                  "  at engine refusals (%llu): mean slots held SIMT %5.1f  engine %5.1f  free %5.1f\n",
                  static_cast<unsigned long long>(br), avg(r.c.s.get("bulk_ref_simt_sum"), br),
                  avg(r.c.s.get("bulk_ref_bulk_sum"), br),
                  avg(r.c.s.get("bulk_ref_free_sum"), br));
    os << line;
    std::snprintf(line, sizeof line,
                  "  reconciliation: free==0 + reserved = %llu vs slot_stall = %llu; "
                  "taken - released = %lld\n",
                  static_cast<unsigned long long>(r.c.s.get("bulk_tgt_refuse_noslot") +
                                                  r.c.s.get("bulk_tgt_refuse_reserve")),
                  static_cast<unsigned long long>(br),
                  static_cast<long long>(r.c.s.get("bulk_slot_take")) -
                      static_cast<long long>(r.c.s.get("bulk_slot_give")));
    os << line;
  }

  // ---- [11] simultaneous residency vs simultaneous progress ----------------
  u64 both_act = 0, both_prog = 0, only_s = 0, only_b = 0, none_p = 0;
  u64 both_block = 0, s_block_only = 0, b_block_only = 0, no_block = 0;
  u64 both_hold2 = 0, both_hold_prog = 0;
  u64 simt_block_engine_ran = 0, engine_block_simt_ran = 0;
  u64 simt_block_engine_idle = 0, engine_block_simt_idle = 0;
  for (const auto& q : tc.v) {
    if (!q.simt_active || !q.bulk_active) continue;
    ++both_act;
    const bool sp = q.simt_progress != 0, bp = q.bulk_progress != 0;
    if (sp && bp) ++both_prog;
    else if (sp) ++only_s;
    else if (bp) ++only_b;
    else ++none_p;
    const bool sb = q.simt_blocked != 0, bb = q.bulk_blocked != 0;
    if (sb && bb) ++both_block;
    else if (sb) ++s_block_only;
    else if (bb) ++b_block_only;
    else ++no_block;
    if (q.pool_simt > 0 && q.pool_bulk > 0) {
      ++both_hold2;
      if (sp && bp) ++both_hold_prog;
    }
    if (sb) {
      if (bp) ++simt_block_engine_ran;
      else if (!sp) ++simt_block_engine_idle;
    }
    if (bb) {
      if (sp) ++engine_block_simt_ran;
      else if (!bp) ++engine_block_simt_idle;
    }
  }
  os << "\n[11] readiness vs progress (C, cycles in which both clients still had work)\n";
  std::snprintf(line, sizeof line, "  %-44s %10llu\n", "both clients active",
                static_cast<unsigned long long>(both_act));
  os << line;
  std::snprintf(line, sizeof line, "  %-44s %10llu\n", "  both progressed",
                static_cast<unsigned long long>(both_prog));
  os << line;
  std::snprintf(line, sizeof line, "  %-44s %10llu\n", "  only SIMT progressed",
                static_cast<unsigned long long>(only_s));
  os << line;
  std::snprintf(line, sizeof line, "  %-44s %10llu\n", "  only engine progressed",
                static_cast<unsigned long long>(only_b));
  os << line;
  std::snprintf(line, sizeof line, "  %-44s %10llu\n", "  neither progressed",
                static_cast<unsigned long long>(none_p));
  os << line;
  std::snprintf(line, sizeof line, "  %-44s %10llu %10llu %10llu %10llu\n",
                "  of those, residency-blocked:", static_cast<unsigned long long>(both_block),
                static_cast<unsigned long long>(s_block_only),
                static_cast<unsigned long long>(b_block_only),
                static_cast<unsigned long long>(no_block));
  os << line;
  os << "     (both / SIMT only / engine only / neither)\n";
  std::snprintf(line, sizeof line,
                "  %-44s %10llu %10llu\n  %-44s %10llu %10llu\n",
                "SIMT blocked: engine progressed / did not",
                static_cast<unsigned long long>(simt_block_engine_ran),
                static_cast<unsigned long long>(simt_block_engine_idle),
                "engine blocked: SIMT progressed / did not",
                static_cast<unsigned long long>(engine_block_simt_ran),
                static_cast<unsigned long long>(engine_block_simt_idle));
  os << line;
  std::snprintf(line, sizeof line,
                "  %-44s %10llu   of those both progressed %llu\n",
                "cycles both clients held slots", static_cast<unsigned long long>(both_hold2),
                static_cast<unsigned long long>(both_hold_prog));
  os << line;

  // ---- [12] slot lifetime ---------------------------------------------------
  // Percentiles and the mean are over claims that outlived the cycle they were
  // made in, i.e. over requests. A zero-age claim is a take/release pair inside
  // one cycle (a submission the hierarchy refused); those are counted
  // separately rather than averaged into a request latency.
  auto pct = [](const SlotAges& a, int who, double p) {
    u64 tot = 0;
    for (u32 b = 0; b < SlotAges::NB; ++b) tot += a.n[who][b];
    tot -= a.zero[who];
    if (!tot) return std::string("n/a");
    const u64 want = static_cast<u64>(p * double(tot) + 0.999);
    u64 cum = 0;
    char tmp[64];
    for (u32 b = 0; b < SlotAges::NB; ++b) {
      cum += a.n[who][b] - (b == 0 ? a.zero[who] : 0);
      if (cum < want) continue;
      if (b + 1 == SlotAges::NB)
        std::snprintf(tmp, sizeof tmp, ">=%llu", (unsigned long long)SlotAges::lo(b));
      else
        std::snprintf(tmp, sizeof tmp, "%llu-%llu", (unsigned long long)SlotAges::lo(b),
                      (unsigned long long)SlotAges::hi(b));
      return std::string(tmp);
    }
    return std::string("n/a");
  };
  auto spans = [](const SlotAges& a, int who) {
    u64 n = 0;
    for (u32 b = 0; b < SlotAges::NB; ++b) n += a.n[who][b];
    return n;
  };
  auto occ_sum = [](const CycleTrace& t, u32 CycleSample::*f) {
    u64 s = 0;
    for (const auto& q : t.v) s += q.*f;
    return s;
  };
  os << "\n[12] slot lifetime, allocation to release (C's shared pool; cycles)\n";
  std::snprintf(line, sizeof line, "  %-24s %8s %8s %8s %10s %10s %10s\n", "client", "spans",
                "mean", "max", "p50", "p90", "p99");
  os << line;
  {
    const char* nm[2] = {"SIMT", "engine"};
    for (int who = 0; who < 2; ++who) {
      const u64 n = spans(tc.ages, who) - tc.ages.zero[who];
      const double mean = n ? double(tc.ages.sum[who]) / double(n) : 0.0;
      std::snprintf(line, sizeof line, "  %-24s %8llu %8.1f %8llu %10s %10s %10s\n", nm[who],
                    static_cast<unsigned long long>(n), mean,
                    static_cast<unsigned long long>(tc.ages.max[who]),
                    pct(tc.ages, who, 0.50).c_str(), pct(tc.ages, who, 0.90).c_str(),
                    pct(tc.ages, who, 0.99).c_str());
      os << line;
    }
    std::snprintf(line, sizeof line,
                  "  claims tallied / claimed: SIMT %llu / %llu, engine %llu / %llu; "
                  "zero-age (refused submission) SIMT %llu, engine %llu\n",
                  static_cast<unsigned long long>(spans(tc.ages, 0)),
                  static_cast<unsigned long long>(r.c.s.get("slot_allocs")),
                  static_cast<unsigned long long>(spans(tc.ages, 1)),
                  static_cast<unsigned long long>(r.c.s.get("bulk_slot_take")),
                  static_cast<unsigned long long>(tc.ages.zero[0]),
                  static_cast<unsigned long long>(tc.ages.zero[1]));
    os << line;
    // Coarse distribution: [0-3] [4-15] [16-63] [64-255] [256-1023] [1024+]
    for (int who = 0; who < 2; ++who) {
      u64 g[6] = {0, 0, 0, 0, 0, 0};
      for (u32 b = 0; b < SlotAges::NB; ++b) {
        const u32 gi = b <= 1 ? 0 : (b <= 3 ? 1 : (b <= 5 ? 2 : (b <= 7 ? 3 : (b <= 9 ? 4 : 5))));
        g[gi] += tc.ages.n[who][b];
      }
      std::snprintf(line, sizeof line,
                    "  %-24s %8llu %8llu %8llu %8llu %8llu %8llu  [0-3|4-15|16-63|64-255|256-1023|1024+]\n"
                    "",
                    who ? "  engine distribution" : "  SIMT distribution",
                    static_cast<unsigned long long>(g[0]), static_cast<unsigned long long>(g[1]),
                    static_cast<unsigned long long>(g[2]), static_cast<unsigned long long>(g[3]),
                    static_cast<unsigned long long>(g[4]), static_cast<unsigned long long>(g[5]));
      os << line;
    }
    // Little's law cross-check on the observed spans only: the summed ages
    // should equal the summed occupancy, up to the same-cycle claims excluded
    // above (which contribute a zero age but one cycle of occupancy each).
    std::snprintf(line, sizeof line,
                  "  sum of observed ages SIMT %llu vs occupancy integral %llu; "
                  "engine %llu vs %llu\n",
                  static_cast<unsigned long long>(tc.ages.sum[0]),
                  static_cast<unsigned long long>(occ_sum(tc, &CycleSample::pool_simt)),
                  static_cast<unsigned long long>(tc.ages.sum[1]),
                  static_cast<unsigned long long>(occ_sum(tc, &CycleSample::pool_bulk)));
    os << line;
  }

  // -------------------------------------------------------------------------
  // [13] index pipeline: acquisition -> readiness -> address generation
  // -------------------------------------------------------------------------
  // Everything below is a run counter or a difference of consecutive per-cycle
  // observations. "Consumed" (an index element taken out of the ring) and
  // "generated" (the address that element produced and admitted) are the same
  // count in this engine -- there is no address queue between them -- and are
  // still printed as two rows so the two events cannot be quoted as one.
  auto changes = [](const CycleTrace& t, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    u64 c = 0;
    for (const auto& q : t.v) {
      const u32 v = q.*f;
      if (v > prev) ++c;
      prev = v;
    }
    return c;
  };
  auto first_change = [](const CycleTrace& t, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    for (size_t i = 0; i < t.v.size(); ++i) {
      const u32 v = t.v[i].*f;
      if (v > prev) return i;
      prev = v;
    }
    return t.v.size();
  };
  auto last_change = [](const CycleTrace& t, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    u64 last = 0;
    for (size_t i = 0; i < t.v.size(); ++i) {
      const u32 v = t.v[i].*f;
      if (v > prev) last = i;
      prev = v;
    }
    return last;
  };
  auto max_delta = [](const CycleTrace& t, u32 CycleSample::*f) -> u64 {
    u32 prev = 0;
    u64 m = 0;
    for (const auto& q : t.v) {
      const u32 v = q.*f;
      if (v > prev && v - prev > m) m = v - prev;
      prev = v;
    }
    return m;
  };
  auto row2f = [&](const char* name, double b, double c) {
    std::snprintf(line, sizeof line, "  %-42s %10.1f %10.1f\n", name, b, c);
    os << line;
  };
  auto row2u = [&](const char* name, u64 b, u64 c) {
    std::snprintf(line, sizeof line, "  %-42s %10llu %10llu\n", name,
                  static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
    os << line;
  };
  auto rate = [](u64 a, u64 b) { return b ? 100.0 * double(a) / double(b) : 0.0; };

  os << "\n[13] index pipeline: acquisition -> readiness -> address generation (B vs C)\n";
  std::snprintf(line, sizeof line, "  %-42s %10s %10s\n", "metric", "B", "C");
  os << line;
  row2("index lines claimed into the ring", "bulk_idx_lines_req");
  row2("index lines submitted to the hierarchy", "bulk_idx_sub");
  row2("index lines returned", "bulk_idx_lines_done");
  row2("index elements consumed", "bulk_idx_consumed");
  row2("target addresses generated", "bulk_tgt_addr_gen");
  row2("target requests submitted", "bulk_tgt_req_issued");
  row2("AG stalls: no ready index element", "bulk_idx_wait_stall");
  row2("  of those, target capacity free", "bulk_idx_wait_cap");
  row2("target refusals (free <= reserve)", "bulk_slot_stall");
  row2("index claims refused for residency", "bulk_idx_buf_stall");
  row2("index lines deferred at the issue port", "bulk_idx_refuse");
  {
    const u64 wb = r.b.s.get("bulk_idx_wait_stall"), wc = r.c.s.get("bulk_idx_wait_stall");
    row2f("mean free slots at an AG stall",
          wb ? double(r.b.s.get("bulk_idx_wait_free_sum")) / double(wb) : 0.0,
          wc ? double(r.c.s.get("bulk_idx_wait_free_sum")) / double(wc) : 0.0);
    const u64 sb = r.b.s.get("bulk_slot_stall"), sc = r.c.s.get("bulk_slot_stall");
    row2f("mean free slots at a target refusal",
          sb ? double(r.b.s.get("bulk_slot_stall_free_sum")) / double(sb) : 0.0,
          sc ? double(r.c.s.get("bulk_slot_stall_free_sum")) / double(sc) : 0.0);
  }

  os << "\n[13b] per-cycle activity (cycles in which that counter advanced)\n";
  std::snprintf(line, sizeof line, "  %-42s %10s %10s\n", "cycles with ...", "B", "C");
  os << line;
  row2u("an index line submitted", changes(tb, &CycleSample::idx_sub),
        changes(tc, &CycleSample::idx_sub));
  row2u("an index line returned", changes(tb, &CycleSample::bulk_idx_done),
        changes(tc, &CycleSample::bulk_idx_done));
  row2u("an index element consumed", changes(tb, &CycleSample::idx_elems),
        changes(tc, &CycleSample::idx_elems));
  row2u("an address generated", changes(tb, &CycleSample::addr_gen),
        changes(tc, &CycleSample::addr_gen));
  row2u("a target submitted", changes(tb, &CycleSample::bulk_tgt_issue),
        changes(tc, &CycleSample::bulk_tgt_issue));
  row2u("the AG starved (no ready element)", changes(tb, &CycleSample::bulk_idx_wait),
        changes(tc, &CycleSample::bulk_idx_wait));
  row2u("the AG starved with capacity free", changes(tb, &CycleSample::idx_stall_cap),
        changes(tc, &CycleSample::idx_stall_cap));
  row2u("max index lines returned in a cycle", max_delta(tb, &CycleSample::bulk_idx_done),
        max_delta(tc, &CycleSample::bulk_idx_done));
  row2u("max elements generated in a cycle", max_delta(tb, &CycleSample::addr_gen),
        max_delta(tc, &CycleSample::addr_gen));
  row2u("max targets submitted in a cycle", max_delta(tb, &CycleSample::bulk_tgt_issue),
        max_delta(tc, &CycleSample::bulk_tgt_issue));
  row2u("first cycle an address was generated", first_change(tb, &CycleSample::addr_gen),
        first_change(tc, &CycleSample::addr_gen));
  row2u("last cycle an address was generated", last_change(tb, &CycleSample::addr_gen),
        last_change(tc, &CycleSample::addr_gen));
  row2u("first cycle an index element was consumed", first_change(tb, &CycleSample::idx_elems),
        first_change(tc, &CycleSample::idx_elems));
  row2u("last cycle an index element was consumed", last_change(tb, &CycleSample::idx_elems),
        last_change(tc, &CycleSample::idx_elems));

  os << "\n[13c] index readiness and buffering (per-cycle means, then peaks)\n";
  std::snprintf(line, sizeof line, "  %-42s %10s %10s\n", "metric", "B", "C");
  os << line;
  row2f("mean ready index elements", mean(tb, &CycleSample::idx_ready),
        mean(tc, &CycleSample::idx_ready));
  row2u("peak ready index elements", peak(tb, &CycleSample::idx_ready),
        peak(tc, &CycleSample::idx_ready));
  row2f("mean ready index lines", mean(tb, &CycleSample::idx_ready_lines),
        mean(tc, &CycleSample::idx_ready_lines));
  row2f("mean index lines in the ring", mean(tb, &CycleSample::idx_ring),
        mean(tc, &CycleSample::idx_ring));
  row2u("peak index lines in the ring", peak(tb, &CycleSample::idx_ring),
        peak(tc, &CycleSample::idx_ring));
  row2f("mean index lines holding a slot", mean(tb, &CycleSample::idx_res),
        mean(tc, &CycleSample::idx_res));
  row2u("peak index lines holding a slot", peak(tb, &CycleSample::idx_res),
        peak(tc, &CycleSample::idx_res));
  row2f("mean formed targets awaiting submit", mean(tb, &CycleSample::tgt_unissued),
        mean(tc, &CycleSample::tgt_unissued));
  row2f("mean sealed targets awaiting issue", mean(tb, &CycleSample::tgt_backlog),
        mean(tc, &CycleSample::tgt_backlog));
  row2u("peak sealed targets awaiting issue", peak(tb, &CycleSample::tgt_backlog),
        peak(tc, &CycleSample::tgt_backlog));

  // -------------------------------------------------------------------------
  // [14] readiness versus target progress: mutually exclusive cycle states
  // -------------------------------------------------------------------------
  // One cycle, first matching rule wins:
  //   D  a target request was submitted this cycle (D_i: an index line also moved)
  //   B  the target port was refused for residency (free <= idx_reserve)
  //   C  capacity was free and a sealed target line was waiting, yet no target
  //      was issued. Split by the measured cause: C1 the issue port was refused
  //      by the dTLB (a deferral or a policy refusal was recorded this cycle),
  //      C2 the generator still had elements but no window was accepting them
  //      (drain backpressure), C3 neither.
  //   A  the address generator stalled for an index value while target capacity
  //      was free and no sealed line was waiting
  //   E  no target issued, but an index line was submitted or returned while a
  //      target request was still outstanding
  //   F  none of the above, with the engine still active
  // Every input is an event-site measurement: `slot_stall` is the refusal at
  // win_emit, `idx_wait_stall` a failed idx_take, `idx_stall_cap` that same
  // stall with the pool above the reserve. A cycle that is merely waiting for a
  // target fill falls in F, never in A: A requires a failed index lookup.
  {
    u64 nA = 0, nB = 0, nC = 0, nD = 0, nDi = 0, nE = 0, nF = 0;
    u64 nC_x = 0, nC_nf = 0, nC_ot = 0, nNoFill = 0, nAgWork = 0;
    u64 nB_ready = 0, nF_idx = 0, nF_out = 0, nF_win = 0;
    u64 rA = 0, rB = 0, rC = 0, rD = 0, rE = 0, rF = 0;
    u64 oA = 0, oB = 0, oC = 0, oD = 0, oE = 0, oF = 0;
    int last = -1;
    for (size_t i = 1; i < tc.v.size(); ++i) {
      const CycleSample& q = tc.v[i];
      const CycleSample& p = tc.v[i - 1];
      if (!q.bulk_active) { last = -1; continue; }
      const bool tgt = q.bulk_tgt_issue > p.bulk_tgt_issue;
      const bool idx_moved = q.idx_sub > p.idx_sub || q.bulk_idx_done > p.bulk_idx_done;
      const bool cap_refuse = q.bulk_slot_stall > p.bulk_slot_stall;
      const bool ag_stall = q.bulk_idx_wait > p.bulk_idx_wait;
      const bool ag_cap = q.idx_stall_cap > p.idx_stall_cap;
      const bool out = q.bulk_tgt_issue > q.bulk_tgt_done;
      const bool xlate = q.bulk_defer > p.bulk_defer || q.winrefuse > p.winrefuse;
      int st = 5;
      if (tgt) {
        st = 3;
        ++nD;
        if (idx_moved) ++nDi;
      } else if (cap_refuse) {
        st = 1;
        ++nB;
        if (q.idx_ready > 0) ++nB_ready;
      } else if (q.tgt_backlog > 0) {
        st = 2;
        ++nC;
        if (xlate) ++nC_x;
        else if (q.ag_remain > 0 && !q.win_fill) ++nC_nf;
        else ++nC_ot;
      } else if (ag_stall && ag_cap) {
        st = 0;
        ++nA;
      } else if (idx_moved && out) {
        st = 4;
        ++nE;
      } else {
        ++nF;
        if (ag_stall) ++nF_idx;
        else if (!idx_moved && out) ++nF_out;
        else ++nF_win;
      }
      if (q.ag_remain > 0) {
        ++nAgWork;
        if (!q.win_fill) ++nNoFill;
      }
      if (q.simt_active) {
        if (st == 0) ++oA; else if (st == 1) ++oB; else if (st == 2) ++oC;
        else if (st == 3) ++oD; else if (st == 4) ++oE; else ++oF;
      }
      if (st != last) {
        if (st == 0) ++rA; else if (st == 1) ++rB; else if (st == 2) ++rC;
        else if (st == 3) ++rD; else if (st == 4) ++rE; else ++rF;
        last = st;
      }
    }
    const u64 tot = nA + nB + nC + nD + nE + nF;
    os << "\n[14] readiness vs target progress (C, engine-active cycles, mutually exclusive)\n";
    std::snprintf(line, sizeof line,
                  "  %-46s %9s %8s %8s %9s %9s\n", "state", "cycles", "% run", "% SIMT",
                  "episodes", "mean len");
    os << line;
    auto st_row = [&](const char* nm, u64 n, u64 runs, u64 overlap) {
      std::snprintf(line, sizeof line, "  %-46s %9llu %7.1f%% %7.1f%% %9llu %9.1f\n", nm,
                    static_cast<unsigned long long>(n),
                    tot ? 100.0 * double(n) / double(tot) : 0.0,
                    oA + oB + oC + oD + oE + oF
                        ? 100.0 * double(overlap) / double(oA + oB + oC + oD + oE + oF)
                        : 0.0,
                    static_cast<unsigned long long>(runs),
                    runs ? double(n) / double(runs) : 0.0);
      os << line;
    };
    st_row("A  capacity free, no ready index element", nA, rA, oA);
    st_row("B  ready index, target capacity refused", nB, rB, oB);
    st_row("C  capacity free, sealed line waiting, no issue", nC, rC, oC);
    {
      auto sub_row = [&](const char* nm, u64 n) {
        std::snprintf(line, sizeof line, "  %-46s %9llu %7.1f%%\n", nm,
                      static_cast<unsigned long long>(n),
                      tot ? 100.0 * double(n) / double(tot) : 0.0);
        os << line;
      };
      sub_row("     C1 issue refused by translation (dTLB)", nC_x);
      sub_row("     C2 generator had work, no window to fill", nC_nf);
      sub_row("     C3 other (port budget / page-run install)", nC_ot);
    }
    st_row("D  target request issued", nD, rD, oD);
    st_row("E  index moved, target outstanding, none issued", nE, rE, oE);
    st_row("F  neither pipeline moved", nF, rF, oF);
    std::snprintf(line, sizeof line,
                  "  classified %llu cycles; %llu of them with both clients live; "
                  "D with an index move %llu; B with ready index %llu; "
                  "F: index-starved %llu, target-waiting %llu, window/organizer %llu\n",
                  static_cast<unsigned long long>(tot),
                  static_cast<unsigned long long>(oA + oB + oC + oD + oE + oF),
                  static_cast<unsigned long long>(nDi),
                  static_cast<unsigned long long>(nB_ready),
                  static_cast<unsigned long long>(nF_idx),
                  static_cast<unsigned long long>(nF_out),
                  static_cast<unsigned long long>(nF_win));
    os << line;
    std::snprintf(line, sizeof line,
                  "  generator had elements left in %llu cycles; in %llu of them no window "
                  "was accepting elements\n",
                  static_cast<unsigned long long>(nAgWork),
                  static_cast<unsigned long long>(nNoFill));
    os << line;

    // ---- [14b] cycle-bucket timeline, aligned to cycle zero ---------------
    const size_t NBK = 12;
    os << "\n[14b] cycle-bucket timeline (C, 12 equal buckets aligned to cycle 0)\n";
    std::snprintf(line, sizeof line,
                  "  %-12s %8s %8s %8s %8s %9s %9s %9s %9s %8s %8s\n", "bucket", "cycles",
                  "A", "B", "C", "targets", "done", "idx ret", "AGstall", "ready", "DRAMq");
    os << line;
    std::vector<u64> bA(NBK, 0), bB(NBK, 0), bC(NBK, 0), bT(NBK, 0), bD(NBK, 0),
        bI(NBK, 0), bS(NBK, 0), bR(NBK, 0), bQ(NBK, 0), bN(NBK, 0);
    for (size_t i = 1; i < tc.v.size(); ++i) {
      const CycleSample& q = tc.v[i];
      const CycleSample& p = tc.v[i - 1];
      const size_t bk = i * NBK / tc.v.size();
      ++bN[bk];
      const bool tgt = q.bulk_tgt_issue > p.bulk_tgt_issue;
      const bool idx_moved = q.idx_sub > p.idx_sub || q.bulk_idx_done > p.bulk_idx_done;
      const bool cap_refuse = q.bulk_slot_stall > p.bulk_slot_stall;
      const bool ag_stall = q.bulk_idx_wait > p.bulk_idx_wait;
      const bool ag_cap = q.idx_stall_cap > p.idx_stall_cap;
      int st = 5;
      if (q.bulk_active) {
        if (tgt) st = 3;
        else if (cap_refuse) st = 1;
        else if (q.tgt_backlog > 0) st = 2;
        else if (ag_stall && ag_cap) st = 0;
        else if (!(idx_moved && q.bulk_tgt_issue > q.bulk_tgt_done)) st = 5;
        else st = 4;
        if (st == 0) ++bA[bk];
        else if (st == 1) ++bB[bk];
        else if (st == 2) ++bC[bk];
        if (ag_stall) ++bS[bk];
      }
      bT[bk] += q.bulk_tgt_issue > p.bulk_tgt_issue ? q.bulk_tgt_issue - p.bulk_tgt_issue : 0;
      bD[bk] += q.bulk_tgt_done > p.bulk_tgt_done ? q.bulk_tgt_done - p.bulk_tgt_done : 0;
      bI[bk] += q.bulk_idx_done > p.bulk_idx_done ? q.bulk_idx_done - p.bulk_idx_done : 0;
      bR[bk] += q.idx_ready;
      bQ[bk] += q.dram_q;
    }
    for (size_t k = 0; k < NBK; ++k) {
      std::snprintf(line, sizeof line,
                    "  %5zu-%-6zu %8llu %8llu %8llu %8llu %9llu %9llu %9llu %9llu %8.1f %8.1f\n",
                    k * tc.v.size() / NBK, (k + 1) * tc.v.size() / NBK - 1,
                    static_cast<unsigned long long>(bN[k]),
                    static_cast<unsigned long long>(bA[k]),
                    static_cast<unsigned long long>(bB[k]),
                    static_cast<unsigned long long>(bC[k]),
                    static_cast<unsigned long long>(bT[k]),
                    static_cast<unsigned long long>(bD[k]),
                    static_cast<unsigned long long>(bI[k]),
                    static_cast<unsigned long long>(bS[k]),
                    bN[k] ? double(bR[k]) / double(bN[k]) : 0.0,
                    bN[k] ? double(bQ[k]) / double(bN[k]) : 0.0);
      os << line;
    }
  }

  // -------------------------------------------------------------------------
  // [15] target-memory scheduling
  // -------------------------------------------------------------------------
  os << "\n[15] target-memory scheduling (B vs C)\n";
  std::snprintf(line, sizeof line, "  %-42s %10s %10s\n", "metric", "B", "C");
  os << line;
  row2("target requests submitted", "bulk_tgt_req_issued");
  row2("target requests completed", "bulk_tgt_req_done");
  row2("DRAM reads", "dram_reads");
  row2("DRAM row hits", "dram_row_hits");
  row2("DRAM row conflicts", "dram_row_conflicts");
  row2("DRAM starved cycles", "dram_starved_cycles");
  row2("DRAM blocked cycles", "dram_blocked_cycles");
  row2("DRAM service decisions", "dram_svc_events");
  row2("eligible candidates, summed", "dram_elig_sum");
  row2("L1 reads", "l1_reads");
  row2("L1 hits", "l1_hits");
  row2("L2 reads", "l2_reads");
  row2("L2 hits", "l2_hits");
  row2("dTLB MSHR stall cycles", "tlb0_mshr_stalls");
  row2("dTLB refusals: parked", "tlb_refuse_parked");
  row2("dTLB refusals: MSHR full", "tlb_refuse_full");
  row2("page walks", "walks");
  {
    auto inflight_sum = [](const CycleTrace& t) {
      u64 s = 0;
      for (const auto& q : t.v) s += q.bulk_tgt_issue > q.bulk_tgt_done ? q.bulk_tgt_issue - q.bulk_tgt_done : 0;
      return s;
    };
    auto inflight_max = [](const CycleTrace& t) {
      u64 m = 0;
      for (const auto& q : t.v)
        if (q.bulk_tgt_issue > q.bulk_tgt_done && q.bulk_tgt_issue - q.bulk_tgt_done > m)
          m = q.bulk_tgt_issue - q.bulk_tgt_done;
      return m;
    };
    row2f("DRAM row-hit rate (%)", rate(r.b.s.get("dram_row_hits"), r.b.s.get("dram_reads")),
          rate(r.c.s.get("dram_row_hits"), r.c.s.get("dram_reads")));
    row2f("mean DRAM queue depth",
          cb ? double(r.b.s.get("dram_q_occ_sum")) / double(cb + 1) : 0.0,
          cc ? double(r.c.s.get("dram_q_occ_sum")) / double(cc + 1) : 0.0);
    row2u("peak DRAM queue depth", peak(tb, &CycleSample::dram_q), peak(tc, &CycleSample::dram_q));
    row2f("mean candidates per decision",
          r.b.s.get("dram_svc_events")
              ? double(r.b.s.get("dram_elig_sum")) / double(r.b.s.get("dram_svc_events"))
              : 0.0,
          r.c.s.get("dram_svc_events")
              ? double(r.c.s.get("dram_elig_sum")) / double(r.c.s.get("dram_svc_events"))
              : 0.0);
    row2u("peak candidates at one decision", r.b.s.get("dram_elig_max"),
          r.c.s.get("dram_elig_max"));
    row2f("mean targets in flight",
          tb.v.size() ? double(inflight_sum(tb)) / double(tb.v.size()) : 0.0,
          tc.v.size() ? double(inflight_sum(tc)) / double(tc.v.size()) : 0.0);
    row2u("peak targets in flight", inflight_max(tb), inflight_max(tc));
    row2f("targets submitted per cycle",
          cb ? double(r.b.s.get("bulk_tgt_req_issued")) / double(cb + 1) : 0.0,
          cc ? double(r.c.s.get("bulk_tgt_req_issued")) / double(cc + 1) : 0.0);
    row2f("target fills per cycle",
          cb ? double(r.b.s.get("bulk_tgt_req_done")) / double(cb + 1) : 0.0,
          cc ? double(r.c.s.get("bulk_tgt_req_done")) / double(cc + 1) : 0.0);
    row2f("mean hierarchy inflight", mean(tb, &CycleSample::mem_inflight),
          mean(tc, &CycleSample::mem_inflight));
    row2f("L1 hit rate (%)", rate(r.b.s.get("l1_hits"), r.b.s.get("l1_reads")),
          rate(r.c.s.get("l1_hits"), r.c.s.get("l1_reads")));
    row2f("L2 hit rate (%)", rate(r.b.s.get("l2_hits"), r.b.s.get("l2_reads")),
          rate(r.c.s.get("l2_hits"), r.c.s.get("l2_reads")));
    // M12: MSHR occupancy, so "does a larger window push the caches harder?" is
    // answered from a measured peak rather than from the request count alone.
    // Display only; every counter below already existed.
    row2("L1 MSHR peak occupancy", "l1_mshr_max");
    row2("L2 MSHR peak occupancy", "l2_mshr_max");
    row2("L1 MSHR allocations", "l1_mshr_allocs");
    row2("L2 MSHR allocations", "l2_mshr_allocs");
    row2("L1 stall cycles", "l1_stall_cycles");
  }

  // -------------------------------------------------------------------------
  // [16] window management (E0-E4)
  // -------------------------------------------------------------------------
  // Every row is a recorded event or an aggregate of the per-window records:
  // allocation, admission, seal and release cycles, taken at those events. The
  // window manager holds one *incomplete* window by construction (single `fill_`
  // cursor), so "peak live windows" is an observation of the fill/drain overlap,
  // not of the number of incomplete windows.
  os << "\n[16] window management (policy, sizes, live windows, waiting)\n";
  std::snprintf(line, sizeof line, "  %-42s %10s %10s\n", "metric", "B", "C");
  os << line;
  row2u("cycles", cb, cc);
  row2("policy (E0=0, E2=1, E3=2, E4=3)", "bulk_win_policy");
  row2("partial-seal deadline (cycles)", "bulk_win_deadline");
  row2("live-window limit", "bulk_win_buf_limit");
  row2("windows adopted for filling", "bulk_win_alloc");
  row2("windows sealed (non-empty)", "bulk_win_sealed");
  row2("  sealed full", "bulk_seals_full");
  row2("  sealed partial (deadline/policy)", "bulk_win_partial");
  row2("  sealed at descriptor end", "bulk_seals_end");
  row2("  discarded empty", "bulk_win_empty_discarded");
  row2("elements per sealed window: mean", "bulk_win_size_mean");
  row2("elements per sealed window: median", "bulk_win_size_median");
  row2("elements per sealed window: min", "bulk_win_size_min");
  row2("elements per sealed window: max", "bulk_win_size_max");
  {
    // M12: buckets follow W/8, so the labels stay true when `batch_window` is
    // swept. At W = 8 192 the step is 1 024 and the labels are unchanged.
    const u64 step = std::max<u64>(1, r.cfg.batch_window / 8);
    for (u32 k = 0; k < 9; ++k) {
      char lab[64];
      if (k < 8)
        std::snprintf(lab, sizeof lab, "  fill-size bucket [%llu,%llu)",
                      static_cast<unsigned long long>(k * step),
                      static_cast<unsigned long long>((k + 1) * step));
      else
        std::snprintf(lab, sizeof lab, "  fill-size bucket [%llu,full]",
                      static_cast<unsigned long long>(7 * step));
      row2(lab, ("bulk_win_hist_" + std::to_string(k)).c_str());
    }
  }
  row2("allocation -> seal: mean", "bulk_win_age_mean");
  row2("allocation -> seal: max", "bulk_win_age_max");
  row2("allocation -> first admission: mean", "bulk_win_first_wait_mean");
  row2("allocation -> first admission: max", "bulk_win_first_wait_max");
  row2("first -> last admission: mean span", "bulk_win_fill_span_mean");
  row2("first -> last admission: max span", "bulk_win_fill_span_max");
  row2("partial seals: mean allocation -> seal", "bulk_win_partial_age_mean");
  row2("sealed -> processing start: mean", "bulk_win_issue_wait_mean");
  row2("sealed -> processing start: max", "bulk_win_issue_wait_max");
  row2("processing span (start -> release): mean", "bulk_win_proc_mean");
  row2("processing span (start -> release): max", "bulk_win_proc_max");
  row2("  of which issuing: mean", "bulk_win_issue_span_mean");
  row2("  of which issuing: max", "bulk_win_issue_span_max");
  row2("  of which draining: mean", "bulk_win_drain_mean");
  row2("  of which draining: max", "bulk_win_drain_max");
  row2("cycles sealed but nothing adopting", "bulk_win_issue_idle");
  row2("peak simultaneously unsealed windows", "bulk_win_incomplete_max");
  row2("peak simultaneously live windows (any state)", "bulk_win_live_max");
  row2("peak simultaneously sealed/draining windows", "bulk_win_drain_max");
  row2("cycles with two live windows", "bulk_win_live_cycles");
  row2("cycles with work but no filling window", "bulk_win_fill_wait");
  row2("  of which held back by the policy", "bulk_win_fill_blocked");
  row2("model window SRAM (bytes)", "bulk_win_bytes");
  row2("realized batch storage (bytes, vectors)", "bulk_win_vec_bytes");
  {
    // The individual windows, in allocation order: size, allocation, seal and
    // release cycles. why: 0 full, 1 descriptor end, 2 partial, 3 discarded.
    for (int arm = 0; arm < 2; ++arm) {
      const RunResult& x = arm ? r.c : r.b;
      const u64 nrec = x.s.get("bulk_win_recs");
      os << "  " << (arm ? "C" : "B")
         << " windows (n/alloc/seal/issue/lastissue/retire):";
      for (u64 k = 0; k < nrec; ++k) {
        const std::string p = "bulk_win_r" + std::to_string(k) + "_";
        std::snprintf(line, sizeof line, " [%llu/%llu/%llu/%llu/%llu/%llu]",
                      static_cast<unsigned long long>(x.s.get(p + "n")),
                      static_cast<unsigned long long>(x.s.get(p + "alloc")),
                      static_cast<unsigned long long>(x.s.get(p + "seal")),
                      static_cast<unsigned long long>(x.s.get(p + "issue")),
                      static_cast<unsigned long long>(x.s.get(p + "last_issue")),
                      static_cast<unsigned long long>(x.s.get(p + "retire")));
        os << line;
      }
      os << "\n";
    }
  }

  os << "\n[16b] critical-path context under this policy (B vs C)\n";
  std::snprintf(line, sizeof line, "  %-42s %10s %10s\n", "metric", "B", "C");
  os << line;
  row2("index lines submitted", "bulk_idx_sub");
  row2("index elements consumed", "bulk_idx_consumed");
  row2("AG stalls with target capacity free", "bulk_idx_wait_cap");
  row2("AG stalls (all)", "bulk_idx_wait_stall");
  row2("target refusals: pool empty", "bulk_tgt_refuse_noslot");
  row2("target refusals: index reserve", "bulk_tgt_refuse_reserve");
  row2("pool slots", "slot_pool_slots");
  row2("peak bulk residency", "bulk_peak_slot");
  row2("peak SIMT residency", "simt_peak_slot");
  row2("peak total outstanding", "peak_total_outstanding");
  row2("dedup merged elements", "bulk_dedup_merged");
  row2("target lines issued", "bulk_lines_issued");
  row2("sealed lines waiting for a slot (last)", "bulk_win_backlog");
  row2("DRAM reads", "dram_reads");
  row2("DRAM row hits", "dram_row_hits");
  row2("DRAM starved cycles", "dram_starved_cycles");
  row2("page walks", "walks");
  row2("replays", "replays");
  {
    auto inflight_sum = [](const CycleTrace& t) {
      u64 s = 0;
      for (const auto& q : t.v)
        s += q.bulk_tgt_issue > q.bulk_tgt_done ? q.bulk_tgt_issue - q.bulk_tgt_done : 0;
      return s;
    };
    row2f("DRAM row-hit rate (%)", rate(r.b.s.get("dram_row_hits"), r.b.s.get("dram_reads")),
          rate(r.c.s.get("dram_row_hits"), r.c.s.get("dram_reads")));
    row2u("peak DRAM queue depth", peak(tb, &CycleSample::dram_q), peak(tc, &CycleSample::dram_q));
    row2f("mean DRAM queue depth", mean(tb, &CycleSample::dram_q), mean(tc, &CycleSample::dram_q));
    row2f("mean targets in flight",
          tb.v.size() ? double(inflight_sum(tb)) / double(tb.v.size()) : 0.0,
          tc.v.size() ? double(inflight_sum(tc)) / double(tc.v.size()) : 0.0);
    row2f("mean candidates per DRAM decision",
          r.b.s.get("dram_svc_events")
              ? double(r.b.s.get("dram_elig_sum")) / double(r.b.s.get("dram_svc_events"))
              : 0.0,
          r.c.s.get("dram_svc_events")
              ? double(r.c.s.get("dram_elig_sum")) / double(r.c.s.get("dram_svc_events"))
              : 0.0);
    row2f("mean index lines ready", mean(tb, &CycleSample::idx_ready_lines),
          mean(tc, &CycleSample::idx_ready_lines));
    row2u("peak index lines ready", peak(tb, &CycleSample::idx_ready_lines),
          peak(tc, &CycleSample::idx_ready_lines));
  }

  // -------------------------------------------------------------------------
  // [18] M13: bounded cross-window reuse buffer
  // -------------------------------------------------------------------------
  // Every row is a counter the engine maintained at the event it describes
  // (lookup at seal, publish at fill, evict at allocation). A run without the
  // mechanism prints zeros here and nothing else changes.
  os << "\n[18] M13 cross-window reuse buffer (2 x 8K windows + bounded directory)\n";
  std::snprintf(line, sizeof line, "  %-44s %10s %10s\n", "metric", "B", "C");
  os << line;
  if (!r.cfg.reuse_dir) {
    os << "  mechanism off: M10's two 8K windows and nothing else\n";
  } else {
    row2("sets", "bulk_reuse_sets");
    row2("ways", "bulk_reuse_ways");
    row2("entries (sets x ways)", "bulk_reuse_entries");
    row2("in-flight merge enabled", "bulk_reuse_merge");
    row2("control cost charged", "bulk_reuse_charge");
    row2("distinct lines looked up", "bulk_reuse_lines_planned");
    row2("lookups performed", "bulk_reuse_probes");
    row2("  served from retained data", "bulk_reuse_hit_ready");
    row2("  merged into an outstanding request", "bulk_reuse_hit_inflight");
    row2("  had to issue (miss)", "bulk_reuse_miss");
    row2("requests avoided (lines)", "bulk_reuse_req_avoided");
    row2("elements retired without a request", "bulk_reuse_elems");
    row2("  from retained data", "bulk_reuse_elems_ready");
    row2("  by a merged fill", "bulk_reuse_elems_merged");
    row2("lookups bypassed: no evictable entry", "bulk_reuse_full");
    row2("lookups bypassed: fanout group full", "bulk_reuse_sub_full");
    row2("entries published with data", "bulk_reuse_inserts");
    row2("entries evicted", "bulk_reuse_evict");
    row2("entries dropped at abort", "bulk_reuse_dropped");
    row2("peak valid entries", "bulk_reuse_occ_max");
    row2("peak dependants on one entry", "bulk_reuse_sub_max");
    row2("modelled SRAM (bytes)", "bulk_reuse_bytes");
    row2("realized storage (bytes, vectors)", "bulk_reuse_vec_bytes");
    row2f("retained-data hit rate (% of lookups)",
          rate(r.b.s.get("bulk_reuse_hit_ready"), r.b.s.get("bulk_reuse_probes")),
          rate(r.c.s.get("bulk_reuse_hit_ready"), r.c.s.get("bulk_reuse_probes")));
    row2f("any-hit rate (% of lookups, retained + merged)",
          rate(r.b.s.get("bulk_reuse_req_avoided"), r.b.s.get("bulk_reuse_probes")),
          rate(r.c.s.get("bulk_reuse_req_avoided"), r.c.s.get("bulk_reuse_probes")));
    row2f("directory SRAM as % of a 2 x 32K window pair",
          rate(r.b.s.get("bulk_reuse_bytes"), 2ull * 32768 * 8),
          rate(r.c.s.get("bulk_reuse_bytes"), 2ull * 32768 * 8));
  }
}

}  // namespace dx100
