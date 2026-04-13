#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

constexpr std::size_t kRecordSize = 256;
constexpr std::size_t kUpdateSize = 16;
constexpr double kZipfTheta = 0.99;

struct WorkloadSpec {
    std::string name;
    double read_ratio = 0.5;
};

WorkloadSpec ParseWorkload(const char* arg) {
    if (arg == nullptr) {
        return {"A", 0.50};
    }
    std::string name = arg;
    if (name == "A" || name == "a") return {"A", 0.50};
    if (name == "B" || name == "b") return {"B", 0.95};
    return {name, 0.50};
}

std::size_t ParseCount(const char* arg, std::size_t fallback) {
    if (arg == nullptr) return fallback;
    return static_cast<std::size_t>(std::strtoull(arg, nullptr, 10));
}

std::size_t SampleSkewedIndex(std::mt19937_64& rng, std::size_t record_count) {
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const double u = uniform(rng);
    const double biased = std::pow(u, 1.0 + kZipfTheta);
    const std::size_t idx = static_cast<std::size_t>(biased * static_cast<double>(record_count));
    return std::min(record_count - 1, idx);
}

std::vector<GlobalAddr> BuildRecordArray(const Config& cfg, std::size_t record_count) {
    std::vector<GlobalAddr> records;
    records.reserve(record_count);
    const std::uint16_t total_nodes = std::max<std::uint16_t>(cfg.nr_nodes, 2);
    const std::uint16_t remote_nodes = std::max<std::uint16_t>(1, total_nodes - 1);
    for (std::size_t i = 0; i < record_count; ++i) {
        const NodeId home = static_cast<NodeId>(1 + (i % remote_nodes));
        records.push_back(GlobalAddr{
            home,
            0,
            static_cast<std::uint64_t>(131072 + i * kRecordSize),
        });
    }
    return records;
}

void InitializeRecords(const std::vector<GlobalAddr>& records) {
    std::array<std::uint8_t, kRecordSize> payload{};
    for (std::size_t i = 0; i < records.size(); ++i) {
        for (std::size_t j = 0; j < payload.size(); ++j) {
            payload[j] = static_cast<std::uint8_t>((i + j) & 0xFFu);
        }
        const Status status = leomem::lm_write(records[i], payload.data(), payload.size());
        if (status != Status::kOk) {
            std::fprintf(stderr, "record init failed at index=%zu node=%u status=%s\n",
                         i,
                         static_cast<unsigned>(records[i].home_node),
                         leomem::StatusToString(status));
            std::abort();
        }
    }
    if (leomem::lm_fence() != Status::kOk) {
        std::fprintf(stderr, "record init fence failed\n");
        std::abort();
    }
}

int RunWorkload(const Config& cfg,
                const WorkloadSpec& workload,
                std::size_t total_ops,
                std::size_t record_count) {
    Config run_cfg = cfg;
    run_cfg.control_ack_timeout_polls =
        std::max<std::size_t>(run_cfg.control_ack_timeout_polls, record_count + total_ops + 1024);
    run_cfg.control_ack_max_retries =
        std::max<std::size_t>(run_cfg.control_ack_max_retries, 4);

    RuntimeContext& ctx = RuntimeContext::Instance();
    if (ctx.Init(run_cfg) != Status::kOk) {
        std::fprintf(stderr, "runtime init failed\n");
        return 1;
    }

    const auto records = BuildRecordArray(run_cfg, record_count);
    InitializeRecords(records);

    std::mt19937_64 rng(0xBAD5EEDULL + total_ops + record_count);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::array<std::uint8_t, kRecordSize> read_buf{};
    std::array<std::uint8_t, kUpdateSize> update_buf{};

    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t op = 0; op < total_ops; ++op) {
        const std::size_t idx = SampleSkewedIndex(rng, records.size());
        const auto& record = records[idx];
        if (coin(rng) < workload.read_ratio) {
            if (leomem::lm_read(record, read_buf.data(), read_buf.size()) != Status::kOk) {
                std::fprintf(stderr, "read failed at op=%zu\n", op);
                std::abort();
            }
            continue;
        }

        for (std::size_t b = 0; b < update_buf.size(); ++b) {
            update_buf[b] = static_cast<std::uint8_t>((idx + op + b) & 0xFFu);
        }
        if (leomem::lm_write(record, update_buf.data(), update_buf.size()) != Status::kOk) {
            std::fprintf(stderr, "write failed at op=%zu\n", op);
            std::abort();
        }
    }
    if (leomem::lm_fence() != Status::kOk) {
        std::fprintf(stderr, "final fence failed\n");
        std::abort();
    }
    const auto end = std::chrono::steady_clock::now();

    const StatsSnapshot st = leomem::lm_get_stats();
    const double runtime_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    const double throughput_mops = runtime_ms == 0.0
        ? 0.0
        : static_cast<double>(total_ops) / runtime_ms / 1000.0;

    std::printf("ycsb-like mixed workload\n");
    std::printf("workload=%s nodes=%u total_ops=%zu record_count=%zu runtime_ms=%.3f throughput_mops=%.3f\n",
                workload.name.c_str(),
                static_cast<unsigned>(cfg.nr_nodes),
                total_ops,
                record_count,
                runtime_ms,
                throughput_mops);
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
    std::printf("csv: benchmark=ycsb_mixed,workload=%s,nodes=%u,total_ops=%zu,record_count=%zu,runtime_ms=%.3f,throughput_mops=%.3f,reads=%llu,writes=%llu,cache_hits=%llu,cache_misses=%llu,admissions=%llu,rejections=%llu,cache_evictions=%llu,cache_resident_entries=%llu,cache_resident_bytes=%llu,switches=%llu,wi=%llu,si=%llu,adaptive=%llu,precise_inv=%llu,range_inv=%llu,owner_biased=%llu,handoffs=%llu,merged=%llu,batches=%llu,bytes=%llu\n",
                workload.name.c_str(),
                static_cast<unsigned>(cfg.nr_nodes),
                total_ops,
                record_count,
                runtime_ms,
                throughput_mops,
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

    const WorkloadSpec workload = ParseWorkload(argc > 2 ? argv[2] : nullptr);
    const std::size_t total_ops = ParseCount(argc > 3 ? argv[3] : nullptr, 200000);
    const std::size_t record_count = ParseCount(argc > 4 ? argv[4] : nullptr, 262144);
    return RunWorkload(cfg, workload, total_ops, record_count);
}
