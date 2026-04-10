#include <cassert>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "leomem/leomem.h"
#include "memory/cache/cache_manager.h"
#include "metadata/block_meta.h"
#include "metadata/block_table.h"
#include "runtime/context.h"
#include "transport/transport.h"

int main() {
    using namespace leomem;

    Status s = lm_init();
    assert(s == Status::kOk);

    GlobalAddr addr = lm_malloc(sizeof(std::uint64_t));
    assert(addr.IsValid());

    std::uint64_t x = 42;
    s = lm_write(addr, &x, sizeof(x));
    assert(s == Status::kOk);

    std::uint64_t y = 0;
    s = lm_read(addr, &y, sizeof(y));
    assert(s == Status::kOk);
    assert(y == 42);

    auto meta = RuntimeContext::Instance().block_table()->Find(
        BlockId{addr.region_id, addr.offset / RuntimeContext::Instance().GetConfig().block_size});
    assert(meta != nullptr);
    assert(meta->owner_node.load(std::memory_order_relaxed) ==
           RuntimeContext::Instance().GetConfig().node_id);
    assert(meta->version.load(std::memory_order_relaxed) == 1);

    std::size_t block_size = RuntimeContext::Instance().GetConfig().block_size;
    GlobalAddr span = lm_malloc(block_size + 128);
    assert(span.IsValid());

    std::vector<std::uint8_t> wbuf(block_size + 128, 7);
    s = lm_write(span, wbuf.data(), wbuf.size());
    assert(s == Status::kOk);

    std::vector<std::uint8_t> rbuf(block_size + 128, 0);
    s = lm_read(span, rbuf.data(), rbuf.size());
    assert(s == Status::kOk);
    assert(rbuf == wbuf);

    auto first = RuntimeContext::Instance().block_table()->Find(
        BlockId{span.region_id, span.offset / block_size});
    auto second = RuntimeContext::Instance().block_table()->Find(
        BlockId{span.region_id, (span.offset + block_size) / block_size});
    assert(first != nullptr);
    assert(second != nullptr);
    assert(first->version.load(std::memory_order_relaxed) == 1);
    assert(second->version.load(std::memory_order_relaxed) == 1);

    Config cache_cfg;
    cache_cfg.profiling_window_size = 4;
    cache_cfg.cache_admission_min_reads = 2;
    cache_cfg.cache_admission_max_writes = 0;
    cache_cfg.cache_admission_max_sharers = 3;
    cache_cfg.cache_admission_max_reuse_distance = 3;
    cache_cfg.cache_admission_min_read_ratio = 0.75;
    cache_cfg.cache_admission_max_phase_change_ratio = 0.30;
    cache_cfg.remote_write_batch_threshold = 2;
    cache_cfg.coherence_sharer_promote_threshold = 2;
    cache_cfg.coherence_invalidation_promote_threshold = 2;
    cache_cfg.coherence_write_dominant_ratio = 0.55;
    cache_cfg.coherence_phase_change_promote_ratio = 0.30;
    cache_cfg.adaptive_mode_batch_threshold = 1;
    CacheManager cache_mgr(cache_cfg);
    assert(cache_mgr.Init() == Status::kOk);

    auto prof = std::make_shared<BlockMeta>();
    assert(cache_mgr.ObserveAccess(prof, AccessType::kRead, false) == CoherenceMode::kSI);
    assert(!cache_mgr.ShouldCache(prof));
    assert(cache_mgr.ObserveAccess(prof, AccessType::kRead, false) == CoherenceMode::kAdaptive);
    assert(cache_mgr.ObserveAccess(prof, AccessType::kWrite, false) == CoherenceMode::kWI);
    assert(!cache_mgr.ShouldCache(prof));
    assert(prof->coherence_switch_count.load(std::memory_order_relaxed) >= 2);

    auto phase_stable = std::make_shared<BlockMeta>();
    assert(cache_mgr.ObserveAccess(phase_stable, AccessType::kRead, false) == CoherenceMode::kSI);
    assert(cache_mgr.ObserveAccess(phase_stable, AccessType::kRead, false) == CoherenceMode::kAdaptive);
    assert(cache_mgr.ObserveAccess(phase_stable, AccessType::kRead, false) == CoherenceMode::kAdaptive);

    auto locally_cacheable = std::make_shared<BlockMeta>();
    assert(cache_mgr.ObserveAccess(locally_cacheable, AccessType::kRead, true) == CoherenceMode::kSI);
    assert(cache_mgr.ObserveAccess(locally_cacheable, AccessType::kRead, true) == CoherenceMode::kSI);
    assert(cache_mgr.ObserveAccess(locally_cacheable, AccessType::kRead, true) == CoherenceMode::kSI);
    assert(cache_mgr.ShouldCache(locally_cacheable));

    auto write_dominant = std::make_shared<BlockMeta>();
    assert(cache_mgr.ObserveAccess(write_dominant, AccessType::kWrite, false) == CoherenceMode::kWI);
    assert(cache_mgr.ObserveAccess(write_dominant, AccessType::kWrite, false) == CoherenceMode::kWI);
    assert(write_dominant->mode.load(std::memory_order_relaxed) == CoherenceMode::kWI);

    GlobalAddr remote_addr;
    remote_addr.home_node = 1;
    remote_addr.region_id = 0;
    remote_addr.offset = 8192;

    std::uint64_t cached = 99;
    std::uint64_t cached_next = 123;
    cache_mgr.InsertCached(remote_addr, &cached, sizeof(cached));
    std::uint64_t cached_out = 0;
    assert(cache_mgr.TryReadCached(remote_addr, &cached_out, sizeof(cached_out)));
    assert(cached_out == 99);
    assert(cache_mgr.Invalidate(remote_addr, sizeof(cached)) == 1);
    assert(!cache_mgr.TryReadCached(remote_addr, &cached_out, sizeof(cached_out)));

    GlobalAddr overlap_base = remote_addr;
    GlobalAddr overlap_neighbor = remote_addr;
    overlap_neighbor.offset += sizeof(cached);
    GlobalAddr separate = remote_addr;
    separate.offset += 128;
    cache_mgr.InsertCached(overlap_base, &cached, sizeof(cached));
    cache_mgr.InsertCached(overlap_neighbor, &cached_next, sizeof(cached_next));
    cache_mgr.InsertCached(separate, &cached, sizeof(cached));
    assert(cache_mgr.InvalidateRange(overlap_base, sizeof(cached) * 2) == 2);
    assert(!cache_mgr.TryReadCached(overlap_base, &cached_out, sizeof(cached_out)));
    assert(!cache_mgr.TryReadCached(overlap_neighbor, &cached_out, sizeof(cached_out)));
    assert(cache_mgr.TryReadCached(separate, &cached_out, sizeof(cached_out)));
    assert(cache_mgr.Invalidate(separate, sizeof(cached)) == 1);

    cache_mgr.QueueRemoteWrite(remote_addr, &cached, sizeof(cached), CoherenceMode::kSI, false, false);
    assert(!cache_mgr.ShouldFlushDestination(remote_addr.home_node,
                                            cache_mgr.EffectiveBatchThreshold(CoherenceMode::kSI)));
    GlobalAddr remote_adj = remote_addr;
    remote_adj.offset += sizeof(cached);
    assert(cache_mgr.QueueRemoteWrite(remote_adj, &cached_next, sizeof(cached_next),
                                     CoherenceMode::kAdaptive, true, true));
    assert(cache_mgr.ShouldFlushDestination(remote_addr.home_node,
                                           cache_mgr.EffectiveBatchThreshold(CoherenceMode::kAdaptive)));
    assert(!cache_mgr.ShouldFlushDestination(remote_addr.home_node,
                                            cache_mgr.EffectiveBatchThreshold(CoherenceMode::kSI)));

    GlobalAddr remote_far = remote_addr;
    remote_far.offset += 64;
    assert(!cache_mgr.QueueRemoteWrite(remote_far, &cached, sizeof(cached),
                                      CoherenceMode::kSI, false, false));
    assert(cache_mgr.ShouldFlushDestination(remote_addr.home_node,
                                           cache_mgr.EffectiveBatchThreshold(CoherenceMode::kSI)));
    assert(cache_mgr.EffectiveBatchThreshold(CoherenceMode::kWI) == 1);
    assert(cache_mgr.EffectiveBatchThreshold(CoherenceMode::kAdaptive) == 1);
    assert(cache_mgr.EffectiveBatchThreshold(CoherenceMode::kSI) == 2);

    auto drained = cache_mgr.DrainRemoteWrites(remote_addr.home_node);
    assert(drained.writes.size() == 2);
    assert(drained.total_bytes == sizeof(cached) * 3);
    assert(drained.writes[0].addr.offset == remote_addr.offset);
    assert(drained.writes[0].payload.size() == sizeof(cached) * 2);
    std::uint64_t merged_first = 0;
    std::uint64_t merged_second = 0;
    std::memcpy(&merged_first, drained.writes[0].payload.data(), sizeof(merged_first));
    std::memcpy(&merged_second,
                drained.writes[0].payload.data() + sizeof(merged_second),
                sizeof(merged_second));
    assert(merged_first == cached);
    assert(merged_second == cached_next);
    assert(cache_mgr.PendingDestinations().empty());
    assert(cache_mgr.Shutdown() == Status::kOk);

    GlobalAddr owner_shadow_addr;
    owner_shadow_addr.home_node = 1;
    owner_shadow_addr.region_id = 0;
    owner_shadow_addr.offset = 16384;
    auto owner_meta = RuntimeContext::Instance().block_table()->GetOrCreate(
        BlockId{owner_shadow_addr.region_id,
                owner_shadow_addr.offset / RuntimeContext::Instance().GetConfig().block_size});
    owner_meta->owner_node.store(2, std::memory_order_relaxed);
    owner_meta->state.store(BlockState::kOwner, std::memory_order_relaxed);
    owner_meta->mode.store(CoherenceMode::kAdaptive, std::memory_order_relaxed);

    std::uint64_t owner_payload = 777;
    GlobalAddr owner_location = owner_shadow_addr;
    owner_location.home_node = 2;
    s = RuntimeContext::Instance().transport()->Write(owner_location, &owner_payload, sizeof(owner_payload));
    assert(s == Status::kOk);

    std::uint64_t owner_read = 0;
    s = lm_read(owner_shadow_addr, &owner_read, sizeof(owner_read));
    assert(s == Status::kOk);
    assert(owner_read == owner_payload);

    constexpr std::uint64_t kAckRequestId = 41;
    RuntimeContext::Instance().TrackPendingControlAck(kAckRequestId);
    assert(RuntimeContext::Instance().HasPendingControlAck(kAckRequestId));
    ControlMessage ack;
    ack.opcode = ControlOpcode::kOwnerTransferAck;
    ack.source_node = 1;
    ack.target_node = RuntimeContext::Instance().GetConfig().node_id;
    ack.request_id = kAckRequestId;
    s = RuntimeContext::Instance().transport()->PostControlMessage(ack);
    assert(s == Status::kOk);
    s = lm_fence();
    assert(s == Status::kOk);
    assert(!RuntimeContext::Instance().HasPendingControlAck(kAckRequestId));

    StatsSnapshot st = lm_get_stats();
    assert(st.alloc_ops == 2);
    assert(st.write_ops == 2);
    assert(st.read_ops == 3);

    s = lm_shutdown();
    assert(s == Status::kOk);

    std::printf("smoke test passed\n");
    return 0;
}
