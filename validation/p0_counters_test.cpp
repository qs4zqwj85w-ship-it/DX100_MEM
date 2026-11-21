// P0 instrumentation validation: counter semantics for the SimX memory system.
//
// This drives the REAL Mmu / L2Tlb / Ptw objects through the REAL SimPlatform,
// with a stub cluster TLB and a stub downstream cache standing in for the
// parts of the hierarchy a unit test does not need. No simulated behaviour is
// stubbed inside the objects under test: every counter checked here is
// produced by the production code path.
//
// Build (see the P0 report for the exact command):
//   g++ -std=c++17 -DVX_CFG_VM_ENABLE ... p0_counters_test.cpp \
//       <vortex>/sim/simx/mem/mmu.cpp <vortex>/sim/simx/mem/mmu_tlb.cpp \
//       <vortex>/sim/simx/mem/tlb_l2.cpp <vortex>/sim/simx/mem/ptw.cpp \
//       <vortex>/sim/simx/mem/cache.cpp ...

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <VX_config.h>
#include <VX_types.h>

#include "types.h"
#include "mem/mmu.h"
#include "mem/tlb_l2.h"
#include "mem/ptw.h"

using namespace vortex;

static int g_failures = 0;

#define CHECK(cond, ...)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
      std::printf(__VA_ARGS__);                                           \
      std::printf("\n");                                                  \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

#define CHECK_EQ(actual, expected)                                        \
  do {                                                                    \
    auto _a = (actual);                                                   \
    auto _e = (expected);                                                 \
    if (_a != _e) {                                                       \
      std::printf("  FAIL %s:%d: %s = %llu, expected %llu\n", __FILE__,   \
                  __LINE__, #actual, (unsigned long long)_a,              \
                  (unsigned long long)_e);                                \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

///////////////////////////////////////////////////////////////////////////////
// Stub cluster TLB: answers every miss with a distinct translation after a
// fixed delay, and counts what it served so the MMU's view can be cross-checked.

class FakeTlb : public SimObject<FakeTlb> {
public:
  SimChannel<TlbReq> ReqIn;
  SimChannel<TlbRsp> RspOut;

  uint64_t served = 0;
  uint64_t vpn_seen_sum = 0;

  FakeTlb(const SimContext& ctx, const char* name, uint32_t latency)
    : SimObject<FakeTlb>(ctx, name)
    , ReqIn(this)
    , RspOut(this)
    , latency_(latency) {}

protected:
  void on_reset() {}

  void on_tick() {
    if (ReqIn.empty() || RspOut.full()) {
      return;
    }
    const TlbReq& req = ReqIn.peek();
    TlbRsp rsp;
    rsp.ppn = 0x1000 + req.vpn;  // distinct, non-zero physical page
    rsp.flags = 0xdf;            // V R W X U A D
    rsp.level = 0;
    rsp.fault = false;
    rsp.client_id = req.client_id;
    rsp.slot = req.slot;
    RspOut.send(rsp, latency_);
    ++served;
    vpn_seen_sum += req.vpn;
    ReqIn.pop();
  }

private:
  uint32_t latency_;

  friend class SimObject<FakeTlb>;
};

///////////////////////////////////////////////////////////////////////////////
// Stub downstream cache: accepts translated requests and echoes one response
// per request, so the MMU's response path is exercised as it is in production.

class FakeCache : public SimObject<FakeCache> {
public:
  std::vector<SimChannel<MemReq>> ReqIn;
  std::vector<SimChannel<MemRsp>> RspOut;

  uint64_t accepted = 0;

  FakeCache(const SimContext& ctx, const char* name, uint32_t num_ports)
    : SimObject<FakeCache>(ctx, name) {
    for (uint32_t p = 0; p < num_ports; ++p) {
      ReqIn.emplace_back(this);
      RspOut.emplace_back(this);
    }
  }

protected:
  void on_reset() {}

  void on_tick() {
    for (size_t p = 0; p < ReqIn.size(); ++p) {
      if (ReqIn.at(p).empty() || RspOut.at(p).full()) {
        continue;
      }
      const MemReq& req = ReqIn.at(p).peek();
      auto data = std::make_shared<mem_block_t>();
      data->fill(0);
      RspOut.at(p).send(MemRsp(req.tag, req.hart_id, req.uuid, data));
      ReqIn.at(p).pop();
      ++accepted;
    }
  }

  friend class SimObject<FakeCache>;
};

///////////////////////////////////////////////////////////////////////////////
// Stub PTE memory for the walker: returns a zero (invalid) PTE for every
// fetch, so a walk faults at its first level after exactly one fetch. That is
// enough to check the walker's own counters without building a page table.

class FakePteMem : public SimObject<FakePteMem> {
public:
  SimChannel<MemReq> ReqIn;
  SimChannel<MemRsp> RspOut;

  uint64_t fetches = 0;  // PTE loads actually served, for cross-checking

  FakePteMem(const SimContext& ctx, const char* name)
    : SimObject<FakePteMem>(ctx, name)
    , ReqIn(this)
    , RspOut(this) {}

protected:
  void on_reset() {}

  void on_tick() {
    if (ReqIn.empty() || RspOut.full()) {
      return;
    }
    const MemReq& req = ReqIn.peek();
    auto data = std::make_shared<mem_block_t>();
    data->fill(0);  // PTE V=0: the walk stops here and reports a fault
    RspOut.send(MemRsp(req.tag, req.hart_id, req.uuid, data));
    ReqIn.pop();
    ++fetches;
  }

  friend class SimObject<FakePteMem>;
};

// Drains the walker's response port.
class TlbRspSink : public SimObject<TlbRspSink> {
public:
  SimChannel<TlbRsp> In;
  uint64_t received = 0;
  bool     saw_fault = false;

  TlbRspSink(const SimContext& ctx, const char* name)
    : SimObject<TlbRspSink>(ctx, name)
    , In(this) {}

protected:
  void on_reset() {}

  void on_tick() {
    while (!In.empty()) {
      saw_fault = saw_fault || In.peek().fault;
      In.pop();
      ++received;
    }
  }

  friend class SimObject<TlbRspSink>;
};

///////////////////////////////////////////////////////////////////////////////

// One MMU wired to its stubs, with the knobs a scenario needs.
struct Fixture {
  Mmu::Ptr      mmu;
  FakeTlb::Ptr  tlb;
  FakeCache::Ptr cache;
  uint32_t      ports;
};

static Fixture make_fixture(const char* tag, uint32_t num_ports,
                            uint32_t tlb_size, uint32_t miss_latency) {
  Fixture f;
  f.ports = num_ports;
  std::string mmu_name = std::string(tag) + "-mmu";
  std::string tlb_name = std::string(tag) + "-tlb";
  std::string cac_name = std::string(tag) + "-cache";
  f.mmu   = Mmu::Create(mmu_name.c_str(), num_ports, tlb_size, /*client_id*/0,
                        /*exec_side*/false, /*rsp_reg*/false,
                        /*write_enable*/true, /*park_reg*/false,
                        /*req_skid*/false);
  f.tlb   = FakeTlb::Create(tlb_name.c_str(), miss_latency);
  f.cache = FakeCache::Create(cac_name.c_str(), num_ports);

  f.mmu->TlbMissOut.bind(&f.tlb->ReqIn);
  f.tlb->RspOut.bind(&f.mmu->TlbFillIn);
  for (uint32_t p = 0; p < num_ports; ++p) {
    f.mmu->ReqOut.at(p).bind(&f.cache->ReqIn.at(p));
    f.cache->RspOut.at(p).bind(&f.mmu->RspIn.at(p));
  }
  // A root page table pointer: the walker is a stub here, but the MMU's
  // permission check reads it, and needs_translation() disables translation
  // entirely in BARE mode, so the mode field must be programmed for this
  // XLEN (SV32 puts it at bit 31, Sv39 at bits 60..63).
#if VX_VM_ADDR_MODE == SV32
  f.mmu->set_satp((uint64_t(1) << 31) | 0x1);
#else
  f.mmu->set_satp((uint64_t(8) << 60) | 0x1);
#endif
  return f;
}

static void tick(int n) {
  for (int i = 0; i < n; ++i) {
    SimPlatform::instance().tick();
  }
}

static MemReq gmem_req(uint64_t addr, uint32_t tag) {
  return MemReq(MemOp::LD, addr, nullptr, 0, tag, 0, tag);
}

///////////////////////////////////////////////////////////////////////////////
// Scenario A: a cold miss, a dedup join, a replay, then a hit.

static void scenario_cold_miss_join_hit() {
  std::printf("[A] cold miss, dedup join, replay, hit\n");
  auto f = make_fixture("A", 2, 8, /*miss_latency*/4);

  // Fresh object: nothing counted yet.
  auto s0 = f.mmu->perf_stats();
  CHECK_EQ(s0.reads, 0);
  CHECK_EQ(s0.hits, 0);
  CHECK_EQ(s0.misses, 0);
  CHECK_EQ(s0.mshr_allocs, 0);
  CHECK_EQ(s0.mshr_joins, 0);
  CHECK_EQ(s0.mshr_occupancy, 0);
  CHECK_EQ(s0.stall_cycles, 0);

  const uint64_t vpn_a = 0x3;              // page 3
  const uint64_t addr_a = vpn_a * VX_VM_PAGE_SIZE;

  // Two requests to the same page: the first allocates, the second joins.
  // Delivery is one cycle after the host sends, and the miss issues on the
  // cycle after it parks, so settle the counters before checking them.
  f.mmu->ReqIn.at(0).send(gmem_req(addr_a + 0x10, 0));
  tick(2);
  f.mmu->ReqIn.at(0).send(gmem_req(addr_a + 0x20, 1));
  tick(2);

  auto s1 = f.mmu->perf_stats();
  CHECK_EQ(s1.reads, 2);        // two lookups
  CHECK_EQ(s1.misses, 2);       // both missed
  CHECK_EQ(s1.mshr_allocs, 1);  // one entry for the page
  CHECK_EQ(s1.mshr_joins, 1);   // the second request joined it
  CHECK_EQ(f.tlb->served, 1);   // one miss reached the cluster TLB

  // Let the fill land and both requests replay downstream.
  tick(20);
  auto s2 = f.mmu->perf_stats();
  CHECK_EQ(s2.replay_requests, 2);
  CHECK_EQ(s2.mshr_max_occupancy, 1);
  CHECK_EQ(f.cache->accepted, 2);
  CHECK(s2.mshr_occupancy > 0, "occupancy integral should be non-zero");
  CHECK(s2.mshr_occupancy <= SimPlatform::instance().cycles(),
        "occupancy integral exceeds elapsed cycles with one entry");

  // Same page again: a hit, no new allocation.
  f.mmu->ReqIn.at(1).send(gmem_req(addr_a + 0x30, 2));
  tick(4);
  auto s3 = f.mmu->perf_stats();
  CHECK_EQ(s3.hits, 1);
  CHECK_EQ(s3.mshr_allocs, 1);
  CHECK_EQ(f.tlb->served, 1);
  CHECK_EQ(f.cache->accepted, 3);
}

///////////////////////////////////////////////////////////////////////////////
// Scenario B: the miss station is full, so a port must stall.

static void scenario_station_full_stall() {
  std::printf("[B] miss station full -> stall cycles\n");
  // Depth 1 and MAX_PARKED_PER_ENTRY = 2, with a slow fill.
  auto f = make_fixture("B", 2, 1, /*miss_latency*/200);

  const uint64_t addr_a = 0x5 * VX_VM_PAGE_SIZE;
  // Two requests fill the entry's parked list; the third has nowhere to go.
  f.mmu->ReqIn.at(0).send(gmem_req(addr_a + 0x00, 0));
  tick(1);
  f.mmu->ReqIn.at(0).send(gmem_req(addr_a + 0x08, 1));
  tick(1);
  f.mmu->ReqIn.at(0).send(gmem_req(addr_a + 0x10, 2));
  tick(6);

  auto s = f.mmu->perf_stats();
  CHECK_EQ(s.mshr_allocs, 1);
  CHECK_EQ(s.mshr_joins, 1);   // request 2 joined; request 3 could not
  CHECK(s.stall_cycles > 0, "expected stall cycles with a full station");
  CHECK(s.stall_lane_cycles >= s.stall_cycles,
        "lane-cycles must be at least stall cycles");
  CHECK(s.stall_cycles <= SimPlatform::instance().cycles(),
        "stall cycles cannot exceed elapsed cycles");
  CHECK_EQ(s.mshr_max_occupancy, 1);

  // Drain: the stalled request must still be served (no request lost).
  tick(400);
  auto d = f.mmu->perf_stats();
  CHECK_EQ(f.cache->accepted, 3);
  CHECK_EQ(f.tlb->served, 1);
  CHECK(d.stall_cycles > 0, "stall counter must not be cleared by draining");
}

///////////////////////////////////////////////////////////////////////////////
// Scenario C: aggregation algebra used by Cluster/Processor.

static void scenario_aggregation() {
  std::printf("[C] PerfStats aggregation algebra\n");
  Mmu::PerfStats a, b;
  a.reads = 10; a.hits = 6; a.misses = 4; a.evictions = 2;
  a.mshr_allocs = 4; a.mshr_joins = 1; a.mshr_occupancy = 40;
  a.mshr_max_occupancy = 5; a.stall_cycles = 7; a.stall_lane_cycles = 9;
  a.replay_requests = 3;
  b.reads = 5; b.hits = 3; b.misses = 2; b.evictions = 1;
  b.mshr_allocs = 2; b.mshr_joins = 0; b.mshr_occupancy = 20;
  b.mshr_max_occupancy = 3; b.stall_cycles = 1; b.stall_lane_cycles = 1;
  b.replay_requests = 2;

  a += b;
  CHECK_EQ(a.reads, 15);
  CHECK_EQ(a.hits, 9);
  CHECK_EQ(a.misses, 6);
  CHECK_EQ(a.evictions, 3);
  CHECK_EQ(a.mshr_allocs, 6);
  CHECK_EQ(a.mshr_joins, 1);
  CHECK_EQ(a.mshr_occupancy, 60);
  CHECK_EQ(a.mshr_max_occupancy, 5);  // max, not 8
  CHECK_EQ(a.stall_cycles, 8);
  CHECK_EQ(a.stall_lane_cycles, 10);
  CHECK_EQ(a.replay_requests, 5);

  L2Tlb::PerfStats c, d;
  c.reads = 4; c.hits = 3; c.misses = 1; c.evictions = 0;
  c.mshr_dedups = 2; c.mshr_allocs = 1; c.mshr_occupancy = 8;
  c.mshr_max_occupancy = 2; c.stall_cycles = 0; c.walk_stalls = 1;
  d.reads = 6; d.hits = 6; d.misses = 0; d.evictions = 1;
  d.mshr_dedups = 0; d.mshr_allocs = 0; d.mshr_occupancy = 0;
  d.mshr_max_occupancy = 0; d.stall_cycles = 2; d.walk_stalls = 0;
  c += d;
  CHECK_EQ(c.reads, 10);
  CHECK_EQ(c.hits, 9);
  CHECK_EQ(c.misses, 1);
  CHECK_EQ(c.mshr_dedups, 2);
  CHECK_EQ(c.mshr_max_occupancy, 2);
  CHECK_EQ(c.stall_cycles, 2);
  CHECK_EQ(c.walk_stalls, 1);
}

///////////////////////////////////////////////////////////////////////////////
// Stub walker: accepts every walk request and never answers, so the L2 TLB's
// miss station fills up and its own counters can be observed.

class FakeWalker : public SimObject<FakeWalker> {
public:
  SimChannel<TlbReq> In;
  uint64_t accepted = 0;

  FakeWalker(const SimContext& ctx, const char* name)
    : SimObject<FakeWalker>(ctx, name)
    , In(this) {}

protected:
  void on_reset() {}

  void on_tick() {
    while (!In.empty()) {
      In.pop();
      ++accepted;
    }
  }

  friend class SimObject<FakeWalker>;
};

///////////////////////////////////////////////////////////////////////////////
// Scenario E: the real cluster L2 TLB, with its own miss station.

static void scenario_l2tlb() {
  std::printf("[E] L2 TLB: dedup, allocs, occupancy, station-full stalls\n");

  auto l2    = L2Tlb::Create("E-l2tlb", /*num_tlb_clients*/1);
  auto walk  = FakeWalker::Create("E-walker");
  auto sink  = TlbRspSink::Create("E-rsp-sink");

  l2->PtwReqOut.bind(&walk->In);
  l2->RspOut.at(0).bind(&sink->In);

  auto z = l2->perf_stats();
  CHECK_EQ(z.reads, 0);
  CHECK_EQ(z.misses, 0);
  CHECK_EQ(z.mshr_allocs, 0);
  CHECK_EQ(z.mshr_occupancy, 0);
  CHECK_EQ(z.stall_cycles, 0);
  CHECK_EQ(z.walk_stalls, 0);

  TlbReq req;
  req.access = TlbAccess::Read;
  req.client_id = 0;
  req.slot = 0;

  req.vpn = 0x7;
  l2->ReqIn.at(0).send(req);
  tick(10);
  req.vpn = 0x7;  // same page: must join the live entry, not allocate
  l2->ReqIn.at(0).send(req);
  tick(10);

  auto a = l2->perf_stats();
  CHECK_EQ(a.reads, 2);
  CHECK_EQ(a.misses, 2);
  CHECK_EQ(a.mshr_dedups, 1);
  CHECK_EQ(a.mshr_allocs, 1);
  CHECK(a.mshr_occupancy > 0, "a held entry must accrue occupancy");
  CHECK_EQ(a.mshr_max_occupancy, 1);
  CHECK_EQ(walk->accepted, 1);  // one walk for the page, not two

  // Fill every MSHR entry, then one more page has nowhere to go.
  for (uint32_t i = 0; i < VX_CFG_L2_TLB_MSHR_SIZE + 2; ++i) {
    req.vpn = 0x100 + i;
    l2->ReqIn.at(0).send(req);
    tick(10);
  }

  auto b = l2->perf_stats();
  CHECK(b.stall_cycles > 0, "expected station-full stall cycles");
  CHECK(b.mshr_max_occupancy <= VX_CFG_L2_TLB_MSHR_SIZE,
        "occupancy cannot exceed the station depth");
  CHECK_EQ(b.walk_stalls, 0);  // the stub walker always accepts
  // NOTE: `misses` is counted per attempt, and a pipe head that cannot
  // register an entry is re-examined every cycle, so under back-pressure
  // misses exceeds the number of distinct missing lookups. That is the
  // existing behaviour of the counter (see the P0 report); only
  // mshr_allocs is a per-miss-event count.
  CHECK(b.misses >= b.mshr_allocs, "each allocation is preceded by a miss");
  CHECK(b.reads >= b.hits, "hits cannot exceed lookups");
}

///////////////////////////////////////////////////////////////////////////////
// Scenario D: the real page-table walker, with its own counters.
//
// A walk faults on its first fetch, which pins down the relationship the
// experiment relies on: one PTE access is one fetch, and pte_fetches counts
// fetch requests, not bytes transferred (each fetch is one line request, and
// its DRAM traffic is whatever the cache hierarchy reports).

static void scenario_walker() {
  std::printf("[D] walker: walks, pte_fetches, latency, occupancy\n");

  auto ptw    = Ptw::Create("D-ptw");
  auto ptemem = FakePteMem::Create("D-ptemem");
  auto sink   = TlbRspSink::Create("D-rsp-sink");

  ptw->MemReqOut.bind(&ptemem->ReqIn);
  ptemem->RspOut.bind(&ptw->MemRspIn);
  ptw->RspOut.bind(&sink->In);

#if VX_VM_ADDR_MODE == SV32
  ptw->set_satp((uint64_t(1) << 31) | 0x1);  // SV32, root PPN 1
#else
  ptw->set_satp((uint64_t(8) << 60) | 0x1);  // Sv39, root PPN 1
#endif

  auto p0 = ptw->perf_stats();
  CHECK_EQ(p0.walks, 0);
  CHECK_EQ(p0.pte_fetches, 0);
  CHECK_EQ(p0.walker_busy_cycles, 0);
  CHECK_EQ(p0.walker_max_active, 0);

  TlbReq req;
  req.vpn = 0x1234;
  req.access = TlbAccess::Read;
  req.client_id = 0;
  req.slot = 0;
  ptw->ReqIn.send(req);
  tick(100);

  auto p = ptw->perf_stats();
  CHECK_EQ(p.walks, 1);
  CHECK_EQ(p.pte_fetches, 1);         // one level fetched before the fault
  CHECK_EQ(ptemem->fetches, 1);       // counted once, not at two levels
  CHECK(p.walker_busy_cycles >= 1, "a dispatched walk must occupy a walker");
  CHECK(p.walker_busy_cycles <= SimPlatform::instance().cycles(),
        "one walker cannot be busy for more cycles than have elapsed");
  CHECK_EQ(p.walker_max_active, 1);
  CHECK(p.walk_latency >= 1, "walk latency must include the fetch");
  CHECK_EQ(p.wc_hits, 0);
  CHECK(sink->received == 1, "the faulted walk must still answer its requester");
  CHECK(sink->saw_fault, "an invalid PTE must report a fault");
  CHECK(ptemem->fetches == p.pte_fetches,
        "pte_fetches must equal the PTE loads the hierarchy saw");
}

///////////////////////////////////////////////////////////////////////////////

int main() {
  std::printf("P0 counter validation (VM=%d, pages=%u B)\n",
              VX_CFG_VM_ENABLED, (unsigned)VX_VM_PAGE_SIZE);
  scenario_cold_miss_join_hit();
  scenario_station_full_stall();
  scenario_aggregation();
  scenario_walker();
  scenario_l2tlb();

  if (g_failures == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failures);
  return 1;
}
