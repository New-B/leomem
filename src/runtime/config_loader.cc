#include "runtime/config_loader.h"

#include <fstream>
#include <sstream>
#include <string>

namespace leomem {

static void ApplyKV(const std::string& key, const std::string& value, Config* cfg) {
    if (key == "node_id") cfg->node_id = static_cast<std::uint16_t>(std::stoul(value));
    else if (key == "nr_nodes") cfg->nr_nodes = static_cast<std::uint16_t>(std::stoul(value));
    else if (key == "block_size") cfg->block_size = std::stoull(value);
    else if (key == "local_region_size") cfg->local_region_size = std::stoull(value);
    else if (key == "enable_rdma") cfg->enable_rdma = (value == "1" || value == "true");
    else if (key == "enable_cache") cfg->enable_cache = (value == "1" || value == "true");
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