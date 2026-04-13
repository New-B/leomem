#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace leomem {

struct Config {
    std::uint16_t node_id = 0;
    std::uint16_t nr_nodes = 1;

    std::size_t block_size = 4096;
    std::size_t local_region_size = 64ULL * 1024 * 1024;  // 64 MB

    bool enable_rdma = false;
    bool enable_cache = true;
    bool enable_write_batching = true;
    bool rdma_enable_control_path = true;
    std::int32_t coherence_mode_override = -1;
    std::int32_t cache_admission_policy = 0;
    std::size_t control_ack_timeout_polls = 4;
    std::size_t control_ack_max_retries = 2;

    std::string rdma_device_name;
    std::uint8_t rdma_port = 1;
    std::uint8_t rdma_gid_index = 0;
    std::size_t rdma_max_inline_bytes = 256;
    std::size_t rdma_cq_depth = 1024;
    std::size_t rdma_sq_depth = 1024;
    std::size_t rdma_rq_depth = 1024;
    std::size_t rdma_bootstrap_timeout_ms = 30000;
    std::string rdma_exchange_dir;

    std::size_t profiling_window_size = 8;
    std::size_t cache_capacity_bytes = 0;
    std::size_t cache_admission_min_reads = 2;
    std::size_t cache_admission_max_writes = 1;
    std::size_t cache_admission_max_sharers = 2;
    std::size_t cache_admission_max_reuse_distance = 4;
    double cache_admission_min_read_ratio = 0.60;
    double cache_admission_max_phase_change_ratio = 0.35;
    std::size_t remote_write_batch_threshold = 1;
    std::size_t coherence_sharer_promote_threshold = 2;
    std::size_t coherence_invalidation_promote_threshold = 2;
    double coherence_write_dominant_ratio = 0.55;
    double coherence_phase_change_promote_ratio = 0.30;
    std::size_t adaptive_mode_batch_threshold = 1;

    std::string cluster_config_path;
};

}  // namespace leomem
