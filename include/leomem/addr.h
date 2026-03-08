#pragma once

#include <cstdint>
#include "leomem/types.h"

namespace leomem {

struct GlobalAddr {
    NodeId home_node = kInvalidNodeId;
    RegionId region_id = kDefaultRegionId;
    std::uint64_t offset = 0;

    bool IsValid() const { return home_node != kInvalidNodeId; }
};

inline bool operator==(const GlobalAddr& a, const GlobalAddr& b) {
    return a.home_node == b.home_node &&
           a.region_id == b.region_id &&
           a.offset == b.offset;
}

inline bool operator!=(const GlobalAddr& a, const GlobalAddr& b) {
    return !(a == b);
}

inline GlobalAddr InvalidGlobalAddr() {
    return GlobalAddr{};
}

}  // namespace leomem