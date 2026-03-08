#include "memory/global/dsm_allocator.h"

#include "common/utils.h"

namespace leomem {

DsmAllocator::DsmAllocator(const Config& cfg) : cfg_(cfg) {}

Status DsmAllocator::Init() {
    region_.resize(cfg_.local_region_size, 0);
    next_offset_ = 0;
    return Status::kOk;
}

Status DsmAllocator::Shutdown() {
    region_.clear();
    next_offset_ = 0;
    return Status::kOk;
}

GlobalAddr DsmAllocator::Allocate(std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);

    std::size_t aligned_size = AlignUp(size, cfg_.block_size);
    if (next_offset_ + aligned_size > region_.size()) {
        return InvalidGlobalAddr();
    }

    GlobalAddr addr;
    addr.home_node = cfg_.node_id;
    addr.region_id = kDefaultRegionId;
    addr.offset = next_offset_;

    next_offset_ += aligned_size;
    return addr;
}

Status DsmAllocator::Free(GlobalAddr addr) {
    // Phase 1: no-op bump allocator
    if (!addr.IsValid()) return Status::kInvalidArg;
    return Status::kOk;
}

void* DsmAllocator::TranslateLocal(GlobalAddr addr, std::size_t size) {
    if (!addr.IsValid()) return nullptr;
    if (addr.home_node != cfg_.node_id) return nullptr;
    if (addr.offset + size > region_.size()) return nullptr;
    return region_.data() + addr.offset;
}

}  // namespace leomem