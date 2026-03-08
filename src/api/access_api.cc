#include "leomem/leomem.h"

#include <cstring>

#include "runtime/context.h"
#include "transport/transport.h"

namespace leomem {

Status lm_read(GlobalAddr addr, void* buf, std::size_t size) {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    if (!addr.IsValid() || buf == nullptr || size == 0) return Status::kInvalidArg;

    const bool is_local = (addr.home_node == ctx.GetConfig().node_id);
    Status s = ctx.transport()->Read(addr, buf, size);
    if (s == Status::kOk) {
        ctx.stats()->IncRead(is_local);
    }
    return s;
}

Status lm_write(GlobalAddr addr, const void* buf, std::size_t size) {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    if (!addr.IsValid() || buf == nullptr || size == 0) return Status::kInvalidArg;

    const bool is_local = (addr.home_node == ctx.GetConfig().node_id);
    Status s = ctx.transport()->Write(addr, buf, size);
    if (s == Status::kOk) {
        ctx.stats()->IncWrite(is_local);
    }
    return s;
}

}  // namespace leomem