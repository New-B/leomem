#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

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

constexpr double kDamping = 0.85;
constexpr std::size_t kRankStride = 64;
constexpr std::size_t kAccumStride = 64;

struct PageRankSpec {
    std::size_t vertices = 2048;
    std::size_t out_degree = 4;
    std::size_t iterations = 5;
};

PageRankSpec ParseSpec(int argc, char** argv) {
    PageRankSpec spec;
    if (argc > 2) spec.vertices = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    if (argc > 3) spec.out_degree = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
    if (argc > 4) spec.iterations = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
    spec.vertices = std::max<std::size_t>(2, spec.vertices);
    spec.out_degree = std::max<std::size_t>(1, spec.out_degree);
    spec.iterations = std::max<std::size_t>(1, spec.iterations);
    return spec;
}

std::vector<std::vector<std::uint32_t>> GenerateGraph(const PageRankSpec& spec) {
    std::vector<std::vector<std::uint32_t>> edges(spec.vertices);
    std::mt19937_64 rng(0x5041474552414E4BULL);
    std::uniform_int_distribution<std::uint32_t> pick(0, static_cast<std::uint32_t>(spec.vertices - 1));
    for (std::size_t src = 0; src < spec.vertices; ++src) {
        auto& out = edges[src];
        out.reserve(spec.out_degree);
        while (out.size() < spec.out_degree) {
            const std::uint32_t dst = pick(rng);
            if (dst == src) continue;
            out.push_back(dst);
        }
    }
    return edges;
}

std::vector<GlobalAddr> BuildVectorAddrs(const Config& cfg,
                                         std::size_t count,
                                         std::uint64_t base_offset,
                                         std::size_t stride) {
    std::vector<GlobalAddr> addrs;
    addrs.reserve(count);
    const std::uint16_t total_nodes = std::max<std::uint16_t>(cfg.nr_nodes, 2);
    const std::uint16_t remote_nodes = std::max<std::uint16_t>(1, total_nodes - 1);
    for (std::size_t i = 0; i < count; ++i) {
        addrs.push_back(GlobalAddr{
            static_cast<NodeId>(1 + (i % remote_nodes)),
            0,
            base_offset + static_cast<std::uint64_t>(i * stride),
        });
    }
    return addrs;
}

int RunPageRank(const Config& cfg, const PageRankSpec& spec) {
    Config run_cfg = cfg;
    run_cfg.control_ack_timeout_polls =
        std::max<std::size_t>(run_cfg.control_ack_timeout_polls,
                              spec.vertices * spec.out_degree * spec.iterations + 4096);
    run_cfg.control_ack_max_retries = std::max<std::size_t>(run_cfg.control_ack_max_retries, 4);

    RuntimeContext& ctx = RuntimeContext::Instance();
    if (ctx.Init(run_cfg) != Status::kOk) {
        std::fprintf(stderr, "runtime init failed\n");
        return 1;
    }

    const auto graph = GenerateGraph(spec);
    const auto rank_addrs = BuildVectorAddrs(run_cfg, spec.vertices, 786432, kRankStride);
    const auto accum_addrs = BuildVectorAddrs(run_cfg, spec.vertices, 1048576, kAccumStride);

    const double initial_rank = 1.0 / static_cast<double>(spec.vertices);
    const double zero = 0.0;
    for (std::size_t v = 0; v < spec.vertices; ++v) {
        if (leomem::lm_write(rank_addrs[v], &initial_rank, sizeof(initial_rank)) != Status::kOk) {
            std::fprintf(stderr, "rank init failed at vertex=%zu\n", v);
            return 1;
        }
        if (leomem::lm_write(accum_addrs[v], &zero, sizeof(zero)) != Status::kOk) {
            std::fprintf(stderr, "accum init failed at vertex=%zu\n", v);
            return 1;
        }
    }
    if (leomem::lm_fence() != Status::kOk) {
        std::fprintf(stderr, "initial fence failed\n");
        return 1;
    }

    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t iter = 0; iter < spec.iterations; ++iter) {
        for (std::size_t src = 0; src < spec.vertices; ++src) {
            double rank = 0.0;
            if (leomem::lm_read(rank_addrs[src], &rank, sizeof(rank)) != Status::kOk) {
                std::fprintf(stderr, "rank read failed at iter=%zu vertex=%zu\n", iter, src);
                return 1;
            }
            const double contrib = rank / static_cast<double>(graph[src].size());
            for (std::uint32_t dst : graph[src]) {
                double accum = 0.0;
                if (leomem::lm_read(accum_addrs[dst], &accum, sizeof(accum)) != Status::kOk) {
                    std::fprintf(stderr, "accum read failed at iter=%zu dst=%u\n", iter, dst);
                    return 1;
                }
                accum += contrib;
                if (leomem::lm_write(accum_addrs[dst], &accum, sizeof(accum)) != Status::kOk) {
                    std::fprintf(stderr, "accum write failed at iter=%zu dst=%u\n", iter, dst);
                    return 1;
                }
            }
        }

        if (leomem::lm_fence() != Status::kOk) {
            std::fprintf(stderr, "scatter fence failed at iter=%zu\n", iter);
            return 1;
        }

        for (std::size_t v = 0; v < spec.vertices; ++v) {
            double accum = 0.0;
            if (leomem::lm_read(accum_addrs[v], &accum, sizeof(accum)) != Status::kOk) {
                std::fprintf(stderr, "accum readback failed at iter=%zu vertex=%zu\n", iter, v);
                return 1;
            }
            const double rank = ((1.0 - kDamping) / static_cast<double>(spec.vertices)) + kDamping * accum;
            if (leomem::lm_write(rank_addrs[v], &rank, sizeof(rank)) != Status::kOk) {
                std::fprintf(stderr, "rank update failed at iter=%zu vertex=%zu\n", iter, v);
                return 1;
            }
            if (leomem::lm_write(accum_addrs[v], &zero, sizeof(zero)) != Status::kOk) {
                std::fprintf(stderr, "accum reset failed at iter=%zu vertex=%zu\n", iter, v);
                return 1;
            }
        }

        if (leomem::lm_fence() != Status::kOk) {
            std::fprintf(stderr, "update fence failed at iter=%zu\n", iter);
            return 1;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const StatsSnapshot st = leomem::lm_get_stats();
    const double runtime_ms = std::chrono::duration<double, std::milli>(end - begin).count();

    std::printf("pagerank workload\n");
    std::printf("nodes=%u vertices=%zu out_degree=%zu iterations=%zu runtime_ms=%.3f\n",
                static_cast<unsigned>(run_cfg.nr_nodes),
                spec.vertices,
                spec.out_degree,
                spec.iterations,
                runtime_ms);
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
    std::printf("csv: benchmark=pagerank,nodes=%u,vertices=%zu,out_degree=%zu,iterations=%zu,runtime_ms=%.3f,reads=%llu,writes=%llu,cache_hits=%llu,cache_misses=%llu,admissions=%llu,rejections=%llu,cache_evictions=%llu,cache_resident_entries=%llu,cache_resident_bytes=%llu,switches=%llu,wi=%llu,si=%llu,adaptive=%llu,precise_inv=%llu,range_inv=%llu,owner_biased=%llu,handoffs=%llu,merged=%llu,batches=%llu,bytes=%llu\n",
                static_cast<unsigned>(run_cfg.nr_nodes),
                spec.vertices,
                spec.out_degree,
                spec.iterations,
                runtime_ms,
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

    ctx.Shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (argc > 1) {
        const Status load = leomem::LoadConfigFile(argv[1], &cfg);
        if (load != Status::kOk) {
            std::fprintf(stderr, "config load failed: %s\n", leomem::StatusToString(load));
            return 1;
        }
    }

    const PageRankSpec spec = ParseSpec(argc, argv);
    return RunPageRank(cfg, spec);
}
