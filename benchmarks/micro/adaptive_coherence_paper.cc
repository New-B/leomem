#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include <optional>

#include "leomem/leomem.h"
#include "runtime/config_loader.h"
#include "runtime/context.h"

namespace {

using leomem::Config;
using leomem::GlobalAddr;
using leomem::NodeId;
using leomem::RuntimeContext;
using leomem::StatsSnapshot;
using leomem::Status;

constexpr int kTotalNodes = 8;
constexpr int kTotalEpochs = 300;
constexpr int kWarmupEpochs = 100;
constexpr int kMeasuredEpochs = 200;
constexpr std::size_t kBlockSize = 4096;
constexpr std::size_t kHotSetBlocks = 256;
constexpr std::size_t kWriterUpdatesPerEpoch = 32;
constexpr std::size_t kWritesPerBlock = 4;
constexpr std::size_t kWriteSize = 64;
constexpr std::size_t kReadsPerReaderPerEpoch = 2048;

struct RunSummary {
    std::string mode;
    int readers = 0;
    double mean_epoch_us = 0.0;
    StatsSnapshot stats{};
};

StatsSnapshot DiffStats(const StatsSnapshot& after, const StatsSnapshot& before) {
    StatsSnapshot delta;
    delta.alloc_ops = after.alloc_ops - before.alloc_ops;
    delta.read_ops = after.read_ops - before.read_ops;
    delta.write_ops = after.write_ops - before.write_ops;
    delta.local_reads = after.local_reads - before.local_reads;
    delta.local_writes = after.local_writes - before.local_writes;
    delta.remote_reads = after.remote_reads - before.remote_reads;
    delta.remote_writes = after.remote_writes - before.remote_writes;
    delta.cache_hits = after.cache_hits - before.cache_hits;
    delta.cache_misses = after.cache_misses - before.cache_misses;
    delta.cache_admissions = after.cache_admissions - before.cache_admissions;
    delta.cache_rejections = after.cache_rejections - before.cache_rejections;
    delta.cache_evictions = after.cache_evictions - before.cache_evictions;
    delta.cache_resident_entries = after.cache_resident_entries;
    delta.cache_resident_bytes = after.cache_resident_bytes;
    delta.queued_remote_writes = after.queued_remote_writes - before.queued_remote_writes;
    delta.flushed_remote_writes = after.flushed_remote_writes - before.flushed_remote_writes;
    delta.flushed_remote_write_bytes = after.flushed_remote_write_bytes - before.flushed_remote_write_bytes;
    delta.flushed_remote_batches = after.flushed_remote_batches - before.flushed_remote_batches;
    delta.merged_remote_writes = after.merged_remote_writes - before.merged_remote_writes;
    delta.coherence_mode_switches = after.coherence_mode_switches - before.coherence_mode_switches;
    delta.coherence_mode_wi = after.coherence_mode_wi - before.coherence_mode_wi;
    delta.coherence_mode_si = after.coherence_mode_si - before.coherence_mode_si;
    delta.coherence_mode_adaptive = after.coherence_mode_adaptive - before.coherence_mode_adaptive;
    delta.precise_cache_invalidations = after.precise_cache_invalidations - before.precise_cache_invalidations;
    delta.range_cache_invalidations = after.range_cache_invalidations - before.range_cache_invalidations;
    delta.owner_biased_writes = after.owner_biased_writes - before.owner_biased_writes;
    delta.owner_handoffs = after.owner_handoffs - before.owner_handoffs;
    return delta;
}

Config MakePaperConfig(const Config& base, const std::string& mode_name) {
    Config cfg = base;
    cfg.node_id = 0;
    cfg.nr_nodes = kTotalNodes;
    cfg.block_size = kBlockSize;
    cfg.enable_rdma = false;
    cfg.enable_cache = true;
    cfg.enable_write_batching = false;
    cfg.cache_admission_policy = 2;
    cfg.remote_write_batch_threshold = 1;
    cfg.adaptive_mode_batch_threshold = 1;
    cfg.local_region_size = std::max<std::size_t>(cfg.local_region_size, 16ULL * 1024 * 1024);

    if (mode_name == "wi") {
        cfg.coherence_mode_override = 0;
    } else if (mode_name == "si") {
        cfg.coherence_mode_override = 1;
    } else {
        cfg.coherence_mode_override = -1;
    }
    return cfg;
}

std::vector<GlobalAddr> MakeHotSet(int readers) {
    std::vector<GlobalAddr> hot_set;
    hot_set.reserve(kHotSetBlocks);
    for (std::size_t i = 0; i < kHotSetBlocks; ++i) {
        const NodeId home = static_cast<NodeId>(1 + (i % static_cast<std::size_t>(readers)));
        hot_set.push_back(GlobalAddr{
            home,
            0,
            static_cast<std::uint64_t>(i * kBlockSize),
        });
    }
    return hot_set;
}

void InitializeHotSet(const std::vector<GlobalAddr>& hot_set) {
    std::array<std::uint8_t, kWriteSize> zeros{};
    for (const auto& addr : hot_set) {
        for (std::size_t write_idx = 0; write_idx < kWritesPerBlock; ++write_idx) {
            GlobalAddr chunk = addr;
            chunk.offset += write_idx * kWriteSize;
            if (leomem::lm_write(chunk, zeros.data(), zeros.size()) != Status::kOk) {
                std::fprintf(stderr, "hot-set init write failed on node %u\n", chunk.home_node);
                std::abort();
            }
        }
    }
    if (leomem::lm_fence() != Status::kOk) {
        std::fprintf(stderr, "hot-set init fence failed\n");
        std::abort();
    }
}

RunSummary RunMode(const Config& base_cfg, const std::string& mode_name, int readers) {
    const Config cfg = MakePaperConfig(base_cfg, mode_name);
    RuntimeContext& ctx = RuntimeContext::Instance();
    if (ctx.Init(cfg) != Status::kOk) {
        std::fprintf(stderr, "runtime init failed for mode=%s readers=%d\n", mode_name.c_str(), readers);
        std::abort();
    }

    const auto hot_set = MakeHotSet(readers);
    InitializeHotSet(hot_set);

    std::mt19937_64 rng(0xC0FFEEULL + static_cast<std::uint64_t>(readers * 17) + static_cast<std::uint64_t>(mode_name[0]));
    std::vector<std::size_t> indices(kHotSetBlocks);
    std::iota(indices.begin(), indices.end(), 0);
    std::array<std::uint8_t, kWriteSize> payload{};
    std::array<std::uint8_t, kWriteSize> read_buf{};
    std::vector<double> epoch_us;
    epoch_us.reserve(kMeasuredEpochs);

    StatsSnapshot start_measure{};
    StatsSnapshot end_measure{};

    for (int epoch = 0; epoch < kTotalEpochs; ++epoch) {
        if (epoch == kWarmupEpochs) {
            start_measure = leomem::lm_get_stats();
        }

        const auto epoch_begin = std::chrono::steady_clock::now();
        std::shuffle(indices.begin(), indices.end(), rng);

        for (std::size_t i = 0; i < kWriterUpdatesPerEpoch; ++i) {
            const auto& block = hot_set[indices[i]];
            for (std::size_t chunk_idx = 0; chunk_idx < kWritesPerBlock; ++chunk_idx) {
                for (std::size_t b = 0; b < payload.size(); ++b) {
                    payload[b] = static_cast<std::uint8_t>((epoch + i + chunk_idx + b) & 0xFFu);
                }
                GlobalAddr chunk = block;
                chunk.offset += chunk_idx * kWriteSize;
                if (leomem::lm_write(chunk, payload.data(), payload.size()) != Status::kOk) {
                    std::fprintf(stderr, "writer update failed in mode=%s readers=%d epoch=%d\n",
                                 mode_name.c_str(), readers, epoch);
                    std::abort();
                }
            }
        }

        if (leomem::lm_fence() != Status::kOk) {
            std::fprintf(stderr, "writer barrier failed in mode=%s readers=%d epoch=%d\n",
                         mode_name.c_str(), readers, epoch);
            std::abort();
        }

        for (int reader = 0; reader < readers; ++reader) {
            for (std::size_t read_idx = 0; read_idx < kReadsPerReaderPerEpoch; ++read_idx) {
                const auto& block = hot_set[static_cast<std::size_t>(rng() % hot_set.size())];
                const std::size_t chunk_idx = static_cast<std::size_t>(rng() % kWritesPerBlock);
                GlobalAddr chunk = block;
                chunk.offset += chunk_idx * kWriteSize;
                if (leomem::lm_read(chunk, read_buf.data(), read_buf.size()) != Status::kOk) {
                    std::fprintf(stderr, "reader access failed in mode=%s readers=%d epoch=%d\n",
                                 mode_name.c_str(), readers, epoch);
                    std::abort();
                }
            }
        }

        const auto epoch_end = std::chrono::steady_clock::now();
        if (epoch >= kWarmupEpochs) {
            epoch_us.push_back(
                std::chrono::duration<double, std::micro>(epoch_end - epoch_begin).count());
        }
    }

    end_measure = leomem::lm_get_stats();
    const StatsSnapshot delta = DiffStats(end_measure, start_measure);
    const double total_epoch_us = std::accumulate(epoch_us.begin(), epoch_us.end(), 0.0);
    const double mean_epoch_us = epoch_us.empty() ? 0.0 : total_epoch_us / static_cast<double>(epoch_us.size());

    if (ctx.Shutdown() != Status::kOk) {
        std::fprintf(stderr, "runtime shutdown failed for mode=%s readers=%d\n", mode_name.c_str(), readers);
        std::abort();
    }

    return RunSummary{mode_name, readers, mean_epoch_us, delta};
}

void PrintSummary(const RunSummary& summary) {
    const auto& st = summary.stats;
    std::printf("paper adaptive coherence benchmark\n");
    std::printf("mode=%s readers=%d epochs=%d measured_epochs=%d mean_epoch_us=%.2f\n",
                summary.mode.c_str(),
                summary.readers,
                kTotalEpochs,
                kMeasuredEpochs,
                summary.mean_epoch_us);
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
    std::printf("csv: benchmark=adaptive_coherence_paper,mode=%s,readers=%d,epochs=%d,measured_epochs=%d,mean_epoch_us=%.2f,reads=%llu,writes=%llu,cache_hits=%llu,cache_misses=%llu,admissions=%llu,rejections=%llu,cache_evictions=%llu,cache_resident_entries=%llu,cache_resident_bytes=%llu,switches=%llu,wi=%llu,si=%llu,adaptive=%llu,precise_inv=%llu,range_inv=%llu,owner_biased=%llu,handoffs=%llu\n",
                summary.mode.c_str(),
                summary.readers,
                kTotalEpochs,
                kMeasuredEpochs,
                summary.mean_epoch_us,
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
                static_cast<unsigned long long>(st.owner_handoffs));
}

}  // namespace

int main(int argc, char** argv) {
    Config base_cfg;
    if (argc > 1) {
        const Status load = leomem::LoadConfigFile(argv[1], &base_cfg);
        if (load != Status::kOk) {
            std::fprintf(stderr, "config load failed: %s\n", leomem::StatusToString(load));
            return 1;
        }
    }

    std::optional<std::string> mode_filter;
    std::optional<int> reader_filter;
    if (argc > 2) {
        mode_filter = argv[2];
    }
    if (argc > 3) {
        reader_filter = std::atoi(argv[3]);
        if (*reader_filter < 1 || *reader_filter > 7) {
            std::fprintf(stderr, "reader_count must be in [1,7]\n");
            return 1;
        }
    }

    const std::array<std::string, 3> modes = {"wi", "si", "adaptive"};
    for (const auto& mode : modes) {
        if (mode_filter.has_value() && *mode_filter != mode) {
            continue;
        }
        for (int readers = 1; readers <= 7; ++readers) {
            if (reader_filter.has_value() && *reader_filter != readers) {
                continue;
            }
            const RunSummary summary = RunMode(base_cfg, mode, readers);
            PrintSummary(summary);
            std::fflush(stdout);
        }
    }
    return 0;
}
