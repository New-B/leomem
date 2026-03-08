#pragma once

#include <atomic>
#include "leomem/stats.h"

namespace leomem {

class StatsCollector {
public:
    void IncAlloc() { alloc_ops_.fetch_add(1, std::memory_order_relaxed); }
    void IncRead(bool local);
    void IncWrite(bool local);

    StatsSnapshot Snapshot() const;

private:
    std::atomic<std::uint64_t> alloc_ops_{0};
    std::atomic<std::uint64_t> read_ops_{0};
    std::atomic<std::uint64_t> write_ops_{0};
    std::atomic<std::uint64_t> local_reads_{0};
    std::atomic<std::uint64_t> local_writes_{0};
    std::atomic<std::uint64_t> remote_reads_{0};
    std::atomic<std::uint64_t> remote_writes_{0};
};

}  // namespace leomem