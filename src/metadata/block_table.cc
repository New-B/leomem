#include "metadata/block_table.h"

namespace leomem {

std::shared_ptr<BlockMeta> BlockTable::GetOrCreate(const BlockId& bid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(bid);
    if (it != table_.end()) return it->second;

    auto meta = std::make_shared<BlockMeta>();
    meta->bid = bid;
    table_.emplace(bid, meta);
    return meta;
}

std::shared_ptr<BlockMeta> BlockTable::Find(const BlockId& bid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(bid);
    if (it == table_.end()) return nullptr;
    return it->second;
}

}  // namespace leomem