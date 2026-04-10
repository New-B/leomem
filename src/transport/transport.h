#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "leomem/addr.h"
#include "leomem/block.h"
#include "leomem/config.h"
#include "leomem/status.h"

namespace leomem {

struct RemoteWriteRequest {
    GlobalAddr addr{};
    const void* buf = nullptr;
    std::size_t size = 0;
    CoherenceMode mode = CoherenceMode::kSI;
    bool owner_biased = false;
    bool owner_handoff = false;
};

enum class ControlOpcode {
    kInvalidateRange = 0,
    kOwnerTransfer = 1,
    kInvalidateAck = 2,
    kOwnerTransferAck = 3,
};

struct ControlMessage {
    ControlOpcode opcode = ControlOpcode::kInvalidateRange;
    NodeId source_node = kInvalidNodeId;
    NodeId target_node = kInvalidNodeId;
    std::uint64_t request_id = 0;
    BlockId first_block{};
    BlockId last_block{};
    Version version = 0;
    CoherenceMode mode = CoherenceMode::kSI;
};

struct TransportCapabilities {
    bool supports_one_sided_read = false;
    bool supports_one_sided_write = false;
    bool supports_batched_write = false;
    bool supports_control_path = false;
};

struct RdmaMemoryRegion {
    void* local_addr = nullptr;
    std::uint64_t remote_addr = 0;
    std::uint32_t rkey = 0;
    std::size_t length = 0;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual Status Init() = 0;
    virtual Status Shutdown() = 0;

    virtual Status Read(GlobalAddr addr, void* buf, std::size_t size) = 0;
    virtual Status Write(GlobalAddr addr, const void* buf, std::size_t size) = 0;
    virtual Status BatchWrite(const std::vector<RemoteWriteRequest>& requests) = 0;
    virtual Status Fence() = 0;
    virtual Status PollCompletions(std::size_t max_completions, std::size_t* completed) = 0;

    virtual TransportCapabilities GetCapabilities() const = 0;
    virtual Status RegisterMemoryRegion(void* addr, std::size_t length, RdmaMemoryRegion* out) = 0;
    virtual Status PostControlMessage(const ControlMessage& message) = 0;
    virtual Status DrainControlMessages(std::size_t max_messages,
                                        std::vector<ControlMessage>* messages) = 0;
};

std::unique_ptr<Transport> CreateTransport(const Config& cfg);

}  // namespace leomem
