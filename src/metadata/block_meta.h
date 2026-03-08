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
};

}  // namespace leomem