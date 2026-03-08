#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "leomem/block.h"
#include "metadata/block_meta.h"

namespace leomem {

struct BlockIdHash {
    std::size_t operator()(const BlockId& id) const noexcept {
        return (static_cast<std::size_t>(id.region_id) << 48) ^
               static_cast<std::size_t>(id.block_index);
    }
};

class BlockTable {
public:
    std::shared_ptr<BlockMeta> GetOrCreate(const BlockId& bid);
    std::shared_ptr<BlockMeta> Find(const BlockId& bid);

private:
    std::mutex mu_;
    std::unordered_map<BlockId, std::shared_ptr<BlockMeta>, BlockIdHash> table_;
};

}  // namespace leomem