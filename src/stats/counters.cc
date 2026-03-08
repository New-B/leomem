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

StatsSnapshot StatsCollector::Snapshot() const {
    StatsSnapshot s;
    s.alloc_ops = alloc_ops_.load(std::memory_order_relaxed);
    s.read_ops = read_ops_.load(std::memory_order_relaxed);
    s.write_ops = write_ops_.load(std::memory_order_relaxed);
    s.local_reads = local_reads_.load(std::memory_order_relaxed);
    s.local_writes = local_writes_.load(std::memory_order_relaxed);
    s.remote_reads = remote_reads_.load(std::memory_order_relaxed);
    s.remote_writes = remote_writes_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace leomem