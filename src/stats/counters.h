#pragma once

#include <atomic>
#include "leomem/block.h"
#include "leomem/stats.h"

namespace leomem {

class StatsCollector {
public:
    void IncAlloc() { alloc_ops_.fetch_add(1, std::memory_order_relaxed); }
    void IncRead(bool local);
    void IncWrite(bool local);
    void IncCacheHit() { cache_hits_.fetch_add(1, std::memory_order_relaxed); }
    void IncCacheMiss() { cache_misses_.fetch_add(1, std::memory_order_relaxed); }
    void IncCacheAdmission(bool admitted);
    void IncQueuedRemoteWrite() { queued_remote_writes_.fetch_add(1, std::memory_order_relaxed); }
    void AddFlushedRemoteBatch(std::size_t writes, std::size_t bytes);
    void IncMergedRemoteWrite() { merged_remote_writes_.fetch_add(1, std::memory_order_relaxed); }
    void RecordCoherenceMode(CoherenceMode mode, bool switched);
    void AddCacheInvalidations(bool range_based, std::size_t count);
    void IncOwnerBiasedWrite() { owner_biased_writes_.fetch_add(1, std::memory_order_relaxed); }
    void IncOwnerHandoff() { owner_handoffs_.fetch_add(1, std::memory_order_relaxed); }

    StatsSnapshot Snapshot() const;

private:
    std::atomic<std::uint64_t> alloc_ops_{0};
    std::atomic<std::uint64_t> read_ops_{0};
    std::atomic<std::uint64_t> write_ops_{0};
    std::atomic<std::uint64_t> local_reads_{0};
    std::atomic<std::uint64_t> local_writes_{0};
    std::atomic<std::uint64_t> remote_reads_{0};
    std::atomic<std::uint64_t> remote_writes_{0};
    std::atomic<std::uint64_t> cache_hits_{0};
    std::atomic<std::uint64_t> cache_misses_{0};
    std::atomic<std::uint64_t> cache_admissions_{0};
    std::atomic<std::uint64_t> cache_rejections_{0};
    std::atomic<std::uint64_t> queued_remote_writes_{0};
    std::atomic<std::uint64_t> flushed_remote_writes_{0};
    std::atomic<std::uint64_t> flushed_remote_write_bytes_{0};
    std::atomic<std::uint64_t> flushed_remote_batches_{0};
    std::atomic<std::uint64_t> merged_remote_writes_{0};
    std::atomic<std::uint64_t> coherence_mode_switches_{0};
    std::atomic<std::uint64_t> coherence_mode_wi_{0};
    std::atomic<std::uint64_t> coherence_mode_si_{0};
    std::atomic<std::uint64_t> coherence_mode_adaptive_{0};
    std::atomic<std::uint64_t> precise_cache_invalidations_{0};
    std::atomic<std::uint64_t> range_cache_invalidations_{0};
    std::atomic<std::uint64_t> owner_biased_writes_{0};
    std::atomic<std::uint64_t> owner_handoffs_{0};
};

}  // namespace leomem
