#include "transport/transport.h"
#include "transport/rdma_transport.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "runtime/context.h"
#include "memory/global/dsm_allocator.h"

namespace leomem {

namespace {

std::mutex g_control_mu;
std::unordered_map<NodeId, std::deque<ControlMessage>> g_control_inboxes;

bool IsAckOpcode(ControlOpcode opcode) {
    return opcode == ControlOpcode::kInvalidateAck || opcode == ControlOpcode::kOwnerTransferAck;
}

ControlOpcode AckOpcodeFor(ControlOpcode opcode) {
    switch (opcode) {
        case ControlOpcode::kInvalidateRange:
            return ControlOpcode::kInvalidateAck;
        case ControlOpcode::kOwnerTransfer:
            return ControlOpcode::kOwnerTransferAck;
        case ControlOpcode::kInvalidateAck:
            return ControlOpcode::kInvalidateAck;
        case ControlOpcode::kOwnerTransferAck:
            return ControlOpcode::kOwnerTransferAck;
    }
    return ControlOpcode::kInvalidateAck;
}

}  // namespace

class StubTransport final : public Transport {
public:
    explicit StubTransport(const Config& cfg) : cfg_(cfg) {}

    Status Init() override {
        std::lock_guard<std::mutex> lk(mu_);
        remote_regions_.clear();
        std::lock_guard<std::mutex> ctrl_lk(g_control_mu);
        g_control_inboxes[cfg_.node_id];
        LM_LOG_INFO("Using stub transport");
        return Status::kOk;
    }

    Status Shutdown() override {
        std::lock_guard<std::mutex> lk(mu_);
        remote_regions_.clear();
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

        std::lock_guard<std::mutex> lk(mu_);
        auto& region = remote_regions_[addr.home_node];
        if (region.empty()) {
            region.resize(cfg_.local_region_size, 0);
        }
        if (addr.offset + size > region.size()) return Status::kOutOfRange;
        std::memcpy(buf, region.data() + addr.offset, size);
        return Status::kOk;
    }

    Status Write(GlobalAddr addr, const void* buf, std::size_t size) override {
        auto& ctx = RuntimeContext::Instance();
        if (addr.home_node == ctx.GetConfig().node_id) {
            void* dst = ctx.allocator()->TranslateLocal(addr, size);
            if (dst == nullptr) return Status::kOutOfRange;
            std::memcpy(dst, buf, size);
            return Status::kOk;
        }

        std::lock_guard<std::mutex> lk(mu_);
        auto& region = remote_regions_[addr.home_node];
        if (region.empty()) {
            region.resize(cfg_.local_region_size, 0);
        }
        if (addr.offset + size > region.size()) return Status::kOutOfRange;
        std::memcpy(region.data() + addr.offset, buf, size);
        return Status::kOk;
    }

    Status BatchWrite(const std::vector<RemoteWriteRequest>& requests) override {
        for (const auto& req : requests) {
            Status s = Write(req.addr, req.buf, req.size);
            if (s != Status::kOk) return s;
        }
        return Status::kOk;
    }

    Status Fence() override {
        return Status::kOk;
    }

    Status PollCompletions(std::size_t, std::size_t* completed) override {
        if (completed != nullptr) {
            *completed = 0;
        }
        return Status::kOk;
    }

    TransportCapabilities GetCapabilities() const override {
        TransportCapabilities caps;
        caps.supports_one_sided_read = true;
        caps.supports_one_sided_write = true;
        caps.supports_batched_write = true;
        caps.supports_control_path = true;
        return caps;
    }

    Status RegisterMemoryRegion(void* addr, std::size_t length, RdmaMemoryRegion* out) override {
        if (addr == nullptr || length == 0 || out == nullptr) return Status::kInvalidArg;
        out->local_addr = addr;
        out->remote_addr = reinterpret_cast<std::uint64_t>(addr);
        out->rkey = 0;
        out->length = length;
        return Status::kOk;
    }

    Status PostControlMessage(const ControlMessage& message) override {
        std::lock_guard<std::mutex> lk(g_control_mu);
        if (message.target_node != cfg_.node_id && !IsAckOpcode(message.opcode)) {
            ControlMessage ack = message;
            ack.opcode = AckOpcodeFor(message.opcode);
            ack.source_node = message.target_node;
            ack.target_node = message.source_node;
            g_control_inboxes[ack.target_node].push_back(ack);
            return Status::kOk;
        }
        g_control_inboxes[message.target_node].push_back(message);
        return Status::kOk;
    }

    Status DrainControlMessages(std::size_t max_messages,
                                std::vector<ControlMessage>* messages) override {
        if (messages == nullptr) return Status::kInvalidArg;
        messages->clear();

        std::lock_guard<std::mutex> lk(g_control_mu);
        auto& inbox = g_control_inboxes[cfg_.node_id];
        const std::size_t limit = std::max<std::size_t>(1, max_messages);
        while (!inbox.empty() && messages->size() < limit) {
            messages->push_back(inbox.front());
            inbox.pop_front();
        }
        return Status::kOk;
    }

private:
    Config cfg_;
    std::mutex mu_;
    std::unordered_map<NodeId, std::vector<std::uint8_t>> remote_regions_;
};

std::unique_ptr<Transport> CreateTransport(const Config& cfg) {
    if (cfg.enable_rdma) {
        return CreateRdmaTransport(cfg);
    }
    return std::make_unique<StubTransport>(cfg);
}

}  // namespace leomem

// 这层现在是 local loopback + remote TODO。
// 后面 Phase 2/3 再把真正的 RDMA transport 接进去即可，不会影响 API 层。
