#pragma once

#include <memory>
#include <mutex>

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
};

}  // namespace leomem