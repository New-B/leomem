#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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

constexpr std::size_t kMaxDims = 8;
constexpr std::size_t kCentroidStride = 128;
constexpr std::size_t kPartialStride = 256;

struct KMeansSpec {
    std::size_t points = 8192;
    std::size_t clusters = 8;
    std::size_t dims = 4;
    std::size_t iterations = 5;
};

struct PartialRecord {
    std::uint64_t count = 0;
    double dims[kMaxDims]{};
};

KMeansSpec ParseSpec(int argc, char** argv) {
    KMeansSpec spec;
    if (argc > 2) spec.points = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    if (argc > 3) spec.clusters = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
    if (argc > 4) spec.dims = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
    if (argc > 5) spec.iterations = static_cast<std::size_t>(std::strtoull(argv[5], nullptr, 10));
    spec.dims = std::max<std::size_t>(1, std::min<std::size_t>(spec.dims, kMaxDims));
    spec.clusters = std::max<std::size_t>(1, spec.clusters);
    spec.points = std::max<std::size_t>(spec.points, spec.clusters);
    spec.iterations = std::max<std::size_t>(1, spec.iterations);
    return spec;
}

std::vector<std::array<double, kMaxDims>> GeneratePoints(const KMeansSpec& spec) {
    std::vector<std::array<double, kMaxDims>> points(spec.points);
    std::mt19937_64 rng(0x4B4D45415555ULL);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (std::size_t i = 0; i < spec.points; ++i) {
        const std::size_t seed_cluster = i % spec.clusters;
        for (std::size_t d = 0; d < spec.dims; ++d) {
            const double center = static_cast<double>((seed_cluster * 13 + d * 7) % 17) / 17.0;
            points[i][d] = center + uniform(rng) * 0.05;
        }
    }
    return points;
}

std::vector<GlobalAddr> BuildCentroidAddrs(const Config& cfg, const KMeansSpec& spec) {
    std::vector<GlobalAddr> addrs;
    addrs.reserve(spec.clusters);
    const std::uint16_t total_nodes = std::max<std::uint16_t>(cfg.nr_nodes, 2);
    const std::uint16_t remote_nodes = std::max<std::uint16_t>(1, total_nodes - 1);
    for (std::size_t c = 0; c < spec.clusters; ++c) {
        addrs.push_back(GlobalAddr{
            static_cast<NodeId>(1 + (c % remote_nodes)),
            0,
            static_cast<std::uint64_t>(262144 + c * kCentroidStride),
        });
    }
    return addrs;
}

std::vector<GlobalAddr> BuildPartialAddrs(const Config& cfg, const KMeansSpec& spec) {
    std::vector<GlobalAddr> addrs;
    const std::uint16_t total_nodes = std::max<std::uint16_t>(cfg.nr_nodes, 2);
    const std::uint16_t remote_nodes = std::max<std::uint16_t>(1, total_nodes - 1);
    addrs.reserve(spec.clusters * total_nodes);
    for (std::size_t worker = 0; worker < total_nodes; ++worker) {
        for (std::size_t cluster = 0; cluster < spec.clusters; ++cluster) {
            addrs.push_back(GlobalAddr{
                static_cast<NodeId>(1 + ((worker + cluster) % remote_nodes)),
                0,
                static_cast<std::uint64_t>(524288 + (worker * spec.clusters + cluster) * kPartialStride),
            });
        }
    }
    return addrs;
}

std::size_t PartialIndex(const KMeansSpec& spec, std::size_t worker, std::size_t cluster) {
    return worker * spec.clusters + cluster;
}

double DistanceSquared(const std::array<double, kMaxDims>& point,
                       const std::array<double, kMaxDims>& centroid,
                       std::size_t dims) {
    double sum = 0.0;
    for (std::size_t d = 0; d < dims; ++d) {
        const double diff = point[d] - centroid[d];
        sum += diff * diff;
    }
    return sum;
}

int RunKMeans(const Config& cfg, const KMeansSpec& spec) {
    Config run_cfg = cfg;
    run_cfg.control_ack_timeout_polls =
        std::max<std::size_t>(run_cfg.control_ack_timeout_polls, spec.points * spec.iterations + 4096);
    run_cfg.control_ack_max_retries = std::max<std::size_t>(run_cfg.control_ack_max_retries, 4);

    RuntimeContext& ctx = RuntimeContext::Instance();
    if (ctx.Init(run_cfg) != Status::kOk) {
        std::fprintf(stderr, "runtime init failed\n");
        return 1;
    }

    const auto points = GeneratePoints(spec);
    const auto centroid_addrs = BuildCentroidAddrs(run_cfg, spec);
    const auto partial_addrs = BuildPartialAddrs(run_cfg, spec);
    const std::size_t total_workers = std::max<std::uint16_t>(run_cfg.nr_nodes, 2);

    std::vector<std::array<double, kMaxDims>> centroids(spec.clusters);
    for (std::size_t c = 0; c < spec.clusters; ++c) {
        centroids[c] = points[c];
        if (leomem::lm_write(centroid_addrs[c], centroids[c].data(), spec.dims * sizeof(double)) != Status::kOk) {
            std::fprintf(stderr, "centroid init failed at cluster=%zu\n", c);
            return 1;
        }
    }

    PartialRecord zero_partial{};
    for (const auto& addr : partial_addrs) {
        if (leomem::lm_write(addr, &zero_partial, sizeof(zero_partial)) != Status::kOk) {
            std::fprintf(stderr, "partial init failed at node=%u\n", static_cast<unsigned>(addr.home_node));
            return 1;
        }
    }
    if (leomem::lm_fence() != Status::kOk) {
        std::fprintf(stderr, "initial fence failed\n");
        return 1;
    }

    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t iter = 0; iter < spec.iterations; ++iter) {
        for (std::size_t c = 0; c < spec.clusters; ++c) {
            if (leomem::lm_read(centroid_addrs[c], centroids[c].data(), spec.dims * sizeof(double)) != Status::kOk) {
                std::fprintf(stderr, "centroid read failed at iter=%zu cluster=%zu\n", iter, c);
                return 1;
            }
        }

        for (std::size_t p = 0; p < spec.points; ++p) {
            const std::size_t worker = p % total_workers;
            std::size_t best_cluster = 0;
            double best_distance = std::numeric_limits<double>::max();
            for (std::size_t c = 0; c < spec.clusters; ++c) {
                const double dist = DistanceSquared(points[p], centroids[c], spec.dims);
                if (dist < best_distance) {
                    best_distance = dist;
                    best_cluster = c;
                }
            }

            const std::size_t idx = PartialIndex(spec, worker, best_cluster);
            PartialRecord partial{};
            if (leomem::lm_read(partial_addrs[idx], &partial, sizeof(partial)) != Status::kOk) {
                std::fprintf(stderr, "partial read failed at iter=%zu point=%zu\n", iter, p);
                return 1;
            }
            partial.count += 1;
            for (std::size_t d = 0; d < spec.dims; ++d) {
                partial.dims[d] += points[p][d];
            }
            if (leomem::lm_write(partial_addrs[idx], &partial, sizeof(partial)) != Status::kOk) {
                std::fprintf(stderr, "partial write failed at iter=%zu point=%zu\n", iter, p);
                return 1;
            }
        }

        if (leomem::lm_fence() != Status::kOk) {
            std::fprintf(stderr, "assignment fence failed at iter=%zu\n", iter);
            return 1;
        }

        for (std::size_t cluster = 0; cluster < spec.clusters; ++cluster) {
            PartialRecord aggregate{};
            for (std::size_t worker = 0; worker < total_workers; ++worker) {
                const std::size_t idx = PartialIndex(spec, worker, cluster);
                PartialRecord partial{};
                if (leomem::lm_read(partial_addrs[idx], &partial, sizeof(partial)) != Status::kOk) {
                    std::fprintf(stderr, "partial reduce read failed at iter=%zu cluster=%zu worker=%zu\n",
                                 iter, cluster, worker);
                    return 1;
                }
                aggregate.count += partial.count;
                for (std::size_t d = 0; d < spec.dims; ++d) {
                    aggregate.dims[d] += partial.dims[d];
                }
                if (leomem::lm_write(partial_addrs[idx], &zero_partial, sizeof(zero_partial)) != Status::kOk) {
                    std::fprintf(stderr, "partial reset failed at iter=%zu cluster=%zu worker=%zu\n",
                                 iter, cluster, worker);
                    return 1;
                }
            }

            if (aggregate.count != 0) {
                for (std::size_t d = 0; d < spec.dims; ++d) {
                    centroids[cluster][d] = aggregate.dims[d] / static_cast<double>(aggregate.count);
                }
            }
            if (leomem::lm_write(centroid_addrs[cluster], centroids[cluster].data(),
                                 spec.dims * sizeof(double)) != Status::kOk) {
                std::fprintf(stderr, "centroid update failed at iter=%zu cluster=%zu\n", iter, cluster);
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

    std::printf("k-means workload\n");
    std::printf("nodes=%u points=%zu clusters=%zu dims=%zu iterations=%zu runtime_ms=%.3f\n",
                static_cast<unsigned>(run_cfg.nr_nodes),
                spec.points,
                spec.clusters,
                spec.dims,
                spec.iterations,
                runtime_ms);
    std::printf("ops: reads=%llu writes=%llu remote_reads=%llu remote_writes=%llu\n",
                static_cast<unsigned long long>(st.read_ops),
                static_cast<unsigned long long>(st.write_ops),
                static_cast<unsigned long long>(st.remote_reads),
                static_cast<unsigned long long>(st.remote_writes));
    std::printf("cache: hits=%llu misses=%llu admissions=%llu rejections=%llu\n",
                static_cast<unsigned long long>(st.cache_hits),
                static_cast<unsigned long long>(st.cache_misses),
                static_cast<unsigned long long>(st.cache_admissions),
                static_cast<unsigned long long>(st.cache_rejections));
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
    std::printf("csv: benchmark=kmeans,nodes=%u,points=%zu,clusters=%zu,dims=%zu,iterations=%zu,runtime_ms=%.3f,reads=%llu,writes=%llu,cache_hits=%llu,cache_misses=%llu,admissions=%llu,rejections=%llu,switches=%llu,wi=%llu,si=%llu,adaptive=%llu,precise_inv=%llu,range_inv=%llu,owner_biased=%llu,handoffs=%llu,merged=%llu,batches=%llu,bytes=%llu\n",
                static_cast<unsigned>(run_cfg.nr_nodes),
                spec.points,
                spec.clusters,
                spec.dims,
                spec.iterations,
                runtime_ms,
                static_cast<unsigned long long>(st.read_ops),
                static_cast<unsigned long long>(st.write_ops),
                static_cast<unsigned long long>(st.cache_hits),
                static_cast<unsigned long long>(st.cache_misses),
                static_cast<unsigned long long>(st.cache_admissions),
                static_cast<unsigned long long>(st.cache_rejections),
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

    const KMeansSpec spec = ParseSpec(argc, argv);
    return RunKMeans(cfg, spec);
}
