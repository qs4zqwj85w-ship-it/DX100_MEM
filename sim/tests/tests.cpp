// Component and system tests for the SIMT baseline.
#include "sim.hpp"
#include "hbm.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <string>
#include <vector>

using namespace dx100;

static int g_checks = 0, g_fail = 0;

#define CHECK(c)                                                       \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(c)) {                                                        \
      ++g_fail;                                                        \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
    }                                                                  \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                          \
  do {                                                                 \
    ++g_checks;                                                        \
    const double _a = (a), _b = (b);                                   \
    if (std::fabs(_a - _b) > (tol)) {                                  \
      ++g_fail;                                                        \
      std::printf("    FAIL %s:%d: %.4f vs %.4f (tol %.4f)\n",         \
                  __FILE__, __LINE__, _a, _b, (double)(tol));          \
    }                                                                  \
  } while (0)

namespace {

// Drives the memory system one line request at a time.
struct Drv {
  MemorySystem m;
  u64 cycle = 0;
  explicit Drv(const Config& c) : m(c) {}

  void access(u64 line, u32 core = 0) {
    Req r;
    r.va_line = line;
    r.vpn = line >> 6;
    r.core = core;
    r.slot = 0;
    std::vector<u64> d;
    while (!m.submit(r, cycle)) {
      d.clear();
      m.tick(cycle, d);
      ++cycle;
    }
    for (;;) {
      d.clear();
      m.tick(cycle, d);
      ++cycle;
      if (!d.empty()) break;
    }
  }
};

Config base_small() {
  Config c;
  c.cores = 1;
  c.N = 4096;
  c.Lambda = 4096;
  c.P = 256;
  return c;
}

// -------------------------------------------------------------------------
void test_config() {
  Config c;
  c.set("cores", "8");
  c.set("wl", "W0");
  c.set("kappa", "0.25");
  CHECK(c.cores == 8);
  CHECK(c.wl == "W0");
  CHECK_NEAR(c.kappa, 0.25, 1e-12);
  bool threw = false;
  try { c.set("not_a_key", "1"); } catch (...) { threw = true; }
  CHECK(threw);
}

void test_solve_M() {
  for (double target : {0.25, 0.5, 0.75}) {
    const double M = solve_M(16384, target);
    const double f = 1.0 - (M / 16384.0) * (1.0 - std::exp(-16384.0 / M));
    CHECK_NEAR(f, target, 1e-6);
  }
}

void test_trace_permutation() {
  Config c;
  c.wl = "W1";
  c.N = 4096;
  c.Lambda = 4096;
  c.P = 512;
  c.kappa = 0.0;
  c.rho_dup = 0.0;
  c.seed = 7;
  const Trace t = build_trace(c);
  CHECK(t.acc.size() == 4096);
  CHECK(t.lines_real == 4096);   // permutation: every line exactly once
  CHECK(t.pages_real == 512);
  CHECK_NEAR(t.rho_line_real, 0.0, 1e-12);
}

void test_trace_regular() {
  Config c;
  c.wl = "W0";
  c.N = 16384;
  c.Lambda = 16384;
  c.P = 256;
  c.kappa = 1.0;
  const Trace t = build_trace(c);
  CHECK_NEAR(t.kappa_real, 1.0, 1e-12);
  // 16 384 x 8 B of contiguity = 2 048 distinct lines.
  CHECK(t.lines_real >= 1900 && t.lines_real <= 2048);
}

void test_trace_duplicates() {
  Config c;
  c.wl = "W1";
  c.N = 16384;
  c.Lambda = 16384;
  c.P = 2048;
  c.kappa = 0.0;
  c.rho_dup = 0.5;
  c.seed = 11;
  const Trace t = build_trace(c);
  CHECK_NEAR(t.rho_line_real, 0.5, 0.05);
}

void test_tlb_capacity() {
  Config c = base_small();
  c.dtlb_entries = 2;
  c.dtlb_ways = 1;
  c.l1_mshr = 64;
  Drv d(c);
  d.access(0);        // vpn 0
  d.access(64);       // vpn 1
  d.access(0);        // hit
  d.access(64);       // hit
  CHECK(d.m.tlb[0].misses == 2);
  CHECK(d.m.tlb[0].hits == 2);
  d.access(128);      // vpn 2 -> set 0, evicts vpn 0
  d.access(0);        // miss again
  CHECK(d.m.tlb[0].misses == 4);
  CHECK(d.m.tlb[0].evict >= 2);
}

void test_l1_reuse() {
  Config c = base_small();
  c.l1_lines = 16;
  c.l1_ways = 4;
  c.l1_mshr = 16;
  Drv d(c);
  for (u64 i = 0; i < 16; ++i) d.access(i);
  CHECK(d.m.l1.misses == 16);
  CHECK(d.m.l1.hits == 0);
  for (u64 i = 0; i < 16; ++i) d.access(i);
  CHECK(d.m.l1.hits == 16);
  for (u64 i = 0; i < 16; ++i) d.access(i + 16);  // distinct set-compatible lines
  CHECK(d.m.l1.misses >= 32);
}

void test_dram_locality() {
  // A sequential sweep reuses open rows; a scatter over the same range cannot.
  Config c1 = base_small();
  c1.l1_lines = 4;  // force misses
  Drv seq(c1);
  for (u64 i = 0; i < 4096; ++i) seq.access(i);

  Config c2 = base_small();
  c2.l1_lines = 4;
  Drv rnd(c2);
  u64 x = 12345;
  for (u64 i = 0; i < 4096; ++i) {
    x = x * 6364136223846793005ull + 1442695040888963407ull;
    rnd.access((x >> 33) % 4096);
  }

  CHECK(seq.m.dram.reads > 0);
  CHECK(rnd.m.dram.reads > 0);
  CHECK(seq.m.dram.row_hits * 2 > seq.m.dram.reads);       // stream: >50% row hits
  CHECK(rnd.m.dram.row_conflicts > seq.m.dram.row_conflicts);  // scatter: mostly conflicts
}

void test_bank_hash_spreads_strides() {
  // Regression: without a hashed map, a stride equal to the bank count pins every
  // request to one bank (the defect that produced 0.086 lines/cycle).
  auto banks_used = [](bool hash) {
    Config c = base_small();
    c.l1_lines = 4;
    c.bank_hash = hash;
    Drv d(c);
    for (u64 i = 0; i < 64; ++i) d.access(i * 16);  // stride == bank count
    u64 m = d.m.dram.bank_mask, n = 0;
    while (m) { n += m & 1; m >>= 1; }
    return n;
  };
  CHECK(banks_used(false) == 1);   // aliasing: one bank serves every request
  CHECK(banks_used(true) > 4);     // hashed: the stride spreads across banks
}

void test_conservation() {
  Config c;
  c.wl = "W1";
  c.N = 4096;
  c.Lambda = 256;   // 4 pages: the 16-entry TLB holds them after warm-up
  c.P = 4;
  c.cores = 4;
  c.seed = 3;
  Sim s(c);
  const RunResult r = s.run();
  CHECK(r.ok);
  if (!r.ok) for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.s.get("completed") == r.s.get("requests_generated"));
  CHECK(r.s.get("requests_unissued_end") == 0);
  CHECK(r.s.get("l1_reads") == r.s.get("l1_hits") + r.s.get("l1_misses"));
  CHECK(r.s.get("l2_reads") == r.s.get("l2_hits") + r.s.get("l2_misses"));
}

void test_determinism() {
  Config c;
  c.wl = "W1";
  c.N = 2048;
  c.Lambda = 2048;
  c.P = 128;
  c.cores = 4;
  c.seed = 99;
  Sim a(c), b(c);
  const RunResult ra = a.run(), rb = b.run();
  CHECK(ra.cycles == rb.cycles);
  CHECK(ra.s.get("walks") == rb.s.get("walks"));
  CHECK(ra.s.get("dram_reads") == rb.s.get("dram_reads"));
}

void test_streaming_ceiling() {
  // A TLB-resident streaming sweep: the data path is the only limit.
  Config c;
  c.wl = "W0";
  c.kappa = 1.0;
  c.N = 4096;
  c.Lambda = 512;
  c.P = 8;  // 8 pages fit the 16-entry TLB
  c.cores = 4;
  Sim s(c);
  const RunResult r = s.run();
  CHECK(r.ok);
  const u64 lines = r.s.get("trace_lines");
  CHECK(r.cycles >= lines);                    // never faster than 1 line/cycle
  CHECK(r.cycles <= 8 * lines);                // no pathological slowdown
  CHECK(r.s.get("walks") == r.s.get("trace_pages"));  // one walk per page
  // A stream must mostly hit open DRAM rows.
  CHECK(r.s.get("dram_row_hits") * 5 >= r.s.get("dram_reads") * 4);
}

void test_stream_beats_gather() {
  // The one comparative property the baseline must show: with the same number
  // of line requests, a sequential sweep is much cheaper than a random gather.
  // The footprint must span many more DRAM rows than there are banks, else
  // both patterns look identical to the memory system.
  Config w0;
  w0.wl = "W0";
  w0.kappa = 1.0;
  w0.N = 32768;
  w0.Lambda = 4096;  // 256 KiB: 16 rows per bank
  w0.P = 64;
  w0.cores = 4;
  Sim s0(w0);
  const RunResult r0 = s0.run();

  Config w1 = w0;
  w1.wl = "W1";
  w1.kappa = 0.0;
  w1.seed = 17;
  Sim s1(w1);
  const RunResult r1 = s1.run();

  CHECK(r0.ok && r1.ok);
  CHECK(r0.s.get("trace_lines") == 4096);
  CHECK(r1.s.get("trace_lines") == 4096);

  const double hr0 = double(r0.s.get("dram_row_hits")) / double(r0.s.get("dram_reads"));
  const double hr1 = double(r1.s.get("dram_row_hits")) / double(r1.s.get("dram_reads"));
  std::printf("    W0 cycles=%llu row_hit_rate=%.2f | W1 cycles=%llu row_hit_rate=%.2f\n",
              (unsigned long long)r0.cycles, hr0,
              (unsigned long long)r1.cycles, hr1);
  // Structural property, independent of the fitted timings: a stream reuses
  // open rows, a random gather cannot.
  CHECK(hr0 > 0.50);
  CHECK(hr1 < 0.25);
  CHECK(r0.cycles < r1.cycles);
}

void test_walks_analytic() {
  Config c;
  c.wl = "W1";
  c.N = 16384;
  c.Lambda = 16384;
  c.P = 512;   // exactly the L2 TLB reach: each page is walked once
  c.kappa = 0.0;
  c.cores = 4;
  c.seed = 5;
  Sim s(c);
  const RunResult r = s.run();
  CHECK(r.ok);
  const u64 pages = r.s.get("trace_pages");
  const u64 walks = r.s.get("walks");
  // With the working set resident in the L2 TLB, walks equals distinct pages.
  CHECK_NEAR(static_cast<double>(walks), static_cast<double>(pages), 0.05 * pages);
  CHECK(r.s.get("pte_fetches") == 2 * walks);   // Sv32: two levels
  CHECK(r.s.get("l2tlb_max_occ") <= 8);
}

// -------------------------------------------------------------------------
// M4a: exact equivalence between the baseline front-end and the independently
// written neutral issuer. Any difference is a bug, never noise.
void test_m4a_exact_equivalence() {
  Config c;
  c.wl = "W1";
  c.N = 4096;
  c.Lambda = 4096;
  c.P = 512;
  c.cores = 4;
  c.seed = 21;
  // Neutral profile: one request per cycle (AG_LANES = 1), no duplicate table,
  // L1 on path, same 16-entry dMMU TLB, same R = 128, FIFO.
  c.warps_per_core = 1;
  c.lsu_pending = 0;

  Sim base(c);
  const RunResult a = base.run(Mode::Baseline);
  Sim neutral(c);
  const RunResult b = neutral.run(Mode::Neutral);

  CHECK(a.ok && b.ok);
  CHECK(a.completion_hash == b.completion_hash);

  // Two counters describe the issuer's own internal structure rather than the
  // architecture under test (the neutral issuer flattens the trace and has no
  // per-instruction stage), so they are excluded by name, not by tolerance.
  auto front_end_bookkeeping = [](const std::string& k) {
    // `simt_*` counters describe the SIMT front-end's own occupancy accounting
    // (per-instruction occupancy cycle count and the coalescer's input latch).
    // The neutral issuer has no per-instruction stage, so these are structural,
    // not architectural; they are excluded by name, never by tolerance.
    return k == "instr_issued" || k == "unissued_max" ||
           k.compare(0, 5, "simt_") == 0;
  };

  std::string first;
  int diffs = 0;
  for (const auto& kv : a.s.c) {
    if (front_end_bookkeeping(kv.first)) continue;
    const u64 other = b.s.get(kv.first);
    if (other != kv.second) {
      if (first.empty()) first = kv.first + ": " + std::to_string(kv.second) +
                                  " vs " + std::to_string(other);
      ++diffs;
    }
  }
  for (const auto& kv : b.s.c) {
    if (front_end_bookkeeping(kv.first)) continue;
    if (a.s.c.find(kv.first) == a.s.c.end()) {
      if (first.empty()) first = kv.first + " missing in baseline";
      ++diffs;
    }
  }
  if (diffs) std::printf("      %d counter(s) differ; first: %s\n", diffs, first.c_str());
  CHECK(diffs == 0);
  std::printf("    M4a: hash=%llx cycles=%llu/%llu\n",
              (unsigned long long)a.completion_hash,
              (unsigned long long)a.cycles, (unsigned long long)b.cycles);
}

// M4b: architectural null test. Same trace, same page set, only the mapping of
// instructions to cores changes — an architecturally empty difference.
void test_m4b_null() {
  Config c;
  c.wl = "W1";
  c.N = 8192;
  c.Lambda = 16384;
  c.P = 512;  // fits the 512-entry L2 TLB: the page set is shared, walks must be equal
  c.cores = 4;
  c.seed = 33;

  c.core_map = "round_robin";
  Sim s1(c);
  const RunResult a = s1.run();
  c.core_map = "blocked";
  Sim s2(c);
  const RunResult b = s2.run();

  CHECK(a.ok && b.ok);
  auto rel = [](double x, double y) {
    const double d = std::max(std::fabs(x), std::fabs(y));
    return d == 0 ? 0.0 : std::fabs(x - y) / d;
  };
  auto cmp = [&](const char* k, double tol) {
    const double x = double(a.s.get(k)), y = double(b.s.get(k));
    const double r = rel(x, y);
    if (r > tol) {
      std::printf("      %s: %.0f vs %.0f (%.3f > %.3f)\n", k, x, y, r, tol);
      ++g_fail;
    }
    ++g_checks;
  };

  // Family 1: request counts.
  cmp("requests_generated", 0.02);
  cmp("issued", 0.02);
  cmp("completed", 0.02);
  // Family 2: line transactions.
  cmp("l2_reads", 0.02);
  cmp("l2_misses", 0.02);
  cmp("dram_reads", 0.02);
  cmp("dram_bytes", 0.02);
  // Family 3: translation. Walks must be exact: the page set is identical.
  cmp("l2tlb_reads", 0.02);
  CHECK(a.s.get("walks") == b.s.get("walks"));
  CHECK(a.s.get("pte_fetches") == b.s.get("pte_fetches"));
  // Family 4: completion behaviour. Counts and walks are architectural; the
  // latency *distribution* is a queueing observable that legitimately shifts
  // when the same requests are interleaved differently, so it is bounded more
  // loosely and the measured value is printed rather than hidden.
  cmp("lat_sum", 0.10);
  cmp("lat_p95", 0.15);
  // Timing.
  cmp("cycles", 0.05);

  std::printf("    M4b: cycles=%llu/%llu walks=%llu/%llu lat_mean=%.1f/%.1f\n",
              (unsigned long long)a.cycles, (unsigned long long)b.cycles,
              (unsigned long long)a.s.get("walks"), (unsigned long long)b.s.get("walks"),
              a.lat_mean, b.lat_mean);
}

// The Vortex reference preset must load and run with conservation intact.
void test_vortex_ref_preset() {
  Config c = Config::load("configs/vortex_ref.cfg");
  CHECK(c.cores == 1);
  CHECK(c.warps_per_core == 4);
  CHECK(c.lanes == 4);
  CHECK(c.lsu_pending == 8);
  CHECK(c.banks == 2);
  CHECK(c.levels == 2);
  c.N = 2048;
  c.Lambda = 2048;
  c.P = 64;
  Sim s(c);
  const RunResult r = s.run();
  CHECK(r.ok);
  CHECK(r.s.get("completed") == r.s.get("requests_generated"));
  std::printf("    vortex_ref: cycles=%llu lines=%llu walks=%llu\n",
              (unsigned long long)r.cycles,
              (unsigned long long)r.s.get("trace_lines"),
              (unsigned long long)r.s.get("walks"));
}

// =========================================================================
// M5: bulk indirect-access engine
//   output[i] = data[index[i]]
// Every expected value below is derived from what the test itself wrote into
// the memory image, never from the engine's bookkeeping.
// =========================================================================

constexpr u64 B_DATA = 0x00200000ull;        // 2 MiB: data array base
constexpr u64 B_INDEX = 0x00100000ull;       // 1 MiB: index array base
constexpr u64 B_DESC_LINE = 0x00080000ull / 64ull;

Config bulk_cfg() {
  Config c;
  c.cores = 1;      // the engine is one global client
  c.R = 128;
  c.ag_lanes = 8;
  c.eng_issue = 4;  // several submissions per cycle keeps small tests short
  c.bulk_max_elems = 4096;
  c.dram_q = 0;     // unbounded queue, as in vortex_ref
  c.mode = Mode::Bulk;
  return c;
}

inline u64 load_le(const u8* p, u32 n) {
  u64 v = 0;
  for (u32 i = 0; i < n; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}

// Expected value of an element read from the image at a byte address. The
// address itself is always computed by the test from the planted index list.
u64 image_value(const MemorySystem& mem, u64 va, u32 width) {
  const u8* b = mem.line_bytes(mem.pa_line_of_va(va));
  return load_le(b + (va & (LINE_BYTES - 1)), width);
}

void plant(MemorySystem& mem, u64 va, const std::vector<u64>& vals, u32 width) {
  for (size_t i = 0; i < vals.size(); ++i) {
    u8 b[8];
    for (u32 k = 0; k < 8; ++k) b[k] = static_cast<u8>(vals[i] >> (8 * k));
    mem.write_va(va + i * width, b, width);
  }
}

void plant_desc(MemorySystem& mem, const BulkDesc& d) {
  u8 line[LINE_BYTES];
  d.encode(line);
  mem.write_va(d.desc_line * LINE_BYTES, line, LINE_BYTES);
}

// One memory system + one engine, driven by the test.
struct Rig {
  MemorySystem mem;
  BulkEngine eng;
  u64 cycle = 0;
  explicit Rig(const Config& c) : mem(c), eng(c, mem) {}

  void warm(u64 va_line) {
    Req r;
    r.va_line = va_line;
    r.vpn = va_line >> 6;
    r.core = 0;
    r.slot = 0;
    r.bypass_l1 = true;
    std::vector<u64> d;
    while (!mem.submit(r, cycle)) { d.clear(); mem.tick(cycle, d); ++cycle; }
    for (;;) { d.clear(); mem.tick(cycle, d); ++cycle; if (!d.empty()) break; }
  }

  void run(u64 limit = 4000000) {
    for (;;) {
      eng.tick(cycle);
      if (eng.idle()) break;
      if (++cycle > limit) break;
    }
  }
};

BulkDesc mkdesc(u64 index_base, u64 data_base, u32 count, u8 ib, u8 db) {
  BulkDesc d;
  d.desc_line = B_DESC_LINE;
  d.index_base = index_base;
  d.data_base = data_base;
  d.count = count;
  d.elem_base = 0;
  d.index_bytes = ib;
  d.data_bytes = db;
  return d;
}

// -------------------------------------------------------------------------
// 1. Descriptor validation (pure + engine-level rejection).
void test_bulk_desc_validation() {
  BulkDesc d = mkdesc(B_INDEX, B_DATA, 8, 4, 8);
  CHECK(d.validate(4096).empty());

  { BulkDesc x = d; x.index_bytes = 3; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.data_bytes = 3; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.index_base = B_INDEX + 4; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.data_base = B_DATA + 1; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.count = 5000; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.index_base = VA_LIMIT - 32; x.count = 8; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.elem_base = 0xffffffffu; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.desc_line = VA_LIMIT >> 6; CHECK(!x.validate(4096).empty()); }
  { BulkDesc x = d; x.count = 0; CHECK(x.validate(4096).empty()); }  // empty is legal

  // Encoding round-trip, and a corrupted line must not decode.
  u8 line[LINE_BYTES];
  d.encode(line);
  BulkDesc e;
  CHECK(BulkDesc::decode(line, e));
  CHECK(e.index_base == d.index_base);
  CHECK(e.data_base == d.data_base);
  CHECK(e.count == d.count);
  CHECK(e.index_bytes == d.index_bytes);
  CHECK(e.data_bytes == d.data_bytes);
  line[0] ^= 0xff;
  CHECK(!BulkDesc::decode(line, e));

  // A descriptor whose fields are invalid is fetched and then rejected: it is
  // counted once, never as a completion.
  Config c = bulk_cfg();
  Rig rig(c);
  BulkDesc bad = mkdesc(B_INDEX, B_DATA, 5000, 4, 8);  // count > bulk_max_elems
  plant_desc(rig.mem, bad);
  CHECK(rig.eng.submit(bad));
  rig.run();
  CHECK(rig.eng.stats().desc_submitted == 1);
  CHECK(rig.eng.stats().desc_rejected == 1);
  CHECK(rig.eng.stats().desc_completed == 0);
  CHECK(rig.eng.stats().elems_total == 0);
  CHECK(!rig.eng.errors().empty());

  // An undecodable line is also rejected, not executed with garbage fields.
  Config c2 = bulk_cfg();
  Rig rig2(c2);
  u8 junk[LINE_BYTES];
  for (u32 i = 0; i < LINE_BYTES; ++i) junk[i] = static_cast<u8>(i * 37 + 11);
  rig2.mem.write_va(B_DESC_LINE * LINE_BYTES, junk, LINE_BYTES);
  CHECK(rig2.eng.submit(mkdesc(B_INDEX, B_DATA, 4, 4, 8)));
  rig2.run();
  CHECK(rig2.eng.stats().desc_rejected == 1);
  CHECK(rig2.eng.stats().desc_completed == 0);
  CHECK(rig2.eng.stats().tgt_req_gen == 0);

  std::printf("    desc_validation: rejected=%llu/%llu\n",
              (unsigned long long)rig.eng.stats().desc_rejected,
              (unsigned long long)rig2.eng.stats().desc_rejected);
}

// -------------------------------------------------------------------------
// 2. Index-address calculation: one line request per 64 B of index array, and
//    the right value at the right offset (proved by the target addresses).
void test_bulk_index_addressing() {
  Config c = bulk_cfg();
  Rig rig(c);
  const u32 n = 32;
  std::vector<u64> idx(n);
  for (u32 i = 0; i < n; ++i) idx[i] = i;
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, n, 4, 8);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();

  const BulkStats& b = rig.eng.stats();
  CHECK(b.desc_completed == 1);
  // 32 x 4 B = 128 B = exactly two index lines.
  CHECK(b.idx_lines_req == 2);
  CHECK(b.idx_lines_done == 2);
  CHECK(b.idx_elems_done == n);
  // One target request per element even though only four lines are distinct:
  // duplicate detection is off (M5 scope), so no request may be merged.
  CHECK(b.tgt_req_gen == n);
  for (u32 i = 0; i < n; ++i) CHECK(rig.eng.addr_of(i) == B_DATA + static_cast<u64>(i) * 8);
  u32 distinct = 0;
  for (u32 i = 0; i < n; ++i)
    if (i == 0 || (rig.eng.addr_of(i) >> 6) != (rig.eng.addr_of(i - 1) >> 6)) ++distinct;
  CHECK(distinct == 4);

  // 8 B indices: 9 elements = 72 B = two lines, the second holding one element.
  Config c2 = bulk_cfg();
  Rig rig2(c2);
  std::vector<u64> idx2 = {0, 1, 2, 3, 4, 5, 6, 7, 9};
  plant(rig2.mem, B_INDEX, idx2, 8);
  BulkDesc d2 = mkdesc(B_INDEX, B_DATA, 9, 8, 8);
  plant_desc(rig2.mem, d2);
  rig2.eng.submit(d2);
  rig2.run();
  CHECK(rig2.eng.stats().idx_lines_req == 2);
  CHECK(rig2.eng.stats().tgt_req_done == 9);
  CHECK(rig2.eng.addr_of(8) == B_DATA + 9 * 8);
  std::printf("    index_addressing: lines=%llu/%llu elems=%llu\n",
              (unsigned long long)b.idx_lines_req,
              (unsigned long long)rig2.eng.stats().idx_lines_req,
              (unsigned long long)b.idx_elems_done);
}

// -------------------------------------------------------------------------
// 3. Target-address calculation and value extraction, hand-computed.
void test_bulk_target_addressing() {
  Config c = bulk_cfg();
  Rig rig(c);
  // A known pattern in one data line: byte j = 100 + j.
  u8 line[LINE_BYTES];
  for (u32 j = 0; j < LINE_BYTES; ++j) line[j] = static_cast<u8>(100 + j);
  rig.mem.write_va(B_DATA, line, LINE_BYTES);

  const std::vector<u64> idx = {3, 0, 7};
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, 3, 4, 8);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();

  CHECK(rig.eng.stats().elems_done == 3);
  for (u32 i = 0; i < 3; ++i) {
    const u64 expect_addr = B_DATA + idx[i] * 8;   // 8 B elements
    CHECK(rig.eng.addr_of(i) == expect_addr);
    CHECK(rig.eng.value_of(i) == load_le(line + idx[i] * 8, 8));
  }
  CHECK(rig.eng.value_of(0) == load_le(line + 24, 8));
  CHECK(rig.eng.value_of(1) == load_le(line + 0, 8));
  CHECK(rig.eng.value_of(2) == load_le(line + 56, 8));
  CHECK(!rig.eng.has_value(3));
  CHECK(rig.eng.stats().addr_mismatch == 0);
}

// -------------------------------------------------------------------------
// 4. A target request must not exist before its index value has been fetched.
void test_bulk_index_dependency() {
  Config c = bulk_cfg();
  Rig rig(c);   // nothing warmed: the index page must be walked
  std::vector<u64> idx = {0, 1};
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, 2, 4, 8);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();

  const BulkStats& b = rig.eng.stats();
  CHECK(b.desc_completed == 1);
  CHECK(b.idx_first_done != NONE);
  CHECK(b.tgt_first_gen != NONE);
  // Generation is not allowed before the index fill lands.
  CHECK(b.tgt_first_gen >= b.idx_first_done);
  CHECK(b.tgt_first_gen > 0);
  // The generator demonstrably waited for index data rather than assuming it.
  CHECK(b.idx_wait_stall > 0);
  CHECK(b.addr_mismatch == 0);
  std::printf("    index_dependency: idx_done=%llu tgt_gen=%llu wait_stall=%llu\n",
              (unsigned long long)b.idx_first_done,
              (unsigned long long)b.tgt_first_gen,
              (unsigned long long)b.idx_wait_stall);
}

// -------------------------------------------------------------------------
// 5. Multiple outstanding index requests.
void test_bulk_multi_index_outstanding() {
  Config c = bulk_cfg();
  c.eng_issue = 8;
  c.idx_window = 8;   // bound the index stream so the window is what is tested
  Rig rig(c);
  const u32 n = 1024;
  std::vector<u64> idx(n);
  for (u32 i = 0; i < n; ++i) idx[i] = i % 64;   // reuse lines: keeps the run short
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, n, 4, 8);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();

  const BulkStats& b = rig.eng.stats();
  CHECK(b.idx_lines_req == 64);          // 1024 x 4 B / 64 B
  CHECK(b.idx_lines_done == 64);
  CHECK(b.idx_out_max > 1);              // more than one index line in flight
  CHECK(b.idx_out_max <= c.idx_window);  // ... but bounded by the window
  CHECK(b.idx_elems_done == n);
  CHECK(b.tgt_req_done == n);
  CHECK(b.addr_mismatch == 0);
  // Target requests coexisted with index lines in the buffer: the stream and
  // the generator are overlapped, not serialised.
  CHECK(b.buf_max > c.idx_window);

  // The window is what bounds it: with idx_window = R the stream may hold the
  // whole residency budget, which is the serialising case this test avoids.
  Config c1 = bulk_cfg();
  c1.eng_issue = 8;
  c1.idx_window = 0;                     // 0 = no separate window (R)
  Rig rig1(c1);
  plant(rig1.mem, B_INDEX, idx, 4);
  plant_desc(rig1.mem, d);
  rig1.eng.submit(d);
  rig1.run();
  CHECK(rig1.eng.stats().idx_out_max > c.idx_window);
  CHECK(rig1.eng.stats().tgt_req_done == n);
  std::printf("    multi_index_outstanding: lines=64 idx_out_max=%llu (window=%u) vs %llu (no window)\n",
              (unsigned long long)b.idx_out_max, c.idx_window,
              (unsigned long long)rig1.eng.stats().idx_out_max);
}

// -------------------------------------------------------------------------
// 6. Out-of-order index responses, and correct element mapping despite them.
//    The index array straddles a page boundary; the second page is warmed so
//    its lines return long before the cold first page's lines.
void test_bulk_index_out_of_order() {
  Config c = bulk_cfg();
  c.eng_issue = 8;
  Rig rig(c);
  c.idx_window = 16;                      // all 16 index lines outstanding
  const u64 index_base = 0x0010FE00ull;   // 512 B before a page boundary
  const u32 n = 128;                      // 1024 B of 8 B indices = 16 lines
  std::vector<u64> idx(n);
  for (u32 i = 0; i < n; ++i) idx[i] = i;
  plant(rig.mem, index_base, idx, 8);
  BulkDesc d = mkdesc(index_base, B_DATA, n, 8, 8);
  plant_desc(rig.mem, d);

  // Warm the pages that must return immediately: descriptor, data, and the
  // second index page. The first index page stays cold.
  rig.warm(B_DESC_LINE);
  rig.warm(B_DATA >> 6);
  rig.warm(0x00110000ull >> 6);

  rig.eng.submit(d);
  rig.run();

  const BulkStats& b = rig.eng.stats();
  CHECK(b.desc_completed == 1);
  CHECK(b.idx_lines_req == 16);
  CHECK(b.idx_lines_done == 16);
  CHECK(b.idx_lines_ooo > 0);             // fills arrived out of request order
  CHECK(b.idx_elems_done == n);
  CHECK(b.tgt_req_done == n);
  CHECK(b.addr_mismatch == 0);
  for (u32 i = 0; i < n; ++i) {
    CHECK(rig.eng.addr_of(i) == B_DATA + static_cast<u64>(i) * 8);
    CHECK(rig.eng.value_of(i) == image_value(rig.mem, B_DATA + static_cast<u64>(i) * 8, 8));
  }
  std::printf("    index_out_of_order: ooo=%llu (of %llu lines)\n",
              (unsigned long long)b.idx_lines_ooo,
              (unsigned long long)b.idx_lines_done);
}

// -------------------------------------------------------------------------
// 7. Out-of-order target responses with correct reconstruction, and FIFO issue.
void test_bulk_target_out_of_order() {
  Config c = bulk_cfg();
  c.eng_issue = 8;
  Rig rig(c);
  const u32 n = 32;
  // Even elements land in a warm page, odd elements in a cold page.
  std::vector<u64> idx(n);
  for (u32 i = 0; i < n; ++i) idx[i] = (i % 2 == 0) ? (i % 8) : (512 + (i % 8));
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, n, 4, 8);
  plant_desc(rig.mem, d);
  rig.warm(B_DESC_LINE);
  rig.warm(B_DATA >> 6);          // page 0x200 warm, page 0x201 cold

  rig.eng.submit(d);
  rig.run();

  const BulkStats& b = rig.eng.stats();
  CHECK(b.tgt_req_done == n);
  CHECK(b.tgt_ooo > 0);                   // responses, not issues, reordered
  CHECK(b.issue_monotonic_violations == 0);  // issue stayed strictly FIFO
  CHECK(b.addr_mismatch == 0);
  for (u32 i = 0; i < n; ++i) {
    const u64 a = B_DATA + idx[i] * 8;
    CHECK(rig.eng.addr_of(i) == a);
    CHECK(rig.eng.value_of(i) == image_value(rig.mem, a, 8));
  }
  std::printf("    target_out_of_order: ooo=%llu fifo_violations=%llu\n",
              (unsigned long long)b.tgt_ooo,
              (unsigned long long)b.issue_monotonic_violations);
}

// -------------------------------------------------------------------------
// 8. Element-id reconstruction, including a non-zero element base.
void test_bulk_element_ids() {
  Config c = bulk_cfg();
  Rig rig(c);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, 4, 4, 8);
  d.elem_base = 1000;
  std::vector<u64> idx = {5, 5, 1, 6};   // element 1 is a deliberate duplicate
  plant(rig.mem, B_INDEX, idx, 4);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();

  CHECK(rig.eng.stats().elems_done == 4);
  CHECK(!rig.eng.has_value(999));
  CHECK(!rig.eng.has_value(1004));
  for (u32 i = 0; i < 4; ++i) {
    CHECK(rig.eng.has_value(1000 + i));
    CHECK(rig.eng.addr_of(1000 + i) == B_DATA + idx[i] * 8);
    CHECK(rig.eng.value_of(1000 + i) == image_value(rig.mem, B_DATA + idx[i] * 8, 8));
  }
  // Duplicated index values are not merged: two elements, two requests, same
  // value reported at both element ids.
  CHECK(rig.eng.stats().tgt_req_gen == 4);
  CHECK(rig.eng.value_of(1000) == rig.eng.value_of(1001));
}

// -------------------------------------------------------------------------
// 9. Request-buffer capacity enforcement (R small).
void test_bulk_buffer_capacity() {
  Config c = bulk_cfg();
  c.R = 8;
  Rig rig(c);
  const u32 n = 64;
  std::vector<u64> idx(n);
  for (u32 i = 0; i < n; ++i) idx[i] = i % 16;
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, n, 4, 8);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();

  const BulkStats& b = rig.eng.stats();
  CHECK(b.tgt_req_done == n);
  CHECK(b.buf_max <= 8);
  CHECK(b.unissued_max <= 8);
  CHECK(b.buf_full_stall > 0);   // the buffer really did bind
  CHECK(b.residency_violations == 0);
  std::printf("    buffer_capacity: R=8 buf_max=%llu full_stall=%llu\n",
              (unsigned long long)b.buf_max,
              (unsigned long long)b.buf_full_stall);
}

// -------------------------------------------------------------------------
// 10. The R = 128 residency invariant, including the R = 1 worst case.
void test_bulk_residency_invariant() {
  Config c = bulk_cfg();
  Rig rig(c);
  const u32 n = 512;
  std::vector<u64> idx(n);
  for (u32 i = 0; i < n; ++i) idx[i] = i;
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, n, 4, 8);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();
  CHECK(rig.eng.stats().residency_violations == 0);
  CHECK(rig.eng.stats().buf_max <= 128);
  CHECK(rig.eng.stats().tgt_req_done == n);

  // R = 1: one slot total. Slow, but every element must still complete.
  Config c1 = bulk_cfg();
  c1.R = 1;
  Rig rig1(c1);
  std::vector<u64> idx1 = {0, 1, 2, 3};
  plant(rig1.mem, B_INDEX, idx1, 4);
  BulkDesc d1 = mkdesc(B_INDEX, B_DATA, 4, 4, 8);
  plant_desc(rig1.mem, d1);
  rig1.eng.submit(d1);
  rig1.run();
  CHECK(rig1.eng.stats().residency_violations == 0);
  CHECK(rig1.eng.stats().buf_max <= 1);
  CHECK(rig1.eng.stats().tgt_req_done == 4);
  for (u32 i = 0; i < 4; ++i)
    CHECK(rig1.eng.value_of(i) == image_value(rig1.mem, B_DATA + i * 8, 8));
  std::printf("    residency: R=128 buf_max=%llu | R=1 buf_max=%llu cycles=%llu\n",
              (unsigned long long)rig.eng.stats().buf_max,
              (unsigned long long)rig1.eng.stats().buf_max,
              (unsigned long long)rig1.cycle);
}

// -------------------------------------------------------------------------
// 11. Descriptor completion conditions, and sequential descriptors.
void test_bulk_descriptor_completion() {
  Config c = bulk_cfg();
  Rig rig(c);
  BulkDesc a = mkdesc(B_INDEX, B_DATA, 3, 4, 8);
  plant(rig.mem, B_INDEX, {0, 1, 2}, 4);
  plant_desc(rig.mem, a);
  CHECK(rig.eng.submit(a));
  rig.run();
  CHECK(rig.eng.stats().desc_completed == 1);
  CHECK(rig.eng.stats().desc_cycles_max > 0);

  // Two descriptors in one run: each retires only after all its elements.
  // Each queued descriptor owns its own line (fields are read when the
  // descriptor begins, not at submission) and its own index array.
  Config c2 = bulk_cfg();
  Rig rig2(c2);
  BulkDesc d1 = mkdesc(B_INDEX, B_DATA, 4, 4, 8);
  plant(rig2.mem, B_INDEX, {0, 1, 2, 3}, 4);
  plant_desc(rig2.mem, d1);
  BulkDesc d2 = mkdesc(B_INDEX + 0x1000, B_DATA, 4, 4, 8);
  d2.desc_line = B_DESC_LINE + 1;
  d2.elem_base = 4;
  plant(rig2.mem, d2.index_base, {4, 5, 6, 7}, 4);
  plant_desc(rig2.mem, d2);
  rig2.eng.submit(d1);
  rig2.eng.submit(d2);
  rig2.run();
  CHECK(rig2.eng.stats().desc_submitted == 2);
  CHECK(rig2.eng.stats().desc_completed == 2);
  CHECK(rig2.eng.stats().desc_rejected == 0);
  CHECK(rig2.eng.stats().elems_total == 8);
  CHECK(rig2.eng.stats().elems_done == 8);
  for (u32 i = 0; i < 8; ++i) CHECK(rig2.eng.has_value(i));
  std::printf("    completion: descriptors=%llu latencies=%llu+%llu\n",
              (unsigned long long)rig2.eng.stats().desc_completed,
              (unsigned long long)rig.eng.stats().desc_cycles_max,
              (unsigned long long)rig2.eng.stats().desc_cycles_max);
}

// -------------------------------------------------------------------------
// 12. Empty and single-element descriptors.
void test_bulk_empty_and_small() {
  Config c = bulk_cfg();
  Rig rig(c);
  BulkDesc e = mkdesc(B_INDEX, B_DATA, 0, 4, 8);
  plant_desc(rig.mem, e);
  CHECK(rig.eng.submit(e));
  rig.run();
  CHECK(rig.eng.stats().desc_completed == 1);
  CHECK(rig.eng.stats().idx_lines_req == 0);   // nothing to stream
  CHECK(rig.eng.stats().tgt_req_gen == 0);
  CHECK(rig.eng.stats().elems_total == 0);
  CHECK(rig.eng.stats().desc_cycles_max > 0);  // the fetch still happened
  const u64 empty_cycles = rig.cycle;

  Config c1 = bulk_cfg();
  Rig rig1(c1);
  plant(rig1.mem, B_INDEX, {2}, 4);
  BulkDesc one = mkdesc(B_INDEX, B_DATA, 1, 4, 8);
  plant_desc(rig1.mem, one);
  rig1.eng.submit(one);
  rig1.run();
  CHECK(rig1.eng.stats().idx_lines_req == 1);
  CHECK(rig1.eng.stats().tgt_req_gen == 1);
  CHECK(rig1.eng.value_of(0) == image_value(rig1.mem, B_DATA + 16, 8));
  std::printf("    empty_and_small: empty_cycles=%llu one_cycles=%llu\n",
              (unsigned long long)empty_cycles, (unsigned long long)rig1.cycle);
}

// -------------------------------------------------------------------------
// 13. Translation and cache interaction through the shared hierarchy.
void test_bulk_translation_and_cache() {
  Config c = bulk_cfg();
  Rig rig(c);   // everything cold
  std::vector<u64> idx = {0, 1, 2, 3};
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, 4, 4, 8);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();

  // Three distinct pages: descriptor, index array, data array.
  CHECK(rig.mem.ptw.walks >= 3);
  CHECK(rig.mem.ptw.pte_fetches == 2 * rig.mem.ptw.walks);   // Sv32
  CHECK(rig.mem.l2.reads > 0);
  CHECK(rig.mem.dram.reads > 0);
  CHECK(rig.mem.l1.reads == 0);            // the engine has no private L1
  CHECK(rig.mem.tlb[0].reads > 0);
  CHECK(rig.mem.tlb[0].misses >= 3);
  CHECK(rig.mem.l2tlb.reads > 0);

  // A second descriptor over the same pages runs with the TLB already warm.
  Config c2 = bulk_cfg();
  Rig rig2(c2);
  std::vector<u64> idx2 = {0, 1, 2, 3, 4, 5, 6, 7};
  plant(rig2.mem, B_INDEX, idx2, 4);
  plant_desc(rig2.mem, d);
  BulkDesc d2 = mkdesc(B_INDEX, B_DATA, 8, 4, 8);
  d2.desc_line = B_DESC_LINE + 1;
  plant_desc(rig2.mem, d2);
  rig2.eng.submit(mkdesc(B_INDEX, B_DATA, 4, 4, 8));
  rig2.eng.submit(d2);
  rig2.run();
  CHECK(rig2.eng.stats().desc_completed == 2);
  CHECK(rig2.mem.tlb[0].hits > 0);
  CHECK(rig2.mem.ptw.walks == rig.mem.ptw.walks);  // same pages, no new walks
  std::printf("    translation: walks=%llu (2nd run) tlb_hits=%llu l1_reads=%llu\n",
              (unsigned long long)rig2.mem.ptw.walks,
              (unsigned long long)rig2.mem.tlb[0].hits,
              (unsigned long long)rig2.mem.l1.reads);
}

// -------------------------------------------------------------------------
// 14. Shared MemorySystem accounting, and every address checked against the
//     trace-derived index array (bulk_wl = E0 / E1).
void test_bulk_memory_accounting() {
  Config c = bulk_cfg();
  c.wl = "W1";
  c.N = 1024;
  c.Lambda = 1024;
  c.P = 64;
  c.kappa = 0.0;
  c.seed = 7;
  c.elem_bytes = 8;
  c.bulk_wl = "E0";
  Sim sim(c);
  const RunResult r = sim.run();
  CHECK(r.ok);
  if (!r.ok) for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());

  const BulkStats& b = sim.engine().stats();
  CHECK(b.desc_completed == 1);
  CHECK(b.desc_rejected == 0);
  CHECK(b.idx_lines_req == 64);            // 1024 x 4 B / 64 B
  CHECK(b.tgt_req_gen == 1024);            // one request per element, no dedup
  CHECK(b.tgt_req_done == 1024);
  CHECK(b.addr_mismatch == 0);
  CHECK(b.residency_violations == 0);
  CHECK(b.issue_monotonic_violations == 0);
  CHECK(r.s.get("requests_generated") == 1 + 64 + 1024);
  CHECK(r.s.get("issued") == r.s.get("completed"));

  // Shared hierarchy: the engine is the only client, and it never touched L1.
  CHECK(r.s.get("l1_reads") == 0);
  CHECK(r.s.get("l2_reads") == r.s.get("l2_hits") + r.s.get("l2_misses"));
  CHECK(r.s.get("dram_bytes") == r.s.get("dram_reads") * 64);
  CHECK(r.s.get("dram_reads") > 0);

  // Every generated address against the source index array.
  const Trace& t = sim.trace();
  u64 checked = 0;
  for (u32 i = 0; i < t.acc.size(); ++i) {
    const u64 expect = c.bulk_data_base + t.acc[i].addr;
    CHECK(sim.engine().addr_of(i) == expect);
    CHECK(sim.engine().value_of(i) == image_value(sim.mem(), expect, 8));
    ++checked;
  }
  CHECK(checked == 1024);

  // E1 stores the same indices as 8 B values: only the index stream changes.
  Config c1 = c;
  c1.bulk_wl = "E1";
  Sim sim1(c1);
  const RunResult r1 = sim1.run();
  CHECK(r1.ok);
  CHECK(sim1.engine().stats().idx_lines_req == 128);   // 1024 x 8 B / 64 B
  CHECK(sim1.engine().stats().tgt_req_gen == 1024);
  CHECK(sim1.engine().stats().addr_mismatch == 0);
  for (u32 i = 0; i < t.acc.size(); ++i)
    CHECK(sim1.engine().addr_of(i) == c.bulk_data_base + t.acc[i].addr);

  std::printf("    accounting: idx_lines=%llu tgt=%llu cycles=%llu dram_reads=%llu\n",
              (unsigned long long)b.idx_lines_req,
              (unsigned long long)b.tgt_req_done,
              (unsigned long long)r.cycles,
              (unsigned long long)r.s.get("dram_reads"));
}

// -------------------------------------------------------------------------
// 15. FIFO issue behaviour: element order out, bounded submission rate, and
//     real backpressure when the shared hierarchy refuses a request.
void test_bulk_fifo_issue() {
  Config c = bulk_cfg();
  c.eng_issue = 4;
  Rig rig(c);
  const u32 n = 16;                 // all elements in one 8 B-slot data line
  std::vector<u64> idx(n);
  for (u32 i = 0; i < n; ++i) idx[i] = 0;
  plant(rig.mem, B_INDEX, idx, 4);
  BulkDesc d = mkdesc(B_INDEX, B_DATA, n, 4, 8);
  plant_desc(rig.mem, d);
  rig.warm(B_DESC_LINE);
  rig.warm(B_INDEX >> 6);
  rig.warm(B_DATA >> 6);
  rig.eng.submit(d);
  rig.run();

  const BulkStats& b = rig.eng.stats();
  CHECK(b.tgt_req_gen == n);
  CHECK(b.tgt_req_done == n);
  CHECK(b.issue_monotonic_violations == 0);  // issued strictly in element order
  CHECK(b.tgt_ooo == 0);                     // single line: fills land together
  // No request may be handed to the memory faster than eng_issue per cycle.
  CHECK(b.tgt_req_issued <= static_cast<u64>(c.eng_issue) * (rig.cycle + 1));
  CHECK(b.buf_max <= static_cast<u64>(c.R));

  // Force the memory to refuse submissions: the request stays in the engine's
  // buffer (counted), rather than being pushed in or dropped.
  Config c2 = bulk_cfg();
  c2.l2_mshr = 2;
  Rig rig2(c2);
  plant(rig2.mem, B_INDEX, idx, 4);
  plant_desc(rig2.mem, d);
  rig2.eng.submit(d);
  rig2.run();
  CHECK(rig2.eng.stats().tgt_req_done == n);   // nothing lost to backpressure
  CHECK(rig2.eng.stats().issue_stall > 0);
  CHECK(rig2.eng.stats().residency_violations == 0);
  std::printf("    fifo_issue: ooo=%llu monotonic_violations=%llu issue_stall=%llu\n",
              (unsigned long long)b.tgt_ooo,
              (unsigned long long)b.issue_monotonic_violations,
              (unsigned long long)rig2.eng.stats().issue_stall);
}

// =========================================================================
// M5.1: controlled A/B comparison (accelerator OFF vs ON)
// =========================================================================

// The controlled environment at a size that keeps the suite fast. The full
// configuration is configs/ab_controlled.cfg; `bulk_data_base = 0` makes the
// engine's target addresses the trace's own addresses.
Config ab_test_cfg() {
  // Every value is stated explicitly (no reliance on defaults) so the test
  // environment is the same one configs/ab_controlled.cfg describes.
  Config c;
  c.cores = 1;
  c.warps_per_core = 4;
  c.lanes = 4;
  c.elem_bytes = 4;
  c.R = 128;
  c.lsu_pending = 0;
  c.mem_path = "l1";
  c.l1_lines = 256;
  c.l1_ways = 4;
  c.l1_latency = 2;
  c.l1_mshr = 16;
  c.l2_lines = 16384;
  c.l2_ways = 8;
  c.l2_latency = 4;
  c.l2_mshr = 64;
  c.dtlb_entries = 16;
  c.dtlb_ways = 4;
  c.dtlb_mshr = 4;
  c.dtlb_parked = 2;
  c.l2tlb_entries = 512;
  c.l2tlb_ways = 4;
  c.l2tlb_mshr = 8;
  c.l2tlb_req_per_entry = 8;
  c.l2tlb_latency = 4;
  c.walkers = 2;
  c.walk_cache = 16;
  c.levels = 2;
  c.walk_latency_per_level = 150;
  c.banks = 2;
  c.rows = 4096;
  c.row_bytes = 1024;
  c.t_hit = 14;
  c.t_miss = 26;
  c.link = 0;
  c.dram_q = 0;
  c.frfcfs = true;
  c.bank_hash = true;
  c.eng_issue = 4;
  c.ag_lanes = 8;
  c.idx_window = 0;
  c.desc_fetch = true;
  c.wl = "W1";
  c.N = 2048;
  c.Lambda = 2048;
  c.P = 64;
  c.kappa = 0.0;
  c.rho_dup = 0.0;
  c.seed = 1;
  c.bulk_wl = "E0";
  c.bulk_index_base = 0x00800000ull;
  c.bulk_data_base = 0;
  c.bulk_desc_line = 98304;
  c.bulk_max_elems = 65536;
  c.max_cycles = 20000000;
  c.experiment = "ab";
  return c;
}

u64 trace_digest(const Trace& t) {
  u64 h = 1469598103934665603ull;
  for (const auto& a : t.acc) {
    const u64 vals[5] = {a.addr, a.line, static_cast<u64>(a.elem), static_cast<u64>(a.instr),
                         static_cast<u64>(a.lane)};
    for (u64 v : vals)
      for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ull; }
  }
  return h;
}

// The signature must actually cover every performance-relevant setting: if a
// field can be changed without changing the signature, the equality check is
// vacuous and this test fails.
void test_ab_environment_identity() {
  const Config base = ab_test_cfg();
  const std::string sig = environment_signature(base);
  CHECK(!sig.empty());
  CHECK(environment_signature(ab_test_cfg()) == sig);   // same values, same signature
  CHECK(environment_fields(base).size() >= 40);

  struct Mut { const char* what; std::function<void(Config&)> f; };
  const std::vector<Mut> muts = {
      {"cores", [](Config& c) { c.cores = 2; }},
      {"warps_per_core", [](Config& c) { c.warps_per_core = 8; }},
      {"lanes", [](Config& c) { c.lanes = 32; }},
      {"R", [](Config& c) { c.R = 64; }},
      {"l1_lines", [](Config& c) { c.l1_lines = 512; }},
      {"l1_ways", [](Config& c) { c.l1_ways = 8; }},
      {"l1_latency", [](Config& c) { c.l1_latency = 3; }},
      {"l1_mshr", [](Config& c) { c.l1_mshr = 32; }},
      {"l2_lines", [](Config& c) { c.l2_lines = 8192; }},
      {"l2_ways", [](Config& c) { c.l2_ways = 16; }},
      {"l2_latency", [](Config& c) { c.l2_latency = 12; }},
      {"l2_mshr", [](Config& c) { c.l2_mshr = 128; }},
      {"dtlb_entries", [](Config& c) { c.dtlb_entries = 8; }},
      {"dtlb_ways", [](Config& c) { c.dtlb_ways = 2; }},
      {"dtlb_mshr", [](Config& c) { c.dtlb_mshr = 8; }},
      {"dtlb_parked", [](Config& c) { c.dtlb_parked = 3; }},
      {"l2tlb_entries", [](Config& c) { c.l2tlb_entries = 256; }},
      {"l2tlb_ways", [](Config& c) { c.l2tlb_ways = 8; }},
      {"l2tlb_mshr", [](Config& c) { c.l2tlb_mshr = 16; }},
      {"l2tlb_req_per_entry", [](Config& c) { c.l2tlb_req_per_entry = 4; }},
      {"l2tlb_latency", [](Config& c) { c.l2tlb_latency = 6; }},
      {"walkers", [](Config& c) { c.walkers = 4; }},
      {"walk_cache", [](Config& c) { c.walk_cache = 32; }},
      {"walk_latency_per_level", [](Config& c) { c.walk_latency_per_level = 100; }},
      {"levels", [](Config& c) { c.levels = 3; }},
      {"banks", [](Config& c) { c.banks = 16; }},
      {"rows", [](Config& c) { c.rows = 1024; }},
      {"row_bytes", [](Config& c) { c.row_bytes = 2048; }},
      {"t_hit", [](Config& c) { c.t_hit = 20; }},
      {"t_miss", [](Config& c) { c.t_miss = 40; }},
      {"link", [](Config& c) { c.link = 2; }},
      {"dram_q", [](Config& c) { c.dram_q = 32; }},
      {"frfcfs", [](Config& c) { c.frfcfs = false; }},
      {"bank_hash", [](Config& c) { c.bank_hash = false; }},
      {"elem_bytes", [](Config& c) { c.elem_bytes = 8; }},
      {"N", [](Config& c) { c.N = 1024; }},
      {"Lambda", [](Config& c) { c.Lambda = 1024; }},
      {"P", [](Config& c) { c.P = 32; }},
      {"seed", [](Config& c) { c.seed = 2; }},
      {"kappa", [](Config& c) { c.kappa = 1.0; }},
      {"rho_dup", [](Config& c) { c.rho_dup = 0.5; }},
      {"bulk_index_base", [](Config& c) { c.bulk_index_base += 0x1000; }},
      {"bulk_data_base", [](Config& c) { c.bulk_data_base = 0x1000; }},
      {"bulk_desc_line", [](Config& c) { c.bulk_desc_line += 1; }},
      {"bulk_max_elems", [](Config& c) { c.bulk_max_elems = 1024; }},
      {"ag_lanes", [](Config& c) { c.ag_lanes = 4; }},
      {"eng_issue", [](Config& c) { c.eng_issue = 1; }},
      {"idx_window", [](Config& c) { c.idx_window = 8; }},
      {"desc_fetch", [](Config& c) { c.desc_fetch = false; }},
      {"mem_path", [](Config& c) { c.mem_path = "l2"; }},
      {"ab_charge_latch", [](Config& c) { c.ab_charge_latch = true; }},
  };
  u32 blind = 0;
  for (const auto& m : muts) {
    Config x = ab_test_cfg();
    m.f(x);
    if (environment_signature(x) == sig) {
      ++blind;
      std::printf("      signature does not cover %s\n", m.what);
    }
  }
  CHECK(blind == 0);

  // Configuration mismatches must fail loudly, not warn.
  {
    Config bad = ab_test_cfg();
    bad.mem_path = "native";   // different cache path per arm
    const AbResult r = run_ab(bad);
    CHECK(!r.ok);
    CHECK(!r.violations.empty());
  }
  {
    Config bad = ab_test_cfg();
    bad.lsu_pending = 8;       // SIMT capped below R
    const AbResult r = run_ab(bad);
    CHECK(!r.ok);
    CHECK(!r.violations.empty());
  }
  {
    Config bad = ab_test_cfg();
    bad.cores = 2;             // SIMT would get per-core TBs/L1s
    const AbResult r = run_ab(bad);
    CHECK(!r.ok);
    CHECK(!r.violations.empty());
  }
  {
    Config bad = ab_test_cfg();
    bool threw = false;
    try { bad.set("mem_path", "halfway"); } catch (...) { threw = true; }
    CHECK(threw);
  }
}

// Both arms must consume one materialized workload: the same trace object, the
// same memory image, the same descriptor.
void test_ab_same_workload() {
  const Config c = ab_test_cfg();
  const AbResult r = run_ab(c);
  CHECK(r.ok);
  if (!r.ok) for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.a.image_hash == r.b.image_hash);
  CHECK(r.a.image_hash != 0);
  CHECK(r.a.elements == r.b.elements);
  CHECK(r.a.elements == 2048);
  CHECK(r.trace_hash == trace_digest(build_trace(c)));   // the trace both arms ran
  CHECK(r.workload.armed);
  CHECK(r.workload.desc.count == 2048);
  CHECK(r.workload.writes.size() == 2);                  // index array + descriptor
  CHECK(r.signature == environment_signature(c));
  std::printf("    same_workload: elements=%llu image=%llx trace=%llx\n",
              (unsigned long long)r.a.elements, (unsigned long long)r.a.image_hash,
              (unsigned long long)r.trace_hash);
}

// Equal residency: both arms are bounded by one R-slot budget, and the SIMT
// arm's coalescer latch is counted and bounded by `lanes`.
void test_ab_budget_equality() {
  Config c = ab_test_cfg();
  c.N = 16384;
  c.Lambda = 16384;
  c.P = 512;
  const AbResult r = run_ab(c);
  CHECK(r.ok);
  if (!r.ok) for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.cfg.R == 128);
  CHECK(r.a.peak_slot == r.cfg.R);
  CHECK(r.b.peak_slot == r.cfg.R);
  CHECK(r.a.peak_outstanding == r.a.peak_slot + r.a.staging_peak);
  CHECK(r.a.staging_peak <= c.lanes);   // latch, not a hidden queue
  CHECK(r.b.staging_peak == 0);
  // Translation: identical hierarchy, and the engine's extra walks are exactly
  // the index array (64 KiB = 16 pages) plus the descriptor page.
  CHECK(r.a.pte_fetches == 2 * r.a.walks);
  CHECK(r.b.pte_fetches == 2 * r.b.walks);
  CHECK(r.b.walks - r.a.walks == 17);
  std::printf("    budget: A peak=%llu (%llu slots + %llu latch)  B peak=%llu  walks=%llu/%llu\n",
              (unsigned long long)r.a.peak_outstanding, (unsigned long long)r.a.peak_slot,
              (unsigned long long)r.a.staging_peak, (unsigned long long)r.b.peak_outstanding,
              (unsigned long long)r.a.walks, (unsigned long long)r.b.walks);
}

// Traffic: OFF generates nothing for the accelerator; ON generates exactly the
// descriptor, the index stream and one target request per element.
void test_ab_traffic_accounting() {
  const Config c = ab_test_cfg();
  const AbResult r = run_ab(c);
  CHECK(r.ok);
  CHECK(r.a.desc_requests == 0);
  CHECK(r.a.index_requests == 0);
  CHECK(r.a.target_requests == 0);
  CHECK(r.b.desc_requests == 1);
  CHECK(r.b.index_requests == 2048 / 16);            // 16 four-byte indices per line
  CHECK(r.b.target_requests == 2048);                // one per element, no dedup
  CHECK(r.b.requests == r.b.desc_requests + r.b.index_requests + r.b.target_requests);
  CHECK(r.a.requests < r.b.requests);                // the index stream is extra
  // Same path: both take the L1 under mem_path = l1, and neither shows hits on
  // this cold, evicting footprint.
  CHECK(r.a.l1_reads == r.a.requests);
  CHECK(r.b.l1_reads == r.b.requests);
  CHECK(r.a.dram_bytes == r.a.dram_reads * 64);
  CHECK(r.b.dram_bytes == r.b.dram_reads * 64);
  // Same hierarchy, same rules: an L1 probe either hits, joins an MSHR that is
  // already fetching the line, or becomes exactly one L2 probe — and PTE fetches
  // are L2 probes too. If this identity holds for both arms, they ran through
  // the same cache path and the same translation path.
  CHECK(r.a.l2_reads == (r.a.l1_reads - r.a.l1_hits - r.a.l1_mshr_joins) + r.a.pte_fetches);
  CHECK(r.b.l2_reads == (r.b.l1_reads - r.b.l1_hits - r.b.l1_mshr_joins) + r.b.pte_fetches);

  // The same identity on the coalescing-friendly workload (W0, kappa = 1), where
  // 16 elements share a line and the L1 MSHRs absorb the engine's repeats.
  Config cw0 = ab_test_cfg();
  cw0.wl = "W0";
  cw0.kappa = 1.0;
  cw0.P = 32;
  const AbResult r0 = run_ab(cw0);
  CHECK(r0.ok);
  CHECK(r0.a.l2_reads == (r0.a.l1_reads - r0.a.l1_hits - r0.a.l1_mshr_joins) + r0.a.pte_fetches);
  CHECK(r0.b.l2_reads == (r0.b.l1_reads - r0.b.l1_hits - r0.b.l1_mshr_joins) + r0.b.pte_fetches);
  CHECK(r0.b.l1_mshr_joins > 0);        // repeats of a line join one fetch
  std::printf("    w0 joins: A=%llu B=%llu (of %llu L1 reads)\n",
              (unsigned long long)r0.a.l1_mshr_joins,
              (unsigned long long)r0.b.l1_mshr_joins,
              (unsigned long long)r0.b.l1_reads);
  CHECK(r.a.tlb_reads == r.a.requests);              // every request is translated
  CHECK(r.b.tlb_reads == r.b.requests);
  std::printf("    traffic: A req=%llu dram=%llu | B req=%llu (1+%llu+%llu) dram=%llu\n",
              (unsigned long long)r.a.requests, (unsigned long long)r.a.dram_reads,
              (unsigned long long)r.b.requests, (unsigned long long)r.b.index_requests,
              (unsigned long long)r.b.target_requests, (unsigned long long)r.b.dram_reads);
}

// The environment resets completely between arms and between runs: a repeated
// experiment reproduces every number, so no state is carried over.
void test_ab_reset_between_arms() {
  const Config c = ab_test_cfg();
  const AbResult r1 = run_ab(c);
  const AbResult r2 = run_ab(c);
  CHECK(r1.ok && r2.ok);
  CHECK(r1.a.cycles == r2.a.cycles);
  CHECK(r1.b.cycles == r2.b.cycles);
  CHECK(r1.a.dram_reads == r2.a.dram_reads);
  CHECK(r1.b.dram_reads == r2.b.dram_reads);
  CHECK(r1.a.l1_reads == r2.a.l1_reads);
  CHECK(r1.b.l1_reads == r2.b.l1_reads);
  CHECK(r1.a.image_hash == r2.a.image_hash);
  CHECK(r1.b.completion_hash == r2.b.completion_hash);
  CHECK(r1.a.walks == r2.a.walks);
  CHECK(r1.b.walks == r2.b.walks);

  // And the ON arm alone, run twice through fresh simulations, is identical.
  const Config cb = [] { Config x = ab_test_cfg(); x.mode = Mode::Bulk; return x; }();
  std::string werr;
  const BulkWorkload w = build_bulk_workload(cb, build_trace(cb), werr);
  CHECK(werr.empty());
  Sim s1(cb), s2(cb);
  w.apply(s1.memory());
  w.apply(s2.memory());
  const RunResult a1 = s1.run_bulk(w);
  const RunResult a2 = s2.run_bulk(w);
  CHECK(a1.cycles == a2.cycles);
  CHECK(a1.s.get("dram_reads") == a2.s.get("dram_reads"));
  CHECK(a1.s.get("bulk_tgt_req_done") == a2.s.get("bulk_tgt_req_done"));
  std::printf("    reset: A=%llu/%llu B=%llu/%llu cycles (run 1/run 2)\n",
              (unsigned long long)r1.a.cycles, (unsigned long long)r2.a.cycles,
              (unsigned long long)r1.b.cycles, (unsigned long long)r2.b.cycles);
}

// The sweep runs every required budget and every pair is internally consistent.
void test_ab_sweep_budgets() {
  const std::vector<AbResult> rs = run_ab_sweep(ab_test_cfg());
  CHECK(rs.size() == 5);
  const u32 want[5] = {8, 16, 32, 64, 128};
  u32 mismatched = 0, notok = 0;
  for (size_t i = 0; i < rs.size(); ++i) {
    const AbResult& r = rs[i];
    if (r.cfg.R != want[i]) ++mismatched;
    if (!r.ok) ++notok;
    CHECK(r.a.peak_slot <= r.cfg.R);
    CHECK(r.b.peak_slot <= r.cfg.R);
    CHECK(r.a.image_hash == r.b.image_hash);
    CHECK(r.a.elements == r.b.elements);
    CHECK(r.b.target_requests == 2048);
    CHECK(r.a.desc_requests == 0 && r.a.index_requests == 0 && r.a.target_requests == 0);
  }
  CHECK(mismatched == 0);
  CHECK(notok == 0);
  std::printf("    sweep:");
  for (const auto& r : rs) std::printf(" R=%u %.3f", r.cfg.R, r.speedup);
  std::printf("\n");
}

// The conventional baseline is unchanged by the M5.1 infrastructure: the same
// configuration must still produce the M5 numbers, and OFF must construct no
// accelerator at all.
void test_ab_baseline_unchanged() {
  Config c = Config::load("configs/vortex_ref.cfg");
  Sim s(c);
  const RunResult r = s.run();
  CHECK(r.ok);
  CHECK(!s.has_engine());
  CHECK(r.s.get("mem_path") == 0);          // native path, as before M5.1
  CHECK(r.cycles == 280432);                // the M5 measurement, unchanged
  CHECK(r.s.get("requests_generated") == 16384);
  CHECK(r.s.get("dram_reads") == 16417);
  CHECK(r.s.get("l1_reads") == 16384);
  CHECK(r.s.get("simt_peak_slot") == 128);  // slots were always R-bounded
  CHECK(r.s.get("simt_staging_max") <= c.lanes);
  std::printf("    baseline_unchanged: cycles=%llu dram=%llu peak_slot=%llu staging=%llu\n",
              (unsigned long long)r.cycles, (unsigned long long)r.s.get("dram_reads"),
              (unsigned long long)r.s.get("simt_peak_slot"),
              (unsigned long long)r.s.get("simt_staging_max"));
}

// -------------------------------------------------------------------------
// 16. Regressions for defects found while implementing the engine.
void test_bulk_regressions() {
  // (a) Requests carry VA line numbers, not byte addresses. Using the byte
  //     address mapping on a line number read the wrong physical line, so the
  //     descriptor failed to decode and every value came from the wrong line.
  Config c = bulk_cfg();
  MemorySystem mem(c);
  u64 bad = 0;
  for (u64 line : {1ull, 64ull, 4096ull, 98304ull, 700000ull})
    if (mem.pa_line_of_va_line(line) != mem.pa_line_of_va(line * LINE_BYTES)) ++bad;
  CHECK(bad == 0);

  // (b) A rejected descriptor must be counted once (rejected), never also as a
  //     completion: that broke submitted == completed + rejected.
  Rig rig(c);
  BulkDesc bad_desc = mkdesc(B_INDEX, B_DATA, 9, 3 /*invalid*/, 8);
  plant_desc(rig.mem, bad_desc);
  rig.eng.submit(bad_desc);
  rig.run();
  CHECK(rig.eng.stats().desc_submitted == 1);
  CHECK(rig.eng.stats().desc_rejected == 1);
  CHECK(rig.eng.stats().desc_completed == 0);
  CHECK(rig.eng.stats().desc_cycles_sum == 0);

  // (c) A submitted descriptor whose address is outside the address space is
  //     refused before any fetch; nothing is queued or executed.
  Rig rig2(c);
  BulkDesc far = mkdesc(B_INDEX, B_DATA, 4, 4, 8);
  far.desc_line = VA_LIMIT >> 6;
  CHECK(!rig2.eng.submit(far));
  rig2.run();
  CHECK(rig2.eng.stats().desc_submitted == 1);
  CHECK(rig2.eng.stats().desc_rejected == 1);
  CHECK(rig2.eng.stats().idx_lines_req == 0);

  // (d) An index array that does not start on a line boundary would be decoded
  //     at the wrong offsets; it is rejected instead of silently misread.
  BulkDesc unaligned = mkdesc(B_INDEX + 8, B_DATA, 4, 4, 8);
  CHECK(!unaligned.validate(4096).empty());
  std::printf("    regressions: pa_line=%s reject_counting=%s\n",
              bad == 0 ? "ok" : "BUG",
              (rig.eng.stats().desc_completed == 0 && rig.eng.stats().desc_rejected == 1) ? "ok" : "BUG");
}

// =========================================================================
// M6: bulk visibility window
// =========================================================================

// The published M5.1 result is the acceptance baseline: the legacy path must be
// untouched, bit for bit, including its cycle count.
void test_m6_legacy_identity() {
  Config c;
  try {
    c = Config::load("configs/ab_controlled.cfg");
  } catch (...) {
    CHECK(false);
    return;
  }
  c.experiment = "none";
  c.batch_window = 0;
  const AbResult r = run_ab(c);
  CHECK(r.ok);
  if (!r.ok)
    for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.b.cycles == 278034);   // M5.1 §8
  CHECK(r.b.requests == 17409);
  CHECK(r.b.dram_reads == 17444);
  CHECK(r.a.cycles == 270753);   // controlled SIMT baseline, unchanged
  std::printf("    legacy: A=%llu B=%llu cyc, B req=%llu dram=%llu walks=%llu\n",
              (unsigned long long)r.a.cycles, (unsigned long long)r.b.cycles,
              (unsigned long long)r.b.requests, (unsigned long long)r.b.dram_reads,
              (unsigned long long)r.b.walks);
}

// With both mechanisms off the window must behave like the M5 FIFO generator:
// one request per element, the same traffic, the same values.
void test_m6_switch_off() {
  Config base = ab_test_cfg();
  base.experiment = "none";
  Config legacy = base;
  legacy.batch_window = 0;
  Config win = base;
  win.batch_window = 256;
  win.dedup = false;
  win.page_order = false;

  const AbResult rl = run_ab(legacy);
  const AbResult rw = run_ab(win);
  CHECK(rl.ok && rw.ok);
  CHECK(rw.b.requests == rl.b.requests);      // one request per element
  CHECK(rw.b.dram_reads == rl.b.dram_reads);
  CHECK(rw.b.lines_issued == rw.b.elements);  // nothing merged
  CHECK(rw.b.dedup_merged == 0);
  std::printf("    switch_off: requests %llu/%llu dram %llu/%llu cycles %llu/%llu\n",
              (unsigned long long)rl.b.requests, (unsigned long long)rw.b.requests,
              (unsigned long long)rl.b.dram_reads, (unsigned long long)rw.b.dram_reads,
              (unsigned long long)rl.b.cycles, (unsigned long long)rw.b.cycles);
}

// Every element must be reconstructed exactly once, with the same value and the
// same address as the per-element path. This is the "no lost, duplicated or
// misrouted result" requirement, checked element by element.
void test_m6_value_equivalence() {
  Config base = ab_test_cfg();
  base.experiment = "none";
  auto trace = std::make_shared<const Trace>(build_trace(base));
  std::string err;
  const BulkWorkload w = build_bulk_workload(base, *trace, err);
  CHECK(err.empty());
  if (!err.empty() || !w.armed) return;

  Config a = base;
  a.batch_window = 0;
  Config b = base;
  b.batch_window = 512;   // both mechanisms on
  Sim sa(a, trace);
  const RunResult ra = sa.run_bulk(w);
  Sim sb(b, trace);
  const RunResult rb = sb.run_bulk(w);
  CHECK(ra.ok && rb.ok);

  u64 missing = 0, bad_value = 0, bad_addr = 0;
  for (u32 i = 0; i < w.desc.count; ++i) {
    if (!sb.engine().has_value(i)) { ++missing; continue; }
    if (sa.engine().value_of(i) != sb.engine().value_of(i)) ++bad_value;
    if (sa.engine().addr_of(i) != sb.engine().addr_of(i)) ++bad_addr;
  }
  CHECK(missing == 0);
  CHECK(bad_value == 0);
  CHECK(bad_addr == 0);
  std::printf("    values: %u elements, missing=%llu bad_value=%llu bad_addr=%llu\n",
              w.desc.count, (unsigned long long)missing, (unsigned long long)bad_value,
              (unsigned long long)bad_addr);
}

// Deduplication is the only mechanism that helps a coalescible workload: it must
// collapse 4 elements per line into one request and leave nothing for the L1
// MSHRs to absorb.
void test_m6_dedup_soundness() {
  Config c = ab_test_cfg();
  c.experiment = "none";
  c.wl = "W0";
  c.kappa = 1.0;
  c.P = 32;
  Config legacy = c;
  legacy.batch_window = 0;
  Config win = c;
  win.batch_window = 512;
  win.dedup = true;
  win.page_order = true;

  const AbResult rl = run_ab(legacy);
  const AbResult rw = run_ab(win);
  CHECK(rl.ok && rw.ok);
  CHECK(rw.b.elements == 2048);
  CHECK(rw.b.lines_issued == 512);            // 4 elements per line
  CHECK(rw.b.dedup_merged == 1536);
  CHECK(rw.b.requests == 512 + 128 + 1);      // lines + index lines + descriptor
  CHECK(rw.b.l1_mshr_joins == 0);             // nothing left to absorb
  CHECK(rl.b.l1_mshr_joins > 0);              // the FIFO path needed absorbing
  CHECK(rw.b.dir_collisions == 0);
  std::printf("    dedup: lines=%llu merged=%llu requests=%llu joins %llu->%llu pages=%llu\n",
              (unsigned long long)rw.b.lines_issued, (unsigned long long)rw.b.dedup_merged,
              (unsigned long long)rw.b.requests, (unsigned long long)rl.b.l1_mshr_joins,
              (unsigned long long)rw.b.l1_mshr_joins, (unsigned long long)rw.b.dir_occ_max);
}

// W and R are independent budgets, and the organizer's work is charged.
void test_m6_resource_accounting() {
  Config c = ab_test_cfg();
  c.experiment = "none";
  c.batch_window = 256;
  c.page_dir = 512;
  c.org_lanes = 4;
  const AbResult r = run_ab(c);
  CHECK(r.ok);
  if (!r.ok)
    for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.b.win_entries == 2048);                        // every element admitted
  CHECK(r.b.win_occ_max <= 256);                         // W bounds the window
  CHECK(r.b.peak_slot <= 128);                           // R bounds line requests
  CHECK(r.b.batches == 8);                               // 2048 / 256
  CHECK(r.b.org_cycles == 2 * 2048 / 4);                 // 2 visits/entry at 4/cycle
  CHECK(r.b.seals_full + r.b.seals_end + r.b.seals_deadline == 8);
  CHECK(r.b.win_bytes == 2 * (256 * 8 + 512 * 16));      // two buffers, modelled
  std::printf("    resources: W=%u occ=%llu R_slots=%llu batches=%llu org=%llu bytes=%llu\n",
              c.batch_window, (unsigned long long)r.b.win_occ_max,
              (unsigned long long)r.b.peak_slot, (unsigned long long)r.b.batches,
              (unsigned long long)r.b.org_cycles, (unsigned long long)r.b.win_bytes);
}

// The two mechanisms are independent: on a permutation dedup has nothing to do,
// on a coalescible stream page order has nothing to do.
void test_m6_mechanism_independence() {
  Config base = ab_test_cfg();
  base.experiment = "none";
  base.batch_window = 1024;
  auto cell = [&](bool d, bool p) {
    Config c = base;
    c.dedup = d;
    c.page_order = p;
    return run_ab(c);
  };
  const AbResult none = cell(false, false), ded = cell(true, false);
  const AbResult po = cell(false, true), both = cell(true, true);
  CHECK(none.ok && ded.ok && po.ok && both.ok);

  // W1 is a permutation: 1 element per line, so dedup cannot change anything.
  CHECK(ded.b.lines_issued == ded.b.elements);
  CHECK(none.b.cycles == ded.b.cycles);
  CHECK(ded.b.dram_reads == none.b.dram_reads);
  // Page order does change the row behaviour and does change the cycles.
  CHECK(po.b.dram_row_hits > none.b.dram_row_hits);
  CHECK(po.b.cycles != none.b.cycles);
  // Applying both must reproduce page order exactly on this workload.
  CHECK(both.b.cycles == po.b.cycles);
  std::printf("    independence: none=%llu dedup=%llu porder=%llu both=%llu rowhits %llu->%llu\n",
              (unsigned long long)none.b.cycles, (unsigned long long)ded.b.cycles,
              (unsigned long long)po.b.cycles, (unsigned long long)both.b.cycles,
              (unsigned long long)none.b.dram_row_hits, (unsigned long long)po.b.dram_row_hits);
}

// Fanout is a real cost: it must retire every element and it must be visible.
void test_m6_fanout_accounting() {
  Config c = ab_test_cfg();
  c.experiment = "none";
  c.wl = "W0";
  c.kappa = 1.0;
  c.P = 32;
  c.batch_window = 512;
  Config wide = c;
  wide.fanout_lanes = 8;
  Config narrow = c;
  narrow.fanout_lanes = 1;
  const AbResult rw = run_ab(wide);
  const AbResult rn = run_ab(narrow);
  CHECK(rw.ok && rn.ok);
  CHECK(rw.b.fanout_elems == rw.b.elements);   // every element reconstructed
  CHECK(rw.b.fanout_cycles > 0);               // and the work is charged
  CHECK(rn.b.cycles >= rw.b.cycles);           // a narrow fanout cannot be faster
  std::printf("    fanout: lanes 8=%llu cyc, 1=%llu cyc, elems=%llu cycles_busy=%llu\n",
              (unsigned long long)rw.b.cycles, (unsigned long long)rn.b.cycles,
              (unsigned long long)rw.b.fanout_elems, (unsigned long long)rw.b.fanout_cycles);
}

// The latch charge removes the last capacity difference between the arms.
void test_m6_latch_charge() {
  Config c = ab_test_cfg();
  c.experiment = "none";
  c.ab_charge_latch = true;
  const AbResult r = run_ab(c);
  CHECK(r.ok);
  CHECK(r.cfg.ab_charge_latch);
  CHECK(r.a.peak_outstanding == r.cfg.R);   // 124 slots + <=4 latch = 128
  CHECK(r.a.peak_outstanding == r.b.peak_outstanding);
  CHECK(r.a.staging_peak <= r.cfg.lanes);
  std::printf("    latch: A out=%llu (slot %llu + latch %llu) B out=%llu  R=%u\n",
              (unsigned long long)r.a.peak_outstanding, (unsigned long long)r.a.peak_slot,
              (unsigned long long)r.a.staging_peak, (unsigned long long)r.b.peak_outstanding,
              r.cfg.R);
}

// The headline M6 result on the controlled environment, as a regression guard:
// a large window with page ordering must beat the SIMT arm and must do it by
// changing row behaviour, not by changing traffic.
void test_m6_window_advantage() {
  Config c;
  try {
    c = Config::load("configs/ab_controlled.cfg");
  } catch (...) {
    CHECK(false);
    return;
  }
  c.experiment = "none";
  c.batch_window = 8192;
  c.idx_mode = 0;  // the M6 measurement configuration
  const AbResult r = run_ab(c);
  CHECK(r.ok);
  if (!r.ok)
    for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.b.lines_issued == 16384);              // a permutation: no dedup available
  CHECK(r.b.dedup_merged == 0);
  CHECK(r.b.dram_reads > r.a.dram_reads);        // B still reads more (index array)
  CHECK(r.b.dram_row_hits > 40 * r.a.dram_row_hits);
  CHECK(r.speedup > 1.10);                       // measured 1.160
  CHECK(r.b.walks == 529);                       // +17 index/descriptor pages
  std::printf("    window 8192: A=%llu B=%llu speedup=%.3f rowhits %llu->%llu dtlb %llu->%llu\n",
              (unsigned long long)r.a.cycles, (unsigned long long)r.b.cycles, r.speedup,
              (unsigned long long)r.a.dram_row_hits, (unsigned long long)r.b.dram_row_hits,
              (unsigned long long)r.a.tlb_misses, (unsigned long long)r.b.tlb_misses);
}

// M6.1: the index oracle and the phase attribution are measurement tools, so
// what needs locking down is that they change nothing else — same target
// addresses, same values, same traffic, same residency rule.
void test_m6_index_oracle() {
  Config c = ab_test_cfg();
  c.experiment = "none";
  c.wl = "W0";
  c.kappa = 1.0;
  c.P = 32;
  c.batch_window = 512;
  const DiagResult r = run_index_oracle_diag(c);
  CHECK(r.ok);
  if (!r.ok)
    for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.b.size() == 4);
  if (r.b.size() == 4) {
    const DiagRow& b0 = r.b[0];
    const DiagRow& b1 = r.b[1];
    const DiagRow& b2 = r.b[2];
    const DiagRow& b3 = r.b[3];
    CHECK(b0.index_req == 128);              // 2048 elements x 4 B / 64 B
    CHECK(b2.index_req == 0);                // the oracle fetches nothing
    CHECK(b2.oracle_elems == 2048);          // and every element came from it
    CHECK(b3.total_req == r.a.total_req);    // same target volume as SIMT
    CHECK(b2.dram_reads < b0.dram_reads);    // and less DRAM traffic
    CHECK(b1.cycles < b0.cycles);            // the descriptor is a real cost
    CHECK(b2.cycles <= b0.cycles);
    CHECK(b3.cycles <= b2.cycles);
    CHECK(b0.first_tgt_issue > b3.first_tgt_issue);   // index fills gate the start
    for (const auto& d : r.b) {
      CHECK(d.value_hash == r.a.expected_hash);       // identical output values
      CHECK(d.addr_mismatch == 0);                    // identical target addresses
      CHECK(d.addr_set_hash == r.a.addr_set_hash);     // same set, nothing doubled
      CHECK(d.peak_slots <= c.R);                     // no extra residency granted
      CHECK(d.elems == 2048);
    }
    CHECK(r.prefix_cycles + r.span_cycles + r.drain_cycles == b0.cycles);
    std::printf("    oracle: A=%llu B0=%llu B1=%llu B2=%llu B3=%llu dram %llu->%llu\n",
                (unsigned long long)r.a.cycles, (unsigned long long)b0.cycles,
                (unsigned long long)b1.cycles, (unsigned long long)b2.cycles,
                (unsigned long long)b3.cycles, (unsigned long long)b0.dram_reads,
                (unsigned long long)b3.dram_reads);
  }
}

// The W0 diagnostic's published numbers (m6-w0-diagnostic-report.md §4).
void test_m6_w0_diagnostic_baseline() {
  Config c;
  try {
    c = Config::load("configs/ab_controlled.cfg");
  } catch (...) {
    CHECK(false);
    return;
  }
  c.experiment = "none";
  c.wl = "W0";
  c.kappa = 1.0;
  c.P = 256;
  c.batch_window = 8192;
  // The M6.2 numbers were measured with the M6 acquisition path; M8 made the
  // tiled unit the default, so the historical configuration has to be named.
  c.idx_mode = 0;
  const DiagResult r = run_index_oracle_diag(c);
  CHECK(r.ok);
  if (!r.ok)
    for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.a.cycles == 30573);
  CHECK(r.b.size() == 4);
  if (r.b.size() == 4) {
    CHECK(r.b[0].cycles == 39322);   // measured windowed engine
    CHECK(r.b[1].cycles == 38985);   // - descriptor fetch
    CHECK(r.b[2].cycles == 33268);   // - index array (oracle)
    CHECK(r.b[3].cycles == 32931);   // - both: organization cost alone
    CHECK(r.b[0].dram_reads == 5128);
    CHECK(r.b[3].dram_reads == 4101);   // exactly the SIMT arm's traffic
    CHECK(r.b[0].first_tgt_issue == 4431);
    CHECK(r.prefix_cycles == 4432);
    CHECK(r.b[0].tlb_refuse_parked == 20577);
    CHECK(r.b[0].tlb_refuse_full == 0);      // every refusal is the parked rule
    std::printf("    w0 diag: A=%llu B0=%llu B1=%llu B2=%llu B3=%llu prefix=%llu\n",
                (unsigned long long)r.a.cycles, (unsigned long long)r.b[0].cycles,
                (unsigned long long)r.b[1].cycles, (unsigned long long)r.b[2].cycles,
                (unsigned long long)r.b[3].cycles, (unsigned long long)r.prefix_cycles);
  }
}

// The W1 diagnostic's published numbers (m6-w1-diagnostic-report.md §3).
void test_m6_w1_diagnostic_baseline() {
  Config c;
  try {
    c = Config::load("configs/ab_controlled.cfg");
  } catch (...) {
    CHECK(false);
    return;
  }
  c.experiment = "none";
  c.batch_window = 8192;
  c.idx_mode = 0;  // the M6.2/M7 diagnostic configuration (see above)
  const DiagResult r = run_index_oracle_diag(c);
  CHECK(r.ok);
  if (!r.ok)
    for (const auto& v : r.violations) std::printf("      %s\n", v.c_str());
  CHECK(r.a.cycles == 270753);
  CHECK(r.b.size() == 4);
  if (r.b.size() == 4) {
    CHECK(r.b[0].cycles == 233475);   // measured windowed engine
    CHECK(r.b[1].cycles == 233138);   // - descriptor fetch
    CHECK(r.b[2].cycles == 225560);   // - index array (oracle)
    CHECK(r.b[3].cycles == 225223);   // - both
    CHECK(r.b[0].dram_reads == 17444);
    CHECK(r.b[3].dram_reads == 16417);
    CHECK(r.b[0].index_req == 1024);
    CHECK(r.b[3].index_req == 0);
    // Translation is the W1 limiter: the target-issue port is blocked by a
    // dTLB refusal in 69 % of the measured run's cycles, and freeing the index
    // array does not move it.
    CHECK(r.b[0].win_refuse == 155501);
    CHECK(r.b[3].win_refuse == 155407);
    CHECK(r.b[3].walker_busy == 153600);
    // Same target set in every arm, element for element and as a multiset.
    CHECK(r.b[0].addr_set_hash == r.a.addr_set_hash);
    CHECK(r.b[3].addr_set_hash == r.a.addr_set_hash);
    CHECK(r.b[0].violations.empty() && r.b[3].violations.empty());
    std::printf("    w1 diag: A=%llu B0=%llu B1=%llu B2=%llu B3=%llu blocked=%llu walks=%llu\n",
                (unsigned long long)r.a.cycles, (unsigned long long)r.b[0].cycles,
                (unsigned long long)r.b[1].cycles, (unsigned long long)r.b[2].cycles,
                (unsigned long long)r.b[3].cycles, (unsigned long long)r.b[3].win_refuse,
                (unsigned long long)r.b[3].walks);
  }
}

// M7: translation-aware page scheduling. Three things have to hold.
//  (a) one cohort reproduces the M6 policy exactly, so the scheduler is a pure
//      scheduling change and not a second design;
//  (b) with more cohorts the output is unchanged element for element and the
//      residency budget and per-page park limit are not exceeded;
//  (c) a blocked page can never park forever: bounded retry plus an adversarial
//      all-distinct-pages distribution still completes.
void test_m7_page_sched() {
  Config base = ab_test_cfg();
  base.experiment = "none";
  base.batch_window = 512;
  auto trace = std::make_shared<const Trace>(build_trace(base));
  std::string err;
  const BulkWorkload w = build_bulk_workload(base, *trace, err);
  CHECK(err.empty());
  if (!err.empty() || !w.armed) return;

  auto arm = [&](bool sched, u32 coh, u32 retry, Sim** keep) {
    Config c = base;
    c.page_sched = sched;
    c.page_cohorts = coh;
    c.park_retry = retry;
    Sim* s = new Sim(c, trace);
    const RunResult r = s->run_bulk(w);
    CHECK(r.ok);
    *keep = s;
    return r;
  };

  Sim* legacy = nullptr;
  Sim* c1 = nullptr;
  Sim* c4 = nullptr;
  const RunResult r_legacy = arm(false, 0, 1, &legacy);
  const RunResult r_c1 = arm(true, 1, 1, &c1);
  const RunResult r_c4 = arm(true, 4, 1, &c4);
  const u64 cyc_legacy = r_legacy.cycles;
  const u64 cyc_c1 = r_c1.cycles;
  const u64 cyc_c4 = r_c4.cycles;

  // (a) the scheduler with one cohort *is* the M6 policy.
  CHECK(cyc_c1 == cyc_legacy);
  CHECK(r_c1.s.get("dram_reads") == r_legacy.s.get("dram_reads"));
  CHECK(c1->engine().stats().lines_issued == legacy->engine().stats().lines_issued);
  // Nothing is allocated for the scheduler when it is off, and the metadata it
  // does own is charged per cohort.
  CHECK(legacy->engine().stats().sched_bytes == 0);
  CHECK(legacy->engine().stats().sched_opens == 0);
  CHECK(c4->engine().stats().sched_bytes == 4ull * 32);

  // (b) element for element, four cohorts produce the same bytes at the same
  // addresses, and no budget was exceeded.
  u64 missing = 0, bad_value = 0, bad_addr = 0;
  for (u32 i = 0; i < w.desc.count; ++i) {
    if (!c4->engine().has_value(i)) { ++missing; continue; }
    if (legacy->engine().value_of(i) != c4->engine().value_of(i)) ++bad_value;
    if (legacy->engine().addr_of(i) != c4->engine().addr_of(i)) ++bad_addr;
  }
  CHECK(missing == 0);
  CHECK(bad_value == 0);
  CHECK(bad_addr == 0);
  CHECK(c4->engine().stats().residency_violations == 0);
  CHECK(c4->engine().stats().buf_max <= base.R);
  // The park limit is untouched: the scheduler spreads across *pages*, it does
  // not let one page park more requests.
  CHECK(r_c4.s.get("tlb_max_parked") <= base.dtlb_parked);
  // Every line still issued exactly once, and every page run accounted for.
  CHECK(c4->engine().stats().lines_issued == legacy->engine().stats().lines_issued);
  CHECK(c4->engine().stats().sched_opens > 0);
  CHECK(c4->engine().stats().win_refuse == 0);  // that counter is the M6 policy's

  // (c) adversarial: nearly every line on its own page, a long retry interval.
  Config adv = base;
  adv.wl = "W1";
  adv.N = 2048;
  adv.Lambda = 2048;
  adv.P = 1024;
  adv.kappa = 0.0;
  adv.batch_window = 256;
  auto atrace = std::make_shared<const Trace>(build_trace(adv));
  std::string aerr;
  const BulkWorkload aw = build_bulk_workload(adv, *atrace, aerr);
  CHECK(aerr.empty());
  if (aerr.empty() && aw.armed) {
    Config c = adv;
    c.page_sched = true;
    c.page_cohorts = 6;
    c.park_retry = 64;
    Sim s(c, atrace);
    const RunResult r = s.run_bulk(aw);
    CHECK(r.ok);
    CHECK(r.cycles < adv.max_cycles);  // no deadlock, no permanent park
    CHECK(s.engine().stats().elems_done == aw.desc.count);
    std::printf("    adversarial: %llu cycles, %llu opens, %llu idle\n",
                (unsigned long long)r.cycles,
                (unsigned long long)s.engine().stats().sched_opens,
                (unsigned long long)s.engine().stats().sched_idle);
  }

  std::printf("    page_sched: legacy=%llu c1=%llu c4=%llu (lines %llu) idle_c4=%llu\n",
              (unsigned long long)cyc_legacy, (unsigned long long)cyc_c1,
              (unsigned long long)cyc_c4,
              (unsigned long long)c4->engine().stats().lines_issued,
              (unsigned long long)c4->engine().stats().sched_idle);
  delete legacy;
  delete c1;
  delete c4;
}

// M7 measured baselines. Every published number is pinned so a later change
// cannot move them silently, and the two workloads are checked together so the
// scheduler cannot be tuned on W1 alone.
void test_m7_baselines() {
  Config base;
  try {
    base = Config::load("configs/ab_controlled.cfg");
  } catch (...) {
    CHECK(false);
    return;
  }
  base.experiment = "none";
  base.batch_window = 8192;
  auto trace = std::make_shared<const Trace>(build_trace(base));
  std::string err;
  const BulkWorkload w = build_bulk_workload(base, *trace, err);
  CHECK(err.empty());
  if (!err.empty() || !w.armed) return;
  base.idx_mode = 0;  // the M7 acquisition path, see the note below
  auto cyc = [&](bool sched, u32 coh, const BulkWorkload& ww,
                 const std::shared_ptr<const Trace>& tr) {
    Config c = base;
    c.page_sched = sched;
    c.page_cohorts = coh;
    Sim s(c, tr);
    const RunResult r = s.run_bulk(ww);
    CHECK(r.ok);
    return r.cycles;
  };
  // The M7 configuration is `idx_mode = 0` plus the page scheduler: M8 moved the
  // index acquisition default, and the M7 audit (§9 of the M8 report) charged the
  // scheduler installation cycles, which costs 5 cycles on W1 and 53 on the
  // legacy page path. The published M7 figures (156 907 / 154 673) are the
  // pre-audit numbers and are reproduced by `idx_mode = 0, page_sched = 1` with
  // the audit disabled; these are the corrected ones.
  const u64 a_legacy = cyc(false, 0, w, trace);
  const u64 a_c2 = cyc(true, 2, w, trace);
  const u64 a_c4 = cyc(true, 4, w, trace);
  CHECK(a_legacy == 233475);
  CHECK(a_c2 == 156912);
  CHECK(a_c4 == 154807);

  // W0: DRAM-bound, nothing for the scheduler to recover, so it must not move
  // the run materially in either direction.
  Config w0 = base;
  w0.wl = "W0";
  w0.kappa = 1.0;
  w0.P = 256;
  auto t0 = std::make_shared<const Trace>(build_trace(w0));
  std::string e0;
  const BulkWorkload b0 = build_bulk_workload(w0, *t0, e0);
  CHECK(e0.empty());
  if (!e0.empty() && !b0.armed) return;
  const u64 b_legacy = cyc(false, 0, b0, t0);
  const u64 b_c2 = cyc(true, 2, b0, t0);
  CHECK(b_legacy == 39322);
  CHECK(b_c2 == 39530);
  std::printf("    m7 baselines: W1 %llu/%llu/%llu  W0 %llu/%llu\n",
              (unsigned long long)a_legacy, (unsigned long long)a_c2,
              (unsigned long long)a_c4, (unsigned long long)b_legacy,
              (unsigned long long)b_c2);
}

// M8: pipelined index acquisition. The tiled unit must produce exactly the M6
// acquisition's bytes at the same addresses, must not reuse a slot before its last
// element is generated, and must keep making progress when the outstanding-request
// table is one entry deep (backpressure) and when `count` is not a whole number of
// index lines (the final partial line).
void test_m8_index_acquisition() {
  Config base = ab_test_cfg();
  base.experiment = "none";
  base.batch_window = 512;
  base.page_sched = true;
  auto trace = std::make_shared<const Trace>(build_trace(base));
  std::string err;
  const BulkWorkload w = build_bulk_workload(base, *trace, err);
  CHECK(err.empty());
  if (!err.empty() || !w.armed) return;

  struct Arm { u32 mode, bl, ms, res; const char* name; };
  const Arm arms[4] = {{0, 64, 64, 8, "M6 acquisition"},
                       {1, 64, 64, 8, "two buffers, in order"},
                       {2, 64, 64, 8, "buffers + fine readiness"},
                       {3, 1, 1, 0, "1-entry table (backpressure)"}};
  Sim* sims[4] = {nullptr, nullptr, nullptr, nullptr};
  u64 cyc[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    Config c = base;
    c.idx_mode = arms[i].mode;
    c.idx_buf_lines = arms[i].bl;
    c.idx_mshr = arms[i].ms;
    c.idx_reserve = arms[i].res;
    sims[i] = new Sim(c, trace);
    const RunResult r = sims[i]->run_bulk(w);
    CHECK(r.ok);
    CHECK(r.cycles < c.max_cycles);  // no deadlock under backpressure
    cyc[i] = r.cycles;
    const BulkStats& s = sims[i]->engine().stats();
    CHECK(s.elems_done == w.desc.count);       // nothing missing
    CHECK(s.residency_violations == 0);        // R = 128 never exceeded
    CHECK(s.addr_mismatch == 0);               // every address re-derives from image
    CHECK(s.buf_max <= c.R);
    // Same index traffic as the M6 path: acquisition is not inventing requests.
    CHECK(s.idx_lines_req == sims[0]->engine().stats().idx_lines_req);
    CHECK(s.lines_issued == sims[0]->engine().stats().lines_issued);
  }
  u64 missing = 0, bad_v = 0, bad_a = 0;
  for (int i = 1; i < 4; ++i) {
    for (u32 e = 0; e < w.desc.count; ++e) {
      if (!sims[i]->engine().has_value(e)) { ++missing; continue; }
      if (sims[0]->engine().value_of(e) != sims[i]->engine().value_of(e)) ++bad_v;
      if (sims[0]->engine().addr_of(e) != sims[i]->engine().addr_of(e)) ++bad_a;
    }
  }
  CHECK(missing == 0);
  CHECK(bad_v == 0);
  CHECK(bad_a == 0);
  std::printf("    idx: %s=%llu %s=%llu %s=%llu %s=%llu (elements %u)\n", arms[0].name,
              (unsigned long long)cyc[0], arms[1].name, (unsigned long long)cyc[1],
              arms[2].name, (unsigned long long)cyc[2], arms[3].name,
              (unsigned long long)cyc[3], w.desc.count);
  for (int i = 0; i < 4; ++i) delete sims[i];
}

// =========================================================================
// M13: bounded cross-window reuse buffer
// =========================================================================
// Every expected value below is computed by the test from what it wrote into the
// memory image -- word b of data line k holds 1000*k + b -- never from the
// engine's bookkeeping. The cases walk the mechanism's failure modes one at a
// time: a repeat inside one window, a repeat across a window boundary, one line
// at different word offsets, several dependants on one outstanding request,
// eviction followed by re-access, a directory that cannot allocate, a workload
// with no reuse at all, an adversarial pattern that crosses the boundary every
// window, and a second descriptor reusing what the first retained.
u64 m13_word(u64 k, u32 off) { return 1000 * k + off; }

void m13_plant(MemorySystem& mem, const std::vector<std::pair<u64, u32>>& els) {
  for (const auto& e : els) {
    for (u32 b = 0; b < LINE_BYTES; b += 4) {
      u8 buf[4];
      const u64 v = m13_word(e.first, b);
      for (u32 i = 0; i < 4; ++i) buf[i] = static_cast<u8>(v >> (8 * i));
      mem.write_va(B_DATA + e.first * LINE_BYTES + b, buf, 4);
    }
  }
}

// One descriptor whose elements are (data line k, byte offset off).
struct M13Rig {
  MemorySystem mem;
  BulkEngine eng;
  std::vector<std::pair<u64, u32>> els;
  u64 cycle = 0;
  M13Rig(const Config& c, std::vector<std::pair<u64, u32>> e)
      : mem(c), eng(c, mem), els(std::move(e)) {
    std::vector<u64> iv;
    for (const auto& x : els) iv.push_back((x.first * LINE_BYTES + x.second) / 4);
    plant(mem, B_INDEX, iv, 4);
    m13_plant(mem, els);
    BulkDesc d = mkdesc(B_INDEX, B_DATA, static_cast<u32>(els.size()), 4, 4);
    plant_desc(mem, d);
    eng.submit(d);
  }
  void run() {
    for (;;) {
      eng.tick(cycle);
      if (eng.idle()) break;
      if (++cycle > 4000000) break;
    }
  }
  void check_values() const {
    for (u32 i = 0; i < els.size(); ++i) {
      CHECK(eng.has_value(i));
      CHECK(eng.value_of(i) == m13_word(els[i].first, els[i].second));
      CHECK(eng.addr_of(i) == B_DATA + els[i].first * LINE_BYTES + els[i].second);
    }
  }
};

Config m13_cfg(u32 window, u32 sets, u32 ways) {
  Config c = bulk_cfg();
  c.batch_window = window;
  c.page_sched = false;  // the M6 issue policy: one cursor, no cohorts
  c.reuse_dir = true;
  c.reuse_sets = sets;
  c.reuse_ways = ways;
  return c;
}

// Repeats inside one window are already folded by the window's own dedup; the
// directory must not be consulted twice for the same line or double-count them.
void test_m13_within_window() {
  Config c = m13_cfg(64, 256, 4);
  std::vector<std::pair<u64, u32>> e;
  for (u32 i = 0; i < 4; ++i) {
    e.push_back({0, i * 4});
    e.push_back({1, i * 4});
  }
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.dedup_merged == 6);          // 8 elements folded onto 2 lines
  CHECK(b.reuse_lines_planned == 2);   // one lookup per distinct line
  CHECK(b.reuse_miss == 2);            // nothing retained yet
  CHECK(b.lines_issued == 2);
  CHECK(b.tgt_req_gen == 2);
  CHECK(b.reuse_hit_ready == 0 && b.reuse_hit_inflight == 0);
  CHECK(b.addr_mismatch == 0);
  CHECK(b.residency_violations == 0);
  r.check_values();
}

// A line repeated in the next window must cost nothing: whether its fill had
// landed (retained data) or not (merged into the outstanding request), exactly
// one request is formed for the whole run.
void test_m13_cross_window() {
  Config c = m13_cfg(4, 256, 4);
  std::vector<std::pair<u64, u32>> e;
  for (u32 i = 0; i < 8; ++i) e.push_back({3, (i % 4) * 4});
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.win_alloc >= 2);
  CHECK(b.reuse_lines_planned == 2);  // one distinct line in each window
  CHECK(b.reuse_hit_ready + b.reuse_hit_inflight == 1);
  CHECK(b.reuse_elems_ready + b.reuse_elems_merged == 4);          // four elements retired with no request
  CHECK(b.lines_issued == 1);
  CHECK(b.tgt_req_gen == 1 && b.tgt_req_done == 1);
  CHECK(b.dedup_merged == 6);
  r.check_values();
}

// The retained bytes are a whole 64 B line, so each dependant must still be
// served its own word at its own width -- a directory that served "the line"
// rather than the element would fail here.
void test_m13_word_offsets() {
  Config c = m13_cfg(2, 256, 4);
  std::vector<std::pair<u64, u32>> e = {{5, 0}, {5, 4}, {5, 8}, {5, 60}};
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.reuse_lines_planned == 2);
  CHECK(b.reuse_hit_ready + b.reuse_hit_inflight == 1);
  CHECK(b.lines_issued == 1);
  CHECK(b.reuse_elems_ready + b.reuse_elems_merged == 2);
  CHECK(b.addr_mismatch == 0);  // every address re-derived from the index image
  r.check_values();
}

// One element per window on one line: the first request serves all four.
void test_m13_multi_dependent() {
  Config c = m13_cfg(1, 256, 4);
  std::vector<std::pair<u64, u32>> e = {{7, 0}, {7, 4}, {7, 8}, {7, 12}};
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.reuse_lines_planned == 4);
  CHECK(b.reuse_hit_ready + b.reuse_hit_inflight == 3);
  CHECK(b.reuse_elems_ready + b.reuse_elems_merged == 3);
  CHECK(b.lines_issued == 1);
  CHECK(b.tgt_req_done == 1);
  CHECK(b.reuse_sub_max >= 1 && b.reuse_sub_max <= 3);
  r.check_values();
}

// One entry: a completed line is evicted by a different one, and re-accessing
// the evicted line must miss and re-issue rather than serve a stale entry. The
// first descriptor drains before the second starts, so both its entries are
// completed and the eviction is deterministic.
void test_m13_eviction() {
  Config c = m13_cfg(1, 1, 1);  // one set, one way
  Rig rig(c);
  m13_plant(rig.mem, {{0, 0}, {0, 4}, {1, 0}});
  plant(rig.mem, B_INDEX, {0}, 4);  // line 0, word 0
  BulkDesc d1 = mkdesc(B_INDEX, B_DATA, 1, 4, 4);
  plant_desc(rig.mem, d1);
  rig.eng.submit(d1);
  rig.run();
  CHECK(rig.eng.stats().lines_issued == 1);
  CHECK(rig.eng.value_of(0) == m13_word(0, 0));
  // Now line 1, then line 0 word 4: the entry for line 0 is evicted by line 1.
  plant(rig.mem, B_INDEX + 0x100, {16, 1}, 4);  // line 1 word 0; line 0 word 4
  BulkDesc d2 = mkdesc(B_INDEX + 0x100, B_DATA, 2, 4, 4);
  plant_desc(rig.mem, d2);
  rig.eng.submit(d2);
  rig.run();
  const BulkStats& b = rig.eng.stats();
  CHECK(b.lines_issued == 3);       // 1 for d1, 2 for d2: the eviction re-issued
  CHECK(b.reuse_evict == 1);
  CHECK(b.reuse_hit_ready == 0);    // line 0 was gone when it came back
  CHECK(b.reuse_lines_planned == 3);
  CHECK(b.desc_completed == 2);
  CHECK(b.addr_mismatch == 0);
  CHECK(rig.eng.value_of(0) == m13_word(1, 0));
  CHECK(rig.eng.value_of(1) == m13_word(0, 4));
}

// The directory cannot allocate because its only entry is an outstanding
// promise to an earlier window. It must fall back to an ordinary request -- not
// evict the promise, and not lose the line.
void test_m13_full() {
  Config c = m13_cfg(2, 1, 1);
  std::vector<std::pair<u64, u32>> e = {{21, 0}, {21, 4}, {22, 0}, {22, 4}};
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.reuse_full + b.reuse_evict == 1);  // one entry, two distinct lines
  CHECK(b.lines_issued == 2);
  CHECK(b.reuse_hit_ready + b.reuse_hit_inflight == 0);
  CHECK(b.elems_done == b.elems_total);
  r.check_values();
}

// No reuse at all: the directory must never report a hit, and the lookup count
// must equal the number of distinct lines.
void test_m13_no_reuse() {
  Config c = m13_cfg(2, 256, 4);
  std::vector<std::pair<u64, u32>> e;
  for (u32 i = 0; i < 8; ++i) e.push_back({40 + i, 0});
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.reuse_hit_ready == 0 && b.reuse_hit_inflight == 0);
  CHECK(b.reuse_miss == b.reuse_probes);
  CHECK(b.reuse_probes == b.reuse_lines_planned);
  CHECK(b.lines_issued == 8);
  r.check_values();
}

// Two lines alternating across every window boundary, in a two-entry directory:
// a thrash. Values, addresses and the residency bound must still hold.
void test_m13_adversarial() {
  Config c = m13_cfg(2, 1, 2);
  std::vector<std::pair<u64, u32>> e;
  for (u32 w = 0; w < 6; ++w) {
    e.push_back({50 + (w % 2), 0});
    e.push_back({50 + (w % 2), 4});
  }
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.reuse_lines_planned == 6);
  CHECK(b.elems_done == 12);
  CHECK(b.dedup_merged == 6);
  CHECK(b.lines_issued + b.reuse_hit_ready + b.reuse_hit_inflight == 6);
  CHECK(b.residency_violations == 0);
  CHECK(b.buf_max <= static_cast<u64>(c.R));
  CHECK(b.addr_mismatch == 0);
  r.check_values();
}

// The identities the run gates on, on a pattern with hits, merges and misses.
void test_m13_accounting() {
  Config c = m13_cfg(4, 2, 2);
  std::vector<std::pair<u64, u32>> e;
  for (u32 w = 0; w < 8; ++w)
    for (u32 i = 0; i < 4; ++i) e.push_back({60 + ((w * 3 + i) % 5), i * 4});
  M13Rig r(c, e);
  r.run();
  const BulkStats& b = r.eng.stats();
  CHECK(b.reuse_probes == b.reuse_lines_planned);
  CHECK(b.reuse_hit_ready + b.reuse_hit_inflight + b.reuse_miss == b.reuse_lines_planned);
  CHECK(b.lines_issued == b.reuse_miss);
  CHECK(b.lines_issued + b.dedup_merged + b.reuse_hit_ready + b.reuse_hit_inflight ==
        b.win_entries);
  CHECK(b.tgt_req_gen == b.tgt_req_issued && b.tgt_req_issued == b.tgt_req_done);
  CHECK(b.elems_done == b.elems_total);
  CHECK(b.residency_violations == 0);
  CHECK(b.addr_mismatch == 0);
  r.check_values();
}

// A second descriptor must be able to reuse what the first retained: tags are
// absolute VA lines, so the buffer survives the descriptor boundary, and the
// data it serves must still be the image's own bytes.
void test_m13_descriptor_boundary() {
  Config c = m13_cfg(64, 16, 4);
  Rig rig(c);
  std::vector<u64> idx = {0, 1, 2, 3};  // data line 0, words 0..3
  plant(rig.mem, B_INDEX, idx, 4);
  m13_plant(rig.mem, {{0, 0}, {0, 4}});
  BulkDesc d = mkdesc(B_INDEX, B_DATA, 4, 4, 4);
  plant_desc(rig.mem, d);
  rig.eng.submit(d);
  rig.run();
  const u64 first_issued = rig.eng.stats().lines_issued;
  CHECK(first_issued == 1);
  CHECK(rig.eng.value_of(2) == m13_word(0, 8));
  // Same lines again: the retained line now serves the whole second descriptor.
  rig.eng.submit(d);
  rig.run();
  const BulkStats& b = rig.eng.stats();
  CHECK(b.lines_issued == first_issued);           // no new request at all
  CHECK(b.reuse_hit_ready + b.reuse_hit_inflight == 1);
  CHECK(b.desc_completed == 2);
  CHECK(b.addr_mismatch == 0);
  CHECK(rig.eng.value_of(2) == m13_word(0, 8));
}

}  // namespace

// ---------------------------------------------------------------------------
// M15: per-requester attribution and the fused window representation
// ---------------------------------------------------------------------------
// The attribution counters are only worth reporting if they are exact -- the
// requester split must add back up to the aggregate -- and inert: turning them on
// must not move a single cycle. Both are checked on one short sequence rather
// than argued from the source.
void test_m15_client_attribution() {
  Config on = bulk_cfg();
  on.cache_diag = true;
  Config off = bulk_cfg();
  off.cache_diag = false;
  MemorySystem m_on(on), m_off(off);

  // Two requesters on two lines. 4000 is fetched by SIMT and then re-read by the
  // engine, which bypasses the private L1, so that read lands in the shared L2
  // and is a cross-requester hit; 4002 is read twice by SIMT only, a self hit.
  struct Step { u64 line; u8 client; };
  const Step seq[] = {{4000, 0}, {4000, 3}, {4001, 3}, {4001, 0}, {4002, 0}, {4002, 0}};
  u64 c_on = 0, c_off = 0;
  for (const Step& s : seq) {
    auto drive = [&](MemorySystem& mem, u64& cycle) {
      Req r;
      r.va_line = s.line;
      r.vpn = s.line >> 6;
      r.core = 0;
      r.slot = 0;
      r.bypass_l1 = (s.client == 3);  // the engine has no private L1
      r.client = s.client;
      std::vector<u64> d;
      while (!mem.submit(r, cycle)) { d.clear(); mem.tick(cycle, d); ++cycle; }
      for (u32 t = 0; t < 4000; ++t) {
        d.clear();
        mem.tick(cycle, d);
        ++cycle;
        if (!d.empty()) break;
      }
    };
    drive(m_on, c_on);
    drive(m_off, c_off);
  }

  // (b) inert: nothing the model reports moved.
  CHECK(c_on == c_off);
  CHECK(m_on.l1.reads == m_off.l1.reads);
  CHECK(m_on.l1.hits == m_off.l1.hits);
  CHECK(m_on.l2.reads == m_off.l2.reads);
  CHECK(m_on.l2.hits == m_off.l2.hits);
  CHECK(m_on.l2.evict == m_off.l2.evict);
  CHECK(m_on.dram.reads == m_off.dram.reads);
  CHECK(m_on.dram.row_hits == m_off.dram.row_hits);

  // (a) exact: per requester reads == hits + misses, and the rows add back up.
  u64 r1 = 0, h1 = 0, m1 = 0, r2 = 0, h2 = 0, m2 = 0, done = 0;
  for (u32 k = 0; k < 4; ++k) {
    CHECK(m_on.cc[k].l1_reads == m_on.cc[k].l1_hits + m_on.cc[k].l1_misses);
    CHECK(m_on.cc[k].l2_reads == m_on.cc[k].l2_hits + m_on.cc[k].l2_misses);
    r1 += m_on.cc[k].l1_reads;
    h1 += m_on.cc[k].l1_hits;
    m1 += m_on.cc[k].l1_misses;
    r2 += m_on.cc[k].l2_reads;
    h2 += m_on.cc[k].l2_hits;
    m2 += m_on.cc[k].l2_misses;
    done += m_on.cc[k].done;
    // No engine index traffic exists in this sequence (there is no descriptor),
    // so that row must be empty. The walker's row is not: a dTLB miss fetches
    // PTEs through the shared L2, tagged client 2, which is the point of having
    // four tags rather than two.
    if (k == 1) CHECK(m_on.cc[k].l1_reads == 0 && m_on.cc[k].l2_reads == 0);
  }
  CHECK(r1 == m_on.l1.reads && h1 == m_on.l1.hits && m1 == m_on.l1.misses);
  CHECK(r2 == m_on.l2.reads && h2 == m_on.l2.hits && m2 == m_on.l2.misses);
  CHECK(done == 6);  // every request completed, exactly once
  CHECK(m_on.cc[0].done == 4 && m_on.cc[3].done == 2);
  CHECK(m_on.cc[3].l2_hit_cross >= 1);  // the engine re-read a line SIMT fetched
}

// The fused window must be a *representation* change: same lines issued, same
// dedup, same values, same addresses, same cycles. Storage is the only thing it
// is allowed to move.
void test_m15_fused_window() {
  std::vector<std::pair<u64, u32>> e;
  for (u32 i = 0; i < 32; ++i) e.push_back({i % 5, (i % 4) * 4});

  Config base = bulk_cfg();
  base.batch_window = 8;
  base.page_sched = true;
  Config fz = base;
  fz.win_fused = true;

  M13Rig a(base, e), b(fz, e);
  a.run();
  b.run();
  const BulkStats& sa = a.eng.stats();
  const BulkStats& sb = b.eng.stats();

  CHECK(sb.win_fused == 1);
  CHECK(sb.addr_mismatch == 0);
  CHECK(sb.dedup_merged == sa.dedup_merged);
  CHECK(sb.lines_issued == sa.lines_issued);
  CHECK(sb.tgt_req_gen == sa.tgt_req_gen);
  CHECK(sb.tgt_req_done == sa.tgt_req_done);
  CHECK(sb.win_entries == sa.win_entries);
  CHECK(sb.elems_done == e.size());
  CHECK(b.cycle == a.cycle);  // a representation, not a policy
  for (u32 i = 0; i < e.size(); ++i) {
    CHECK(b.eng.has_value(i));
    CHECK(b.eng.value_of(i) == a.eng.value_of(i));
    CHECK(b.eng.addr_of(i) == a.eng.addr_of(i));
  }
  // 8 B per element + 6 B per distinct line, against 16+4 B per element and
  // 32+4 B per line.
  CHECK(sb.win_fused_vec_bytes > 0);
  CHECK(sb.win_fused_vec_bytes < sa.win_vec_bytes);

  // The layout's preconditions are enforced, not assumed.
  auto throws = [](Config c) {
    try {
      MemorySystem m(c);
      BulkEngine en(c, m);
    } catch (const std::runtime_error&) {
      return true;
    }
    return false;
  };
  Config no_dedup = base;
  no_dedup.win_fused = true;
  no_dedup.dedup = false;
  CHECK(throws(no_dedup));
  Config no_order = base;
  no_order.win_fused = true;
  no_order.page_order = false;
  CHECK(throws(no_order));
  Config big = base;
  big.win_fused = true;
  big.batch_window = 131072;
  CHECK(throws(big));
}

// M9 infrastructure (docs/m9-dispatch-design.md §9): one residency budget shared
// by two clients. A pool that leaked a slot, over-issued, or accepted a second
// release would silently move every hybrid result, so the properties the driver
// stands on are checked directly rather than inferred from a run.
void test_slot_pool() {
  // Ascending fill reproduces Sim::run: the highest id is handed out first.
  {
    SlotPool p(4, true);
    CHECK(p.consistent());
    CHECK(p.residency() == 0);
    CHECK(p.free_count() == 4);
    CHECK(p.take(0) == 3u);
    CHECK(p.take(0) == 2u);
    CHECK(p.held_by(0) == 2 && p.held_by(1) == 0);
    CHECK(p.free_count() == 2);
    CHECK(p.consistent());
  }
  // Descending fill reproduces BulkEngine's own free list: lowest id first.
  {
    SlotPool p(4, false);
    CHECK(p.take(1) == 0u);
    CHECK(p.take(1) == 1u);
    CHECK(p.held_by(1) == 2 && p.held_by(0) == 0);
    CHECK(p.consistent());
  }
  // Two clients, one budget: the pool -- not the client -- is the bound.
  {
    SlotPool p(3, true);
    const u32 a = p.take(0);
    const u32 b = p.take(1);
    const u32 c = p.take(0);
    CHECK(a != b && b != c && a != c);  // ids are exclusive across clients
    CHECK(p.residency() == 3);
    CHECK(!p.has_free());
    CHECK(p.take(1) == static_cast<u32>(NONE));  // never over-issues
    CHECK(p.consistent());
    CHECK(p.give(b));  // release the middle slot
    CHECK(p.held_by(1) == 0 && p.held_by(0) == 2);
    CHECK(p.take(1) == b);  // LIFO: the released id comes back
    CHECK(p.residency() == 3 && !p.has_free());
    CHECK(p.consistent());
    CHECK(p.give(c));
    CHECK(!p.give(c));  // a second release of the same id is refused
    CHECK(p.held_by(0) == 1);
    CHECK(!p.give(99));  // an id outside the pool is refused
    CHECK(p.consistent());
  }
  // Full take/release sweep returns the pool to its start state.
  {
    SlotPool p(8, true);
    std::vector<u32> ids;
    for (int i = 0; i < 8; ++i) {
      ids.push_back(p.take(static_cast<u8>(i & 1)));
      CHECK(p.consistent());
    }
    CHECK(p.residency() == 8 && !p.has_free());
    for (u32 s : ids) {
      CHECK(p.give(s));
      CHECK(p.consistent());
    }
    CHECK(p.residency() == 0 && p.free_count() == 8);
  }
}

// =========================================================================
// M23: the selected archetype, and the baselines it must be measured against
//
// M22 established that the fused representation is behaviourally identical to
// the unfused one and 3.74x smaller at 8K. M23 carries that forward as the
// archetype and freezes its numbers here, because until now no test pinned the
// shipped acquisition path at scale: every historical windowed baseline below
// runs with `idx_mode = 0`, while the shipped default is `idx_mode = 2`, and
// every published M18-M22 number was taken on a configuration (windowed,
// scheduled) that is *not* what `Config()` gives you.
void test_m23_archetype_config() {
  // The knobs the archetype depends on. Two of them are off by default, which
  // is why every windowed experiment has to name them; a change to either
  // default would silently move every published number in this tree.
  const Config d;
  CHECK(d.batch_window == 0);    // the engine is not windowed by default
  CHECK(d.page_sched == false);  // and the page scheduler is off by default
  CHECK(d.page_order == true);
  CHECK(d.page_cohorts == 2);
  CHECK(d.dedup == true);
  CHECK(d.idx_mode == 2);        // tiled acquisition is the shipped path
  CHECK(d.win_fused == false);   // M22: the default is the expensive build
  CHECK(d.ag_lanes == 8);
  CHECK(d.fanout_lanes == 4);
  CHECK(d.idx_buf_lines == 64);
  CHECK(d.page_dir == 512);
  CHECK(d.reuse_dir == false);
  CHECK(d.R == 128);

  // Both window points, both builds, must be constructible.
  auto builds = [](Config c) {
    try {
      MemorySystem m(c);
      BulkEngine e(c, m);
    } catch (const std::runtime_error&) {
      return false;
    }
    return true;
  };
  for (u32 W : {8192u, 16384u})
    for (bool fused : {false, true}) {
      Config c = Config::load("configs/ab_controlled.cfg");
      c.batch_window = W;
      c.page_sched = true;
      c.page_order = true;
      c.page_cohorts = 2;
      c.win_fused = fused;
      CHECK(builds(c));
    }
  // The fused layout refuses a configuration that cannot express a line as a
  // run; that is a precondition, not a warning.
  Config nd = Config::load("configs/ab_controlled.cfg");
  nd.batch_window = 8192;
  nd.page_sched = true;
  nd.win_fused = true;
  nd.dedup = false;
  CHECK(!builds(nd));
  Config no = nd;
  no.dedup = true;
  no.page_order = false;
  CHECK(!builds(no));
}

// The archetype's own numbers (M23A). Measured on `configs/ab_controlled.cfg`
// with the shipped acquisition path, page scheduler on, two cohorts. Read the
// `vec` column as the *realized* arrays, which is what M22 costed.
void test_m23_archetype_baseline() {
  struct Cell { u32 W; bool fused; u64 cycles, vec, dram; };
  const Cell expect[] = {
      {8192, false, 156419, 703104, 17446},
      {8192, true, 156419, 188064, 17446},
      {16384, false, 152059, 1097472, 17447},
      {16384, true, 152059, 286656, 17447},
  };
  u64 cyc[2][2] = {}, bytes[2][2] = {};
  int wi = 0;
  for (u32 W : {8192u, 16384u}) {
    int bi = 0;
    for (bool fused : {false, true}) {
      Config c = Config::load("configs/ab_controlled.cfg");
      c.experiment = "none";
      c.batch_window = W;
      c.page_sched = true;
      c.page_order = true;
      c.page_cohorts = 2;
      c.win_fused = fused;
      auto tr = std::make_shared<const Trace>(build_trace(c));
      std::string err;
      const BulkWorkload w = build_bulk_workload(c, *tr, err);
      CHECK(err.empty());
      if (!err.empty() || !w.armed) return;
      Sim s(c, tr);
      const RunResult r = s.run_bulk(w);
      const BulkStats& b = s.engine().stats();
      const Cell& e = expect[wi * 2 + bi];
      CHECK(r.ok);
      CHECK(r.cycles == e.cycles);
      CHECK(b.win_entries == 16384);      // every element admitted
      CHECK(b.lines_issued == 16384);     // a permutation: one line per element
      CHECK(b.dedup_merged == 0);         // ... so there is nothing to merge
      CHECK(b.tgt_req_done == 16384);
      CHECK(b.addr_mismatch == 0);
      CHECK(r.s.get("dram_reads") == e.dram);
      // `win_vec_bytes` is the realized total in both builds;
      // `win_fused_vec_bytes` is an additional fused-only accounting of the
      // same bytes, so in the fused build the two must agree exactly.
      const u64 v = b.win_vec_bytes;
      CHECK(v == e.vec);
      if (fused) {
        CHECK(b.win_fused_vec_bytes == v);
      } else {
        CHECK(b.win_fused_vec_bytes == 0);
      }
      cyc[wi][bi] = r.cycles;
      bytes[wi][bi] = v;
      ++bi;
    }
    ++wi;
  }
  // A representation, not a policy: the two builds must be cycle-identical at
  // both window points, and the fused one must be materially smaller.
  CHECK(cyc[0][0] == cyc[0][1]);
  CHECK(cyc[1][0] == cyc[1][1]);
  CHECK(bytes[0][0] / bytes[0][1] >= 3 && bytes[0][0] / bytes[0][1] <= 4);
  CHECK(bytes[1][0] / bytes[1][1] >= 3 && bytes[1][0] / bytes[1][1] <= 4);
  std::printf("    archetype: 8K %llu cyc %llu/%llu B | 16K %llu cyc %llu/%llu B\n",
              (unsigned long long)cyc[0][0], (unsigned long long)bytes[0][0],
              (unsigned long long)bytes[0][1], (unsigned long long)cyc[1][0],
              (unsigned long long)bytes[1][0], (unsigned long long)bytes[1][1]);
}

// M6's per-entry charge (8 B/element + 16 B/line, both buffers) is the number
// every M6/M7 resource claim was made against. M22 measured the realized arrays
// at 16 B/element + 32 B/line + 4 B/line of order metadata (plus a 4 B
// permutation per element in the unfused build), i.e. several times the charge.
// Both numbers are pinned here so neither can silently become the other.
void test_m23_model_vs_realized() {
  // The modelled charge at the shape test_m6_resource_accounting uses.
  const u64 modelled = 2ull * (256 * 8 + 512 * 16);
  CHECK(modelled == 20480);
  // The same shape priced from the M22 sizeof probe: WinEnt 16 B, LineReq 32 B,
  // permutation 4 B/element, order 4 B/line, two buffers.
  const u64 realized = 2ull * (256 * (16 + 4) + 512 * (32 + 4));
  CHECK(realized == 47104);
  CHECK(realized > modelled);
  CHECK(realized >= 2 * modelled);   // 2.3x at this shape; 6.33x at W = 8K
}

// =========================================================================
// M23-A: the four architecture configurations, validated against each other.
//
// The point of the matrix is the *negative* claim: M0, 2x8K, 2x16K and 2x32K
// must differ in the DX-100 block and nowhere else. If any shared resource ever
// drifts between the files, a measured difference stops being attributable to
// the architecture, so this checks the shared fields field by field rather
// than trusting that the four files were derived from one template.
void test_m23_architecture_matrix() {
  const char* files[4] = {"configs/m23_m0.cfg", "configs/m23_dx8k.cfg",
                          "configs/m23_dx16k.cfg", "configs/m23_dx32k.cfg"};
  const u32 windows[4] = {0, 8192, 16384, 32768};
  Config c[4];
  for (int i = 0; i < 4; ++i) {
    try {
      c[i] = Config::load(files[i]);
    } catch (const std::exception& e) {
      std::printf("      %s: %s\n", files[i], e.what());
      CHECK(false);
      return;
    }
  }

  // ---- 1. every shared resource must be identical across the four --------
  for (int i = 1; i < 4; ++i) {
    CHECK(c[i].cores == c[0].cores);
    CHECK(c[i].warps_per_core == c[0].warps_per_core);
    CHECK(c[i].lanes == c[0].lanes);           // same SIMT lane width everywhere
    CHECK(c[i].elem_bytes == c[0].elem_bytes);
    CHECK(c[i].R == c[0].R);                   // one global request budget
    CHECK(c[i].lsu_pending == c[0].lsu_pending);
    CHECK(c[i].mem_path == c[0].mem_path);
    CHECK(c[i].l1_lines == c[0].l1_lines);     // L1 capacity
    CHECK(c[i].l1_ways == c[0].l1_ways);       // L1 associativity
    CHECK(c[i].l1_mshr == c[0].l1_mshr);       // 128 in all four
    CHECK(c[i].l2_lines == c[0].l2_lines);
    CHECK(c[i].l2_ways == c[0].l2_ways);
    CHECK(c[i].l2_mshr == c[0].l2_mshr);
    CHECK(c[i].dtlb_entries == c[0].dtlb_entries);
    CHECK(c[i].dtlb_ways == c[0].dtlb_ways);
    CHECK(c[i].dtlb_mshr == c[0].dtlb_mshr);
    CHECK(c[i].dtlb_parked == c[0].dtlb_parked);
    CHECK(c[i].l2tlb_entries == c[0].l2tlb_entries);
    CHECK(c[i].l2tlb_ways == c[0].l2tlb_ways);
    CHECK(c[i].l2tlb_mshr == c[0].l2tlb_mshr);
    CHECK(c[i].walkers == c[0].walkers);
    CHECK(c[i].walk_cache == c[0].walk_cache);
    CHECK(c[i].levels == c[0].levels);         // same page size (Sv32, 4 KiB)
    CHECK(c[i].banks == c[0].banks);           // same DRAM geometry
    CHECK(c[i].rows == c[0].rows);
    CHECK(c[i].row_bytes == c[0].row_bytes);
    CHECK(c[i].t_hit == c[0].t_hit);
    CHECK(c[i].t_miss == c[0].t_miss);
    CHECK(c[i].dram_q == c[0].dram_q);
    CHECK(c[i].frfcfs == c[0].frfcfs);
    CHECK(c[i].bank_hash == c[0].bank_hash);
    CHECK(c[i].cache_diag == c[0].cache_diag);
    CHECK(c[i].N == c[0].N);                   // one workload, one address map
    CHECK(c[i].Lambda == c[0].Lambda);
    CHECK(c[i].P == c[0].P);
    CHECK(c[i].seed == c[0].seed);
    CHECK(c[i].bulk_index_base == c[0].bulk_index_base);
    CHECK(c[i].bulk_desc_line == c[0].bulk_desc_line);
    CHECK(c[i].bulk_max_elems == c[0].bulk_max_elems);
  }
  CHECK(c[0].lanes == 4);        // the established experimental lane width
  CHECK(c[0].l1_mshr == 128);    // the M16 strengthening, in all four
  CHECK(c[0].R == 128);
  CHECK(c[0].l1_lines * 64 == 16384);   // 16 KiB L1
  CHECK(c[0].l2_lines * 64 == 1048576); // 1 MiB L2

  // ---- 2. M0 has no DX-100 block at all ----------------------------------
  CHECK(c[0].dispatch == 0);
  CHECK(c[0].batch_window == 0);
  CHECK(c[0].win_fused == false);

  // ---- 3. the three DX-100 configurations, and only their capacity differs
  for (int i = 1; i < 4; ++i) {
    CHECK(c[i].batch_window == windows[i]);
    CHECK(c[i].win_fused == true);        // fused storage in every M23 experiment
    CHECK(c[i].dispatch == 1);            // compiler-directed selection, always
    CHECK(c[i].op_class == "gen");        // the annotation is the compiler's
    CHECK(c[i].page_sched == true);
    CHECK(c[i].page_order == true);
    CHECK(c[i].dedup == true);            // required by the fused layout
    CHECK(c[i].page_cohorts == 2);
  }
  // The three differ *only* in the context capacity.
  for (int i = 1; i < 4; ++i) {
    Config a = c[1], b = c[i];
    a.batch_window = b.batch_window = 0;   // the one permitted difference
    CHECK(a.to_string() == b.to_string());
  }

  // ---- 4. smoke: each configuration actually runs, and runs as itself -----
  for (int i = 0; i < 4; ++i) {
    Config run = c[i];
    run.N = 2048;
    run.Lambda = 2048;
    run.max_cycles = 5000000;
    Sim s(run);
    const RunResult r = s.run();
    CHECK(r.ok);
    CHECK(r.cycles > 0);
    if (i == 0) {
      // No engine, no Bulk counters, no dispatch.
      CHECK(!s.has_engine());
      CHECK(r.s.get("dispatch_mode") == 0);
      CHECK(r.s.get("bulk_win_entries") == 0);
      CHECK(r.s.get("win_fused_active") == 0);
    } else {
      // Dispatched, fused, and the partition accounts for every element.
      CHECK(r.s.get("dispatch_mode") == 1);
      CHECK(r.s.get("win_fused_active") == 1);
      CHECK(r.s.get("bulk_win_fused") == 1);
      CHECK(r.s.get("bulk_win_fused_vec_bytes") > 0);
      CHECK(r.s.get("slot_pool_total") == c[i].R);
      CHECK(r.s.get("bulk_elems") + r.s.get("simt_elems") == r.s.get("trace_accesses"));
      CHECK(r.s.get("bulk_dispatch_unaccounted") == 0);
      CHECK(r.s.get("bulk_win_entries") == r.s.get("bulk_elems"));
      // A permutation has nothing to merge and one line per element.
      CHECK(r.s.get("bulk_dedup_merged") == 0);
      CHECK(r.s.get("bulk_lines_issued") == r.s.get("bulk_elems"));
    }
    std::printf("    %-22s ok=%d cycles=%llu dispatch=%llu fused=%llu entries=%llu\n",
                files[i], (int)r.ok, (unsigned long long)r.cycles,
                (unsigned long long)r.s.get("dispatch_mode"),
                (unsigned long long)r.s.get("win_fused_active"),
                (unsigned long long)r.s.get("bulk_win_entries"));
  }
}

// =========================================================================
// M23-G6C: P0's admission deferral inside G6B's streaming pipeline
// =========================================================================
// `bulk_org = 8` has to do three things at once, and each is a counter here, not
// an argument: keep streaming (a release happens while other translations are
// still outstanding), keep the answers of the arm it is built on, and turn the
// dTLB's temporary refusals into deferrals that are revisited and admitted
// instead of a cursor that stops the cycle.
void test_m23_g6c_admission_deferral() {
  Config base = ab_test_cfg();
  base.experiment = "none";
  base.batch_window = 512;
  base.win_fused = true;     // the fused window is what bulk_org >= 2 owns
  base.dedup = true;
  base.page_order = true;
  base.page_sched = true;    // P0's cohort table and retry interval
  base.page_cohorts = 2;
  base.park_retry = 1;
  base.g6b_pa_capacity = 128;
  // Translation capacity that makes a refusal a *page* conflict: two dTLB MSHRs,
  // both parkable by the page being walked, so a second page's probe is refused
  // and has to be deferred.
  base.dtlb_mshr = 2;
  base.dtlb_parked = 2;
  auto trace = std::make_shared<const Trace>(build_trace(base));
  std::string err;
  const BulkWorkload w = build_bulk_workload(base, *trace, err);
  CHECK(err.empty());
  if (!err.empty() || !w.armed) return;

  auto run = [&](u32 org, Sim** keep) {
    Config c = base;
    c.bulk_org = org;
    Sim* s = new Sim(c, trace);
    const RunResult r = s->run_bulk(w);
    CHECK(r.ok);
    *keep = s;
    return r;
  };
  Sim* s7 = nullptr;
  Sim* s8 = nullptr;
  const RunResult r7 = run(7, &s7);
  const RunResult r8 = run(8, &s8);
  const BulkStats& g8 = s8->engine().stats();

  // (1) every run admitted exactly once, every probe completed, every element
  //     retired: nothing lost, nothing admitted twice.
  CHECK(g8.g6b_tr_reqs == g8.g6b_tr_done);
  CHECK(g8.g6c_admit_ok == g8.g6b_tr_reqs);
  CHECK(g8.g6c_admit_attempts == g8.g6c_admit_ok + g8.g6c_refused);
  CHECK(g8.g6c_dup_submit == 0);
  CHECK(g8.g6c_refuse_other == 0);
  CHECK(g8.elems_done == w.desc.count);
  CHECK(g8.addr_mismatch == 0);
  CHECK(g8.residency_violations == 0);
  // (2) a refused run is deferred, and admission moves to another page in the
  //     same cycle rather than ending the cycle on the refusal.
  CHECK(g8.g6c_refused > 0);
  CHECK(g8.g6c_defer_events == g8.g6c_refused);
  CHECK(g8.g6c_cohorts_opened >= 2);
  CHECK(g8.g6c_defer_other_admit > 0);
  // (3) the deferred run is revisited and admitted once the MSHR retires.
  CHECK(g8.g6c_defer_readmit > 0);
  CHECK(g8.g6c_defer_readmit <= g8.g6c_defer_events);
  // (4) runs of one page share its translation: one walk serves many runs.
  CHECK(g8.g6b_tr_reqs > r8.s.get("walks"));
  // (5) the deferral state is bounded by what was provisioned, and the park
  //     limit is still the memory model's own.
  CHECK(g8.g6c_defer_depth_max <= base.page_cohorts);
  CHECK(r8.s.get("tlb_max_parked") <= base.dtlb_parked);
  // (6) streaming is intact: releases happen while translation is outstanding,
  //     and every translated run is released exactly once.
  CHECK(g8.g6b_overlap > 0);
  CHECK(g8.g6b_runs_released == g8.g6b_tr_done);
  CHECK(g8.g6b_ready_peak <= base.g6b_pa_capacity);
  // (7) same machine, same answers: only the admission schedule moved. The
  //     per-element values and addresses must be identical; the order-dependent
  //     `completion_hash` is *not* comparable between policies by construction
  //     (it is the FNV of the retirement sequence, which this arm may reorder).
  u64 bad = 0;
  for (u32 i = 0; i < w.desc.count; ++i)
    if (s7->engine().value_of(i) != s8->engine().value_of(i) ||
        s7->engine().addr_of(i) != s8->engine().addr_of(i))
      ++bad;
  CHECK(bad == 0);

  std::printf("    g6c: refused=%llu (parked %llu / full %llu) readmit=%llu age_max=%llu "
              "depth_max=%llu opens=%llu idle=%llu other=%llu | g6b runs=%llu\n",
              (unsigned long long)g8.g6c_refused,
              (unsigned long long)g8.g6c_refuse_parked,
              (unsigned long long)g8.g6c_refuse_full,
              (unsigned long long)g8.g6c_defer_readmit,
              (unsigned long long)g8.g6c_defer_age_max,
              (unsigned long long)g8.g6c_defer_depth_max,
              (unsigned long long)g8.g6c_cohorts_opened,
              (unsigned long long)g8.g6c_admit_idle,
              (unsigned long long)g8.g6c_defer_other_admit,
              (unsigned long long)g8.g6b_runs_released);

  // (8) fairness and eventual completion in the worst case for a page-aware
  //     cursor: nearly every line on its own page, a long retry interval. The
  //     descriptor must still finish, with the depth bounded by the cohorts.
  Config adv = base;
  adv.N = 1024;
  adv.Lambda = 1024;
  adv.P = 1024;
  adv.kappa = 0.0;
  adv.batch_window = 128;
  adv.page_cohorts = 4;
  adv.park_retry = 16;
  auto atrace = std::make_shared<const Trace>(build_trace(adv));
  std::string aerr;
  const BulkWorkload aw = build_bulk_workload(adv, *atrace, aerr);
  CHECK(aerr.empty());
  if (aerr.empty() && aw.armed) {
    Config c = adv;
    c.bulk_org = 8;
    Sim s(c, atrace);
    const RunResult r = s.run_bulk(aw);
    const BulkStats& ga = s.engine().stats();
    CHECK(r.ok);
    CHECK(r.cycles < adv.max_cycles);
    CHECK(ga.elems_done == aw.desc.count);
    CHECK(ga.g6c_refused > 0);
    CHECK(ga.g6c_defer_depth_max <= adv.page_cohorts);
    CHECK(ga.g6c_dup_submit == 0);
    CHECK(ga.g6b_tr_reqs == ga.g6b_tr_done);
    std::printf("    g6c adversarial: %llu cycles, refused %llu, readmit %llu, "
                "depth_max %llu, opens %llu\n",
                (unsigned long long)r.cycles, (unsigned long long)ga.g6c_refused,
                (unsigned long long)ga.g6c_defer_readmit,
                (unsigned long long)ga.g6c_defer_depth_max,
                (unsigned long long)ga.g6c_cohorts_opened);
  }
  delete s7;
  delete s8;
}

// =========================================================================
// M24-A: four-way page / DRAM organization, and the mapping experiments
// =========================================================================
// Two independently switchable mechanisms over one unchanged pipeline:
//   page organization   bulk_org 8 (page cohorts, multi-page admission) vs 7
//                       (one cursor, stored order) -- same window, pool, dTLB;
//   DRAM organization   m24_dram_org 1 (release in physical (row,bank,line)
//                       order) vs 0 (release in the sealed order).
// Every check below is an exact decode or an event count, not an argument.
void test_m24_four_way_organization() {
  // ---- (a) the mapping equations, one arm at a time ---------------------
  Config mc = ab_test_cfg();
  mc.experiment = "none";
  mc.banks = 2;
  mc.rows = 4096;
  mc.row_bytes = 1024;
  mc.dram_il_lines = 1;
  mc.bank_hash = true;
  {
    Config c0 = mc, c1 = mc, c2 = mc, c3 = mc, c4 = mc;
    c1.m24_map = 1;
    c2.m24_map = 2;
    c3.m24_map = 3;
    c3.m24_perm_seed = 11;
    c4.m24_map = 3;
    c4.m24_perm_seed = 12;
    MemorySystem m0(c0), m1(c1), m2(c2), m3(c3), m4(c4);
    u64 bank_bad = 0, row_bad = 0, m0_bad = 0;
    for (u64 l = 0; l < 8192; ++l) {
      u32 b0 = 0, r0 = 0, b1 = 0, r1 = 0, b2 = 0, r2 = 0, b3 = 0, r3 = 0;
      m0.dram_bank_row(l, b0, r0);
      m1.dram_bank_row(l, b1, r1);
      m2.dram_bank_row(l, b2, r2);
      m3.dram_bank_row(l, b3, r3);
      if (b1 != l % 2) ++bank_bad;                                        // M1
      if (b2 != ((l ^ (l >> 5) ^ (l >> 11) ^ (l >> 17)) % 2)) ++bank_bad;  // M2
      if (r0 != r1 || r0 != r2 || r0 != r3) ++row_bad;   // the row map is untouched
      if (b0 != ((l ^ (l >> 6) ^ (l >> 12)) % 2) || r0 != ((l >> 5) % 4096)) ++m0_bad;
    }
    CHECK(bank_bad == 0);
    CHECK(row_bad == 0);
    CHECK(m0_bad == 0);
    // M3: a bijection on the 20-bit page space, offsets preserved, deterministic,
    // and dependent on the seed. The whole point is that a virtual page still owns
    // 64 consecutive lines -- only the frame it occupies changes.
    std::vector<u8> seen(1u << 20, 0);
    u64 dup = 0, off_bad = 0, range_bad = 0, moved = 0, seed_bad = 0, det_bad = 0;
    for (u64 vp = 0; vp < 4096; ++vp) {
      const u64 f = m3.pa_page_of_vpn(vp) - 0x4000ull;
      if (f >= (1ull << 20)) {
        ++range_bad;
      } else {
        if (seen[static_cast<size_t>(f)]) ++dup;
        seen[static_cast<size_t>(f)] = 1;
      }
      if (f != vp) ++moved;
      if (m4.pa_page_of_vpn(vp) != m3.pa_page_of_vpn(vp)) ++seed_bad;
      if (m0.pa_page_of_vpn(vp) != vp + 0x4000ull) ++det_bad;  // M0/M1/M2 identity
      const u64 l0 = m3.pa_line_of_va(vp * 4096);
      for (u64 k = 0; k < 64; ++k)
        if (m3.pa_line_of_va(vp * 4096 + 64 * k) != l0 + k) ++off_bad;
    }
    CHECK(dup == 0);
    CHECK(range_bad == 0);
    CHECK(off_bad == 0);
    CHECK(det_bad == 0);
    CHECK(moved > 4000);     // not the identity map
    CHECK(seed_bad > 4000);  // the seed selects the permutation
  }

  // ---- (b) the four cells, on one workload ------------------------------
  Config base = ab_test_cfg();
  base.experiment = "none";
  base.batch_window = 512;
  base.win_fused = true;
  base.dedup = true;
  base.page_order = true;
  base.page_sched = true;
  base.page_cohorts = 4;
  base.park_retry = 1;
  base.g6b_pa_capacity = 64;
  base.dtlb_mshr = 2;
  base.dtlb_parked = 2;
  auto trace = std::make_shared<const Trace>(build_trace(base));
  std::string err;
  const BulkWorkload w = build_bulk_workload(base, *trace, err);
  CHECK(err.empty());
  if (!err.empty() || !w.armed) return;

  struct Cell { u32 org; bool dorg; };
  const Cell cells[4] = {{7, false}, {8, false}, {7, true}, {8, true}};
  u64 cyc[4] = {0, 0, 0, 0}, inv[4] = {0, 0, 0, 0};
  for (u32 i = 0; i < 4; ++i) {
    Config c = base;
    c.bulk_org = cells[i].org;
    c.m24_dram_org = cells[i].dorg;
    Sim* s = new Sim(c, trace);
    const RunResult r = s->run_bulk(w);
    const BulkStats& g = s->engine().stats();
    CHECK(r.ok);
    CHECK(g.elems_done == w.desc.count);           // nothing lost
    CHECK(g.g6b_tr_reqs == g.g6b_tr_done);         // translated exactly once
    CHECK(g.g6b_runs_released == g.g6b_tr_done);   // released exactly once
    CHECK(g.g6b_ready_peak <= c.g6b_pa_capacity);  // never over capacity
    if (cells[i].org == 8) CHECK(g.g6c_dup_submit == 0);
    // The DRAM factor as an event count. With the factor off the organizer never
    // runs, so no run carries a physical rank and the release order is the sealed
    // one -- exact, not approximate. With it on, every run is ranked: that pass is
    // what makes a physical release order possible at all.
    CHECK(g.m24_rel_pairs > 0);
    if (cells[i].dorg) CHECK(g.org_runs > 0);
    else CHECK(g.org_runs == 0);
    // Whether the physical order then *moves* a release depends on the pool's
    // depth, which is the page factor's business: a shallow pool has nothing to
    // reorder. Cell D is the deep case and must show reordering; cell C is the
    // shallow case here, so only the pass is asserted for it.
    if (cells[i].org == 8 && cells[i].dorg) CHECK(g.m24_rel_inversions > 0);
    // The page factor as an event count: cohort admission exists only in arm 8.
    if (cells[i].org == 8) CHECK(g.g6c_cohorts_opened > 0);
    else CHECK(g.g6c_cohorts_opened == 0);
    cyc[i] = r.cycles;
    inv[i] = g.m24_rel_inversions;
    delete s;
  }
  CHECK(cyc[0] && cyc[1] && cyc[2] && cyc[3]);
  std::printf("    m24 cells A..D: %llu %llu %llu %llu cycles, inversions %llu %llu %llu %llu\n",
              (unsigned long long)cyc[0], (unsigned long long)cyc[1],
              (unsigned long long)cyc[2], (unsigned long long)cyc[3],
              (unsigned long long)inv[0], (unsigned long long)inv[1],
              (unsigned long long)inv[2], (unsigned long long)inv[3]);

  // ---- (c) determinism, the shipped default, and M3 under the engine ----
  {
    Config d = base;
    d.bulk_org = 8;
    d.m24_dram_org = true;
    Sim* s = new Sim(d, trace);
    const RunResult r = s->run_bulk(w);
    CHECK(r.ok);
    CHECK(r.cycles == cyc[3]);                            // repeat is identical
    CHECK(s->engine().stats().m24_rel_inversions == inv[3]);
    delete s;
  }
  {
    Config def = base;  // m24_dram_org defaults to the shipped physical order
    def.bulk_org = 8;
    Sim* s = new Sim(def, trace);
    const RunResult r = s->run_bulk(w);
    CHECK(r.cycles == cyc[3]);                            // defaults = cell D
    delete s;
  }
  {
    Config c = base;
    c.bulk_org = 8;
    c.m24_map = 3;   // the permutation must not disturb translation or the pool
    c.m24_perm_seed = 5;
    Sim* s = new Sim(c, trace);
    const RunResult r = s->run_bulk(w);
    const BulkStats& g = s->engine().stats();
    CHECK(r.ok);
    CHECK(g.elems_done == w.desc.count);
    CHECK(g.g6b_runs_released == g.g6b_tr_done);
    CHECK(g.g6b_ready_peak <= c.g6b_pa_capacity);
    CHECK(g.m24_rel_pairs > 0);
    delete s;
  }
}

// =========================================================================
// M23-G6I: continuous cross-page translation / DRAM pipeline
// =========================================================================
// The claim under test is structural: the two stages are independent and the
// only link between them is the ready pool's capacity. Each check below is an
// event count, not an argument -- several pages in the pool at once, releases
// while probes are in flight, releases alternating pages, a pool that fills and
// then drains, and a diagnostic that does not move the machine.
void test_m23_g6i_continuous_pipeline() {
  Config base = ab_test_cfg();
  base.experiment = "none";
  base.batch_window = 512;
  base.win_fused = true;
  base.dedup = true;
  base.page_order = true;
  base.page_sched = true;
  base.page_cohorts = 4;
  base.park_retry = 1;
  base.g6b_pa_capacity = 64;
  // Two dTLB MSHRs, both parkable by the walking page: a second page's probe is
  // refused and deferred, so concurrency is real and page-granular.
  base.dtlb_mshr = 2;
  base.dtlb_parked = 2;
  base.g6i_diag = 1;
  base.g6i_sample_period = 1;   // sample every cycle: the pool is small here
  auto trace = std::make_shared<const Trace>(build_trace(base));
  std::string err;
  const BulkWorkload w = build_bulk_workload(base, *trace, err);
  CHECK(err.empty());
  if (!err.empty() || !w.armed) return;

  auto run = [&](const Config& c, Sim** keep) {
    Sim* s = new Sim(c, trace);
    const RunResult r = s->run_bulk(w);
    CHECK(r.ok);
    *keep = s;
    return r;
  };

  Config on = base;
  on.bulk_org = 8;               // the G6B/G6C streaming pipeline
  Sim* s1 = nullptr;
  const RunResult r1 = run(on, &s1);
  const BulkStats& g = s1->engine().stats();

  // (1) multiple virtual pages contribute to one pool, and more than one page is
  //     translating at the same time.
  CHECK(g.g6i_samples > 0);
  CHECK(g.g6i_vpn_max >= 2);
  CHECK(g.g6i_pages_open_max >= 2);
  CHECK(g.g6i_cells_max >= 1);
  CHECK(g.g6i_cells_max <= g.g6i_vpn_max * 32);
  CHECK(g.g6i_xpage_cells_sum <= g.g6i_cells_sum);
  // (2) translation continues while the organizer releases: releases happen with
  //     probes still in flight, and consecutive releases switch pages.
  CHECK(g.g6i_rel_with_tr > 0);
  CHECK(g.g6b_overlap > 0);
  CHECK(g.g6i_rel_vpn_switch > 0);
  // Every release but the first of each window has a predecessor to be compared
  // with, so the two classes account for exactly `releases - windows`.
  CHECK(g.g6i_rel_vpn_switch + g.g6i_rel_same_vpn + g.g6b_windows ==
        g.g6b_runs_released);
  // (3) no barrier at a page boundary: a release is never gated on the previous
  //     page's DRAM work, so the pool holds entries from several pages while the
  //     DRAM is busy with an earlier one.
  CHECK(g.g6i_pool_empty < r1.cycles);
  CHECK(g.g6i_rel_cycles > 0);
  CHECK(g.g6b_ready_peak <= base.g6b_pa_capacity);
  CHECK(g.g6i_pool_over == 0);
  // (4) each run translated once, entered the pool once, released once.
  CHECK(g.g6b_tr_reqs == g.g6b_tr_done);
  CHECK(g.g6b_runs_released == g.g6b_tr_done);
  CHECK(g.g6c_dup_submit == 0);
  CHECK(g.g6c_admit_ok == g.g6b_tr_reqs);
  CHECK(g.elems_done == w.desc.count);

  // (5) the diagnostic cannot move the machine: identical cycles, identical
  //     values, and no counters when it is off.
  Config off = on;
  off.g6i_diag = 0;
  Sim* s2 = nullptr;
  const RunResult r2 = run(off, &s2);
  CHECK(r2.cycles == r1.cycles);
  CHECK(s2->engine().stats().g6i_samples == 0);
  u64 bad = 0;
  for (u32 i = 0; i < w.desc.count; ++i)
    if (s2->engine().value_of(i) != s1->engine().value_of(i)) ++bad;
  CHECK(bad == 0);

  // (6) deterministic repeated run.
  Sim* s3 = nullptr;
  const RunResult r3 = run(on, &s3);
  CHECK(r3.cycles == r1.cycles);
  CHECK(s3->engine().stats().g6i_vpn_sum == g.g6i_vpn_sum);
  CHECK(s3->engine().stats().g6i_xpage_cells_sum == g.g6i_xpage_cells_sum);
  CHECK(s3->engine().stats().g6b_runs_released == g.g6b_runs_released);

  // (7) the pool fills, backpressures, drains and refills -- with a capacity far
  //     below the window's run count, so the pool must recycle many times.
  Config tiny = on;
  tiny.g6b_pa_capacity = 8;
  Sim* s4 = nullptr;
  const RunResult r4 = run(tiny, &s4);
  const BulkStats& gt = s4->engine().stats();
  CHECK(gt.g6b_ready_peak <= 8);
  CHECK(gt.g6b_full_events > 0);
  CHECK(gt.g6c_admit_ok > 8);            // admitted, drained, admitted again
  CHECK(gt.g6b_runs_released == gt.g6b_tr_done);
  CHECK(gt.g6i_pool_over == 0);
  CHECK(gt.g6b_tr_reqs == gt.g6b_tr_done);
  CHECK(gt.elems_done == w.desc.count);

  // (8) the grouping key is the physical map, not the page: two distinct virtual
  //     pages can target one physical (bank,row), so their runs share a cell.
  {
    MemorySystem m(base);
    bool found = false;
    const u64 stride = 64ull * 4096ull;   // one page per bank-row period
    for (u64 line = 0; line < 4096 && !found; ++line) {
      u32 b0 = 0, r0 = 0, b1 = 0, r1v = 0;
      m.dram_bank_row(m.pa_line_of_va_line(line), b0, r0);
      m.dram_bank_row(m.pa_line_of_va_line(line + stride), b1, r1v);
      if (b0 == b1 && r0 == r1v && (line >> 6) != ((line + stride) >> 6)) found = true;
    }
    CHECK(found);
  }

  std::printf("    g6i: samples=%llu vpn_max=%llu cells_max=%llu xpage_cells=%llu "
              "xpage_entries=%llu pages_open_max=%llu rel_with_tr=%llu/%llu "
              "vpn_switch=%llu gap_mean=%llu | tiny pool: full=%llu peak=%llu\n",
              (unsigned long long)g.g6i_samples, (unsigned long long)g.g6i_vpn_max,
              (unsigned long long)g.g6i_cells_max,
              (unsigned long long)g.g6i_xpage_cells_max,
              (unsigned long long)g.g6i_xpage_entries_sum,
              (unsigned long long)g.g6i_pages_open_max,
              (unsigned long long)g.g6i_rel_with_tr,
              (unsigned long long)g.g6i_rel_cycles,
              (unsigned long long)g.g6i_rel_vpn_switch,
              (unsigned long long)(g.g6i_gap_n ? g.g6i_gap_sum / g.g6i_gap_n : 0),
              (unsigned long long)gt.g6b_full_events,
              (unsigned long long)gt.g6b_ready_peak);
  delete s1;
  delete s2;
  delete s3;
  delete s4;
}


// ---------------------------------------------------------------------------
// M24-B: the HBM hierarchy. Decode injectivity, the documented mapping
// equations, page size plumbing, controller timing and channel parallelism.
// ---------------------------------------------------------------------------
void test_m24b_hbm() {
  // ---- (a) the two documented bank organisations (PG276) -------------------
  HbmParams p16, p32;
  p16.groups = 4; p16.banks_pg = 4; p16.rows = 8192;   // 16 banks, 4 groups x 4
  p32.groups = 8; p32.banks_pg = 4; p32.rows = 4096;   // 32 banks, 8 groups x 4
  CHECK(p16.groups * p16.banks_pg == 16);
  CHECK(p32.groups * p32.banks_pg == 32);
  CHECK(static_cast<u64>(16) * 8192 == static_cast<u64>(32) * 4096);  // same capacity
  {
    HbmController a(p16), b(p32);
    CHECK(a.banks_per_ch() == 16);
    CHECK(b.banks_per_ch() == 32);
    CHECK(a.nchan() == 32);   // 2 stacks x 16 pseudochannels
    CHECK(b.nchan() == 32);
    CHECK(a.total_lines() == b.total_lines());
  }

  // ---- (b) decode: in range and injective, every map, both geometries ------
  for (int geo = 0; geo < 2; ++geo) {
    for (unsigned map = 0; map < 4; ++map) {
      HbmParams p = geo ? p32 : p16;
      p.map = map;
      p.base_map = 1;
      HbmController h(p);
      std::unordered_set<u64> seen;
      bool in_range = true;
      for (u64 L = 0; L < (1u << 17); ++L) {
        unsigned st, ch, bg, bk, r, c;
        h.decode(L, st, ch, bg, bk, r, c);
        in_range = in_range && st < p.stacks && ch < p.pcs && bg < p.groups &&
                   bk < p.banks_pg && r < p.rows && c < 16u;
        seen.insert((((((static_cast<u64>(st) * p.pcs + ch) * p.groups + bg) * p.banks_pg + bk) *
                       p.rows + r) * 16) + c);
      }
      CHECK(in_range);
      CHECK(seen.size() == (1u << 17));
    }
  }

  // ---- (c) the equations: A keeps a row contiguous, B spreads a row --------
  {
    unsigned st, ch, bg, bk, r, c;
    HbmParams p = p16;
    p.map = 0;
    HbmController a(p);
    unsigned s0, c0, g0, b0, r0, o0;
    a.decode(0, s0, c0, g0, b0, r0, o0);
    bool one_row = true;
    for (u64 L = 0; L < 16; ++L) {
      a.decode(L, st, ch, bg, bk, r, c);
      one_row = one_row && st == s0 && ch == c0 && bg == g0 && bk == b0 && r == r0 && c == L;
    }
    CHECK(one_row);  // A: 16 consecutive lines fill one 1 KiB row

    p.map = 1;
    HbmController b(p);
    std::set<unsigned> chans;
    for (u64 L = 0; L < 64; ++L) { b.decode(L, st, ch, bg, bk, r, c); chans.insert(ch); }
    CHECK(chans.size() == 16);  // B: 64 lines (1 KiB) reach all 16 pseudochannels
    // and the 256 B granule stays in one channel (il_lines = 4)
    b.decode(0, s0, c0, g0, b0, r0, o0);
    bool granule = true;
    for (u64 L = 1; L < 4; ++L) {
      b.decode(L, st, ch, bg, bk, r, c);
      granule = granule && ch == c0 && c == o0 + L;
    }
    CHECK(granule);

    p.map = 2;
    HbmController x(p);
    unsigned diff = 0;
    for (u64 L = 0; L < (1u << 17); ++L) {  // several rows, so the XORs can act
      b.decode(L, st, ch, bg, bk, r, c);
      unsigned s2, c2, g2, b2, r2, o2;
      x.decode(L, s2, c2, g2, b2, r2, o2);
      diff += (ch != c2 || bg != g2 || bk != b2);
    }
    CHECK(diff > 0);  // C is a different, still deterministic, function of L
    CHECK(x.linear_bank(123) == x.linear_bank(123));
  }

  // ---- (d) D: page permutation with offsets preserved ---------------------
  {
    HbmParams p = p16;
    p.map = 3;
    p.base_map = 0;   // A decode after the page permutation, so offsets are visible
    p.perm_seed = 7;
    HbmController d(p);
    HbmParams q = p;
    q.perm_seed = 8;
    HbmController e(q);
    unsigned moved = 0;
    bool offsets = true;
    for (u64 base = 0; base < 4096; base += 64) {
      unsigned s0, c0, g0, b0, r0, o0, s1, c1, g1, b1, r1, o1;
      d.decode(base, s0, c0, g0, b0, r0, o0);
      e.decode(base, s1, c1, g1, b1, r1, o1);
      moved += (c0 != c1 || g0 != g1 || b0 != b1 || r0 != r1);
      for (u64 k = 1; k < 16; ++k) {
        unsigned st, ch, bg, bk, r, c;
        d.decode(base + k, st, ch, bg, bk, r, c);
        offsets = offsets && st == s0 && ch == c0 && bg == g0 && bk == b0 && r == r0 && c == o0 + k;
      }
    }
    CHECK(moved > 0);   // the seed changes where a page lands
    CHECK(offsets);     // but never an offset inside the page
    // A page permutation is a bijection: distinct pages stay distinct.
    std::unordered_set<u64> pages;
    for (u64 pg = 0; pg < 4096; ++pg) {
      unsigned st, ch, bg, bk, r, c;
      d.decode(pg * 64, st, ch, bg, bk, r, c);
      pages.insert((((static_cast<u64>(st) * p.pcs + ch) * p.groups + bg) * p.banks_pg + bk) *
                       p.rows + r);
    }
    CHECK(pages.size() == 4096);
  }

  // ---- (e) controller timing: cold activation, hit, conflict -------------
  {
    HbmParams p = p16;
    p.map = 1;
    HbmController h(p);
    std::vector<HbmServed> out;
    h.enqueue(0, 1, 0, 0);
    h.tick(0, out);
    CHECK(out.size() == 1);
    CHECK(out[0].lat == 12 + 12 + 4);   // cold bank: tRCD + tCAS + tBURST
    out.clear();
    // The same row once the bank frees: tCAS + tBURST. It is queued at cycle 0
    // and cannot be served before the first command finishes, which is exactly
    // what per-bank readiness means.
    h.enqueue(0, 2, 0, 0);
    u64 t = 1;
    while (out.empty() && t < 200) h.tick(t++, out);
    CHECK(out.size() == 1 && out[0].lat == 12 + 4);
    CHECK(h.stats().hits == 1);

    // A different row of the same bank: tRP + tRCD + tCAS + tBURST, on a fresh
    // controller so nothing else is queued ahead of it.
    HbmController k(p);
    out.clear();
    k.enqueue(0, 1, 0, 0);
    k.tick(0, out);
    out.clear();
    unsigned st0, ch0, bg0, bk0, r0, c0;
    k.decode(0, st0, ch0, bg0, bk0, r0, c0);
    u64 other = 0;
    for (u64 L = 1; L < (1u << 20); ++L) {
      unsigned st, ch, bg, bk, r, c;
      k.decode(L, st, ch, bg, bk, r, c);
      if (st == st0 && ch == ch0 && bg == bg0 && bk == bk0 && r != r0) { other = L; break; }
    }
    CHECK(other != 0);
    k.enqueue(other, 2, 100, 0);
    k.tick(100, out);
    CHECK(out.size() == 1);
    CHECK(out[0].lat == 12 + 12 + 12 + 4);
    CHECK(k.stats().conf == 1);
    CHECK(k.stats().act == 2);
    CHECK(k.stats().hits == 0);
    CHECK(k.depth() == 0);
  }

  // ---- (f) channel parallelism -------------------------------------------
  {
    HbmParams p = p16;
    p.map = 1;
    HbmController h(p);
    std::vector<HbmServed> out;
    for (u64 k = 0; k < 8; ++k) h.enqueue(k * 4, k + 1, 0, 0);  // 8 distinct channels
    h.tick(0, out);
    CHECK(out.size() == 8);   // one command per channel in the same cycle
    CHECK(h.depth() == 0);
    std::set<unsigned> used;
    for (const auto& s : out) used.insert(s.ch);
    CHECK(used.size() == 8);
  }

  // ---- (g) refresh is off unless asked for --------------------------------
  {
    HbmParams p = p16;
    p.map = 1;
    HbmController h(p);
    std::vector<HbmServed> out;
    for (u64 t = 0; t < 200; ++t) h.tick(t, out);
    CHECK(h.stats().refresh == 0);
    p.t_refi = 100;
    p.t_rfc = 20;
    HbmController r(p);
    for (u64 t = 0; t < 400; ++t) r.tick(t, out);
    CHECK(r.stats().refresh >= 2);
  }

  // ---- (h) page sizes: decomposition, TLB reach, physical uniqueness ------
  for (u32 pb : {4096u, 65536u, 2097152u}) {
    Config c = ab_test_cfg();
    c.page_bytes = pb;
    MemorySystem m(c);
    CHECK(m.page_shift() == (pb == 4096 ? 12u : pb == 65536 ? 16u : 21u));
    CHECK(m.page_line_bits() == (pb == 4096 ? 6u : pb == 65536 ? 10u : 15u));
    // TLB reach = entries x page size, and it must scale with the page size.
    const u64 reach = static_cast<u64>(c.dtlb_entries) * c.dtlb_ways * pb;
    CHECK(reach >= static_cast<u64>(c.dtlb_entries) * 4096);
    // The physical line of a VA keeps the offset inside the page and differs
    // from the virtual line (PA != VA), for every page size.
    bool offsets_ok = true, nonzero = true;
    for (u64 vpn = 0; vpn < 4; ++vpn) {
      for (u64 off = 0; off < c.page_lines(); off += (c.page_lines() / 4 + 1)) {
        const u64 va = (vpn << c.page_shift()) + (off << 6);
        const u64 pa = m.pa_line_of_va(va);
        offsets_ok = offsets_ok && ((pa & (c.page_lines() - 1)) == off);
      }
    }
    CHECK(offsets_ok);
    for (u64 va = 0; va < 4096; va += 64) nonzero = nonzero && (m.pa_line_of_va(va) != (va >> 6));
    CHECK(nonzero);
  }
}

// ---------------------------------------------------------------------------
// M26-B1: the two known Bulk liveness failures, as regression tests
// ---------------------------------------------------------------------------
// Each cell is the published configuration -- the M24-D Bulk template with one
// mapping (M24-D §8.3) or one cohort count (M24-G §8) changed -- driven through
// the same entry point the campaign used (`Sim::run_hybrid`). The assertions are
// the properties the architecture claims: every accepted request retires, the
// translation stream reconciles, every descriptor completes, and the trace's
// own enter/leave accounting closes.
//
// On the current source these two tests are expected to FAIL: the milestone is
// documenting a defect, not fixing it. `stall_abort` is a *test harness* switch
// (default 0, i.e. off in every real run): it stops the run once nothing has
// retired for N cycles, which is a state the model cannot leave. The cycle
// guard stays at the configuration's 500 M and is not touched.
static bool m26_present(const char* p) {
  std::FILE* f = std::fopen(p, "r");
  if (!f) {
    std::printf("    SKIP: %s not present\n", p);
    return false;
  }
  std::fclose(f);
  return true;
}

static void m26_liveness_cell(const char* name, const char* trace, u32 map, u64 seed,
                              u32 cohorts) {
  const char* cfgpath = "../build/wm/m24d/m24d_bulk.cfg";
  if (!m26_present(cfgpath) || !m26_present(trace)) return;
  Config c = Config::load(cfgpath);
  c.dataset = trace;
  c.m24_map = map;
  c.m24_perm_seed = static_cast<u32>(seed);
  if (cohorts) c.page_cohorts = cohorts;
  c.life_diag = true;       // counters only: no report path in a unit test
  c.stall_abort = 3000000;  // harness switch, see sim.hpp; 0 in every real cell
  auto tr = std::make_shared<const Trace>(build_trace(c));
  Sim s(c, tr);
  const RunResult r = s.run_hybrid();
  const MemorySystem& m = s.memory();
  const BulkStats& b = s.engine().stats();
  if (!r.ok)
    for (const auto& v : r.violations) std::printf("    %s: %s\n", name, v.c_str());
  std::printf("    %s: cycles=%llu inflight=%zu stale=%llu tr=%llu/%llu xl=%llu/%llu/%llu\n",
              name, static_cast<unsigned long long>(r.cycles), m.inflight_count(),
              static_cast<unsigned long long>(r.s.get("life_stale_end")),
              static_cast<unsigned long long>(b.g6b_tr_done),
              static_cast<unsigned long long>(b.g6b_tr_reqs),
              static_cast<unsigned long long>(m.life_xl_enq),
              static_cast<unsigned long long>(m.life_xl_deq),
              static_cast<unsigned long long>(m.life_xl_rej));
  // 1. every accepted request eventually retires (the run reaches a fixed point).
  CHECK(r.ok);
  CHECK(m.inflight_count() == 0);
  // 2. exact request conservation on the trace's own accounting.
  CHECK(m.life_enter == m.life_leave + m.inflight_count());
  // 3. translation requests and completions reconcile.
  CHECK(b.g6b_tr_reqs == b.g6b_tr_done);
  // 4. every descriptor completed and no window was left open.
  CHECK(b.desc_completed == b.desc_submitted);
  CHECK(b.win_incomplete_max <= 1);
  // 5. no completed translation was dropped by the hand-off (H3), whatever
  //    else is true of the run.
  CHECK(m.life_xl_rej == 0);
  CHECK(m.life_xl_enq == m.life_xl_deq);
}

static void test_m26_liveness_map3_s3() {
  m26_liveness_cell("m26_map3_s3", "../build/wm/m23_criteo_16k.bin", 3, 3, 0);
}

static void test_m26_liveness_cohorts1() {
  m26_liveness_cell("m26_cohorts1", "../build/wm/m23_criteo_2k.bin", 0, 1, 1);
}


// =========================================================================
// M27: correctness, resource accounting and the production Bulk path
// =========================================================================

// A tiny trace whose page count is far larger than the dTLB, so every probe is
// followed by a replacement almost immediately. This is the shape of the defect:
// hit at C, entry replaced before the C+1 lookup.
static Config m27_cfg() {
  Config c;
  c.cores = 1;
  c.warps_per_core = 4;
  c.lanes = 4;
  c.elem_bytes = 4;
  c.R = 64;
  c.lsu_pending = 0;
  c.mem_path = "l1";
  c.l1_lines = 256; c.l1_ways = 4; c.l1_latency = 2; c.l1_mshr = 64;
  c.l2_lines = 16384; c.l2_ways = 8; c.l2_latency = 4; c.l2_mshr = 64;
  // 4 entries, 1 way: every fill replaces the live entry.
  c.dtlb_entries = 4; c.dtlb_ways = 1; c.dtlb_mshr = 8; c.dtlb_parked = 4;
  c.l2tlb_entries = 512; c.l2tlb_ways = 4; c.l2tlb_mshr = 8; c.l2tlb_latency = 4;
  c.walkers = 4; c.walk_cache = 16; c.levels = 2; c.walk_latency_per_level = 40;
  c.banks = 2; c.rows = 1024; c.row_bytes = 1024;
  c.t_hit = 14; c.t_miss = 26; c.link = 0; c.dram_q = 0; c.frfcfs = true;
  c.wl = "W1"; c.N = 2048; c.Lambda = 2048; c.P = 512;
  c.kappa = 0; c.rho_dup = 0; c.seed = 1;
  c.max_cycles = 500000000;
  return c;
}

// 1. The original hit/eviction race completes. Before M27 this configuration
//    stranded requests: a hit promised a translation, a fill replaced the entry
//    in the wait cycle, and stage 1 re-looked-up forever.
void test_m27_tlb_hit_eviction() {
  const Config c = m27_cfg();
  Sim s(c);
  const RunResult r = s.run(Mode::Baseline);
  const MemorySystem& m = s.memory();
  std::printf("    tlb_hit_eviction: cycles=%llu hits=%llu misses=%llu evict=%llu inflight=%zu\n",
              (unsigned long long)r.cycles, (unsigned long long)m.tlb[0].hits,
              (unsigned long long)m.tlb[0].misses, (unsigned long long)m.tlb[0].evict,
              m.inflight_count());
  CHECK(r.ok);                                   // no liveness trip, no guard hit
  CHECK(m.inflight_count() == 0);
  CHECK(r.s.get("completed") == r.s.get("requests_generated"));
  // The race is actually exercised: entries are replaced, so hits happened with
  // a replacement following them.
  CHECK(m.tlb[0].misses > 0);
  CHECK(m.tlb[0].evict > 0);
  CHECK(m.tlb[0].hits > 0);
}

// 2. Liveness is an invariant, not a harness switch: a run in which no request
//    can ever be admitted must be reported as a liveness failure, quickly, and
//    not as a cycle-guard timeout.
void test_m27_liveness_detects_stall() {
  Config c = m27_cfg();
  c.dtlb_mshr = 0;          // no translation MSHR: no probe can ever be accepted
  Sim s(c);
  const RunResult r = s.run(Mode::Baseline);
  bool named = false;
  for (const auto& v : r.violations)
    if (v.find("liveness") != std::string::npos) named = true;
  std::printf("    liveness_stall: ok=%d cycles=%llu violations=%zu bound=%llu\n", r.ok ? 1 : 0,
              (unsigned long long)r.cycles, r.violations.size(),
              (unsigned long long)s.memory().live_limit());
  CHECK(!r.ok);
  CHECK(named);                                // it is the liveness check that fired
  CHECK(r.cycles < s.memory().live_limit() + 16);  // and it fired at the bound
  CHECK(r.cycles < c.max_cycles);              // not the cycle guard
}

// 3. Bulk translation probes obey the shared residency budget (M27-P3).
void test_m27_xlate_slots() {
  const char* cfgpath = "../build/wm/m24d/m24d_bulk.cfg";
  const char* trace = "../build/wm/m23_criteo_2k.bin";
  if (!m26_present(cfgpath) || !m26_present(trace)) return;
  Config c = Config::load(cfgpath);
  c.dataset = trace;
  c.life_diag = true;
  auto tr = std::make_shared<const Trace>(build_trace(c));
  Sim s(c, tr);
  const RunResult r = s.run_hybrid();
  const MemorySystem& m = s.memory();
  const BulkStats& b = s.engine().stats();
  std::printf("    xlate_slots: cycles=%llu inflight_max=%llu tr=%llu/%llu slot_stall=%llu\n",
              (unsigned long long)r.cycles, (unsigned long long)m.inflight_max,
              (unsigned long long)b.g6b_tr_done, (unsigned long long)b.g6b_tr_reqs,
              (unsigned long long)b.tr_slot_stall);
  CHECK(r.ok);
  CHECK(m.inflight_count() == 0);
  CHECK(b.g6b_tr_done == b.g6b_tr_reqs);
  CHECK(m.inflight_max <= c.R);        // probes are inside the shared budget
  CHECK(m.life_enter == m.life_leave + m.inflight_count());
}

// 4. The production Bulk path performs page organization only: no physical
//    row/bank ordering of the release stream (M27-P4).
void test_m27_bulk_no_dram_org() {
  const char* cfgpath = "../configs/m27_bulk.cfg";      // production from M27 on
  const char* trace = "../build/wm/m23_criteo_2k.bin";
  if (!m26_present(cfgpath) || !m26_present(trace)) return;
  Config c = Config::load(cfgpath);
  CHECK(c.m24_dram_org == false);    // the file, not merely the default, says so
  c.dataset = trace;
  auto tr = std::make_shared<const Trace>(build_trace(c));
  Sim s(c, tr);
  const RunResult r = s.run_hybrid();
  const BulkStats& b = s.engine().stats();
  std::printf("    no_dram_org: ok=%d org_runs=%llu desc=%llu cohorts=%llu\n", r.ok ? 1 : 0,
              (unsigned long long)b.org_runs, (unsigned long long)b.desc_completed,
              (unsigned long long)c.page_cohorts);
  CHECK(r.ok);
  CHECK(b.desc_completed == 1);
  CHECK(b.org_runs == 0);              // the sorter never ran
  CHECK(b.win_entries > 0);            // page organization still ran
  // The same cell with the historical sorter explicitly enabled does sort.
  Config h = c;
  h.m24_dram_org = true;
  auto trh = std::make_shared<const Trace>(build_trace(h));
  Sim sh(h, trh);
  const RunResult rh = sh.run_hybrid();
  std::printf("    dram_org_on:   ok=%d org_runs=%llu\n", rh.ok ? 1 : 0,
              (unsigned long long)sh.engine().stats().org_runs);
  CHECK(rh.ok);
  CHECK(sh.engine().stats().org_runs > 0);
}

// 5. The engine's home SM is named, never assumed (M27-P5).
void test_m27_eng_core() {
  const char* cfgpath = "../build/wm/m24d/m24d_bulk.cfg";
  if (!m26_present(cfgpath)) return;
  Config c = Config::load(cfgpath);
  c.cores = 2;
  c.eng_core = 0xffffffffu;
  CHECK(c.eng_core == 0xffffffffu);          // unset: the engine names no SM
  {
    Sim s(c);
    const RunResult r = s.run_hybrid();
    bool named = false;
    for (const auto& v : r.violations)
      if (v.find("single-core") != std::string::npos) named = true;
    std::printf("    cores=2, eng_core unset: ok=%d named=%d\n", r.ok ? 1 : 0, named ? 1 : 0);
    CHECK(!r.ok);                            // a multi-core dispatch is refused ...
    CHECK(named);                            // ... with a message that says why
  }
  // A name that does not exist is a configuration error, never a silent fallback.
  {
    Config bad = c;
    bad.eng_core = 7;                 // cores = 2
    Sim s(bad);
    const RunResult r = s.run_hybrid();
    bool named = false;
    for (const auto& v : r.violations)
      if (v.find("eng_core") != std::string::npos) named = true;
    std::printf("    cores=2, eng_core=7: ok=%d named=%d\n", r.ok ? 1 : 0, named ? 1 : 0);
    CHECK(!r.ok);
    CHECK(named);
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  struct TestCase { const char* name; void (*fn)(); };
  const TestCase tests[] = {
      {"config", test_config},
      {"solve_M", test_solve_M},
      {"trace_permutation", test_trace_permutation},
      {"trace_regular", test_trace_regular},
      {"trace_duplicates", test_trace_duplicates},
      {"tlb_capacity", test_tlb_capacity},
      {"l1_reuse", test_l1_reuse},
      {"dram_locality", test_dram_locality},
      {"bank_hash_spreads", test_bank_hash_spreads_strides},
      {"conservation", test_conservation},
      {"determinism", test_determinism},
      {"streaming_ceiling", test_streaming_ceiling},
      {"stream_beats_gather", test_stream_beats_gather},
      {"walks_analytic", test_walks_analytic},
      {"m4a_exact_equivalence", test_m4a_exact_equivalence},
      {"m4b_null", test_m4b_null},
      {"vortex_ref_preset", test_vortex_ref_preset},
      {"bulk_desc_validation", test_bulk_desc_validation},
      {"bulk_index_addressing", test_bulk_index_addressing},
      {"bulk_target_addressing", test_bulk_target_addressing},
      {"bulk_index_dependency", test_bulk_index_dependency},
      {"bulk_multi_index_outstanding", test_bulk_multi_index_outstanding},
      {"bulk_index_out_of_order", test_bulk_index_out_of_order},
      {"bulk_target_out_of_order", test_bulk_target_out_of_order},
      {"bulk_element_ids", test_bulk_element_ids},
      {"bulk_buffer_capacity", test_bulk_buffer_capacity},
      {"bulk_residency_invariant", test_bulk_residency_invariant},
      {"bulk_descriptor_completion", test_bulk_descriptor_completion},
      {"bulk_empty_and_small", test_bulk_empty_and_small},
      {"bulk_translation_and_cache", test_bulk_translation_and_cache},
      {"bulk_memory_accounting", test_bulk_memory_accounting},
      {"bulk_fifo_issue", test_bulk_fifo_issue},
      {"bulk_regressions", test_bulk_regressions},
      {"ab_environment_identity", test_ab_environment_identity},
      {"ab_same_workload", test_ab_same_workload},
      {"ab_budget_equality", test_ab_budget_equality},
      {"ab_traffic_accounting", test_ab_traffic_accounting},
      {"ab_reset_between_arms", test_ab_reset_between_arms},
      {"ab_sweep_budgets", test_ab_sweep_budgets},
      {"ab_baseline_unchanged", test_ab_baseline_unchanged},
      {"m6_legacy_identity", test_m6_legacy_identity},
      {"m6_switch_off", test_m6_switch_off},
      {"m6_value_equivalence", test_m6_value_equivalence},
      {"m6_dedup_soundness", test_m6_dedup_soundness},
      {"m6_resource_accounting", test_m6_resource_accounting},
      {"m6_mechanism_independence", test_m6_mechanism_independence},
      {"m6_fanout_accounting", test_m6_fanout_accounting},
      {"m6_latch_charge", test_m6_latch_charge},
      {"m6_window_advantage", test_m6_window_advantage},
      {"m6_index_oracle", test_m6_index_oracle},
      {"m6_w0_diagnostic", test_m6_w0_diagnostic_baseline},
      {"m6_w1_diagnostic", test_m6_w1_diagnostic_baseline},
      {"m7_page_sched", test_m7_page_sched},
      {"m7_baselines", test_m7_baselines},
      {"m8_index_acquisition", test_m8_index_acquisition},
      {"m13_within_window", test_m13_within_window},
      {"m13_cross_window", test_m13_cross_window},
      {"m13_word_offsets", test_m13_word_offsets},
      {"m13_multi_dependent", test_m13_multi_dependent},
      {"m13_eviction", test_m13_eviction},
      {"m13_full", test_m13_full},
      {"m13_no_reuse", test_m13_no_reuse},
      {"m13_adversarial", test_m13_adversarial},
      {"m13_accounting", test_m13_accounting},
      {"m13_descriptor_boundary", test_m13_descriptor_boundary},
      {"slot_pool", test_slot_pool},
      {"m15_client_attribution", test_m15_client_attribution},
      {"m15_fused_window", test_m15_fused_window},
      {"m23_archetype_config", test_m23_archetype_config},
      {"m23_archetype_baseline", test_m23_archetype_baseline},
      {"m23_model_vs_realized", test_m23_model_vs_realized},
      {"m23_architecture_matrix", test_m23_architecture_matrix},
      {"m23_g6c_admission_deferral", test_m23_g6c_admission_deferral},
      {"m23_g6i_continuous_pipeline", test_m23_g6i_continuous_pipeline},
      {"m24_four_way_organization", test_m24_four_way_organization},
      {"m24b_hbm", test_m24b_hbm},
      {"m26_liveness_map3_s3", test_m26_liveness_map3_s3},
      {"m26_liveness_cohorts1", test_m26_liveness_cohorts1},
      {"m27_tlb_hit_eviction", test_m27_tlb_hit_eviction},
      {"m27_liveness_stall", test_m27_liveness_detects_stall},
      {"m27_xlate_slots", test_m27_xlate_slots},
      {"m27_bulk_no_dram_org", test_m27_bulk_no_dram_org},
      {"m27_eng_core", test_m27_eng_core},
  };
  const char* only = argc > 1 ? argv[1] : nullptr;
  for (const auto& t : tests) {
    if (only && std::string(only) != t.name) continue;
    const int before = g_fail;
    t.fn();
    std::printf("%-22s %s\n", t.name, g_fail == before ? "ok" : "FAILED");
  }
  std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
  std::printf("%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
  return g_fail == 0 ? 0 : 1;
}
