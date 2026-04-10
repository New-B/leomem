#include "leomem/leomem.h"

#include "memory/global/dsm_allocator.h"
#include "memory/cache/cache_manager.h"
#include "metadata/block_table.h"
#include "metadata/block_meta.h"
#include "runtime/context.h"
#include "stats/counters.h"
#include "transport/transport.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace leomem {

namespace {

BlockRange ResolveBlockRange(const RuntimeContext& ctx, GlobalAddr addr, std::size_t size) {
    BlockRange range;
    range.first.region_id = addr.region_id;
    range.first.block_index = addr.offset / ctx.GetConfig().block_size;
    range.last.region_id = addr.region_id;
    range.last.block_index = (addr.offset + size - 1) / ctx.GetConfig().block_size;
    range.count = static_cast<std::size_t>(range.last.block_index - range.first.block_index + 1);
    return range;
}

Status ValidateAndResolveRange(RuntimeContext& ctx, GlobalAddr addr, std::size_t size, BlockRange* range) {
    if (range == nullptr) return Status::kInvalidArg;
    if (size == 0 || ctx.GetConfig().block_size == 0) return Status::kInvalidArg;

    if (addr.home_node == ctx.GetConfig().node_id) {
        auto local = ctx.allocator()->ResolveBlockRange(addr, size);
        if (!local.has_value()) return Status::kOutOfRange;
        *range = *local;
        return Status::kOk;
    }

    *range = ResolveBlockRange(ctx, addr, size);
    return Status::kOk;
}

std::vector<std::shared_ptr<BlockMeta>> PrepareMetadata(RuntimeContext& ctx,
                                                        const BlockRange& range,
                                                        GlobalAddr addr,
                                                        AccessType type) {
    std::vector<std::shared_ptr<BlockMeta>> metas;
    metas.reserve(range.count);

    for (std::uint64_t index = range.first.block_index; index <= range.last.block_index; ++index) {
        BlockId bid{addr.region_id, index};
        auto meta = ctx.block_table()->GetOrCreate(bid);
        if (addr.home_node == ctx.GetConfig().node_id) {
            meta->state.store(BlockState::kHome, std::memory_order_relaxed);
            meta->owner_node.store(ctx.GetConfig().node_id, std::memory_order_relaxed);
            if (type == AccessType::kWrite) {
                meta->version.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            meta->state.store(BlockState::kShared, std::memory_order_relaxed);
            meta->owner_node.store(addr.home_node, std::memory_order_relaxed);
        }
        metas.push_back(std::move(meta));
    }

    return metas;
}

std::vector<CoherenceMode> ObserveMetas(RuntimeContext& ctx,
                                        const std::vector<std::shared_ptr<BlockMeta>>& metas,
                                        AccessType type,
                                        bool is_local) {
    std::vector<CoherenceMode> modes;
    modes.reserve(metas.size());
    for (const auto& meta : metas) {
        const CoherenceMode before = meta->mode.load(std::memory_order_relaxed);
        const CoherenceMode after = ctx.cache_manager()->ObserveAccess(meta, type, is_local);
        ctx.stats()->RecordCoherenceMode(after, before != after);
        modes.push_back(after);
    }
    return modes;
}

bool ShouldAdmitCached(RuntimeContext& ctx, const std::vector<std::shared_ptr<BlockMeta>>& metas) {
    if (metas.empty()) return false;
    for (const auto& meta : metas) {
        if (!ctx.cache_manager()->ShouldCache(meta)) {
            return false;
        }
    }
    for (const auto& meta : metas) {
        meta->cache_admitted.store(true, std::memory_order_relaxed);
    }
    return true;
}

bool HasMode(const std::vector<CoherenceMode>& modes, CoherenceMode target) {
    for (CoherenceMode mode : modes) {
        if (mode == target) return true;
    }
    return false;
}

std::size_t MostAggressiveBatchThreshold(CacheManager* cache_manager,
                                         const std::vector<CoherenceMode>& modes) {
    std::size_t threshold = cache_manager->EffectiveBatchThreshold(CoherenceMode::kSI);
    for (CoherenceMode mode : modes) {
        threshold = std::min(threshold, cache_manager->EffectiveBatchThreshold(mode));
    }
    return threshold;
}

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

GlobalAddr BlockRangeBaseAddr(const RuntimeContext& ctx, const BlockRange& range, NodeId home_node) {
    GlobalAddr addr;
    addr.home_node = home_node;
    addr.region_id = range.first.region_id;
    addr.offset = range.first.block_index * ctx.GetConfig().block_size;
    return addr;
}

std::size_t BlockRangeSize(const RuntimeContext& ctx, const BlockRange& range) {
    return range.count * ctx.GetConfig().block_size;
}

Status ProcessPendingControlMessages(RuntimeContext& ctx) {
    std::vector<ControlMessage> messages;
    Status s = ctx.transport()->DrainControlMessages(64, &messages);
    if (s != Status::kOk && s != Status::kUnimplemented) return s;

    for (const auto& msg : messages) {
        if (IsAckOpcode(msg.opcode)) {
            ctx.CompleteControlAck(msg.request_id);
            continue;
        }

        BlockRange range;
        range.first = msg.first_block;
        range.last = msg.last_block;
        range.count = static_cast<std::size_t>(range.last.block_index - range.first.block_index + 1);

        GlobalAddr base = BlockRangeBaseAddr(ctx, range, msg.source_node);
        const std::size_t span = BlockRangeSize(ctx, range);

        if (msg.opcode == ControlOpcode::kInvalidateRange) {
            ctx.cache_manager()->InvalidateRange(base, span);
            for (std::uint64_t index = range.first.block_index; index <= range.last.block_index; ++index) {
                auto meta = ctx.block_table()->GetOrCreate(BlockId{range.first.region_id, index});
                meta->version.store(msg.version, std::memory_order_relaxed);
                meta->state.store(BlockState::kInvalid, std::memory_order_relaxed);
                meta->mode.store(msg.mode, std::memory_order_relaxed);
            }
            ControlMessage ack = msg;
            ack.opcode = AckOpcodeFor(msg.opcode);
            ack.source_node = ctx.GetConfig().node_id;
            ack.target_node = msg.source_node;
            Status ack_status = ctx.transport()->PostControlMessage(ack);
            if (ack_status != Status::kOk && ack_status != Status::kUnimplemented) return ack_status;
            continue;
        }

        if (msg.opcode == ControlOpcode::kOwnerTransfer) {
            for (std::uint64_t index = range.first.block_index; index <= range.last.block_index; ++index) {
                auto meta = ctx.block_table()->GetOrCreate(BlockId{range.first.region_id, index});
                meta->owner_node.store(msg.source_node, std::memory_order_relaxed);
                meta->version.store(msg.version, std::memory_order_relaxed);
                meta->mode.store(msg.mode, std::memory_order_relaxed);
                meta->state.store(BlockState::kOwner, std::memory_order_relaxed);
            }
            ControlMessage ack = msg;
            ack.opcode = AckOpcodeFor(msg.opcode);
            ack.source_node = ctx.GetConfig().node_id;
            ack.target_node = msg.source_node;
            Status ack_status = ctx.transport()->PostControlMessage(ack);
            if (ack_status != Status::kOk && ack_status != Status::kUnimplemented) return ack_status;
        }
    }
    return Status::kOk;
}

ControlMessage MakeControlMessage(RuntimeContext& ctx,
                                  GlobalAddr addr,
                                  const BlockRange& range,
                                  CoherenceMode mode,
                                  ControlOpcode opcode,
                                  Version version) {
    ControlMessage msg;
    msg.opcode = opcode;
    msg.source_node = ctx.GetConfig().node_id;
    msg.target_node = addr.home_node;
    msg.request_id = ctx.NextControlRequestId();
    msg.first_block = range.first;
    msg.last_block = range.last;
    msg.version = version;
    msg.mode = mode;
    return msg;
}

NodeId DominantOwnerNode(const std::vector<std::shared_ptr<BlockMeta>>& metas, NodeId fallback) {
    if (metas.empty()) return fallback;
    NodeId owner = metas.front()->owner_node.load(std::memory_order_relaxed);
    if (owner == kInvalidNodeId) return fallback;
    for (std::size_t i = 1; i < metas.size(); ++i) {
        if (metas[i]->owner_node.load(std::memory_order_relaxed) != owner) {
            return fallback;
        }
    }
    return owner;
}

Version MaxObservedVersion(const std::vector<std::shared_ptr<BlockMeta>>& metas) {
    Version version = 0;
    for (const auto& meta : metas) {
        version = std::max(version, meta->version.load(std::memory_order_relaxed));
    }
    return version;
}

Status TryOwnerAwareRead(RuntimeContext& ctx,
                         GlobalAddr addr,
                         void* buf,
                         std::size_t size,
                         const std::vector<std::shared_ptr<BlockMeta>>& metas,
                         bool* redirected_to_owner) {
    if (redirected_to_owner != nullptr) {
        *redirected_to_owner = false;
    }

    const NodeId owner = DominantOwnerNode(metas, addr.home_node);
    if (owner == ctx.GetConfig().node_id) {
        if (ctx.cache_manager()->TryReadCached(addr, buf, size)) {
            if (redirected_to_owner != nullptr) {
                *redirected_to_owner = true;
            }
            return Status::kOk;
        }
        return ctx.transport()->Read(addr, buf, size);
    }

    if (owner != kInvalidNodeId && owner != addr.home_node) {
        GlobalAddr owner_addr = addr;
        owner_addr.home_node = owner;
        Status owner_status = ctx.transport()->Read(owner_addr, buf, size);
        if (owner_status == Status::kOk) {
            if (redirected_to_owner != nullptr) {
                *redirected_to_owner = true;
            }
            return Status::kOk;
        }
    }

    return ctx.transport()->Read(addr, buf, size);
}

void ApplyWriteMetadata(RuntimeContext& ctx,
                        GlobalAddr addr,
                        const std::vector<std::shared_ptr<BlockMeta>>& metas,
                        const std::vector<CoherenceMode>& modes,
                        bool* owner_biased,
                        bool* owner_handoff) {
    if (owner_biased != nullptr) *owner_biased = false;
    if (owner_handoff != nullptr) *owner_handoff = false;

    for (std::size_t i = 0; i < metas.size(); ++i) {
        auto& meta = metas[i];
        const CoherenceMode mode = modes[i];
        meta->version.fetch_add(1, std::memory_order_relaxed);

        if (mode == CoherenceMode::kWI) {
            meta->owner_node.store(addr.home_node, std::memory_order_relaxed);
            meta->state.store(BlockState::kShared, std::memory_order_relaxed);
            continue;
        }

        if (mode == CoherenceMode::kAdaptive) {
            const NodeId previous_owner = meta->owner_node.exchange(ctx.GetConfig().node_id, std::memory_order_relaxed);
            if (owner_handoff != nullptr && previous_owner != ctx.GetConfig().node_id) {
                *owner_handoff = true;
            }
            meta->owner_epoch.fetch_add(1, std::memory_order_relaxed);
            meta->state.store(BlockState::kOwner, std::memory_order_relaxed);
            if (owner_biased != nullptr) {
                *owner_biased = true;
            }
            continue;
        }

        meta->state.store(BlockState::kShared, std::memory_order_relaxed);
    }
}

Status FlushDestination(RuntimeContext& ctx, NodeId node) {
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
    return Status::kOk;
}

}  // namespace

Status lm_read(GlobalAddr addr, void* buf, std::size_t size) {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    if (!addr.IsValid() || buf == nullptr || size == 0) return Status::kInvalidArg;
    Status ctrl = ProcessPendingControlMessages(ctx);
    if (ctrl != Status::kOk) return ctrl;

    const bool is_local = (addr.home_node == ctx.GetConfig().node_id);
    BlockRange range;
    Status meta_status = ValidateAndResolveRange(ctx, addr, size, &range);
    if (meta_status != Status::kOk) return meta_status;

    auto metas = PrepareMetadata(ctx, range, addr, AccessType::kRead);
    auto modes = ObserveMetas(ctx, metas, AccessType::kRead, is_local);

    if (!is_local && ctx.cache_manager()->TryReadCached(addr, buf, size)) {
        ctx.stats()->IncCacheHit();
        ctx.stats()->IncRead(false);
        return Status::kOk;
    }

    if (!is_local) {
        ctx.stats()->IncCacheMiss();
        for (const auto& meta : metas) {
            meta->remote_miss_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool redirected_to_owner = false;
    Status s = is_local
        ? ctx.transport()->Read(addr, buf, size)
        : TryOwnerAwareRead(ctx, addr, buf, size, metas, &redirected_to_owner);
    if (s == Status::kOk) {
        if (!is_local && redirected_to_owner) {
            const Version owner_version = MaxObservedVersion(metas);
            for (const auto& meta : metas) {
                meta->state.store(BlockState::kOwner, std::memory_order_relaxed);
                meta->version.store(owner_version, std::memory_order_relaxed);
            }
        }
        if (!is_local) {
            const bool admitted = !HasMode(modes, CoherenceMode::kWI) && ShouldAdmitCached(ctx, metas);
            ctx.stats()->IncCacheAdmission(admitted);
            if (admitted) {
                ctx.cache_manager()->InsertCached(addr, buf, size);
            }
        }
        ctx.stats()->IncRead(is_local);
    }
    return s;
}

Status lm_write(GlobalAddr addr, const void* buf, std::size_t size) {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return Status::kNotInitialized;
    if (!addr.IsValid() || buf == nullptr || size == 0) return Status::kInvalidArg;
    Status ctrl = ProcessPendingControlMessages(ctx);
    if (ctrl != Status::kOk) return ctrl;

    const bool is_local = (addr.home_node == ctx.GetConfig().node_id);
    BlockRange range;
    Status meta_status = ValidateAndResolveRange(ctx, addr, size, &range);
    if (meta_status != Status::kOk) return meta_status;

    auto metas = PrepareMetadata(ctx, range, addr, AccessType::kWrite);
    auto modes = ObserveMetas(ctx, metas, AccessType::kWrite, is_local);

    if (!is_local) {
        bool owner_biased = false;
        bool owner_handoff = false;
        ApplyWriteMetadata(ctx, addr, metas, modes, &owner_biased, &owner_handoff);

        Version max_version = 0;
        for (const auto& meta : metas) {
            max_version = std::max(max_version, meta->version.load(std::memory_order_relaxed));
        }

        const bool range_invalidation = HasMode(modes, CoherenceMode::kWI);
        if (range_invalidation) {
            ControlMessage msg = MakeControlMessage(
                ctx, addr, range, CoherenceMode::kWI, ControlOpcode::kInvalidateRange, max_version);
            Status ctrl = ctx.transport()->PostControlMessage(msg);
            if (ctrl != Status::kOk && ctrl != Status::kUnimplemented) return ctrl;
            if (ctrl == Status::kOk) ctx.TrackPendingControlAck(msg.request_id);
        }
        if (owner_handoff) {
            ControlMessage msg = MakeControlMessage(
                ctx, addr, range, CoherenceMode::kAdaptive, ControlOpcode::kOwnerTransfer, max_version);
            Status ctrl = ctx.transport()->PostControlMessage(msg);
            if (ctrl != Status::kOk && ctrl != Status::kUnimplemented) return ctrl;
            if (ctrl == Status::kOk) ctx.TrackPendingControlAck(msg.request_id);
        }

        const std::size_t invalidated = range_invalidation
            ? ctx.cache_manager()->InvalidateRange(addr, size)
            : ctx.cache_manager()->Invalidate(addr, size);
        ctx.stats()->AddCacheInvalidations(range_invalidation, invalidated);
        if (!ctx.GetConfig().enable_write_batching) {
            Status s = ctx.transport()->Write(addr, buf, size);
            if (s == Status::kOk) {
                ctx.stats()->IncWrite(false);
            }
            return s;
        }

        const CoherenceMode dominant_mode =
            HasMode(modes, CoherenceMode::kWI) ? CoherenceMode::kWI :
            HasMode(modes, CoherenceMode::kAdaptive) ? CoherenceMode::kAdaptive :
                                                       CoherenceMode::kSI;
        const bool merged = ctx.cache_manager()->QueueRemoteWrite(
            addr, buf, size, dominant_mode, owner_biased, owner_handoff);
        ctx.stats()->IncQueuedRemoteWrite();
        if (merged) {
            ctx.stats()->IncMergedRemoteWrite();
        }
        if (owner_biased) {
            ctx.cache_manager()->InsertCached(addr, buf, size);
        }

        const std::size_t threshold = MostAggressiveBatchThreshold(ctx.cache_manager(), modes);
        if (HasMode(modes, CoherenceMode::kWI) ||
            ctx.cache_manager()->ShouldFlushDestination(addr.home_node, threshold)) {
            Status flush_status = FlushDestination(ctx, addr.home_node);
            if (flush_status != Status::kOk) return flush_status;
        }
        ctx.stats()->IncWrite(false);
        return Status::kOk;
    }

    Status s = ctx.transport()->Write(addr, buf, size);
    if (s == Status::kOk) {
        ctx.stats()->IncWrite(is_local);
    }
    return s;
}

}  // namespace leomem
