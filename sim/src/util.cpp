#include "sim.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dx100 {

namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

}  // namespace

void Config::set(const std::string& key_in, const std::string& value) {
  std::string key = key_in;
  for (auto& ch : key) ch = static_cast<char>(::tolower(ch));
  auto num = [&]() { return static_cast<u32>(std::stoul(value)); };
  auto dbl = [&]() { return std::stod(value); };

  if (key == "cores") cores = num();
  else if (key == "warps_per_core") warps_per_core = num();
  else if (key == "lanes") lanes = num();
  else if (key == "r") R = num();
  else if (key == "l1_lines") l1_lines = num();
  else if (key == "l1_ways") l1_ways = num();
  else if (key == "l1_latency") l1_latency = num();
  else if (key == "l1_mshr") l1_mshr = num();
  else if (key == "l2_lines") l2_lines = num();
  else if (key == "l2_ways") l2_ways = num();
  else if (key == "l2_latency") l2_latency = num();
  else if (key == "l2_mshr") l2_mshr = num();
  else if (key == "dtlb_entries") dtlb_entries = num();
  else if (key == "dtlb_ways") dtlb_ways = num();
  else if (key == "dtlb_mshr") dtlb_mshr = num();
  else if (key == "dtlb_parked") dtlb_parked = num();
  else if (key == "l2tlb_entries") l2tlb_entries = num();
  else if (key == "l2tlb_ways") l2tlb_ways = num();
  else if (key == "l2tlb_mshr") l2tlb_mshr = num();
  else if (key == "l2tlb_latency") l2tlb_latency = num();
  else if (key == "walkers") walkers = num();
  else if (key == "walk_cache") walk_cache = num();
  else if (key == "walk_latency_per_level") walk_latency_per_level = num();
  else if (key == "levels") levels = num();
  else if (key == "ideal_translation") ideal_translation = (value != "0" && value != "false");
  else if (key == "banks") banks = num();
  else if (key == "rows") rows = num();
  else if (key == "row_bytes") row_bytes = num();
  else if (key == "dram_il_lines") dram_il_lines = num();
  else if (key == "t_hit") t_hit = num();
  else if (key == "t_miss") t_miss = num();
  else if (key == "link") link = num();
  else if (key == "dram_q") dram_q = num();
  // M24-B: page size and the HBM hierarchy. Every key defaults to the
  // historical value, so an old config file parses to the old model.
  else if (key == "page_bytes") page_bytes = num();
  else if (key == "dram_kind") {
    if (value != "ddr" && value != "hbm")
      throw std::runtime_error("dram_kind must be ddr or hbm");
    dram_kind = value;
  }
  else if (key == "hbm_stacks") hbm_stacks = num();
  else if (key == "hbm_pcs") hbm_pcs = num();
  else if (key == "hbm_bank_groups") hbm_bank_groups = num();
  else if (key == "hbm_banks_pg") hbm_banks_pg = num();
  else if (key == "hbm_capacity") hbm_capacity = num();
  else if (key == "hbm_row_bytes") hbm_row_bytes = num();
  else if (key == "hbm_map") hbm_map = num();
  else if (key == "hbm_base_map") hbm_base_map = num();
  else if (key == "hbm_perm_seed") hbm_perm_seed = num();
  else if (key == "hbm_perm_page") hbm_perm_page = num();
  else if (key == "hbm_il_lines") hbm_il_lines = num();
  else if (key == "hbm_t_rcd") hbm_t_rcd = num();
  else if (key == "hbm_t_rp") hbm_t_rp = num();
  else if (key == "hbm_t_ras") hbm_t_ras = num();
  else if (key == "hbm_t_cas") hbm_t_cas = num();
  else if (key == "hbm_t_ccd_s") hbm_t_ccd_s = num();
  else if (key == "hbm_t_ccd_l") hbm_t_ccd_l = num();
  else if (key == "hbm_t_rrd_s") hbm_t_rrd_s = num();
  else if (key == "hbm_t_rrd_l") hbm_t_rrd_l = num();
  else if (key == "hbm_t_burst") hbm_t_burst = num();
  else if (key == "hbm_t_faw") hbm_t_faw = num();
  else if (key == "hbm_t_refi") hbm_t_refi = num();
  else if (key == "hbm_t_rfc") hbm_t_rfc = num();
  else if (key == "frfcfs") frfcfs = (value != "0" && value != "false");
  else if (key == "dram_diag") dram_diag = (value != "0" && value != "false");
  else if (key == "dram_map") dram_map = num();  // M11.3 diagnostic maps only
  else if (key == "bank_hash") bank_hash = (value != "0" && value != "false");
  else if (key == "lsu_pending") lsu_pending = num();
  else if (key == "core_map") core_map = value;
  else if (key == "mode")
    mode = (value == "neutral") ? Mode::Neutral : (value == "bulk" ? Mode::Bulk : Mode::Baseline);
  else if (key == "ag_lanes") ag_lanes = num();
  else if (key == "eng_issue") eng_issue = num();
  else if (key == "idx_window") idx_window = num();
  else if (key == "mem_path") {
    if (value != "native" && value != "l1" && value != "l2")
      throw std::runtime_error("mem_path must be native, l1 or l2");
    mem_path = value;
  } else if (key == "experiment") {
    if (value != "none" && value != "ab" && value != "ab_sweep" && value != "m6" &&
        value != "m6_sweep" && value != "diag" && value != "m7" && value != "m7_sweep" &&
        value != "m9" && value != "m9diag")
      throw std::runtime_error(
          "experiment must be none, ab, ab_sweep, m6, m6_sweep, diag, m7, m7_sweep, m9 or "
          "m9diag");
    experiment = value;
  }
  else if (key == "desc_fetch") desc_fetch = (value != "0" && value != "false");
  else if (key == "idx_oracle") idx_oracle = (value != "0" && value != "false");
  else if (key == "batch_window") batch_window = num();
  else if (key == "page_dir") page_dir = num();
  else if (key == "org_lanes") org_lanes = num();
  else if (key == "fanout_lanes") fanout_lanes = num();
  else if (key == "batch_deadline") batch_deadline = num();
  else if (key == "org_overlap") org_overlap = (value != "0" && value != "false");
  else if (key == "win_policy") win_policy = num();
  else if (key == "win_abs_cap") win_abs_cap = num();
  else if (key == "bulk_stream") bulk_stream = (value != "0" && value != "false");
  else if (key == "dedup") dedup = (value != "0" && value != "false");
  else if (key == "page_order") page_order = (value != "0" && value != "false");
  else if (key == "bulk_org") bulk_org = num();
  else if (key == "g6a_pa_capacity") g6a_pa_capacity = num();
  else if (key == "g6b_pa_capacity") g6b_pa_capacity = num();
  else if (key == "sched_vis") sched_vis = num();
  else if (key == "sched_group") sched_group = num();
  else if (key == "g6i_diag") g6i_diag = num();
  else if (key == "g6i_sample_period") g6i_sample_period = num();
  // M24-A: the four-way factors and the mapping experiments.
  else if (key == "m24_map") m24_map = num();
  else if (key == "m24_perm_seed") m24_perm_seed = num();
  else if (key == "m24_dram_org") m24_dram_org = (value != "0" && value != "false");
  else if (key == "eng_core") eng_core = static_cast<u32>(std::stoul(value));
  else if (key == "page_sched") page_sched = (value != "0" && value != "false");
  else if (key == "page_cohorts") page_cohorts = num();
  else if (key == "park_retry") park_retry = num();
  else if (key == "idx_mode") idx_mode = num();
  else if (key == "idx_buf_lines") idx_buf_lines = num();
  else if (key == "idx_mshr") idx_mshr = num();
  else if (key == "idx_reserve") idx_reserve = num();
  // ---- M13: bounded cross-window reuse buffer (off unless reuse_dir = 1) ----
  else if (key == "reuse_dir") reuse_dir = (value != "0" && value != "false");
  else if (key == "reuse_sets") reuse_sets = num();
  else if (key == "reuse_ways") reuse_ways = num();
  else if (key == "reuse_hash") reuse_hash = (value != "0" && value != "false");
  else if (key == "reuse_merge") reuse_merge = (value != "0" && value != "false");
  else if (key == "reuse_charge") reuse_charge = (value != "0" && value != "false");
  else if (key == "m13_dump") m13_dump = value;
  else if (key == "m23_select") m23_select = value;  // M23: selection audit trail
  else if (key == "dispatch") dispatch = num();
  else if (key == "op_class") {
    if (value != "gen" && value != "pattern" && value != "simt" && value != "bulk")
      throw std::runtime_error("op_class must be gen, pattern, simt or bulk");
    op_class = value;
  }
  else if (key == "dispatch_order") dispatch_order = num();
  else if (key == "pool_slots") pool_slots = num();
  // ---- M15: diagnostic attribution and the fused window prototype ----------
  else if (key == "cache_diag") cache_diag = (value != "0" && value != "false");
  else if (key == "win_fused") win_fused = (value != "0" && value != "false");
  else if (key == "ab_charge_latch") ab_charge_latch = (value != "0" && value != "false");
  else if (key == "bulk_max_elems") bulk_max_elems = num();
  else if (key == "bulk_wl") bulk_wl = value;
  else if (key == "bulk_index_base") bulk_index_base = std::stoull(value, nullptr, 0);
  else if (key == "bulk_data_base") bulk_data_base = std::stoull(value, nullptr, 0);
  else if (key == "bulk_desc_line") bulk_desc_line = std::stoull(value, nullptr, 0);
  else if (key == "wl") wl = value;
  else if (key == "dataset") dataset = value;  // M18: file-driven access trace
  else if (key == "skip_b") skip_b = (value != "0" && value != "false");
  else if (key == "n") N = num();
  else if (key == "lambda") Lambda = num();
  else if (key == "p") P = num();
  else if (key == "kappa") kappa = dbl();
  else if (key == "rho_dup") rho_dup = dbl();
  else if (key == "seed") seed = std::stoull(value);
  else if (key == "max_cycles") max_cycles = std::stoull(value);
  // M26-B1 diagnostics. Not echoed by `to_string`, so a normal run's header
  // line is byte-identical to the one every published cell produced.
  else if (key == "life_diag") life_diag = (value != "0" && value != "false");
  else if (key == "life_dump") life_dump = value;
  else if (key == "stall_abort") stall_abort = std::stoull(value);
  else if (key == "elem_bytes") elem_bytes = num();
  else throw std::runtime_error("unknown config key: " + key_in);
}

Config Config::load(const std::string& path) {
  Config cfg;
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open config: " + path);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) throw std::runtime_error("bad config line: " + line);
    cfg.set(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
  }
  return cfg;
}

u32 Config::page_shift() const {
  if (page_bytes == 65536) return 16;
  if (page_bytes == 2097152) return 21;
  return 12;
}
u32 Config::page_line_bits() const { return page_shift() - 6; }

std::string Config::to_string() const {
  std::ostringstream os;
  os << "cores=" << cores << " warps=" << warps_per_core << " lanes=" << lanes
     << " R=" << R << "\n"
     << "l1=" << l1_lines << "x" << l1_ways << "/" << l1_latency << "cy mshr=" << l1_mshr << "\n"
     << "l2=" << l2_lines << "x" << l2_ways << "/" << l2_latency << "cy mshr=" << l2_mshr << "\n"
     << "dtlb=" << dtlb_entries << "x" << dtlb_ways << " mshr=" << dtlb_mshr
     << " parked=" << dtlb_parked << "\n"
     << "l2tlb=" << l2tlb_entries << "x" << l2tlb_ways << " mshr=" << l2tlb_mshr
     << " lat=" << l2tlb_latency << "\n"
     << "walkers=" << walkers << " wc=" << walk_cache
     << " walk_lat=" << walk_latency_per_level << "x" << levels
     << (ideal_translation ? " ideal_translation" : "") << "\n"
     << "dram=" << banks << " banks x " << rows << " rows, hit=" << t_hit
     << " miss=" << t_miss << " link=" << link << " q=" << dram_q
     << (frfcfs ? " frfcfs" : " fifo") << (bank_hash ? " hash" : " linear")
     << " lsu_pending=" << lsu_pending << " core_map=" << core_map << "\n"
     // M23-G4C: the address-map identity, on its own line so a log can be
     // checked against the mapping it claims to have run. `capacity_mib` is the
     // row-index period as well: rows wrap after banks x rows x row_bytes.
     << "dram_map=" << dram_map << " bank_hash=" << (bank_hash ? 1 : 0)
     << " row_bytes=" << row_bytes << " dram_il_lines=" << dram_il_lines
     << " capacity_mib="
     << ((static_cast<u64>(banks) * rows * row_bytes) >> 20)
     // M24-A: the mapping experiment and the DRAM-organization factor, on the
     // same identity line, so a log states which four-way cell and which map it
     // ran. `m24_map` 0 and `m24_dram_org` 1 are the shipped behaviour.
     << " m24_map=" << m24_map << " m24_dram_org=" << (m24_dram_org ? 1 : 0)
     << " m24_perm_seed=" << m24_perm_seed
     // M24-B: page size and the memory model, on the same identity line, so a
     // log states which translation geometry and which DRAM model it ran.
     << " page_bytes=" << page_bytes << " page_shift=" << page_shift()
     << " dram_kind=" << dram_kind << "\n"
     << "hbm=" << hbm_stacks << " stacks x " << hbm_pcs << " pcs x " << hbm_bank_groups
     << " bg x " << hbm_banks_pg << " banks, capacity=" << (hbm_capacity >> 20)
     << " MiB row_bytes=" << hbm_row_bytes << " map=" << hbm_map
     << " base=" << hbm_base_map << " seed=" << hbm_perm_seed
     << " perm_page=" << hbm_perm_page << " il_lines=" << hbm_il_lines << " t_rcd=" << hbm_t_rcd << " t_rp=" << hbm_t_rp
     << " t_ras=" << hbm_t_ras << " t_cas=" << hbm_t_cas << " t_ccd=" << hbm_t_ccd_s
     << "/" << hbm_t_ccd_l << " t_rrd=" << hbm_t_rrd_s << "/" << hbm_t_rrd_l
     << " t_burst=" << hbm_t_burst << " t_faw=" << hbm_t_faw
     << " t_refi=" << hbm_t_refi << " t_rfc=" << hbm_t_rfc << "\n"
     << "workload=" << wl << " N=" << N << " Lambda=" << Lambda << " P=" << P
     << " kappa=" << kappa << " rho_dup=" << rho_dup << " seed=" << seed << "\n"
     << "front_end=" << (mode == Mode::Bulk ? "bulk" : (mode == Mode::Neutral ? "neutral" : "simt"))
     << " ag_lanes=" << ag_lanes << " eng_issue=" << eng_issue
     << " idx_window=" << (idx_window ? idx_window : R)
     << " desc_fetch=" << (desc_fetch ? 1 : 0) << " bulk_wl=" << bulk_wl
     << " mem_path=" << mem_path << " idx_oracle=" << (idx_oracle ? 1 : 0)
     << "\nwindow=" << (batch_window ? std::to_string(batch_window) : std::string("off"))
     << " page_dir=" << page_dir << " org_lanes=" << org_lanes
     << " fanout_lanes=" << fanout_lanes << " deadline=" << batch_deadline
     << " overlap=" << (org_overlap ? 1 : 0) << " dedup=" << (dedup ? 1 : 0)
     << " win_policy=" << effective_win_policy()
     << (win_policy == 0xffu ? "(default)" : "(set)")
     << " bulk_stream=" << (bulk_stream ? 1 : 0)
     << " page_order=" << (page_order ? 1 : 0)
     << " bulk_org=" << bulk_org << " g6a_pa_capacity=" << g6a_pa_capacity
     << " g6b_pa_capacity=" << g6b_pa_capacity
     // M23-G6F: printed only when set, so every historical log's configuration
     // block stays byte-identical when the knob is off.
     << (sched_vis ? " sched_vis=" + std::to_string(sched_vis) : std::string())
     // M23-G6G: printed only when set, on the same rule.
     << (sched_group ? " sched_group=" + std::to_string(sched_group) : std::string())
     // M23-G6I: the pipeline diagnostic, printed only when set, on the same rule.
     << (g6i_diag ? " g6i_diag=" + std::to_string(g6i_diag) : std::string())
     << " charge_latch=" << (ab_charge_latch ? 1 : 0)
     << "\ndispatch=" << dispatch << " op_class=" << op_class
     << " dispatch_order=" << dispatch_order
     << " pool_slots=" << (pool_slots ? std::to_string(pool_slots) : std::string("R"))
     << "\nreuse_dir=" << (reuse_dir ? 1 : 0)
     << (reuse_dir ? " entries=" + std::to_string(reuse_sets * reuse_ways) + " (" +
                        std::to_string(reuse_sets) + "x" + std::to_string(reuse_ways) + ")"
                  : std::string())
     << " merge=" << (reuse_merge ? 1 : 0) << " charge=" << (reuse_charge ? 1 : 0)
     << "\ncache_diag=" << (cache_diag ? 1 : 0) << " win_fused=" << (win_fused ? 1 : 0);
  return os.str();
}

void Stats::dump(std::ostream& os, const std::string& prefix) const {
  for (const auto& kv : c) os << prefix << kv.first << "=" << kv.second << "\n";
}

}  // namespace dx100
