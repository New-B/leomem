#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "leomem/leomem.h"
#include "runtime/config_loader.h"

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

    std::vector<GlobalAddr> remotes;
    for (NodeId node = 0; node < std::max<std::uint16_t>(cfg.nr_nodes, 2); ++node) {
        if (node == cfg.node_id) continue;
        GlobalAddr remote;
        remote.home_node = node;
        remote.region_id = 0;
        remote.offset = 32768 + static_cast<std::uint64_t>(node) * cfg.block_size;
        remotes.push_back(remote);
    }
    if (remotes.empty()) {
        GlobalAddr remote;
        remote.home_node = 1;
        remote.region_id = 0;
        remote.offset = 32768;
        remotes.push_back(remote);
    }

    std::uint64_t value = 1;
    std::uint64_t out = 0;

    for (const auto& remote : remotes) {
        for (int i = 0; i < 6; ++i) {
            if (lm_read(remote, &out, sizeof(out)) != Status::kOk) {
                std::fprintf(stderr, "phase-1 read failed on node %u\n", remote.home_node);
                return 1;
            }
        }
    }

    for (const auto& remote : remotes) {
        for (int i = 0; i < 2; ++i) {
            value += 1;
            if (lm_write(remote, &value, sizeof(value)) != Status::kOk) {
                std::fprintf(stderr, "phase-2 adaptive write failed on node %u\n", remote.home_node);
                return 1;
            }
        }
    }
    if (lm_fence() != Status::kOk) {
        std::fprintf(stderr, "phase-2 fence failed\n");
        return 1;
    }

    for (const auto& remote : remotes) {
        for (int i = 0; i < 4; ++i) {
            value += 1;
            if (lm_write(remote, &value, sizeof(value)) != Status::kOk) {
                std::fprintf(stderr, "phase-3 write failed on node %u\n", remote.home_node);
                return 1;
            }
            if (lm_read(remote, &out, sizeof(out)) != Status::kOk) {
                std::fprintf(stderr, "phase-3 read failed on node %u\n", remote.home_node);
                return 1;
            }
        }
    }
    if (lm_fence() != Status::kOk) {
        std::fprintf(stderr, "final fence failed\n");
        return 1;
    }

    const StatsSnapshot st = lm_get_stats();
    std::printf("runtime phase-shift benchmark\n");
    std::printf("nodes: local=%u total_targets=%zu\n",
                static_cast<unsigned>(cfg.node_id),
                remotes.size());
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

    lm_shutdown();
    return 0;
}
