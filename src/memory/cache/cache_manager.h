#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "leomem/addr.h"
#include "leomem/block.h"
#include "leomem/config.h"
#include "leomem/status.h"

namespace leomem {

struct BlockMeta;

struct PendingRemoteWrite {
    GlobalAddr addr{};
    std::vector<std::uint8_t> payload;
    CoherenceMode mode = CoherenceMode::kSI;
    bool owner_biased = false;
    bool owner_handoff = false;
};

struct DrainedRemoteWriteBatch {
    std::vector<PendingRemoteWrite> writes;
    std::size_t merged_writes = 0;
    std::size_t total_bytes = 0;
};

class CacheManager {
public:
    explicit CacheManager(const Config& cfg) : cfg_(cfg) {}

    Status Init();
    Status Shutdown();

    CoherenceMode ObserveAccess(const std::shared_ptr<BlockMeta>& meta, AccessType type, bool is_local);
    bool ShouldCache(const std::shared_ptr<BlockMeta>& meta) const;
    bool TryReadCached(GlobalAddr addr, void* buf, std::size_t size);
    void InsertCached(GlobalAddr addr, const void* buf, std::size_t size);
    std::size_t Invalidate(GlobalAddr addr, std::size_t size);
    std::size_t InvalidateRange(GlobalAddr addr, std::size_t size);
    std::size_t EffectiveBatchThreshold(CoherenceMode mode) const;

    bool QueueRemoteWrite(GlobalAddr addr,
                          const void* buf,
                          std::size_t size,
                          CoherenceMode mode,
                          bool owner_biased,
                          bool owner_handoff);
    bool ShouldFlushDestination(NodeId node, std::size_t threshold) const;
    DrainedRemoteWriteBatch DrainRemoteWrites(NodeId node);
    std::vector<NodeId> PendingDestinations() const;

private:
    struct CacheKey {
        GlobalAddr addr{};
        std::size_t size = 0;

        bool operator==(const CacheKey& other) const {
            return addr == other.addr && size == other.size;
        }
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& key) const noexcept;
    };

    struct CachedValue {
        std::vector<std::uint8_t> payload;
    };

    CoherenceMode DecideCoherenceMode(const BlockMeta& meta) const;
    static bool CanMergeWrites(const PendingRemoteWrite& current, const PendingRemoteWrite& incoming);
    static void MergeWrites(PendingRemoteWrite* current, const PendingRemoteWrite& incoming);

    Config cfg_;
    mutable std::mutex mu_;
    std::uint64_t access_epoch_ = 0;
    std::unordered_map<CacheKey, CachedValue, CacheKeyHash> cached_reads_;
    std::unordered_map<NodeId, std::vector<PendingRemoteWrite>> write_queues_;
};

}  // namespace leomem
