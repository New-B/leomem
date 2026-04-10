#include "runtime/config_loader.h"

#include <fstream>
#include <sstream>
#include <string>

namespace leomem {

namespace {

std::int32_t ParseCoherenceModeOverride(const std::string& value) {
    if (value == "wi" || value == "WI") return 0;
    if (value == "si" || value == "SI") return 1;
    if (value == "adaptive" || value == "ADAPTIVE") return 2;
    return static_cast<std::int32_t>(std::stoi(value));
}

std::int32_t ParseAdmissionPolicy(const std::string& value) {
    if (value == "profiled") return 0;
    if (value == "always") return 1;
    if (value == "never") return 2;
    return static_cast<std::int32_t>(std::stoi(value));
}

}  // namespace

static void ApplyKV(const std::string& key, const std::string& value, Config* cfg) {
    if (key == "node_id") cfg->node_id = static_cast<std::uint16_t>(std::stoul(value));
    else if (key == "nr_nodes") cfg->nr_nodes = static_cast<std::uint16_t>(std::stoul(value));
    else if (key == "block_size") cfg->block_size = std::stoull(value);
    else if (key == "local_region_size") cfg->local_region_size = std::stoull(value);
    else if (key == "enable_rdma") cfg->enable_rdma = (value == "1" || value == "true");
    else if (key == "enable_cache") cfg->enable_cache = (value == "1" || value == "true");
    else if (key == "enable_write_batching") cfg->enable_write_batching = (value == "1" || value == "true");
    else if (key == "rdma_enable_control_path") cfg->rdma_enable_control_path = (value == "1" || value == "true");
    else if (key == "coherence_mode_override") cfg->coherence_mode_override = ParseCoherenceModeOverride(value);
    else if (key == "cache_admission_policy") cfg->cache_admission_policy = ParseAdmissionPolicy(value);
    else if (key == "rdma_device_name") cfg->rdma_device_name = value;
    else if (key == "rdma_port") cfg->rdma_port = static_cast<std::uint8_t>(std::stoul(value));
    else if (key == "rdma_gid_index") cfg->rdma_gid_index = static_cast<std::uint8_t>(std::stoul(value));
    else if (key == "rdma_max_inline_bytes") cfg->rdma_max_inline_bytes = std::stoull(value);
    else if (key == "rdma_cq_depth") cfg->rdma_cq_depth = std::stoull(value);
    else if (key == "rdma_sq_depth") cfg->rdma_sq_depth = std::stoull(value);
    else if (key == "rdma_rq_depth") cfg->rdma_rq_depth = std::stoull(value);
    else if (key == "rdma_bootstrap_timeout_ms") cfg->rdma_bootstrap_timeout_ms = std::stoull(value);
    else if (key == "rdma_exchange_dir") cfg->rdma_exchange_dir = value;
    else if (key == "profiling_window_size") cfg->profiling_window_size = std::stoull(value);
    else if (key == "cache_admission_min_reads") cfg->cache_admission_min_reads = std::stoull(value);
    else if (key == "cache_admission_max_writes") cfg->cache_admission_max_writes = std::stoull(value);
    else if (key == "cache_admission_max_sharers") cfg->cache_admission_max_sharers = std::stoull(value);
    else if (key == "cache_admission_max_reuse_distance") cfg->cache_admission_max_reuse_distance = std::stoull(value);
    else if (key == "cache_admission_min_read_ratio") cfg->cache_admission_min_read_ratio = std::stod(value);
    else if (key == "cache_admission_max_phase_change_ratio") cfg->cache_admission_max_phase_change_ratio = std::stod(value);
    else if (key == "remote_write_batch_threshold") cfg->remote_write_batch_threshold = std::stoull(value);
    else if (key == "coherence_sharer_promote_threshold") cfg->coherence_sharer_promote_threshold = std::stoull(value);
    else if (key == "coherence_invalidation_promote_threshold") cfg->coherence_invalidation_promote_threshold = std::stoull(value);
    else if (key == "coherence_write_dominant_ratio") cfg->coherence_write_dominant_ratio = std::stod(value);
    else if (key == "coherence_phase_change_promote_ratio") cfg->coherence_phase_change_promote_ratio = std::stod(value);
    else if (key == "adaptive_mode_batch_threshold") cfg->adaptive_mode_batch_threshold = std::stoull(value);
}

Status LoadConfigFile(const std::string& path, Config* cfg) {
    if (cfg == nullptr) return Status::kInvalidArg;
    if (path.empty()) return Status::kOk;

    std::ifstream fin(path);
    if (!fin.is_open()) return Status::kNotFound;

    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        ApplyKV(key, value, cfg);
    }

    cfg->cluster_config_path = path;
    return Status::kOk;
}

}  // namespace leomem
