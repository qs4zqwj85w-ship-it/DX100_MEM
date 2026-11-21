// M28-A: dispatch-only inspection harness.
//
// Loads a trace and builds the ClassMap exactly as Sim::run_hybrid does
// (build_op_classes), then prints the decision for every instruction. It never
// constructs a MemorySystem, a BulkEngine or an event: no request is issued, no
// cache or DRAM is touched, nothing is timed. This is a reader of the dispatch
// decision, not a simulator run.
//
// Build:
//   clang++ -std=c++17 -O2 -Isrc tools/m28a_dispatch.cpp \
//           src/util.cpp src/workload.cpp src/memory.cpp src/hbm.cpp \
//           src/bulk.cpp src/ab.cpp src/system.cpp -o bin/m28a_dispatch

#include "sim.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace dx100;

int main(int argc, char** argv) {
  Config cfg;
  std::string cfgpath;
  std::vector<std::string> overrides;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) cfgpath = argv[++i];
    else if (a == "--set" && i + 1 < argc) overrides.push_back(argv[++i]);
    else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
  }

  try {
    if (!cfgpath.empty()) cfg = Config::load(cfgpath);
    for (const auto& kv : overrides) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) { std::fprintf(stderr, "bad --set: %s\n", kv.c_str()); return 2; }
      cfg.set(kv.substr(0, eq), kv.substr(eq + 1));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 2;
  }

  if (cfg.dataset.empty()) { std::fprintf(stderr, "need --set dataset=FILE\n"); return 2; }

  // ---- exactly what run_hybrid does, before any request exists -------------
  const Trace tr = build_trace(cfg);
  const ClassMap cm = build_op_classes(cfg, tr);
  // Nothing below this line touches the model.

  const u32 n = tr.n_instr;
  std::vector<u32> nacc(n, 0);
  std::vector<u64> lo(n, ~0ull), hi(n, 0), first(n, ~0ull), last(n, 0);
  std::vector<std::set<u64>> lines(n), pages(n);
  u64 acc_in_range = 0;
  for (const auto& a : tr.acc) {
    if (a.instr >= n) continue;
    ++acc_in_range;
    ++nacc[a.instr];
    lo[a.instr] = std::min(lo[a.instr], a.addr);
    hi[a.instr] = std::max(hi[a.instr], a.addr);
    first[a.instr] = std::min(first[a.instr], a.addr);
    last[a.instr] = std::max(last[a.instr], a.addr);
    lines[a.instr].insert(a.addr / LINE_BYTES);
    pages[a.instr].insert(a.addr / (LINE_BYTES * cfg.page_lines()));
  }

  std::printf("# M28-A dispatch-only dump\n");
  std::printf("trace=%s lanes=%u elem_bytes=%u op_class=%s\n",
              cfg.dataset.c_str(), cfg.lanes, cfg.elem_bytes, cfg.op_class.c_str());
  std::printf("trace_instr=%u trace_acc=%zu kappa_real=%.4f\n", n, tr.acc.size(), tr.kappa_real);
  std::printf("%-6s %-6s %-5s %-6s %-6s %-6s %-14s %-14s\n",
              "instr", "class", "n_acc", "contig", "lines", "pages", "first_addr", "last_addr");

  u64 simt_i = 0, bulk_i = 0, simt_a = 0, bulk_a = 0, other_i = 0, empty_i = 0, contig_i = 0;
  for (u32 i = 0; i < n; ++i) {
    const u8 c = cm.at(i);
    const bool contig = nacc[i] > 1 && (hi[i] - lo[i]) == (u64)(nacc[i] - 1) * cfg.elem_bytes;
    if (c == static_cast<u8>(OpClass::Simt)) { ++simt_i; simt_a += nacc[i]; }
    else if (c == static_cast<u8>(OpClass::Bulk)) { ++bulk_i; bulk_a += nacc[i]; }
    else ++other_i;
    if (nacc[i] == 0) ++empty_i;
    if (contig) ++contig_i;
    std::printf("%-6u %-6s %-5u %-6d %-6zu %-6zu %-14llu %-14llu\n",
                i, c == static_cast<u8>(OpClass::Simt) ? "SIMT"
                   : (c == static_cast<u8>(OpClass::Bulk) ? "BULK" : "?"),
                nacc[i], contig ? 1 : 0, lines[i].size(), pages[i].size(),
                (unsigned long long)first[i], (unsigned long long)last[i]);
  }

  const bool instr_ok = (simt_i + bulk_i == n) && other_i == 0 && cm.cls.size() == n;
  const bool acc_ok = (simt_a + bulk_a == tr.acc.size()) && acc_in_range == tr.acc.size();
  std::printf("summary total_instr=%u simt_instr=%llu bulk_instr=%llu unclassified_instr=%llu "
              "instr_sum_ok=%d\n", n, (unsigned long long)simt_i, (unsigned long long)bulk_i,
              (unsigned long long)other_i, instr_ok ? 1 : 0);
  std::printf("summary acc_total=%zu simt_acc=%llu bulk_acc=%llu acc_sum_ok=%d "
              "instr_without_acc=%llu contiguous_instr=%llu\n",
              tr.acc.size(), (unsigned long long)simt_a, (unsigned long long)bulk_a,
              acc_ok ? 1 : 0, (unsigned long long)empty_i, (unsigned long long)contig_i);
  std::printf("summary class_entries=%zu one_assignment_per_instr=%d mixed=%d\n",
              cm.cls.size(), instr_ok ? 1 : 0,
              (simt_i > 0 && bulk_i > 0) ? 1 : 0);
  return (instr_ok && acc_ok) ? 0 : 1;
}
