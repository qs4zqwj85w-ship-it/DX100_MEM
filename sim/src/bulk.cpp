// M5: bulk indirect-access engine.
//
// Implements, for one descriptor at a time:
//
//   output[i] = data[index[i]]        (i = 0 .. descriptor.count-1)
//
// as three request streams into the *same* MemorySystem the SIMT baseline uses:
//
//   1. one descriptor fetch (the descriptor lives in memory, design §4.4),
//   2. index-line fetches: ceil(count * index_bytes / 64) FIFO line requests,
//   3. target-line requests: one per element, generated only after the index
//      value for that element has actually been fetched.
//
// Every one of those line requests occupies exactly one of the R residency slots
// from the cycle it is generated until the cycle its fill returns, so
//
//   unissued + issued_not_filled <= R
//
// holds by construction and is asserted every cycle. No duplicate detection, no
// reordering, no knowledge of future memory-controller state: issue is plain
// FIFO in generation order. The extension points for M6 are marked "M6:".
#include "sim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dx100 {

namespace {

// M9 window management: derive the window aggregates from the per-window records
// at export time. Nothing here runs during simulation, so no number below can
// influence a decision, and every one of them traces back to a recorded event
// (allocation, admission, seal, release) rather than to a snapshot.
void export_win_records(const BulkStats& b, const Config& cfg, Stats& s) {
  std::vector<u32> sz;
  u64 sealed = 0, sum = 0, age_sum = 0, age_max = 0, first_wait = 0, fill_span = 0;
  u64 partial_age = 0, first_wait_max = 0, fill_span_max = 0;
  u64 partial_n = 0, empty_discarded = 0;
  // Producer/consumer phases, from the four recorded events: fill is
  // alloc..seal, the issue-slot wait is seal..issue, the issuing phase is
  // issue..last_issue, the drain is last_issue..retire, and processing is
  // issue..retire.
  u64 issue_wait = 0, issue_wait_max = 0, proc = 0, proc_max = 0;
  u64 ispan = 0, ispan_max = 0, drn = 0, drn_max = 0;
  u64 hist[10] = {0};
  // M12: the bucket width follows W (W/8), so the histogram keeps its meaning when
  // `batch_window` is swept instead of labelling a 16K window with 8K buckets. At
  // the published W = 8 192 this is exactly the previous 1 024-entry bucketing, so
  // every earlier log keeps its numbers; only the labels for a larger W change.
  const u32 bw = std::max<u32>(1, cfg.batch_window / 8);
  for (const auto& w : b.win_recs) {
    if (w.n == 0) {
      if (w.why == 3) ++empty_discarded;
      continue;
    }
    ++sealed;
    sum += w.n;
    sz.push_back(w.n);
    hist[std::min<u32>(9, w.n / bw)]++;
    const u64 age = w.seal >= w.alloc ? w.seal - w.alloc : 0;
    age_sum += age;
    age_max = std::max(age_max, age);
    if (w.first != NONE) {
      const u64 fw = w.first - w.alloc;
      first_wait += fw;
      first_wait_max = std::max(first_wait_max, fw);
    }
    if (w.first != NONE && w.last != NONE) {
      const u64 sp = w.last - w.first;
      fill_span += sp;
      fill_span_max = std::max(fill_span_max, sp);
    }
    if (w.why == 2) {
      ++partial_n;
      partial_age += age;
    }
    if (w.issue >= w.seal && w.retire >= w.issue) {
      const u64 iw = w.issue - w.seal;
      const u64 pr = w.retire - w.issue;
      const u64 is = w.last_issue >= w.issue ? w.last_issue - w.issue : 0;
      const u64 dr = w.retire >= w.last_issue ? w.retire - w.last_issue : 0;
      issue_wait += iw;
      issue_wait_max = std::max(issue_wait_max, iw);
      proc += pr;
      proc_max = std::max(proc_max, pr);
      ispan += is;
      ispan_max = std::max(ispan_max, is);
      drn += dr;
      drn_max = std::max(drn_max, dr);
    }
  }
  std::sort(sz.begin(), sz.end());
  const u64 median = sz.empty() ? 0 : sz[(sz.size() - 1) / 2];  // lower median
  // Effective policy, so a log always says which sealing rule actually ran.
  s.set("bulk_win_policy", cfg.effective_win_policy());
  s.set("bulk_win_deadline", cfg.batch_deadline);
  s.set("bulk_win_buf_limit", cfg.effective_win_policy() == 1 ? 1 : 2);
  s.set("bulk_win_alloc", b.win_alloc);
  s.set("bulk_win_sealed", sealed);
  s.set("bulk_win_partial", partial_n);
  s.set("bulk_win_empty_discarded", empty_discarded);
  s.set("bulk_win_size_sum", sum);
  s.set("bulk_win_size_min", sz.empty() ? 0 : sz.front());
  s.set("bulk_win_size_max", sz.empty() ? 0 : sz.back());
  s.set("bulk_win_size_median", median);
  for (u32 i = 0; i < 10; ++i) s.set("bulk_win_hist_" + std::to_string(i), hist[i]);
  s.set("bulk_win_age_sum", age_sum);
  s.set("bulk_win_age_max", age_max);
  s.set("bulk_win_first_wait_sum", first_wait);
  s.set("bulk_win_first_wait_max", first_wait_max);
  s.set("bulk_win_first_wait_mean", sealed ? first_wait / sealed : 0);
  s.set("bulk_win_fill_span_sum", fill_span);
  s.set("bulk_win_fill_span_max", fill_span_max);
  s.set("bulk_win_fill_span_mean", sealed ? fill_span / sealed : 0);
  s.set("bulk_win_partial_age_sum", partial_age);
  s.set("bulk_win_partial_age_mean", partial_n ? partial_age / partial_n : 0);
  s.set("bulk_win_age_mean", sealed ? age_sum / sealed : 0);
  s.set("bulk_win_size_mean", sealed ? sum / sealed : 0);
  s.set("bulk_win_issue_wait_mean", sealed ? issue_wait / sealed : 0);
  s.set("bulk_win_issue_wait_max", issue_wait_max);
  s.set("bulk_win_proc_mean", sealed ? proc / sealed : 0);
  s.set("bulk_win_proc_max", proc_max);
  s.set("bulk_win_issue_span_mean", sealed ? ispan / sealed : 0);
  s.set("bulk_win_issue_span_max", ispan_max);
  s.set("bulk_win_drain_mean", sealed ? drn / sealed : 0);
  s.set("bulk_win_drain_max", drn_max);
  s.set("bulk_win_issue_idle", b.win_issue_idle);
  s.set("bulk_win_live_max", b.win_live_max);
  s.set("bulk_win_incomplete_max", b.win_incomplete_max);
  s.set("bulk_win_drain_max", b.win_drain_max);
  s.set("bulk_win_live_cycles", b.win_live_cycles);
  s.set("bulk_win_fill_wait", b.win_fill_wait);
  s.set("bulk_win_fill_blocked", b.win_fill_blocked);
  // M12: peak realized storage of the two batch buffers in the simulator itself
  // (the vectors that actually hold entries, the sealed order and the issue
  // order). Diagnostic: it is a `capacity()` reading taken in the occupancy
  // block, never read by a decision, and `bulk_win_bytes` remains the design's
  // modelled SRAM charge (8 B per entry).
  s.set("bulk_win_vec_bytes", b.win_vec_bytes);
  // M15: the fused prototype's own realized storage, and whether it was active.
  // `bulk_win_bytes` above stays the M6-M13 design charge either way, so the two
  // layouts' design footprints are computed independently in the report.
  s.set("bulk_win_fused", b.win_fused);
  s.set("bulk_win_fused_vec_bytes", b.win_fused_vec_bytes);
  // M13: the bounded cross-window reuse buffer. `bulk_reuse_bytes` is the design
  // charge -- entries x 88 B (64 data + 4 tag + 4 LRU + state/count + 3 x 4 B
  // subscribers) -- and `bulk_reuse_vec_bytes` is what the simulator's own
  // vector actually occupies, reported so the two are never confused.
  s.set("bulk_reuse_on", cfg.reuse_dir ? 1 : 0);
  s.set("bulk_reuse_sets", cfg.reuse_dir ? cfg.reuse_sets : 0);
  s.set("bulk_reuse_ways", cfg.reuse_dir ? cfg.reuse_ways : 0);
  s.set("bulk_reuse_entries",
        cfg.reuse_dir ? static_cast<u64>(std::max<u32>(1, cfg.reuse_sets)) *
                            std::max<u32>(1, cfg.reuse_ways)
                      : 0ull);
  s.set("bulk_reuse_hash", cfg.reuse_hash ? 1 : 0);
  s.set("bulk_reuse_merge", cfg.reuse_merge ? 1 : 0);
  s.set("bulk_reuse_charge", cfg.reuse_charge ? 1 : 0);
  s.set("bulk_reuse_lines_planned", b.reuse_lines_planned);
  s.set("bulk_reuse_probes", b.reuse_probes);
  s.set("bulk_reuse_hit_ready", b.reuse_hit_ready);
  s.set("bulk_reuse_hit_inflight", b.reuse_hit_inflight);
  s.set("bulk_reuse_miss", b.reuse_miss);
  s.set("bulk_reuse_full", b.reuse_full);
  s.set("bulk_reuse_sub_full", b.reuse_sub_full);
  s.set("bulk_reuse_evict", b.reuse_evict);
  s.set("bulk_reuse_inserts", b.reuse_inserts);
  s.set("bulk_reuse_dropped", b.reuse_dropped);
  s.set("bulk_reuse_elems_ready", b.reuse_elems_ready);
  s.set("bulk_reuse_elems_merged", b.reuse_elems_merged);
  s.set("bulk_reuse_elems", b.reuse_elems_ready + b.reuse_elems_merged);
  s.set("bulk_reuse_req_avoided", b.reuse_hit_ready + b.reuse_hit_inflight);
  s.set("bulk_reuse_sub_max", b.reuse_sub_max);
  s.set("bulk_reuse_occ_max", b.reuse_occ_max);
  s.set("bulk_reuse_occ_sum", b.reuse_occ_sum);
  s.set("bulk_reuse_bytes",
        cfg.reuse_dir
            ? static_cast<u64>(std::max<u32>(1, cfg.reuse_sets)) *
                  std::max<u32>(1, cfg.reuse_ways) * 88ull
            : 0ull);
  s.set("bulk_reuse_vec_bytes", b.reuse_vec_bytes);
  // Individual windows, in allocation order. Up to eight; the runs in the
  // investigation seal at most five, so this is the complete distribution.
  u32 slot = 0;
  for (const auto& w : b.win_recs) {
    if (slot >= 8) break;
    const std::string p = "bulk_win_r" + std::to_string(slot) + "_";
    s.set(p + "n", w.n);
    s.set(p + "alloc", w.alloc);
    s.set(p + "first", w.first == NONE ? 0 : w.first);
    s.set(p + "last", w.last == NONE ? 0 : w.last);
    s.set(p + "seal", w.seal);
    s.set(p + "retire", w.retire);
    s.set(p + "why", w.why);
    s.set(p + "issue", w.issue);
    s.set(p + "last_issue", w.last_issue);
    ++slot;
  }
  s.set("bulk_win_recs", slot);
}

constexpr u32 DESC_MAGIC = 0x44583031u;  // "DX01" as little-endian bytes
constexpr u32 DESC_VERSION = 1u;

inline void put32(u8* p, u32 v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}
inline void put64(u8* p, u64 v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}
inline u32 get32(const u8* p) {
  u32 v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (8 * i);
  return v;
}
inline u64 get64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}
// Width-limited little-endian load, used for both index values (4 or 8 B) and
// data elements (1, 2, 4 or 8 B).
inline u64 load_le(const u8* p, u32 bytes) {
  u64 v = 0;
  for (u32 i = 0; i < bytes; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}
inline u64 fnv(u64 h, u64 v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xff;
    h *= 1099511628211ull;
  }
  return h;
}

// ---- M15: the fused 64-bit window word ------------------------------------
// What a window element must remember: which target line (26 bits at 64 B
// granularity inside the 4 GiB VA space), which descriptor position it is (32
// bits), and where in the line (6 bits). Exactly 64 bits, and all of it: the
// data width is a descriptor field and the value itself is never stored.
//
// Field order is not arbitrary. Sorting the word numerically orders by
// (va_line, position, offset), which is the same order the M6 layout's
// (va_line, entry id) comparator produced for an in-order window, so the fanout
// sequence within a line is unchanged.
inline u64 fuse_ent(u64 va_line, u32 pos, u32 off) {
  return ((va_line & 0x3ffffffull) << 38) | (static_cast<u64>(pos) << 6) | (off & 0x3fu);
}
inline u64 fuse_line(u64 w) { return w >> 38; }
inline u32 fuse_pos(u64 w) { return static_cast<u32>((w >> 6) & 0xffffffffull); }
inline u32 fuse_off(u64 w) { return static_cast<u32>(w & 0x3fu); }

}  // namespace

// ---------------------------------------------------------------------------
// Descriptor encoding
// ---------------------------------------------------------------------------
void BulkDesc::encode(u8* line64) const {
  for (u32 i = 0; i < LINE_BYTES; ++i) line64[i] = 0;
  put32(line64 + 0, DESC_MAGIC);
  put32(line64 + 4, DESC_VERSION);
  put64(line64 + 8, desc_line);
  put64(line64 + 16, index_base);
  put64(line64 + 24, data_base);
  put32(line64 + 32, count);
  put32(line64 + 36, elem_base);
  line64[40] = index_bytes;
  line64[41] = data_bytes;
}

bool BulkDesc::decode(const u8* line64, BulkDesc& out) {
  if (get32(line64 + 0) != DESC_MAGIC) return false;
  if (get32(line64 + 4) != DESC_VERSION) return false;
  out.desc_line = get64(line64 + 8);
  out.index_base = get64(line64 + 16);
  out.data_base = get64(line64 + 24);
  out.count = get32(line64 + 32);
  out.elem_base = get32(line64 + 36);
  out.index_bytes = line64[40];
  out.data_bytes = line64[41];
  return true;
}

std::string BulkDesc::validate(u32 max_elems) const {
  if (index_bytes != 4 && index_bytes != 8) return "index_bytes must be 4 or 8";
  if (data_bytes != 1 && data_bytes != 2 && data_bytes != 4 && data_bytes != 8)
    return "data_bytes must be 1, 2, 4 or 8";
  if (64 % data_bytes != 0) return "data_bytes must divide the 64 B line";
  // Line alignment is a correctness condition, not a convenience: the index
  // stream is fetched one line at a time and an index element is extracted at
  // (first element of the line) + pos * index_bytes, so an array that did not
  // start on a line boundary would be decoded at the wrong offsets.
  if (index_base % LINE_BYTES != 0) return "index_base must be 64 B (line) aligned";
  if (data_base % data_bytes != 0) return "data_base is not data_bytes-aligned";
  if (count > max_elems) return "count exceeds bulk_max_elems";
  if (desc_line >= (VA_LIMIT >> 6)) return "desc_line outside the 4 GiB VA space";
  if (index_base >= VA_LIMIT) return "index_base outside the 4 GiB VA space";
  if (data_base >= VA_LIMIT) return "data_base outside the 4 GiB VA space";
  // The whole index array must lie inside Sv32 and must not wrap.
  if (static_cast<u64>(count) * index_bytes > VA_LIMIT ||
      index_base + static_cast<u64>(count) * index_bytes > VA_LIMIT)
    return "index array exceeds the 4 GiB VA space";
  if (static_cast<u64>(elem_base) + count > VA_LIMIT)
    return "element ids exceed the 32-bit element-id space";
  return "";
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
// M9: `pool` is the shared residency budget. Null -- the default, and what both
// published front-ends pass -- keeps the engine's own free list, so the pure
// bulk path is byte for byte the M8 machine. Non-null means the driver owns the
// budget and this engine is one of two clients.
BulkEngine::BulkEngine(const Config& cfg, MemorySystem& mem, SlotPool* pool)
    : cfg_(cfg), mem_(mem), pool_(pool) {
  // M15: the fused layout's preconditions, enforced where they arise. Each one
  // follows from the representation rather than from a preference: a window is
  // what gets sealed, dedup is what makes a line a contiguous run, page_order is
  // what makes the sorted array the issue order, (start,count) are 16-bit, the
  // residency slot is 8-bit, and the M13 reuse buffer keeps a per-line record the
  // fused array does not carry.
  if (cfg_.win_fused) {
    if (!windowed()) throw std::runtime_error("win_fused requires batch_window > 0");
    if (!cfg_.dedup) throw std::runtime_error("win_fused requires dedup = 1 (a line is a run)");
    if (!cfg_.page_order) throw std::runtime_error("win_fused requires page_order = 1");
    if (cfg_.batch_window > 65536)
      throw std::runtime_error("win_fused requires batch_window <= 65536 (16-bit run bounds)");
    if (cfg_.R > 255) throw std::runtime_error("win_fused requires R <= 255 (8-bit slot)");
    if (cfg_.reuse_dir) throw std::runtime_error("win_fused is not combined with reuse_dir");
  }
  // M23-G3: the organization policies reuse the fused run array, so they exist
  // only where it does. P2/P3 group by the DRAM map the controller uses, which
  // is exact only for the production mapping (the diagnostic dram_map = 1..5
  // maps would silently make the engine's grouping disagree with the DRAM).
  // M23-G6C is G6B's pipeline with P0's admission policy, so it inherits every
  // fused/organization precondition above and adds only the ones below.
  if (cfg_.bulk_org > 8) throw std::runtime_error("bulk_org must be 0..8");
  // M23-G6A: the PA buffer is the experiment's own resource. A zero capacity
  // would translate a window and drop its addresses, so it is refused here
  // rather than discovered as a wrong cycle count later.
  if (cfg_.bulk_org == 6 && cfg_.g6a_pa_capacity == 0)
    throw std::runtime_error("bulk_org = 6 requires g6a_pa_capacity > 0");
  // M23-G6B: the ready buffer is the streaming policy's only coupling between
  // translation and DRAM. A zero capacity could never admit a translation.
  if ((cfg_.bulk_org == 7 || cfg_.bulk_org == 8) && cfg_.g6b_pa_capacity == 0)
    throw std::runtime_error("bulk_org = 7 or 8 requires g6b_pa_capacity > 0");
  // M23-G6C *is* P0's page-run scheduler, pointed at the translation port: its
  // cohort table is `b.coh` (installed by `win_seal`) and its retry interval is
  // `park_retry`. A configuration without them has no policy to run, so it is
  // refused here rather than silently falling back to G6B's single cursor.
  if (cfg_.bulk_org == 8 && (!cfg_.page_sched || cfg_.page_cohorts == 0))
    throw std::runtime_error("bulk_org = 8 requires page_sched = 1 and page_cohorts > 0");
  if (cfg_.bulk_org && !fused())
    throw std::runtime_error("bulk_org requires the fused window (win_fused = 1)");
  if (cfg_.bulk_org >= 2 && cfg_.dram_map)
    throw std::runtime_error("bulk_org >= 2 requires the production address map (dram_map = 0)");
  // M24-A: the explicit DRAM-organization factor is defined as "none of the
  // physical ordering is applied" against the streaming arms, which are the only
  // arms whose release order *is* the organizer's output. Refusing every other
  // policy keeps the factor from silently redefining P1/P2/P3/P4.
  // M27-P4: `m24_dram_org = false` is the production default from M27 on, so it
  // must be admissible for every arm. What is still refused is turning the
  // organizer off for an arm whose *definition* is the organizer (the M24-A
  // comparison policies P1-P5), where the flag would silently redefine the arm.
  if (!cfg_.m24_dram_org &&
      (cfg_.bulk_org == 1 || cfg_.bulk_org == 2 || cfg_.bulk_org == 3 || cfg_.bulk_org == 5))
    throw std::runtime_error(
        "m24_dram_org = 0 is not defined for bulk_org = 1,2,3,5: those arms are the "
        "physical-order organizer, so disabling it would silently redefine them");
  // M27-P5: the engine's requests enter one SM. With one core that SM is
  // unambiguous; with several, the configuration must name it, because routing
  // every engine request through core 0's private L1 would silently be a
  // different (and unsupported) machine.
  if (cfg_.eng_core != 0xffffffffu && cfg_.eng_core >= cfg_.cores)
    throw std::runtime_error("eng_core must name an existing core");
  if (cfg_.m24_map > 3) throw std::runtime_error("m24_map must be 0..3");
  if (cfg_.m24_map && cfg_.dram_map)
    throw std::runtime_error("m24_map composes with the production map only (dram_map = 0)");
  slot_cap_ = pool_ ? static_cast<u32>(pool_->owner.size()) : cfg_.R;
  info_.assign(std::max<u32>(1, slot_cap_), Pending{});
  // M13: the reuse buffer exists only when the mechanism is on and the windowed
  // path (the only path with a seal event to probe at) is active. Allocated
  // once, bounded, never grown.
  if (reuse_on()) {
    reuse_sets_ = std::max<u32>(1, cfg_.reuse_sets);
    reuse_ways_ = std::max<u32>(1, cfg_.reuse_ways);
    reuse_.assign(static_cast<size_t>(reuse_sets_) * reuse_ways_, ReuseEnt{});
  }
  if (pool_) return;  // the driver's pool already holds the ids
  for (u32 i = cfg_.R; i-- > 0;) free_.push_back(i);
}

// ---- M9 slot plumbing -----------------------------------------------------
// Every take and every release goes through these four helpers, so the two modes
// differ only in which list the ids come from. On the pure path they are exactly
// the operations the engine performed before (`back()/pop_back()`, `push_back`),
// which is what keeps the review's E1/E2 equivalence argument true.
u64 BulkEngine::engine_residency() const {
  if (pool_) return pool_->held_by(1);
  return unissued_.size() + idx_q_.size() + mem_.inflight_count();
}
size_t BulkEngine::free_count() const { return pool_ ? pool_->free_count() : free_.size(); }
bool BulkEngine::slot_space() const { return pool_ ? pool_->has_free() : !free_.empty(); }
u32 BulkEngine::alloc_slot() {
  ++st_.slot_take;  // diagnostic counter only
  if (pool_) return pool_->take(1);
  const u32 s = free_.back();
  free_.pop_back();
  return s;
}
void BulkEngine::free_slot(u32 s) {
  ++st_.slot_give;  // diagnostic counter only
  if (pool_) {
    pool_->give(s);  // false = double release; the driver's invariant catches it
    return;
  }
  free_.push_back(s);
}

bool BulkEngine::submit(const BulkDesc& d) {
  st_.desc_submitted++;
  if (d.desc_line >= (VA_LIMIT >> 6)) {
    reject("descriptor rejected at submission: desc_line outside the VA space");
    return false;
  }
  queue_.push_back(d);
  return true;
}

void BulkEngine::reject(const std::string& why) { st_.desc_rejected++; err_.push_back(why); }

// Index requests waiting on the dedicated port (M8 mode 3) hold a residency slot
// exactly like `unissued_`, so they are counted here. Without this the R budget
// would silently grow by up to `idx_mshr` requests.
size_t BulkEngine::residency() const { return static_cast<size_t>(engine_residency()); }

bool BulkEngine::idle() const {
  return !active_ && queue_.empty() && unissued_.empty() && mem_.idle() && win_idle();
}

// No batch holds anything, no fill is waiting to be fanned out, and the
// organizer has finished. `idle()` must not return true while a window still
// owns elements, or a run would end with work outstanding.
bool BulkEngine::win_idle() const {
  if (!windowed()) return true;
  if (!fan_.empty() || st_.org_backlog) return false;
  for (const auto& b : bat_)
    if (b.n_ent) return false;
  return true;
}

u64 BulkEngine::value_of(u32 elem) const {
  auto it = val_.find(elem);
  return it == val_.end() ? NONE : it->second;
}

u64 BulkEngine::addr_of(u32 elem) const {
  auto it = addr_.find(elem);
  return it == addr_.end() ? NONE : it->second;
}

u32 BulkEngine::alloc_and_enqueue(ReqKind k, u64 va_line, u32 elem, u64 first_elem) {
  const u32 slot = alloc_slot();
  Pending p;
  p.kind = k;
  p.va_line = va_line;
  p.elem = elem;
  p.seq = seq_++;
  p.first_elem = first_elem;
  info_[slot] = p;
  // M8: the index stream gets its own issue queue (and thus its own arbitration
  // point) in mode 3. The slot still comes from the shared R pool.
  if (k == ReqKind::Index && idx_windowed() && cfg_.idx_mode >= 3) idx_q_.push_back(slot);
  else unissued_.push_back(slot);
  return slot;
}

void BulkEngine::start_next(u64 cycle) {
  if (active_ || queue_.empty()) return;
  if (!slot_space()) { st_.buf_full_stall++; return; }  // no slot for the fetch
  run_ = Run{};
  run_.d = queue_.front();
  queue_.pop_front();
  run_.phase = cfg_.desc_fetch ? 0 : 1;
  run_.start = cycle;
  active_ = true;
  if (cfg_.desc_fetch) alloc_and_enqueue(ReqKind::Desc, run_.d.desc_line, 0, 0);
  else {
    // Ablation only: fields are taken as submitted and validated here instead
    // of by a fetch. Off by default (design §4.4 charges the fetch).
    const std::string why = run_.d.validate(cfg_.bulk_max_elems);
    if (!why.empty()) {
      reject("descriptor rejected: " + why);
      abort_desc();
      return;
    }
    st_.idx_elems_req += run_.d.count;
    st_.elems_total += run_.d.count;
    if (run_.d.count == 0) finish_desc(cycle);
  }
}

void BulkEngine::on_desc(u64 cycle, const Pending& p) {
  st_.first_desc_done = cycle;
  const u8* line = mem_.line_bytes(mem_.pa_line_of_va_line(p.va_line));
  BulkDesc d;
  if (!BulkDesc::decode(line, d)) {
    reject("descriptor rejected: decode failed (magic/version)");
    abort_desc();
    return;
  }
  if (d.desc_line != p.va_line) {
    reject("descriptor rejected: descriptor line does not match its fetch address");
    abort_desc();
    return;
  }
  const std::string why = d.validate(cfg_.bulk_max_elems);
  if (!why.empty()) {
    reject("descriptor rejected: " + why);
    abort_desc();
    return;
  }
  st_.desc_fetch_cycles_sum += cycle - run_.start;  // submission -> decoded
  run_.d = d;
  run_.phase = 1;
  run_.next_line_elem = 0;
  run_.gen_cursor = 0;
  run_.gen_done = 0;
  run_.done = 0;
  st_.idx_elems_req += d.count;
  st_.elems_total += d.count;
  if (d.count == 0) finish_desc(cycle);  // empty descriptor: fetch, validate, retire
}

void BulkEngine::on_index(u64 cycle, const Pending& p) {
  st_.idx_lines_done++;
  st_.last_idx_done = cycle;
  if (st_.idx_first_done == NONE) st_.idx_first_done = cycle;
  // M8: the response is matched to its tile slot by line address, so a fill that
  // arrives out of order marks exactly its own line ready and nothing else.
  if (idx_windowed() && cfg_.idx_mode > 0) {
    auto it2 = idx_pending_.find(p.seq);
    if (it2 != idx_pending_.end()) {
      if (it2 != idx_pending_.begin()) st_.idx_lines_ooo++;
      idx_pending_.erase(it2);
    }
    if (idx_inflight_ > 0) --idx_inflight_;
    if (idx_resident_ > 0) --idx_resident_;
    IdxRing& r = run_.ring;
    for (u32 j = 0; j < r.n; ++j) {
      const u32 s = (r.head + j) % IDX_SLOTS;
      if (!r.ready[s] && r.va[s] == p.va_line) { r.ready[s] = 1; return; }
    }
    return;
  }
  // An index line completed out of request order if older requests are still
  // outstanding. Tracked against request order, not issue order (they are the
  // same here: issue is FIFO).
  auto it = idx_pending_.find(p.seq);
  if (it != idx_pending_.end()) {
    if (it != idx_pending_.begin()) st_.idx_lines_ooo++;
    idx_pending_.erase(it);
  }
  idx_done_[p.first_elem] = p.va_line;
  if (idx_inflight_ > 0) --idx_inflight_;
  if (idx_resident_ > 0) --idx_resident_;
}

void BulkEngine::on_target(u64 cycle, const Pending& p) {
  st_.tgt_req_done++;
  st_.last_fill = cycle;
  st_.busy_at_last_fill = mem_.dram.busy_cycles;
  st_.elems_done++;
  {
    auto it = tgt_pending_.find(p.seq);
    if (it != tgt_pending_.end()) {
      if (it != tgt_pending_.begin()) st_.tgt_ooo++;
      tgt_pending_.erase(it);
    }
  }
  const u64 pa = mem_.pa_line_of_va_line(p.va_line);
  const u8* b = mem_.line_bytes(pa);
  val_[p.elem] = load_le(b + p.off, p.width);
  const u64 addr = p.va_line * LINE_BYTES + p.off;
  addr_[p.elem] = addr;
  // Independent re-derivation of this element's address from the index image:
  // element index -> index byte address -> index value -> target address. A
  // disagreement means bad extraction, bad scaling or a stale index line.
  if (active_ && run_.phase == 1 && p.elem >= run_.d.elem_base) {
    const u64 pos = p.elem - run_.d.elem_base;
    const u64 ib = run_.d.index_bytes;
    const u8* ib_bytes = mem_.line_bytes(mem_.pa_line_of_va(run_.d.index_base + pos * ib));
    const u64 off = (run_.d.index_base + pos * ib) & (LINE_BYTES - 1);
    const u64 expect = run_.d.data_base + load_le(ib_bytes + off, static_cast<u32>(ib)) * run_.d.data_bytes;
    if (expect != addr) st_.addr_mismatch++;
  }
  st_.completion_hash = fnv(st_.completion_hash, p.elem);
  run_.done++;
  if (active_ && run_.phase == 1 && run_.done == run_.d.count) finish_desc(cycle);
}

// A descriptor that leaves the engine without completing (rejected, or the run
// ended): its own counters stay untouched, so submitted == completed + rejected.
void BulkEngine::abort_desc() {
  active_ = false;
  win_reset();
  reuse_abort();  // outstanding records can never be filled now
  run_ = Run{};
  idx_done_.clear();
  idx_pending_.clear();
  tgt_pending_.clear();
  // Slots queued on the dedicated index port were never submitted: return them.
  for (u32 s2 : idx_q_) free_slot(s2);
  idx_q_.clear();
  idx_inflight_ = 0;
  idx_resident_ = 0;
  last_tgt_elem_ = NONE;
}

void BulkEngine::win_reset() {
  for (int i = 0; i < 2; ++i) {
    // A window dropped without being sealed (only an aborted descriptor does
    // this) is still reported, as a discarded record, never silently lost.
    if (BulkStats::WinRec* rec = win_rec(i)) {
      if (rec->seal == 0) {
        rec->seal = rec->alloc;
        rec->why = 3;
      }
      rec_ix_[static_cast<size_t>(i)] = 0xffffffffu;
    }
    bat_[static_cast<size_t>(i)] = Batch{};
  }
  fan_.clear();
  // M28-D: the release path's shared buffers outlive a window; nothing is live
  // once every window has been dropped.
  rel_ready_.clear();
  rel_pos_.clear();
  fill_ = iss_ = -1;
  seal_pending_ = false;
  win_occ_ = 0;
  fan_backlog_ = 0;
  st_.org_backlog = 0;
}

// M9 window management: the record of the window a buffer currently holds.
BulkStats::WinRec* BulkEngine::win_rec(int buf) {
  if (buf < 0 || buf > 1) return nullptr;
  const u32 ix = rec_ix_[static_cast<size_t>(buf)];
  if (ix == 0xffffffffu || ix >= st_.win_recs.size()) return nullptr;
  return &st_.win_recs[ix];
}

void BulkEngine::finish_desc(u64 cycle) {
  if (!active_) return;
  win_reset();
  st_.desc_completed++;
  const u64 lat = cycle - run_.start;
  st_.desc_cycles_sum += lat;
  st_.desc_cycles_max = std::max(st_.desc_cycles_max, lat);
  active_ = false;
  run_ = Run{};
  idx_done_.clear();
  idx_pending_.clear();
  tgt_pending_.clear();
  for (u32 s2 : idx_q_) free_slot(s2);
  idx_q_.clear();
  idx_inflight_ = 0;
  last_tgt_elem_ = NONE;
}

void BulkEngine::attribute(u64 cycle, const std::vector<u64>& done) {
  for (u64 pack : done) {
    const u32 slot = static_cast<u32>(pack & 0xffffffffu);
    const Pending p = info_[slot];
    info_[slot] = Pending{};
    free_slot(slot);
    switch (p.kind) {
      case ReqKind::Desc: on_desc(cycle, p); break;
      case ReqKind::Index: on_index(cycle, p); break;
      case ReqKind::Xlate:
        // M27-P3: a phase-1 probe holds a slot for its whole translation; its
        // result travels through `xlate_done_`, so releasing the slot (done
        // above, for every kind) is all this completion has to do.
        break;
      case ReqKind::Target:
        // A windowed request names the batch line whose dependents it retires;
        // the M5 path has no window and keeps its per-element retirement.
        if (p.win_line != 0xffffffffu) on_line(cycle, p);
        else on_target(cycle, p);
        break;
    }
  }
}

void BulkEngine::generate(u64 cycle) {
  if (!active_ || run_.phase != 1) return;
  const BulkDesc& d = run_.d;
  const u32 per = LINE_BYTES / d.index_bytes;  // indices per index line

  // (a) Stream the index array (shared with the windowed path, so both regimes
  // charge exactly the same index traffic).
  stream_index();

  // (b) Address generation: consume index values that have actually arrived,
  // in element order. `idx_done_` is keyed by the element range each completed
  // index line covers, so out-of-order fills are held until their turn; a
  // target request for element i can never be formed before its index line has
  // returned (structural; `addr_mismatch` re-derives every address at completion).
  u32 budget = std::max<u32>(1, cfg_.ag_lanes);
  bool blocked = false;
  while (!blocked && budget > 0 && run_.gen_cursor < d.count) {
    // The oracle removes the *lookup*, not the address generation: the value
    // still flows through the same multiply, fault check and request path.
    const bool direct = oracle_ != nullptr;
    u64 chunk = (run_.gen_cursor / per) * per;
    const u8* b = nullptr;
    if (direct) {
      if (run_.gen_cursor >= oracle_->size()) break;
    } else {
      auto it = idx_done_.find(chunk);
      if (it == idx_done_.end()) {
        st_.idx_wait_stall++;  // AG idle: the index value is not here yet
        break;
      }
      b = mem_.line_bytes(mem_.pa_line_of_va_line(it->second));
    }
    while (budget > 0 && run_.gen_cursor < d.count &&
           (direct || (run_.gen_cursor / per) * per == chunk)) {
      if (!slot_space()) { st_.buf_full_stall++; blocked = true; break; }
      const u64 pos = run_.gen_cursor - chunk;
      const u64 v = direct ? (*oracle_)[run_.gen_cursor]
                           : load_le(b + pos * d.index_bytes, d.index_bytes);
      if (direct) st_.oracle_elems++;
      const u64 addr = d.data_base + v * d.data_bytes;
      const u32 elem = d.elem_base + static_cast<u32>(run_.gen_cursor);
      if (addr + d.data_bytes > VA_LIMIT) {
        // Reported, not silently dropped: the element is accounted as faulted.
        st_.addr_faults++;
        run_.gen_cursor++;
        budget--;
        continue;
      }
      const u32 slot = alloc_and_enqueue(ReqKind::Target, addr >> 6, elem, chunk);
      info_[slot].off = static_cast<u16>(addr & (LINE_BYTES - 1));
      info_[slot].width = d.data_bytes;
      tgt_pending_.insert(info_[slot].seq);
      if (st_.tgt_first_gen == NONE) st_.tgt_first_gen = cycle;
      st_.tgt_addr_gen++;
      st_.tgt_req_gen++;
      st_.idx_elems_done++;
      if (st_.first_admit == NONE) st_.first_admit = cycle;
      run_.gen_cursor++;
      budget--;
    }
    // Retire the chunk once every element it covers has been generated.
    if (!direct &&
        (run_.gen_cursor >= d.count || run_.gen_cursor / per != chunk / per))
      idx_done_.erase(chunk);
  }
  run_.gen_done = run_.gen_cursor;  // the M5 path generates strictly in order
}

void BulkEngine::issue(u64 cycle) {
  u32 n = std::max<u32>(1, cfg_.eng_issue);
  while (n > 0 && !unissued_.empty()) {
    const u32 slot = unissued_.front();
    Pending& p = info_[slot];
    Req r;
    r.va_line = p.va_line;
    r.vpn = p.va_line >> cfg_.page_line_bits();
    r.core = ecore();  // M27-P5: the SM named by eng_core
    r.slot = slot;
    r.seq = p.seq;
    r.bypass_l1 = true;  // the engine has no private L1 (design §4.4)
    r.client = (p.kind == ReqKind::Index) ? 1 : 3;  // M11.3: 1 = index, 3 = target
    if (!mem_.submit(r, cycle)) {
      st_.issue_stall++;
      if (p.kind == ReqKind::Index) st_.idx_refuse++;
      break;
    }
    if (p.kind == ReqKind::Target) {
      st_.tgt_req_issued++;
      // M5 issue policy: strict FIFO, so target requests are issued in element
      // order. Anything else would be an M6 scheduling result.
      if (last_tgt_elem_ != NONE && p.elem < last_tgt_elem_) st_.issue_monotonic_violations++;
      last_tgt_elem_ = p.elem;
    }
    if (p.kind == ReqKind::Index) {
      ++idx_inflight_;
      ++st_.idx_sub;  // diagnostic: claimed vs handed to the hierarchy
      if (st_.first_idx_issue == NONE) st_.first_idx_issue = cycle;
      st_.last_idx_issue = cycle;
    }
    if (p.kind == ReqKind::Target) {
      if (st_.first_tgt_issue == NONE) {
        st_.first_tgt_issue = cycle;
        st_.busy_at_first_tgt = mem_.dram.busy_cycles;
        st_.starved_at_first_tgt = mem_.dram.starved_cycles;
      }
      st_.last_tgt_issue = cycle;
      st_.busy_at_last_tgt = mem_.dram.busy_cycles;
    }
    unissued_.pop_front();
    n--;
  }
  st_.idx_out_max = std::max(st_.idx_out_max, idx_inflight_);
}

// The cycle advances the hierarchy, attributes this cycle's completions, then
// lets the engine make progress -- in that order, and that order is the contract.
// M9 splits the two halves so a driver that also runs the SIMT front-end can
// tick the hierarchy once and hand this engine only its own records; `tick`
// remains their composition, so the pure bulk path is unchanged (review E3).
void BulkEngine::tick(u64 cycle) {
  std::vector<u64> done;
  mem_.tick(cycle, done);
  attribute_done(cycle, done);
  progress(cycle);
}

// Responses only: consumes no cycle of its own and forms no request. The records
// must arrive in the order the hierarchy produced them, because `attribute` walks
// them in that order.
void BulkEngine::attribute_done(u64 cycle, const std::vector<u64>& done) {
  attribute(cycle, done);
}

// Everything after the hierarchy has advanced. Called exactly once per cycle by
// `tick` (pure bulk) or by the M9 driver (hybrid).
void BulkEngine::progress(u64 cycle) {
  if (active_) st_.ag_cycles++;
  start_next(cycle);
  if (windowed()) {
    win_tick(cycle);
  } else {
    generate(cycle);
    issue(cycle);
  }

  const u64 res = engine_residency();
  st_.buf_occ += res;
  st_.buf_max = std::max<u64>(st_.buf_max, res);
  st_.unissued_max = std::max<u64>(st_.unissued_max, unissued_.size());
  // The invariant this milestone is required to preserve, checked every cycle.
  if (res > slot_cap_) st_.residency_violations++;
}

// One residency slot per index line, in element order, never more than
// `idx_window` index lines in flight. Charged as real traffic: without the
// window the prefetch would take every slot and the dependent target stream
// could not start until the whole index array had been requested.
void BulkEngine::stream_index() {
  if (!active_ || run_.phase != 1) return;
  // M8 modes >= 1 replace this with the tiled acquisition unit below.
  if (idx_windowed()) return;
  const BulkDesc& d = run_.d;
  const u32 per = LINE_BYTES / d.index_bytes;
  u64 window = cfg_.idx_window ? cfg_.idx_window : cfg_.R;
  // A windowed engine must not let the index stream take the whole budget: the
  // target lines it feeds need room to be issued, so half each.
  if (windowed()) window = std::min<u64>(window, std::max<u64>(1, cfg_.R / 2));
  // Oracle diagnostic: the values are already here, so there is no index
  // traffic to stream and no fill to wait for.
  if (oracle_) return;
  while (run_.next_line_elem < d.count) {
    if (idx_resident_ >= window) break;
    if (!slot_space()) { st_.buf_full_stall++; break; }
    const u64 first = run_.next_line_elem;
    const u64 va = d.index_base + first * d.index_bytes;
    const u32 slot = alloc_and_enqueue(ReqKind::Index, va >> 6, 0, first);
    idx_pending_.insert(info_[slot].seq);
    st_.idx_lines_req++;
    idx_resident_++;
    run_.next_line_elem += per;
  }
}

// ---------------------------------------------------------------------------
// M8: tiled index acquisition
// ---------------------------------------------------------------------------
// Two bounded tiles of index-line descriptors. Ownership is explicit: exactly one
// tile is CONSUMING (address generation reads it) and the other is FILLING. A
// tile becomes consumable as soon as its first line lands, so generation starts
// after one index fill rather than after the whole array, and it is retired when
// its last element has been generated -- that is when its space is reusable.
//
// Readiness is per line (`ready`, set by the response carrying that line) and
// consumption is per line (`used`), so an out-of-order response marks only its
// own line, and with mode >= 2 a ready line is consumed while an earlier one is
// still outstanding. An element is only ever taken from the tile that owns it,
// which is what keeps index position and index value associated.
bool BulkEngine::idx_take(u64& pos, const u8** line, u32& off) {
  const BulkDesc& d = run_.d;
  const u32 per = LINE_BYTES / d.index_bytes;
  if (cfg_.idx_mode == 0) {
    const u64 chunk = (run_.gen_done / per) * per;
    auto it = idx_done_.find(chunk);
    if (it == idx_done_.end()) return false;
    pos = run_.gen_done;
    off = static_cast<u32>(pos - chunk);
    *line = mem_.line_bytes(mem_.pa_line_of_va_line(it->second));
    return true;
  }
  IdxRing& r = run_.ring;
  u32 pick = r.n;
  for (u32 i = 0; i < r.n; ++i) {
    const u32 s = (r.head + i) % IDX_SLOTS;
    if (r.used[s] >= r.cnt[s]) continue;  // fully generated already
    if (!r.ready[s]) {
      if (cfg_.idx_mode < 2) return false;  // in order: the head line, or nothing
      continue;                             // fine-grained: a later line may be ready
    }
    pick = i;
    break;
  }
  if (pick >= r.n) return false;
  const u32 s = (r.head + pick) % IDX_SLOTS;
  pos = r.elem_base[s] + r.used[s];
  off = r.used[s];
  *line = mem_.line_bytes(mem_.pa_line_of_va_line(r.va[s]));
  return true;
}

void BulkEngine::idx_consume(u64 pos) {
  if (!idx_windowed() || cfg_.idx_mode == 0) return;  // mode 0 retires by chunk
  IdxRing& r = run_.ring;
  for (u32 i = 0; i < r.n; ++i) {
    const u32 s = (r.head + i) % IDX_SLOTS;
    const u64 base = r.elem_base[s];
    if (pos < base || pos >= base + r.cnt[s]) continue;
    r.used[s]++;
    // A line releases its slot the moment its last element is generated, so the
    // far half can overwrite it while the near half is still in use.
    if (r.used[s] == r.cnt[s]) {
      while (r.n > 0 && r.used[r.head] >= r.cnt[r.head]) {
        r.head = (r.head + 1) % IDX_SLOTS;
        r.n--;
      }
    }
    return;
  }
}

void BulkEngine::idx_stream(u64 cycle) {
  (void)cycle;
  if (!idx_windowed() || oracle_) return;
  const BulkDesc& d = run_.d;
  const u32 per = LINE_BYTES / d.index_bytes;
  const u32 cap = std::min<u32>(cfg_.idx_buf_lines ? cfg_.idx_buf_lines : 1u, IDX_SLOTS / 2);
  u64 bound = cfg_.idx_window ? cfg_.idx_window : cfg_.R;
  bound = std::min<u64>(bound, std::max<u64>(1, cfg_.R / 2));
  if (cfg_.idx_mode >= 3) bound = std::min<u64>(bound, std::max<u32>(1, cfg_.idx_mshr));

  // One bounded ring of 2*cap slots, claimed from the tail and released per line
  // at the head by `idx_consume`. The far half fills while the near half is
  // consumed, and the ring wraps: that is the double buffering, at line rather
  // than buffer granularity (report §5.4).
  if (run_.idx_cap == 0) run_.idx_cap = std::min<u32>(cap, IDX_SLOTS / 2);
  const u32 limit = 2 * run_.idx_cap;
  IdxRing& r = run_.ring;
  while (r.n < limit && r.next_elem < d.count) {
    if (idx_resident_ >= bound) break;
    if (!slot_space()) { st_.idx_buf_stall++; break; }
    const u64 first = r.next_elem;
    const u64 va = d.index_base + first * d.index_bytes;
    const u32 slot = alloc_and_enqueue(ReqKind::Index, va >> 6, 0, first);
    idx_pending_.insert(info_[slot].seq);
    st_.idx_lines_req++;
    idx_resident_++;
    const u32 s = (r.head + r.n) % IDX_SLOTS;
    if (s == 0) st_.idx_tile_swaps++;  // the ring wrapped: A <-> B handover
    r.elem_base[s] = first;
    r.va[s] = va >> 6;
    r.cnt[s] = static_cast<u16>(std::min<u64>(per, d.count - first));
    r.used[s] = 0;
    r.ready[s] = 0;
    r.n++;
    r.next_elem += per;
  }
  u64 ready = 0;
  for (u32 i = 0; i < r.n; ++i) {
    const u32 s = (r.head + i) % IDX_SLOTS;
    if (r.ready[s] && r.used[s] < r.cnt[s]) ready++;
  }
  if (ready > st_.idx_ready_max) st_.idx_ready_max = ready;
}

// Diagnostic accessors on the acquisition ring. They read state and touch
// nothing; the drivers call them only while a cycle trace is attached.
u64 BulkEngine::idx_ready_elems() const {
  if (!idx_windowed() || cfg_.idx_mode == 0) return 0;
  const IdxRing& r = run_.ring;
  u64 n = 0;
  for (u32 i = 0; i < r.n; ++i) {
    const u32 s = (r.head + i) % IDX_SLOTS;
    if (r.ready[s] && r.used[s] < r.cnt[s]) n += r.cnt[s] - r.used[s];
  }
  return n;
}

u64 BulkEngine::idx_ready_lines() const {
  if (!idx_windowed() || cfg_.idx_mode == 0) return 0;
  const IdxRing& r = run_.ring;
  u64 n = 0;
  for (u32 i = 0; i < r.n; ++i) {
    const u32 s = (r.head + i) % IDX_SLOTS;
    if (r.ready[s] && r.used[s] < r.cnt[s]) ++n;
  }
  return n;
}

u64 BulkEngine::idx_ring_lines() const {
  return idx_windowed() && cfg_.idx_mode > 0 ? run_.ring.n : 0;
}

// Elements still to address-generate. Non-zero means the generator has work;
// together with `win_filling()` it separates "the feeder has nothing to give"
// from "the feeder has work but the window cannot take it" (drain backpressure)
// without inferring either from a stall counter.
u64 BulkEngine::ag_remaining() const {
  if (!active_ || run_.phase != 1) return 0;
  return run_.d.count > run_.gen_done ? run_.d.count - run_.gen_done : 0;
}

// The index stream's own issue port. It runs after the target port, so target
// lines always win the cycle and the index stream uses what is left -- that is
// the explicit arbitration between regular acquisition and irregular targets.
// Slots come from the same R pool, so this adds no outstanding-request capacity.
void BulkEngine::idx_issue(u64 cycle) {
  if (!idx_windowed() || cfg_.idx_mode < 3) return;
  u32 n = std::max<u32>(1, cfg_.eng_issue);
  while (n > 0 && !idx_q_.empty()) {
    const u32 slot = idx_q_.front();
    Pending& p = info_[slot];
    Req r;
    r.va_line = p.va_line;
    r.vpn = p.va_line >> cfg_.page_line_bits();
    r.core = ecore();
    r.slot = slot;
    r.seq = p.seq;
    r.bypass_l1 = true;
    r.client = 1;  // M11.3 diagnostic tag: engine index stream
    if (!mem_.submit(r, cycle)) {
      st_.issue_stall++;
      st_.idx_refuse++;
      break;
    }
    ++idx_inflight_;
    ++st_.idx_sub;  // diagnostic: claimed vs handed to the hierarchy
    if (st_.first_idx_issue == NONE) st_.first_idx_issue = cycle;
    st_.last_idx_issue = cycle;
    idx_q_.pop_front();
    n--;
  }
  if (idx_inflight_ > st_.idx_out_max) st_.idx_out_max = idx_inflight_;
}

// ---------------------------------------------------------------------------
// M6: windowed organization
// ---------------------------------------------------------------------------
// A window is a bounded set of pending elements organized by page. It is a
// *visibility* resource, not a residency resource:
//
//   W (batch_window)  elements a window may hold (logical visibility)
//   D (page_dir)      distinct pages a window may hold
//   R (R)             line requests in flight, shared with the SIMT arm
//
// An element waits in the window holding no slot. A unique line request takes
// one slot when it is issued and gives it back when the fill arrives, so the
// request-side invariant `unissued + inflight <= R` is unchanged and the two
// budgets are reported separately.
//
// Organization is charged, not free: admission visits one entry per element,
// sealing visits one more (the line-ordered pass), and one organizer consumes
// `org_lanes` visits per cycle. While it has work, admission stalls.
//
// The element->line mapping is exact on virtual addresses because a 64 B line
// never straddles a 4 KiB page, so two elements on one VA line are necessarily
// on one physical line. No aliasing can defeat it, and no translation is needed
// to group. Nothing here assumes a page maps to a fixed number of DRAM rows:
// the row effect is a consequence of the configured address map alone.

bool BulkEngine::win_admit_one(u64 va_line, u32 elem, u16 off, u8 width, u32 pos) {
  if (fill_ < 0) return false;
  Batch& b = bat_[static_cast<size_t>(fill_)];
  if (b.n_ent >= cfg_.batch_window) return false;
  if (fused()) {
    // One 64-bit word per element: the target line, the descriptor position it
    // belongs to and the in-line offset. `elem` is not stored -- it is
    // `elem_base + pos`, which the fanout reconstructs -- and `width` is a
    // descriptor field rather than an element field. That is what makes the
    // entry 8 B instead of 16 B, and what lets the grouping be a sort instead of
    // a permutation plus a per-line record.
    b.fent.push_back(fuse_ent(va_line, pos, off));
  } else {
    WinEnt e;
    e.va_line = va_line;
    e.elem = elem;
    e.off = off;
    e.width = width;
    b.ent.push_back(e);
  }
  b.n_ent++;
  st_.win_entries++;
  st_.org_backlog++;
  return true;
}

void BulkEngine::win_admit(u64 cycle) {
  if (fill_ < 0 || !active_ || run_.phase != 1) return;
  Batch& b = bat_[static_cast<size_t>(fill_)];
  if (b.state != 1) return;

  const BulkDesc& d = run_.d;
  const u32 per = LINE_BYTES / d.index_bytes;
  u32 budget = std::max<u32>(1, cfg_.ag_lanes);

  while (budget > 0 && run_.gen_done < d.count) {
    if (b.n_ent >= cfg_.batch_window) { seal_pending_ = true; return; }
    u64 pos = run_.gen_done;
    u32 ioff = 0;
    const u8* ib = nullptr;
    u64 v = 0;
    if (oracle_) {
      if (pos >= oracle_->size()) break;
      v = (*oracle_)[pos];
      st_.oracle_elems++;
    } else {
      // Fine-grained readiness: the unit hands back whichever element is ready,
      // which is this one unless an earlier line is still missing.
      if (!idx_take(pos, &ib, ioff)) {
        st_.idx_wait_stall++;
        // M9 diagnostic: attribute the readiness stall where it happens. Was the
        // target port able to issue at that instant? Read here, so "capacity was
        // free" is an observation made at the stall, not an inference from a
        // later snapshot.
        if (pool_) {
          const size_t f = pool_->free_count();
          st_.idx_wait_free_sum += f;
          if (f > tgt_reserve()) st_.idx_wait_cap++;
        }
        break;
      }
      ++st_.idx_consumed;  // one index element taken for address generation
      if (pos != run_.gen_done) st_.idx_ooo_gen++;
      v = load_le(ib + ioff * d.index_bytes, d.index_bytes);
    }
    const u64 addr = d.data_base + v * d.data_bytes;
    const u32 elem = d.elem_base + static_cast<u32>(pos);
    if (addr + d.data_bytes > VA_LIMIT) {
      st_.addr_faults++;  // reported, never silently dropped
      run_.gen_done++;
      idx_consume(pos);
      budget--;
      continue;
    }
    if (!win_admit_one(addr >> 6, elem, static_cast<u16>(addr & (LINE_BYTES - 1)), d.data_bytes,
                       static_cast<u32>(pos))) {
      seal_pending_ = true;
      return;
    }
    st_.tgt_addr_gen++;
    st_.idx_elems_done++;
    // M9 window management: the admission event. `last_admit` is read only by
    // the E3/E4 deadline test; the record is never read by a decision.
    b.last_admit = cycle;
    if (BulkStats::WinRec* rec = win_rec(fill_)) {
      if (rec->first == NONE) rec->first = cycle;
      rec->last = cycle;
    }
    run_.gen_done++;
    idx_consume(pos);
    budget--;
  }

  // Mode 0 retires completed-line entries here; the tiled path retires them in
  // `idx_consume` when the last element of a line is generated.
  run_.gen_cursor = run_.gen_done;
  if (oracle_ || cfg_.idx_mode > 0) return;
  const u64 cur = (run_.gen_done / per) * per;
  for (auto i2 = idx_done_.begin(); i2 != idx_done_.end();) {
    if (i2->first < cur) i2 = idx_done_.erase(i2);
    else break;
  }
}

// Close the window and build what issue needs: an exact page directory (line
// masks), the element->line grouping, and the issue order.
void BulkEngine::win_seal(u64 cycle, u8 why) {
  if (fill_ < 0) return;
  Batch& b = bat_[static_cast<size_t>(fill_)];
  if (b.n_ent == 0) {
    if (BulkStats::WinRec* rec = win_rec(fill_)) {
      rec->n = 0;
      rec->seal = cycle;
      rec->retire = cycle;
      rec->why = 3;  // allocated, sealed empty, discarded
      rec_ix_[static_cast<size_t>(fill_)] = 0xffffffffu;
    }
    bat_[static_cast<size_t>(fill_)] = Batch{};
    fill_ = -1;
    seal_pending_ = false;
    return;
  }

  if (fused()) {
    win_seal_fused(b);
  } else {
  // (1) Line-ordered view of the window: equal VA lines become adjacent, which
  //     is the whole deduplication mechanism.
  b.sorted.resize(b.n_ent);
  for (u32 i = 0; i < b.n_ent; ++i) b.sorted[i] = i;
  std::sort(b.sorted.begin(), b.sorted.end(), [&b](u32 x, u32 y) {
    if (b.ent[x].va_line != b.ent[y].va_line) return b.ent[x].va_line < b.ent[y].va_line;
    return x < y;
  });

  // (2) One request per distinct line (dedup) or one per element (no dedup).
  b.lines.clear();
  if (cfg_.dedup) {
    for (u32 i = 0; i < b.n_ent;) {
      const u64 vl = b.ent[b.sorted[i]].va_line;
      u32 j = i;
      while (j < b.n_ent && b.ent[b.sorted[j]].va_line == vl) ++j;
      LineReq lr;
      lr.va_line = vl;
      lr.first = i;
      lr.count = j - i;
      lr.seq = b.sorted[i];
      b.lines.push_back(lr);
      st_.dedup_merged += (j - i) - 1;
      i = j;
    }
  } else {
    for (u32 i = 0; i < b.n_ent; ++i) {
      LineReq lr;
      lr.va_line = b.ent[b.sorted[i]].va_line;
      lr.first = i;
      lr.count = 1;
      lr.seq = b.sorted[i];
      b.lines.push_back(lr);
    }
  }

  // (3) Issue order. Page order = ascending pages, ascending lines within a
  //     page (which `lines` already is). Without it, first-admission order.
  b.order.resize(b.lines.size());
  for (u32 i = 0; i < b.order.size(); ++i) b.order[i] = i;
  if (!cfg_.page_order)
    std::stable_sort(b.order.begin(), b.order.end(),
                     [&b](u32 x, u32 y) { return b.lines[x].seq < b.lines[y].seq; });
  }

  // (3b) M7: page-run cohorts. No storage is allocated on the legacy path, so
  //      a page_sched = 0 run is byte for byte the M6 machine.
  b.coh.clear();
  b.open_pos = 0;
  b.last_coh = 0xffffffffu;
  b.issued_n = 0;
  if (cfg_.page_sched) {
    const u32 n = std::min<u32>(cfg_.page_cohorts ? cfg_.page_cohorts : 1u, 16u);
    b.coh.assign(n, Batch::Cohort{});
    // 32 B per run: vpn (8) + span/cursor (8) + retry deadline (8) + state.
    st_.sched_bytes = static_cast<u64>(n) * 32;
  }

  // (3c) M13: one lookup per distinct line, now that the window's line set and
  //      its issue order are final and `issued_n` has been reset. Lines served
  //      from retained data (or attached to an outstanding request) are marked
  //      issued here, so the issue policies skip them exactly as they skip an
  //      already-issued line.
  if (reuse_on()) win_reuse_probe(b, static_cast<u32>(fill_), cycle);
  if (!cfg_.m13_dump.empty() && !fused()) {
    // Diagnostic: the window's distinct target lines, in ascending VA-line
    // order. Written after every decision for this window has been made, so it
    // cannot influence the run; the file is only ever read offline.
    if (std::FILE* f = std::fopen(cfg_.m13_dump.c_str(), "a")) {
      std::fprintf(f, "W %llu %u", static_cast<unsigned long long>(st_.batches), b.n_ent);
      for (const auto& lr : b.lines) std::fprintf(f, " %llu", static_cast<unsigned long long>(lr.va_line));
      std::fprintf(f, "\n");
      std::fclose(f);
    }
  }

  // (4) Page directory: exact line mask per resident page. Reported as the
  //     window's page-level occupancy; collisions only occur when the window
  //     touches more than `page_dir` distinct pages.
  b.dir_vpn.assign(cfg_.page_dir ? cfg_.page_dir : 1, NONE);
  b.dir_mask.assign(cfg_.page_dir ? cfg_.page_dir : 1, 0);
  b.n_pages = 0;
  for (u32 i = 0; i < b.n_ent; ++i) {
    const u64 vl = fused() ? fuse_line(b.fent[i]) : b.ent[i].va_line;
    const u64 vpn = vl >> cfg_.page_line_bits();
    const u32 s = static_cast<u32>(vpn % (cfg_.page_dir ? cfg_.page_dir : 1));
    if (b.dir_vpn[s] != vpn) {
      if (b.dir_vpn[s] != NONE) st_.dir_collisions++;
      b.dir_vpn[s] = vpn;
      b.dir_mask[s] = 0;
      b.n_pages++;
    }
    // M24-B: the mask is a 64-bit per-page diagnostic from the 4 KiB era. It is
    // only ever reported (never read by a decision), so for larger pages only
    // the first 64 lines are marked instead of widening every window's array.
    if (cfg_.page_line_bits() == 6) b.dir_mask[s] |= 1ull << (vl & 63u);
  }
  if (b.n_pages > st_.dir_occ_max) st_.dir_occ_max = b.n_pages;

  st_.org_backlog += b.n_ent;  // the line-ordered pass costs one visit per entry
  b.state = 2;
  st_.batches++;
  if (st_.first_seal == NONE) st_.first_seal = cycle;
  st_.last_seal = cycle;
  if (why == 0) st_.seals_full++;
  else if (why == 1) st_.seals_end++;
  else st_.seals_deadline++;
  if (BulkStats::WinRec* rec = win_rec(fill_)) {
    rec->n = b.n_ent;
    rec->seal = cycle;
    rec->why = why;
  }
  seal_pending_ = false;
  fill_ = -1;
}

// M8: while the tiled acquisition unit is active, the last `idx_reserve` slots
// belong to the index stream. Targets may not take them, which is what keeps
// index claims (and therefore address generation) from being starved by their
// own consumers. It partitions R; it does not enlarge it. One expression, used
// by the issue decision and by the readiness attribution, so they cannot drift.
u32 BulkEngine::tgt_reserve() const {
  return (idx_windowed() && cfg_.idx_mode > 0) ? std::min<u32>(cfg_.idx_reserve, cfg_.R / 4) : 0;
}

// One target line: take a residency slot, submit, book keep. 0 = issued,
// 1 = no slot free, 2 = the memory system refused it. Neither failure leaves a
// slot or a translation request allocated, so a caller may retry the line.
int BulkEngine::win_emit(Batch& b, u32 li, u64 cycle) {
  const u32 reserve = tgt_reserve();
  if (free_count() <= reserve) {
    st_.slot_stall++;
    // M9 diagnostic: attribute the refusal where it happens. On the shared pool
    // this records who actually held the slots, and whether the block was a
    // genuinely empty pool or the index stream's reserved share (0 < free <=
    // reserve). Neither counter influences the decision below it.
    if (pool_) {
      if (pool_->free_count() == 0) st_.tgt_refuse_noslot++;
      else st_.tgt_refuse_reserve++;
      st_.tgt_refuse_simt_held += pool_->held_by(0);
      st_.tgt_refuse_bulk_held += pool_->held_by(1);
      st_.tgt_refuse_free += pool_->free_count();
      st_.slot_stall_free_sum += pool_->free_count();
    }
    return 1;
  }
  LineReq& lr = b.lines[li];
  const u32 slot = alloc_slot();
  Req r;
  r.va_line = lr.va_line;
  r.vpn = lr.va_line >> cfg_.page_line_bits();
  r.core = ecore();
  r.slot = slot;
  r.seq = static_cast<u64>(iss_) << 32 | li;
  r.bypass_l1 = true;
  r.client = 3;  // M11.3 diagnostic tag: engine target stream
  if (!mem_.submit(r, cycle)) {
    free_slot(slot);
    st_.issue_stall++;
    return 2;
  }
  Pending p;
  p.kind = ReqKind::Target;
  p.va_line = lr.va_line;
  p.seq = r.seq;
  p.win_batch = static_cast<u32>(iss_);
  p.win_line = li;
  p.reuse_ent = lr.reuse_ent;  // M13: publish here when this fill lands
  info_[slot] = p;
  lr.slot = slot;
  lr.issued = true;
  b.pending++;
  b.issued_n++;
  st_.tgt_req_gen++;
  st_.tgt_req_issued++;
  st_.lines_issued++;
  if (st_.first_tgt_issue == NONE) {
    st_.first_tgt_issue = cycle;
    st_.busy_at_first_tgt = mem_.dram.busy_cycles;
    st_.starved_at_first_tgt = mem_.dram.starved_cycles;
  }
  st_.last_tgt_issue = cycle;
  st_.busy_at_last_tgt = mem_.dram.busy_cycles;
  // M9 double-buffer balance: the consumer's completing event, per window.
  if (BulkStats::WinRec* rec = win_rec(iss_)) rec->last_issue = cycle;
  return 0;
}

// Claim the next page run for a free cohort: the maximal span of `order` that
// maps to one page, starting where no cohort has claimed yet. Runs are claimed
// in sealed order, so pages are still touched in ascending order and the
// locality `page_order` was for is preserved; only the *waiting* is removed. A
// run degenerates to one line when `page_order` is off, which keeps the cohorts
// correct for any ordering. Nothing about the permutation, the page count or
// the DRAM row mapping is assumed: a run is "equal page numbers in a row".
bool BulkEngine::win_open_cohort(Batch& b) {
  if (b.open_pos >= b.order.size()) return false;
  Batch::Cohort* c = nullptr;
  for (auto& x : b.coh)
    if (!x.open) { c = &x; break; }
  if (!c) return false;
  const u32 start = b.open_pos;
  const u64 vpn = b.lines[b.order[start]].va_line >> cfg_.page_line_bits();
  u32 end = start + 1;
  while (end < b.order.size() &&
         (b.lines[b.order[end]].va_line >> cfg_.page_line_bits()) == vpn)
    ++end;
  c->pos = start;
  c->end = end;
  c->vpn = vpn;
  c->resume_at = 0;
  c->open = true;
  b.open_pos = end;
  st_.sched_opens++;
  st_.sched_scan += end - start;  // pointer steps; the sealed order already knows
  st_.sched_cycles++;             // installing a run costs the scheduler one cycle
  // M7 audit: charge it. The port loses this cycle's issue (bounded: one cycle
  // per page run, never more than one outstanding).
  sched_busy_ = std::min<u32>(sched_busy_ + 1, 1);
  return true;
}

// M7: issue from whichever open page run the dTLB will accept.
//
// The M6 policy keeps one cursor and gives up the whole cycle on the first
// refusal, so a page at its park limit blocks 32 lines for a 300-cycle walk. The
// scheduler keeps up to `page_cohorts` runs open; a refused run is deferred for
// `park_retry` cycles and the next run is tried in the same cycle. A refusal is
// therefore an *event* (counted in `sched_defer`), and only a cycle in which
// every open run is deferred is a *lost cycle* (`sched_idle`). No capacity moves:
// a run holds a residency slot only when its line is actually submitted, and the
// dTLB, walkers, MSHRs, caches and DRAM are untouched.
void BulkEngine::win_issue_sched(u64 cycle, Batch& b) {
  const u32 n = static_cast<u32>(b.coh.size());
  if (!n) return;
  if (sched_busy_) { sched_busy_ = 0; return; }  // the port is installing a run
  u32 budget = std::max<u32>(1, cfg_.eng_issue);
  while (budget > 0) {
    // Oldest ready run wins. Preference for the earliest page keeps each page's
    // lines together in the DRAM (page 0 drains at full rate while it can), and
    // a run is opened only when no open run can take the slot -- so extra pages
    // fill translation gaps instead of competing for the port. That is what
    // keeps row locality, and it is why this policy does not regress a
    // DRAM-bound workload (report §6.3).
    u32 idx = 0xffffffffu;
    for (u32 i = 0; i < n; ++i) {
      const Batch::Cohort& x = b.coh[i];
      if (!x.open || x.pos >= x.end) continue;
      if (x.resume_at > cycle) continue;
      idx = i;  // cohorts are claimed in seal order, so the first ready is the oldest
      break;
    }
    if (idx == 0xffffffffu) {
      // Nothing already open can take a line. Open one more page if the budget
      // allows; that is the whole scheduler. Only when nothing is left to open
      // is the cycle lost, and it is counted once per cycle, not per run.
      if (win_open_cohort(b)) continue;
      bool work = b.open_pos < b.order.size();
      for (const auto& x : b.coh)
        if (x.open && x.pos < x.end) work = true;
      if (work && slot_space()) st_.sched_idle++;
      break;
    }
    Batch::Cohort& c = b.coh[idx];
    // M13: a line served at seal has nothing to issue. Advancing costs nothing,
    // so it does not consume the cycle's issue budget.
    if (b.lines[b.order[c.pos]].issued) {
      c.pos++;
      if (c.pos >= c.end) { c.open = false; c.pos = c.end = 0; }
      continue;
    }
    st_.sched_probes++;
    const int rc = win_emit(b, b.order[c.pos], cycle);
    if (rc == 1) break;  // residency, not translation: nothing else can move
    if (rc == 2) {
      c.resume_at = cycle + std::max<u32>(1, cfg_.park_retry);
      st_.sched_defer++;
      continue;  // next pass opens another page or finds one that is ready
    }
    // Counted only when the port actually *moves* to another page and issues,
    // so it is a lower bound on the page-switch cost (report §8.7).
    if (b.last_coh != idx) { st_.sched_switches++; b.last_coh = idx; }
    c.pos++;
    if (c.pos >= c.end) { c.open = false; c.pos = c.end = 0; }
    budget--;
  }
}

// ---------------------------------------------------------------------------
// M15: the fused window representation (Config::win_fused)
// ---------------------------------------------------------------------------
// Design footprint per window buffer:
//   W x 8 B   elements, sorted by (line, position)
//   L x 4 B   one (start:16 | count:16) word per distinct line
//   L x 1 B   residency slot
//   L x 1 B   issued flag
// against the M6 layout's W x 16 B (WinEnt) + W x 4 B (sorted) + L x 32 B
// (LineReq) + L x 4 B (order). Everything those arrays encode is either reduced
// to the 32-bit run word or reconstructed by a scan of the sorted array, and the
// organizer work is the same single visit per entry the M6 seal already charges.
void BulkEngine::win_seal_fused(Batch& b) {
  // The sort is the deduplication: equal lines become adjacent. In place, over
  // 64-bit keys, so the permutation array the M6 layout materialises has no
  // counterpart here.
  b.fmeta.clear();
  if (cfg_.bulk_org == 4) {
    // M23-G3 diagnostic, not a policy: the counterfactual for the cache-line
    // mechanism. The sort stays -- it *is* the published order, so ordering is
    // held fixed -- and only the merge is dropped: one run per element, so a
    // line touched k times is requested k times instead of once.
    std::sort(b.fent.begin(), b.fent.end());
    b.fmeta.reserve(b.fent.size());
    for (u32 i = 0; i < b.fent.size(); ++i) b.fmeta.push_back((i << 16) | 1u);
  } else {
  std::sort(b.fent.begin(), b.fent.end());
  for (u32 i = 0; i < b.fent.size();) {
    const u64 vl = fuse_line(b.fent[i]);
    u32 j = i;
    while (j < b.fent.size() && fuse_line(b.fent[j]) == vl) ++j;
    b.fmeta.push_back((i << 16) | (j - i));
    st_.dedup_merged += (j - i) - 1;
    i = j;
  }
  }
  b.fslot.assign(b.fmeta.size(), 0xff);
  b.fflag.assign(b.fmeta.size(), 0);
  // M23-G3: the run array is now fixed, so the issue order can be chosen.
  win_order_fused(b);
}

// ---------------------------------------------------------------------------
// M23-G3: request organization
// ---------------------------------------------------------------------------
// The window's grouping is finished by the time this runs (a run is one cache
// line and all of its elements), so the only remaining decision is *which run
// goes out next*. Three orderings, one per mechanism the milestone asks for:
//
//   P1  reuse first   -- the line serving the most elements goes first. It is
//       the "cache-line" objective taken to its limit: retire the most
//       dependants per request. It ignores addresses entirely, which is why it
//       costs DRAM row locality; that trade is the point of measuring it.
//   P2  row first     -- ascending (row, bank, line) from the controller's own
//       map, so a bank's open row is reused by its own lines before the stream
//       moves on. Works off the *physical* line (pa_line_of_va_line), i.e. the
//       address the DRAM actually uses, not the virtual page.
//   P3  combined      -- row and bank first (the DRAM is the binding resource:
//       M23-G3 §4 shows 93-99 % of cycles are DRAM-service), then reuse inside
//       the cell, then line. The two objectives cannot both be primary, so the
//       one that costs throughput when lost is primary and the one that only
//       reorders retirement inside an already-batched cell is secondary.
//   P4  page, then row -- M23-G4B. Ascending *virtual page*, then (row, bank,
//       line) from the production map inside that page. The page sequence is
//       P0's, so the dTLB/page stream and the cohort unit (one 4 KiB page, 64
//       lines) are P0's as well; only the order *inside* a page changes. This
//       is what separates the two things P2 changed at once: the row order and
//       the 4x smaller issue unit. Nothing assumes a virtual page is a physical
//       row -- the row/bank keys come from the controller's own map, applied to
//       the physical line, exactly as in P2.
//
// Nothing here sees a request outside the sealed window: the sort is over this
// window's runs only, and a run that has not been admitted does not exist yet.
// The cost is charged: one organizer visit per run, on the same one-visit-per-
// element basis the seal already uses for its own sort.
u64 BulkEngine::run_line(const Batch& b, u32 li) const {
  return fuse_line(b.fent[b.fmeta[li] >> 16]);
}

void BulkEngine::win_order_fused(Batch& b) {
  b.ford.clear();
  const u32 org = cfg_.bulk_org;
  // M24-A: DRAM organization off. The sealed order -- ascending line, which is
  // P0's published issue order -- becomes the release order: `ford` stays empty,
  // so `fpos` is the identity and the pool's heap key is the stored position.
  // One branch, read once per seal; nothing else in the path changes.
  if (!cfg_.m24_dram_org) return;
  // P0: the sealed order *is* the order, so no storage and no organizer visits.
  // 4 (the no-grouping diagnostic) keeps that order too -- its whole point is to
  // hold ordering fixed and drop only the merge.
  if (org == 0 || org == 4) return;
  const u32 n = static_cast<u32>(b.fmeta.size());
  if (n < 2) return;
  b.ford.resize(n);
  for (u32 i = 0; i < n; ++i) b.ford[i] = i;
  std::vector<u32> bank(n, 0), row(n, 0), vpage(n, 0);
  if (org >= 2) {
    for (u32 i = 0; i < n; ++i)
      mem_.dram_bank_row(mem_.pa_line_of_va_line(run_line(b, i)), bank[i], row[i]);
  }
  if (org == 5) {
    // P4: the page key is the virtual page the access was issued against, the
    // same 64-line unit the cohort and the dTLB use. The row/bank keys are the
    // physical ones computed above, so a page whose PPN scatters it across rows
    // is still handled by the actual map rather than by the VA==PA assumption.
    for (u32 i = 0; i < n; ++i)
      vpage[i] = static_cast<u32>(run_line(b, i) >> 6);
  }
  if (org == 1) {
    std::stable_sort(b.ford.begin(), b.ford.end(), [&b](u32 x, u32 y) {
      const u32 cx = b.fmeta[x] & 0xffffu, cy = b.fmeta[y] & 0xffffu;
      return cx > cy;
    });
  } else if (org == 5) {
    std::stable_sort(b.ford.begin(), b.ford.end(), [&](u32 x, u32 y) {
      if (vpage[x] != vpage[y]) return vpage[x] < vpage[y];
      if (row[x] != row[y]) return row[x] < row[y];
      if (bank[x] != bank[y]) return bank[x] < bank[y];
      return x < y;  // sealed order == ascending line, so this is line order
    });
  } else {
    std::stable_sort(b.ford.begin(), b.ford.end(), [&](u32 x, u32 y) {
      if (row[x] != row[y]) return row[x] < row[y];
      if (bank[x] != bank[y]) return bank[x] < bank[y];
      if (org == 3) {  // combined: reuse order inside the open-row cell
        const u32 cx = b.fmeta[x] & 0xffffu, cy = b.fmeta[y] & 0xffffu;
        if (cx != cy) return cx > cy;
      }
      return x < y;  // sealed order == ascending line, so this is line order
    });
  }
  st_.org_runs += n;
  st_.org_backlog += n;
}

// A run belongs to the unit the cohort opened: a 4 KiB page (P0/P1, the sealed
// order) or a (bank,row) cell (P2/P3). A page is 2 rows x 2 banks x 16 lines in
// this map, so the two units differ only where the map aliases.
bool BulkEngine::same_run_unit(const Batch& b, u32 li, bool by_cell, u64 vpn, u32 bank,
                               u32 row) const {
  const u64 vl = run_line(b, li);
  if (!by_cell) return (vl >> 6) == vpn;
  u32 bk = 0, rw = 0;
  mem_.dram_bank_row(mem_.pa_line_of_va_line(vl), bk, rw);
  return rw == row && bk == bank;
}

// One target line from the fused array: the run word gives the element range and
// the line address is decoded from the run's first element.
// M23-G6F: choose the candidate the DRAM scheduler would prefer, out of the part
// of the ready buffer it is allowed to see (`Config::sched_vis`).
//
// The ranking is the *existing* FR-FCFS priority applied to candidates instead of
// to queued requests: a candidate whose (bank,row) is currently open outranks one
// that would activate a row, and inside a class the smallest window position wins
// -- which is plain FCFS on the P8 order, i.e. the policy the release already
// had. Nothing is admitted here: the chosen run is handed to `win_emit_fused`,
// so it still passes the residency gate, L1, L2 and the L2 MSHR file, and the
// DRAM still issues it under its own timing.
//
// When the configured visibility is smaller than the buffer, the compared set is
// the `sched_vis` smallest positions (a bounded reorder window over the P8
// order), not an arbitrary subset of the heap's internal array.
//
// `xlate` and `fflag` are consulted because an entry may have been released
// already and still sit in the heap until it reaches the top.
u64 BulkEngine::pick_candidate(Batch& b) {
  size_t scan = rel_ready_.size();
  const u64* keys = rel_ready_.data();
  if (cfg_.sched_vis && scan > cfg_.sched_vis) {
    scan = cfg_.sched_vis;
    g6f_scratch_.assign(rel_ready_.begin(), rel_ready_.end());
    std::nth_element(g6f_scratch_.begin(), g6f_scratch_.begin() + scan, g6f_scratch_.end());
    keys = g6f_scratch_.data();
  }
  if (g6f_stamp_.empty())
    g6f_stamp_.assign(static_cast<size_t>(cfg_.banks) * cfg_.rows, 0);
  const u32 gen = ++g6f_gen_;
  bool have = false;
  bool best_hit = false;
  u64 best_pos = 0;
  u64 cells = 0, hits = 0;
  for (size_t i = 0; i < scan; ++i) {
    const u64 pos = keys[i];
    const u32 li = run_at(b, static_cast<u32>(pos));
    if (b.fflag[li] & 1u) continue;            // already released; entry is stale
    u32 bk = 0, rw = 0;
    const bool hit = mem_.dram_row_open(rel_pa_[li], bk, rw);
    const size_t ci = static_cast<size_t>(bk) * cfg_.rows + rw;
    if (ci < g6f_stamp_.size() && g6f_stamp_[ci] != gen) { g6f_stamp_[ci] = gen; ++cells; }
    if (hit) ++hits;
    if (!have || (hit && !best_hit) || (hit == best_hit && pos < best_pos)) {
      have = true; best_hit = hit; best_pos = pos;
    }
  }
  ++st_.g6f_scan_calls;
  st_.g6f_scan_entries += scan;
  st_.g6f_vis_cells_sum += cells;
  st_.g6f_vis_hit_sum += hits;
  if (cells > st_.g6f_vis_cells_max) st_.g6f_vis_cells_max = cells;
  if (best_hit) ++st_.g6f_pick_hit; else ++st_.g6f_pick_frontier;
  return have ? best_pos : rel_ready_.front();
}

// ---------------------------------------------------------------------------
// M23-G6G: release by physical (bank,row) group
// ---------------------------------------------------------------------------
// G6F asked the buffer for requests that match a row the DRAM already has open
// and found almost none, because the buffer is strictly ahead of the open rows.
// G6G asks a different question: which requests share a *destination* cell, and
// can they be handed over as one burst so that the activation that opens the
// cell serves them all? The key is the physical (bank,row) the controller will
// use -- `dram_bank_row` on the translated physical line, never a virtual page.
//
// The ready buffer is already *sorted* by that key: a run's position in `ford`
// is its (row, bank, line) rank, so a cell's runs occupy a contiguous key range
// and every run of one cell sits at a known place. What the order cannot do is
// keep a cell together when its translations land out of order, and that is the
// only thing this policy changes: the release commits to one cell (G1) or to one
// per bank (G2) and drains it, instead of re-reading the global frontier on
// every pop.
//
//   sched_group = 1  one group: the cell with the most pending runs (ties: the
//                    smaller key, i.e. the more local cell) is drained in
//                    ascending line order before any other cell is chosen.
//   sched_group = 2  one group per bank: the same choice inside each bank, and
//                    the release alternates banks, so an activation in one bank
//                    overlaps service in the other.
//
// Determinism: the choice is a total order on (pending count, smallest key), and
// the keys are unique, so no tie is broken by heap-array order. The group only
// closes when the buffer no longer holds a pending run of its cell -- it never
// waits for lines that have not translated, so this cannot stall the release.
//
// Everything here is a metadata read (`rel_pa_`, `b.fflag`) that returns a
// position. The chosen run is still submitted by `win_emit_fused`, so the
// residency gate, the L1/L2 admission path, the L2 MSHR file and the DRAM's own
// FR-FCFS see exactly what they saw before. Candidate organization is not
// transaction concurrency: one request per release slot, as always.
u64 BulkEngine::pick_group(Batch& b) {
  const u32 nrows = cfg_.rows;
  const u32 nb = std::min<u32>(cfg_.banks, 8);
  if (g6g_stamp_.empty()) {
    if (cfg_.banks > 8)
      throw std::runtime_error("sched_group supports up to 8 banks");
    const size_t nc = static_cast<size_t>(cfg_.banks) * nrows;
    g6g_stamp_.assign(nc, 0);
    g6g_cnt_.assign(nc, 0);
    g6g_min_.assign(nc, 0);
    g6g_seen_.assign(nc, 0);
  }
  const u32 gen = ++g6g_gen_;
  const u32 slot = (cfg_.sched_group == 2 && nb) ? (b.g6g_rr % nb) : 0;
  const u32 open_cell = (cfg_.sched_group == 2) ? b.g6g_cell_b[slot] : b.g6g_cell;

  // (1) Keep draining the group this slot already owns, if the buffer still
  //     holds a pending run of it. This is what makes the group persistent: a
  //     line of the cell that translated late joins the burst in progress rather
  //     than starting a new one.
  if (open_cell != 0xffffffffu) {
    u64 best = 0;
    bool have = false;
    for (size_t i = 0; i < rel_ready_.size(); ++i) {
      const u64 pos = rel_ready_[i];
      const u32 li = run_at(b, static_cast<u32>(pos));
      if (b.fflag[li] & 1u) continue;  // released; the heap entry is stale
      u32 bk = 0, rw = 0;
      mem_.dram_bank_row(rel_pa_[li], bk, rw);
      if (bk * nrows + rw != open_cell) continue;
      if (!have || pos < best) { best = pos; have = true; }
    }
    ++st_.g6g_scan_calls;
    st_.g6g_scan_entries += rel_ready_.size();
    if (have) {
      b.g6g_sel_cell = open_cell;
      b.g6g_sel_slot = slot;
      return best;
    }
    // The cell has left the buffer: the group ends here, and what it actually
    // released is recorded now rather than assumed from the size at open.
    ++st_.g6g_groups_closed;
    st_.g6g_group_drain_sum += b.g6g_grp_n[slot];
    if (b.g6g_grp_n[slot] > st_.g6g_group_drain_max)
      st_.g6g_group_drain_max = b.g6g_grp_n[slot];
    b.g6g_grp_n[slot] = 0;
    if (cfg_.sched_group == 2) b.g6g_cell_b[slot] = 0xffffffffu;
    else b.g6g_cell = 0xffffffffu;
  }

  // (2) Open a new group: count the pending runs of every destination cell.
  //     M23-G6H: this pass enumerates `rel_ready_` in full -- every translated run
  //     the buffer holds -- and the two counters below prove it. There is no
  //     prefix, no cursor and no limit: `exam` is `ready.size()`, and the number
  //     of live entries it found must equal the sum of the per-cell counts.
  g6g_cells_.clear();
  u64 elig = 0, stale = 0, cnt_sum = 0;
  for (size_t i = 0; i < rel_ready_.size(); ++i) {
    const u64 pos = rel_ready_[i];
    const u32 li = run_at(b, static_cast<u32>(pos));
    if (b.fflag[li] & 1u) { ++stale; continue; }   // released; the heap keeps it
    ++elig;
    u32 bk = 0, rw = 0;
    mem_.dram_bank_row(rel_pa_[li], bk, rw);
    const u32 c = bk * nrows + rw;
    if (g6g_stamp_[c] != gen) {
      g6g_stamp_[c] = gen;
      g6g_cnt_[c] = 1;
      ++cnt_sum;
      g6g_min_[c] = pos;
      g6g_cells_.push_back(c);
    } else {
      ++g6g_cnt_[c];
      ++cnt_sum;
      if (pos < g6g_min_[c]) g6g_min_[c] = pos;
    }
  }
  ++st_.g6g_scan_calls;
  st_.g6g_scan_entries += rel_ready_.size();
  {
    const u64 exam = rel_ready_.size();
    const u64 untr = static_cast<u64>(b.fmeta.size()) - b.g6b_tr_done;
    ++st_.g6h_decisions;
    st_.g6h_exam_sum += exam;
    if (exam > st_.g6h_exam_max) st_.g6h_exam_max = exam;
    st_.g6h_elig_sum += elig;
    if (elig > st_.g6h_elig_max) st_.g6h_elig_max = elig;
    st_.g6h_stale_sum += stale;
    st_.g6h_untr_sum += untr;
    if (untr > st_.g6h_untr_max) st_.g6h_untr_max = untr;
    st_.g6h_groups_sum += g6g_cells_.size();
    if (g6g_cells_.size() > st_.g6h_groups_max) st_.g6h_groups_max = g6g_cells_.size();
    st_.g6h_considered_sum += g6g_cells_.size();
    if (exam != elig + stale || elig != cnt_sum) ++st_.g6h_inv_viol;
  }

  // Densest cell wins; a tie goes to the smaller key. G2 restricts the choice to
  // the bank whose turn it is and only falls back to the other bank when that
  // bank has nothing pending, so a bank is never starved by the other's backlog.
  // G6H's arm 3 skips both rules and uses P2's key order instead.
  u32 best_cell = 0xffffffffu, best_cnt = 0;
  if (cfg_.sched_group == 3) {
    // M23-G6H: G3 P2's own rule. P2 issues the sealed window's runs in ascending
    // (row, bank, line), so a run's rank in `ford` -- the cell's smallest rank
    // `g6g_min_` -- is exactly the P2 key of the cell. The smallest key wins; no
    // density, no bank rotation, no other tie-break is consulted. Applying this
    // to the translated window is the whole correction: same key, same order,
    // same population scope as G6G's arms, only the choice differs.
    for (u32 c : g6g_cells_)
      if (best_cell == 0xffffffffu || g6g_min_[c] < g6g_min_[best_cell]) best_cell = c;
    if (best_cell != 0xffffffffu) best_cnt = g6g_cnt_[best_cell];
  }
  for (u32 pass = 0; cfg_.sched_group != 3 &&
                      pass < ((cfg_.sched_group == 2 && nb > 1) ? nb : 1u); ++pass) {
    const u32 want = (cfg_.sched_group == 2) ? ((slot + pass) % nb) : 0;
    for (u32 c : g6g_cells_) {
      if (cfg_.sched_group == 2 && (c / nrows) != want) continue;
      if (g6g_cnt_[c] > best_cnt ||
          (g6g_cnt_[c] == best_cnt &&
           (best_cell == 0xffffffffu || g6g_min_[c] < g6g_min_[best_cell]))) {
        best_cell = c;
        best_cnt = g6g_cnt_[c];
      }
    }
    if (best_cell != 0xffffffffu) break;
  }
  // Unreachable in practice: every pending entry has a cell, and the caller
  // already removed a stale front, so at least one cell exists.
  if (best_cell == 0xffffffffu) return rel_ready_.front();

  st_.g6h_deferred_sum += elig - best_cnt;  // eligible, but not in the chosen group
  if (cfg_.sched_group == 2) b.g6g_cell_b[slot] = best_cell;
  else b.g6g_cell = best_cell;
  b.g6g_grp_n[slot] = 0;
  b.g6g_sel_cell = best_cell;
  b.g6g_sel_slot = slot;
  ++st_.g6g_groups_opened;
  st_.g6g_group_size_sum += best_cnt;
  if (best_cnt > st_.g6g_group_size_max) st_.g6g_group_size_max = best_cnt;
  const u32 hb = best_cnt >= 16 ? 5u
                               : (best_cnt >= 9 ? 4u
                                                : (best_cnt >= 5 ? 3u
                                                                 : (best_cnt >= 3 ? 2u
                                                                                  : best_cnt - 1u)));
  ++st_.g6g_group_hist[hb];
  // A cell that opens again in the same window was drained and refilled: the
  // translation stream delivered its lines in more than one burst.
  if (g6g_seen_[best_cell] == g6g_wgen_) ++st_.g6g_revisit;
  else g6g_seen_[best_cell] = g6g_wgen_;
  return g6g_min_[best_cell];
}

// The released run's own (bank,row), as the release loop computed it. Counts the
// realized burst length of the group and the interleaving the sequence produced.
void BulkEngine::note_group_release(Batch& b, u32 bank, u32 row) {
  const u32 c = bank * cfg_.rows + row;
  if (b.g6g_any) {
    if (c == b.g6g_last_cell) ++st_.g6g_same_cell;
    else ++st_.g6g_switch_cell;
    if (bank != b.g6g_last_bank) ++st_.g6g_bank_alt;
  }
  b.g6g_any = 1;
  b.g6g_last_cell = c;
  b.g6g_last_bank = bank;
  if (c != b.g6g_sel_cell) ++st_.g6g_sel_mismatch;
  if (b.g6g_sel_slot < 8) ++b.g6g_grp_n[b.g6g_sel_slot];
  ++st_.g6g_group_runs;
  if (cfg_.sched_group == 2 && cfg_.banks) b.g6g_rr = (b.g6g_rr + 1) % cfg_.banks;
}

int BulkEngine::win_emit_fused(Batch& b, u32 li, u64 cycle) {
  const u32 reserve = tgt_reserve();
  if (free_count() <= reserve) {
    st_.slot_stall++;
    if (pool_) {
      if (pool_->free_count() == 0) st_.tgt_refuse_noslot++;
      else st_.tgt_refuse_reserve++;
      st_.tgt_refuse_simt_held += pool_->held_by(0);
      st_.tgt_refuse_bulk_held += pool_->held_by(1);
      st_.tgt_refuse_free += pool_->free_count();
      st_.slot_stall_free_sum += pool_->free_count();
    }
    return 1;
  }
  const u64 vl = fuse_line(b.fent[b.fmeta[li] >> 16]);
  const u32 slot = alloc_slot();
  Req r;
  r.va_line = vl;
  r.vpn = vl >> 6;
  r.core = ecore();
  r.slot = slot;
  r.seq = static_cast<u64>(iss_) << 32 | li;
  r.bypass_l1 = true;
  r.client = 3;  // M11.3 diagnostic tag: engine target stream
  if (cfg_.bulk_org == 6 || cfg_.bulk_org == 7 || cfg_.bulk_org == 8) {
    // M23-G6A phase 2 / M23-G6B (G6C) release: this run was translated earlier and its
    // physical line has been buffered since. The hierarchy installs it directly,
    // and the lifetime counters still start at the translation submission, so
    // the translation half of the latency is not erased from the record.
    r.pretranslated = true;
    r.pa_line = rel_pa_[li];
    r.gen = rel_gen_[li];
    r.tlb_at = rel_done_[li];
  }
  if (!mem_.submit(r, cycle)) {
    free_slot(slot);
    st_.issue_stall++;
    return 2;
  }
  Pending p;
  p.kind = ReqKind::Target;
  p.va_line = vl;
  p.seq = r.seq;
  p.win_batch = static_cast<u32>(iss_);
  p.win_line = li;
  p.reuse_ent = 0xffffffffu;  // the fused layout is not combined with reuse_dir
  info_[slot] = p;
  b.fslot[li] = static_cast<u8>(slot);
  b.fflag[li] |= 1u;
  b.pending++;
  b.issued_n++;
  st_.tgt_req_gen++;
  st_.tgt_req_issued++;
  st_.lines_issued++;
  if (cfg_.bulk_org == 6 || cfg_.bulk_org == 7 || cfg_.bulk_org == 8) {
    // How long a translated run waited for its turn at the DRAM: the release
    // cycle minus the cycle its translation completed.
    const u64 w = cycle - rel_done_[li];
    if (cfg_.bulk_org != 6) {
      st_.g6b_tr_wait_dram += w;
      if (w > st_.g6b_tr_wait_max) st_.g6b_tr_wait_max = w;
      st_.g6b_runs_released++;
    } else {
      st_.g6a_tr_wait_dram += w;
      if (w > st_.g6a_tr_wait_max) st_.g6a_tr_wait_max = w;
      st_.g6a_runs_released++;
    }
  }
  if (st_.first_tgt_issue == NONE) {
    st_.first_tgt_issue = cycle;
    st_.busy_at_first_tgt = mem_.dram.busy_cycles;
    st_.starved_at_first_tgt = mem_.dram.starved_cycles;
  }
  st_.last_tgt_issue = cycle;
  st_.busy_at_last_tgt = mem_.dram.busy_cycles;
  if (BulkStats::WinRec* rec = win_rec(iss_)) rec->last_issue = cycle;
  return 0;
}

bool BulkEngine::win_open_cohort_fused(Batch& b) {
  const u32 nrun = static_cast<u32>(b.fmeta.size());
  if (b.open_pos >= nrun) return false;
  Batch::Cohort* c = nullptr;
  for (auto& x : b.coh)
    if (!x.open) { c = &x; break; }
  if (!c) return false;
  const u32 start = b.open_pos;
  // M23-G3: the unit a cohort owns is the organization's run. P0/P1 keep the
  // published page run; P2/P3 own one (bank,row) cell, whose lines are 16-32
  // consecutive runs of the row-first order. M23-G4B: P4 owns a page again,
  // even though it orders by row inside it -- the page is 64 lines, the cell is
  // 16, and the cohort is also the issue unit, which is exactly the difference
  // this policy is built to keep.
  const u64 vl0 = run_line(b, run_at(b, start));
  // M23-G6A releases its buffered addresses through this same scheduler, so it
  // owns the same (bank,row) cell P2 does: the organization is the existing one,
  // only the translation moved in front of it.
  const bool by_cell = cfg_.bulk_org == 2 || cfg_.bulk_org == 3 || cfg_.bulk_org == 6;
  const u64 vpn = vl0 >> 6;
  u32 bank0 = 0, row0 = 0;
  if (by_cell) mem_.dram_bank_row(mem_.pa_line_of_va_line(vl0), bank0, row0);
  u32 end = start + 1;
  while (end < nrun && same_run_unit(b, run_at(b, end), by_cell, vpn, bank0, row0)) ++end;
  c->pos = start;
  c->end = end;
  c->vpn = vpn;
  c->resume_at = 0;
  c->open = true;
  b.open_pos = end;
  st_.sched_opens++;
  st_.sched_scan += end - start;
  st_.sched_cycles++;
  sched_busy_ = std::min<u32>(sched_busy_ + 1, 1);
  return true;
}

void BulkEngine::win_issue_sched_fused(u64 cycle, Batch& b) {
  const u32 n = static_cast<u32>(b.coh.size());
  if (!n) return;
  if (sched_busy_) { sched_busy_ = 0; return; }
  u32 budget = std::max<u32>(1, cfg_.eng_issue);
  while (budget > 0) {
    u32 idx = 0xffffffffu;
    for (u32 i = 0; i < n; ++i) {
      const Batch::Cohort& x = b.coh[i];
      if (!x.open || x.pos >= x.end) continue;
      if (x.resume_at > cycle) continue;
      idx = i;
      break;
    }
    if (idx == 0xffffffffu) {
      if (win_open_cohort_fused(b)) continue;
      bool work = b.open_pos < b.fmeta.size();
      for (const auto& x : b.coh)
        if (x.open && x.pos < x.end) work = true;
      if (work && slot_space()) st_.sched_idle++;
      break;
    }
    Batch::Cohort& c = b.coh[idx];
    if (b.fflag[run_at(b, c.pos)] & 1u) {
      c.pos++;
      if (c.pos >= c.end) { c.open = false; c.pos = c.end = 0; }
      continue;
    }
    st_.sched_probes++;
    const int rc = win_emit_fused(b, run_at(b, c.pos), cycle);
    if (rc == 1) break;
    if (rc == 2) {
      c.resume_at = cycle + std::max<u32>(1, cfg_.park_retry);
      st_.sched_defer++;
      continue;
    }
    if (b.last_coh != idx) { st_.sched_switches++; b.last_coh = idx; }
    c.pos++;
    if (c.pos >= c.end) { c.open = false; c.pos = c.end = 0; }
    budget--;
  }
}

// ---------------------------------------------------------------------------
// M23-G6A: translation first, DRAM second
// ---------------------------------------------------------------------------
// One window, two strictly ordered phases.
//
// Phase 1 submits one translation probe per run of the sealed window, in the
// order the window stores them (ascending line), and nothing else. A probe is a
// translation-only request: it stops the cycle the dTLB yields the mapping, so
// it can never reach the cache hierarchy or the DRAM, and the physical line it
// produced is buffered with the cycle it arrived. The refusal that bounds the
// phase is the dTLB's own -- every MSHR slot taken by another VPN, or this VPN's
// MSHR already at `dtlb_parked` -- so the phase adds no outstanding-request
// capacity beyond the published translation model.
//
// Phase 2 hands exactly those runs to the unchanged P2 scheduler: same (bank,row)
// cohort unit, same deferral, same issue budget, same DRAM controller. The only
// difference from P2 is that no run translates again: it carries the physical
// line it was given.
//
// The barrier is structural, not timed. A window's targets cannot be released
// before its own last translation returns, and the engine adopts a window for
// issue only when none is issuing (`iss_ < 0`), so no other window can be
// translating while this one drains. Nothing overlaps the two phases.
void BulkEngine::win_issue_g6a(u64 cycle, Batch& b) {
  const u32 n = static_cast<u32>(b.fmeta.size());
  if (b.g6a_phase == 0) {
    b.g6a_phase = 1;
    b.tr_start = cycle;
    b.busy_tr_start = mem_.dram.busy_cycles;
    b.tr_next = b.tr_done = 0;
    rel_pa_.assign(n, 0);
    rel_tr_.assign(n, 0);
    rel_gen_.assign(n, 0);
    rel_done_.assign(n, 0);
    st_.g6a_windows++;
    st_.g6a_pa_capacity = cfg_.g6a_pa_capacity;
    st_.g6a_pa_need_max = std::max<u64>(st_.g6a_pa_need_max, n);
    st_.g6a_pa_bytes = static_cast<u64>(cfg_.g6a_pa_capacity) * 16u;
    if (n > cfg_.g6a_pa_capacity)
      throw std::runtime_error("g6a: window needs " + std::to_string(n) +
                               " PA entries, buffer capacity is " +
                               std::to_string(cfg_.g6a_pa_capacity));
  }

  if (b.g6a_phase == 1) {
    st_.g6a_tr_cycles++;
    // (a) collect this cycle's completed translations
    MemorySystem::Xlate x;
    while (mem_.take_xlate(x)) {
      const u32 li = static_cast<u32>(x.seq & 0xffffffffu);
      if (li >= n || rel_tr_[li]) continue;
      rel_pa_[li] = x.pa;
      rel_done_[li] = x.cycle;
      rel_tr_[li] = 1;
      b.tr_done++;
      st_.g6a_tr_done++;
      b.tr_last_done = cycle;
    }
    // (b) submit more probes, bounded by the dTLB and by nothing else
    u32 budget = std::max<u32>(1, cfg_.eng_issue);
    while (budget > 0 && b.tr_next < n) {
      const u32 li = b.tr_next;
      const u64 vl = run_line(b, li);
      if (free_count() <= tgt_reserve()) { ++st_.tr_slot_stall; break; }
      const u32 pslot = alloc_slot();
      Req r;
      r.va_line = vl;
      r.vpn = vl >> 6;
      r.core = ecore();
      r.slot = pslot;  // M27-P3: a translation holds a residency slot
      r.seq = li;
      r.bypass_l1 = true;
      r.client = 2;  // PTE: the probe is translation work and nothing else
      r.xlate_only = true;
      if (!mem_.submit(r, cycle)) {
        free_slot(pslot);
        st_.g6a_tr_stall++;
        break;
      }
      info_[pslot] = Pending{};
      info_[pslot].kind = ReqKind::Xlate;
      rel_gen_[li] = cycle;
      st_.g6a_tr_reqs++;
      b.tr_next++;
      budget--;
    }
    st_.g6a_pa_occ_max = std::max<u64>(st_.g6a_pa_occ_max, b.tr_next);
    if (b.tr_done < n) return;  // hard barrier: nothing issues during phase 1
    b.g6a_phase = 2;
    b.dr_start = cycle;
    st_.g6a_barrier_cycles += cycle - b.tr_last_done;
    // Bank-time the DRAM spent idle while this window translated: the phase is
    // 2 banks x cycles, minus the bank-cycles that were already busy (with the
    // index stream, if it had work).
    const u64 span = 2ull * (cycle - b.tr_start);
    const u64 busy = mem_.dram.busy_cycles - b.busy_tr_start;
    st_.g6a_dram_idle_tr += span > busy ? span - busy : 0;
  }

  st_.g6a_dr_cycles++;
  win_issue_sched_fused(cycle, b);
}

// ---------------------------------------------------------------------------
// M23-G6B: streaming translation / DRAM decoupling
// ---------------------------------------------------------------------------
// Translation and DRAM release run in the same cycle, every cycle. Three pieces,
// and the only thing between them is the ready buffer:
//
//  1. Translation admission walks the window's runs in *stored* order (ascending
//     line, so a page's 64 runs stay together) and submits one translation-only
//     probe at a time, bounded by the dTLB's own MSHRs and by the ready buffer's
//     capacity. It never looks at the DRAM order -- it cannot, the addresses are
//     not translated yet.
//  2. A probe that completes drops its run into `ready`, keyed by its position
//     in `ford` = its physical (row, bank, line) key. The buffer therefore holds
//     whatever pages have finished translating, in whatever order they finished.
//  3. The DRAM scheduler pops the *smallest* key available, i.e. the most local
//     translated line in the P2 order, and releases it through the unchanged
//     `win_emit_fused` path. Requests behind it keep translating.
//
// Because translation admission never waits for the buffer to drain and the
// release never waits for the window to finish translating, the two overlap by
// ---------------------------------------------------------------------------
// M23-G6I: continuous cross-page pipeline instrumentation
// ---------------------------------------------------------------------------
// The pipeline measured here is G6B/G6C's, unchanged: translation admission walks
// every page run of the window through `page_cohorts` open descriptors, the
// release drains the one ready pool, and the only link between the two stages is
// the capacity rule in step (2) of `win_issue_g6b`. Nothing below feeds a
// decision -- both functions return immediately unless Config::g6i_diag is set,
// and the counters they write are read only by the exporter.
//
// `note_g6i_release` records, per release, the physical cell's previous release
// cycle (so the same-row revisit gap is measured in cycles, not inferred) and the
// virtual page the released run came from (so "the organizer served a different
// page than the last release" is an event count, not a claim).
void BulkEngine::note_g6i_release(u64 cycle, const Batch& b, u32 li, u32 bank,
                                  u32 row) {
  const size_t nc = static_cast<size_t>(cfg_.banks) * cfg_.rows;
  if (g6i_last_rel_.empty()) g6i_last_rel_.assign(nc, NONE);
  const size_t c = static_cast<size_t>(bank) * cfg_.rows + row;
  if (c < nc) {
    if (g6i_last_rel_[c] != NONE) {
      const u64 gap = cycle - g6i_last_rel_[c];
      ++st_.g6i_gap_n;
      st_.g6i_gap_sum += gap;
      if (gap > st_.g6i_gap_max) st_.g6i_gap_max = gap;
    }
    g6i_last_rel_[c] = cycle;
  }
  const u64 vpn = run_line(b, li) >> 6;
  if (g6i_last_pub_vpn_ != NONE) {
    if (g6i_last_pub_vpn_ == vpn) ++st_.g6i_rel_same_vpn;
    else ++st_.g6i_rel_vpn_switch;
  }
  g6i_last_pub_vpn_ = vpn;
}

// One sample of the ready pool: how many entries it holds, how many distinct
// virtual pages produced them, how many distinct physical (bank,row) cells they
// target, and how many of those cells are fed by more than one page at the same
// time -- the direct evidence that page boundaries are not scheduling boundaries.
// Sampled, not exact: at most one scan per `g6i_sample_period` cycles, because a
// per-release scan of a 4 096-entry pool is exactly the cost the report says a
// real implementation must avoid.
void BulkEngine::g6i_sample(u64 cycle, const Batch& b) {
  const u32 period = cfg_.g6i_sample_period ? cfg_.g6i_sample_period : 512;
  if (g6i_last_sample_ != NONE && cycle - g6i_last_sample_ < period) return;
  g6i_last_sample_ = cycle;
  const size_t nc = static_cast<size_t>(cfg_.banks) * cfg_.rows;
  const u32 slots = 1u << 14;  // 16 384 VPN slots: load <= 50 % at a full pool
  if (g6i_vpn_key_.empty()) {
    g6i_vpn_key_.assign(slots, 0);
    g6i_vpn_gen_.assign(slots, 0);
    g6i_cell_gen_.assign(nc, 0);
    g6i_cell_vpn_.assign(nc, 0);
    g6i_cell_n_.assign(nc, 0);
    g6i_cell_multi_.assign(nc, 0);
  }
  const u32 gen = ++g6i_gen_;
  u64 vpns = 0, cells = 0, xcells = 0, xentries = 0;
  for (size_t i = 0; i < rel_ready_.size(); ++i) {
    const u32 li = run_at(b, static_cast<u32>(rel_ready_[i]));
    const u64 vpn = run_line(b, li) >> 6;
    const u32 key32 = static_cast<u32>(vpn ^ (vpn >> 32));
    size_t h = (key32 * 2654435761u) >> (32 - 14);
    u32 probe = 0;
    for (;;) {
      if (g6i_vpn_gen_[h] != gen) {
        g6i_vpn_gen_[h] = gen;
        g6i_vpn_key_[h] = key32;
        ++vpns;
        break;
      }
      if (g6i_vpn_key_[h] == key32) break;
      h = (h + 1) & (slots - 1);
      if (++probe > st_.g6i_probe_max) st_.g6i_probe_max = probe;
      if (probe > 64) break;  // counted, never silently merged
    }
    u32 bk = 0, rw = 0;
    mem_.dram_bank_row(rel_pa_[li], bk, rw);
    const size_t c = static_cast<size_t>(bk) * cfg_.rows + rw;
    if (c >= nc) continue;
    if (g6i_cell_gen_[c] != gen) {
      g6i_cell_gen_[c] = gen;
      g6i_cell_vpn_[c] = key32;
      g6i_cell_n_[c] = 1;
      g6i_cell_multi_[c] = 0;
      ++cells;
    } else {
      ++g6i_cell_n_[c];
      if (!g6i_cell_multi_[c] && g6i_cell_vpn_[c] != key32) {
        g6i_cell_multi_[c] = 1;
        ++xcells;
        xentries += g6i_cell_n_[c];  // this entry plus the ones already counted
      } else if (g6i_cell_multi_[c]) {
        ++xentries;
      }
    }
  }
  ++st_.g6i_samples;
  st_.g6i_samp_pool_sum += rel_ready_.size();
  if (rel_ready_.size() > st_.g6i_samp_pool_max) st_.g6i_samp_pool_max = rel_ready_.size();
  st_.g6i_vpn_sum += vpns;
  if (vpns > st_.g6i_vpn_max) st_.g6i_vpn_max = vpns;
  st_.g6i_cells_sum += cells;
  if (cells > st_.g6i_cells_max) st_.g6i_cells_max = cells;
  st_.g6i_xpage_cells_sum += xcells;
  if (xcells > st_.g6i_xpage_cells_max) st_.g6i_xpage_cells_max = xcells;
  st_.g6i_xpage_entries_sum += xentries;
  const u64 untr = static_cast<u64>(b.fmeta.size()) - b.g6b_tr_done;
  st_.g6i_untr_sum += untr;
  if (untr > st_.g6i_untr_max) st_.g6i_untr_max = untr;
  u32 pgs = 0;
  for (const auto& x : b.coh)
    if (x.open && x.pos < x.end) ++pgs;
  st_.g6i_pages_open_sum += pgs;
  if (pgs > st_.g6i_pages_open_max) st_.g6i_pages_open_max = pgs;
}

// construction. The capacity is the entire coupling: a full buffer backpressures
// translation, which is the one place the two can constrain each other. There is
// no deadlock: a non-empty buffer always yields a releasable run (the pop either
// issues it or the residency budget is full, and that budget drains), and a run
// only enters the buffer after its translation completes.
void BulkEngine::win_issue_g6b(u64 cycle, Batch& b) {
  const u32 n = static_cast<u32>(b.fmeta.size());
  if (!b.g6b_init) {
    b.g6b_init = 1;
    b.g6b_tr_start = cycle;
    b.g6b_tr_next = b.g6b_tr_done = 0;
    rel_pa_.assign(n, 0);
    rel_tr_.assign(n, 0);
    rel_gen_.assign(n, 0);
    rel_done_.assign(n, 0);
    // M28-D: these two are engine state now, so the adopted window must find
    // them in the state a fresh window had. `rel_ready_` was empty by
    // construction on every adoption (a window is only adopted with `state == 0`,
    // i.e. right after `Batch{}`), and the same runs are pushed in the same order
    // with the same keys below. `rel_pos_` is the inverse of `ford`, and the
    // production default (`m24_dram_org = 0`) leaves `ford` empty -- where that
    // inverse is the identity, so nothing is allocated and `run_key` returns the
    // stored position. With the organizer on it holds exactly the old `fpos`.
    rel_ready_.clear();
    if (b.ford.empty()) {
      rel_pos_.clear();
    } else {
      rel_pos_.assign(n, 0);
      for (u32 i = 0; i < n; ++i) rel_pos_[b.ford[i]] = i;
    }
    // M23-G6G: no group is open when a window is adopted, and the revisit test
    // is per window (`g6g_wgen_`), so a cell drained in an earlier window is not
    // reported as a revisit of this one.
    b.g6g_cell = 0xffffffffu;
    for (u32 i = 0; i < 8; ++i) { b.g6g_cell_b[i] = 0xffffffffu; b.g6g_grp_n[i] = 0; }
    b.g6g_rr = 0;
    b.g6g_sel_cell = 0xffffffffu;
    b.g6g_sel_slot = 0;
    b.g6g_last_cell = 0xffffffffu;
    b.g6g_last_bank = 0xffffffffu;
    b.g6g_any = 0;
    // M23-G6I: the page-switch test restarts with each window, so the first
    // release of a window is the one release that has no predecessor.
    g6i_last_pub_vpn_ = NONE;
    m24_prev_pos_ = NONE;  // M24-A: ditto for the release-order test
    ++g6g_wgen_;
    st_.g6b_windows++;
    st_.g6b_capacity = cfg_.g6b_pa_capacity;
    st_.g6b_pa_bytes = static_cast<u64>(cfg_.g6b_pa_capacity) * 16u;
  }

  // (1) translations that landed this cycle join the ready buffer
  MemorySystem::Xlate x;
  while (mem_.take_xlate(x)) {
    const u32 li = static_cast<u32>(x.seq & 0xffffffffu);
    if (li >= n || rel_tr_[li]) continue;
    rel_pa_[li] = x.pa;
    rel_done_[li] = x.cycle;
    rel_tr_[li] = 1;
    b.g6b_tr_done++;
    st_.g6b_tr_done++;
    ready_push(rel_ready_, run_key(li));
  }

  // (2) translation admission: page-aware (stored order), capacity-bounded.
  //     M23-G6C replaces only the walk over this window's runs: instead of one
  //     cursor that stops at the first refusal, the same stored-order page runs
  //     are kept open in P0's cohort table and a refused run is deferred. The
  //     capacity rule below is shared, because it is the buffer's coupling to
  //     translation and not part of the admission policy.
  const u64 cap = cfg_.g6b_pa_capacity;
  const bool g6c = g6c_admit();
  {
    const u64 pend = static_cast<u64>(g6c ? b.g6c_submitted : b.g6b_tr_next) - b.g6b_tr_done;
    const u64 held = static_cast<u64>(rel_ready_.size()) + pend;    // buffer + in flight
    const bool more = g6c ? g6c_work_left(b) : b.g6b_tr_next < n;
    if (more && held >= cap) {
      ++st_.g6b_full_events;
      ++st_.g6b_backpressure;
    }
  }
  if (g6c) {
    win_admit_g6c(cycle, b);
  } else {
  u64 pend = static_cast<u64>(b.g6b_tr_next) - b.g6b_tr_done;   // probes in flight
  u32 tbudget = std::max<u32>(1, cfg_.eng_issue);
  while (tbudget > 0 && b.g6b_tr_next < n) {
    if (static_cast<u64>(rel_ready_.size()) + pend >= cap) break;
    const u32 li = b.g6b_tr_next;
    const u64 vl = run_line(b, li);
    if (free_count() <= tgt_reserve()) { ++st_.tr_slot_stall; break; }
    const u32 pslot = alloc_slot();
    Req r;
    r.va_line = vl;
    r.vpn = vl >> 6;
    r.core = ecore();
    r.slot = pslot;  // M27-P3: a translation holds a residency slot
    r.seq = li;
    r.bypass_l1 = true;
    r.client = 2;  // PTE: translation work only
    r.xlate_only = true;
    if (!mem_.submit(r, cycle)) {
      free_slot(pslot);
      ++st_.g6b_tr_stall;
      break;
    }
    info_[pslot] = Pending{};
    info_[pslot].kind = ReqKind::Xlate;
    rel_gen_[li] = cycle;
    ++st_.g6b_tr_reqs;
    b.g6b_tr_next++;
    ++pend;
    tbudget--;
  }
  }

  // (3) DRAM release: the most local translated line that exists right now
  u32 budget = std::max<u32>(1, cfg_.eng_issue);
  u32 pops = 0;
  // Sampled *before* the drain: the DRAM scheduler's view. `no_ready` is the
  // cycles it had issue budget and nothing translated to issue.
  const bool ready_at_entry = !rel_ready_.empty();
  while (budget > 0 && !rel_ready_.empty()) {
    const u32 li0 = run_at(b, static_cast<u32>(rel_ready_.front()));
    if (b.fflag[li0] & 1u) { ready_pop(rel_ready_); continue; }
    // M23-G6F: the candidate scan runs only when a release is possible at all.
    // This is the first test `win_emit_fused` makes, so a closed gate makes every
    // candidate equally unissuable and skipping the scan changes nothing.
    if ((cfg_.sched_vis || cfg_.sched_group) && free_count() <= tgt_reserve()) {
      ++st_.g6b_rel_slot_stall;
      break;
    }
    const u64 pos = cfg_.sched_group
                        ? pick_group(b)
                        : (cfg_.sched_vis ? pick_candidate(b) : rel_ready_.front());
    const u32 li = run_at(b, static_cast<u32>(pos));
    const int rc = win_emit_fused(b, li, cycle);
    if (rc != 0) {                   // residency budget full: keep it in place
      if (rc == 1) ++st_.g6b_rel_slot_stall;  // M23-G6E: the gate, not the queue
      break;
    }
    ready_erase_val(rel_ready_, pos);
    ++pops;
    // M24-A: order evidence. `pos` is the released run's position in `ford` (its
    // physical rank when the organizer is on, its sealed rank when it is off), so
    // a decrease is a release that went backwards in the order actually used.
    if (m24_prev_pos_ != NONE) {
      ++st_.m24_rel_pairs;
      if (pos < m24_prev_pos_) ++st_.m24_rel_inversions;
    }
    m24_prev_pos_ = pos;
    // A release into a different (bank,row) cell is a cell change: this is the
    // streaming policy's cohort counter, the same event P2 counts as an open.
    u32 bk = 0, rw = 0;
    mem_.dram_bank_row(rel_pa_[li], bk, rw);
    if (!b.g6b_cell_valid || bk != b.g6b_bk || rw != b.g6b_rw) {
      ++st_.sched_opens;
      ++st_.sched_switches;
      b.g6b_bk = bk;
      b.g6b_rw = rw;
      b.g6b_cell_valid = 1;
    }
    // M23-G6G: the group the released run belongs to has just made progress.
    if (cfg_.sched_group) note_group_release(b, bk, rw);
    // M23-G6I: cross-page release bookkeeping (no-op unless g6i_diag is set).
    if (cfg_.g6i_diag) note_g6i_release(cycle, b, li, bk, rw);
    budget--;
  }
  if (pops) {
    ++st_.sched_cycles;          // one scheduling decision per releasing cycle
    st_.sched_scan += pops;      // one queue operation per release
  }
  // M23-G6E: translated work existed and nothing was released this cycle.
  // = the cycles in which the ready buffer is invisible to the DRAM scheduler.
  st_.g6b_rel_pops += pops;
  if (pops == 0 && !rel_ready_.empty()) ++st_.g6b_rel_blocked;
  // M23-G6I: exact per-cycle pipeline facts. "Release while translating" is the
  // overlap the decoupling claim rests on, so it is counted at the event, not
  // inferred from the two lifetime totals.
  if (cfg_.g6i_diag) {
    const u32 tr_sub_i = g6c ? b.g6c_submitted : b.g6b_tr_next;
    if (pops) {
      ++st_.g6i_rel_cycles;
      if (tr_sub_i > b.g6b_tr_done) ++st_.g6i_rel_with_tr;
    }
    if (rel_ready_.empty()) ++st_.g6i_pool_empty;
    if (static_cast<u64>(rel_ready_.size()) > cap) ++st_.g6i_pool_over;
  }

  // (4) accounting: overlap, starvation, buffer. `tr_live` reads whichever
  // submitted counter the admission policy advanced -- G6B's cursor or G6C's
  // per-run count -- so both arms report the same event.
  const u32 tr_sub = g6c ? b.g6c_submitted : b.g6b_tr_next;
  const bool tr_live = tr_sub > b.g6b_tr_done;
  const bool tgt_live = b.pending > 0;
  if (tr_live) ++st_.g6b_tr_active;
  if (tgt_live) ++st_.g6b_tgt_out;
  if (tr_live && tgt_live) ++st_.g6b_overlap;
  if (!ready_at_entry && b.issued_n < n) ++st_.g6b_no_ready;
  st_.g6b_ready_occ_sum += rel_ready_.size();
  if (rel_ready_.size() > st_.g6b_ready_peak) st_.g6b_ready_peak = rel_ready_.size();
  // M23-G6D: occupancy distribution in log2 buckets (0 = empty, 12 = 2048+).
  {
    u64 v = rel_ready_.size();
    u32 rb = 0;
    while (v > 1) { v >>= 1; ++rb; }
    if (!rel_ready_.empty()) ++rb;
    ++st_.g6b_ready_hist[rb < 12 ? rb : 12];
  }
  // M23-G6I: pool contents sample (observation only).
  if (cfg_.g6i_diag) g6i_sample(cycle, b);
}

// ---------------------------------------------------------------------------
// M23-G6C: P0's page-run admission, applied to translation
// ---------------------------------------------------------------------------
// `win_issue_sched` (P0) keeps up to `page_cohorts` page runs open, and when the
// dTLB refuses the run it is issuing from it sets `resume_at = cycle +
// park_retry`, counts one deferral and tries the oldest *other* ready run in the
// same cycle. Those three lines are the whole policy; everything below is that
// loop with the port redirected: the line it submits is a translation-only
// probe, so an accepted run takes no residency slot and `win_emit`'s rc == 1
// (residency, which ends P0's cycle because nothing else can move) has no
// counterpart here.
//
// The window's runs are walked in *stored* order (`fmeta` is ascending line, so a
// 4 KiB page's 64 runs are contiguous), which is the order P0's sealed window is
// in -- not the P2 order `ford` holds. The cohort therefore owns the same unit it
// owns in P0, and translation admission never looks at the DRAM order.
//
// Two things are deliberately not P0's:
//   * the refusal is classified at the caller from the dTLB's own counters
//     (`refuse_parked`: this page's MSHR already holds `dtlb_parked` waiters;
//     `refuse_full`: no dTLB MSHR is free for another page). Both are temporary --
//     a fill retires the MSHR -- and both defer the same way, as they do in P0.
//   * a run leaves for the ready buffer the cycle its own translation lands, not
//     when its page finishes, so a partially translated page can already have
//     runs at the DRAM. Streaming release is preserved.
//
// Nothing here can lose or duplicate a run: a cohort's cursor advances only on an
// accepted probe, cohorts partition [0, open_pos) of the run array, and a re-probe
// is the same run at the same index. `g6c_dup_submit` counts a submission of an
// index that was already submitted -- structurally impossible -- so a future edit
// cannot break the property silently.
void BulkEngine::win_admit_g6c(u64 cycle, Batch& b) {
  const u64 cap = cfg_.g6b_pa_capacity;
  if (!b.g6c_init) {
    b.g6c_init = 1;
    b.g6c_coh_def.assign(b.coh.size(), 0);
    b.g6c_coh_age.assign(b.coh.size(), 0);
    ++st_.g6c_windows;
  }
  // P0 charges the cycle a run takes to install in the scheduler port and issues
  // nothing in that cycle. The same charge, on the admission port.
  if (g6c_busy_) { g6c_busy_ = 0; return; }
  u32 budget = std::max<u32>(1, cfg_.eng_issue);
  u32 refused_idx = 0xffffffffu;
  while (budget > 0) {
    // Oldest ready run wins, exactly as P0 selects: cohorts are opened in stored
    // order, so the first ready one is the oldest page, and a page at its park
    // limit never blocks the next page for more than its own retry deadline.
    u32 idx = 0xffffffffu;
    for (u32 i = 0; i < b.coh.size(); ++i) {
      const Batch::Cohort& x = b.coh[i];
      if (!x.open || x.pos >= x.end) continue;
      if (x.resume_at > cycle) continue;
      idx = i;
      break;
    }
    if (idx == 0xffffffffu) {
      if (g6c_open_cohort(b)) continue;
      // Nothing open can move and no page is left to open: every run is
      // submitted, in flight, or deferred. One lost cycle, counted once, and only
      // when admission had work and the buffer had room (P0's `sched_idle` rule).
      const u64 pend = static_cast<u64>(b.g6c_submitted) - b.g6b_tr_done;
      if (static_cast<u64>(rel_ready_.size()) + pend < cap) ++st_.g6c_admit_idle;
      break;
    }
    Batch::Cohort& c = b.coh[idx];
    const u32 li = c.pos;
    // The buffer, not the dTLB, is the other bound: a full buffer backpressures
    // the whole window and is counted where G6B counts it, not as a refusal.
    const u64 pend = static_cast<u64>(b.g6c_submitted) - b.g6b_tr_done;
    if (static_cast<u64>(rel_ready_.size()) + pend >= cap) break;
    if (rel_gen_[li] != 0 || rel_tr_[li]) {
      ++st_.g6c_dup_submit;  // unreachable; see the note above
      c.pos++;
      if (c.pos >= c.end) { c.open = false; c.pos = c.end = 0; }
      continue;
    }
    // M27-P3: without a residency slot the probe cannot be submitted at all, so
    // the cycle is deferred *before* an admission attempt is counted; the
    // attempts == ok + refused invariant therefore still holds.
    if (free_count() <= tgt_reserve()) { ++st_.tr_slot_stall; break; }
    ++st_.g6c_admit_attempts;
    const u64 vl = run_line(b, li);
    const u32 pslot = alloc_slot();
    Req r;
    r.va_line = vl;
    r.vpn = vl >> 6;
    r.core = ecore();
    r.slot = pslot;  // M27-P3: a translation holds a residency slot
    r.seq = li;
    r.bypass_l1 = true;
    r.client = 2;  // PTE: translation work only
    r.xlate_only = true;
    // The dTLB is a probe's only refusal source, and a refusal increments exactly
    // one of these two counters, so the delta names the cause without the memory
    // model having to return it.
    const u64 rp0 = mem_.refuse_parked, rf0 = mem_.refuse_full;
    if (!mem_.submit(r, cycle)) {
      free_slot(pslot);
      ++st_.g6c_refused;
      ++st_.g6c_defer_events;
      if (mem_.refuse_parked != rp0) ++st_.g6c_refuse_parked;
      else if (mem_.refuse_full != rf0) ++st_.g6c_refuse_full;
      else ++st_.g6c_refuse_other;
      if (!b.g6c_coh_def[idx]) b.g6c_coh_age[idx] = cycle;  // episode begins
      ++b.g6c_coh_def[idx];
      c.resume_at = cycle + std::max<u32>(1, cfg_.park_retry);
      u32 d = 0;
      for (const auto& x : b.coh)
        if (x.open && x.resume_at > cycle) ++d;
      st_.g6c_defer_depth_sum += d;
      ++st_.g6c_defer_depth_n;
      if (d > st_.g6c_defer_depth_max) st_.g6c_defer_depth_max = d;
      refused_idx = idx;
      continue;  // this same cycle can still admit another page
    }
    info_[pslot] = Pending{};
    info_[pslot].kind = ReqKind::Xlate;
    rel_gen_[li] = cycle;
    ++b.g6c_submitted;
    ++st_.g6b_tr_reqs;  // G6B's probe count: the same event, the same key
    ++st_.g6c_admit_ok;
    if (b.g6c_coh_def[idx]) {
      ++st_.g6c_defer_readmit;
      const u64 age = cycle - b.g6c_coh_age[idx];
      st_.g6c_defer_age_sum += age;
      if (age > st_.g6c_defer_age_max) st_.g6c_defer_age_max = age;
      b.g6c_coh_def[idx] = 0;
      b.g6c_coh_age[idx] = 0;
    }
    if (refused_idx != 0xffffffffu && refused_idx != idx) ++st_.g6c_defer_other_admit;
    c.pos++;
    if (c.pos >= c.end) { c.open = false; c.pos = c.end = 0; }
    budget--;
  }
}

// Claim the next stored-order page run for a free cohort: the maximal span of
// `fmeta` that maps to one 4 KiB page, starting where no cohort has claimed yet.
// P0's `win_open_cohort` over an unfused window is the same function; the only
// difference is that the run id here is already an index into `fmeta` (stored
// order), so no permutation is consulted.
bool BulkEngine::g6c_open_cohort(Batch& b) {
  const u32 nrun = static_cast<u32>(b.fmeta.size());
  if (b.g6c_open_pos >= nrun) return false;
  u32 idx = 0;
  for (; idx < b.coh.size(); ++idx)
    if (!b.coh[idx].open) break;
  if (idx == b.coh.size()) return false;
  const u32 start = b.g6c_open_pos;
  const u64 vpn = run_line(b, start) >> 6;
  u32 end = start + 1;
  while (end < nrun && (run_line(b, end) >> 6) == vpn) ++end;
  Batch::Cohort& c = b.coh[idx];
  c.pos = start;
  c.end = end;
  c.vpn = vpn;
  c.resume_at = 0;
  c.open = true;
  b.g6c_open_pos = end;
  b.g6c_coh_def[idx] = 0;
  b.g6c_coh_age[idx] = 0;
  ++st_.g6c_cohorts_opened;
  g6c_busy_ = 1;  // installing a run costs the port a cycle, as it does in P0
  return true;
}

// Any run this window has neither admitted nor released yet, in the form the
// admission loop needs: an unclaimed run, or a claimed one still cursored.
bool BulkEngine::g6c_work_left(const Batch& b) const {
  if (b.g6c_open_pos < b.fmeta.size()) return true;
  for (const auto& x : b.coh)
    if (x.open && x.pos < x.end) return true;
  return false;
}

// The fanout reads its element range out of the run word and reconstructs both
// the element id and the width, which are the two fields the fused entry drops.
void BulkEngine::win_fanout_fused(u64 cycle) {
  (void)cycle;
  const u32 ebase = active_ ? run_.d.elem_base : 0;
  const u8 ewidth = active_ ? run_.d.data_bytes : 8;
  u32 budget = std::max<u32>(1, cfg_.fanout_lanes);
  bool worked = false;
  while (budget > 0 && !fan_.empty()) {
    FanJob& j = fan_.front();
    Batch& b = bat_[j.batch];
    const u32 start = b.fmeta[j.line] >> 16;
    const u32 count = b.fmeta[j.line] & 0xffffu;
    while (budget > 0 && j.pos < count) {
      const u64 w = b.fent[start + j.pos];
      const u32 elem = ebase + fuse_pos(w);
      const u64 addr = fuse_line(w) * LINE_BYTES + fuse_off(w);
      val_[elem] = load_le(j.bytes.data() + fuse_off(w), ewidth);
      addr_[elem] = addr;
      // Independent re-derivation from the index image, exactly as the M6 and M5
      // paths do it: the representation may change, the check may not.
      if (active_ && run_.phase == 1 && elem >= run_.d.elem_base) {
        const u64 pos2 = elem - run_.d.elem_base;
        const u64 ib = run_.d.index_bytes;
        const u8* ibb = mem_.line_bytes(mem_.pa_line_of_va(run_.d.index_base + pos2 * ib));
        const u64 off2 = (run_.d.index_base + pos2 * ib) & (LINE_BYTES - 1);
        const u64 expect =
            run_.d.data_base + load_le(ibb + off2, static_cast<u32>(ib)) * run_.d.data_bytes;
        if (expect != addr) st_.addr_mismatch++;
      }
      st_.completion_hash = fnv(st_.completion_hash, elem);
      b.retired++;
      run_.done++;
      st_.elems_done++;
      st_.fanout_elems++;
      fan_backlog_--;
      ++j.pos;
      --budget;
      worked = true;
    }
    if (j.pos >= count) fan_.pop_front();
  }
  if (worked) st_.fanout_cycles++;
}

void BulkEngine::win_issue(u64 cycle) {
  if (iss_ < 0) return;
  Batch& b = bat_[static_cast<size_t>(iss_)];
  if (cfg_.bulk_org == 6) { win_issue_g6a(cycle, b); return; }
  if (cfg_.bulk_org == 7 || cfg_.bulk_org == 8) { win_issue_g6b(cycle, b); return; }
  if (fused()) {
    // The sorted line array *is* the issue order, so there is no `order` to
    // index through.
    if (cfg_.page_sched) { win_issue_sched_fused(cycle, b); return; }
    u32 n = std::max<u32>(1, cfg_.eng_issue);
    while (n > 0 && b.issue_pos < b.fmeta.size()) {
      const u32 li = run_at(b, static_cast<u32>(b.issue_pos));
      if (b.fflag[li] & 1u) { b.issue_pos++; continue; }
      const int rc = win_emit_fused(b, li, cycle);
      if (rc == 1) break;
      if (rc == 2) { st_.win_refuse++; break; }
      b.issue_pos++;
      n--;
    }
    return;
  }
  if (cfg_.page_sched) { win_issue_sched(cycle, b); return; }

  // M6 policy: one cursor, one refusal ends the cycle. `win_refuse` is
  // therefore a cycle count: the port had a line ready and a free slot and the
  // dTLB would not take it.
  u32 n = std::max<u32>(1, cfg_.eng_issue);
  while (n > 0 && b.issue_pos < b.order.size()) {
    const u32 li = b.order[b.issue_pos];
    if (b.lines[li].issued) { b.issue_pos++; continue; }
    const int rc = win_emit(b, li, cycle);
    if (rc == 1) break;
    if (rc == 2) { st_.win_refuse++; break; }
    b.issue_pos++;
    n--;
  }
}

// One fill retires every element that depends on the line. The bytes are copied
// out of the hierarchy immediately: `line_bytes` returns a scratch buffer for a
// line the host never wrote, so holding the pointer across calls would be a bug.
void BulkEngine::on_line(u64 cycle, const Pending& p) {
  st_.last_fill = cycle;
  st_.busy_at_last_fill = mem_.dram.busy_cycles;
  if (p.win_batch >= bat_.size()) return;
  Batch& b = bat_[p.win_batch];
  if (b.state != 2 || p.win_line >= (fused() ? b.fmeta.size() : b.lines.size())) return;
  if (b.pending > 0) b.pending--;
  st_.tgt_req_done++;  // one line fill; the elements are retired by the fanout
  const u8* bytes = mem_.line_bytes(mem_.pa_line_of_va_line(p.va_line));
  // M13: a registered line is published into the reuse buffer and then fanned
  // out to every window line attached to it -- the one that owned the request
  // and any that arrived while it was outstanding. Every dependant still reads
  // its own bytes out of a 64 B copy, so the retirement path is unchanged.
  if (reuse_on() && p.reuse_ent != 0xffffffffu) {
    reuse_publish(p.reuse_ent, bytes, cycle);
    return;
  }
  push_fan(p.win_batch, p.win_line, bytes);
}

// ---- M13: bounded cross-window reuse buffer -------------------------------
// The mechanism, in full: a set-associative store of completed 64 B target
// lines. Sealing a window probes it once per distinct line; a fill publishes
// into it. There is no other state, no history, no prefetch and no knowledge of
// anything the trace has not already asked for.
//
// A lookup therefore ends in exactly one of three ways, and the three are
// exhaustive:
//   * the line's data is held          -> elements retired from those bytes;
//                                          no request, no residency slot, no
//                                          L1/L2/DRAM traffic.
//   * the line is still outstanding    -> this window's line attaches to that
//                                          request; the fill fans out to both.
//                                          One request serves two windows, and
//                                          no second slot is taken.
//   * neither                          -> the line issues a request as it would
//                                          have without the mechanism, and its
//                                          entry is allocated so the *next*
//                                          window can reuse it.
// Eviction is LRU within a set and only ever of completed entries: an
// outstanding entry is a promise to a window that has not retired yet.
void BulkEngine::win_reuse_probe(Batch& b, u32 bi, u64 cycle) {
  (void)cycle;
  st_.reuse_lines_planned += b.lines.size();
  for (u32 li = 0; li < b.lines.size(); ++li) {
    LineReq& lr = b.lines[li];
    ++st_.reuse_probes;
    // One organizer visit per lookup: a 4-way tag compare plus the 64 B read.
    // Charged, not merely counted -- the organizer is the same bounded unit
    // that admits and sorts, so the cost really does delay admission.
    if (cfg_.reuse_charge) ++st_.org_backlog;
    const u32 set = reuse_set_of(lr.va_line);
    const size_t base = static_cast<size_t>(set) * reuse_ways_;
    size_t hit = reuse_.size();
    for (u32 w = 0; w < reuse_ways_; ++w) {
      const ReuseEnt& e = reuse_[base + w];
      if (e.state && e.tag == static_cast<u32>(lr.va_line)) { hit = base + w; break; }
    }
    if (hit != reuse_.size() && reuse_[hit].state == 2) {
      ReuseEnt& e = reuse_[hit];
      ++st_.reuse_hit_ready;
      st_.reuse_elems_ready += lr.count;
      e.lru = ++reuse_clock_;
      lr.issued = true;
      ++b.issued_n;
      push_fan(bi, li, e.data.data());
      continue;
    }
    if (hit != reuse_.size()) {
      ReuseEnt& e = reuse_[hit];
      if (cfg_.reuse_merge && e.nsub < REUSE_SUBS) {
        ++st_.reuse_hit_inflight;
        st_.reuse_elems_merged += lr.count;
        e.subs[e.nsub].batch = bi;
        e.subs[e.nsub].line = li;
        ++e.nsub;
        if (e.nsub > st_.reuse_sub_max) st_.reuse_sub_max = e.nsub;
        e.lru = ++reuse_clock_;
        lr.issued = true;
        ++b.issued_n;
        continue;
      }
      // Outstanding and not mergeable: the request is formed anyway (it will
      // join the hierarchy's own MSHR, as it did before M13). No second record
      // is created for the same tag.
      if (e.nsub >= REUSE_SUBS) ++st_.reuse_sub_full;
      ++st_.reuse_miss;
      continue;
    }
    ++st_.reuse_miss;
    size_t pick = reuse_.size(), victim = reuse_.size();
    for (u32 w = 0; w < reuse_ways_; ++w) {
      const ReuseEnt& e = reuse_[base + w];
      if (!e.state) { pick = base + w; break; }
      if (e.state == 2 && (victim == reuse_.size() || e.lru < reuse_[victim].lru)) victim = base + w;
    }
    if (pick == reuse_.size()) {
      // Every way is outstanding: keep no record rather than evict a promise.
      if (victim == reuse_.size()) { ++st_.reuse_full; continue; }
      pick = victim;
      ++st_.reuse_evict;
    } else {
      ++reuse_occ_;
    }
    ReuseEnt& e = reuse_[pick];
    e.tag = static_cast<u32>(lr.va_line);
    e.state = 1;
    e.nsub = 1;
    e.subs[0].batch = bi;
    e.subs[0].line = li;
    e.lru = ++reuse_clock_;
    lr.reuse_ent = static_cast<u32>(pick);
  }
}

void BulkEngine::reuse_publish(u32 ent, const u8* bytes, u64 cycle) {
  (void)cycle;
  if (ent >= reuse_.size()) return;
  ReuseEnt& e = reuse_[ent];
  std::memcpy(e.data.data(), bytes, LINE_BYTES);
  e.state = 2;
  e.lru = ++reuse_clock_;
  ++st_.reuse_inserts;
  for (u32 i = 0; i < e.nsub; ++i) push_fan(e.subs[i].batch, e.subs[i].line, e.data.data());
  e.nsub = 0;
}

void BulkEngine::reuse_abort() {
  for (auto& e : reuse_) {
    if (e.state == 1) {
      e.state = 0;
      e.nsub = 0;
      if (reuse_occ_ > 0) --reuse_occ_;
      ++st_.reuse_dropped;
    }
  }
}

void BulkEngine::push_fan(u32 bi, u32 li, const u8* bytes) {
  if (bi >= bat_.size()) return;
  Batch& b = bat_[bi];
  if (li >= (fused() ? b.fmeta.size() : b.lines.size())) return;
  FanJob j;
  j.batch = bi;
  j.line = li;
  j.pos = 0;
  std::memcpy(j.bytes.data(), bytes, LINE_BYTES);
  fan_backlog_ += fused() ? (b.fmeta[li] & 0xffffu) : b.lines[li].count;
  if (fan_backlog_ > st_.fanout_max_backlog) st_.fanout_max_backlog = fan_backlog_;
  fan_.push_back(j);
}

void BulkEngine::win_fanout(u64 cycle) {
  if (fused()) { win_fanout_fused(cycle); return; }
  (void)cycle;
  u32 budget = std::max<u32>(1, cfg_.fanout_lanes);
  bool worked = false;
  while (budget > 0 && !fan_.empty()) {
    FanJob& j = fan_.front();
    Batch& b = bat_[j.batch];
    LineReq& lr = b.lines[j.line];
    while (budget > 0 && j.pos < lr.count) {
      const u32 eid = b.sorted[lr.first + j.pos];
      const WinEnt& e = b.ent[eid];
      val_[e.elem] = load_le(j.bytes.data() + e.off, e.width);
      addr_[e.elem] = e.va_line * LINE_BYTES + e.off;
      // Independent re-derivation, exactly as the M5 path does it: value and
      // address must agree with the index image, element by element.
      if (active_ && run_.phase == 1 && e.elem >= run_.d.elem_base) {
        const u64 pos2 = e.elem - run_.d.elem_base;
        const u64 ib = run_.d.index_bytes;
        const u8* ibb = mem_.line_bytes(mem_.pa_line_of_va(run_.d.index_base + pos2 * ib));
        const u64 off2 = (run_.d.index_base + pos2 * ib) & (LINE_BYTES - 1);
        const u64 expect =
            run_.d.data_base + load_le(ibb + off2, static_cast<u32>(ib)) * run_.d.data_bytes;
        if (expect != addr_[e.elem]) st_.addr_mismatch++;
      }
      st_.completion_hash = fnv(st_.completion_hash, e.elem);
      b.retired++;
      run_.done++;
      st_.elems_done++;
      st_.fanout_elems++;
      fan_backlog_--;
      ++j.pos;
      --budget;
      worked = true;
    }
    if (j.pos >= lr.count) fan_.pop_front();
  }
  if (worked) st_.fanout_cycles++;
}

void BulkEngine::win_tick(u64 cycle) {
  // (1) the organizer makes progress: it is one engine at `org_lanes`/cycle
  if (st_.org_backlog) {
    const u64 lanes = std::max<u32>(1, cfg_.org_lanes);
    st_.org_backlog = st_.org_backlog > lanes ? st_.org_backlog - lanes : 0;
    st_.org_cycles++;
  }

  // (2) release windows that are fully retired
  for (int i = 0; i < 2; ++i) {
    Batch& b = bat_[static_cast<size_t>(i)];
    // `issued_n` counts lines actually submitted, so it reaches |order| exactly
    // when the last line is out -- for the single-cursor and the cohort policy
    // alike (cohorts partition `order`, so no line is claimed twice).
    const size_t nlines = fused() ? b.fmeta.size() : b.order.size();
    if (b.state == 2 && b.issued_n >= nlines && b.pending == 0 && b.retired >= b.n_ent) {
      if (iss_ == i) iss_ = -1;
      if (BulkStats::WinRec* rec = win_rec(i)) rec->retire = cycle;
      rec_ix_[static_cast<size_t>(i)] = 0xffffffffu;
      b = Batch{};
    }
  }

  // (3) adopt a sealed window for issue
  if (iss_ < 0) {
    for (int i = 0; i < 2; ++i)
      if (bat_[static_cast<size_t>(i)].state == 2) {
        iss_ = i;
        if (BulkStats::WinRec* rec = win_rec(i)) rec->issue = cycle;
        break;
      }
  }

  // (4) pick a window to fill: never the issuing one, and with org_overlap = 0
  //     nothing new starts until the previous window has fully drained
  // `gen_done < count` is inert for every decision -- an empty window accepts
  // nothing and step (6) discards it in the same tick -- but without it the
  // manager adopts and discards a buffer every cycle once the generator is
  // exhausted, which made the allocation count (and the record) meaningless.
  if (fill_ < 0 && active_ && run_.phase == 1 && run_.gen_done < run_.d.count) {
    // M9 window management: the only place a window is adopted. `no_overlap` is
    // the published `org_overlap = 0` rule plus E2's one-live-window limit. The
    // manager already holds at most one *incomplete* window (single `fill_`
    // cursor), so refusing a second *live* window is the only distinct form of
    // "one window at a time" the structure admits.
    const bool no_overlap = !cfg_.org_overlap || cfg_.effective_win_policy() == 1;
    for (int i = 0; i < 2; ++i) {
      if (i == iss_) continue;
      if (bat_[static_cast<size_t>(i)].state != 0) continue;
      if (no_overlap && iss_ >= 0) break;
      fill_ = i;
      Batch& nb = bat_[static_cast<size_t>(i)];
      nb.state = 1;
      nb.fill_start = cycle;   // E0/E1/E2 deadline anchor
      nb.last_admit = cycle;   // E3/E4 deadline anchor, until the first admission
      st_.win_recs.push_back(BulkStats::WinRec{});
      st_.win_recs.back().alloc = cycle;
      rec_ix_[static_cast<size_t>(i)] = static_cast<u32>(st_.win_recs.size() - 1);
      ++st_.win_alloc;
      break;
    }
    // Time spent waiting for a window: the generator has elements and no buffer
    // is accepting them. `win_fill_blocked` separates the cycles in which the
    // policy, not the buffer count, is what held the new window back.
    if (fill_ < 0 && run_.gen_done < run_.d.count) {
      ++st_.win_fill_wait;
      u32 live = 0;
      for (const auto& x : bat_)
        if (x.state != 0) ++live;
      if (no_overlap && live > 0) ++st_.win_fill_blocked;
    }
  }

  // (5) submit anything queued by the shared streams (descriptor, index lines),
  //     then issue the adopted window before the index stream competes for
  //     slots, so target lines are never starved by their own index fetches
  issue(cycle);
  // M8: the index port runs *before* the target port. That is the arbitration
  // rule the M6 path already had implicitly (`issue()` drains index and target
  // slots together, before the window issues anything): address generation
  // cannot produce target lines without index values, so letting targets take
  // every slot first inverts the dependency and starves the feeder -- measured
  // at +43 445 cycles on W1 (report §5.3). The bound `idx_mshr` keeps the stream
  // from taking more than its share.
  idx_issue(cycle);
  win_issue(cycle);
  win_fanout(cycle);

  // (6) keep the index stream moving, then admit into the window
  stream_index();
  idx_stream(cycle);
  if (fill_ >= 0) {
    if (st_.org_backlog > 2ull * std::max<u32>(1, cfg_.org_lanes)) {
      st_.win_full_stall++;  // organizer saturated: admission backpressure
    } else {
      win_admit(cycle);
    }
  }

  // (6) seal on: window full, descriptor exhausted, or deadline
  if (fill_ >= 0) {
    Batch& b = bat_[static_cast<size_t>(fill_)];
    const bool full = b.n_ent >= cfg_.batch_window;
    const bool ended = active_ && run_.phase == 1 && run_.gen_done >= run_.d.count;
    // The partial-seal bound, and the only thing the policies E1-E4 change here.
    // `age` is the published rule: cycles since the buffer was adopted. `idle`
    // measures the same bound from the last admission, so a window that is still
    // making progress is never truncated (this is E3; the sequential filling it
    // asks for is already the structure above). `cap` is E4's absolute ceiling,
    // so a trickling window cannot stay open for the whole descriptor. Precedence
    // full > descriptor end > partial is unchanged in every policy.
    const u64 D = cfg_.batch_deadline;
    const bool age = D && b.n_ent > 0 && cycle - b.fill_start >= D;
    const bool idle = D && b.n_ent > 0 && cycle - b.last_admit >= D;
    const bool cap = D && b.n_ent > 0 &&
                     cycle - b.fill_start >= static_cast<u64>(cfg_.win_abs_cap) * D;
    const u32 pol = cfg_.effective_win_policy();
    bool late = false;
    if (pol <= 1) late = age;        // E0/E1 (control), E2
    else if (pol == 2) late = idle;  // E3 (default for M9)
    else late = idle || cap;         // E4
    if (seal_pending_ || full) win_seal(cycle, 0);
    else if (ended) win_seal(cycle, 1);
    else if (late) win_seal(cycle, 2);
  }

  // (8) occupancy. `win_occ_max` is the peak of a single window (what W bounds);
  //     `win_total_max` is the peak across both buffers (what the SRAM holds).
  u64 occ = 0, per_buf = 0, backlog = 0;
  u64 vec_bytes = 0;
  for (const auto& b : bat_) {
    occ += b.n_ent;
    if (b.n_ent > per_buf) per_buf = b.n_ent;
    if (b.state == 2 && b.order.size() > b.issued_n) backlog += b.order.size() - b.issued_n;
    // M12 diagnostic: what the two buffers actually occupy in this simulator.
    vec_bytes += static_cast<u64>(b.ent.capacity()) * sizeof(WinEnt) +
                 static_cast<u64>(b.sorted.capacity()) * sizeof(u32) +
                 static_cast<u64>(b.lines.capacity()) * sizeof(LineReq) +
                 static_cast<u64>(b.order.capacity()) * sizeof(u32) +
                 static_cast<u64>(b.dir_vpn.capacity()) * sizeof(u64) +
                 static_cast<u64>(b.dir_mask.capacity()) * sizeof(u64);
    if (fused()) {
      vec_bytes += static_cast<u64>(b.fent.capacity()) * 8 +
                   static_cast<u64>(b.fmeta.capacity()) * 4 +
                   static_cast<u64>(b.fslot.capacity()) +
                   static_cast<u64>(b.fflag.capacity());
      st_.win_fused = 1;
    }
  }
  if (vec_bytes > st_.win_vec_bytes) st_.win_vec_bytes = vec_bytes;
  if (fused() && vec_bytes > st_.win_fused_vec_bytes) st_.win_fused_vec_bytes = vec_bytes;
  // M13 occupancy is maintained incrementally at the entry's own events (alloc,
  // evict, abort), so this block never walks the array.
  if (reuse_on()) {
    st_.reuse_occ_sum += reuse_occ_;
    if (reuse_occ_ > st_.reuse_occ_max) st_.reuse_occ_max = reuse_occ_;
    st_.reuse_vec_bytes = std::max<u64>(st_.reuse_vec_bytes,
                                       static_cast<u64>(reuse_.capacity()) * sizeof(ReuseEnt));
  }
  st_.win_backlog = backlog;  // diagnostic: sealed lines still waiting for a slot
  // How many windows are live at once, observed at the cycle boundary. `live`
  // counts non-empty buffers; `drain` counts sealed ones still issuing or
  // filling. E2 (win_policy = 1) must show live <= 1 and drain == 0.
  {
    u64 live = 0, drain = 0, open = 0;
    for (const auto& b : bat_) {
      if (b.n_ent) ++live;
      if (b.state == 1) ++open;
      if (b.state == 2 && b.n_ent) ++drain;
    }
    if (live > st_.win_live_max) st_.win_live_max = live;
    if (open > st_.win_incomplete_max) st_.win_incomplete_max = open;
    if (drain > st_.win_drain_max) st_.win_drain_max = drain;
    if (live >= 2) ++st_.win_live_cycles;
    if (drain > 0 && iss_ < 0) ++st_.win_issue_idle;
  }
  win_occ_ = occ;
  st_.win_occ_sum += occ;
  if (per_buf > st_.win_occ_max) st_.win_occ_max = per_buf;
  if (occ > st_.win_total_max) st_.win_total_max = occ;

  // (10) a descriptor is complete when every element has been reconstructed and
  //      no window or fill is still outstanding
  if (active_ && run_.phase == 1 && run_.done >= run_.d.count && win_idle()) finish_desc(cycle);
}

// ---------------------------------------------------------------------------
// Driver: bulk mode over the same trace / memory configuration as the baseline
// ---------------------------------------------------------------------------
void BulkWorkload::apply(MemorySystem& mem) const {
  for (const auto& w : writes) mem.write_va(w.first, w.second.data(), w.second.size());
}

// `bulk_wl = E0 | E1` turns the calibrated trace into an indirect gather:
// index[i] = the byte address of access i divided by the element width, so
//   data_base + index[i] * elem_bytes
// is exactly the address the trace itself accesses. Both front-ends therefore
// touch the same lines at the same in-line offsets, and the engine additionally
// pays the index stream (ceil(count * index_bytes / 64) lines) and the
// descriptor fetch. With bulk_data_base = 0 the virtual-address layouts are
// identical, which is what the controlled A/B comparison requires (§2).
//
// The result is a *value*: built once by the caller and applied to each arm's
// fresh memory, so no arm can see a differently generated workload.
BulkWorkload build_bulk_workload(const Config& cfg, const Trace& tr, std::string& err,
                                 const ClassMap* cm) {
  BulkWorkload w;
  if (cfg.bulk_wl == "none") return w;
  if (cfg.elem_bytes != 1 && cfg.elem_bytes != 2 && cfg.elem_bytes != 4 && cfg.elem_bytes != 8) {
    err = "bulk_wl requires elem_bytes in {1,2,4,8}";
    return w;
  }

  // ---- M9: which elements this descriptor may cover ------------------------
  // The class map is consulted here, once, before a descriptor exists and before
  // any index value is generated. An element whose instruction is SIMT is never
  // selected, so it is never written into the index array -- the engine has
  // nothing to acquire for it. With `cm` null (every published path) the
  // selection is the whole trace and the workload is byte for byte the M5-M8
  // one. `sel[j]` is the trace access index of the descriptor's element j, i.e.
  // the original element identity that compaction would otherwise destroy.
  std::vector<u32> sel;
  sel.reserve(tr.acc.size());
  for (u32 i = 0; i < tr.acc.size(); ++i) {
    if (cm && cm->at(tr.acc[i].instr) != static_cast<u8>(OpClass::Bulk)) continue;
    sel.push_back(i);
  }
  if (sel.empty()) {
    // Nothing was dispatched to the accelerator: no descriptor, no index array,
    // no image writes, and the engine stays idle for the whole run.
    return w;
  }

  BulkDesc d;
  d.desc_line = cfg.bulk_desc_line;
  d.index_base = cfg.bulk_index_base;
  d.data_base = cfg.bulk_data_base;
  d.count = static_cast<u32>(sel.size());
  d.elem_base = 0;
  d.index_bytes = (cfg.bulk_wl == "E1") ? 8 : 4;
  d.data_bytes = static_cast<u8>(cfg.elem_bytes);

  if (d.data_base + static_cast<u64>(d.count) * cfg.elem_bytes > d.index_base &&
      d.data_base < d.index_base + static_cast<u64>(d.count) * d.index_bytes) {
    err = "bulk_data_base overlaps the index array";
    return w;
  }
  if (d.desc_line >= (d.index_base >> 6)) {
    err = "bulk_desc_line must sit below the index array";
    return w;
  }
  const std::string why = d.validate(cfg.bulk_max_elems);
  if (!why.empty()) { err = "configuration descriptor invalid: " + why; return w; }

  // Materialize the index array once: the values are the trace addresses
  // divided by the element width, so the engine's target addresses are
  // verifiable against the trace the workload was derived from.
  std::vector<u8> idx(static_cast<size_t>(d.count) * d.index_bytes, 0);
  w.index_values.assign(d.count, 0);
  w.elem_ids.assign(d.count, 0);
  for (size_t j = 0; j < sel.size(); ++j) {
    const u32 ai = sel[j];
    const u64 v = tr.acc[ai].addr / cfg.elem_bytes;
    w.index_values[j] = v;
    w.elem_ids[j] = ai;
    u8 buf[8];
    put64(buf, v);
    for (u32 k = 0; k < d.index_bytes; ++k) idx[j * d.index_bytes + k] = buf[k];
  }
  w.writes.push_back({d.index_base, idx});
  std::vector<u8> line(LINE_BYTES, 0);
  d.encode(line.data());
  w.writes.push_back({d.desc_line * LINE_BYTES, line});

  w.desc = d;
  w.armed = true;
  return w;
}

// ---------------------------------------------------------------------------
// M9: hybrid driver (docs/m9-dispatch-design.md §4.3)
// ---------------------------------------------------------------------------
// One materialised trace, one MemorySystem, one residency budget, two clients.
//
// Per cycle:
//   1. advance the hierarchy ONCE     -- the only mem_->tick in the process
//   2. route this cycle's completions by slot ownership (the pool's owner table)
//   3. release SIMT slots / attribute the engine's completions
//   4. engine generation, then SIMT generation (or the reverse: dispatch_order)
//   5. accounting, then the pool's structural invariant
//
// Nothing here classifies anything. The class map is an input, built before the
// first cycle; the bulk workload and its index array are derived from it, so a
// SIMT element cannot reach the engine. There is no fallback path and no
// operation can change class once the run has started.
RunResult Sim::run_hybrid() {
  RunResult res;
  Stats& s = res.s;
  auto fail = [&](const std::string& m) {
    res.ok = false;
    res.violations.push_back(m);
  };

  // M27-P2: liveness. Returns false when the run must stop because work remains
  // and nothing has been admitted or has retired for longer than the model's own
  // worst-case service time. `stall_abort` (M26 harness, default 0) only
  // tightens the bound for tests; the cycle guard is untouched.
  auto live_check = [&](u64 now, bool work_left) -> bool {
    if (!work_left) return true;
    const u64 thr = cfg_.stall_abort ? cfg_.stall_abort : mem_->live_limit();
    if (!mem_->liveness_tripped(now, thr)) return true;
    fail("liveness: no admission or retirement for " +
         std::to_string(now - mem_->last_progress_cycle()) + " cycles (bound " +
         std::to_string(thr) + "); " + mem_->outstanding_report());
    return false;
  };

  if (cfg_.cores != 1) {
    // M27-P5: single-core is a property of this model, stated rather than
    // assumed. The engine's traffic enters exactly one SM's L1 and dTLB; with
    // more cores the dispatch path would have to define which, so it refuses.
    fail("dispatch is single-core in this model: the SIMT front-end holds one slot "
         "vector per core and the engine enters one SM (eng_core); set cores = 1");
    return res;
  }
  if (cfg_.eng_core != 0xffffffffu && cfg_.eng_core >= cfg_.cores) {
    fail("eng_core must name an existing core");
    return res;
  }

  // ---- 1. compiler metadata, then the partition ----------------------------
  const ClassMap cm = build_op_classes(cfg_, *tr_);
  std::string err;
  const BulkWorkload w = build_bulk_workload(cfg_, *tr_, err, &cm);
  if (!err.empty()) {
    fail("workload: " + err);
    return res;
  }

  u64 instr_simt = 0, instr_bulk = 0;
  for (u32 i = 0; i < tr_->n_instr; ++i) {
    if (cm.at(i) == static_cast<u8>(OpClass::Bulk)) ++instr_bulk;
    else ++instr_simt;
  }
  u64 bulk_elems = 0;
  for (const auto& a : tr_->acc)
    if (cm.at(a.instr) == static_cast<u8>(OpClass::Bulk)) ++bulk_elems;
  const u64 simt_elems = tr_->acc.size() - bulk_elems;

  // The compaction must be exactly the metadata's selection -- no more, no less,
  // and every index value must still name its own element's address.
  if (bulk_elems != (w.armed ? static_cast<u64>(w.desc.count) : 0ull))
    fail("the bulk workload does not cover exactly the bulk-class elements");
  for (u32 j = 0; j < w.desc.count; ++j) {
    const u32 ai = w.elem_ids[j];
    if (ai >= tr_->acc.size() ||
        cm.at(tr_->acc[ai].instr) != static_cast<u8>(OpClass::Bulk)) {
      fail("a SIMT-classified element reached the bulk index array");
      break;
    }
    if (w.index_values[j] * cfg_.elem_bytes != tr_->acc[ai].addr) {
      fail("an index value does not name its element's address");
      break;
    }
  }

  // M23: the selection audit trail. Written after the class map is final and
  // before the first cycle, so it records the decision, not an outcome. No
  // counter below is derived from it, and an empty path writes nothing.
  if (!cfg_.m23_select.empty()) {
    std::ofstream out(cfg_.m23_select);
    if (out) {
      std::vector<u64> per(tr_->n_instr, 0);
      for (const auto& a : tr_->acc) ++per[a.instr];
      const char* src = cfg_.op_class == "pattern"  ? "static-stride-analysis"
                        : cfg_.op_class == "gen"    ? "compiler-annotation"
                                                    : "forced-uniform";
      out << "# instr\tclass\telems\tsource\treason\n";
      for (u32 i = 0; i < tr_->n_instr; ++i) {
        const bool b = cm.at(i) == static_cast<u8>(OpClass::Bulk);
        out << i << '\t' << (b ? "bulk" : "simt") << '\t' << per[i] << '\t' << src
            << '\t'
            << (b ? "independent-irregular-gather" : "regular-or-localised-coalesced")
            << '\n';
      }
      out << "# totals\tsimt_instr=" << instr_simt << "\tbulk_instr=" << instr_bulk
          << "\tsimt_elems=" << simt_elems << "\tbulk_elems=" << bulk_elems << "\n";
    }
  }

  // ---- 2. one budget, one hierarchy ---------------------------------------
  const u32 latch_charge = cfg_.ab_charge_latch ? cfg_.lanes : 0;
  // Diagnostic capacity override (Config::pool_slots). Zero -- every published
  // configuration -- is the arithmetic below, unchanged.
  const u32 slots = cfg_.pool_slots ? cfg_.pool_slots
                                    : std::max<u32>(1, cfg_.R - latch_charge);
  SlotPool pool(slots, true);  // ascending: the id sequence Sim::run produces
  eng_ = std::make_unique<BulkEngine>(cfg_, *mem_, &pool);
  w.apply(*mem_);
  if (w.armed) eng_->submit(w.desc);

  // ---- 3. SIMT front-end state, same shape as Sim::run --------------------
  struct HCore {
    std::vector<u32> instrs;
    size_t next = 0;
    std::deque<Req> held;
    std::vector<u64> cur_lines;
    size_t cur_pos = 0;
    u64 completed = 0;
  };
  std::vector<u32> begin(tr_->n_instr + 1, 0);
  for (const auto& a : tr_->acc) begin[a.instr + 1]++;
  for (u32 i = 0; i < tr_->n_instr; ++i) begin[i + 1] += begin[i];
  const u32 chunk = tr_->n_instr ? (tr_->n_instr + cfg_.cores - 1) / cfg_.cores : 0;
  const bool blocked = cfg_.core_map == "blocked";
  std::vector<HCore> cores(cfg_.cores);
  std::vector<u64> in_sys(cfg_.cores, 0);
  for (u32 c = 0; c < cfg_.cores; ++c) {
    for (u32 i = 0; i < tr_->n_instr; ++i) {
      // THE DISPATCH. One predicate, evaluated on the metadata, before any
      // request is formed.
      if (cm.at(i) != static_cast<u8>(OpClass::Simt)) continue;
      const bool mine = blocked ? (i / std::max<u32>(1, chunk) == c) : (i % cfg_.cores == c);
      if (mine) cores[c].instrs.push_back(i);
    }
  }

  s.set("cores", cfg_.cores);
  s.set("slots_total", slots + static_cast<u64>(latch_charge) * cfg_.cores);
  s.set("slot_pool_total", slots);
  s.set("latch_charge", latch_charge);
  s.set("trace_accesses", tr_->acc.size());
  s.set("trace_instr", tr_->n_instr);
  s.set("trace_lines", tr_->lines_real);
  s.set("trace_pages", tr_->pages_real);
  s.set("mem_path", cfg_.mem_path == "l2" ? 2 : (cfg_.mem_path == "l1" ? 1 : 0));
  s.set("dispatch_mode", 1);
  s.set("dispatch_order", cfg_.dispatch_order);
  s.set("simt_instr", instr_simt);
  s.set("bulk_instr", instr_bulk);
  s.set("simt_elems", simt_elems);
  s.set("bulk_elems", bulk_elems);
  s.set("simt_share_x1000",
        tr_->acc.empty() ? 0 : simt_elems * 1000ull / static_cast<u64>(tr_->acc.size()));
  s.set("bulk_workload_armed", w.armed ? 1 : 0);

  u64 cycle = 0;
  u64 hash = 1469598103934665603ull;
  u64 simt_gen = 0, simt_issued = 0, simt_done = 0;
  // M15: when each path completed its first and last element. Observed from the
  // counters the driver already maintains, one comparison per cycle, read by no
  // decision -- they exist so a hybrid run can state when each client finished
  // instead of requiring a cycle trace to reconstruct it.
  u64 simt_first_cycle = NONE, simt_last_cycle = 0;
  u64 bulk_first_cycle = NONE, bulk_last_cycle = 0;
  u64 simt_seen = 0, bulk_seen = 0;  // previous counts, so "last" is a change
  u64 peak_simt = 0, peak_bulk = 0, peak_total = 0, staging_max = 0;
  bool pool_reported = false, latch_reported = false;
  // M9 diagnostic: SIMT-side refusals and who held the pool when they happened.
  // Counters only; nothing reads them during the run.
  u64 simt_form_refuse = 0, simt_ref_simt = 0, simt_ref_bulk = 0, simt_ref_free = 0;
  // Per-slot allocation cycle for the lifetime distribution, and last cycle's
  // owner table so an allocation and a release can be dated by diffing it.
  std::vector<u64> born;
  if (cyc_) {
    born.assign(slots, 0);
    pool.born = &born;         // every claim stamped, every release tallied
    pool.ages = &cyc_->ages;   // (exact, including same-cycle churn)
  }

  auto simt_step = [&](u64 now) {
    for (u32 ci = 0; ci < cfg_.cores; ++ci) {
      HCore& c = cores[ci];
      if (cfg_.lsu_pending && in_sys[ci] >= cfg_.lsu_pending) continue;

      // Issue: up to one request per warp per cycle. A request refused by the
      // hierarchy keeps its slot and is retried next cycle, as in Sim::run.
      u32 attempts = 0;
      const u32 cap = std::max<u32>(1, cfg_.warps_per_core);
      for (size_t i = 0; i < c.held.size() && attempts < cap;) {
        Req r = c.held[i];
        if (mem_->submit(r, now)) {
          c.held.erase(c.held.begin() + static_cast<long>(i));
          s.inc("issued");
          ++simt_issued;
          in_sys[ci]++;
        } else {
          ++i;
          s.inc("simt_issue_stalls");
        }
        ++attempts;
      }

      // Coalesce the next SIMT-classified instruction into distinct lines.
      if (c.cur_pos >= c.cur_lines.size() && c.next < c.instrs.size()) {
        const u32 id = c.instrs[c.next];
        c.cur_lines.clear();
        c.cur_pos = 0;
        for (u32 i = begin[id]; i < begin[id + 1]; ++i) {
          const u64 line = tr_->acc[i].line;
          if (std::find(c.cur_lines.begin(), c.cur_lines.end(), line) == c.cur_lines.end())
            c.cur_lines.push_back(line);
        }
        c.next++;
        s.inc("instr_issued");
        s.inc("requests_generated", c.cur_lines.size());
        simt_gen += c.cur_lines.size();
      }

      // Form requests against the shared pool. The latch (the rest of
      // `cur_lines`) is the same unformed remainder Sim::run keeps, and is
      // accounted separately, never as a slot.
      while (c.cur_pos < c.cur_lines.size() && pool.has_free()) {
        Req r;
        r.va_line = c.cur_lines[c.cur_pos++];
        r.vpn = r.va_line >> 6;
        r.core = ci;
        r.slot = pool.take(0);
        r.seq = s.get("requests_generated");
        s.inc("slot_allocs");
        c.held.push_back(r);
      }
      // M9 diagnostic: the cycle in which a coalesced line was ready and the
      // pool had no slot, attributed to whoever held them at that instant.
      if (c.cur_pos < c.cur_lines.size()) {
        ++simt_form_refuse;
        simt_ref_simt += pool.held_by(0);
        simt_ref_bulk += pool.held_by(1);
        simt_ref_free += pool.free_count();
      }
      s.inc("unissued_cycles", c.held.size());
      if (c.held.size() > s.get("unissued_max")) s.set("unissued_max", c.held.size());
    }
  };

  for (;; ++cycle) {
    // M9 diagnostic snapshots, taken before the hierarchy advances. Unused when
    // no trace is attached (the compiler removes the block's body).
    u64 d_sgen = 0, d_siss = 0, d_sdone = 0, d_sref = 0;
    u64 d_biss = 0, d_btgt = 0, d_bidx = 0, d_belem = 0, d_bstall = 0, d_bidxstall = 0;
    if (cyc_) {
      pool.now = cycle;
      d_sgen = simt_gen;
      d_siss = simt_issued;
      d_sdone = simt_done;
      d_sref = simt_form_refuse;
      const BulkStats& z = eng_->stats();
      d_biss = z.tgt_req_issued;
      d_btgt = z.tgt_req_done;
      d_bidx = z.idx_lines_done;
      d_belem = z.elems_done;
      d_bstall = z.slot_stall;
      d_bidxstall = z.idx_buf_stall;
    }
    std::vector<u64> done;
    mem_->tick(cycle, done);  // the only tick in the process

    // Route this cycle's completions. `done` is in hierarchy order and the SIMT
    // hash folds slot and cycle in that order, so this must stay a stable
    // filter, never a sort.
    std::vector<u64> eng_done;
    for (u64 rec : done) {
      const u32 slot = static_cast<u32>(rec & 0xffffffffu);
      const u32 core = static_cast<u32>(rec >> 32);
      if (slot >= slots || core >= cfg_.cores) {
        fail("a completion record names a slot or core outside the pool");
        continue;
      }
      const u8 who = pool.owner[slot];
      if (who == SlotPool::FREE) {
        fail("a completion arrived for a slot nobody holds");
        continue;
      }
      if (who == 1) {
        eng_done.push_back(rec);  // attribute() releases it through the pool
        continue;
      }
      pool.give(slot);
      if (in_sys[core] > 0) in_sys[core]--;
      hash = fnv(hash, slot);
      hash = fnv(hash, cycle);
      s.inc("completed");
      ++simt_done;
      cores[core].completed++;
    }
    eng_->attribute_done(cycle, eng_done);

    if (cfg_.dispatch_order == 0) {
      eng_->progress(cycle);
      simt_step(cycle);
    } else {
      simt_step(cycle);
      eng_->progress(cycle);
    }

    if (simt_done != simt_seen) {
      if (simt_first_cycle == NONE) simt_first_cycle = cycle;
      simt_last_cycle = cycle;
      simt_seen = simt_done;
    }
    const u64 bulk_done_now = eng_->stats().elems_done;
    if (bulk_done_now != bulk_seen) {
      if (bulk_first_cycle == NONE) bulk_first_cycle = cycle;
      bulk_last_cycle = cycle;
      bulk_seen = bulk_done_now;
    }

    // ---- accounting -------------------------------------------------------
    u64 latched = 0;
    for (const auto& c : cores)
      if (c.cur_pos < c.cur_lines.size()) latched += c.cur_lines.size() - c.cur_pos;
    staging_max = std::max(staging_max, latched);
    peak_simt = std::max<u64>(peak_simt, pool.held_by(0));
    peak_bulk = std::max<u64>(peak_bulk, pool.held_by(1));
    peak_total = std::max<u64>(peak_total, pool.residency());
    if (!pool.consistent() && !pool_reported) {
      fail("slot pool is inconsistent (a slot is in two places)");
      pool_reported = true;
    }
    if (latched > cfg_.lanes && !latch_reported) {
      fail("coalescer latch exceeds lanes");
      latch_reported = true;
    }

    bool simt_work = false;
    for (const auto& c : cores)
      if (c.next < c.instrs.size() || !c.held.empty() || c.cur_pos < c.cur_lines.size())
        simt_work = true;
    // "Still has work" for the diagnostic includes requests already submitted and
    // still draining: without them the flag would go false one drain-window early
    // and the overlap matrix would miss the cycles in which SIMT holds its last
    // slots. The termination test below still uses `simt_work` plus `mem_->idle()`,
    // so no run can end with a request in flight.
    bool simt_live = simt_work;
    for (u32 ci = 0; ci < cfg_.cores; ++ci)
      if (in_sys[ci] > 0) simt_live = true;

    if (cyc_) {  // M9 diagnostic: pure observation
      const BulkStats& bs = eng_->stats();
      CycleSample q;
      q.simt_done = static_cast<u32>(simt_done);
      q.bulk_done = static_cast<u32>(bs.elems_done);
      q.pool_simt = static_cast<u32>(pool.held_by(0));
      q.pool_bulk = static_cast<u32>(pool.held_by(1));
      q.pool_free = static_cast<u32>(pool.free_count());
      q.simt_latch = static_cast<u32>(latched);
      q.bulk_res = static_cast<u32>(eng_->residency());
      q.mem_inflight = static_cast<u32>(mem_->inflight_count());
      q.issued = static_cast<u32>(s.get("issued"));
      q.simt_stalls = static_cast<u32>(s.get("simt_issue_stalls"));
      q.bulk_slot_stall = static_cast<u32>(bs.slot_stall);
      q.bulk_idx_wait = static_cast<u32>(bs.idx_wait_stall);
      q.bulk_buf_full = static_cast<u32>(bs.buf_full_stall);
      q.bulk_win_full = static_cast<u32>(bs.win_full_stall);
      // Contention detail. Cumulative counters, so a per-cycle event is a
      // difference against the snapshot taken before this cycle's progress.
      q.simt_form = static_cast<u32>(simt_gen);
      q.simt_issue = static_cast<u32>(simt_issued);
      q.simt_form_refuse = static_cast<u32>(simt_form_refuse);
      q.simt_ref_simt = static_cast<u32>(simt_ref_simt);
      q.simt_ref_bulk = static_cast<u32>(simt_ref_bulk);
      q.simt_ref_free = static_cast<u32>(simt_ref_free);
      q.bulk_alloc = static_cast<u32>(bs.slot_take);
      q.bulk_release = static_cast<u32>(bs.slot_give);
      q.bulk_refuse_noslot = static_cast<u32>(bs.tgt_refuse_noslot);
      q.bulk_refuse_reserve = static_cast<u32>(bs.tgt_refuse_reserve);
      q.bulk_ref_simt = static_cast<u32>(bs.tgt_refuse_simt_held);
      q.bulk_ref_bulk = static_cast<u32>(bs.tgt_refuse_bulk_held);
      q.bulk_ref_free = static_cast<u32>(bs.tgt_refuse_free);
      q.bulk_tgt_issue = static_cast<u32>(bs.tgt_req_issued);
      q.bulk_tgt_done = static_cast<u32>(bs.tgt_req_done);
      q.bulk_idx_done = static_cast<u32>(bs.idx_lines_done);
      q.bulk_elems = static_cast<u32>(bs.elems_done);
      // Index pipeline, target pipeline and DRAM scheduler (capacity study).
      q.idx_sub = static_cast<u32>(bs.idx_sub);
      q.idx_elems = static_cast<u32>(bs.idx_consumed);
      q.addr_gen = static_cast<u32>(bs.tgt_addr_gen);
      q.idx_stall_cap = static_cast<u32>(bs.idx_wait_cap);
      q.idx_ready = static_cast<u32>(eng_->idx_ready_elems());
      q.idx_ready_lines = static_cast<u32>(eng_->idx_ready_lines());
      q.idx_ring = static_cast<u32>(eng_->idx_ring_lines());
      q.idx_res = static_cast<u32>(eng_->idx_resident_lines());
      q.tgt_backlog = static_cast<u32>(bs.win_backlog);
      q.tgt_unissued = static_cast<u32>(eng_->tgt_unissued());
      q.dram_q = static_cast<u32>(mem_->dram_queue());
      q.dram_elig = static_cast<u32>(mem_->dram_elig_now());
      q.ag_remain = static_cast<u32>(eng_->ag_remaining());
      q.win_fill = eng_->win_filling() ? 1u : 0u;
      q.bulk_defer = static_cast<u32>(bs.sched_defer);
      q.winrefuse = static_cast<u32>(bs.win_refuse);
      // Readiness and movement for this cycle only.
      q.simt_active = simt_live ? 1u : 0u;
      q.simt_blocked = simt_form_refuse > d_sref ? 1u : 0u;
      q.simt_progress = (simt_gen > d_sgen || simt_issued > d_siss || simt_done > d_sdone) ? 1u : 0u;
      q.bulk_active = eng_->idle() ? 0u : 1u;
      q.bulk_blocked =
          (bs.slot_stall > d_bstall || bs.idx_buf_stall > d_bidxstall) ? 1u : 0u;
      q.bulk_progress =
          (bs.tgt_req_issued > d_biss || bs.tgt_req_done > d_btgt ||
           bs.idx_lines_done > d_bidx || bs.elems_done > d_belem)
              ? 1u
              : 0u;
      cyc_->v.push_back(q);
      // Lifetime is tallied inside `pool.give` from the stamp `pool.take` left,
      // so a slot released and re-claimed in the same cycle is still two spans.
    }

    if (!simt_work && eng_->idle() && mem_->idle()) break;
    // M27-P2: liveness is a correctness invariant. The bound is the model's own
    // worst-case service time (MemorySystem::live_limit), so a stranded request
    // is reported in ~55 K cycles instead of running to the cycle guard.
    if (!live_check(cycle, simt_work || !eng_->idle() || !mem_->idle())) break;
    if (cycle >= cfg_.max_cycles) {
      fail("cycle limit exceeded (deadlock guard)");
      break;
    }
  }

  mem_->finish(cycle);
  res.cycles = cycle;

  // ---- per-path conservation ----------------------------------------------
  u64 held_end = 0;
  for (const auto& c : cores) held_end += c.held.size() + (c.cur_lines.size() - c.cur_pos);
  if (simt_gen != simt_issued + held_end) fail("SIMT: generated != issued + unissued");
  if (simt_issued != simt_done) fail("SIMT: issued != completed (work left in flight)");
  if (pool.residency() != 0) fail("slots still held when the run ended");
  if (!mem_->idle()) fail("the hierarchy still had requests in flight");

  const BulkStats& b = eng_->stats();
  if (b.desc_submitted != (w.armed ? 1u : 0u)) fail("expected exactly one descriptor");
  if (b.desc_completed != (w.armed ? 1u : 0u)) fail("the descriptor did not complete");
  if (b.desc_rejected) fail("the descriptor was rejected");
  if (b.addr_mismatch) fail("an engine address disagreed with its index value");
  if (b.residency_violations) fail("engine residency exceeded the budget");
  if (b.win_alloc != static_cast<u64>(b.win_recs.size()))
    fail("window records lost an allocation");
  {
    u64 rec_sum = 0;
    for (const auto& wr : b.win_recs) rec_sum += wr.n;
    if (rec_sum != b.win_entries) fail("window records lost an admitted element");
  }
  if (b.win_incomplete_max > 1) fail("two windows were unsealed at once");
  if (cfg_.effective_win_policy() == 1 && b.win_live_max > 1) fail("E2 kept two windows live");
  if (b.elems_done + b.addr_faults != bulk_elems) fail("bulk elements not all completed");
  if (w.armed) {
    const u64 per = LINE_BYTES / w.desc.index_bytes;
    if (b.idx_lines_req != (bulk_elems + per - 1) / per)
      fail("index lines != ceil(bulk elements / indices per line)");
  } else if (b.idx_lines_req || b.tgt_req_gen || b.desc_submitted) {
    fail("acquisition happened with no bulk-class element");
  }

  // ---- counters ------------------------------------------------------------
  s.inc("requests_generated", b.desc_submitted + b.idx_lines_req + b.tgt_req_gen);
  s.inc("issued", b.desc_submitted + b.idx_lines_req + b.tgt_req_issued);
  s.inc("completed", b.desc_completed + b.idx_lines_done + b.tgt_req_done);
  s.set("cycles", cycle);
  s.set("requests_unissued_end", held_end);
  s.set("simt_requests_generated", simt_gen);
  s.set("simt_issued", simt_issued);
  s.set("simt_completed", simt_done);
  s.set("simt_completion_hash", hash & 0xffffffffull);
  s.set("simt_peak_slot", peak_simt);
  s.set("bulk_peak_slot", peak_bulk);
  s.set("peak_total_outstanding", peak_total);
  s.set("simt_staging_max", staging_max);
  // M15: the two paths' completion spans, and the dispatch partition stated as
  // an identity. A non-zero `dispatch_unaccounted` is a routing defect, not a
  // performance observation: it can only be non-zero if an element is in no
  // class, in two classes, or was not completed by the path that owns it.
  s.set("simt_first_done_cycle", simt_first_cycle == NONE ? 0 : simt_first_cycle);
  s.set("simt_last_done_cycle", simt_last_cycle);
  s.set("bulk_first_done_cycle", bulk_first_cycle == NONE ? 0 : bulk_first_cycle);
  s.set("bulk_last_done_cycle", bulk_last_cycle);
  s.set("win_fused_active", (cfg_.win_fused && cfg_.batch_window) ? 1 : 0);
  {
    u64 unacc = 0;
    if (simt_elems + bulk_elems != tr_->acc.size()) ++unacc;          // no class
    if (instr_simt + instr_bulk != tr_->n_instr) ++unacc;            // instruction split
    if (b.elems_done + b.addr_faults != bulk_elems) ++unacc;         // bulk short/long
    if (simt_done != simt_gen) ++unacc;                              // SIMT short/long
    s.set("dispatch_unaccounted", unacc);
    s.set("dispatch_partition_ok", unacc ? 0 : 1);
  }
  // M9 diagnostic counters: allocation, refusal attribution, releases. They are
  // exported so a run can be quoted without replaying it under a trace.
  s.set("slot_pool_slots", slots);
  s.set("simt_form_refuse", simt_form_refuse);
  s.set("simt_ref_simt_sum", simt_ref_simt);
  s.set("simt_ref_bulk_sum", simt_ref_bulk);
  s.set("simt_ref_free_sum", simt_ref_free);
  s.set("bulk_slot_take", b.slot_take);
  s.set("bulk_slot_give", b.slot_give);
  s.set("bulk_tgt_refuse_noslot", b.tgt_refuse_noslot);
  s.set("bulk_tgt_refuse_reserve", b.tgt_refuse_reserve);
  s.set("bulk_ref_simt_sum", b.tgt_refuse_simt_held);
  s.set("bulk_ref_bulk_sum", b.tgt_refuse_bulk_held);
  s.set("bulk_ref_free_sum", b.tgt_refuse_free);
  s.set("bulk_win_backlog", b.win_backlog);
  // M9 capacity diagnostic: the index pipeline and the readiness attribution.
  s.set("bulk_idx_sub", b.idx_sub);
  s.set("bulk_idx_consumed", b.idx_consumed);
  s.set("bulk_idx_wait_cap", b.idx_wait_cap);
  s.set("bulk_idx_wait_free_sum", b.idx_wait_free_sum);
  s.set("bulk_slot_stall_free_sum", b.slot_stall_free_sum);
  // One hash for the run: the engine's when any bulk work exists (element-keyed,
  // so comparable with arm B), otherwise the SIMT front-end's (slot-keyed, so
  // comparable with arm A). The other path's hash is reported separately above
  // and is always carried in `simt_completion_hash` / `bulk_*` counters.
  s.set("completion_hash",
        (bulk_elems ? b.completion_hash : hash) & 0xffffffffull);
  res.completion_hash = s.get("completion_hash");

  s.set("bulk_desc_submitted", b.desc_submitted);
  s.set("bulk_desc_completed", b.desc_completed);
  s.set("bulk_desc_rejected", b.desc_rejected);
  s.set("bulk_idx_lines_req", b.idx_lines_req);
  s.set("bulk_idx_lines_done", b.idx_lines_done);
  s.set("bulk_idx_elems_done", b.idx_elems_done);
  s.set("bulk_tgt_req_gen", b.tgt_req_gen);
  s.set("bulk_tgt_req_issued", b.tgt_req_issued);
  s.set("bulk_tgt_req_done", b.tgt_req_done);
  s.set("bulk_elems_total", b.elems_total);
  s.set("bulk_elems_done", b.elems_done);
  s.set("bulk_addr_faults", b.addr_faults);
  s.set("bulk_addr_mismatch", b.addr_mismatch);
  s.set("bulk_residency_violations", b.residency_violations);
  s.set("bulk_lines_issued", b.lines_issued);
  s.set("bulk_dedup_merged", b.dedup_merged);
  s.set("bulk_batches", b.batches);
  s.set("bulk_seals_full", b.seals_full);
  s.set("bulk_seals_end", b.seals_end);
  s.set("bulk_seals_deadline", b.seals_deadline);
  s.set("bulk_win_entries", b.win_entries);
  s.set("bulk_win_occ_max", b.win_occ_max);
  s.set("bulk_dir_occ_max", b.dir_occ_max);
  s.set("bulk_sched_idle", b.sched_idle);
  s.set("bulk_sched_defer", b.sched_defer);
  s.set("bulk_sched_opens", b.sched_opens);
  s.set("bulk_idx_ooo_gen", b.idx_ooo_gen);
  s.set("bulk_idx_buf_stall", b.idx_buf_stall);
  s.set("bulk_idx_wait_stall", b.idx_wait_stall);
  s.set("bulk_buf_max", b.buf_max);
  s.set("bulk_slot_stall", b.slot_stall);
  s.set("bulk_win_refuse", b.win_refuse);
  s.set("bulk_idx_refuse", b.idx_refuse);
  s.set("bulk_issue_stall", b.issue_stall);
  s.set("bulk_ag_cycles", b.ag_cycles);
  s.set("bulk_first_tgt_issue", b.first_tgt_issue == NONE ? 0 : b.first_tgt_issue);
  s.set("bulk_last_fill", b.last_fill == NONE ? 0 : b.last_fill);
  s.set("bulk_idx_bytes", cfg_.idx_mode ? 2ull * std::min<u32>(cfg_.idx_buf_lines ? cfg_.idx_buf_lines : 1u, 64u) * 21 + 32 : 0);
  s.set("bulk_win_bytes",
        cfg_.batch_window
            ? ((cfg_.org_overlap && cfg_.effective_win_policy() != 1) ? 2ull : 1ull) *
                  (static_cast<u64>(cfg_.batch_window) * 8 +
                   static_cast<u64>(cfg_.page_dir) * 16 + b.sched_bytes)
            : 0);
  // The remaining engine counters, exported with the same names and the same
  // expressions `run_bulk` uses, so a hybrid run and a bulk-only run can be
  // compared key for key (gate G2) rather than on a hand-picked subset.
  s.set("bulk_desc_cycles_sum", b.desc_cycles_sum);
  s.set("bulk_desc_cycles_max", b.desc_cycles_max);
  s.set("bulk_desc_cycles_mean", b.desc_completed ? b.desc_cycles_sum / b.desc_completed : 0);
  s.set("bulk_desc_fetch_cycles_mean",
        b.desc_completed ? b.desc_fetch_cycles_sum / b.desc_completed : 0);
  s.set("bulk_idx_lines_ooo", b.idx_lines_ooo);
  s.set("bulk_idx_out_max", b.idx_out_max);
  s.set("bulk_idx_window", cfg_.idx_window ? cfg_.idx_window : cfg_.R);
  s.set("bulk_idx_elems_req", b.idx_elems_req);
  s.set("bulk_idx_first_done", b.idx_first_done == NONE ? 0 : b.idx_first_done);
  s.set("bulk_tgt_addr_gen", b.tgt_addr_gen);
  s.set("bulk_tgt_ooo", b.tgt_ooo);
  s.set("bulk_tgt_first_gen", b.tgt_first_gen == NONE ? 0 : b.tgt_first_gen);
  s.set("bulk_buf_occ", b.buf_occ);
  s.set("bulk_unissued_max", b.unissued_max);
  s.set("bulk_buf_full_stall", b.buf_full_stall);
  s.set("bulk_sched_on", cfg_.page_sched ? 1 : 0);
  s.set("bulk_sched_cohorts", cfg_.page_sched ? cfg_.page_cohorts : 0);
  s.set("bulk_sched_retry", cfg_.park_retry);
  s.set("bulk_sched_switches", b.sched_switches);
  s.set("bulk_sched_probes", b.sched_probes);
  s.set("bulk_sched_cycles", b.sched_cycles);
  s.set("bulk_sched_scan", b.sched_scan);
  s.set("bulk_sched_bytes", b.sched_bytes);
  s.set("bulk_idx_mode", cfg_.idx_mode);
  s.set("bulk_idx_buf_lines",
        cfg_.idx_mode ? std::min<u32>(cfg_.idx_buf_lines ? cfg_.idx_buf_lines : 1u, 64u) : 0);
  s.set("bulk_idx_mshr", cfg_.idx_mode >= 3 ? cfg_.idx_mshr : 0);
  s.set("bulk_idx_tile_swaps", b.idx_tile_swaps);
  s.set("bulk_idx_ready_max", b.idx_ready_max);
  s.set("bulk_idx_resident_max", b.idx_out_max);
  s.set("bulk_idx_oracle", cfg_.idx_oracle ? 1 : 0);
  s.set("bulk_ag_lanes", cfg_.ag_lanes);
  s.set("bulk_eng_issue", cfg_.eng_issue);
  s.set("bulk_ag_util_x1000",
        b.ag_cycles && cfg_.ag_lanes
            ? b.tgt_addr_gen * 1000ull / (static_cast<u64>(b.ag_cycles) * cfg_.ag_lanes)
            : 0);
  s.set("bulk_issue_monotonic_violations", b.issue_monotonic_violations);
  s.set("bulk_desc_line_bytes", b.desc_submitted * LINE_BYTES);
  s.set("bulk_idx_line_bytes", b.idx_lines_req * LINE_BYTES);
  s.set("bulk_tgt_line_bytes", b.tgt_req_gen * LINE_BYTES);
  s.set("bulk_win_window", cfg_.batch_window);
  s.set("bulk_win_page_dir", cfg_.page_dir);
  s.set("bulk_win_org_lanes", cfg_.org_lanes);
  s.set("bulk_win_fanout_lanes", cfg_.fanout_lanes);
  s.set("bulk_win_dedup", cfg_.dedup ? 1 : 0);
  s.set("bulk_win_page_order", cfg_.page_order ? 1 : 0);
  s.set("bulk_org_policy", cfg_.bulk_org);
  s.set("bulk_org_runs", b.org_runs);
  // ---- M23-G6A: translation-first, DRAM-second ----
  s.set("bulk_g6a_windows", b.g6a_windows);
  s.set("bulk_g6a_tr_requests", b.g6a_tr_reqs);
  s.set("bulk_g6a_tr_completed", b.g6a_tr_done);
  s.set("bulk_g6a_tr_cycles", b.g6a_tr_cycles);
  s.set("bulk_g6a_dr_cycles", b.g6a_dr_cycles);
  s.set("bulk_g6a_tr_stall_cycles", b.g6a_tr_stall);
  s.set("bulk_g6a_barrier_cycles", b.g6a_barrier_cycles);
  s.set("bulk_g6a_pa_capacity", b.g6a_pa_capacity);
  s.set("bulk_g6a_pa_need_max", b.g6a_pa_need_max);
  s.set("bulk_g6a_pa_occ_max", b.g6a_pa_occ_max);
  s.set("bulk_g6a_pa_bytes", b.g6a_pa_bytes);
  s.set("bulk_g6a_dram_idle_bank_cycles_tr", b.g6a_dram_idle_tr);
  s.set("bulk_g6a_runs_released", b.g6a_runs_released);
  s.set("bulk_g6a_tr_wait_dram", b.g6a_tr_wait_dram);
  s.set("bulk_g6a_tr_wait_max", b.g6a_tr_wait_max);
  // ---- M23-G6B: streaming translation/DRAM decoupling ----
  s.set("bulk_g6b_windows", b.g6b_windows);
  s.set("bulk_g6b_tr_requests", b.g6b_tr_reqs);
  s.set("bulk_g6b_tr_completed", b.g6b_tr_done);
  s.set("bulk_g6b_pa_capacity", b.g6b_capacity);
  s.set("bulk_g6b_pa_bytes", b.g6b_pa_bytes);
  s.set("bulk_g6b_ready_peak", b.g6b_ready_peak);
  s.set("bulk_g6b_ready_occ_sum", b.g6b_ready_occ_sum);
  for (u32 hi = 0; hi < 13; ++hi)
    s.set("bulk_g6b_ready_h" + std::to_string(hi), b.g6b_ready_hist[hi]);
  s.set("bulk_g6b_backpressure_cycles", b.g6b_backpressure);
  s.set("bulk_g6b_full_cycles", b.g6b_full_events);
  s.set("bulk_g6b_tr_stall_cycles", b.g6b_tr_stall);
  s.set("bulk_g6b_no_ready_cycles", b.g6b_no_ready);
  s.set("bulk_g6b_tr_active_cycles", b.g6b_tr_active);
  s.set("bulk_g6b_tgt_out_cycles", b.g6b_tgt_out);
  s.set("bulk_g6b_overlap_cycles", b.g6b_overlap);
  s.set("bulk_g6b_runs_released", b.g6b_runs_released);
  s.set("bulk_g6f_scan_calls", b.g6f_scan_calls);
  s.set("bulk_g6f_scan_entries", b.g6f_scan_entries);
  s.set("bulk_g6f_vis_cells_sum", b.g6f_vis_cells_sum);
  s.set("bulk_g6f_vis_cells_max", b.g6f_vis_cells_max);
  s.set("bulk_g6f_vis_hit_sum", b.g6f_vis_hit_sum);
  s.set("bulk_g6f_pick_hit", b.g6f_pick_hit);
  s.set("bulk_g6f_pick_frontier", b.g6f_pick_frontier);
  // ---- M23-G6G: physical (bank,row) group release ----
  s.set("bulk_g6g_groups_opened", b.g6g_groups_opened);
  s.set("bulk_g6g_groups_closed", b.g6g_groups_closed);
  s.set("bulk_g6g_group_runs", b.g6g_group_runs);
  s.set("bulk_g6g_group_size_sum", b.g6g_group_size_sum);
  s.set("bulk_g6g_group_size_max", b.g6g_group_size_max);
  s.set("bulk_g6g_group_drain_sum", b.g6g_group_drain_sum);
  s.set("bulk_g6g_group_drain_max", b.g6g_group_drain_max);
  for (u32 hi = 0; hi < 6; ++hi)
    s.set("bulk_g6g_group_h" + std::to_string(hi), b.g6g_group_hist[hi]);
  s.set("bulk_g6g_same_cell", b.g6g_same_cell);
  s.set("bulk_g6g_switch_cell", b.g6g_switch_cell);
  s.set("bulk_g6g_bank_alt", b.g6g_bank_alt);
  s.set("bulk_g6g_revisit", b.g6g_revisit);
  s.set("bulk_g6g_sel_mismatch", b.g6g_sel_mismatch);
  s.set("bulk_g6g_scan_calls", b.g6g_scan_calls);
  s.set("bulk_g6g_scan_entries", b.g6g_scan_entries);
  // ---- M23-G6H: complete-eligible-window audit ----
  s.set("bulk_g6h_decisions", b.g6h_decisions);
  s.set("bulk_g6h_exam_sum", b.g6h_exam_sum);
  s.set("bulk_g6h_exam_max", b.g6h_exam_max);
  s.set("bulk_g6h_elig_sum", b.g6h_elig_sum);
  s.set("bulk_g6h_elig_max", b.g6h_elig_max);
  s.set("bulk_g6h_stale_sum", b.g6h_stale_sum);
  s.set("bulk_g6h_untr_sum", b.g6h_untr_sum);
  s.set("bulk_g6h_untr_max", b.g6h_untr_max);
  s.set("bulk_g6h_groups_sum", b.g6h_groups_sum);
  s.set("bulk_g6h_groups_max", b.g6h_groups_max);
  s.set("bulk_g6h_considered_sum", b.g6h_considered_sum);
  s.set("bulk_g6h_deferred_sum", b.g6h_deferred_sum);
  s.set("bulk_g6h_inv_viol", b.g6h_inv_viol);
  // ---- M23-G6I: continuous translation/DRAM pipeline ----
  s.set("bulk_g6i_samples", b.g6i_samples);
  s.set("bulk_g6i_samp_pool_sum", b.g6i_samp_pool_sum);
  s.set("bulk_g6i_samp_pool_max", b.g6i_samp_pool_max);
  s.set("bulk_g6i_vpn_sum", b.g6i_vpn_sum);
  s.set("bulk_g6i_vpn_max", b.g6i_vpn_max);
  s.set("bulk_g6i_cells_sum", b.g6i_cells_sum);
  s.set("bulk_g6i_cells_max", b.g6i_cells_max);
  s.set("bulk_g6i_xpage_cells_sum", b.g6i_xpage_cells_sum);
  s.set("bulk_g6i_xpage_cells_max", b.g6i_xpage_cells_max);
  s.set("bulk_g6i_xpage_entries_sum", b.g6i_xpage_entries_sum);
  s.set("bulk_g6i_untr_sum", b.g6i_untr_sum);
  s.set("bulk_g6i_untr_max", b.g6i_untr_max);
  s.set("bulk_g6i_pages_open_sum", b.g6i_pages_open_sum);
  s.set("bulk_g6i_pages_open_max", b.g6i_pages_open_max);
  s.set("bulk_g6i_probe_max", b.g6i_probe_max);
  s.set("bulk_g6i_rel_cycles", b.g6i_rel_cycles);
  s.set("bulk_g6i_rel_with_tr", b.g6i_rel_with_tr);
  s.set("bulk_m24_rel_pairs", b.m24_rel_pairs);
  s.set("bulk_m24_rel_inversions", b.m24_rel_inversions);
  s.set("bulk_g6i_pool_empty", b.g6i_pool_empty);
  s.set("bulk_g6i_pool_over", b.g6i_pool_over);
  s.set("bulk_g6i_gap_n", b.g6i_gap_n);
  s.set("bulk_g6i_gap_sum", b.g6i_gap_sum);
  s.set("bulk_g6i_gap_max", b.g6i_gap_max);
  s.set("bulk_g6i_rel_vpn_switch", b.g6i_rel_vpn_switch);
  s.set("bulk_g6i_rel_same_vpn", b.g6i_rel_same_vpn);
  s.set("bulk_g6b_release_pops", b.g6b_rel_pops);
  s.set("bulk_g6b_release_blocked_cycles", b.g6b_rel_blocked);
  s.set("bulk_g6b_release_slot_stall", b.g6b_rel_slot_stall);
  s.set("bulk_g6b_tr_wait_dram", b.g6b_tr_wait_dram);
  s.set("bulk_g6b_tr_wait_max", b.g6b_tr_wait_max);
  // ---- M23-G6C: P0-style admission deferral on the G6B pipeline -------------
  s.set("bulk_g6c_windows", b.g6c_windows);
  s.set("bulk_g6c_admit_attempts", b.g6c_admit_attempts);
  s.set("bulk_g6c_admit_accepted", b.g6c_admit_ok);
  s.set("bulk_g6c_refused", b.g6c_refused);
  s.set("bulk_g6c_refuse_parked", b.g6c_refuse_parked);
  s.set("bulk_g6c_refuse_full", b.g6c_refuse_full);
  s.set("bulk_g6c_refuse_other", b.g6c_refuse_other);
  s.set("bulk_g6c_defer_events", b.g6c_defer_events);
  s.set("bulk_g6c_deferred_readmitted", b.g6c_defer_readmit);
  s.set("bulk_g6c_defer_then_other_admit", b.g6c_defer_other_admit);
  s.set("bulk_g6c_defer_age_sum", b.g6c_defer_age_sum);
  s.set("bulk_g6c_defer_age_max", b.g6c_defer_age_max);
  s.set("bulk_g6c_defer_depth_sum", b.g6c_defer_depth_sum);
  s.set("bulk_g6c_defer_depth_n", b.g6c_defer_depth_n);
  s.set("bulk_g6c_defer_depth_max", b.g6c_defer_depth_max);
  s.set("bulk_g6c_cohorts_opened", b.g6c_cohorts_opened);
  s.set("bulk_g6c_admit_idle_cycles", b.g6c_admit_idle);
  s.set("bulk_g6c_dup_submit_blocked", b.g6c_dup_submit);
  s.set("bulk_win_total_max", b.win_total_max);
  s.set("bulk_win_occ_sum", b.win_occ_sum);
  s.set("bulk_org_cycles", b.org_cycles);
  s.set("bulk_fanout_cycles", b.fanout_cycles);
  s.set("bulk_fanout_elems", b.fanout_elems);
  s.set("bulk_fanout_max_backlog", b.fanout_max_backlog);
  s.set("bulk_win_full_stall", b.win_full_stall);
  s.set("bulk_dir_collisions", b.dir_collisions);
  s.set("bulk_first_desc_done", b.first_desc_done == NONE ? 0 : b.first_desc_done);
  s.set("bulk_first_idx_issue", b.first_idx_issue == NONE ? 0 : b.first_idx_issue);
  s.set("bulk_last_idx_issue", b.last_idx_issue == NONE ? 0 : b.last_idx_issue);
  s.set("bulk_last_idx_done", b.last_idx_done == NONE ? 0 : b.last_idx_done);
  s.set("bulk_first_admit", b.first_admit == NONE ? 0 : b.first_admit);
  s.set("bulk_first_seal", b.first_seal == NONE ? 0 : b.first_seal);
  s.set("bulk_last_seal", b.last_seal == NONE ? 0 : b.last_seal);
  s.set("bulk_last_tgt_issue", b.last_tgt_issue == NONE ? 0 : b.last_tgt_issue);
  s.set("bulk_busy_at_first_tgt", b.busy_at_first_tgt);
  s.set("bulk_busy_at_last_tgt", b.busy_at_last_tgt);
  s.set("bulk_busy_at_last_fill", b.busy_at_last_fill);
  s.set("bulk_starved_at_first_tgt", b.starved_at_first_tgt);
  s.set("bulk_oracle_elems", b.oracle_elems);

  export_win_records(b, cfg_, s);
  collect_mem_stats(*mem_, cfg_, s);
  collect_lat_stats(*mem_, res);
  s.set("conservation_ok", res.ok ? 1 : 0);
  s.inc("conservation_violations", res.violations.size());
  return res;
}

RunResult Sim::run_bulk() {
  std::string err;
  const BulkWorkload w = build_bulk_workload(cfg_, *tr_, err);
  if (!err.empty()) {
    RunResult res;
    res.ok = false;
    res.violations.push_back(err);
    return res;
  }
  return run_bulk(w);
}

RunResult Sim::run_bulk(const BulkWorkload& w) {
  RunResult res;
  Stats& s = res.s;

  // M27-P5: the engine's requests enter one SM. With one core that SM is
  // unambiguous. With several, an unnamed SM would mean every engine request
  // enters core 0's private L1 -- a different machine from the one the
  // configuration describes -- so the run is refused rather than silently
  // redefined. Reported, never thrown: a configuration error must fail the run.
  eng_ = std::make_unique<BulkEngine>(cfg_, *mem_);
  if (cfg_.cores > 1 && cfg_.eng_core == 0xffffffffu && cfg_.mem_path == "l1") {
    res.ok = false;
    res.violations.push_back(
        "Bulk is single-core in this model: with cores > 1 and mem_path = l1, set "
        "eng_core to the SM whose L1 the engine shares (a multi-SM engine is not modelled)");
    s.set("conservation_ok", 0);
    return res;
  }
  s.set("cores", cfg_.cores);
  s.set("slots_total", cfg_.R);
  s.set("trace_accesses", tr_->acc.size());
  s.set("trace_instr", tr_->n_instr);
  s.set("trace_lines", tr_->lines_real);
  s.set("trace_pages", tr_->pages_real);
  s.set("mem_path", cfg_.mem_path == "l2" ? 2 : (cfg_.mem_path == "l1" ? 1 : 0));

  // The workload is applied to this arm's own fresh memory image, then armed.
  w.apply(*mem_);
  if (cfg_.idx_oracle) eng_->set_index_oracle(&w.index_values);
  if (w.armed) eng_->submit(w.desc);

  u64 cycle = 0;
  for (;; ++cycle) {
    eng_->tick(cycle);
    if (cyc_) {  // M9 diagnostic: pure observation
      const BulkStats& bs = eng_->stats();
      CycleSample q;
      q.bulk_done = static_cast<u32>(bs.elems_done);
      q.bulk_res = static_cast<u32>(eng_->residency());
      q.mem_inflight = static_cast<u32>(mem_->inflight_count());
      q.issued = static_cast<u32>(bs.desc_submitted + bs.idx_lines_req + bs.tgt_req_issued);
      q.bulk_slot_stall = static_cast<u32>(bs.slot_stall);
      q.bulk_idx_wait = static_cast<u32>(bs.idx_wait_stall);
      q.bulk_buf_full = static_cast<u32>(bs.buf_full_stall);
      q.bulk_win_full = static_cast<u32>(bs.win_full_stall);
      q.bulk_alloc = static_cast<u32>(bs.slot_take);
      q.bulk_release = static_cast<u32>(bs.slot_give);
      q.bulk_tgt_issue = static_cast<u32>(bs.tgt_req_issued);
      q.bulk_tgt_done = static_cast<u32>(bs.tgt_req_done);
      q.bulk_idx_done = static_cast<u32>(bs.idx_lines_done);
      q.bulk_elems = static_cast<u32>(bs.elems_done);
      q.idx_sub = static_cast<u32>(bs.idx_sub);
      q.idx_elems = static_cast<u32>(bs.idx_consumed);
      q.addr_gen = static_cast<u32>(bs.tgt_addr_gen);
      q.idx_ready = static_cast<u32>(eng_->idx_ready_elems());
      q.idx_ready_lines = static_cast<u32>(eng_->idx_ready_lines());
      q.idx_ring = static_cast<u32>(eng_->idx_ring_lines());
      q.idx_res = static_cast<u32>(eng_->idx_resident_lines());
      q.tgt_backlog = static_cast<u32>(bs.win_backlog);
      q.tgt_unissued = static_cast<u32>(eng_->tgt_unissued());
      q.dram_q = static_cast<u32>(mem_->dram_queue());
      q.dram_elig = static_cast<u32>(mem_->dram_elig_now());
      q.ag_remain = static_cast<u32>(eng_->ag_remaining());
      q.win_fill = eng_->win_filling() ? 1u : 0u;
      q.bulk_defer = static_cast<u32>(bs.sched_defer);
      q.winrefuse = static_cast<u32>(bs.win_refuse);
      cyc_->v.push_back(q);
    }
    if (eng_->idle()) break;
    // M27-P2: same permanent liveness invariant as every other run loop.
    {
      const u64 thr = cfg_.stall_abort ? cfg_.stall_abort : mem_->live_limit();
      if (mem_->liveness_tripped(cycle, thr)) {
        res.ok = false;
        res.violations.push_back("liveness: no admission or retirement for " +
                                 std::to_string(cycle - mem_->last_progress_cycle()) +
                                 " cycles (bound " + std::to_string(thr) + "); " +
                                 mem_->outstanding_report());
        break;
      }
    }
    if (cycle >= cfg_.max_cycles) {
      res.ok = false;
      res.violations.push_back("cycle limit exceeded (deadlock guard)");
      break;
    }
  }

  mem_->finish(cycle);
  res.cycles = cycle;

  const BulkStats& b = eng_->stats();
  const u64 generated = b.desc_submitted + b.idx_lines_req + b.tgt_req_gen;
  const u64 issued = b.desc_submitted + b.idx_lines_req + b.tgt_req_issued;
  const u64 completed = b.desc_completed + b.idx_lines_done + b.tgt_req_done;

  res.completion_hash = b.completion_hash;
  res.lines_per_cycle = cycle ? double(b.tgt_req_done) / double(cycle) : 0.0;
  res.gbs = cycle ? double(mem_->dram.bytes) / double(cycle) * 400e6 / 1e9 : 0.0;

  s.set("cycles", cycle);
  s.set("requests_generated", generated);  // descriptor + index + target lines
  s.set("issued", issued);
  s.set("completed", completed);
  s.set("requests_unissued_end", eng_->residency() - mem_->inflight_count());
  s.set("completion_hash", b.completion_hash & 0xffffffffull);
  s.set("replays", mem_->replays);

  // Engine counters. Names are stable so reports can quote them directly.
  s.set("bulk_desc_submitted", b.desc_submitted);
  s.set("bulk_desc_completed", b.desc_completed);
  s.set("bulk_desc_rejected", b.desc_rejected);
  s.set("bulk_desc_cycles_sum", b.desc_cycles_sum);
  s.set("bulk_desc_cycles_max", b.desc_cycles_max);
  s.set("bulk_desc_cycles_mean", b.desc_completed ? b.desc_cycles_sum / b.desc_completed : 0);
  s.set("bulk_desc_fetch_cycles_mean",
        b.desc_completed ? b.desc_fetch_cycles_sum / b.desc_completed : 0);
  s.set("bulk_idx_lines_req", b.idx_lines_req);
  s.set("bulk_idx_lines_done", b.idx_lines_done);
  s.set("bulk_idx_lines_ooo", b.idx_lines_ooo);
  s.set("bulk_idx_out_max", b.idx_out_max);
  s.set("bulk_idx_window", cfg_.idx_window ? cfg_.idx_window : cfg_.R);
  s.set("bulk_idx_elems_req", b.idx_elems_req);
  s.set("bulk_idx_elems_done", b.idx_elems_done);
  s.set("bulk_idx_first_done", b.idx_first_done == NONE ? 0 : b.idx_first_done);
  s.set("bulk_tgt_addr_gen", b.tgt_addr_gen);
  s.set("bulk_tgt_req_gen", b.tgt_req_gen);
  s.set("bulk_tgt_req_issued", b.tgt_req_issued);
  s.set("bulk_tgt_req_done", b.tgt_req_done);
  s.set("bulk_tgt_ooo", b.tgt_ooo);
  s.set("bulk_addr_mismatch", b.addr_mismatch);
  s.set("bulk_tgt_first_gen", b.tgt_first_gen == NONE ? 0 : b.tgt_first_gen);
  s.set("bulk_elems_total", b.elems_total);
  s.set("bulk_elems_done", b.elems_done);
  s.set("bulk_addr_faults", b.addr_faults);
  s.set("bulk_buf_occ", b.buf_occ);
  s.set("bulk_buf_max", b.buf_max);
  s.set("bulk_unissued_max", b.unissued_max);
  s.set("bulk_buf_full_stall", b.buf_full_stall);
  s.set("bulk_idx_wait_stall", b.idx_wait_stall);
  s.set("bulk_issue_stall", b.issue_stall);
  s.set("bulk_slot_stall", b.slot_stall);
  s.set("bulk_slot_take", b.slot_take);
  s.set("bulk_slot_give", b.slot_give);
  s.set("bulk_win_backlog", b.win_backlog);
  // M9 capacity diagnostic: the index pipeline and the readiness attribution.
  s.set("bulk_idx_sub", b.idx_sub);
  s.set("bulk_idx_consumed", b.idx_consumed);
  s.set("bulk_idx_wait_cap", b.idx_wait_cap);
  s.set("bulk_idx_wait_free_sum", b.idx_wait_free_sum);
  s.set("bulk_slot_stall_free_sum", b.slot_stall_free_sum);
  s.set("bulk_win_refuse", b.win_refuse);
  s.set("bulk_sched_on", cfg_.page_sched ? 1 : 0);
  s.set("bulk_sched_cohorts", cfg_.page_sched ? cfg_.page_cohorts : 0);
  s.set("bulk_sched_retry", cfg_.park_retry);
  s.set("bulk_sched_opens", b.sched_opens);
  s.set("bulk_sched_switches", b.sched_switches);
  s.set("bulk_sched_defer", b.sched_defer);
  s.set("bulk_sched_idle", b.sched_idle);
  s.set("bulk_sched_probes", b.sched_probes);
  s.set("bulk_sched_cycles", b.sched_cycles);
  s.set("bulk_sched_scan", b.sched_scan);
  s.set("bulk_sched_bytes", b.sched_bytes);
  // M8: index acquisition. `bulk_idx_bytes` is the tile SRAM the design owns:
  // per line a VA descriptor (8), element count and consumed count (2+2) and a
  // ready bit (1), plus a header per tile. Charged, not free.
  const u64 tile_lines = std::min<u32>(cfg_.idx_buf_lines ? cfg_.idx_buf_lines : 1u, 64u);
  s.set("bulk_idx_mode", cfg_.idx_mode);
  s.set("bulk_idx_buf_lines", cfg_.idx_mode ? tile_lines : 0);
  s.set("bulk_idx_mshr", cfg_.idx_mode >= 3 ? cfg_.idx_mshr : 0);
  // Per slot: element base (8) + line (8) + count (2) + used (2) + ready (1).
  s.set("bulk_idx_bytes", cfg_.idx_mode ? 2 * tile_lines * 21 + 32 : 0);
  s.set("bulk_idx_tile_swaps", b.idx_tile_swaps);
  s.set("bulk_idx_buf_stall", b.idx_buf_stall);
  s.set("bulk_idx_ooo_gen", b.idx_ooo_gen);
  s.set("bulk_idx_ready_max", b.idx_ready_max);
  s.set("bulk_idx_resident_max", b.idx_out_max);
  s.set("bulk_idx_refuse", b.idx_refuse);
  s.set("bulk_ag_cycles", b.ag_cycles);
  s.set("bulk_ag_util_x1000",
        b.ag_cycles && cfg_.ag_lanes ? b.tgt_addr_gen * 1000ull / (b.ag_cycles * cfg_.ag_lanes) : 0);
  s.set("bulk_ag_lanes", cfg_.ag_lanes);
  s.set("bulk_eng_issue", cfg_.eng_issue);
  s.set("bulk_residency_violations", b.residency_violations);
  s.set("bulk_issue_monotonic_violations", b.issue_monotonic_violations);
  // Traffic split: bytes the engine *asked* for, by stream (line requests x 64 B).
  // Aggregate `dram_bytes` remains authoritative for what the hierarchy moved.
  s.set("bulk_desc_line_bytes", b.desc_submitted * LINE_BYTES);
  s.set("bulk_idx_line_bytes", b.idx_lines_req * LINE_BYTES);
  s.set("bulk_tgt_line_bytes", b.tgt_req_gen * LINE_BYTES);

  // M6 window. Reported even when the window is off (= 0) so a table can carry
  // both regimes without changing shape.
  // E2 keeps one window live, so it owns one buffer: the modelled SRAM halves.
  const u64 bufs = (cfg_.org_overlap && cfg_.effective_win_policy() != 1) ? 2 : 1;
  s.set("bulk_win_window", cfg_.batch_window);
  s.set("bulk_win_page_dir", cfg_.page_dir);
  s.set("bulk_win_org_lanes", cfg_.org_lanes);
  s.set("bulk_win_fanout_lanes", cfg_.fanout_lanes);
  s.set("bulk_win_dedup", cfg_.dedup ? 1 : 0);
  s.set("bulk_win_page_order", cfg_.page_order ? 1 : 0);
  s.set("bulk_org_policy", cfg_.bulk_org);
  s.set("bulk_org_runs", b.org_runs);
  // ---- M23-G6A: translation-first, DRAM-second ----
  s.set("bulk_g6a_windows", b.g6a_windows);
  s.set("bulk_g6a_tr_requests", b.g6a_tr_reqs);
  s.set("bulk_g6a_tr_completed", b.g6a_tr_done);
  s.set("bulk_g6a_tr_cycles", b.g6a_tr_cycles);
  s.set("bulk_g6a_dr_cycles", b.g6a_dr_cycles);
  s.set("bulk_g6a_tr_stall_cycles", b.g6a_tr_stall);
  s.set("bulk_g6a_barrier_cycles", b.g6a_barrier_cycles);
  s.set("bulk_g6a_pa_capacity", b.g6a_pa_capacity);
  s.set("bulk_g6a_pa_need_max", b.g6a_pa_need_max);
  s.set("bulk_g6a_pa_occ_max", b.g6a_pa_occ_max);
  s.set("bulk_g6a_pa_bytes", b.g6a_pa_bytes);
  s.set("bulk_g6a_dram_idle_bank_cycles_tr", b.g6a_dram_idle_tr);
  s.set("bulk_g6a_runs_released", b.g6a_runs_released);
  s.set("bulk_g6a_tr_wait_dram", b.g6a_tr_wait_dram);
  s.set("bulk_g6a_tr_wait_max", b.g6a_tr_wait_max);
  // ---- M23-G6B: streaming translation/DRAM decoupling ----
  s.set("bulk_g6b_windows", b.g6b_windows);
  s.set("bulk_g6b_tr_requests", b.g6b_tr_reqs);
  s.set("bulk_g6b_tr_completed", b.g6b_tr_done);
  s.set("bulk_g6b_pa_capacity", b.g6b_capacity);
  s.set("bulk_g6b_pa_bytes", b.g6b_pa_bytes);
  s.set("bulk_g6b_ready_peak", b.g6b_ready_peak);
  s.set("bulk_g6b_ready_occ_sum", b.g6b_ready_occ_sum);
  for (u32 hi = 0; hi < 13; ++hi)
    s.set("bulk_g6b_ready_h" + std::to_string(hi), b.g6b_ready_hist[hi]);
  s.set("bulk_g6b_backpressure_cycles", b.g6b_backpressure);
  s.set("bulk_g6b_full_cycles", b.g6b_full_events);
  s.set("bulk_g6b_tr_stall_cycles", b.g6b_tr_stall);
  s.set("bulk_g6b_no_ready_cycles", b.g6b_no_ready);
  s.set("bulk_g6b_tr_active_cycles", b.g6b_tr_active);
  s.set("bulk_g6b_tgt_out_cycles", b.g6b_tgt_out);
  s.set("bulk_g6b_overlap_cycles", b.g6b_overlap);
  s.set("bulk_g6b_runs_released", b.g6b_runs_released);
  s.set("bulk_g6f_scan_calls", b.g6f_scan_calls);
  s.set("bulk_g6f_scan_entries", b.g6f_scan_entries);
  s.set("bulk_g6f_vis_cells_sum", b.g6f_vis_cells_sum);
  s.set("bulk_g6f_vis_cells_max", b.g6f_vis_cells_max);
  s.set("bulk_g6f_vis_hit_sum", b.g6f_vis_hit_sum);
  s.set("bulk_g6f_pick_hit", b.g6f_pick_hit);
  s.set("bulk_g6f_pick_frontier", b.g6f_pick_frontier);
  // ---- M23-G6G: physical (bank,row) group release ----
  s.set("bulk_g6g_groups_opened", b.g6g_groups_opened);
  s.set("bulk_g6g_groups_closed", b.g6g_groups_closed);
  s.set("bulk_g6g_group_runs", b.g6g_group_runs);
  s.set("bulk_g6g_group_size_sum", b.g6g_group_size_sum);
  s.set("bulk_g6g_group_size_max", b.g6g_group_size_max);
  s.set("bulk_g6g_group_drain_sum", b.g6g_group_drain_sum);
  s.set("bulk_g6g_group_drain_max", b.g6g_group_drain_max);
  for (u32 hi = 0; hi < 6; ++hi)
    s.set("bulk_g6g_group_h" + std::to_string(hi), b.g6g_group_hist[hi]);
  s.set("bulk_g6g_same_cell", b.g6g_same_cell);
  s.set("bulk_g6g_switch_cell", b.g6g_switch_cell);
  s.set("bulk_g6g_bank_alt", b.g6g_bank_alt);
  s.set("bulk_g6g_revisit", b.g6g_revisit);
  s.set("bulk_g6g_sel_mismatch", b.g6g_sel_mismatch);
  s.set("bulk_g6g_scan_calls", b.g6g_scan_calls);
  s.set("bulk_g6g_scan_entries", b.g6g_scan_entries);
  // ---- M23-G6H: complete-eligible-window audit ----
  s.set("bulk_g6h_decisions", b.g6h_decisions);
  s.set("bulk_g6h_exam_sum", b.g6h_exam_sum);
  s.set("bulk_g6h_exam_max", b.g6h_exam_max);
  s.set("bulk_g6h_elig_sum", b.g6h_elig_sum);
  s.set("bulk_g6h_elig_max", b.g6h_elig_max);
  s.set("bulk_g6h_stale_sum", b.g6h_stale_sum);
  s.set("bulk_g6h_untr_sum", b.g6h_untr_sum);
  s.set("bulk_g6h_untr_max", b.g6h_untr_max);
  s.set("bulk_g6h_groups_sum", b.g6h_groups_sum);
  s.set("bulk_g6h_groups_max", b.g6h_groups_max);
  s.set("bulk_g6h_considered_sum", b.g6h_considered_sum);
  s.set("bulk_g6h_deferred_sum", b.g6h_deferred_sum);
  s.set("bulk_g6h_inv_viol", b.g6h_inv_viol);
  // ---- M23-G6I: continuous translation/DRAM pipeline ----
  s.set("bulk_g6i_samples", b.g6i_samples);
  s.set("bulk_g6i_samp_pool_sum", b.g6i_samp_pool_sum);
  s.set("bulk_g6i_samp_pool_max", b.g6i_samp_pool_max);
  s.set("bulk_g6i_vpn_sum", b.g6i_vpn_sum);
  s.set("bulk_g6i_vpn_max", b.g6i_vpn_max);
  s.set("bulk_g6i_cells_sum", b.g6i_cells_sum);
  s.set("bulk_g6i_cells_max", b.g6i_cells_max);
  s.set("bulk_g6i_xpage_cells_sum", b.g6i_xpage_cells_sum);
  s.set("bulk_g6i_xpage_cells_max", b.g6i_xpage_cells_max);
  s.set("bulk_g6i_xpage_entries_sum", b.g6i_xpage_entries_sum);
  s.set("bulk_g6i_untr_sum", b.g6i_untr_sum);
  s.set("bulk_g6i_untr_max", b.g6i_untr_max);
  s.set("bulk_g6i_pages_open_sum", b.g6i_pages_open_sum);
  s.set("bulk_g6i_pages_open_max", b.g6i_pages_open_max);
  s.set("bulk_g6i_probe_max", b.g6i_probe_max);
  s.set("bulk_g6i_rel_cycles", b.g6i_rel_cycles);
  s.set("bulk_g6i_rel_with_tr", b.g6i_rel_with_tr);
  s.set("bulk_m24_rel_pairs", b.m24_rel_pairs);
  s.set("bulk_m24_rel_inversions", b.m24_rel_inversions);
  s.set("bulk_g6i_pool_empty", b.g6i_pool_empty);
  s.set("bulk_g6i_pool_over", b.g6i_pool_over);
  s.set("bulk_g6i_gap_n", b.g6i_gap_n);
  s.set("bulk_g6i_gap_sum", b.g6i_gap_sum);
  s.set("bulk_g6i_gap_max", b.g6i_gap_max);
  s.set("bulk_g6i_rel_vpn_switch", b.g6i_rel_vpn_switch);
  s.set("bulk_g6i_rel_same_vpn", b.g6i_rel_same_vpn);
  s.set("bulk_g6b_release_pops", b.g6b_rel_pops);
  s.set("bulk_g6b_release_blocked_cycles", b.g6b_rel_blocked);
  s.set("bulk_g6b_release_slot_stall", b.g6b_rel_slot_stall);
  s.set("bulk_g6b_tr_wait_dram", b.g6b_tr_wait_dram);
  s.set("bulk_g6b_tr_wait_max", b.g6b_tr_wait_max);
  // ---- M23-G6C: P0-style admission deferral on the G6B pipeline -------------
  s.set("bulk_g6c_windows", b.g6c_windows);
  s.set("bulk_g6c_admit_attempts", b.g6c_admit_attempts);
  s.set("bulk_g6c_admit_accepted", b.g6c_admit_ok);
  s.set("bulk_g6c_refused", b.g6c_refused);
  s.set("bulk_g6c_refuse_parked", b.g6c_refuse_parked);
  s.set("bulk_g6c_refuse_full", b.g6c_refuse_full);
  s.set("bulk_g6c_refuse_other", b.g6c_refuse_other);
  s.set("bulk_g6c_defer_events", b.g6c_defer_events);
  s.set("bulk_g6c_deferred_readmitted", b.g6c_defer_readmit);
  s.set("bulk_g6c_defer_then_other_admit", b.g6c_defer_other_admit);
  s.set("bulk_g6c_defer_age_sum", b.g6c_defer_age_sum);
  s.set("bulk_g6c_defer_age_max", b.g6c_defer_age_max);
  s.set("bulk_g6c_defer_depth_sum", b.g6c_defer_depth_sum);
  s.set("bulk_g6c_defer_depth_n", b.g6c_defer_depth_n);
  s.set("bulk_g6c_defer_depth_max", b.g6c_defer_depth_max);
  s.set("bulk_g6c_cohorts_opened", b.g6c_cohorts_opened);
  s.set("bulk_g6c_admit_idle_cycles", b.g6c_admit_idle);
  s.set("bulk_g6c_dup_submit_blocked", b.g6c_dup_submit);
  // Window SRAM includes the scheduler's page-run descriptors when enabled:
  // they are state the design now owns, so they are charged, not free.
  s.set("bulk_win_bytes",
        cfg_.batch_window
            ? bufs * (static_cast<u64>(cfg_.batch_window) * 8 +
                      static_cast<u64>(cfg_.page_dir) * 16 + b.sched_bytes)
            : 0);
  s.set("bulk_win_entries", b.win_entries);
  s.set("bulk_win_occ_max", b.win_occ_max);
  s.set("bulk_win_total_max", b.win_total_max);
  s.set("bulk_win_occ_sum", b.win_occ_sum);
  s.set("bulk_dir_occ_max", b.dir_occ_max);
  s.set("bulk_dir_collisions", b.dir_collisions);
  s.set("bulk_batches", b.batches);
  s.set("bulk_seals_full", b.seals_full);
  s.set("bulk_seals_end", b.seals_end);
  s.set("bulk_seals_deadline", b.seals_deadline);
  s.set("bulk_lines_issued", b.lines_issued);
  s.set("bulk_dedup_merged", b.dedup_merged);
  s.set("bulk_org_cycles", b.org_cycles);
  s.set("bulk_fanout_cycles", b.fanout_cycles);
  s.set("bulk_fanout_elems", b.fanout_elems);
  s.set("bulk_fanout_max_backlog", b.fanout_max_backlog);
  s.set("bulk_win_full_stall", b.win_full_stall);
  // M6.1 timeline. `cycles` is the cycle the run stopped on, so a phase that
  // ends at event cycle c spans c+1 cycles (0..c).
  s.set("bulk_first_desc_done", b.first_desc_done == NONE ? 0 : b.first_desc_done);
  s.set("bulk_first_idx_issue", b.first_idx_issue == NONE ? 0 : b.first_idx_issue);
  s.set("bulk_last_idx_issue", b.last_idx_issue == NONE ? 0 : b.last_idx_issue);
  s.set("bulk_last_idx_done", b.last_idx_done == NONE ? 0 : b.last_idx_done);
  s.set("bulk_first_admit", b.first_admit == NONE ? 0 : b.first_admit);
  s.set("bulk_first_seal", b.first_seal == NONE ? 0 : b.first_seal);
  s.set("bulk_last_seal", b.last_seal == NONE ? 0 : b.last_seal);
  s.set("bulk_first_tgt_issue", b.first_tgt_issue == NONE ? 0 : b.first_tgt_issue);
  s.set("bulk_last_tgt_issue", b.last_tgt_issue == NONE ? 0 : b.last_tgt_issue);
  s.set("bulk_last_fill", b.last_fill == NONE ? 0 : b.last_fill);
  s.set("bulk_busy_at_first_tgt", b.busy_at_first_tgt);
  s.set("bulk_busy_at_last_tgt", b.busy_at_last_tgt);
  s.set("bulk_starved_at_first_tgt", b.starved_at_first_tgt);
  s.set("bulk_oracle_elems", b.oracle_elems);
  s.set("bulk_idx_oracle", cfg_.idx_oracle ? 1 : 0);

  export_win_records(b, cfg_, s);
  collect_mem_stats(*mem_, cfg_, s);
  collect_lat_stats(*mem_, res);

  auto check = [&](const char* name, u64 lhs, u64 rhs) {
    if (lhs != rhs) {
      res.ok = false;
      res.violations.push_back(std::string(name) + ": " + std::to_string(lhs) +
                               " != " + std::to_string(rhs));
    }
  };
  check("generated == issued", generated, issued);
  check("issued == completed", issued, completed);
  check("desc submitted == completed + rejected", b.desc_submitted,
        b.desc_completed + b.desc_rejected);
  check("index lines requested == completed", b.idx_lines_req, b.idx_lines_done);
  check("index elements requested == decoded", b.idx_elems_req, b.idx_elems_done);
  check("target requests generated == issued", b.tgt_req_gen, b.tgt_req_issued);
  check("target requests issued == completed", b.tgt_req_issued, b.tgt_req_done);
  check("elements total == done + faults", b.elems_total, b.elems_done + b.addr_faults);
  check("residency invariant held", b.residency_violations, 0);
  check("generated addresses match the index image", b.addr_mismatch, 0);
  // Window invariants: nothing lost, nothing double-counted, and the element ->
  // line accounting closes exactly (one request per distinct line + merges).
  if (cfg_.batch_window) {
    check("window admitted every element", b.win_entries, b.elems_total);
    check("window retired every element", b.elems_done, b.elems_total);
    if (cfg_.reuse_dir) {
      // M13: the three lookup outcomes plus the issued groups must account for
      // every element exactly once, and no request may be lost or invented.
      check("reuse lookups == planned lines", b.reuse_probes, b.reuse_lines_planned);
      check("retained + merged + issued == planned lines",
            b.reuse_hit_ready + b.reuse_hit_inflight + b.reuse_miss, b.reuse_lines_planned);
      check("issued requests == reuse misses", b.lines_issued, b.reuse_miss);
      // Group-level identity: every distinct line of every window is counted
      // exactly once as issued, served from retained data, or merged -- and
      // `dedup_merged` already accounts the extra elements of every group
      // (served or issued), so the two together close on `win_entries`.
      check("lines + merges + reuse == elements",
            b.lines_issued + b.dedup_merged + b.reuse_hit_ready + b.reuse_hit_inflight,
            b.win_entries);
    } else {
      check("lines issued + merged == elements", b.lines_issued + b.dedup_merged, b.win_entries);
    }
    check("window occupancy <= W", b.win_occ_max <= cfg_.batch_window ? 0 : 1, 0);
    check("buffered entries <= bufs*W",
          b.win_total_max <= bufs * cfg_.batch_window ? 0 : 1, 0);
    // M9 window management: the records must cover every allocation, and E2 must
    // never show two live windows.
    check("one window record per allocation", static_cast<u64>(b.win_recs.size()), b.win_alloc);
    u64 rec_sum = 0;
    for (const auto& w : b.win_recs) rec_sum += w.n;
    check("window records account for every admitted element", rec_sum, b.win_entries);
    // Structural, not policy-specific: `fill_` is a single cursor, so no cycle
    // can admit into two buffers. E2 additionally owns only one buffer.
    check("at most one unsealed window", b.win_incomplete_max <= 1 ? 0 : 1, 0);
    if (cfg_.effective_win_policy() == 1)
      check("one live window", b.win_live_max <= 1 ? 0 : 1, 0);
    if (b.dir_collisions == 0)
      check("resident pages <= page_dir", b.dir_occ_max <= cfg_.page_dir ? 0 : 1, 0);
  }
  // Only a bypassing configuration may show zero L1 traffic; under mem_path = l1
  // the engine shares the L1 with the SIMT arm and must show L1 traffic.
  if (mem_->path_bypasses_l1(true)) check("no L1 traffic from the engine", mem_->l1.reads, 0);
  else if (mem_->l1.reads == 0 && b.tgt_req_done > 0) {
    res.ok = false;
    res.violations.push_back("mem_path = l1 but the engine produced no L1 traffic");
  }
  check("l1 reads == hits + misses", mem_->l1.reads, mem_->l1.hits + mem_->l1.misses);
  check("l2 reads == hits + misses", mem_->l2.reads, mem_->l2.hits + mem_->l2.misses);
  check("l2tlb reads == hits + misses", mem_->l2tlb.reads,
        mem_->l2tlb.hits + mem_->l2tlb.misses);
  check("tlb0 reads == hits + misses", mem_->tlb[0].reads,
        mem_->tlb[0].hits + mem_->tlb[0].misses);
  s.set("conservation_ok", res.ok ? 1 : 0);
  s.inc("conservation_violations", res.violations.size());
  return res;
}

}  // namespace dx100
