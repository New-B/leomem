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

    std::string cluster_config_path;
};

}  // namespace leomem