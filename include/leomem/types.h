#pragma once

#include <cstddef>
#include <cstdint>

namespace leomem {

using NodeId = std::uint16_t;
using RegionId = std::uint16_t;
using ThreadId = std::uint32_t;
using BlockIndex = std::uint64_t;
using Version = std::uint64_t;

constexpr NodeId kInvalidNodeId = static_cast<NodeId>(-1);
constexpr RegionId kDefaultRegionId = 0;

}  // namespace leomem