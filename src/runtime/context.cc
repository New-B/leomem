#include "runtime/context.h"

#include "common/log.h"
#include "memory/cache/cache_manager.h"
#include "memory/global/dsm_allocator.h"
#include "metadata/block_table.h"
#include "stats/counters.h"
#include "transport/transport.h"

namespace leomem {

RuntimeContext& RuntimeContext::Instance() {
    static RuntimeContext ctx;
    return ctx;
}

Status RuntimeContext::Init(const Config& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (initialized_) return Status::kAlreadyInitialized;

    config_ = cfg;

    stats_ = std::make_unique<StatsCollector>();
    allocator_ = std::make_unique<DsmAllocator>(config_);
    cache_manager_ = std::make_unique<CacheManager>(config_);
    block_table_ = std::make_unique<BlockTable>();
    transport_ = CreateTransport(config_);

    Status s = allocator_->Init();
    if (s != Status::kOk) return s;

    s = cache_manager_->Init();
    if (s != Status::kOk) return s;

    s = transport_->Init();
    if (s != Status::kOk) return s;

    initialized_ = true;
    LM_LOG_INFO("LeoMem runtime initialized: node_id=%u nr_nodes=%u block_size=%zu",
                config_.node_id, config_.nr_nodes, config_.block_size);
    return Status::kOk;
}

Status RuntimeContext::Shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!initialized_) return Status::kNotInitialized;

    if (transport_) transport_->Shutdown();
    if (cache_manager_) cache_manager_->Shutdown();
    if (allocator_) allocator_->Shutdown();

    transport_.reset();
    cache_manager_.reset();
    allocator_.reset();
    block_table_.reset();
    stats_.reset();

    initialized_ = false;
    return Status::kOk;
}

bool RuntimeContext::IsInitialized() const {
    return initialized_;
}

const Config& RuntimeContext::GetConfig() const {
    return config_;
}

Transport* RuntimeContext::transport() { return transport_.get(); }
DsmAllocator* RuntimeContext::allocator() { return allocator_.get(); }
CacheManager* RuntimeContext::cache_manager() { return cache_manager_.get(); }
BlockTable* RuntimeContext::block_table() { return block_table_.get(); }
StatsCollector* RuntimeContext::stats() { return stats_.get(); }

}  // namespace leomem