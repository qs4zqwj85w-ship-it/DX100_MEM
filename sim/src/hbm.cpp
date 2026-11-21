// M24-B: the HBM-style controller. See hbm.hpp and build/wm/m24b/design.md.
//
// Scheduling is per pseudochannel and identical in shape to the legacy model's
// FR-FCFS: an open-row request wins over a closed-row one, ties break by arrival
// and then by queue order. The difference is that `nch_` channels each arbitrate
// their own queue once per cycle, so channel parallelism is a property of the
// structure rather than a fitted speedup.
#include "hbm.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace dx100 {

namespace {
constexpr unsigned kWindow = 16;  // bounded FR-FCFS lookahead per channel

unsigned log2u(unsigned v) {
  unsigned b = 0;
  while ((1u << b) < v) ++b;
  if ((1u << b) != v) throw std::runtime_error("hbm: geometry must be a power of two");
  return b;
}
}  // namespace

HbmController::HbmController(const HbmParams& p) : p_(p) {
  if (p_.row_bytes % 64 || p_.row_bytes == 0)
    throw std::runtime_error("hbm: row_bytes must be a non-zero multiple of 64");
  cols_ = p_.row_bytes / 64;
  cb_ = log2u(cols_);
  bb_ = log2u(p_.banks_pg);
  gb_ = log2u(p_.groups);
  chb_ = log2u(p_.pcs);
  sb_ = log2u(p_.stacks);
  rb_ = log2u(p_.rows);
  ilb_ = log2u(std::max(1u, p_.il_lines));
  if (ilb_ > cb_) throw std::runtime_error("hbm: il_lines must not exceed a row");
  if (p_.pcs > 32 || p_.stacks > 2 || p_.groups > 8 || p_.banks_pg > 32)
    throw std::runtime_error("hbm: geometry outside the modelled limits");
  nch_ = p_.stacks * p_.pcs;
  banks_per_ch_ = p_.groups * p_.banks_pg;
  if (nch_ > 64) throw std::runtime_error("hbm: too many pseudochannels");
  const unsigned nb = nch_ * banks_per_ch_;
  open_row_.assign(nb, 0);
  act_cycle_.assign(nb, 0);
  bank_ready_.assign(nb, 0);
  open_.assign(nb, 0);
  data_ready_.assign(nch_, 0);
  next_wake_.assign(nch_, 0);
  grp_act_.assign(static_cast<size_t>(nch_) * p_.groups, 0);
  any_act_.assign(nch_, 0);
  faw_.assign(static_cast<size_t>(nch_) * 4, 0);
  faw_pos_.assign(nch_, 0);
  refill_at_.assign(nch_, p_.t_refi ? p_.t_refi : 0);
  // A geometry that is not large enough for the physical space it will be given
  // would alias silently; the caller checks capacity, and decode is defined mod
  // the geometry either way (documented in design §2).
}

void HbmController::decode(std::uint64_t L, unsigned& st, unsigned& ch, unsigned& bg,
                           unsigned& bank, unsigned& row, unsigned& col) const {
  if (p_.map == 3) {
    // D: permute the *page* the line belongs to, keep the offset inside it.
    const std::uint64_t page = L / p_.perm_page_lines;
    const std::uint64_t off = L % p_.perm_page_lines;
    L = (page < (1ull << 20) ? hbm_permute_page(page, p_.perm_seed) : page) *
            p_.perm_page_lines + off;
  }
  const unsigned total = cb_ + rb_ + bb_ + gb_ + chb_ + sb_;
  const std::uint64_t m = (L & ((1ull << total) - 1));
  // D is page placement: the decode that follows is `base_map` (A, B or C).
  const unsigned eff = (p_.map == 3) ? p_.base_map : p_.map;
  if (eff == 0) {
    // A: col | row | bank | bg | pc | stack  (low -> high). Sixteen consecutive
    // lines fill one 1 KiB row, and consecutive rows stay in the same bank: the
    // map that maximises row locality and minimises bank parallelism.
    col = static_cast<unsigned>(m & (cols_ - 1));
    row = static_cast<unsigned>((m >> cb_) & (p_.rows - 1));
    bank = static_cast<unsigned>((m >> (cb_ + rb_)) & (p_.banks_pg - 1));
    bg = static_cast<unsigned>((m >> (cb_ + rb_ + bb_)) & (p_.groups - 1));
    ch = static_cast<unsigned>((m >> (cb_ + rb_ + bb_ + gb_)) & (p_.pcs - 1));
    st = static_cast<unsigned>((m >> (cb_ + rb_ + bb_ + gb_ + chb_)) & (p_.stacks - 1));
    return;
  }
  // B/C: col[il-1:0] | pc | col[cb-1:il] | bg | bank | row | stack.
  // `ilb_` low column bits stay in one channel, so `il_lines` consecutive lines
  // (4 = 256 B by default, the chunk size GPUHammer measured on a real GPU)
  // share a channel and the next group moves to the next pseudochannel. A whole
  // 1 KiB row is therefore spread over all `pcs` channels, which is the point of
  // an interleaved map.
  const unsigned col_hi_bits = cb_ - ilb_;
  col = static_cast<unsigned>(m & ((1u << ilb_) - 1));
  ch = static_cast<unsigned>((m >> ilb_) & (p_.pcs - 1));
  const unsigned chi = static_cast<unsigned>((m >> (ilb_ + chb_)) & ((1u << col_hi_bits) - 1));
  col |= chi << ilb_;
  bg = static_cast<unsigned>((m >> (ilb_ + chb_ + col_hi_bits)) & (p_.groups - 1));
  bank = static_cast<unsigned>((m >> (ilb_ + chb_ + col_hi_bits + gb_)) & (p_.banks_pg - 1));
  row = static_cast<unsigned>((m >> (ilb_ + chb_ + col_hi_bits + gb_ + bb_)) & (p_.rows - 1));
  st = static_cast<unsigned>(
      (m >> (ilb_ + chb_ + col_hi_bits + gb_ + bb_ + rb_)) & (p_.stacks - 1));
  if (eff == 2) {
    // C: the documented XORs of design §3, on top of B.
    bank ^= static_cast<unsigned>(row & (p_.banks_pg - 1));
    bg ^= static_cast<unsigned>((row >> bb_) & (p_.groups - 1));
    ch ^= static_cast<unsigned>((row >> (bb_ + gb_)) & (p_.pcs - 1));
  }
}

unsigned HbmController::linear_bank(std::uint64_t pa_line) const {
  unsigned st, ch, bg, bank, row, col;
  decode(pa_line, st, ch, bg, bank, row, col);
  return (st * p_.pcs + ch) * banks_per_ch_ + bg * p_.banks_pg + bank;
}

void HbmController::bank_row(std::uint64_t pa_line, unsigned& bank, unsigned& row) const {
  unsigned st, ch, bg, bk, r, col;
  decode(pa_line, st, ch, bg, bk, r, col);
  bank = (st * p_.pcs + ch) * banks_per_ch_ + bg * p_.banks_pg + bk;
  row = r;
}

bool HbmController::row_open(std::uint64_t pa_line) const {
  unsigned st, ch, bg, bank, row, col;
  decode(pa_line, st, ch, bg, bank, row, col);
  const unsigned idx = chan_of(st, ch) * banks_per_ch_ + bg * p_.banks_pg + bank;
  return open_[idx] != 0 && open_row_[idx] == row;
}

void HbmController::enqueue(std::uint64_t line, std::uint64_t seq, std::uint64_t arrive,
                            unsigned client) {
  Entry e;
  e.line = line;
  e.seq = seq;
  e.arrive = arrive;
  e.client = client;
  decode(line, e.st, e.ch, e.bg, e.bank, e.row, e.col);
  const unsigned c = chan_of(e.st, e.ch);
  const unsigned idx = c * banks_per_ch_ + e.bg * p_.banks_pg + e.bank;
  ent_.push_back(e);
  chq_[c].push_back(static_cast<unsigned>(ent_.size() - 1));
  // A new arrival can make a channel eligible earlier than its current wake-up:
  // a line to an idle bank is serviceable now.
  next_wake_[c] = std::min<std::uint64_t>(next_wake_[c], bank_ready_[idx]);
}

void HbmController::recompute_next(unsigned c, std::uint64_t cycle) {
  std::uint64_t w = ~0ull;
  unsigned seen = 0;
  for (unsigned id : chq_[c]) {
    if (ent_[id].done) continue;
    if (seen++ >= kWindow) break;
    const Entry& e = ent_[id];
    const unsigned idx = c * banks_per_ch_ + e.bg * p_.banks_pg + e.bank;
    w = std::min<std::uint64_t>(w, std::max<std::uint64_t>(bank_ready_[idx], cycle));
  }
  next_wake_[c] = (w == ~0ull) ? 0 : w;
}

void HbmController::tick(std::uint64_t cycle, std::vector<HbmServed>& out) {
  st_.q_occ_sum += depth();
  st_.q_max = std::max<std::uint64_t>(st_.q_max, depth());
  elig_last_ = 0;
  for (unsigned c = 0; c < nch_; ++c) {
    // Drop finished ids from the front so the window scan stays cheap.
    while (!chq_[c].empty() && ent_[chq_[c].front()].done) {
      chq_[c].pop_front();
    }
    // Refresh is a property of the device, not of the queue: it happens even
    // when the channel is idle, which is why it is checked before the early-out.
    if (p_.t_refi && cycle >= refill_at_[c]) {
      // All-bank refresh: every bank of this channel closes for tRFC.
      for (unsigned b = 0; b < banks_per_ch_; ++b) {
        const unsigned idx = c * banks_per_ch_ + b;
        open_[idx] = 0;
        bank_ready_[idx] = std::max<std::uint64_t>(bank_ready_[idx], cycle + p_.t_rfc);
      }
      data_ready_[c] = std::max<std::uint64_t>(data_ready_[c], cycle + p_.t_rfc);
      refill_at_[c] = cycle + p_.t_refi;
      ++st_.refresh;
      next_wake_[c] = cycle + p_.t_rfc;
      continue;
    }
    if (chq_[c].empty()) { next_wake_[c] = 0; continue; }
    if (cycle < next_wake_[c]) continue;

    // FR-FCFS over a bounded window: open row first, then oldest arrival.
    unsigned best = 0xffffffffu;
    bool best_open = false;
    unsigned elig = 0;
    unsigned seen = 0;
    for (unsigned id : chq_[c]) {
      if (ent_[id].done) continue;
      if (seen++ >= kWindow) break;
      const Entry& e = ent_[id];
      const unsigned idx = c * banks_per_ch_ + e.bg * p_.banks_pg + e.bank;
      if (bank_ready_[idx] > cycle) continue;
      ++elig;
      const bool is_open = open_[idx] && open_row_[idx] == e.row;
      if (best == 0xffffffffu) { best = id; best_open = is_open; continue; }
      if (is_open != best_open) { if (is_open) { best = id; best_open = true; } continue; }
      if (e.arrive < ent_[best].arrive) { best = id; best_open = is_open; }
    }
    elig_last_ = elig;
    if (elig) { ++st_.svc_events; st_.elig_sum += elig; st_.elig_max = std::max(st_.elig_max, static_cast<std::uint64_t>(elig)); }
    if (best == 0xffffffffu) { recompute_next(c, cycle); continue; }

    Entry& e = ent_[best];
    const unsigned idx = c * banks_per_ch_ + e.bg * p_.banks_pg + e.bank;
    std::uint64_t ready = 0;
    const bool open = open_[idx] && open_row_[idx] == e.row;
    if (open) {
      ++st_.hits;
      ready = std::max<std::uint64_t>(cycle, data_ready_[c]) + p_.t_cas + p_.t_burst;
    } else {
      if (open_[idx]) ++st_.conf;  // a different row is open: precharge first
      std::uint64_t t = cycle;
      if (open_[idx]) {
        std::uint64_t pre = p_.t_rp;
        if (act_cycle_[idx] + p_.t_ras > t) pre = act_cycle_[idx] + p_.t_ras - t;
        ++st_.pre;
        t += pre;
      }
      // tRRD against the last activation, same and different bank group.
      const std::uint64_t ga = grp_act_[static_cast<size_t>(c) * p_.groups + e.bg];
      const std::uint64_t any = any_act_[c];
      if (ga && t < ga + p_.t_rrd_l) t = ga + p_.t_rrd_l;
      if (any && t < any + p_.t_rrd_s) t = any + p_.t_rrd_s;
      if (p_.t_faw) {
        const std::uint64_t oldest = faw_[static_cast<size_t>(c) * 4 + faw_pos_[c]];
        if (oldest && t < oldest + p_.t_faw) t = oldest + p_.t_faw;
      }
      grp_act_[static_cast<size_t>(c) * p_.groups + e.bg] = t;
      any_act_[c] = t;
      if (p_.t_faw) {
        faw_[static_cast<size_t>(c) * 4 + faw_pos_[c]] = t;
        faw_pos_[c] = (faw_pos_[c] + 1) % 4;
      }
      ++st_.act;
      ++st_.bank_acts[idx];
      const std::uint64_t key = static_cast<std::uint64_t>(idx) * p_.rows + e.row;
      unsigned& rc = row_cnt_[key];
      if (rc == 0) ++st_.rows_used;
      ++rc;
      ++st_.row_acts_sum;
      if (rc > st_.row_acts_max) st_.row_acts_max = rc;
      open_[idx] = 1;
      open_row_[idx] = e.row;
      act_cycle_[idx] = t;
      ready = t + p_.t_rcd + p_.t_cas + p_.t_burst;
    }
    bank_ready_[idx] = ready;
    data_ready_[c] = ready;
    st_.ch_busy[c] += p_.t_burst;

    ++st_.reads;
    ++st_.ch_reads[c];
    st_.bytes += 64;
    const std::uint64_t w = cycle - e.arrive;
    st_.wait_sum += w;
    st_.wait_max = std::max(st_.wait_max, w);
    unsigned lb = 0;
    while (lb < 15 && (w >> (lb + 1)) != 0) ++lb;
    ++st_.lat_hist[lb];

    HbmServed s;
    s.line = e.line;
    s.lat = ready - cycle + p_.link;
    s.ch = c;
    s.bank = idx;
    s.row = e.row;
    s.client = e.client;
    out.push_back(s);

    e.done = true;
    ++freed_;
    recompute_next(c, cycle);
  }
}

}  // namespace dx100
