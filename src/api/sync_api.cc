#include "leomem/leomem.h"

#include "runtime/context.h"
#include "transport/transport.h"

namespace leomem {

Status lm_fence() {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    return ctx.transport()->Fence();
}

Status lm_barrier() {
    // Phase 1: single-node stub
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    return Status::kOk;
}

}  // namespace leomem