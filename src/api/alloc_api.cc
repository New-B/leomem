#include "leomem/leomem.h"

#include "runtime/context.h"
#include "runtime/init.h"
#include "stats/counters.h"

namespace leomem {

GlobalAddr lm_malloc(std::size_t size) {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return InvalidGlobalAddr();

    GlobalAddr addr = ctx.allocator()->Allocate(size);
    if (addr.IsValid()) {
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