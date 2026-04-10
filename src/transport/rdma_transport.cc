#include "transport/rdma_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"

#if defined(LEOMEM_HAS_IBVERBS)
#include <infiniband/verbs.h>
#endif

namespace leomem {

namespace {

#if defined(LEOMEM_HAS_IBVERBS)

struct PeerExchangeInfo {
    NodeId node_id = kInvalidNodeId;
    std::uint32_t qpn = 0;
    std::uint32_t psn = 0;
    std::uint16_t lid = 0;
    std::uint64_t remote_addr = 0;
    std::uint32_t rkey = 0;
    std::uint64_t length = 0;
    bool has_gid = false;
    std::uint8_t gid_raw[16]{};
};

struct PeerConnection {
    NodeId node_id = kInvalidNodeId;
    ibv_qp* qp = nullptr;
    PeerExchangeInfo remote{};
    std::uint32_t local_psn = 0;
    bool ready = false;
};

std::filesystem::path ResolveExchangeDir(const Config& cfg) {
    if (!cfg.rdma_exchange_dir.empty()) {
        return std::filesystem::path(cfg.rdma_exchange_dir);
    }
    if (!cfg.cluster_config_path.empty()) {
        std::filesystem::path base(cfg.cluster_config_path);
        const auto parent = base.has_parent_path() ? base.parent_path() : std::filesystem::current_path();
        return parent / "rdma_exchange";
    }
    return std::filesystem::temp_directory_path() / "leomem-rdma-exchange";
}

std::string FormatExchangeInfo(const PeerExchangeInfo& info) {
    std::ostringstream oss;
    oss << "node_id=" << info.node_id << '\n'
        << "qpn=" << info.qpn << '\n'
        << "psn=" << info.psn << '\n'
        << "lid=" << info.lid << '\n'
        << "remote_addr=" << info.remote_addr << '\n'
        << "rkey=" << info.rkey << '\n'
        << "length=" << info.length << '\n'
        << "has_gid=" << (info.has_gid ? 1 : 0) << '\n'
        << "gid=";
    for (int i = 0; i < 16; ++i) {
        if (i != 0) oss << ':';
        oss << static_cast<unsigned>(info.gid_raw[i]);
    }
    oss << '\n';
    return oss.str();
}

std::string FormatControlMessage(const ControlMessage& message) {
    std::ostringstream oss;
    oss << "opcode=" << static_cast<int>(message.opcode) << '\n'
        << "source_node=" << message.source_node << '\n'
        << "target_node=" << message.target_node << '\n'
        << "request_id=" << message.request_id << '\n'
        << "first_region=" << message.first_block.region_id << '\n'
        << "first_block=" << message.first_block.block_index << '\n'
        << "last_region=" << message.last_block.region_id << '\n'
        << "last_block=" << message.last_block.block_index << '\n'
        << "version=" << message.version << '\n'
        << "mode=" << static_cast<int>(message.mode) << '\n';
    return oss.str();
}

bool ParseControlMessage(const std::string& text, ControlMessage* out) {
    if (out == nullptr) return false;
    ControlMessage msg;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (key == "opcode") msg.opcode = static_cast<ControlOpcode>(std::stoi(value));
        else if (key == "source_node") msg.source_node = static_cast<NodeId>(std::stoul(value));
        else if (key == "target_node") msg.target_node = static_cast<NodeId>(std::stoul(value));
        else if (key == "request_id") msg.request_id = std::stoull(value);
        else if (key == "first_region") msg.first_block.region_id = static_cast<RegionId>(std::stoul(value));
        else if (key == "first_block") msg.first_block.block_index = std::stoull(value);
        else if (key == "last_region") msg.last_block.region_id = static_cast<RegionId>(std::stoul(value));
        else if (key == "last_block") msg.last_block.block_index = std::stoull(value);
        else if (key == "version") msg.version = std::stoull(value);
        else if (key == "mode") msg.mode = static_cast<CoherenceMode>(std::stoi(value));
    }
    if (msg.source_node == kInvalidNodeId || msg.target_node == kInvalidNodeId) return false;
    *out = msg;
    return true;
}

bool ParseExchangeInfo(const std::string& text, PeerExchangeInfo* out) {
    if (out == nullptr) return false;

    PeerExchangeInfo parsed;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (key == "node_id") parsed.node_id = static_cast<NodeId>(std::stoul(value));
        else if (key == "qpn") parsed.qpn = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "psn") parsed.psn = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "lid") parsed.lid = static_cast<std::uint16_t>(std::stoul(value));
        else if (key == "remote_addr") parsed.remote_addr = std::stoull(value);
        else if (key == "rkey") parsed.rkey = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "length") parsed.length = std::stoull(value);
        else if (key == "has_gid") parsed.has_gid = (value == "1");
        else if (key == "gid") {
            std::istringstream gid_stream(value);
            std::string byte;
            int idx = 0;
            while (std::getline(gid_stream, byte, ':') && idx < 16) {
                parsed.gid_raw[idx++] = static_cast<std::uint8_t>(std::stoul(byte));
            }
        }
    }

    if (parsed.node_id == kInvalidNodeId || parsed.qpn == 0) return false;
    *out = parsed;
    return true;
}

class RdmaTransport final : public Transport {
public:
    explicit RdmaTransport(const Config& cfg) : cfg_(cfg) {}

    Status Init() override {
        if (initialized_) return Status::kAlreadyInitialized;

        int num_devices = 0;
        ibv_device** device_list = ibv_get_device_list(&num_devices);
        if (device_list == nullptr || num_devices == 0) {
            if (device_list != nullptr) ibv_free_device_list(device_list);
            return Status::kNotFound;
        }

        ibv_device* selected = nullptr;
        for (int i = 0; i < num_devices; ++i) {
            if (cfg_.rdma_device_name.empty() ||
                cfg_.rdma_device_name == ibv_get_device_name(device_list[i])) {
                selected = device_list[i];
                break;
            }
        }
        if (selected == nullptr) {
            ibv_free_device_list(device_list);
            return Status::kNotFound;
        }

        context_ = ibv_open_device(selected);
        ibv_free_device_list(device_list);
        if (context_ == nullptr) return Status::kInternalError;

        if (ibv_query_port(context_, cfg_.rdma_port, &port_attr_) != 0) return Status::kInternalError;
        if (ibv_query_gid(context_, cfg_.rdma_port, cfg_.rdma_gid_index, &gid_) != 0) {
            std::memset(&gid_, 0, sizeof(gid_));
        }

        pd_ = ibv_alloc_pd(context_);
        if (pd_ == nullptr) return Status::kInternalError;

        const int cq_depth = static_cast<int>(
            std::max<std::size_t>(cfg_.rdma_cq_depth, cfg_.rdma_sq_depth * std::max<std::uint16_t>(1, cfg_.nr_nodes)));
        cq_ = ibv_create_cq(context_, cq_depth, nullptr, nullptr, 0);
        if (cq_ == nullptr) return Status::kInternalError;

        peers_.resize(cfg_.nr_nodes);
        for (NodeId node = 0; node < cfg_.nr_nodes; ++node) {
            peers_[node].node_id = node;
            if (node == cfg_.node_id) continue;
            Status s = CreateQueuePair(&peers_[node]);
            if (s != Status::kOk) return s;
        }

        initialized_ = true;
        LM_LOG_INFO("RDMA resources initialized: node=%u device=%s port=%u",
                    cfg_.node_id,
                    cfg_.rdma_device_name.empty() ? ibv_get_device_name(context_->device) : cfg_.rdma_device_name.c_str(),
                    static_cast<unsigned>(cfg_.rdma_port));
        return Status::kOk;
    }

    Status Shutdown() override {
        if (local_mr_ != nullptr) {
            ibv_dereg_mr(local_mr_);
            local_mr_ = nullptr;
        }
        for (auto& peer : peers_) {
            if (peer.qp != nullptr) {
                ibv_destroy_qp(peer.qp);
                peer.qp = nullptr;
            }
            peer.ready = false;
        }
        if (cq_ != nullptr) {
            ibv_destroy_cq(cq_);
            cq_ = nullptr;
        }
        if (pd_ != nullptr) {
            ibv_dealloc_pd(pd_);
            pd_ = nullptr;
        }
        if (context_ != nullptr) {
            ibv_close_device(context_);
            context_ = nullptr;
        }
        initialized_ = false;
        outstanding_sends_ = 0;
        local_base_ = nullptr;
        local_length_ = 0;
        return Status::kOk;
    }

    Status Read(GlobalAddr addr, void* buf, std::size_t size) override {
        if (!initialized_) return Status::kNotInitialized;
        if (buf == nullptr || size == 0) return Status::kInvalidArg;
        if (addr.home_node == cfg_.node_id) {
            return CopyLocal(addr.offset, buf, size, false);
        }

        PeerConnection* peer = GetPeer(addr.home_node);
        if (peer == nullptr || !peer->ready) return Status::kNotFound;
        if (addr.offset + size > peer->remote.length) return Status::kOutOfRange;

        ibv_mr* tmp_mr = ibv_reg_mr(pd_, buf, size,
                                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
        if (tmp_mr == nullptr) return Status::kInternalError;

        Status s = PostOneSided(peer,
                                IBV_WR_RDMA_READ,
                                tmp_mr->addr,
                                size,
                                tmp_mr->lkey,
                                peer->remote.remote_addr + addr.offset,
                                peer->remote.rkey);
        ibv_dereg_mr(tmp_mr);
        return s;
    }

    Status Write(GlobalAddr addr, const void* buf, std::size_t size) override {
        if (!initialized_) return Status::kNotInitialized;
        if (buf == nullptr || size == 0) return Status::kInvalidArg;
        if (addr.home_node == cfg_.node_id) {
            return CopyLocal(addr.offset, const_cast<void*>(buf), size, true);
        }

        PeerConnection* peer = GetPeer(addr.home_node);
        if (peer == nullptr || !peer->ready) return Status::kNotFound;
        if (addr.offset + size > peer->remote.length) return Status::kOutOfRange;

        ibv_mr* tmp_mr = ibv_reg_mr(pd_, const_cast<void*>(buf), size,
                                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
        if (tmp_mr == nullptr) return Status::kInternalError;

        Status s = PostOneSided(peer,
                                IBV_WR_RDMA_WRITE,
                                tmp_mr->addr,
                                size,
                                tmp_mr->lkey,
                                peer->remote.remote_addr + addr.offset,
                                peer->remote.rkey);
        ibv_dereg_mr(tmp_mr);
        return s;
    }

    Status BatchWrite(const std::vector<RemoteWriteRequest>& requests) override {
        if (!initialized_) return Status::kNotInitialized;
        if (requests.empty()) return Status::kOk;

        struct RegisteredWrite {
            ibv_mr* mr = nullptr;
        };
        std::vector<RegisteredWrite> regs;
        regs.reserve(requests.size());
        std::size_t posted = 0;

        for (const auto& req : requests) {
            if (req.buf == nullptr || req.size == 0) return Status::kInvalidArg;
            if (req.addr.home_node == cfg_.node_id) {
                Status s = CopyLocal(req.addr.offset, const_cast<void*>(req.buf), req.size, true);
                if (s != Status::kOk) return s;
                continue;
            }

            PeerConnection* peer = GetPeer(req.addr.home_node);
            if (peer == nullptr || !peer->ready) return Status::kNotFound;
            if (req.addr.offset + req.size > peer->remote.length) return Status::kOutOfRange;

            ibv_mr* tmp_mr = ibv_reg_mr(pd_, const_cast<void*>(req.buf), req.size,
                                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
            if (tmp_mr == nullptr) return Status::kInternalError;
            regs.push_back({tmp_mr});

            ibv_sge sge{};
            sge.addr = reinterpret_cast<std::uintptr_t>(tmp_mr->addr);
            sge.length = static_cast<std::uint32_t>(req.size);
            sge.lkey = tmp_mr->lkey;

            ibv_send_wr wr{};
            wr.wr_id = reinterpret_cast<std::uintptr_t>(tmp_mr);
            wr.opcode = IBV_WR_RDMA_WRITE;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.send_flags = IBV_SEND_SIGNALED;
            if (req.size <= cfg_.rdma_max_inline_bytes) {
                wr.send_flags |= IBV_SEND_INLINE;
            }
            wr.wr.rdma.remote_addr = peer->remote.remote_addr + req.addr.offset;
            wr.wr.rdma.rkey = peer->remote.rkey;

            ibv_send_wr* bad = nullptr;
            if (ibv_post_send(peer->qp, &wr, &bad) != 0) {
                for (auto& reg : regs) {
                    if (reg.mr != nullptr) ibv_dereg_mr(reg.mr);
                }
                return Status::kInternalError;
            }
            ++posted;
            ++outstanding_sends_;
        }

        Status poll_status = WaitForCompletions(posted);
        for (auto& reg : regs) {
            if (reg.mr != nullptr) ibv_dereg_mr(reg.mr);
        }
        return poll_status;
    }

    Status Fence() override {
        if (!initialized_) return Status::kNotInitialized;
        return WaitForCompletions(outstanding_sends_);
    }

    Status PollCompletions(std::size_t max_completions, std::size_t* completed) override {
        if (!initialized_) return Status::kNotInitialized;
        if (completed != nullptr) *completed = 0;

        const int limit = static_cast<int>(std::max<std::size_t>(1, max_completions));
        std::vector<ibv_wc> wcs(static_cast<std::size_t>(limit));
        int polled = ibv_poll_cq(cq_, limit, wcs.data());
        if (polled < 0) return Status::kInternalError;

        for (int i = 0; i < polled; ++i) {
            if (wcs[i].status != IBV_WC_SUCCESS) return Status::kInternalError;
        }

        if (completed != nullptr) {
            *completed = static_cast<std::size_t>(polled);
        }
        if (outstanding_sends_ >= static_cast<std::size_t>(polled)) {
            outstanding_sends_ -= static_cast<std::size_t>(polled);
        } else {
            outstanding_sends_ = 0;
        }
        return Status::kOk;
    }

    TransportCapabilities GetCapabilities() const override {
        TransportCapabilities caps;
        caps.supports_one_sided_read = true;
        caps.supports_one_sided_write = true;
        caps.supports_batched_write = true;
        caps.supports_control_path = cfg_.rdma_enable_control_path;
        return caps;
    }

    Status RegisterMemoryRegion(void* addr, std::size_t length, RdmaMemoryRegion* out) override {
        if (!initialized_) return Status::kNotInitialized;
        if (addr == nullptr || length == 0 || out == nullptr) return Status::kInvalidArg;

        if (local_mr_ != nullptr) {
            ibv_dereg_mr(local_mr_);
            local_mr_ = nullptr;
        }

        local_mr_ = ibv_reg_mr(pd_,
                               addr,
                               length,
                               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
        if (local_mr_ == nullptr) return Status::kInternalError;

        local_base_ = addr;
        local_length_ = length;

        out->local_addr = addr;
        out->remote_addr = reinterpret_cast<std::uint64_t>(addr);
        out->rkey = local_mr_->rkey;
        out->length = length;

        return ExchangeAndConnect();
    }

    Status PostControlMessage(const ControlMessage& message) override {
        if (!cfg_.rdma_enable_control_path) return Status::kUnimplemented;
        PeerConnection* peer = GetPeer(message.target_node);
        if (peer == nullptr || !peer->ready) return Status::kNotFound;
        return WriteControlMessageFile(message);
    }

    Status DrainControlMessages(std::size_t max_messages,
                                std::vector<ControlMessage>* messages) override {
        if (!initialized_) return Status::kNotInitialized;
        if (messages == nullptr) return Status::kInvalidArg;
        messages->clear();
        if (!cfg_.rdma_enable_control_path) return Status::kUnimplemented;

        const auto dir = ResolveExchangeDir(cfg_) / "control";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return Status::kInternalError;

        std::vector<std::filesystem::path> files;
        const std::string prefix = "to_" + std::to_string(cfg_.node_id) + "_";
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) return Status::kInternalError;
            if (!entry.is_regular_file()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        const std::size_t limit = std::max<std::size_t>(1, max_messages);
        for (const auto& path : files) {
            if (messages->size() >= limit) break;
            std::ifstream in(path);
            if (!in.is_open()) continue;
            std::ostringstream content;
            content << in.rdbuf();
            ControlMessage msg;
            if (ParseControlMessage(content.str(), &msg)) {
                messages->push_back(msg);
            }
            std::filesystem::remove(path, ec);
        }
        return Status::kOk;
    }

private:
    Status CreateQueuePair(PeerConnection* peer) {
        if (peer == nullptr) return Status::kInvalidArg;

        ibv_qp_init_attr attr{};
        attr.send_cq = cq_;
        attr.recv_cq = cq_;
        attr.qp_type = IBV_QPT_RC;
        attr.cap.max_send_wr = static_cast<std::uint32_t>(cfg_.rdma_sq_depth);
        attr.cap.max_recv_wr = static_cast<std::uint32_t>(cfg_.rdma_rq_depth);
        attr.cap.max_send_sge = 1;
        attr.cap.max_recv_sge = 1;
        attr.cap.max_inline_data = static_cast<std::uint32_t>(cfg_.rdma_max_inline_bytes);

        peer->qp = ibv_create_qp(pd_, &attr);
        if (peer->qp == nullptr) return Status::kInternalError;

        ibv_qp_attr init_attr{};
        init_attr.qp_state = IBV_QPS_INIT;
        init_attr.pkey_index = 0;
        init_attr.port_num = cfg_.rdma_port;
        init_attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
        if (ibv_modify_qp(peer->qp,
                          &init_attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
            return Status::kInternalError;
        }

        peer->local_psn = (static_cast<std::uint32_t>(cfg_.node_id + 1) << 8) ^
                          static_cast<std::uint32_t>(peer->node_id + 17);
        return Status::kOk;
    }

    Status ExchangeAndConnect() {
        if (local_mr_ == nullptr) return Status::kInvalidArg;

        const auto exchange_dir = ResolveExchangeDir(cfg_);
        std::error_code ec;
        std::filesystem::create_directories(exchange_dir, ec);
        if (ec) return Status::kInternalError;

        for (auto& peer : peers_) {
            if (peer.node_id == cfg_.node_id || peer.qp == nullptr) continue;

            PeerExchangeInfo local{};
            local.node_id = cfg_.node_id;
            local.qpn = peer.qp->qp_num;
            local.psn = peer.local_psn;
            local.lid = port_attr_.lid;
            local.remote_addr = reinterpret_cast<std::uint64_t>(local_base_);
            local.rkey = local_mr_->rkey;
            local.length = local_length_;
            local.has_gid = true;
            std::memcpy(local.gid_raw, &gid_, sizeof(local.gid_raw));

            const auto local_path = exchange_dir /
                ("node_" + std::to_string(cfg_.node_id) + "_to_" + std::to_string(peer.node_id) + ".txt");
            {
                std::ofstream out(local_path);
                if (!out.is_open()) return Status::kInternalError;
                out << FormatExchangeInfo(local);
            }

            const auto remote_path = exchange_dir /
                ("node_" + std::to_string(peer.node_id) + "_to_" + std::to_string(cfg_.node_id) + ".txt");

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.rdma_bootstrap_timeout_ms);
            while (!std::filesystem::exists(remote_path)) {
                if (std::chrono::steady_clock::now() >= deadline) return Status::kNotFound;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            std::ifstream in(remote_path);
            if (!in.is_open()) return Status::kInternalError;
            std::ostringstream content;
            content << in.rdbuf();
            if (!ParseExchangeInfo(content.str(), &peer.remote)) return Status::kInternalError;

            Status s = TransitionQueuePair(peer);
            if (s != Status::kOk) return s;
            peer.ready = true;
        }

        return Status::kOk;
    }

    Status WriteControlMessageFile(const ControlMessage& message) {
        const auto dir = ResolveExchangeDir(cfg_) / "control";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return Status::kInternalError;

        const auto seq = ++control_sequence_;
        const auto path = dir / ("to_" + std::to_string(message.target_node) +
                                 "_from_" + std::to_string(message.source_node) +
                                 "_" + std::to_string(seq) + ".txt");
        std::ofstream out(path);
        if (!out.is_open()) return Status::kInternalError;
        out << FormatControlMessage(message);
        return Status::kOk;
    }

    Status TransitionQueuePair(PeerConnection* peer) {
        if (peer == nullptr || peer->qp == nullptr) return Status::kInvalidArg;

        ibv_qp_attr rtr{};
        rtr.qp_state = IBV_QPS_RTR;
        rtr.path_mtu = std::min(port_attr_.active_mtu, static_cast<std::uint8_t>(IBV_MTU_1024));
        rtr.dest_qp_num = peer->remote.qpn;
        rtr.rq_psn = peer->remote.psn;
        rtr.max_dest_rd_atomic = 1;
        rtr.min_rnr_timer = 12;
        rtr.ah_attr.is_global = peer->remote.has_gid ? 1 : 0;
        rtr.ah_attr.dlid = peer->remote.lid;
        rtr.ah_attr.sl = 0;
        rtr.ah_attr.src_path_bits = 0;
        rtr.ah_attr.port_num = cfg_.rdma_port;
        if (peer->remote.has_gid) {
            std::memcpy(&rtr.ah_attr.grh.dgid, peer->remote.gid_raw, sizeof(peer->remote.gid_raw));
            rtr.ah_attr.grh.hop_limit = 1;
            rtr.ah_attr.grh.sgid_index = cfg_.rdma_gid_index;
        }

        if (ibv_modify_qp(peer->qp,
                          &rtr,
                          IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                          IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
            return Status::kInternalError;
        }

        ibv_qp_attr rts{};
        rts.qp_state = IBV_QPS_RTS;
        rts.timeout = 14;
        rts.retry_cnt = 7;
        rts.rnr_retry = 7;
        rts.sq_psn = peer->local_psn;
        rts.max_rd_atomic = 1;
        if (ibv_modify_qp(peer->qp,
                          &rts,
                          IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                          IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
            return Status::kInternalError;
        }

        return Status::kOk;
    }

    PeerConnection* GetPeer(NodeId node) {
        if (node >= peers_.size()) return nullptr;
        return &peers_[node];
    }

    Status CopyLocal(std::uint64_t offset, void* buf, std::size_t size, bool write) {
        if (local_base_ == nullptr || offset + size > local_length_) return Status::kOutOfRange;
        std::uint8_t* base = static_cast<std::uint8_t*>(local_base_);
        if (write) {
            std::memcpy(base + offset, buf, size);
        } else {
            std::memcpy(buf, base + offset, size);
        }
        return Status::kOk;
    }

    Status PostOneSided(PeerConnection* peer,
                        ibv_wr_opcode opcode,
                        void* local_addr,
                        std::size_t size,
                        std::uint32_t lkey,
                        std::uint64_t remote_addr,
                        std::uint32_t rkey) {
        ibv_sge sge{};
        sge.addr = reinterpret_cast<std::uintptr_t>(local_addr);
        sge.length = static_cast<std::uint32_t>(size);
        sge.lkey = lkey;

        ibv_send_wr wr{};
        wr.opcode = opcode;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;
        if (opcode == IBV_WR_RDMA_WRITE && size <= cfg_.rdma_max_inline_bytes) {
            wr.send_flags |= IBV_SEND_INLINE;
        }
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey = rkey;

        ibv_send_wr* bad = nullptr;
        if (ibv_post_send(peer->qp, &wr, &bad) != 0) return Status::kInternalError;
        ++outstanding_sends_;
        return WaitForCompletions(1);
    }

    Status WaitForCompletions(std::size_t target) {
        if (target == 0) return Status::kOk;
        std::size_t completed = 0;
        while (completed < target) {
            std::size_t polled = 0;
            Status s = PollCompletions(target - completed, &polled);
            if (s != Status::kOk) return s;
            if (polled == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            completed += polled;
        }
        return Status::kOk;
    }

    Config cfg_;
    bool initialized_ = false;
    ibv_context* context_ = nullptr;
    ibv_pd* pd_ = nullptr;
    ibv_cq* cq_ = nullptr;
    ibv_mr* local_mr_ = nullptr;
    ibv_port_attr port_attr_{};
    ibv_gid gid_{};
    void* local_base_ = nullptr;
    std::size_t local_length_ = 0;
    std::size_t outstanding_sends_ = 0;
    std::uint64_t control_sequence_ = 0;
    std::vector<PeerConnection> peers_;
};

#else

class RdmaTransport final : public Transport {
public:
    explicit RdmaTransport(const Config& cfg) : cfg_(cfg) {}

    Status Init() override { return Status::kUnimplemented; }
    Status Shutdown() override { return Status::kOk; }
    Status Read(GlobalAddr, void*, std::size_t) override { return Status::kUnimplemented; }
    Status Write(GlobalAddr, const void*, std::size_t) override { return Status::kUnimplemented; }
    Status BatchWrite(const std::vector<RemoteWriteRequest>&) override { return Status::kUnimplemented; }
    Status Fence() override { return Status::kUnimplemented; }
    Status PollCompletions(std::size_t, std::size_t* completed) override {
        if (completed != nullptr) *completed = 0;
        return Status::kUnimplemented;
    }
    TransportCapabilities GetCapabilities() const override { return {}; }
    Status RegisterMemoryRegion(void*, std::size_t, RdmaMemoryRegion*) override {
        return Status::kUnimplemented;
    }
    Status PostControlMessage(const ControlMessage&) override {
        return Status::kUnimplemented;
    }
    Status DrainControlMessages(std::size_t, std::vector<ControlMessage>*) override {
        return Status::kUnimplemented;
    }

private:
    Config cfg_;
};

#endif

}  // namespace

std::unique_ptr<Transport> CreateRdmaTransport(const Config& cfg) {
    return std::make_unique<RdmaTransport>(cfg);
}

}  // namespace leomem
