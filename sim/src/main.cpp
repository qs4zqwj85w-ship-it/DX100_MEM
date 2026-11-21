#include "sim.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace dx100;

namespace {
void usage() {
  std::printf(
      "dx100sim — SIMT baseline memory-system simulator\n"
      "usage: dx100sim [--config FILE] [--set k=v]... [--all] [--quiet]\n"
      "  --config FILE  load key=value configuration (later --set overrides)\n"
      "  experiment=ab|ab_sweep|m6|m6_sweep selects a paired A/B driver\n"
      "  --set k=v      override one setting (repeatable)\n"
      "  --all          print every counter\n"
      "  --quiet        print only the summary line and status\n");
}
}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  std::string cfgpath;
  std::vector<std::string> overrides;
  bool all = false, quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      cfgpath = argv[++i];
    } else if (a == "--set" && i + 1 < argc) {
      overrides.push_back(argv[++i]);
    } else if (a == "--all") {
      all = true;
    } else if (a == "--quiet") {
      quiet = true;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  try {
    if (!cfgpath.empty()) cfg = Config::load(cfgpath);
    for (const auto& kv : overrides) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "bad --set (want k=v): %s\n", kv.c_str());
        return 2;
      }
      cfg.set(kv.substr(0, eq), kv.substr(eq + 1));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 2;
  }

  // M5.1 experiment drivers: the paired A/B run and its residency sweep. These
  // are drivers, not front-ends, so they are selected separately from `mode`.
  if (cfg.experiment != "none") {
    std::ostringstream os;
    if (cfg.experiment == "ab_sweep") {
      const std::vector<AbResult> rs = run_ab_sweep(cfg);
      print_ab_sweep(rs, os);
      std::fputs(os.str().c_str(), stdout);
      bool ok = !rs.empty();
      for (const auto& r : rs) ok = ok && r.ok;
      std::printf("\n%s\n", ok ? "ALL PAIRS OK" : "VIOLATIONS PRESENT");
      return ok ? 0 : 1;
    }
    if (cfg.experiment == "m6_sweep") {
      const std::vector<AbResult> rs = run_m6_sweep(cfg);
      print_m6_sweep(rs, os);
      std::fputs(os.str().c_str(), stdout);
      bool ok = !rs.empty();
      for (const auto& r : rs) ok = ok && r.ok;
      std::printf("\n%s\n", ok ? "ALL CELLS OK" : "VIOLATIONS PRESENT");
      return ok ? 0 : 1;
    }
    if (cfg.experiment == "diag") {
      const DiagResult r = run_index_oracle_diag(cfg);
      print_index_oracle_diag(r, os);
      std::fputs(os.str().c_str(), stdout);
      return r.ok ? 0 : 1;
    }
    if (cfg.experiment == "m9" || cfg.experiment == "m9diag") {
      const bool diag = cfg.experiment == "m9diag";
      CycleTrace ta, tb, tc;
      const M9Result r = diag ? run_m9(cfg, &ta, &tb, &tc) : run_m9(cfg);
      print_m9(r, os);
      if (diag) print_m9_diag(r, ta, tb, tc, os);
      std::fputs(os.str().c_str(), stdout);
      return r.ok ? 0 : 1;
    }
    const AbResult r = run_ab(cfg);
    print_ab(r, os);
    std::fputs(os.str().c_str(), stdout);
    return r.ok ? 0 : 1;
  }

  Sim sim(cfg);
  const RunResult r = sim.run();

  if (!quiet) {
    std::printf("== configuration ==\n%s\n\n", cfg.to_string().c_str());
    const Trace& t = sim.trace();
    std::printf("== realized workload ==\n");
    std::printf("accesses=%zu instrs=%u lines=%llu pages=%u kappa=%.4f rho_line=%.4f\n\n",
                t.acc.size(), t.n_instr, static_cast<unsigned long long>(t.lines_real),
                t.pages_real, t.kappa_real, t.rho_line_real);
    std::printf("== summary ==\n");
    std::printf("cycles=%llu lines_per_cycle=%.3f dram_gbs=%.2f\n",
                static_cast<unsigned long long>(r.cycles), r.lines_per_cycle, r.gbs);
    std::printf("l1 hit_rate=%.3f  l2 hit_rate=%.3f  tlb0 hit_rate=%.3f\n",
                r.s.get("l1_reads") ? double(r.s.get("l1_hits")) / r.s.get("l1_reads") : 0.0,
                r.s.get("l2_reads") ? double(r.s.get("l2_hits")) / r.s.get("l2_reads") : 0.0,
                r.s.get("tlb0_reads") ? double(r.s.get("tlb0_hits")) / r.s.get("tlb0_reads") : 0.0);
    std::printf("walks=%llu pte_fetches=%llu walk_stalls=%llu replays=%llu\n\n",
                static_cast<unsigned long long>(r.s.get("walks")),
                static_cast<unsigned long long>(r.s.get("pte_fetches")),
                static_cast<unsigned long long>(r.s.get("l2tlb_walk_stalls")),
                static_cast<unsigned long long>(r.s.get("replays")));
    if (cfg.mode == Mode::Bulk) {
      std::printf("== bulk engine ==\n");
      std::printf("desc submitted=%llu completed=%llu rejected=%llu latency_mean=%llu max=%llu\n",
                  static_cast<unsigned long long>(r.s.get("bulk_desc_submitted")),
                  static_cast<unsigned long long>(r.s.get("bulk_desc_completed")),
                  static_cast<unsigned long long>(r.s.get("bulk_desc_rejected")),
                  static_cast<unsigned long long>(r.s.get("bulk_desc_cycles_mean")),
                  static_cast<unsigned long long>(r.s.get("bulk_desc_cycles_max")));
      std::printf("index: lines=%llu (ooo=%llu, max_out=%llu) elems=%llu bytes=%llu\n",
                  static_cast<unsigned long long>(r.s.get("bulk_idx_lines_req")),
                  static_cast<unsigned long long>(r.s.get("bulk_idx_lines_ooo")),
                  static_cast<unsigned long long>(r.s.get("bulk_idx_out_max")),
                  static_cast<unsigned long long>(r.s.get("bulk_idx_elems_done")),
                  static_cast<unsigned long long>(r.s.get("bulk_idx_line_bytes")));
      std::printf("target: addrs=%llu req=%llu done=%llu (ooo=%llu) addr_mismatch=%llu bytes=%llu\n",
                  static_cast<unsigned long long>(r.s.get("bulk_tgt_addr_gen")),
                  static_cast<unsigned long long>(r.s.get("bulk_tgt_req_gen")),
                  static_cast<unsigned long long>(r.s.get("bulk_tgt_req_done")),
                  static_cast<unsigned long long>(r.s.get("bulk_tgt_ooo")),
                  static_cast<unsigned long long>(r.s.get("bulk_addr_mismatch")),
                  static_cast<unsigned long long>(r.s.get("bulk_tgt_line_bytes")));
      std::printf("buffer: max=%llu unissued_max=%llu full_stall=%llu issue_stall=%llu "
                  "idx_wait_stall=%llu ag_util=%.1f%%\n",
                  static_cast<unsigned long long>(r.s.get("bulk_buf_max")),
                  static_cast<unsigned long long>(r.s.get("bulk_unissued_max")),
                  static_cast<unsigned long long>(r.s.get("bulk_buf_full_stall")),
                  static_cast<unsigned long long>(r.s.get("bulk_issue_stall")),
                  static_cast<unsigned long long>(r.s.get("bulk_idx_wait_stall")),
                  r.s.get("bulk_ag_util_x1000") / 10.0);
      for (const auto& e : sim.engine().errors()) std::printf("descriptor error: %s\n", e.c_str());
      std::printf("\n");
    }
  }

  if (all) {
    std::printf("== counters ==\n");
    std::ostringstream os;
    r.s.dump(os, "");
    std::fputs(os.str().c_str(), stdout);
  }

  std::printf("conservation=%s", r.ok ? "OK" : "VIOLATED");
  for (const auto& v : r.violations) std::printf(" [%s]", v.c_str());
  std::printf("\n");
  return r.ok ? 0 : 1;
}
