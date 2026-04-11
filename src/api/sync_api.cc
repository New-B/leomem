#include "leomem/leomem.h"

#include "memory/cache/cache_manager.h"
#include "metadata/block_table.h"
#include "runtime/context.h"
#include "stats/counters.h"
#include "transport/transport.h"

namespace leomem {

namespace {

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

Status DrainControl(RuntimeContext& ctx) {
    std::vector<ControlMessage> messages;
    Status s = ctx.transport()->DrainControlMessages(64, &messages);
    if (s != Status::kOk && s != Status::kUnimplemented) return s;

    for (const auto& msg : messages) {
        if (IsAckOpcode(msg.opcode)) {
            ctx.CompleteControlAck(msg.request_id);
            continue;
        }

        if (msg.opcode == ControlOpcode::kInvalidateRange) {
            GlobalAddr addr;
            addr.home_node = msg.source_node;
            addr.region_id = msg.first_block.region_id;
            addr.offset = msg.first_block.block_index * ctx.GetConfig().block_size;
            const std::size_t span =
                static_cast<std::size_t>(msg.last_block.block_index - msg.first_block.block_index + 1) *
                ctx.GetConfig().block_size;
            ctx.cache_manager()->InvalidateRange(addr, span);
            ControlMessage ack = msg;
            ack.opcode = AckOpcodeFor(msg.opcode);
            ack.source_node = ctx.GetConfig().node_id;
            ack.target_node = msg.source_node;
            Status ack_status = ctx.transport()->PostControlMessage(ack);
            if (ack_status != Status::kOk && ack_status != Status::kUnimplemented) return ack_status;
            continue;
        }

        for (std::uint64_t index = msg.first_block.block_index; index <= msg.last_block.block_index; ++index) {
            auto meta = ctx.block_table()->GetOrCreate(BlockId{msg.first_block.region_id, index});
            meta->owner_node.store(msg.source_node, std::memory_order_relaxed);
            meta->version.store(msg.version, std::memory_order_relaxed);
            meta->mode.store(msg.mode, std::memory_order_relaxed);
        }
        ControlMessage ack = msg;
        ack.opcode = AckOpcodeFor(msg.opcode);
        ack.source_node = ctx.GetConfig().node_id;
        ack.target_node = msg.source_node;
        Status ack_status = ctx.transport()->PostControlMessage(ack);
        if (ack_status != Status::kOk && ack_status != Status::kUnimplemented) return ack_status;
    }

    std::vector<ControlMessage> retries;
    Status retry_status = ctx.AdvanceControlAckTimeouts(&retries);
    if (retry_status != Status::kOk) return retry_status;
    for (const auto& retry : retries) {
        Status post = ctx.transport()->PostControlMessage(retry);
        if (post != Status::kOk && post != Status::kUnimplemented) return post;
    }
    return Status::kOk;
}

}  // namespace

Status lm_fence() {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    Status ctrl = DrainControl(ctx);
    if (ctrl != Status::kOk) return ctrl;

    for (NodeId node : ctx.cache_manager()->PendingDestinations()) {
        auto batch = ctx.cache_manager()->DrainRemoteWrites(node);
        std::vector<RemoteWriteRequest> requests;
        requests.reserve(batch.writes.size());
        for (const auto& pending : batch.writes) {
            requests.push_back(RemoteWriteRequest{
                pending.addr,
                pending.payload.data(),
                pending.payload.size(),
                pending.mode,
                pending.owner_biased,
                pending.owner_handoff,
            });
        }
        if (!requests.empty()) {
            Status s = ctx.transport()->BatchWrite(requests);
            if (s != Status::kOk) return s;
        }
        for (const auto& pending : batch.writes) {
            if (pending.owner_biased) {
                ctx.stats()->IncOwnerBiasedWrite();
            }
            if (pending.owner_handoff) {
                ctx.stats()->IncOwnerHandoff();
            }
        }
        if (!batch.writes.empty()) {
            ctx.stats()->AddFlushedRemoteBatch(batch.writes.size(), batch.total_bytes);
        }
    }

    Status fence_status = ctx.transport()->Fence();
    if (fence_status != Status::kOk) return fence_status;
    return DrainControl(ctx);
}

Status lm_barrier() {
    // Phase 1: single-node stub
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    return Status::kOk;
}

}  // namespace leomem
