#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "leomem/config.h"
#include "memory/cache/cache_manager.h"
#include "metadata/block_meta.h"
#include "stats/counters.h"

namespace {

using leomem::AccessType;
using leomem::BlockMeta;
using leomem::CacheManager;
using leomem::CoherenceMode;
using leomem::Config;
using leomem::GlobalAddr;
using leomem::PendingRemoteWrite;
using leomem::StatsCollector;

struct PhaseShiftSummary {
    std::uint64_t switches = 0;
    std::uint64_t owner_biased_writes = 0;
    std::uint64_t owner_handoffs = 0;
};

void RecordMode(StatsCollector* stats, PhaseShiftSummary* summary, CoherenceMode before, CoherenceMode after) {
    stats->RecordCoherenceMode(after, before != after);
    if (before != after) {
        ++summary->switches;
    }
}

void DrainBatch(CacheManager* cache, StatsCollector* stats, leomem::NodeId node) {
    auto batch = cache->DrainRemoteWrites(node);
    if (!batch.writes.empty()) {
        stats->AddFlushedRemoteBatch(batch.writes.size(), batch.total_bytes);
    }
}

PhaseShiftSummary SimulatePhaseShift(CacheManager* cache, StatsCollector* stats) {
    PhaseShiftSummary summary;
    auto meta = std::make_shared<BlockMeta>();
    GlobalAddr remote{1, 0, 16384};
    std::uint64_t payload = 7;
    std::uint64_t payload2 = 11;

    for (int i = 0; i < 3; ++i) {
        const auto before = meta->mode.load(std::memory_order_relaxed);
        const auto mode = cache->ObserveAccess(meta, AccessType::kRead, false);
        RecordMode(stats, &summary, before, mode);
        if (cache->ShouldCache(meta) && mode != CoherenceMode::kWI) {
            cache->InsertCached(remote, &payload, sizeof(payload));
        }
    }

    for (int i = 0; i < 1; ++i) {
        const auto before = meta->mode.load(std::memory_order_relaxed);
        const auto mode = cache->ObserveAccess(meta, AccessType::kWrite, false);
        RecordMode(stats, &summary, before, mode);
        const bool range_invalidation = (mode == CoherenceMode::kWI);
        const std::size_t invalidated = range_invalidation
            ? cache->InvalidateRange(remote, sizeof(payload) * 2)
            : cache->Invalidate(remote, sizeof(payload));
        stats->AddCacheInvalidations(range_invalidation, invalidated);

        if (mode == CoherenceMode::kAdaptive) {
            cache->InsertCached(remote, &payload, sizeof(payload));
            stats->IncOwnerBiasedWrite();
            ++summary.owner_biased_writes;
            ++summary.owner_handoffs;
        }

        const bool merged = cache->QueueRemoteWrite(
            remote, &payload, sizeof(payload), mode, mode == CoherenceMode::kAdaptive, mode == CoherenceMode::kAdaptive);
        stats->IncQueuedRemoteWrite();
        if (merged) stats->IncMergedRemoteWrite();

        GlobalAddr adjacent = remote;
        adjacent.offset += sizeof(payload);
        const bool merged_adj = cache->QueueRemoteWrite(
            adjacent, &payload2, sizeof(payload2), mode, mode == CoherenceMode::kAdaptive, false);
        stats->IncQueuedRemoteWrite();
        if (merged_adj) stats->IncMergedRemoteWrite();

        const std::size_t threshold = cache->EffectiveBatchThreshold(mode);
        if (mode == CoherenceMode::kWI || cache->ShouldFlushDestination(remote.home_node, threshold)) {
            DrainBatch(cache, stats, remote.home_node);
        }
    }

    for (int i = 0; i < 3; ++i) {
        const auto before = meta->mode.load(std::memory_order_relaxed);
        const auto mode = cache->ObserveAccess(meta, AccessType::kWrite, false);
        RecordMode(stats, &summary, before, mode);
        const bool range_invalidation = (mode == CoherenceMode::kWI);
        const std::size_t invalidated = range_invalidation
            ? cache->InvalidateRange(remote, sizeof(payload) * 2)
            : cache->Invalidate(remote, sizeof(payload));
        stats->AddCacheInvalidations(range_invalidation, invalidated);

        if (mode == CoherenceMode::kAdaptive) {
            cache->InsertCached(remote, &payload, sizeof(payload));
            stats->IncOwnerBiasedWrite();
            ++summary.owner_biased_writes;
            ++summary.owner_handoffs;
        }

        const bool merged = cache->QueueRemoteWrite(
            remote, &payload, sizeof(payload), mode, mode == CoherenceMode::kAdaptive, mode == CoherenceMode::kAdaptive);
        stats->IncQueuedRemoteWrite();
        if (merged) stats->IncMergedRemoteWrite();

        GlobalAddr adjacent = remote;
        adjacent.offset += sizeof(payload);
        const bool merged_adj = cache->QueueRemoteWrite(
            adjacent, &payload2, sizeof(payload2), mode, mode == CoherenceMode::kAdaptive, false);
        stats->IncQueuedRemoteWrite();
        if (merged_adj) stats->IncMergedRemoteWrite();

        const std::size_t threshold = cache->EffectiveBatchThreshold(mode);
        if (mode == CoherenceMode::kWI || cache->ShouldFlushDestination(remote.home_node, threshold)) {
            DrainBatch(cache, stats, remote.home_node);
        }
    }

    DrainBatch(cache, stats, remote.home_node);
    return summary;
}

}  // namespace

int main() {
    Config cfg;
    cfg.profiling_window_size = 6;
    cfg.cache_admission_min_reads = 2;
    cfg.cache_admission_max_writes = 1;
    cfg.cache_admission_max_sharers = 8;
    cfg.cache_admission_max_reuse_distance = 8;
    cfg.cache_admission_min_read_ratio = 0.55;
    cfg.cache_admission_max_phase_change_ratio = 0.45;
    cfg.remote_write_batch_threshold = 3;
    cfg.adaptive_mode_batch_threshold = 2;
    cfg.coherence_sharer_promote_threshold = 2;
    cfg.coherence_invalidation_promote_threshold = 2;
    cfg.coherence_write_dominant_ratio = 0.50;
    cfg.coherence_phase_change_promote_ratio = 0.30;

    CacheManager cache(cfg);
    StatsCollector stats;
    if (cache.Init() != leomem::Status::kOk) {
        std::fprintf(stderr, "cache init failed\n");
        return 1;
    }

    const auto summary = SimulatePhaseShift(&cache, &stats);
    const auto snapshot = stats.Snapshot();

    std::printf("phase-shift benchmark\n");
    std::printf("coherence hits: WI=%llu SI=%llu Adaptive=%llu\n",
                static_cast<unsigned long long>(snapshot.coherence_mode_wi),
                static_cast<unsigned long long>(snapshot.coherence_mode_si),
                static_cast<unsigned long long>(snapshot.coherence_mode_adaptive));
    std::printf("coherence switches: stats=%llu workload=%llu\n",
                static_cast<unsigned long long>(snapshot.coherence_mode_switches),
                static_cast<unsigned long long>(summary.switches));
    std::printf("cache invalidations: precise=%llu range=%llu\n",
                static_cast<unsigned long long>(snapshot.precise_cache_invalidations),
                static_cast<unsigned long long>(snapshot.range_cache_invalidations));
    std::printf("owner-biased writes: stats=%llu workload=%llu handoffs=%llu\n",
                static_cast<unsigned long long>(snapshot.owner_biased_writes),
                static_cast<unsigned long long>(summary.owner_biased_writes),
                static_cast<unsigned long long>(summary.owner_handoffs));
    std::printf("batched writes: queued=%llu merged=%llu flushed_writes=%llu batches=%llu bytes=%llu\n",
                static_cast<unsigned long long>(snapshot.queued_remote_writes),
                static_cast<unsigned long long>(snapshot.merged_remote_writes),
                static_cast<unsigned long long>(snapshot.flushed_remote_writes),
                static_cast<unsigned long long>(snapshot.flushed_remote_batches),
                static_cast<unsigned long long>(snapshot.flushed_remote_write_bytes));

    cache.Shutdown();
    return 0;
}
