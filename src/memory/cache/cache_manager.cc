#include "memory/cache/cache_manager.h"

#include <algorithm>
#include <cstring>

#include "metadata/block_meta.h"

namespace leomem {

namespace {

std::uint64_t SafeWindowSize(std::size_t value) {
    return static_cast<std::uint64_t>(std::max<std::size_t>(1, value));
}

bool RangesOverlap(const GlobalAddr& lhs_addr, std::size_t lhs_size,
                   const GlobalAddr& rhs_addr, std::size_t rhs_size) {
    if (lhs_addr.home_node != rhs_addr.home_node) return false;
    if (lhs_addr.region_id != rhs_addr.region_id) return false;

    const std::uint64_t lhs_begin = lhs_addr.offset;
    const std::uint64_t lhs_end = lhs_addr.offset + lhs_size;
    const std::uint64_t rhs_begin = rhs_addr.offset;
    const std::uint64_t rhs_end = rhs_addr.offset + rhs_size;
    return !(lhs_end <= rhs_begin || rhs_end <= lhs_begin);
}

}  // namespace

Status CacheManager::Init() {
    std::lock_guard<std::mutex> lk(mu_);
    access_epoch_ = 0;
    cached_reads_.clear();
    write_queues_.clear();
    return Status::kOk;
}

Status CacheManager::Shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    cached_reads_.clear();
    write_queues_.clear();
    return Status::kOk;
}

CoherenceMode CacheManager::ObserveAccess(const std::shared_ptr<BlockMeta>& meta, AccessType type, bool is_local) {
    if (meta == nullptr) return CoherenceMode::kWI;

    std::lock_guard<std::mutex> lk(mu_);
    const std::uint64_t epoch = ++access_epoch_;
    const std::uint64_t last_epoch = meta->last_access_epoch.exchange(epoch, std::memory_order_relaxed);
    const std::uint64_t window_size = SafeWindowSize(cfg_.profiling_window_size);
    const std::uint64_t window_id = (epoch - 1) / window_size;
    const std::uint64_t previous_window = meta->window_id.load(std::memory_order_relaxed);

    if (previous_window != window_id) {
        meta->window_id.store(window_id, std::memory_order_relaxed);
        meta->window_reads.store(0, std::memory_order_relaxed);
        meta->window_writes.store(0, std::memory_order_relaxed);
        meta->phase_change_count.store(0, std::memory_order_relaxed);
    }

    meta->access_epoch.store(epoch, std::memory_order_relaxed);
    if (last_epoch != 0) {
        meta->last_reuse_distance.store(epoch - last_epoch, std::memory_order_relaxed);
    }

    const auto last_type = static_cast<AccessType>(meta->last_access_type.load(std::memory_order_relaxed));
    if (last_epoch != 0 && last_type != type) {
        meta->phase_change_count.fetch_add(1, std::memory_order_relaxed);
    }
    meta->last_access_type.store(static_cast<std::uint32_t>(type), std::memory_order_relaxed);

    if (type == AccessType::kRead) {
        meta->read_count.fetch_add(1, std::memory_order_relaxed);
        meta->window_reads.fetch_add(1, std::memory_order_relaxed);
        if (!is_local) {
            meta->sharer_count.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        meta->write_count.fetch_add(1, std::memory_order_relaxed);
        meta->window_writes.fetch_add(1, std::memory_order_relaxed);
        if (!is_local) {
            meta->invalidation_cost.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const CoherenceMode next_mode = DecideCoherenceMode(*meta);
    const CoherenceMode previous_mode = meta->mode.exchange(next_mode, std::memory_order_relaxed);
    if (previous_mode != next_mode) {
        meta->coherence_switch_count.fetch_add(1, std::memory_order_relaxed);
    }
    return next_mode;
}

bool CacheManager::ShouldCache(const std::shared_ptr<BlockMeta>& meta) const {
    if (meta == nullptr || !cfg_.enable_cache) return false;
    if (cfg_.cache_admission_policy == 1) return true;
    if (cfg_.cache_admission_policy == 2) return false;

    const double window_reads = static_cast<double>(meta->window_reads.load(std::memory_order_relaxed));
    const double window_writes = static_cast<double>(meta->window_writes.load(std::memory_order_relaxed));
    const double window_total = window_reads + window_writes;
    if (window_total == 0.0) return false;

    const auto reads = meta->read_count.load(std::memory_order_relaxed);
    const auto writes = meta->write_count.load(std::memory_order_relaxed);
    const auto sharers = meta->sharer_count.load(std::memory_order_relaxed);
    const auto reuse_distance = meta->last_reuse_distance.load(std::memory_order_relaxed);
    const auto phase_changes = meta->phase_change_count.load(std::memory_order_relaxed);
    const auto inval = meta->invalidation_cost.load(std::memory_order_relaxed);
    const auto mode = meta->mode.load(std::memory_order_relaxed);

    const double read_ratio = window_reads / window_total;
    const double phase_change_ratio = static_cast<double>(phase_changes) / window_total;

    return reads >= cfg_.cache_admission_min_reads &&
           writes <= cfg_.cache_admission_max_writes &&
           sharers <= cfg_.cache_admission_max_sharers &&
           reuse_distance <= cfg_.cache_admission_max_reuse_distance &&
           read_ratio >= cfg_.cache_admission_min_read_ratio &&
           phase_change_ratio <= cfg_.cache_admission_max_phase_change_ratio &&
           mode != CoherenceMode::kWI &&
           inval <= writes + 1;
}

bool CacheManager::TryReadCached(GlobalAddr addr, void* buf, std::size_t size) {
    if (buf == nullptr || size == 0) return false;

    std::lock_guard<std::mutex> lk(mu_);
    CacheKey key{addr, size};
    auto it = cached_reads_.find(key);
    if (it != cached_reads_.end()) {
        std::memcpy(buf, it->second.payload.data(), size);
        return true;
    }

    for (const auto& [cached_key, cached_value] : cached_reads_) {
        if (cached_key.addr.home_node != addr.home_node) continue;
        if (cached_key.addr.region_id != addr.region_id) continue;
        if (cached_key.addr.offset > addr.offset) continue;
        const std::uint64_t cached_end = cached_key.addr.offset + cached_key.size;
        const std::uint64_t requested_end = addr.offset + size;
        if (cached_end < requested_end) continue;

        const std::size_t begin = static_cast<std::size_t>(addr.offset - cached_key.addr.offset);
        std::memcpy(buf, cached_value.payload.data() + begin, size);
        return true;
    }

    return false;
}

void CacheManager::InsertCached(GlobalAddr addr, const void* buf, std::size_t size) {
    if (buf == nullptr || size == 0 || !cfg_.enable_cache) return;

    std::lock_guard<std::mutex> lk(mu_);
    CacheKey key{addr, size};
    auto& value = cached_reads_[key];
    value.payload.resize(size);
    std::memcpy(value.payload.data(), buf, size);
}

std::size_t CacheManager::Invalidate(GlobalAddr addr, std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    return cached_reads_.erase(CacheKey{addr, size});
}

std::size_t CacheManager::InvalidateRange(GlobalAddr addr, std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t removed = 0;
    for (auto it = cached_reads_.begin(); it != cached_reads_.end();) {
        if (RangesOverlap(it->first.addr, it->first.size, addr, size)) {
            it = cached_reads_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t CacheManager::EffectiveBatchThreshold(CoherenceMode mode) const {
    switch (mode) {
        case CoherenceMode::kWI:
            return 1;
        case CoherenceMode::kAdaptive:
            return std::max<std::size_t>(1, cfg_.adaptive_mode_batch_threshold);
        case CoherenceMode::kSI:
            return std::max<std::size_t>(1, cfg_.remote_write_batch_threshold);
    }
    return 1;
}

bool CacheManager::QueueRemoteWrite(GlobalAddr addr,
                                    const void* buf,
                                    std::size_t size,
                                    CoherenceMode mode,
                                    bool owner_biased,
                                    bool owner_handoff) {
    if (buf == nullptr || size == 0 || !cfg_.enable_write_batching) return false;

    PendingRemoteWrite incoming;
    incoming.addr = addr;
    incoming.payload.resize(size);
    incoming.mode = mode;
    incoming.owner_biased = owner_biased;
    incoming.owner_handoff = owner_handoff;
    std::memcpy(incoming.payload.data(), buf, size);

    std::lock_guard<std::mutex> lk(mu_);
    auto& queue = write_queues_[addr.home_node];
    for (auto& queued : queue) {
        if (CanMergeWrites(queued, incoming)) {
            MergeWrites(&queued, incoming);
            queued.owner_biased = queued.owner_biased || incoming.owner_biased;
            queued.owner_handoff = queued.owner_handoff || incoming.owner_handoff;
            if (static_cast<int>(incoming.mode) < static_cast<int>(queued.mode)) {
                queued.mode = incoming.mode;
            }
            return true;
        }
    }

    queue.push_back(std::move(incoming));
    return false;
}

bool CacheManager::ShouldFlushDestination(NodeId node, std::size_t threshold) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = write_queues_.find(node);
    if (it == write_queues_.end()) return false;
    return it->second.size() >= std::max<std::size_t>(1, threshold);
}

DrainedRemoteWriteBatch CacheManager::DrainRemoteWrites(NodeId node) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = write_queues_.find(node);
    if (it == write_queues_.end()) return {};

    DrainedRemoteWriteBatch batch;
    batch.writes = std::move(it->second);
    for (const auto& write : batch.writes) {
        batch.total_bytes += write.payload.size();
    }
    write_queues_.erase(it);
    return batch;
}

std::vector<NodeId> CacheManager::PendingDestinations() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<NodeId> nodes;
    nodes.reserve(write_queues_.size());
    for (const auto& [node, queued] : write_queues_) {
        if (!queued.empty()) nodes.push_back(node);
    }
    return nodes;
}

std::size_t CacheManager::CacheKeyHash::operator()(const CacheKey& key) const noexcept {
    std::size_t seed = static_cast<std::size_t>(key.addr.home_node);
    seed ^= static_cast<std::size_t>(key.addr.region_id) << 8;
    seed ^= static_cast<std::size_t>(key.addr.offset) << 1;
    seed ^= key.size << 3;
    return seed;
}

CoherenceMode CacheManager::DecideCoherenceMode(const BlockMeta& meta) const {
    if (cfg_.coherence_mode_override >= 0 &&
        cfg_.coherence_mode_override <= static_cast<std::int32_t>(CoherenceMode::kAdaptive)) {
        return static_cast<CoherenceMode>(cfg_.coherence_mode_override);
    }

    const double window_reads = static_cast<double>(meta.window_reads.load(std::memory_order_relaxed));
    const double window_writes = static_cast<double>(meta.window_writes.load(std::memory_order_relaxed));
    const double total = window_reads + window_writes;
    if (total == 0.0) return CoherenceMode::kWI;

    const double write_ratio = window_writes / total;
    const double phase_change_ratio =
        static_cast<double>(meta.phase_change_count.load(std::memory_order_relaxed)) / total;
    const auto sharers = meta.sharer_count.load(std::memory_order_relaxed);
    const auto inval = meta.invalidation_cost.load(std::memory_order_relaxed);

    if (write_ratio >= cfg_.coherence_write_dominant_ratio ||
        phase_change_ratio >= cfg_.coherence_phase_change_promote_ratio) {
        return CoherenceMode::kWI;
    }

    if (sharers >= cfg_.coherence_sharer_promote_threshold ||
        inval >= cfg_.coherence_invalidation_promote_threshold) {
        return CoherenceMode::kAdaptive;
    }

    return CoherenceMode::kSI;
}

bool CacheManager::CanMergeWrites(const PendingRemoteWrite& current, const PendingRemoteWrite& incoming) {
    if (current.addr.home_node != incoming.addr.home_node) return false;
    if (current.addr.region_id != incoming.addr.region_id) return false;

    const std::uint64_t current_begin = current.addr.offset;
    const std::uint64_t current_end = current.addr.offset + current.payload.size();
    const std::uint64_t incoming_begin = incoming.addr.offset;
    const std::uint64_t incoming_end = incoming.addr.offset + incoming.payload.size();

    return !(incoming_end < current_begin || current_end < incoming_begin) ||
           current_end == incoming_begin || incoming_end == current_begin;
}

void CacheManager::MergeWrites(PendingRemoteWrite* current, const PendingRemoteWrite& incoming) {
    if (current == nullptr) return;

    const std::uint64_t merged_begin = std::min(current->addr.offset, incoming.addr.offset);
    const std::uint64_t merged_end =
        std::max(current->addr.offset + current->payload.size(), incoming.addr.offset + incoming.payload.size());

    std::vector<std::uint8_t> merged_payload(static_cast<std::size_t>(merged_end - merged_begin), 0);
    std::memcpy(merged_payload.data() + (current->addr.offset - merged_begin),
                current->payload.data(),
                current->payload.size());
    std::memcpy(merged_payload.data() + (incoming.addr.offset - merged_begin),
                incoming.payload.data(),
                incoming.payload.size());

    current->addr.offset = merged_begin;
    current->payload = std::move(merged_payload);
}

}  // namespace leomem
