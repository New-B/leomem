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
};

}  // namespace leomem