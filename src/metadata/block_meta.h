#pragma once

#include <atomic>

#include "leomem/block.h"
#include "leomem/types.h"

namespace leomem {

struct BlockMeta {
    BlockId bid{};
    std::atomic<NodeId> owner_node{kInvalidNodeId};
    std::atomic<Version> version{0};
    std::atomic<BlockState> state{BlockState::kHome};
    std::atomic<CoherenceMode> mode{CoherenceMode::kWI};
    std::atomic<std::uint64_t> access_epoch{0};
    std::atomic<std::uint64_t> last_access_epoch{0};
    std::atomic<std::uint64_t> read_count{0};
    std::atomic<std::uint64_t> write_count{0};
    std::atomic<std::uint64_t> remote_miss_count{0};
    std::atomic<std::uint64_t> invalidation_cost{0};
    std::atomic<std::uint32_t> sharer_count{0};
    std::atomic<std::uint64_t> window_id{0};
    std::atomic<std::uint64_t> window_reads{0};
    std::atomic<std::uint64_t> window_writes{0};
    std::atomic<std::uint64_t> phase_change_count{0};
    std::atomic<std::uint64_t> last_reuse_distance{0};
    std::atomic<std::uint32_t> last_access_type{static_cast<std::uint32_t>(AccessType::kRead)};
    std::atomic<std::uint64_t> coherence_switch_count{0};
    std::atomic<std::uint64_t> owner_epoch{0};
    std::atomic<std::uint64_t> last_validated_version{0};
    std::atomic<bool> cache_admitted{false};
};

}  // namespace leomem
