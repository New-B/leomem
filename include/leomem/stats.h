#pragma once

#include <cstdint>

namespace leomem {

struct StatsSnapshot {
    std::uint64_t alloc_ops = 0;
    std::uint64_t read_ops = 0;
    std::uint64_t write_ops = 0;
    std::uint64_t local_reads = 0;
    std::uint64_t local_writes = 0;
    std::uint64_t remote_reads = 0;
    std::uint64_t remote_writes = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t cache_admissions = 0;
    std::uint64_t cache_rejections = 0;
    std::uint64_t cache_evictions = 0;
    std::uint64_t cache_resident_entries = 0;
    std::uint64_t cache_resident_bytes = 0;
    std::uint64_t queued_remote_writes = 0;
    std::uint64_t flushed_remote_writes = 0;
    std::uint64_t flushed_remote_write_bytes = 0;
    std::uint64_t flushed_remote_batches = 0;
    std::uint64_t merged_remote_writes = 0;
    std::uint64_t coherence_mode_switches = 0;
    std::uint64_t coherence_mode_wi = 0;
    std::uint64_t coherence_mode_si = 0;
    std::uint64_t coherence_mode_adaptive = 0;
    std::uint64_t precise_cache_invalidations = 0;
    std::uint64_t range_cache_invalidations = 0;
    std::uint64_t owner_biased_writes = 0;
    std::uint64_t owner_handoffs = 0;
};

}  // namespace leomem
