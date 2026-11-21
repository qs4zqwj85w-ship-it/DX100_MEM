// M24-B: an HBM-style memory controller, selectable with `dram_kind=hbm`.
//
// This is a *separate* model. Nothing here is used when `dram_kind=ddr`, and the
// legacy two-bank path in memory.cpp is untouched by it. See
// build/wm/m24b/design.md for the documented facts, the abstractions and the
// assumptions behind every constant.
//
// Hierarchy (one `pa_line` = one 64 B physical cache line):
//   physical line -> stack -> pseudochannel -> bank group -> bank -> row -> column
//
// A pseudochannel is the model's scheduling unit: PG276 gives each of the two
// pseudochannels of a 128-bit channel its own 64-bit data path and its own banks,
// sharing only the command/address bus, so one controller per pseudochannel with a
// 64 B transfer is the closest simple model (design §2).
#pragma once

#include "sim.hpp"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace dx100 {

// Four-round Feistel over a 20-bit page number: a bijection for *any* round
// function, for any seed. Shared with the M24-A virtual-page mapping so both
// milestones permute pages with exactly one implementation.
inline std::uint32_t hbm_feistel_round(std::uint32_t x, std::uint32_t k) {
  x ^= k;
  x *= 0x9E3779B1u;
  x ^= x >> 15;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  return x & 1023u;
}
inline std::uint64_t hbm_permute_page(std::uint64_t p, std::uint32_t seed) {
  std::uint32_t a = static_cast<std::uint32_t>((p >> 10) & 1023u);
  std::uint32_t b = static_cast<std::uint32_t>(p & 1023u);
  for (std::uint32_t r = 0; r < 4; ++r) {
    const std::uint32_t k = seed * 0x9E3779B9u + r * 0x7F4A7C15u;
    const std::uint32_t na = b;
    b = a ^ hbm_feistel_round(b, k);
    a = na;
  }
  return (static_cast<std::uint64_t>(a) << 10) | b;
}

struct HbmParams {
  // geometry
  unsigned stacks = 2;
  unsigned pcs = 16;        // pseudochannels per stack (8 channels x 2)
  unsigned groups = 4;      // bank groups per pseudochannel
  unsigned banks_pg = 4;    // banks per bank group
  unsigned rows = 8192;     // rows per bank (derived from capacity by the caller)
  unsigned row_bytes = 1024;
  unsigned capacity_lines = 0;  // rows*pcs*stacks*groups*banks_pg*cols, for checks
  // mapping
  unsigned map = 1;         // 0 A row-oriented, 1 B interleaved, 2 C xor, 3 D page perm
  unsigned base_map = 1;    // decode used by D
  unsigned perm_seed = 1;
  unsigned perm_page_lines = 64;  // page granularity of D (64 = 4 KiB)
  unsigned il_lines = 4;          // interleave granularity of B/C (4 lines = 256 B)
  // timing [A]
  unsigned t_rcd = 12, t_rp = 12, t_ras = 28, t_cas = 12;
  unsigned t_ccd_s = 2, t_ccd_l = 4, t_rrd_s = 4, t_rrd_l = 6;
  unsigned t_burst = 4, t_faw = 0, t_refi = 0, t_rfc = 0;
  unsigned link = 0;
};

struct HbmStats {
  std::uint64_t reads = 0, hits = 0, conf = 0, act = 0, pre = 0, refresh = 0;
  std::uint64_t bytes = 0, wait_sum = 0, wait_max = 0;
  std::uint64_t q_occ_sum = 0, q_max = 0;
  std::uint64_t svc_events = 0, elig_sum = 0, elig_max = 0;
  std::uint64_t ch_reads[64] = {}, ch_busy[64] = {};
  std::uint64_t bank_acts[64 * 32] = {};
  std::uint64_t rows_used = 0, row_acts_sum = 0, row_acts_max = 0;
  std::uint64_t lat_hist[16] = {};   // log2 buckets of issue->ready
};

class HbmController {
 public:
  explicit HbmController(const HbmParams& p);

  // Decodes a physical line into the full hierarchy. Pure: no state is touched,
  // so the same line always decodes identically (determinism is a test).
  void decode(std::uint64_t pa_line, unsigned& st, unsigned& ch, unsigned& bg,
              unsigned& bank, unsigned& row, unsigned& col) const;
  // Linear bank id the M24-A release-order factor ranks by: (stack, pc, bg, bank).
  unsigned linear_bank(std::uint64_t pa_line) const;
  // (linear bank, row) for the M24-A release-order factor.
  void bank_row(std::uint64_t pa_line, unsigned& bank, unsigned& row) const;
  bool row_open(std::uint64_t pa_line) const;

  void enqueue(std::uint64_t line, std::uint64_t seq, std::uint64_t arrive, unsigned client);
  // Issues at most one command per pseudochannel; appends completions.
  void tick(std::uint64_t cycle, std::vector<HbmServed>& out);

  std::size_t depth() const { return ent_.size() - freed_; }
  std::uint64_t elig_last() const { return elig_last_; }
  const HbmStats& stats() const { return st_; }
  unsigned nchan() const { return nch_; }
  unsigned banks_per_ch() const { return banks_per_ch_; }
  unsigned cols() const { return cols_; }
  unsigned total_lines() const { return cols_ * p_.rows * p_.banks_pg * p_.groups * p_.pcs * p_.stacks; }

 private:
  struct Entry {
    std::uint64_t line = 0, seq = 0, arrive = 0;
    unsigned st = 0, ch = 0, bg = 0, bank = 0, row = 0, col = 0;
    unsigned client = 0;
    bool done = false;
  };
  void recompute_next(unsigned ch, std::uint64_t cycle);
  unsigned chan_of(unsigned st, unsigned pc) const { return st * p_.pcs + pc; }

  HbmParams p_;
  unsigned cols_ = 0, cb_ = 0, bb_ = 0, gb_ = 0, chb_ = 0, rb_ = 0, sb_ = 0;
  unsigned ilb_ = 0;
  unsigned nch_ = 0, banks_per_ch_ = 0;

  std::vector<Entry> ent_;
  std::size_t freed_ = 0;
  std::deque<unsigned> chq_[64];
  // per (channel, bank) row-buffer state
  std::vector<unsigned> open_row_, act_cycle_, bank_ready_;
  std::vector<char> open_;
  std::vector<std::uint64_t> data_ready_;
  std::vector<std::uint64_t> next_wake_;
  std::vector<std::uint64_t> grp_act_, any_act_, faw_;   // tRRD_L/S, tFAW
  std::vector<unsigned> faw_pos_;
  std::unordered_map<std::uint64_t, unsigned> row_cnt_;
  std::vector<std::uint64_t> refill_at_;
  std::uint64_t elig_last_ = 0;
  HbmStats st_;
};

}  // namespace dx100
