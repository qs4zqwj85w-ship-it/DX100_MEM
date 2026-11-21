// Memory system: L1 TLB + MSHR, shared L2 TLB, page-table walker, L1/L2 data
// caches, and a banked DRAM with FR-FCFS scheduling.
//
// Structures and capacities follow Vortex (design §4.1, [V] rows); cycle
// timings are the [A] parameters of that table and are the only fitted values.
#include "sim.hpp"
#include "hbm.hpp"

#include <algorithm>
#include <memory>
#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace dx100 {

namespace {
constexpr u64 VPN_SHIFT = 6;               // 64 x 64 B lines per 4 KiB page
constexpr u64 PAGE_LINES = 64;
constexpr u64 ROOT_BASE = 0x00200000ull;   // root page table (1 line)
constexpr u64 LEAF_BASE = 0x00400000ull;   // leaf tables, 16 PTEs per line
// M24-B: a *byte* offset, not a page count. 64 MiB is exactly 16384 pages at
// 4 KiB (the historical PPN_OFFSET), and it keeps the physical window inside the
// 4 GiB HBM space at 64 KiB and 2 MiB pages too. A page-count offset would push
// 2 MiB pages past 38 GiB and alias them onto each other.
constexpr u64 PA_OFFSET_BYTES = 0x4000000ull;

// M24-B: one page-geometry-aware form. `off` is the line offset inside the
// page; the shift is the configured page's line bits (6, 10 or 15), so the
// arithmetic is the historical one for 4 KiB pages.
inline u64 pa_line_of(u64 ppn, u64 off, u32 line_bits) {
  return (ppn << line_bits) | (off & ((1ull << line_bits) - 1));
}
inline u64 pte_line_of(u32 level, u64 vpn) {
  return level == 0 ? ROOT_BASE : LEAF_BASE + (vpn / 16);
}
// Deterministic filler for a line never written by the host. Keeps the image
// sparse (no allocation cost, no memory-size limit) while making every read
// well defined and reproducible.
// Diagnostic bucket: floor(log2(v)), capped at 15, so bucket b covers
// [2^b, 2^(b+1)) and bucket 15 is [32768, inf). v = 0 lands in bucket 0.
inline u32 log2_bucket(u64 v) {
  u32 b = 0;
  while (b < 15 && (v >> (b + 1)) != 0) ++b;
  return b;
}
inline u8 synthetic_byte(u64 pa_line, u32 i) {
  u64 h = pa_line * 0x9E3779B97F4A7C15ull + i * 0xC2B2AE3D27D4EB4Full;
  return static_cast<u8>(h >> 56);
}
}  // namespace

MemorySystem::MemorySystem(const Config& cfg) : cfg_(cfg) {
  // M26-B1: latch the lifecycle diagnostic once. Nothing else in this file
  // changes because of it: every new write is inside `if (life_)`.
  life_ = cfg.life_diag;
  life_dump_ = cfg.life_dump;
  tlb_e_.assign(static_cast<size_t>(cfg.cores) * cfg.dtlb_entries, TlbEntry{});
  tlb_m_.assign(static_cast<size_t>(cfg.cores) * cfg.dtlb_mshr, TlbMshr{});
  l2tlb_e_.assign(cfg.l2tlb_entries, L2Entry{});
  l2tlb_m_.assign(cfg.l2tlb_mshr, L2Mshr{});
  walk_busy_.assign(cfg.walkers, 0);
  // Private L1 per core (design §4.1); the L2 is shared per socket.
  l1_e_.assign(static_cast<size_t>(cfg.cores) * cfg.l1_lines, CEntry{});
  l2_e_.assign(cfg.l2_lines, CEntry{});
  l1_m_.assign(static_cast<size_t>(cfg.cores) * cfg.l1_mshr, CMshr{});
  l2_m_.assign(cfg.l2_mshr, CMshr{});
  dram_wait_.reserve(cfg.dram_q + 1);
  // M11.2: only allocated when the scheduler diagnostic is on, so a normal run
  // allocates exactly what it allocated before.
  if (cfg.dram_diag) row_acts_.assign(static_cast<size_t>(cfg.banks) * cfg.rows, 0);
  if (cfg.dram_diag) row_stamp_.assign(static_cast<size_t>(cfg.banks) * cfg.rows, 0);
  bank_ready_.assign(cfg.banks, 0);
  bank_row_.assign(cfg.banks, 0);
  bank_open_.assign(cfg.banks, false);
  tlb.resize(cfg.cores);
  inflight_.reserve(cfg.R);
  // Memory access path (M5.1 §4). "native" keeps the historical split between
  // the front-ends; "l1"/"l2" force both onto one path.
  if (cfg.mem_path == "l1") path_mode_ = 1;
  else if (cfg.mem_path == "l2") path_mode_ = 2;
  else if (cfg.mem_path != "native")
    throw std::runtime_error("mem_path must be native, l1 or l2");
  // M27-P2: the liveness bound. A run cannot be silent for longer than it takes
  // the *slowest* legitimate service to complete: every outstanding request can
  // queue behind every other one at the slowest bank, and the worst case adds
  // one full walk. Eight times that is the trip threshold -- far above any
  // healthy retirement gap, and far below the cycle guard, so a stranded
  // request is reported in ~55 K cycles instead of 500 M.
  live_limit_ = 8ull * (static_cast<u64>(cfg.levels) * cfg.walk_latency_per_level +
                        static_cast<u64>(cfg.t_miss + cfg.l2_latency + cfg.l1_latency) *
                            (static_cast<u64>(cfg.l2_mshr) + cfg.R + 16ull));
  // M23-G4C: the address map is a bijection only for these geometries, and a
  // silent non-bijection would look like a locality result. Refuse instead.
  if (static_cast<u64>(cfg.banks) * cfg.rows * cfg.row_bytes == 0)
    throw std::runtime_error("DRAM geometry must be non-zero");
  if (cfg.row_bytes % LINE_BYTES)
    throw std::runtime_error("row_bytes must be a multiple of 64");
  if (cfg.dram_il_lines == 0 || (cfg.row_bytes / LINE_BYTES) % cfg.dram_il_lines)
    throw std::runtime_error("dram_il_lines must divide row_bytes/64");
  diag_ = cfg.cache_diag;  // M15: latched, never consulted by a decision

  // ---- M24-B: page size and the optional HBM hierarchy ---------------------
  if (cfg.page_bytes != 4096 && cfg.page_bytes != 65536 && cfg.page_bytes != 2097152)
    throw std::runtime_error("page_bytes must be 4096, 65536 or 2097152");
  if (cfg.dram_kind == "hbm") {
    HbmParams hp;
    hp.stacks = cfg.hbm_stacks;
    hp.pcs = cfg.hbm_pcs;
    hp.groups = cfg.hbm_bank_groups;
    hp.banks_pg = cfg.hbm_banks_pg;
    hp.row_bytes = cfg.hbm_row_bytes;
    const u64 per_row = static_cast<u64>(cfg.hbm_stacks) * cfg.hbm_pcs *
                        cfg.hbm_bank_groups * cfg.hbm_banks_pg * cfg.hbm_row_bytes;
    if (per_row == 0 || cfg.hbm_capacity % per_row)
      throw std::runtime_error(
          "hbm_capacity must be a whole multiple of stacks x pcs x bank_groups x banks_pg x row_bytes");
    const u64 r = cfg.hbm_capacity / per_row;
    if (r == 0 || (r & (r - 1)) != 0)
      throw std::runtime_error("hbm: rows per bank must be a power of two");
    hp.rows = static_cast<unsigned>(r);
    hp.capacity_lines = static_cast<unsigned>((cfg.hbm_capacity >> 6) > 0xffffffffull
                                                  ? 0xffffffffu
                                                  : (cfg.hbm_capacity >> 6));
    hp.map = cfg.hbm_map;
    hp.base_map = cfg.hbm_base_map;
    hp.perm_seed = cfg.hbm_perm_seed;
    hp.perm_page_lines = std::max<u32>(1, cfg.hbm_perm_page / 64);
    hp.il_lines = std::max<u32>(1, cfg.hbm_il_lines);
    hp.t_rcd = cfg.hbm_t_rcd; hp.t_rp = cfg.hbm_t_rp; hp.t_ras = cfg.hbm_t_ras;
    hp.t_cas = cfg.hbm_t_cas; hp.t_ccd_s = cfg.hbm_t_ccd_s; hp.t_ccd_l = cfg.hbm_t_ccd_l;
    hp.t_rrd_s = cfg.hbm_t_rrd_s; hp.t_rrd_l = cfg.hbm_t_rrd_l; hp.t_burst = cfg.hbm_t_burst;
    hp.t_faw = cfg.hbm_t_faw; hp.t_refi = cfg.hbm_t_refi; hp.t_rfc = cfg.hbm_t_rfc;
    hp.link = static_cast<unsigned>(cfg.link);
    hbm_.reset(new HbmController(hp));
    hbm_out_.reserve(64);
  } else if (cfg.dram_kind != "ddr") {
    throw std::runtime_error("dram_kind must be ddr or hbm");
  }
}

// ---------------------------------------------------------------------------
// Physical memory image
// ---------------------------------------------------------------------------
u64 MemorySystem::image_hash() const {
  // Order-independent: XOR of a per-line digest, so the value depends only on
  // which lines were written and what they contain.
  u64 h = 1469598103934665603ull;
  for (const auto& kv : img_) {
    u64 d = 1469598103934665603ull;
    for (int i = 0; i < 8; ++i) {
      d ^= (kv.first >> (8 * i)) & 0xff;
      d *= 1099511628211ull;
    }
    for (u32 i = 0; i < LINE_BYTES; ++i) {
      d ^= kv.second[i];
      d *= 1099511628211ull;
    }
    h ^= d;
  }
  h ^= img_.size() * 0x9E3779B97F4A7C15ull;
  return h;
}

// M24-A / M3: the physical-page permutation. A 20-bit page number (the VA space
// is 4 GiB = 2^20 pages) is permuted by a four-round Feistel network over two
// 10-bit halves with a seeded round function. A Feistel network is a bijection
// for *any* round function, so for any seed no frame is lost, none is produced
// twice, and the offsets inside a page are never touched -- the permutation only
// decides which frame a virtual page occupies. Deterministic from the seed alone.
namespace {
inline u32 m24_round(u32 x, u32 k) {
  x ^= k;
  x *= 0x9E3779B1u;
  x ^= x >> 15;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  return x & 1023u;
}
inline u64 m24_permute_page(u64 p, u32 seed) {
  u32 a = static_cast<u32>((p >> 10) & 1023u), b = static_cast<u32>(p & 1023u);
  for (u32 r = 0; r < 4; ++r) {
    const u32 k = seed * 0x9E3779B9u + r * 0x7F4A7C15u;
    const u32 na = b;
    b = a ^ m24_round(b, k);
    a = na;
  }
  return (static_cast<u64>(a) << 10) | b;
}
}  // namespace

u64 MemorySystem::pa_page_of_vpn(u64 vpn) const {
  if (cfg_.m24_map != 3) return vpn + (PA_OFFSET_BYTES >> cfg_.page_shift());
  constexpr u64 pages = 1ull << 20;
  return (PA_OFFSET_BYTES >> cfg_.page_shift()) +
         m24_permute_page(vpn & (pages - 1), cfg_.m24_perm_seed);
}

u64 MemorySystem::pa_line_of_va(u64 va) const {
  // Exactly the mapping tlb_fill installs (ppn = pa_page_of_vpn(vpn)). The TLB
  // and walker model the *cost* of translation; the mapping itself is a function.
  return pa_line_of(pa_page_of_vpn(va >> cfg_.page_shift()), va >> 6, cfg_.page_line_bits());
}

void MemorySystem::write_va(u64 va, const void* src, size_t n) {
  const u8* p = static_cast<const u8*>(src);
  for (size_t i = 0; i < n; ++i) {
    const u64 a = va + i;
    auto& line = img_[pa_line_of_va(a)];
    line[(a) & 63] = p[i];
  }
}

const u8* MemorySystem::line_bytes(u64 pa_line) const {
  auto it = img_.find(pa_line);
  if (it != img_.end()) return it->second.data();
  for (u32 i = 0; i < LINE_BYTES; ++i) scratch_[i] = synthetic_byte(pa_line, i);
  return scratch_.data();
}

// ---------------------------------------------------------------------------
// Translation
// ---------------------------------------------------------------------------
bool MemorySystem::tlb_probe(u32 core, u64 vpn, u64 cycle, u64& ready, u64* ppn_out) {
  auto& st = tlb[core];
  // M23-F1 ideal translation: the access is translated in the cycle it is
  // submitted. No array probe, no MSHR, no L2 TLB, no walk, no PTE traffic, so
  // there is no translation-admission limit either. The VA->PA mapping is the
  // one tlb_fill installs, so every data address is unchanged.
  if (cfg_.ideal_translation) {
    st.reads++;
    st.hits++;
    ready = cycle;
    if (ppn_out) *ppn_out = pa_page_of_vpn(vpn);
    if (life_) { life_probe_ = LE_PROBE_HIT; ++vpn_life(vpn, core).probes; }
    return true;
  }
  const u32 sets = std::max<u32>(1, cfg_.dtlb_entries / std::max<u32>(1, cfg_.dtlb_ways));
  const u32 set = static_cast<u32>(vpn % sets);
  const u32 base = core * cfg_.dtlb_entries + set * cfg_.dtlb_ways;

  st.reads++;
  for (u32 w = 0; w < cfg_.dtlb_ways; ++w) {
    auto& e = tlb_e_[base + w];
    if (e.valid && e.vpn == vpn) {
      st.hits++;
      e.lru = ++lru_clock_;
      ready = cycle + 1;  // 1-cycle L1 TLB lookup
      // M27-P1: the translation is handed to the request here, at the cycle the
      // hit is detected. The array is a cache and may evict this entry before
      // the request looks again; the value it accepted may not.
      if (ppn_out) *ppn_out = e.ppn;
      if (life_) { life_probe_ = LE_PROBE_HIT; ++vpn_life(vpn, core).probes; }
      return true;
    }
  }
  st.misses++;

  const u32 mbase = core * cfg_.dtlb_mshr;
  for (u32 i = 0; i < cfg_.dtlb_mshr; ++i) {
    auto& m = tlb_m_[mbase + i];
    if (m.valid && m.vpn == vpn) {
      if (m.parked >= cfg_.dtlb_parked) {
        refuse_parked++; st.stall_cycles++; st.misses--; st.reads--;
        if (life_) life_probe_ = LE_PROBE_REFUSED;
        return false;
      }
      m.parked++;
      if (m.parked > max_parked) max_parked = m.parked;
      st.mshr_joins++;
      if (life_) { life_probe_ = LE_PROBE_JOIN; ++vpn_life(vpn, core).joins; }
      ready = NONE;
      return true;
    }
  }
  for (u32 i = 0; i < cfg_.dtlb_mshr; ++i) {
    auto& m = tlb_m_[mbase + i];
    if (!m.valid) {
      m.valid = true;
      m.vpn = vpn;
      m.parked = 1;
      if (max_parked < 1) max_parked = 1;
      st.mshr_allocs++;
      if (life_) { life_probe_ = LE_PROBE_ALLOC; ++vpn_life(vpn, core).allocs; }
      l2tlb_pipe_.push_back(PipeEnt{cycle + cfg_.l2tlb_latency, vpn, core});
      ready = NONE;
      return true;
    }
  }
  refuse_full++;
  st.stall_cycles++;
  st.misses--;
  st.reads--;  // a refused probe is backpressure, not a translation event
  if (life_) life_probe_ = LE_PROBE_REFUSED;
  return false;
}

void MemorySystem::tlb_fill(u32 core, u64 vpn) {
  if (life_) { ++life_fill_install; ++vpn_life(vpn, core).installs; }
  const u32 sets = std::max<u32>(1, cfg_.dtlb_entries / std::max<u32>(1, cfg_.dtlb_ways));
  const u32 set = static_cast<u32>(vpn % sets);
  const u32 base = core * cfg_.dtlb_entries + set * cfg_.dtlb_ways;
  for (u32 w = 0; w < cfg_.dtlb_ways; ++w) {
    auto& e = tlb_e_[base + w];
    if (e.valid && e.vpn == vpn) { e.lru = ++lru_clock_; return; }
  }
  u32 victim = base, lru = ~0u;
  bool placed = false;
  for (u32 w = 0; w < cfg_.dtlb_ways; ++w) {
    auto& e = tlb_e_[base + w];
    if (!e.valid) { e.valid = true; e.vpn = vpn; e.ppn = pa_page_of_vpn(vpn); e.lru = ++lru_clock_; placed = true; break; }
    if (e.lru < lru) { lru = e.lru; victim = base + w; }
  }
  if (!placed) {
    auto& e = tlb_e_[victim];
    if (life_) ++life_fill_evict;
    e.valid = true; e.vpn = vpn; e.ppn = pa_page_of_vpn(vpn); e.lru = ++lru_clock_;
    tlb[core].evict++;
  }
}

void MemorySystem::l2tlb_tick(u64 cycle) {
  std::deque<PipeEnt> keep;
  for (auto& pr : l2tlb_pipe_) {
    if (pr.ready > cycle) { keep.push_back(pr); continue; }
    const u64 vpn = pr.vpn;
    l2tlb.reads++;
    const u32 sets = std::max<u32>(1, cfg_.l2tlb_entries / std::max<u32>(1, cfg_.l2tlb_ways));
    const u32 set = static_cast<u32>(vpn % sets);
    bool hit = false;
    for (u32 w = 0; w < cfg_.l2tlb_ways; ++w) {
      auto& e = l2tlb_e_[set * cfg_.l2tlb_ways + w];
      if (e.valid && e.vpn == vpn) { e.lru = ++lru_clock_; hit = true; break; }
    }
    if (hit) { l2tlb.hits++; pending_fill_.push_back({vpn, pr.core}); continue; }
    l2tlb.misses++;

    bool joined = false;
    for (auto& m : l2tlb_m_) {
      if (m.valid && m.vpn == vpn) {
        if (m.reqs >= cfg_.l2tlb_req_per_entry) {
          l2tlb.walk_stalls++;
          l2tlb.misses--;
          l2tlb.reads--;
          // M26-B1 / H4: the requeued probe does not add this core to the
          // MSHR's mask in this pass; if the mask never grows, this core is
          // never filled for this VPN. Counted, not changed.
          if (life_) {
            ++life_l2tlb_requeue;
            if ((m.core_mask & (1u << pr.core)) == 0) ++life_l2tlb_requeue_nocore;
            vpn_life(vpn, pr.core).push(LE_L2TLB_REQUEUE, cycle, m.reqs, m.core_mask);
          }
          keep.push_back(PipeEnt{cycle + cfg_.l2tlb_latency, vpn, pr.core});
        } else {
          m.reqs++;
          m.core_mask |= 1u << pr.core;
          l2tlb.dedups++;
        }
        joined = true;
        break;
      }
    }
    if (joined) continue;

    bool alloc = false;
    for (auto& m : l2tlb_m_) {
      if (!m.valid) {
        m.valid = true; m.vpn = vpn; m.reqs = 1; m.core_mask = 1u << pr.core; m.ready = NONE;
        l2tlb.allocs++;
        alloc = true;
        break;
      }
    }
    if (!alloc) {
      l2tlb.misses--;
      l2tlb.reads--;
      l2tlb.walk_stalls++;
      if (life_) {
        ++life_l2tlb_requeue;
        vpn_life(vpn, pr.core).push(LE_L2TLB_REQUEUE, cycle, 0, 0);
      }
      keep.push_back(PipeEnt{cycle + cfg_.l2tlb_latency, vpn, pr.core});
    }
  }
  l2tlb_pipe_ = keep;

  u64 occ = 0;
  for (const auto& m : l2tlb_m_) if (m.valid) occ++;
  l2tlb.occ += occ;
  l2tlb.max_occ = std::max(l2tlb.max_occ, occ);
  ++l2tlb.occ_hist[occ < 16 ? occ : 16];  // M23-G6D: distribution, diagnostic only
}

void MemorySystem::ptw_tick(u64 cycle) {
  // Issue walks for MSHRs that have no completion time yet.
  for (auto& m : l2tlb_m_) {
    if (!m.valid || m.ready != NONE) continue;
    bool in_wc = std::find(walk_cache_.begin(), walk_cache_.end(), m.vpn) != walk_cache_.end();
    u32 w = cfg_.walkers;
    for (u32 i = 0; i < cfg_.walkers; ++i) if (walk_busy_[i] <= cycle) { w = i; break; }
    if (w == cfg_.walkers) { ptw.walker_max = cfg_.walkers; break; }  // all walkers busy

    if (in_wc) {
      ptw.wc_hits++;
      m.ready = cycle + 1;  // walk-cache hit: no walk, no PTE traffic
    } else {
      // PTE fetches become real line traffic in the shared hierarchy.
      u64 lat = static_cast<u64>(cfg_.levels) * cfg_.walk_latency_per_level;
      for (u32 lvl = 0; lvl < cfg_.levels; ++lvl) {
        u64 ready = NONE;
        l2_probe(pte_line_of(lvl, m.vpn), cycle, ready);
        ptw.pte_fetches++;
      }
      ptw.walks++;
      ptw.walk_latency += lat;
      if (life_) {
        ++life_walk_alloc;
        vpn_life(m.vpn, 0).push(LE_WALK_ISSUE, cycle, lat, m.reqs);
      }
      m.ready = cycle + lat;
      walk_cache_.push_back(m.vpn);
      while (walk_cache_.size() > cfg_.walk_cache) walk_cache_.pop_front();
    }
    walk_busy_[w] = m.ready;
  }

  u64 busy = 0;
  for (u64 b : walk_busy_) if (b > cycle) busy++;
  ptw.walker_busy += busy;
  ptw.walker_max = std::max(ptw.walker_max, busy);

  // Retire completed walks into the L2 TLB.
  for (auto& m : l2tlb_m_) {
    if (!m.valid || m.ready == NONE || m.ready > cycle) continue;
    const u32 sets = std::max<u32>(1, cfg_.l2tlb_entries / std::max<u32>(1, cfg_.l2tlb_ways));
    const u32 set = static_cast<u32>(m.vpn % sets);
    u32 victim = set * cfg_.l2tlb_ways, lru = ~0u;
    bool placed = false;
    for (u32 w = 0; w < cfg_.l2tlb_ways; ++w) {
      auto& e = l2tlb_e_[set * cfg_.l2tlb_ways + w];
      if (!e.valid) { e.valid = true; e.vpn = m.vpn; e.ppn = pa_page_of_vpn(m.vpn); e.lru = ++lru_clock_; placed = true; break; }
      if (e.lru < lru) { lru = e.lru; victim = set * cfg_.l2tlb_ways + w; }
    }
    if (!placed) {
      auto& e = l2tlb_e_[victim];
      e.valid = true; e.vpn = m.vpn; e.ppn = pa_page_of_vpn(m.vpn); e.lru = ++lru_clock_;
      l2tlb.evict++;
    }
    if (life_) { ++life_walk_done; ++vpn_life(m.vpn, 0).walk_done; }
    for (u32 c = 0; c < cfg_.cores; ++c)
      if (m.core_mask & (1u << c)) {
        pending_fill_.push_back({m.vpn, c});
        // M26-B1: the *generation* of a fill, per core. H1 is exactly the
        // statement that this count is smaller than the number of parked
        // cores for the same VPN; the dump compares the two per VPN.
        if (life_) {
          ++life_fill_push;
          ++vpn_life(m.vpn, c).fills;
          vpn_life(m.vpn, c).push(LE_FILL_PUSH, cycle, m.vpn, c);
        }
      }
    if (life_) ++life_walk_release;
    m = L2Mshr{};
  }
}

// ---------------------------------------------------------------------------
// Data path
// ---------------------------------------------------------------------------
// The production address map, in one place (M23-G3). Lines interleave across
// banks; each bank row holds row_bytes/64 lines, so a linear stream reuses the
// open row. With bank_hash the bank index is XOR-folded, so a power-of-two
// stride (e.g. one line every 64 lines) cannot alias onto a single bank -- the
// same reason Vortex sets VX_CFG_L2_BANK_HASH = 1. Only the bank is hashed: the
// row still comes from the linear address, so a sequential sweep keeps its
// open-row reuse, and hashing the row as well would destroy exactly the
// locality the model is meant to measure. This is the *production* map; the
// diagnostic maps (Config::dram_map) are applied at the caller.
void MemorySystem::dram_bank_row(u64 line, u32& bank, u32& row) const {
  // M24-B: when the HBM hierarchy is selected, the one decode that decides
  // everything (controller, release rank, engine grouping) is the HBM decoder.
  if (hbm_) {
    unsigned b = 0, r = 0;
    hbm_->bank_row(line, b, r);
    bank = b;
    row = r;
    return;
  }
  // M23-G4C: `dram_il_lines` = il is the bank-interleave granularity. The
  // address space is laid out as [row][group][bank][line-in-unit], where a row
  // of `row_bytes` holds `row_bytes/64/il` groups of il lines per bank, so
  //   bank = hash(line / il) % banks
  //   row  = ((line / (il * banks)) / (lines_per_row / il)) % rows
  // and the space one row index wraps over is
  //   il * banks * rows * (lines_per_row / il) = banks * rows * row_bytes,
  // i.e. il changes the interleaving but never the capacity. With il = 1 both
  // expressions are the ones this function has always computed.
  const u32 lines_per_row = std::max<u32>(1, cfg_.row_bytes / LINE_BYTES);
  const u32 il = std::max<u32>(1, cfg_.dram_il_lines);
  u64 bidx = line / il;
  if (cfg_.bank_hash) bidx ^= (bidx >> 6) ^ (bidx >> 12);
  bank = static_cast<u32>(bidx % cfg_.banks);
  const u32 groups = std::max<u32>(1, lines_per_row / il);
  row = static_cast<u32>(((line / (il * cfg_.banks)) / groups) % cfg_.rows);
  // M24-A: the two bank-selection experiments. They replace *only* the bank
  // index: the row expression above, the row-index period (banks x rows x
  // row_bytes), the capacity, every cache and every timing parameter are
  // untouched, so they change which bank owns a line and nothing about how many
  // lines share a row.
  //   M1  bank = line % banks                  (adjacent lines across banks, no fold)
  //   M2  bank = (line ^ line>>5 ^ line>>11 ^ line>>17) % banks
  //       (bit 5 is the row bit, so the fold decorrelates bank from row parity)
  if (cfg_.m24_map == 1) {
    bank = static_cast<u32>(line % cfg_.banks);
  } else if (cfg_.m24_map == 2) {
    const u64 x = line ^ (line >> 5) ^ (line >> 11) ^ (line >> 17);
    bank = static_cast<u32>(x % cfg_.banks);
  }
}

int MemorySystem::l2_probe(u64 line, u64 cycle, u64& ready, u8 client) {
  const u32 sets = std::max<u32>(1, cfg_.l2_lines / std::max<u32>(1, cfg_.l2_ways));
  const u32 set = static_cast<u32>(line % sets);
  // M15: requester tag. Only the attribution counters below read it; the probe
  // decisions above and below are identical with the diagnostic off.
  const u8 cl = client < 4 ? client : 2;
  l2.reads++;
  if (diag_) ++cc[cl].l2_reads;
  for (u32 w = 0; w < cfg_.l2_ways; ++w) {
    auto& e = l2_e_[set * cfg_.l2_ways + w];
    if (e.valid && e.line == line) {
      e.lru = ++lru_clock_;
      l2.hits++;
      if (diag_) {
        ++cc[cl].l2_hits;
        if (e.last_client == cl) ++cc[cl].l2_hit_self;
        else ++cc[cl].l2_hit_cross;
        e.last_client = cl;
      }
      ready = cycle + cfg_.l2_latency;
      return 0;
    }
  }
  l2.misses++;
  if (diag_) ++cc[cl].l2_misses;
  for (auto& m : l2_m_) {
    if (m.valid && m.line == line) {
      l2.mshr_joins++;
      if (diag_) ++cc[cl].l2_joins;
      ready = m.ready;
      return 1;
    }
  }
  for (auto& m : l2_m_) {
    if (!m.valid) {
      if (cfg_.dram_q && dram_queue() >= cfg_.dram_q) {
        l2.misses--; l2.reads--; dram.q_full++; l2.stall_cycles++;
        if (diag_) { --cc[cl].l2_misses; --cc[cl].l2_reads; ++cc[cl].l2_stalls; }
        return 2;
      }
      m.valid = true; m.line = line; m.ready = NONE;
      if (diag_) m.client = cl;
      l2.mshr_allocs++;
      if (diag_) ++cc[cl].l2_allocs;
      // Address map: one definition, in dram_bank_row below, shared with the
      // Bulk engine's organization policy (M23-G3). The arithmetic is the one
      // this site always had; the shared function exists so a policy that
      // groups by (bank,row) cannot drift from the map the controller uses.
      u32 bank = 0, row = 0;
      dram_bank_row(line, bank, row);
      // M11.3 diagnostic address maps. dram_map = 0 is the production mapping
      // and skips this block entirely, so a normal run computes exactly the
      // bank/row it computed before the switch existed. The same map applies to
      // every requester in a run: SIMT, both engine streams and the walker.
      switch (cfg_.dram_map) {
        case 1: {  // decouple the 8 MiB region from the data footprint: address
                   // bit 23 (8 MiB) moves the line to the other bank and to the
                   // upper half of the row space.
          const u32 hi = static_cast<u32>((line >> 17) & 1ull);
          bank = (bank + hi) % cfg_.banks;
          if (hi && cfg_.rows > 1) row = (row + cfg_.rows / 2) % cfg_.rows;
          break;
        }
        case 2: bank = 0; break;                                    // one bank
        case 3: bank = static_cast<u32>(line % cfg_.banks); break;  // strict balance
        case 4: row = 0; break;                                     // one row per bank
        case 5: bank = 0; row = 0; break;                           // one bank, one row
        default: break;
      }
      if (hbm_) {
        // M24-B: the HBM controller decodes and queues the line itself; the
        // legacy queue stays empty, so every legacy-only counter is untouched.
        hbm_->enqueue(line, ++seq_, cycle, client);
        dram.max_q = std::max<u64>(dram.max_q, static_cast<u64>(hbm_->depth()));
      } else {
        dram_wait_.push_back(DQ{line, ++seq_, cycle, bank, row, client});
        dram.max_q = std::max<u64>(dram.max_q, dram_wait_.size());
      }
      // M23-G2C: per-cycle arrival census, at the enqueue point. Pure read of
      // the cycle the caller already passes in; no state the decision reads.
      if (cfg_.dram_diag) {
        ++dram.arrivals;
        if (dram.last_arrive_cycle != cycle) {
          dram.last_arrive_cycle = cycle;
          dram.arrive_this_cycle = 1;
          ++dram.arrive_cycles;
        } else if (++dram.arrive_this_cycle == 2) {
          ++dram.arrive_multi_cycles;
        }
      }
      ready = NONE;
      return 1;
    }
  }
  l2.misses--;
  l2.reads--;
  l2.stall_cycles++;
  if (diag_) { --cc[cl].l2_misses; --cc[cl].l2_reads; ++cc[cl].l2_stalls; }
  return 2;
}

int MemorySystem::l1_probe(u32 core, u64 line, u64 cycle, u64& ready, u8 client) {
  const u32 ebase = core * cfg_.l1_lines;
  const u32 mbase = core * cfg_.l1_mshr;
  const u32 sets = std::max<u32>(1, cfg_.l1_lines / std::max<u32>(1, cfg_.l1_ways));
  const u32 set = static_cast<u32>(line % sets);
  const u8 cl = client < 4 ? client : 2;
  l1.reads++;
  if (diag_) ++cc[cl].l1_reads;
  for (u32 w = 0; w < cfg_.l1_ways; ++w) {
    auto& e = l1_e_[ebase + set * cfg_.l1_ways + w];
    if (e.valid && e.line == line) {
      e.lru = ++lru_clock_;
      l1.hits++;
      if (diag_) { ++cc[cl].l1_hits; e.last_client = cl; }
      ready = cycle + cfg_.l1_latency;
      return 0;
    }
  }
  l1.misses++;
  if (diag_) ++cc[cl].l1_misses;
  for (u32 i = 0; i < cfg_.l1_mshr; ++i) {
    auto& m = l1_m_[mbase + i];
    if (m.valid && m.line == line) {
      l1.mshr_joins++;
      if (diag_) ++cc[cl].l1_joins;
      ready = m.ready;
      return 1;
    }
  }
  for (u32 i = 0; i < cfg_.l1_mshr; ++i) {
    auto& m = l1_m_[mbase + i];
    if (!m.valid) {
      u64 l2ready = NONE;
      int rc = l2_probe(line, cycle, l2ready, client);
      if (rc == 2) {
        l1.misses--; l1.reads--; l1.stall_cycles++;
        if (diag_) { --cc[cl].l1_misses; --cc[cl].l1_reads; ++cc[cl].l1_stalls; }
        return 2;
      }
      m.valid = true; m.line = line; m.ready = l2ready;
      if (diag_) m.client = cl;
      l1.mshr_allocs++;
      if (diag_) ++cc[cl].l1_allocs;
      ready = m.ready;  // may be NONE: resolved when the fill lands
      return 1;
    }
  }
  l1.misses--;
  l1.reads--;
  l1.stall_cycles++;
  if (diag_) { --cc[cl].l1_misses; --cc[cl].l1_reads; ++cc[cl].l1_stalls; }
  return 2;
}

void MemorySystem::cache_tick(u64 cycle) {
  for (u32 c = 0; c < cfg_.cores; ++c) {
    const u32 ebase = c * cfg_.l1_lines;
    const u32 mbase = c * cfg_.l1_mshr;
    const u32 sets = std::max<u32>(1, cfg_.l1_lines / std::max<u32>(1, cfg_.l1_ways));
    for (u32 i = 0; i < cfg_.l1_mshr; ++i) {
      auto& m = l1_m_[mbase + i];
      if (!m.valid || m.ready == NONE || m.ready > cycle) continue;
      // Hand the completion to every request waiting on this line before the
      // MSHR is recycled; polling the array afterwards could miss it.
      for (auto& r : inflight_)
        if (r.stage == 2 && r.core == c && r.pa_line == m.line && r.ready == NONE) r.ready = m.ready;
      const u32 set = static_cast<u32>(m.line % sets);
      u32 victim = ebase + set * cfg_.l1_ways, lru = ~0u; bool placed = false;
      const u8 who = m.client;
      for (u32 w = 0; w < cfg_.l1_ways; ++w) {
        auto& e = l1_e_[ebase + set * cfg_.l1_ways + w];
        if (!e.valid) { e.valid = true; e.line = m.line; e.lru = ++lru_clock_; placed = true; if (diag_) e.last_client = who; break; }
        if (e.lru < lru) { lru = e.lru; victim = ebase + set * cfg_.l1_ways + w; }
      }
      if (!placed) {
        auto& e = l1_e_[victim];
        e.valid = true; e.line = m.line; e.lru = ++lru_clock_; l1.evict++;
        if (diag_) { e.last_client = who; if (who < 4) ++cc[who].l1_evict; }
      }
      m = CMshr{};
    }
  }
  for (auto& m : l2_m_) {
    if (!m.valid || m.ready == NONE || m.ready > cycle) continue;
    // A client that bypasses the L1 under the configured path waits on the L2
    // MSHR directly.
    for (auto& r : inflight_)
      if (r.stage == 2 && path_bypasses_l1(r.bypass_l1) && r.pa_line == m.line &&
          r.ready == NONE)
        r.ready = m.ready;
    const u32 sets = std::max<u32>(1, cfg_.l2_lines / std::max<u32>(1, cfg_.l2_ways));
    const u32 set = static_cast<u32>(m.line % sets);
    u32 victim = set * cfg_.l2_ways, lru = ~0u; bool placed = false;
    const u8 who = m.client;
    for (u32 w = 0; w < cfg_.l2_ways; ++w) {
      auto& e = l2_e_[set * cfg_.l2_ways + w];
      if (!e.valid) { e.valid = true; e.line = m.line; e.lru = ++lru_clock_; placed = true; if (diag_) e.last_client = who; break; }
      if (e.lru < lru) { lru = e.lru; victim = set * cfg_.l2_ways + w; }
    }
    if (!placed) {
      auto& e = l2_e_[victim];
      // M15: the victim's identity is read before it is replaced, so a fill can
      // be charged with the line it destroyed. Diagnostic only.
      const u8 lost = diag_ ? e.last_client : 0xff;
      e.valid = true; e.line = m.line; e.lru = ++lru_clock_; l2.evict++;
      if (diag_ && who < 4) {
        e.last_client = who;
        ++cc[who].l2_evict;
        if (lost == who) ++cc[who].l2_evict_self;
        else if (lost != 0xff) ++cc[who].l2_evict_cross;
      }
    }
    m = CMshr{};
  }
  u64 o1 = 0, o2 = 0;
  for (const auto& m : l1_m_) if (m.valid) o1++;
  for (const auto& m : l2_m_) if (m.valid) o2++;
  l1.occ += o1; l1.max_occ = std::max(l1.max_occ, o1);
  l2.occ += o2; l2.max_occ = std::max(l2.max_occ, o2);
}

void MemorySystem::dram_tick(u64 cycle) {
  // M24-B: one branch, then the HBM controller owns the decision.
  if (hbm_) { hbm_tick_(cycle); return; }
  // Diagnostic (read-only): queue depth at this decision, and the number of
  // candidates the decision could choose between. Neither value is read by the
  // arbitration below.
  dram.q_occ_sum += dram_wait_.size();
  dram.elig_last = 0;
  // M11.2 diagnostic: nothing written in this block is read by the decision
  // below, so the arbitration is unchanged whether the flag is on or off.
  const bool diag = cfg_.dram_diag;
  if (diag) {
    dram.q_occ_hist[log2_bucket(dram_wait_.size())]++;
    u32 per[4] = {0, 0, 0, 0};
    u32 pc[4] = {0, 0, 0, 0};
    for (const auto& q : dram_wait_) {
      if (q.bank < 4) ++per[q.bank];
      if (q.client < 4) ++pc[q.client];  // M11.3: occupancy by requester
    }
    for (u32 b = 0; b < 4 && b < cfg_.banks; ++b) {
      dram.bank_q_sum[b] += per[b];
      if (per[b] > dram.bank_q_max[b]) dram.bank_q_max[b] = per[b];
    }
    for (u32 c = 0; c < 4; ++c) {
      dram.client_q_sum[c] += pc[c];
      if (pc[c] > dram.client_q_max[c]) dram.client_q_max[c] = pc[c];
    }
    // M23-G6E: how much *diversity* the arbitration can choose between right
    // now, not merely how many requests are queued. A generation stamp keeps
    // this O(queue) with no per-cycle clearing.
    if (!row_stamp_.empty()) {
      ++stamp_;
      u32 distinct_rows = 0, bmask = 0;
      for (const auto& q : dram_wait_) {
        if (q.bank >= cfg_.banks || q.row >= cfg_.rows) continue;
        bmask |= 1u << q.bank;
        const size_t idx = static_cast<size_t>(q.bank) * cfg_.rows + q.row;
        if (row_stamp_[idx] != stamp_) {
          row_stamp_[idx] = stamp_;
          ++distinct_rows;
        }
      }
      u32 distinct_banks = 0;
      for (u32 b = 0; b < 4; ++b) if (bmask & (1u << b)) ++distinct_banks;
      dram.vis_rows_sum += distinct_rows;
      dram.vis_banks_sum += distinct_banks;
      if (distinct_rows > dram.vis_rows_max) dram.vis_rows_max = distinct_rows;
      dram.vis_rows_hist[log2_bucket(distinct_rows)]++;
    }
  }
  if (dram_wait_.empty()) { dram.starved_cycles++; return; }
  if (diag) ++dram.diag_decisions;

  // One command per cycle; FR-FCFS: open row first, then oldest arrival.
  size_t best = dram_wait_.size();
  bool best_open = false;
  u64 eligible = 0;
  u64 diag_hit = 0, diag_busy = 0, diag_hit_elig = 0;
  for (size_t i = 0; i < dram_wait_.size(); ++i) {
    const auto& q = dram_wait_[i];
    // The same predicate the decision uses, hoisted only so the diagnostic can
    // count it before the bank-readiness filter. Pure read, no side effect.
    const bool is_open = bank_open_[q.bank] && bank_row_[q.bank] == q.row;
    if (diag && is_open) ++diag_hit;
    if (bank_ready_[q.bank] > cycle) {
      if (diag && is_open) ++diag_busy;  // a row hit the bank cannot take yet
      continue;
    }
    ++eligible;
    if (diag && is_open) ++diag_hit_elig;
    if (best == dram_wait_.size()) { best = i; best_open = is_open; continue; }
    if (cfg_.frfcfs && is_open && !best_open) { best = i; best_open = is_open; continue; }
    if (is_open == best_open && q.arrive < dram_wait_[best].arrive) { best = i; best_open = is_open; }
  }
  if (diag) {
    dram.hit_cand_sum += diag_hit;
    dram.hit_busy_sum += diag_busy;
    dram.hit_elig_sum += diag_hit_elig;
    ++dram.hit_cand_hist[diag_hit < 16 ? diag_hit : 16];
  }
  if (best == dram_wait_.size()) {
    // M23-G2C guard: a non-empty queue in which no candidate's bank is free.
    // The counter records the complement - eligible candidates present yet
    // nothing issued - which the one-command-per-cycle rule makes structurally
    // impossible; it should read 0 in every cell and would catch a regression
    // in the arbitration. Written after the decision, read by nothing.
    if (diag && eligible > 0) ++dram.elig_wait_cycles;
    dram.blocked_cycles++;
    return;
  }

  DQ q = dram_wait_[best];
  dram_wait_.erase(dram_wait_.begin() + static_cast<long>(best));
  const bool open = bank_open_[q.bank] && bank_row_[q.bank] == q.row;
  dram.bank_mask |= 1ull << q.bank;
  if (open) dram.row_hits++;
  else if (bank_open_[q.bank]) dram.row_conflicts++;
  const bool was_open = bank_open_[q.bank];  // per-bank diagnostic only
  if (!open) { bank_open_[q.bank] = true; bank_row_[q.bank] = q.row; }
  dram.bytes += LINE_BYTES;
  const u64 lat = (open ? cfg_.t_hit : cfg_.t_miss) + cfg_.link;
  bank_ready_[q.bank] = cycle + lat;
  dram.reads++;
  dram.busy_cycles += lat;
  dram.svc_events++;
  dram.elig_sum += eligible;
  dram.elig_last = eligible;
  if (eligible > dram.elig_max) dram.elig_max = eligible;
  // ---- M11.2 diagnostic: written after the decision, read by no decision ---
  if (diag) {
    if (open) {
      dram.svc_hit_cycles += lat;
    } else {
      dram.svc_miss_cycles += lat;
      if (diag_hit_elig > 0) ++dram.missed_hit;      // (A) a hit was selectable
      else if (diag_busy > 0) ++dram.stall_case_b;   // (B) hits bank-blocked
      else ++dram.stall_case_c;                      // (C) no hit queued
    }
    const u64 w = cycle - q.arrive;
    dram.wait_sum += w;
    if (w > dram.wait_max) dram.wait_max = w;
    dram.wait_hist[log2_bucket(w)]++;
    if (q.bank < 4) {
      ++dram.bank_reads[q.bank];
      dram.bank_busy[q.bank] += lat;
      if (open) ++dram.bank_hits[q.bank];
      else if (was_open) ++dram.bank_conf[q.bank];
    }
    if (q.client < 4) {
      ++dram.client_reads[q.client];
      if (open) ++dram.client_hits[q.client];
      dram.client_wait[q.client] += w;
      if (!open) {
        ++dram.client_conf[q.client];
        // Cross-requester interference: this activation interrupted a bank that
        // a different requester used last. A cold bank has no owner and is not
        // counted.
        if (q.bank < 4 && bank_last_client_[q.bank] < 4 &&
            bank_last_client_[q.bank] != q.client)
          ++dram.client_interf[q.client];
      }
    }
    if (q.bank < 4) bank_last_client_[q.bank] = q.client;
    if (!open && !row_acts_.empty()) {
      const size_t ri = static_cast<size_t>(q.bank) * cfg_.rows + q.row;
      if (ri < row_acts_.size()) {
        if (row_acts_[ri] == 0) ++dram.rows_used;
        ++row_acts_[ri];
        ++dram.row_acts_sum;
        if (row_acts_[ri] > dram.row_acts_max) dram.row_acts_max = row_acts_[ri];
      }
    }
  }

  for (auto& m : l2_m_) {
    if (m.valid && m.line == q.line) {
      m.ready = cycle + lat + cfg_.l2_latency;
      // Propagate to any L1 MSHR already waiting on this line.
      for (auto& n : l1_m_)
        if (n.valid && n.line == q.line && n.ready == NONE) n.ready = m.ready + cfg_.l1_latency;
      break;
    }
  }
}

MemorySystem::~MemorySystem() = default;

size_t MemorySystem::dram_queue() const {
  return hbm_ ? hbm_->depth() : dram_wait_.size();
}
u64 MemorySystem::dram_elig_now() const {
  return hbm_ ? hbm_->elig_last() : dram.elig_last;
}

bool MemorySystem::dram_row_open(u64 line, u32& bank, u32& row) const {
  if (hbm_) {
    unsigned b = 0, r = 0;
    hbm_->bank_row(line, b, r);
    bank = b;
    row = r;
    return hbm_->row_open(line);
  }
  dram_bank_row(line, bank, row);
  return bank < bank_open_.size() && bank_open_[bank] && bank_row_[bank] == row;
}

// One HBM cycle: the controller issues up to one command per pseudochannel and
// returns the lines whose data has become ready. The completion path is the
// legacy one, so a request's lifetime is measured the same way in both models.
void MemorySystem::hbm_tick_(u64 cycle) {
  hbm_out_.clear();
  if (hbm_->depth() == 0) ++dram.starved_cycles;
  hbm_->tick(cycle, hbm_out_);
  if (hbm_out_.empty() && hbm_->depth() != 0) ++dram.blocked_cycles;
  for (const auto& s : hbm_out_) {
    for (auto& m : l2_m_) {
      if (m.valid && m.line == s.line) {
        m.ready = cycle + s.lat + cfg_.l2_latency;
        for (auto& n : l1_m_)
          if (n.valid && n.line == s.line && n.ready == NONE) n.ready = m.ready + cfg_.l1_latency;
        break;
      }
    }
  }
  const HbmStats& h = hbm_->stats();
  dram.reads = h.reads;
  dram.row_hits = h.hits;
  dram.row_conflicts = h.conf;
  dram.bytes = h.bytes;
  dram.wait_sum = h.wait_sum;
  dram.wait_max = h.wait_max;
  dram.max_q = h.q_max;
  dram.q_occ_sum = h.q_occ_sum;
  dram.svc_events = h.svc_events;
  dram.elig_sum = h.elig_sum;
  dram.elig_max = h.elig_max;
}

void MemorySystem::dump_hbm(Stats& s) const {
  if (!hbm_) return;
  const HbmStats& h = hbm_->stats();
  s.set("hbm_reads", h.reads);
  s.set("hbm_hits", h.hits);
  s.set("hbm_conflicts", h.conf);
  s.set("hbm_activations", h.act);
  s.set("hbm_precharges", h.pre);
  s.set("hbm_refresh", h.refresh);
  s.set("hbm_bytes", h.bytes);
  s.set("hbm_wait_sum", h.wait_sum);
  s.set("hbm_wait_max", h.wait_max);
  s.set("hbm_svc_events", h.svc_events);
  s.set("hbm_elig_sum", h.elig_sum);
  s.set("hbm_elig_max", h.elig_max);
  s.set("hbm_q_occ_sum", h.q_occ_sum);
  s.set("hbm_q_max", h.q_max);
  s.set("hbm_rows_used", h.rows_used);
  s.set("hbm_row_acts_sum", h.row_acts_sum);
  s.set("hbm_row_acts_max", h.row_acts_max);
  const unsigned nch = hbm_->nchan(), nb = hbm_->banks_per_ch();
  u64 used = 0, busy = 0, bmax = 0, rmin = ~0ull, rmax = 0, banks = 0;
  for (unsigned c = 0; c < nch; ++c) {
    if (h.ch_reads[c]) ++used;
    busy += h.ch_busy[c];
    if (h.ch_busy[c] > bmax) bmax = h.ch_busy[c];
    if (h.ch_reads[c] < rmin) rmin = h.ch_reads[c];
    if (h.ch_reads[c] > rmax) rmax = h.ch_reads[c];
    for (unsigned b = 0; b < nb; ++b)
      if (h.bank_acts[c * nb + b]) ++banks;
  }
  s.set("hbm_ch_used", used);
  s.set("hbm_ch_busy_sum", busy);
  s.set("hbm_ch_busy_max", bmax);
  s.set("hbm_ch_reads_min", used ? rmin : 0);
  s.set("hbm_ch_reads_max", rmax);
  s.set("hbm_banks_used", banks);
  for (u32 k = 0; k < 16; ++k) s.set("hbm_lat" + std::to_string(k), h.lat_hist[k]);
}

// ---------------------------------------------------------------------------
// Front-end interface
// ---------------------------------------------------------------------------
bool MemorySystem::submit(Req& req, u64 cycle) {
  // One attempt to enter the system. False means "try again later": the caller
  // keeps the request, which is exactly the unissued state R bounds.
  u64 ready = NONE;
  if (req.pretranslated) {
    // M23-G6A phase 2: the address was translated in phase 1 and has been held
    // in the engine's PA buffer since. The request enters at the hierarchy
    // directly -- no dTLB probe, no MSHR, no L2 TLB lookup and no walk -- so the
    // translation it already paid for is never paid twice. `gen`/`tlb_at` are
    // left as the caller set them: the lifetime still begins at the phase-1
    // submission, so the latency split stays comparable across policies.
    req.stage = 2;
    req.ready = cycle;
    last_progress = cycle;
    if (life_) {
      req.id = ++life_id_;
      ReqLife& L = life_rec_[req.id];
      L.id = req.id; L.vpn = req.vpn; L.core = req.core; L.gen = cycle;
      ++life_enter;
      life_ev(req.id, LE_ENTER, cycle, req.va_line, req.stage);
    }
    inflight_.push_back(req);
    if (diag_) {
      const u8 cl = req.client < 4 ? req.client : 2;
      ++cc[cl].issue;
      if (cc[cl].first_issue == NONE) cc[cl].first_issue = cycle;
      cc[cl].last_issue = cycle;
    }
    return true;
  }
  u64 ppn = NONE;
  if (!tlb_probe(req.core, req.vpn, cycle, ready, &ppn)) {
    req.ppn_ok = false;  // a retried submission must not carry an old result
    return false;
  }
  req.stage = 1;
  req.ready = ready;
  req.gen = cycle;
  req.ppn = ppn;
  req.ppn_ok = (ppn != NONE);
  last_progress = cycle;
  if (life_) {
    req.id = ++life_id_;
    ReqLife& L = life_rec_[req.id];
    L.id = req.id; L.vpn = req.vpn; L.core = req.core; L.gen = cycle;
    ++life_enter;
    life_ev(req.id, LE_ENTER, cycle, req.va_line, req.stage);
    life_ev(req.id, life_probe_, cycle, ready, req.vpn);
  }
  inflight_.push_back(req);
  if (diag_) {
    const u8 cl = req.client < 4 ? req.client : 2;
    ++cc[cl].issue;
    if (cc[cl].first_issue == NONE) cc[cl].first_issue = cycle;
    cc[cl].last_issue = cycle;
  }
  return true;
}

void MemorySystem::tick(u64 cycle, std::vector<u64>& done) {
  cycles = cycle;

  // Walkers may start walks; DRAM moves one line per cycle; caches install
  // anything whose latency has elapsed.
  l2tlb_tick(cycle);
  ptw_tick(cycle);
  dram_tick(cycle);
  cache_tick(cycle);

  for (auto& f : pending_fill_) {
    tlb_fill(f.second, f.first);
    const u32 mbase = f.second * cfg_.dtlb_mshr;
    for (u32 i = 0; i < cfg_.dtlb_mshr; ++i)
      if (tlb_m_[mbase + i].valid && tlb_m_[mbase + i].vpn == f.first) tlb_m_[mbase + i] = TlbMshr{};
  }
  pending_fill_.clear();

  for (u32 core = 0; core < cfg_.cores; ++core) {
    u64 occ = 0;
    const u32 mbase = core * cfg_.dtlb_mshr;
    for (u32 i = 0; i < cfg_.dtlb_mshr; ++i) if (tlb_m_[mbase + i].valid) occ++;
    tlb[core].mshr_occ += occ;
    tlb[core].mshr_max = std::max(tlb[core].mshr_max, occ);
    ++tlb[core].mshr_hist[occ < 16 ? occ : 16];  // M23-G6D: distribution
  }

  std::vector<Req> keep;
  keep.reserve(inflight_.size());
  for (auto& r : inflight_) {
    // Stage 1: wait for the translation fill, then translate.
    if (r.stage == 1) {
      if (r.ready != NONE && cycle < r.ready) {
        if (life_) { ++life_s1_wait; life_ev(r.id, LE_S1_WAIT, cycle, r.ready, r.vpn); }
        keep.push_back(r);
        continue;
      }
      const u32 sets = std::max<u32>(1, cfg_.dtlb_entries / std::max<u32>(1, cfg_.dtlb_ways));
      const u32 base = r.core * cfg_.dtlb_entries +
                       static_cast<u32>(r.vpn % sets) * cfg_.dtlb_ways;
      u64 ppn = 0;
      bool found = false;
      if (r.ppn_ok) {
        // M27-P1: a hit at admission gave this request its translation; use it.
        // Eviction of the array entry since then cannot invalidate it.
        ppn = r.ppn;
        r.ppn_ok = false;
        found = true;
        // Keep the entry's LRU age honest for the clients that still use it.
        for (u32 w = 0; w < cfg_.dtlb_ways; ++w) {
          auto& e = tlb_e_[base + w];
          if (e.valid && e.vpn == r.vpn) { e.lru = ++lru_clock_; break; }
        }
      } else if (cfg_.ideal_translation) {
        // M23-F1: no array lookup; the mapping is the function tlb_fill uses.
        ppn = pa_page_of_vpn(r.vpn);
        found = true;
      } else {
        // Miss path: the request parked a waiter on an MSHR, so this lookup is
        // the observation of a fill the model has promised to deliver.
        for (u32 w = 0; w < cfg_.dtlb_ways; ++w) {
          auto& e = tlb_e_[base + w];
          if (e.valid && e.vpn == r.vpn) { ppn = e.ppn; e.lru = ++lru_clock_; found = true; break; }
        }
      }
      if (!found) {
        if (life_) {
          ++life_s1_miss;
          life_ev(r.id, LE_S1_MISS, cycle, r.ready == NONE ? NONE : r.ready, r.vpn);
          // M26-B1: "stale" is the cycle after which this request's expected
          // fill should have completed and its dTLB entry is still absent.
          // Nothing about the run changes: this only tags the record and starts
          // the complete post-onset event log for it.
          ReqLife* L = life_at(r.id);
          if (L && L->stale_at == NONE && (r.ready == NONE || cycle >= r.ready)) {
            ++life_s1_stale;
            life_ev(r.id, LE_S1_STALE, cycle, r.gen, r.vpn);
          }
        }
        keep.push_back(r);
        continue;
      }
      if (life_) {
        ++life_s1_hit;
        life_ev(r.id, LE_S1_HIT, cycle, ppn, r.vpn);
        ++vpn_life(r.vpn, r.core).seen_by_waiter;
      }
      r.pa_line = pa_line_of(ppn, r.va_line, cfg_.page_line_bits());
      r.tlb_at = cycle;
      r.ready = NONE;
      r.stage = 2;
      // M23-G6A phase 1: the translation *is* the request. It retires here,
      // before any cache probe, so nothing it asked for can reach the DRAM; the
      // physical line is handed back to the engine's PA buffer instead.
      if (r.xlate_only) {
        if (xlate_done_.size() - xlate_head_ < 8192) {
          xlate_done_.push_back(Xlate{r.seq, r.pa_line, cycle});
          if (life_) { ++life_xl_enq; life_ev(r.id, LE_XL_ENQ, cycle, xlate_done_.size(), r.seq); }
        } else {
          ++xlate_overflow;
          // H3: a completed translation that never reaches the engine. The
          // request still retires, so this is visible only as
          // tr_requests > tr_completed plus this counter.
          if (life_) { ++life_xl_rej; life_ev(r.id, LE_XL_REJ, cycle, xlate_done_.size(), r.seq); }
        }
        if (life_) {
          life_ev(r.id, LE_RETIRE, cycle, 3, r.vpn);
          ++life_leave;
          life_last_retire = cycle;
          life_rec_.erase(r.id);
        }
        last_progress = cycle;
        // M27-P3: a phase-1 probe owns a residency slot from M27 on, so its
        // retirement is announced on the same list as every other release.
        if (r.slot != 0xffffffffu) done.push_back(static_cast<u64>(r.core) << 32 | r.slot);
        continue;
      }
    }

    // Stage 2: cache hierarchy. Whether the L1 is on this request's path is
    // decided by the configured memory path, not by which front-end issued it
    // (M5.1 §4); everything below the L2 is unchanged either way.
    if (r.stage == 2) {
      const bool bypass = path_bypasses_l1(r.bypass_l1);
      if (!r.probed) {
        u64 ready = NONE;
        int rc = bypass ? l2_probe(r.pa_line, cycle, ready, r.client)
                        : l1_probe(r.core, r.pa_line, cycle, ready, r.client);
        if (rc == 2) { replays++; keep.push_back(r); continue; }
        if (life_) life_ev(r.id, rc == 1 ? LE_S2_JOIN : LE_S2_PROBE, cycle, rc, r.pa_line);
        r.probed = true;
        r.ready = ready;
      } else if (r.ready == NONE) {
        if (bypass) {
          for (const auto& m : l2_m_)
            if (m.valid && m.line == r.pa_line && m.ready != NONE) { r.ready = m.ready; break; }
        } else {
          for (const auto& m : l1_m_)
            if (m.valid && m.line == r.pa_line && m.ready != NONE) { r.ready = m.ready; break; }
        }
      }
      if (r.ready != NONE && cycle >= r.ready) {
        const u64 lat = cycle - r.gen;
        lat_sum += lat;
        tlb_lat_sum += r.tlb_at - r.gen;
        cache_lat_sum += cycle - r.tlb_at;
        lat_all.push_back(static_cast<u32>(lat));
        if (diag_) {
          const u8 cl = r.client < 4 ? r.client : 2;
          ++cc[cl].done;
          cc[cl].lat_sum += lat;
          if (cc[cl].first_done == NONE) cc[cl].first_done = cycle;
          cc[cl].last_done = cycle;
        }
        if (life_) {
          life_ev(r.id, LE_RETIRE, cycle, 4, r.vpn);
          ++life_leave;
          life_last_retire = cycle;
          life_rec_.erase(r.id);
        }
        last_progress = cycle;
        done.push_back(static_cast<u64>(r.core) << 32 | r.slot);
        continue;
      }
      keep.push_back(r);
      continue;
    }
    keep.push_back(r);
  }
  inflight_.swap(keep);
  inflight_occ += inflight_.size();
  if (inflight_.size() > inflight_max) inflight_max = inflight_.size();
  if (life_) {
    const u64 xo = xlate_done_.size() - xlate_head_;
    if (xo > life_xl_occ_max) life_xl_occ_max = xo;
    // Per-core parked waiters: the dTLB MSHR file's occupant count for each
    // core, summed across every valid entry (`parked`, not entry count).
    for (u32 c = 0; c < cfg_.cores && c < 8; ++c) {
      u32 held = 0;
      for (u32 i = 0; i < cfg_.dtlb_mshr; ++i) {
        const auto& m = tlb_m_[static_cast<size_t>(c) * cfg_.dtlb_mshr + i];
        if (m.valid) held += m.parked;
      }
      life_parked_core_[c] = held;
      life_parked_sum += held;
      if (held > life_parked_max) life_parked_max = held;
    }
  }
}

void MemorySystem::finish(u64 cycle) {
  cycles = cycle;
  (void)inflight_;
}

// ---------------------------------------------------------------------------
// M26-B1: request-lifecycle diagnostics
// ---------------------------------------------------------------------------
const char* life_ev_name(u16 k) {
  static const char* const n[] = {
      "enter",       "probe_hit", "probe_join", "probe_alloc", "probe_refused",
      "walk_issue",  "walk_done", "fill_push",  "fill_install", "fill_evict",
      "s1_wait",     "s1_miss",   "s1_stale",   "s1_hit",       "xl_enq",
      "xl_rej",      "xl_deq",    "s2_probe",   "s2_join",      "retire",
      "l2tlb_requeue"};
  return k < LE_N ? n[k] : "unknown";
}

ReqLife* MemorySystem::life_at(u64 id) {
  auto it = life_rec_.find(id);
  return it == life_rec_.end() ? nullptr : &it->second;
}

VpnLife& MemorySystem::vpn_life(u64 vpn, u32 core) {
  return vpn_life_[(vpn << 3) | (core & 7u)];
}

void MemorySystem::life_ev(u64 id, u16 kind, u64 cycle, u64 a, u64 b) {
  if (!life_) return;
  auto it = life_rec_.find(id);
  if (it == life_rec_.end()) return;
  ReqLife& L = it->second;
  // The onset itself is part of the post-onset history, so enable the log
  // before the transition is appended, not after.
  if (kind == LE_S1_STALE && L.stale_at == NONE) {
    L.stale_at = cycle;
    L.log_state = true;
  }
  L.push(kind, cycle, a, b);
}

// M27-P2: what is still outstanding, and where each request is stuck.
std::string MemorySystem::outstanding_report() const {
  std::string out = std::to_string(inflight_.size()) + " request(s) outstanding";
  size_t shown = 0;
  for (const auto& r : inflight_) {
    if (shown++ == 8) { out += ", ..."; break; }
    out += " [id=" + std::to_string(r.id) + " vpn=" + std::to_string(r.vpn) +
           " stage=" + std::to_string(r.stage) + " core=" + std::to_string(r.core) +
           " client=" + std::to_string(r.client) +
           " ready=" + (r.ready == NONE ? std::string("none") : std::to_string(r.ready)) +
           " probed=" + std::to_string(r.probed ? 1 : 0) + "]";
  }
  if (!pending_fill_.empty()) out += " pending_fills=" + std::to_string(pending_fill_.size());
  return out;
}

void MemorySystem::dump_life(Stats& s) const {
  if (!life_) return;
  u64 stale = 0, records = life_rec_.size();
  for (const auto& kv : life_rec_)
    if (kv.second.stale_at != NONE) ++stale;

  s.set("life_diag", 1);
  s.set("life_enter", life_enter);
  s.set("life_leave", life_leave);
  s.set("life_inflight_end", inflight_.size());
  s.set("life_records_end", records);
  s.set("life_stale_end", stale);
  s.set("life_last_retire", life_last_retire);
  s.set("life_s1_wait_cycles", life_s1_wait);
  s.set("life_s1_miss_cycles", life_s1_miss);
  s.set("life_s1_stale_events", life_s1_stale);
  s.set("life_s1_hit_events", life_s1_hit);
  s.set("life_reprobe", life_reprobe);
  s.set("life_xl_enq", life_xl_enq);
  s.set("life_xl_rej", life_xl_rej);
  s.set("life_xl_deq", life_xl_deq);
  s.set("life_xl_occ_max", life_xl_occ_max);
  s.set("life_xl_occ_end", xlate_done_.size() - xlate_head_);
  s.set("life_xlate_overflow", xlate_overflow);
  s.set("life_fill_push", life_fill_push);
  s.set("life_fill_install", life_fill_install);
  s.set("life_fill_evict", life_fill_evict);
  s.set("life_walk_alloc", life_walk_alloc);
  s.set("life_walk_done", life_walk_done);
  s.set("life_walk_release", life_walk_release);
  s.set("life_l2tlb_requeue", life_l2tlb_requeue);
  s.set("life_l2tlb_requeue_nocore", life_l2tlb_requeue_nocore);
  s.set("life_parked_sum", life_parked_sum);
  s.set("life_parked_max", life_parked_max);
  { u64 m = 0; for (u32 i = 0; i < 8; ++i) m += life_parked_core_[i];
    s.set("life_parked_last", m); }

  // ---- detailed report ---------------------------------------------------
  // Only when a path was named: a unit test turns the counters on (they are
  // cheap and go into the Stats map) but must not write a report.
  if (life_dump_.empty()) return;
  std::FILE* f = std::fopen(life_dump_.c_str(), "a");
  if (!f) return;
  std::fprintf(f, "# M26-B1 lifecycle dump cycles=%llu inflight=%zu records=%llu stale=%llu "
                  "enter=%llu leave=%llu last_retire=%llu\n",
               static_cast<unsigned long long>(cycles), inflight_.size(),
               static_cast<unsigned long long>(records),
               static_cast<unsigned long long>(stale),
               static_cast<unsigned long long>(life_enter),
               static_cast<unsigned long long>(life_leave),
               static_cast<unsigned long long>(life_last_retire));

  // Outstanding requests, ordered by id so the report is deterministic.
  std::vector<u64> ids;
  ids.reserve(life_rec_.size());
  for (const auto& kv : life_rec_) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end());
  u32 shown = 0;
  for (u64 id : ids) {
    const ReqLife& L = life_rec_.at(id);
    const Req* rq = nullptr;
    for (const auto& r : inflight_) if (r.id == id) { rq = &r; break; }
    std::fprintf(f, "REQ id=%llu vpn=%llu core=%u gen=%llu stage=%u ready=%lld client=%u "
                    "seq=%llu stale_at=%lld last=%s last_cycle=%llu\n",
                 static_cast<unsigned long long>(L.id),
                 static_cast<unsigned long long>(L.vpn), L.core,
                 static_cast<unsigned long long>(L.gen),
                 rq ? rq->stage : 99u,
                 rq ? static_cast<long long>(rq->ready) : -1ll,
                 rq ? rq->client : 0u, static_cast<unsigned long long>(rq ? rq->seq : 0),
                 L.stale_at == NONE ? -1ll : static_cast<long long>(L.stale_at),
                 life_ev_name(L.last), static_cast<unsigned long long>(L.last_cycle));
    if (L.stale_at != NONE && shown < 64) {
      ++shown;
      std::fprintf(f, "  counts:");
      for (u16 k = 0; k < LE_N; ++k)
        if (L.count[k]) std::fprintf(f, " %s=%llu", life_ev_name(k),
                                     static_cast<unsigned long long>(L.count[k]));
      std::fprintf(f, "\n  ring:");
      for (u8 i = 0; i < L.nev; ++i) {
        const LifeEvRec& e = L.ev[(L.head + 8 - L.nev + i) & 7u];
        std::fprintf(f, " %llu:%s(%llu,%llu)", static_cast<unsigned long long>(e.cycle),
                     life_ev_name(e.kind), static_cast<unsigned long long>(e.a),
                     static_cast<unsigned long long>(e.b));
      }
      std::fprintf(f, "\n");
      // Every state-changing transition of this request after it went stale.
      std::fprintf(f, "  hist n=%u dropped=%u\n", L.hist_n, L.hist_drop);
      for (u8 i = 0; i < L.hist_n; ++i) {
        const LifeEvRec& e = L.hist[i];
        std::fprintf(f, "   %llu %s a=%llu b=%llu\n",
                     static_cast<unsigned long long>(e.cycle), life_ev_name(e.kind),
                     static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b));
      }
      // The page's own history: this is what separates H1 from H2.
      const u64 key = (L.vpn << 3) | (L.core & 7u);
      auto vt = vpn_life_.find(key);
      if (vt != vpn_life_.end()) {
        const VpnLife& v = vt->second;
        std::fprintf(f, "  vpn probes=%u joins=%u allocs=%u walk_done=%u fills=%u "
                        "installs=%u seen=%u last=%llu\n",
                     v.probes, v.joins, v.allocs, v.walk_done, v.fills, v.installs,
                     v.seen_by_waiter, static_cast<unsigned long long>(v.last_cycle));
        std::fprintf(f, "  vpn_ring:");
        for (u8 i = 0; i < v.nev; ++i) {
          const LifeEvRec& e = v.ev[(v.head + 8 - v.nev + i) & 7u];
          std::fprintf(f, " %llu:%s(%llu,%llu)", static_cast<unsigned long long>(e.cycle),
                       life_ev_name(e.kind), static_cast<unsigned long long>(e.a),
                       static_cast<unsigned long long>(e.b));
        }
        std::fprintf(f, "\n");
      } else {
        std::fprintf(f, "  vpn probes=0 (no probe was ever recorded for this VPN)\n");
      }
    }
  }
  // Pages probed but never installed: the short list a fill-loss would appear in.
  u32 vshown = 0;
  for (const auto& kv : vpn_life_) {
    const VpnLife& v = kv.second;
    if (v.installs != 0 || v.probes == 0) continue;
    if (vshown++ >= 64) break;
    std::fprintf(f, "VPN_NOINSTALL vpn=%llu core=%llu probes=%u joins=%u allocs=%u "
                    "walk_done=%u fills=%u last=%llu\n",
                 static_cast<unsigned long long>(kv.first >> 3),
                 static_cast<unsigned long long>(kv.first & 7u), v.probes, v.joins,
                 v.allocs, v.walk_done, v.fills,
                 static_cast<unsigned long long>(v.last_cycle));
  }
  std::fprintf(f, "# M26-B1 end of dump\n");
  std::fclose(f);
}

}  // namespace dx100
