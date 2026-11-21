// Workload generator — preserves v3 §5 definitions (Λ, P, λ, κ, ρ_dup, L).
#include "sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <unordered_set>

namespace dx100 {

double solve_M(u32 N, double target) {
  if (target <= 0.0) return static_cast<double>(N) * 1e6;
  auto f = [N](double M) { return 1.0 - (M / N) * (1.0 - std::exp(-static_cast<double>(N) / M)); };
  double lo = 1.0, hi = static_cast<double>(N) * 1e6;
  for (int i = 0; i < 300; ++i) {
    double mid = 0.5 * (lo + hi);
    if (f(mid) > target) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}

// ---------------------------------------------------------------------------
// M18: file-driven trace
// ---------------------------------------------------------------------------
// The synthetic generator answers "how does the engine behave on a designed
// access pattern". A real application answers "which pattern does it actually
// issue". This loader adds no access, reorders nothing and derives nothing: it
// reads the addresses an offline execution of the application issued, in
// program order, together with the class of the instruction that issued them.
// The classes come from the file rather than from a heuristic, so the dispatch
// decision is the application's own structure (see the M18 selection report).
//
// File layout (little-endian), written by tools/build_trace.py:
//   char  magic[8]   "M18TRC1\0"
//   u32   lanes
//   u32   elem_bytes
//   u64   n_instr
//   u64   n_acc
//   u8    cls[n_instr]                0 = SIMT, 1 = Bulk (the OpClass values)
//   struct { u64 addr; u32 instr; u32 lane; } acc[n_acc]
Trace build_trace_file(const Config& cfg, std::string& err) {
  Trace t;
  t.lanes = cfg.lanes;

  std::ifstream f(cfg.dataset, std::ios::binary);
  if (!f) { err = "cannot open " + cfg.dataset; return t; }
  char magic[8] = {0};
  u32 lanes = 0, elem = 0;
  u64 n_instr = 0, n_acc = 0;
  f.read(magic, 8);
  f.read(reinterpret_cast<char*>(&lanes), 4);
  f.read(reinterpret_cast<char*>(&elem), 4);
  f.read(reinterpret_cast<char*>(&n_instr), 8);
  f.read(reinterpret_cast<char*>(&n_acc), 8);
  if (!f || std::string(magic, 7) != "M18TRC1") { err = cfg.dataset + ": not an M18TRC1 trace"; return t; }
  if (lanes != cfg.lanes)
    { err = "trace lanes " + std::to_string(lanes) + " != configured lanes " + std::to_string(cfg.lanes); return t; }
  if (elem != cfg.elem_bytes)
    { err = "trace elem_bytes " + std::to_string(elem) + " != configured elem_bytes " + std::to_string(cfg.elem_bytes); return t; }
  if (n_instr == 0 || n_acc == 0 || n_instr > (1ull << 32) || n_acc > (1ull << 32))
    { err = "trace header counts out of range"; return t; }

  t.n_instr = static_cast<u32>(n_instr);
  t.instr_class.resize(t.n_instr);
  f.read(reinterpret_cast<char*>(t.instr_class.data()), static_cast<std::streamsize>(n_instr));
  if (!f) { err = "truncated class table"; return t; }

  t.acc.resize(static_cast<size_t>(n_acc));
  u32 last = 0;
  for (u64 i = 0; i < n_acc; ++i) {
    u64 addr = 0;
    u32 instr = 0, lane = 0;
    f.read(reinterpret_cast<char*>(&addr), 8);
    f.read(reinterpret_cast<char*>(&instr), 4);
    f.read(reinterpret_cast<char*>(&lane), 4);
    if (!f) { err = "truncated access table at " + std::to_string(i); return t; }
    if (instr >= t.n_instr) { err = "instruction id out of range"; return t; }
    if (lane >= lanes) { err = "lane id out of range"; return t; }
    if (i && instr < last) { err = "accesses are not grouped in instruction order"; return t; }
    last = instr;
    Access a;
    a.addr = addr;
    a.line = addr / LINE_BYTES;
    a.elem = static_cast<u32>(addr / elem);
    a.instr = instr;
    a.lane = lane;
    t.acc[static_cast<size_t>(i)] = a;
  }

  // Same realized ground truth the generator publishes, computed the same way,
  // so a file trace is reported in the same units as a synthetic one.
  std::unordered_set<u64> lset, wset;
  std::set<u64> pset;
  for (const auto& a : t.acc) {
    lset.insert(a.line);
    wset.insert(a.addr);
    pset.insert(a.line / cfg.page_lines());
  }
  t.lines_real = lset.size();
  t.pages_real = static_cast<u32>(pset.size());
  t.rho_line_real = 1.0 - static_cast<double>(lset.size()) / static_cast<double>(t.acc.size());
  t.rho_word_real = 1.0 - static_cast<double>(wset.size()) / static_cast<double>(t.acc.size());
  t.bytes_real = static_cast<u64>(t.acc.size()) * cfg.elem_bytes;
  {
    u64 lo = ~0ull, hi = 0, n = 0, contiguous = 0, n_i = 0;
    u32 cur = ~0u;
    auto flush = [&]() {
      if (cur == ~0u) return;
      ++n_i;
      if (n > 1 && hi - lo == (n - 1) * cfg.elem_bytes) ++contiguous;
    };
    for (const auto& a : t.acc) {
      if (a.instr != cur) { flush(); cur = a.instr; lo = a.addr; hi = a.addr; n = 1; }
      else { lo = std::min(lo, a.addr); hi = std::max(hi, a.addr); ++n; }
    }
    flush();
    t.kappa_real = n_i ? static_cast<double>(contiguous) / n_i : 0.0;
  }
  return t;
}

Trace build_trace(const Config& cfg) {
  if (!cfg.dataset.empty()) {
    std::string err;
    Trace t = build_trace_file(cfg, err);
    if (!err.empty()) {
      std::cerr << "dataset " << cfg.dataset << ": " << err << "\n";
      std::exit(2);
    }
    return t;
  }
  Trace t;
  t.lanes = cfg.lanes;

  const u32 L = cfg.lanes;
  const u32 elems_per_line = LINE_BYTES / cfg.elem_bytes;  // 8
  u32 lam = cfg.P ? cfg.Lambda / cfg.P : cfg.Lambda;       // lines per page
  if (lam == 0) lam = 1;
  if (lam > cfg.page_lines()) lam = cfg.page_lines();  // one page holds page_lines() lines

  // Line set: λ lines per page, spread with stride 64/λ so that each line holds
  // elem_bytes*8 consecutive elements and the page is fully covered (v3 §5).
  std::vector<u64> lines;
  lines.reserve(static_cast<size_t>(cfg.P) * lam);
  for (u32 p = 0; p < cfg.P; ++p) {
    for (u32 j = 0; j < lam; ++j) {
      lines.push_back(static_cast<u64>(p) * LINE_BYTES + static_cast<u64>(j) * (LINE_BYTES / lam));
    }
  }
  if (lines.size() > cfg.Lambda) lines.resize(cfg.Lambda);

  std::mt19937_64 rng(cfg.seed);
  std::uniform_int_distribution<u32> slot_dist(0, elems_per_line - 1);

  // Duplicate ablation: draw from M distinct lines. M > Λ is unrepresentable
  // inside a Λ-line footprint, so clamp and let the realized ρ be measured.
  double M = solve_M(cfg.N, cfg.rho_dup);
  u32 pool = (cfg.rho_dup <= 0.0) ? static_cast<u32>(lines.size())
                                  : std::min<u32>(static_cast<u32>(lines.size()),
                                                  std::max<u32>(1, static_cast<u32>(M)));
  if (pool == 0) pool = 1;

  // rho_dup = 0 is defined as a permutation (no duplicates at all); rho_dup > 0
  // means draws with replacement from M distinct targets, so that the realized
  // duplicate fraction matches the inverted closed form (v3 C24).
  const bool with_repl = cfg.rho_dup > 0.0;
  std::vector<u32> perm(pool);
  std::iota(perm.begin(), perm.end(), 0);
  std::shuffle(perm.begin(), perm.end(), rng);
  size_t perm_pos = 0;
  std::uniform_int_distribution<u32> pool_dist(0, pool - 1);
  auto next_line_idx = [&]() -> u32 {
    if (with_repl) return pool_dist(rng);
    if (perm_pos >= perm.size()) {
      std::shuffle(perm.begin(), perm.end(), rng);
      perm_pos = 0;
    }
    return perm[perm_pos++];
  };

  const u32 n_instr = cfg.N / L;
  t.n_instr = n_instr * L == cfg.N ? n_instr : n_instr + 1;
  t.acc.reserve(cfg.N);
  // M9: the class metadata is a property of the *operation*, decided here where
  // the operation is created, and handed on unchanged. It is recorded, not
  // derived later from the addresses (the "pattern" variant in build_op_classes
  // does that separately, as a cross-check).
  t.instr_class.assign(t.n_instr, static_cast<u8>(OpClass::Simt));

  std::uniform_real_distribution<double> uni(0.0, 1.0);
  u32 made = 0;
  size_t seq_cursor = 0;  // linear sweep cursor for regular instructions
  u64 stream_cursor = 0;  // W0: element cursor of the streaming primary class
  for (u32 i = 0; i < t.n_instr && made < cfg.N; ++i) {
    const bool regular = uni(rng) < cfg.kappa;
    t.instr_class[i] = regular ? static_cast<u8>(OpClass::Simt)
                               : static_cast<u8>(OpClass::Bulk);
    if (regular) {
      // L contiguous elements: a *linear* run of the set, so kappa = 1 is a
      // streaming access pattern (v3 W0), not a scattered one.
      if (lines.empty()) break;
      u32 li = static_cast<u32>(seq_cursor % lines.size());
      seq_cursor += std::max<size_t>(1, (L + elems_per_line - 1) / elems_per_line);
      for (u32 lane = 0; lane < L && made < cfg.N; ++lane) {
        if (lane > 0 && (lane % elems_per_line) == 0) li = (li + 1) % static_cast<u32>(lines.size());
        u32 s = lane % elems_per_line;
        Access a;
        a.line = lines[li];
        a.addr = a.line * LINE_BYTES + static_cast<u64>(s) * cfg.elem_bytes;
        a.elem = static_cast<u32>(a.line * 8 + s);
        a.instr = i;
        a.lane = lane;
        t.acc.push_back(a);
        ++made;
      }
    } else {
      for (u32 lane = 0; lane < L && made < cfg.N; ++lane) {
        // W0 (`bulk_stream`): the same line set, in order -- consecutive elements
        // on consecutive lines, as a streaming sweep. W1 (default): the permuted
        // draw below. Everything else about the trace is unchanged.
        u32 li, s;
        if (cfg.bulk_stream) {
          const u64 g = stream_cursor++;
          li = static_cast<u32>((g / elems_per_line) % lines.size());
          s = static_cast<u32>(g % elems_per_line);
        } else {
          li = next_line_idx();
          s = slot_dist(rng);
        }
        Access a;
        a.line = lines[li];
        a.addr = a.line * LINE_BYTES + static_cast<u64>(s) * cfg.elem_bytes;
        a.elem = static_cast<u32>(a.line * 8 + s);
        a.instr = i;
        a.lane = lane;
        t.acc.push_back(a);
        ++made;
      }
    }
  }

  // Realized ground truth (v3 F7).
  std::unordered_set<u64> lset, wset;
  std::set<u64> pset;
  for (const auto& a : t.acc) {
    lset.insert(a.line);
    wset.insert(a.addr);
    pset.insert(a.line / cfg.page_lines());
  }
  t.lines_real = lset.size();
  t.pages_real = static_cast<u32>(pset.size());
  t.rho_line_real = 1.0 - static_cast<double>(lset.size()) / static_cast<double>(t.acc.size());
  t.rho_word_real = 1.0 - static_cast<double>(wset.size()) / static_cast<double>(t.acc.size());
  t.bytes_real = static_cast<u64>(t.acc.size()) * cfg.elem_bytes;

  // kappa_real: fraction of instructions whose lanes are contiguous in address
  // order (v3 §5: "regular" = the L lanes cover one contiguous run).
  {
    u64 lo = ~0ull, hi = 0, n = 0, contiguous = 0, n_i = 0;
    u32 cur = ~0u;
    auto flush = [&]() {
      if (cur == ~0u) return;
      ++n_i;
      if (n > 1 && hi - lo == (n - 1) * cfg.elem_bytes) ++contiguous;
    };
    for (const auto& a : t.acc) {
      if (a.instr != cur) { flush(); cur = a.instr; lo = a.addr; hi = a.addr; n = 1; }
      else { lo = std::min(lo, a.addr); hi = std::max(hi, a.addr); ++n; }
    }
    flush();
    t.kappa_real = n_i ? static_cast<double>(contiguous) / n_i : 0.0;
  }
  return t;
}

// ---------------------------------------------------------------------------
// M9: operation-class metadata
// ---------------------------------------------------------------------------
// Two sources, one interface. Neither is a compiler: the simulated front-end
// cannot analyse an address expression, so the class is supplied. `gen` hands
// over the decision the generator already made when it built the instruction
// (recorded during trace construction); `pattern` recovers the same decision
// from the addresses with the contiguity rule kappa_real uses, which is the only
// rule a compiler could implement against this trace. `pattern` exists to prove
// the annotation is recoverable, never to classify during execution.
ClassMap build_op_classes(const Config& cfg, const Trace& tr) {
  ClassMap cm;
  cm.cls.assign(tr.n_instr, static_cast<u8>(OpClass::Simt));

  // The two uniform maps exist so the equivalence gates can hold the trace fixed
  // and vary only the annotation: all-SIMT (G1) and all-bulk (G2) on any trace.
  // They are still decided before execution and are not a runtime policy.
  if (cfg.op_class == "simt") return cm;
  if (cfg.op_class == "bulk") {
    cm.cls.assign(tr.n_instr, static_cast<u8>(OpClass::Bulk));
    return cm;
  }

  if (cfg.op_class == "pattern") {
    // Same predicate as Trace::kappa_real: an instruction is regular iff its
    // lanes form one contiguous run of elem_bytes-strided addresses.
    u32 cur = ~0u;
    u64 lo = 0, hi = 0, n = 0;
    auto flush = [&]() {
      if (cur == ~0u || cur >= cm.cls.size()) return;
      const bool contiguous = n > 1 && hi - lo == (n - 1) * cfg.elem_bytes;
      cm.cls[cur] = contiguous ? static_cast<u8>(OpClass::Simt)
                               : static_cast<u8>(OpClass::Bulk);
    };
    for (const auto& a : tr.acc) {
      if (a.instr != cur) {
        flush();
        cur = a.instr;
        lo = hi = a.addr;
        n = 1;
      } else {
        lo = std::min(lo, a.addr);
        hi = std::max(hi, a.addr);
        ++n;
      }
    }
    flush();
    return cm;
  }

  // "gen": the generator's own decision, recorded as metadata.
  if (!tr.instr_class.empty()) cm.cls = tr.instr_class;
  return cm;
}

}  // namespace dx100
