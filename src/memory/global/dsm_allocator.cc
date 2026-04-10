#include "memory/global/dsm_allocator.h"

#include "common/utils.h"

namespace leomem {

DsmAllocator::DsmAllocator(const Config& cfg) : cfg_(cfg) {}

Status DsmAllocator::Init() {
    std::lock_guard<std::mutex> lk(mu_);
    region_.resize(cfg_.local_region_size, 0);
    next_offset_ = 0;
    allocations_.clear();
    return Status::kOk;
}

Status DsmAllocator::Shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    region_.clear();
    next_offset_ = 0;
    allocations_.clear();
    return Status::kOk;
}

GlobalAddr DsmAllocator::Allocate(std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);

    if (size == 0 || cfg_.block_size == 0) {
        return InvalidGlobalAddr();
    }

    std::size_t aligned_size = AlignUp(size, cfg_.block_size);
    if (next_offset_ + aligned_size > region_.size()) {
        return InvalidGlobalAddr();
    }

    GlobalAddr addr;
    addr.home_node = cfg_.node_id;
    addr.region_id = kDefaultRegionId;
    addr.offset = next_offset_;

    allocations_.emplace(addr.offset, AllocationMeta{addr.offset, aligned_size});
    next_offset_ += aligned_size;
    return addr;
}

Status DsmAllocator::Free(GlobalAddr addr) {
    if (!addr.IsValid()) return Status::kInvalidArg;
    std::lock_guard<std::mutex> lk(mu_);
    if (addr.home_node != cfg_.node_id) return Status::kInvalidArg;
    if (allocations_.find(addr.offset) == allocations_.end()) return Status::kNotFound;
    return Status::kOk;
}

void* DsmAllocator::TranslateLocal(GlobalAddr addr, std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ValidateRangeUnlocked(addr, size)) return nullptr;
    return region_.data() + addr.offset;
}

void* DsmAllocator::LocalBase() {
    std::lock_guard<std::mutex> lk(mu_);
    return region_.empty() ? nullptr : region_.data();
}

std::size_t DsmAllocator::LocalRegionSize() const {
    std::lock_guard<std::mutex> lk(mu_);
    return region_.size();
}

std::optional<BlockRange> DsmAllocator::ResolveBlockRange(GlobalAddr addr, std::size_t size) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ValidateRangeUnlocked(addr, size)) return std::nullopt;

    const std::uint64_t first_index = addr.offset / cfg_.block_size;
    const std::uint64_t last_index = (addr.offset + size - 1) / cfg_.block_size;

    BlockRange range;
    range.first.region_id = addr.region_id;
    range.first.block_index = first_index;
    range.last.region_id = addr.region_id;
    range.last.block_index = last_index;
    range.count = static_cast<std::size_t>(last_index - first_index + 1);
    return range;
}

bool DsmAllocator::ValidateRangeUnlocked(GlobalAddr addr, std::size_t size) const {
    if (!addr.IsValid() || size == 0) return false;
    if (addr.home_node != cfg_.node_id) return false;
    if (cfg_.block_size == 0) return false;

    auto it = allocations_.upper_bound(addr.offset);
    if (it == allocations_.begin()) return false;
    --it;

    const AllocationMeta& alloc = it->second;
    if (addr.offset < alloc.offset) return false;

    const std::size_t relative = static_cast<std::size_t>(addr.offset - alloc.offset);
    if (relative + size > alloc.size) return false;
    if (addr.offset + size > region_.size()) return false;
    return true;
}

}  // namespace leomem
