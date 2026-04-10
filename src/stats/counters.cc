#include "stats/counters.h"

namespace leomem {

void StatsCollector::IncRead(bool local) {
    read_ops_.fetch_add(1, std::memory_order_relaxed);
    if (local) local_reads_.fetch_add(1, std::memory_order_relaxed);
    else remote_reads_.fetch_add(1, std::memory_order_relaxed);
}

void StatsCollector::IncWrite(bool local) {
    write_ops_.fetch_add(1, std::memory_order_relaxed);
    if (local) local_writes_.fetch_add(1, std::memory_order_relaxed);
    else remote_writes_.fetch_add(1, std::memory_order_relaxed);
}

void StatsCollector::IncCacheAdmission(bool admitted) {
    if (admitted) cache_admissions_.fetch_add(1, std::memory_order_relaxed);
    else cache_rejections_.fetch_add(1, std::memory_order_relaxed);
}

void StatsCollector::AddFlushedRemoteBatch(std::size_t writes, std::size_t bytes) {
    flushed_remote_batches_.fetch_add(1, std::memory_order_relaxed);
    flushed_remote_writes_.fetch_add(writes, std::memory_order_relaxed);
    flushed_remote_write_bytes_.fetch_add(bytes, std::memory_order_relaxed);
}

void StatsCollector::RecordCoherenceMode(CoherenceMode mode, bool switched) {
    if (switched) {
        coherence_mode_switches_.fetch_add(1, std::memory_order_relaxed);
    }

    switch (mode) {
        case CoherenceMode::kWI:
            coherence_mode_wi_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CoherenceMode::kSI:
            coherence_mode_si_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CoherenceMode::kAdaptive:
            coherence_mode_adaptive_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void StatsCollector::AddCacheInvalidations(bool range_based, std::size_t count) {
    if (range_based) {
        range_cache_invalidations_.fetch_add(count, std::memory_order_relaxed);
    } else {
        precise_cache_invalidations_.fetch_add(count, std::memory_order_relaxed);
    }
}

StatsSnapshot StatsCollector::Snapshot() const {
    StatsSnapshot s;
    s.alloc_ops = alloc_ops_.load(std::memory_order_relaxed);
    s.read_ops = read_ops_.load(std::memory_order_relaxed);
    s.write_ops = write_ops_.load(std::memory_order_relaxed);
    s.local_reads = local_reads_.load(std::memory_order_relaxed);
    s.local_writes = local_writes_.load(std::memory_order_relaxed);
    s.remote_reads = remote_reads_.load(std::memory_order_relaxed);
    s.remote_writes = remote_writes_.load(std::memory_order_relaxed);
    s.cache_hits = cache_hits_.load(std::memory_order_relaxed);
    s.cache_misses = cache_misses_.load(std::memory_order_relaxed);
    s.cache_admissions = cache_admissions_.load(std::memory_order_relaxed);
    s.cache_rejections = cache_rejections_.load(std::memory_order_relaxed);
    s.queued_remote_writes = queued_remote_writes_.load(std::memory_order_relaxed);
    s.flushed_remote_writes = flushed_remote_writes_.load(std::memory_order_relaxed);
    s.flushed_remote_write_bytes = flushed_remote_write_bytes_.load(std::memory_order_relaxed);
    s.flushed_remote_batches = flushed_remote_batches_.load(std::memory_order_relaxed);
    s.merged_remote_writes = merged_remote_writes_.load(std::memory_order_relaxed);
    s.coherence_mode_switches = coherence_mode_switches_.load(std::memory_order_relaxed);
    s.coherence_mode_wi = coherence_mode_wi_.load(std::memory_order_relaxed);
    s.coherence_mode_si = coherence_mode_si_.load(std::memory_order_relaxed);
    s.coherence_mode_adaptive = coherence_mode_adaptive_.load(std::memory_order_relaxed);
    s.precise_cache_invalidations = precise_cache_invalidations_.load(std::memory_order_relaxed);
    s.range_cache_invalidations = range_cache_invalidations_.load(std::memory_order_relaxed);
    s.owner_biased_writes = owner_biased_writes_.load(std::memory_order_relaxed);
    s.owner_handoffs = owner_handoffs_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace leomem
