#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "leomem/leomem.h"
#include "runtime/config_loader.h"

namespace {

int RunWorkload(const leomem::Config& cfg) {
    using namespace leomem;

    std::vector<GlobalAddr> remotes;
    for (NodeId node = 0; node < std::max<std::uint16_t>(cfg.nr_nodes, 2); ++node) {
        if (node == cfg.node_id) continue;
        GlobalAddr remote;
        remote.home_node = node;
        remote.region_id = 0;
        remote.offset = 65536 + static_cast<std::uint64_t>(node) * cfg.block_size;
        remotes.push_back(remote);
    }

    if (remotes.empty()) {
        GlobalAddr remote;
        remote.home_node = 1;
        remote.region_id = 0;
        remote.offset = 65536;
        remotes.push_back(remote);
    }

    std::uint64_t value = 1;
    std::uint64_t observed = 0;
    for (int iter = 0; iter < 3; ++iter) {
        for (const auto& remote : remotes) {
            for (int sample = 0; sample < 8; ++sample) {
                if (lm_read(remote, &observed, sizeof(observed)) != Status::kOk) {
                    std::fprintf(stderr, "iter %d read phase failed on node %u\n", iter, remote.home_node);
                    return 1;
                }
            }
        }

        for (const auto& remote : remotes) {
            for (int update = 0; update < 4; ++update) {
                value += static_cast<std::uint64_t>(iter + update + 1);
                if (lm_write(remote, &value, sizeof(value)) != Status::kOk) {
                    std::fprintf(stderr, "iter %d write phase failed on node %u\n", iter, remote.home_node);
                    return 1;
                }
            }
        }

        for (const auto& remote : remotes) {
            value += 7;
            if (lm_write(remote, &value, sizeof(value)) != Status::kOk) {
                std::fprintf(stderr, "iter %d shuffle write failed on node %u\n", iter, remote.home_node);
                return 1;
            }
            if (lm_read(remote, &observed, sizeof(observed)) != Status::kOk) {
                std::fprintf(stderr, "iter %d validation read failed on node %u\n", iter, remote.home_node);
                return 1;
            }
        }

        if (lm_fence() != Status::kOk) {
            std::fprintf(stderr, "iter %d fence failed\n", iter);
            return 1;
        }
    }

    const StatsSnapshot st = lm_get_stats();
    std::printf("iterative analytics benchmark\n");
    std::printf("nodes: local=%u targets=%zu total=%u\n",
                static_cast<unsigned>(cfg.node_id),
                remotes.size(),
                static_cast<unsigned>(cfg.nr_nodes));
    std::printf("ops: reads=%llu writes=%llu remote_reads=%llu remote_writes=%llu\n",
                static_cast<unsigned long long>(st.read_ops),
                static_cast<unsigned long long>(st.write_ops),
                static_cast<unsigned long long>(st.remote_reads),
                static_cast<unsigned long long>(st.remote_writes));
    std::printf("cache: hits=%llu misses=%llu admissions=%llu rejections=%llu evictions=%llu resident_entries=%llu resident_bytes=%llu\n",
                static_cast<unsigned long long>(st.cache_hits),
                static_cast<unsigned long long>(st.cache_misses),
                static_cast<unsigned long long>(st.cache_admissions),
                static_cast<unsigned long long>(st.cache_rejections),
                static_cast<unsigned long long>(st.cache_evictions),
                static_cast<unsigned long long>(st.cache_resident_entries),
                static_cast<unsigned long long>(st.cache_resident_bytes));
    std::printf("coherence: switches=%llu wi=%llu si=%llu adaptive=%llu\n",
                static_cast<unsigned long long>(st.coherence_mode_switches),
                static_cast<unsigned long long>(st.coherence_mode_wi),
                static_cast<unsigned long long>(st.coherence_mode_si),
                static_cast<unsigned long long>(st.coherence_mode_adaptive));
    std::printf("protocol: precise_inv=%llu range_inv=%llu owner_biased=%llu handoffs=%llu\n",
                static_cast<unsigned long long>(st.precise_cache_invalidations),
                static_cast<unsigned long long>(st.range_cache_invalidations),
                static_cast<unsigned long long>(st.owner_biased_writes),
                static_cast<unsigned long long>(st.owner_handoffs));
    std::printf("batching: queued=%llu merged=%llu flushed_writes=%llu batches=%llu bytes=%llu\n",
                static_cast<unsigned long long>(st.queued_remote_writes),
                static_cast<unsigned long long>(st.merged_remote_writes),
                static_cast<unsigned long long>(st.flushed_remote_writes),
                static_cast<unsigned long long>(st.flushed_remote_batches),
                static_cast<unsigned long long>(st.flushed_remote_write_bytes));
    std::printf("csv: benchmark=iterative_analytics,node=%u,nodes=%u,reads=%llu,writes=%llu,cache_hits=%llu,cache_misses=%llu,admissions=%llu,rejections=%llu,cache_evictions=%llu,cache_resident_entries=%llu,cache_resident_bytes=%llu,switches=%llu,wi=%llu,si=%llu,adaptive=%llu,precise_inv=%llu,range_inv=%llu,owner_biased=%llu,handoffs=%llu,merged=%llu,batches=%llu,bytes=%llu\n",
                static_cast<unsigned>(cfg.node_id),
                static_cast<unsigned>(cfg.nr_nodes),
                static_cast<unsigned long long>(st.read_ops),
                static_cast<unsigned long long>(st.write_ops),
                static_cast<unsigned long long>(st.cache_hits),
                static_cast<unsigned long long>(st.cache_misses),
                static_cast<unsigned long long>(st.cache_admissions),
                static_cast<unsigned long long>(st.cache_rejections),
                static_cast<unsigned long long>(st.cache_evictions),
                static_cast<unsigned long long>(st.cache_resident_entries),
                static_cast<unsigned long long>(st.cache_resident_bytes),
                static_cast<unsigned long long>(st.coherence_mode_switches),
                static_cast<unsigned long long>(st.coherence_mode_wi),
                static_cast<unsigned long long>(st.coherence_mode_si),
                static_cast<unsigned long long>(st.coherence_mode_adaptive),
                static_cast<unsigned long long>(st.precise_cache_invalidations),
                static_cast<unsigned long long>(st.range_cache_invalidations),
                static_cast<unsigned long long>(st.owner_biased_writes),
                static_cast<unsigned long long>(st.owner_handoffs),
                static_cast<unsigned long long>(st.merged_remote_writes),
                static_cast<unsigned long long>(st.flushed_remote_batches),
                static_cast<unsigned long long>(st.flushed_remote_write_bytes));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace leomem;

    const char* config_path = argc > 1 ? argv[1] : nullptr;
    Config cfg;
    if (config_path != nullptr) {
        Status load = LoadConfigFile(config_path, &cfg);
        if (load != Status::kOk) {
            std::fprintf(stderr, "config load failed: %s\n", StatusToString(load));
            return 1;
        }
    }

    if (lm_init(config_path) != Status::kOk) {
        std::fprintf(stderr, "lm_init failed\n");
        return 1;
    }

    const int rc = RunWorkload(cfg);
    lm_shutdown();
    return rc;
}
