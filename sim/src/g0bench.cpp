// M23-G0 — DRAM bandwidth microbenchmark driver (audit instrument only).
//
// This file is NOT part of the simulator and is NOT in the Makefile's SRC list,
// so `bin/dx100sim` and `bin/dx100_tests` neither link it nor change. It is a
// separate binary (`bin/dx100_g0bench`, built by build/wm/m23g0_run.sh) that
// drives the *unmodified* `MemorySystem` with a designed line sequence and
// reports what the DRAM model did.
//
// Why a separate driver: the question is "what throughput can this DRAM system
// sustain", which needs an access stream whose bank/row structure is known in
// advance and whose request supply stops being the variable. The SIMT/Bulk
// front-ends cannot express that (they generate the M18–M23 application
// patterns). Nothing here re-implements the controller: every command, bank
// state, row state and byte count comes from `MemorySystem::tick` exactly as
// `dx100sim` uses it. The only thing this file adds is a request source.
//
// Patterns (all addresses are 64-byte VA lines; the model applies its own
// VA->PA map, ppn = vpn + PPN_OFFSET):
//
//   A  sequential within one bank-row: 32-line runs repeated on successive
//      8 MiB periods (131072 lines). By the production map (row = (pa/32) %
//      rows, bank = parity of pa ^ pa>>6 ^ pa>>12) all 32 lines of a run sit in
//      row 0 of both banks, 16 lines per bank, and the period preserves it.
//      No line repeats, so every request is a compulsory L1/L2 miss and the
//      measured stream is demand DRAM traffic only.
//   B  sequential across both banks: a plain linear sweep (the M23 workloads'
//      own order), 15/16 row hits in steady state.
//   C  alternating rows in one bank: lines v and v+32 on successive periods.
//      Same bank (parity is bit 0, unchanged by +32), row 0 and row 1 in turn,
//      so every command after the first is a row conflict and bank 1 is idle.
//   D  random across both banks: unique lines drawn uniformly from a footprint
//      16x the request count, so the bank/row stream is unpredictable and the
//      row-hit rate collapses to the cold-activation rate.
//   E  sixteen independent linear streams on disjoint 8 MiB periods, issued
//      round-robin: many unrelated request streams at once.
//
// `--outstanding K` is the request generator's independent-request window (the
// analogue of R / the MSHR pool). It is the variable in the E measurement:
// with K below the latency-bandwidth product the controller is starved by its
// own client, above it the DRAM system is the limit.
#include "sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using namespace dx100;

namespace {

constexpr u64 PERIOD = 131072;  // 8 MiB in 64-byte lines: one full row cycle

// Mirror of the production bank/row map (src/memory.cpp, l2_probe). Used ONLY
// to design a pattern and to print which (bank,row) coordinates it targets; the
// simulator always computes its own bank/row from the address it is given, so a
// divergence between this copy and the model would appear as a measured
// row-hit/activation count that contradicts `predict_hit_rate` below.
struct BankRow {
  u32 bank = 0, row = 0;
  bool operator==(const BankRow& o) const { return bank == o.bank && row == o.row; }
};

BankRow map_of_pa(u64 pa_line, const Config& c) {
  const u32 lines_per_row = std::max<u32>(1, c.row_bytes / LINE_BYTES);
  u64 bidx = pa_line;
  if (c.bank_hash) bidx ^= (pa_line >> 6) ^ (pa_line >> 12);
  return BankRow{static_cast<u32>(bidx % std::max<u32>(1, c.banks)),
                 static_cast<u32>(((pa_line / std::max<u32>(1, c.banks)) / lines_per_row) %
                                  std::max<u32>(1, c.rows))};
}

void usage() {
  std::printf(
      "dx100_g0bench — M23-G0 DRAM throughput microbenchmark\n"
      "usage: dx100_g0bench --config FILE [--set k=v]... --pattern A|B|C|D|E\n"
      "                     [--count N] [--outstanding K] [--seed S]\n");
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  std::string cfgpath, pattern = "B";
  u64 n = 100000, K = 128, seed = 1;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      cfgpath = argv[++i];
    } else if (a == "--set" && i + 1 < argc) {
      const std::string kv = argv[++i];
      const auto eq = kv.find('=');
      if (eq == std::string::npos) { std::fprintf(stderr, "bad --set: %s\n", kv.c_str()); return 2; }
      try { cfg.set(kv.substr(0, eq), kv.substr(eq + 1)); }
      catch (const std::exception& e) { std::fprintf(stderr, "config error: %s\n", e.what()); return 2; }
    } else if (a == "--pattern" && i + 1 < argc) {
      pattern = argv[++i];
    } else if (a == "--count" && i + 1 < argc) {
      n = std::strtoull(argv[++i], nullptr, 0);
    } else if (a == "--outstanding" && i + 1 < argc) {
      K = std::strtoull(argv[++i], nullptr, 0);
    } else if (a == "--seed" && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 0);
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  if (!cfgpath.empty()) {
    try { cfg = Config::load(cfgpath); }
    catch (const std::exception& e) { std::fprintf(stderr, "config error: %s\n", e.what()); return 2; }
  }
  // Re-apply --set overrides on top of the loaded file, in command-line order.
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) != "--set" || i + 1 >= argc) continue;
    const std::string kv = argv[i + 1];
    const auto eq = kv.find('=');
    try { cfg.set(kv.substr(0, eq), kv.substr(eq + 1)); }
    catch (const std::exception& e) { std::fprintf(stderr, "config error: %s\n", e.what()); return 2; }
  }

  if (K == 0) K = 1;
  if (n < 1000) { std::fprintf(stderr, "count must be >= 1000 for a steady-state window\n"); return 2; }

  MemorySystem mem(cfg);

  // ---- build the designed VA-line sequence --------------------------------
  std::vector<u64> va;
  va.reserve(static_cast<size_t>(n));
  if (pattern == "A") {
    for (u64 i = 0; i < n; ++i) va.push_back((i / 32) * PERIOD + (i % 32));
  } else if (pattern == "B") {
    for (u64 i = 0; i < n; ++i) va.push_back(i);
  } else if (pattern == "C") {
    for (u64 i = 0; i < n; ++i) va.push_back((i / 2) * PERIOD + ((i & 1) ? 32 : 0));
  } else if (pattern == "D") {
    std::mt19937_64 rng(seed);
    std::unordered_set<u64> seen;
    seen.reserve(static_cast<size_t>(n) * 2);
    while (va.size() < n) {
      const u64 cand = rng() % (n * 16);
      const u64 pa = mem.pa_line_of_va_line(cand);
      if (!seen.insert(pa).second) continue;  // distinct PA lines only
      va.push_back(cand);
    }
  } else if (pattern == "E") {
    const u64 per = (n + 15) / 16;
    u64 made = 0;
    for (u64 step = 0; step < per && made < n; ++step)
      for (u64 s = 0; s < 16 && made < n; ++s) {
        va.push_back(s * PERIOD + step);
        ++made;
      }
  } else {
    std::fprintf(stderr, "unknown pattern: %s\n", pattern.c_str());
    return 2;
  }

  // ---- designed-pattern signature, from the same map the model uses -------
  // `predict_hit_rate` replays the per-bank row state over the designed
  // sequence exactly as the arbiter would see it if it strictly followed this
  // order: an access is a predicted row hit iff its bank's last served row is
  // its own row. Any large disagreement with `dram_row_hits/dram_reads` means
  // the address map assumed here is not the map the simulator applied.
  u64 pred_hits = 0;
  u64 pred_rows_touched = 0;
  std::unordered_set<u64> rowset;
  {
    std::vector<u64> last_row(static_cast<size_t>(std::max<u32>(1, cfg.banks)), NONE);
    std::vector<char> warm(static_cast<size_t>(std::max<u32>(1, cfg.banks)), 0);
    for (u64 i = 0; i < n; ++i) {
      const BankRow br = map_of_pa(mem.pa_line_of_va_line(va[i]), cfg);
      const size_t b = br.bank;
      if (warm[b] && last_row[b] == br.row) ++pred_hits;
      last_row[b] = br.row;
      warm[b] = 1;
      rowset.insert((static_cast<u64>(br.bank) << 32) | br.row);
    }
    pred_rows_touched = rowset.size();
  }

  // ---- drive the unmodified memory system ---------------------------------
  const u64 r10 = n / 10, r90 = (n * 9) / 10;
  u64 cycle = 0, next = 0;
  u64 first_svc = 0, last_svc = 0, prev_svc = 0;
  u64 c10 = 0, c90 = 0;
  bool hit10 = false, hit90 = false;
  bool guard = false;

  for (;; ++cycle) {
    std::vector<u64> done;
    mem.tick(cycle, done);

    if (mem.dram.svc_events != prev_svc) {
      if (first_svc == 0) first_svc = cycle;
      last_svc = cycle;
      prev_svc = mem.dram.svc_events;
    }
    if (!hit10 && mem.dram.reads >= r10) { c10 = cycle; hit10 = true; }
    if (!hit90 && mem.dram.reads >= r90) { c90 = cycle; hit90 = true; }

    while (next < n && mem.inflight_count() < K) {
      Req r;
      r.va_line = va[next];
      r.vpn = r.va_line >> mem.page_line_bits();
      r.core = 0;
      r.slot = static_cast<u32>(next);
      if (!mem.submit(r, cycle)) break;  // refused: retry next cycle
      ++next;
    }
    if (next == n && mem.inflight_count() == 0) break;
    if (cycle >= cfg.max_cycles) { guard = true; break; }
  }

  const u64 span = last_svc > first_svc ? last_svc - first_svc + 1 : 1;
  const u64 steady_cycles = c90 > c10 ? c90 - c10 : 1;
  const u64 steady_lines = r90 > r10 ? r90 - r10 : 1;
  const double bytes = static_cast<double>(mem.dram.bytes);
  const double reads = static_cast<double>(mem.dram.reads);
  const u64 cold = mem.dram.reads - mem.dram.row_hits - mem.dram.row_conflicts;
  double lat_mean = 0.0;
  if (!mem.lat_all.empty()) {
    unsigned long long sum = 0;
    for (u32 v : mem.lat_all) sum += v;
    lat_mean = static_cast<double>(sum) / static_cast<double>(mem.lat_all.size());
  }

  // Human-readable block.
  std::printf("== M23-G0 DRAM microbenchmark ==\n");
  std::printf("pattern=%s requests=%llu outstanding=%llu seed=%llu\n", pattern.c_str(),
              static_cast<unsigned long long>(n), static_cast<unsigned long long>(K),
              static_cast<unsigned long long>(seed));
  std::printf("banks=%u rows=%u row_bytes=%u t_hit=%u t_miss=%u link=%u bank_hash=%d "
              "dram_q=%u frfcfs=%d\n",
              cfg.banks, cfg.rows, cfg.row_bytes, cfg.t_hit, cfg.t_miss, cfg.link,
              cfg.bank_hash ? 1 : 0, cfg.dram_q, cfg.frfcfs ? 1 : 0);
  std::printf("designed: rows_touched=%llu predicted_row_hit_rate=%.4f\n",
              static_cast<unsigned long long>(pred_rows_touched),
              n ? static_cast<double>(pred_hits) / static_cast<double>(n) : 0.0);
  std::printf("\n== measured ==\n");

  auto p = [](const char* k, double v) { std::printf("%s=%.6f\n", k, v); };
  auto u = [](const char* k, unsigned long long v) { std::printf("%s=%llu\n", k, v); };

  u("cycles", cycle);
  u("first_service_cycle", first_svc);
  u("last_service_cycle", last_svc);
  u("service_span_cycles", span);
  u("steady_window_cycles", steady_cycles);
  u("steady_window_lines", steady_lines);
  u("dram_reads", mem.dram.reads);
  u("dram_bytes", mem.dram.bytes);
  u("dram_row_hits", mem.dram.row_hits);
  u("dram_row_conflicts", mem.dram.row_conflicts);
  u("dram_cold_activations", cold);
  u("dram_busy_cycles", mem.dram.busy_cycles);
  u("dram_max_q", mem.dram.max_q);
  u("dram_q_occ_sum", mem.dram.q_occ_sum);
  u("dram_blocked_cycles", mem.dram.blocked_cycles);
  u("dram_starved_cycles", mem.dram.starved_cycles);
  u("dram_wait_sum", mem.dram.wait_sum);
  u("dram_wait_max", mem.dram.wait_max);
  u("dram_rows_used", mem.dram.rows_used);
  u("dram_row_acts_max", mem.dram.row_acts_max);
  u("l1_reads", mem.l1.reads);
  u("l1_hits", mem.l1.hits);
  u("l1_mshr_max", mem.l1.max_occ);
  u("l2_reads", mem.l2.reads);
  u("l2_hits", mem.l2.hits);
  u("l2_mshr_max", mem.l2.max_occ);
  u("walks", mem.ptw.walks);
  u("pte_fetches", mem.ptw.pte_fetches);
  u("inflight_max", mem.inflight_max);

  p("bytes_per_cycle_full", static_cast<double>(mem.dram.reads) / static_cast<double>(span));
  p("bytes_per_cycle", 64.0 * static_cast<double>(mem.dram.reads) / static_cast<double>(span));
  p("lines_per_cycle", static_cast<double>(mem.dram.reads) / static_cast<double>(span));
  p("steady_lines_per_cycle", static_cast<double>(steady_lines) / static_cast<double>(steady_cycles));
  p("steady_bytes_per_cycle", 64.0 * static_cast<double>(steady_lines) / static_cast<double>(steady_cycles));
  p("bank_utilization", static_cast<double>(mem.dram.busy_cycles) /
                            (static_cast<double>(std::max<u32>(1, cfg.banks)) * static_cast<double>(span)));
  p("row_hit_rate", reads > 0 ? static_cast<double>(mem.dram.row_hits) / reads : 0.0);
  p("predicted_row_hit_rate", n ? static_cast<double>(pred_hits) / static_cast<double>(n) : 0.0);
  p("avg_queue_depth", static_cast<double>(mem.dram.q_occ_sum) / static_cast<double>(cycle + 1));
  p("avg_dram_queue_wait", reads > 0 ? static_cast<double>(mem.dram.wait_sum) / reads : 0.0);
  p("avg_request_latency", lat_mean);
  p("gbs_at_400mhz", bytes / static_cast<double>(span) * 400e6 / 1e9);

  std::printf("guard_tripped=%d\n", guard ? 1 : 0);
  std::printf("conservation l2_reads=%llu l1_reads=%llu\n",
              static_cast<unsigned long long>(mem.l2.reads),
              static_cast<unsigned long long>(mem.l1.reads));
  return guard ? 1 : 0;
}
