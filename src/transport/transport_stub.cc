#include "transport/transport.h"

#include <cstring>
#include <memory>

#include "common/log.h"
#include "runtime/context.h"
#include "memory/global/dsm_allocator.h"

namespace leomem {

class StubTransport final : public Transport {
public:
    explicit StubTransport(const Config& cfg) : cfg_(cfg) {}

    Status Init() override {
        LM_LOG_INFO("Using stub transport");
        return Status::kOk;
    }

    Status Shutdown() override {
        return Status::kOk;
    }

    Status Read(GlobalAddr addr, void* buf, std::size_t size) override {
        auto& ctx = RuntimeContext::Instance();
        if (addr.home_node == ctx.GetConfig().node_id) {
            void* src = ctx.allocator()->TranslateLocal(addr, size);
            if (src == nullptr) return Status::kOutOfRange;
            std::memcpy(buf, src, size);
            return Status::kOk;
        }
        return Status::kUnimplemented;
    }

    Status Write(GlobalAddr addr, const void* buf, std::size_t size) override {
        auto& ctx = RuntimeContext::Instance();
        if (addr.home_node == ctx.GetConfig().node_id) {
            void* dst = ctx.allocator()->TranslateLocal(addr, size);
            if (dst == nullptr) return Status::kOutOfRange;
            std::memcpy(dst, buf, size);
            return Status::kOk;
        }
        return Status::kUnimplemented;
    }

    Status Fence() override {
        return Status::kOk;
    }

private:
    Config cfg_;
};

std::unique_ptr<Transport> CreateTransport(const Config& cfg) {
    return std::make_unique<StubTransport>(cfg);
}

}  // namespace leomem

// 这层现在是 local loopback + remote TODO。
// 后面 Phase 2/3 再把真正的 RDMA transport 接进去即可，不会影响 API 层。