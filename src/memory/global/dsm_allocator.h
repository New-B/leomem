#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <mutex>
#include <vector>

#include "leomem/addr.h"
#include "leomem/block.h"
#include "leomem/config.h"
#include "leomem/status.h"

namespace leomem {

struct BlockRange {
    BlockId first{};
    BlockId last{};
    std::size_t count = 0;
};

class DsmAllocator {
public:
    explicit DsmAllocator(const Config& cfg);

    Status Init();
    Status Shutdown();

    GlobalAddr Allocate(std::size_t size);
    Status Free(GlobalAddr addr);

    void* TranslateLocal(GlobalAddr addr, std::size_t size);
    void* LocalBase();
    std::size_t LocalRegionSize() const;
    std::optional<BlockRange> ResolveBlockRange(GlobalAddr addr, std::size_t size) const;

private:
    struct AllocationMeta {
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    bool ValidateRangeUnlocked(GlobalAddr addr, std::size_t size) const;

    Config cfg_;
    std::vector<std::uint8_t> region_;
    std::size_t next_offset_ = 0;
    std::map<std::uint64_t, AllocationMeta> allocations_;
    mutable std::mutex mu_;
};

}  // namespace leomem
