#pragma once

#include <memory>
#include <mutex>
#include <unordered_set>

#include "leomem/config.h"
#include "leomem/stats.h"
#include "leomem/status.h"

namespace leomem {

class Transport;
class DsmAllocator;
class CacheManager;
class BlockTable;
class StatsCollector;

class RuntimeContext {
public:
    static RuntimeContext& Instance();

    Status Init(const Config& cfg);
    Status Shutdown();

    bool IsInitialized() const;
    const Config& GetConfig() const;

    Transport* transport();
    DsmAllocator* allocator();
    CacheManager* cache_manager();
    BlockTable* block_table();
    StatsCollector* stats();

    std::uint64_t NextControlRequestId();
    void TrackPendingControlAck(std::uint64_t request_id);
    void CompleteControlAck(std::uint64_t request_id);
    bool HasPendingControlAck(std::uint64_t request_id);
    std::size_t PendingControlAcks() const;

private:
    RuntimeContext() = default;

    mutable std::mutex mu_;
    bool initialized_ = false;
    Config config_{};

    std::unique_ptr<Transport> transport_;
    std::unique_ptr<DsmAllocator> allocator_;
    std::unique_ptr<CacheManager> cache_manager_;
    std::unique_ptr<BlockTable> block_table_;
    std::unique_ptr<StatsCollector> stats_;
    std::uint64_t next_control_request_id_ = 1;
    std::unordered_set<std::uint64_t> pending_control_acks_;
};

}  // namespace leomem
