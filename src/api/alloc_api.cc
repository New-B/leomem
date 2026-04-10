#include "leomem/leomem.h"

#include "metadata/block_table.h"
#include "metadata/block_meta.h"
#include "memory/global/dsm_allocator.h"
#include "runtime/context.h"
#include "runtime/init.h"
#include "stats/counters.h"

namespace leomem {

GlobalAddr lm_malloc(std::size_t size) {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return InvalidGlobalAddr();

    GlobalAddr addr = ctx.allocator()->Allocate(size);
    if (addr.IsValid()) {
        auto range = ctx.allocator()->ResolveBlockRange(addr, size);
        if (!range.has_value()) {
            return InvalidGlobalAddr();
        }

        for (std::uint64_t index = range->first.block_index; index <= range->last.block_index; ++index) {
            BlockId bid{addr.region_id, index};
            auto meta = ctx.block_table()->GetOrCreate(bid);
            meta->state.store(BlockState::kHome, std::memory_order_relaxed);
            meta->owner_node.store(ctx.GetConfig().node_id, std::memory_order_relaxed);
        }
        ctx.stats()->IncAlloc();
    }
    return addr;
}

Status lm_free(GlobalAddr addr) {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    return ctx.allocator()->Free(addr);
}

}  // namespace leomem
