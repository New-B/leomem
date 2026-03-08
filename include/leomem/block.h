#pragma once

#include <cstdint>
#include "leomem/types.h"

namespace leomem {

enum class AccessType {
    kRead = 0,
    kWrite = 1,
};

enum class BlockState {
    kInvalid = 0,
    kShared,
    kOwner,
    kHome,
    kTransient,
};

enum class CoherenceMode {
    kWI = 0,
    kSI,
    kAdaptive,
};

struct BlockId {
    RegionId region_id = kDefaultRegionId;
    BlockIndex block_index = 0;
};

inline bool operator==(const BlockId& a, const BlockId& b) {
    return a.region_id == b.region_id && a.block_index == b.block_index;
}

}  // namespace leomem