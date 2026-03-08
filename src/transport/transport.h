#pragma once

#include <cstddef>
#include <memory>

#include "leomem/addr.h"
#include "leomem/config.h"
#include "leomem/status.h"

namespace leomem {

class Transport {
public:
    virtual ~Transport() = default;

    virtual Status Init() = 0;
    virtual Status Shutdown() = 0;

    virtual Status Read(GlobalAddr addr, void* buf, std::size_t size) = 0;
    virtual Status Write(GlobalAddr addr, const void* buf, std::size_t size) = 0;
    virtual Status Fence() = 0;
};

std::unique_ptr<Transport> CreateTransport(const Config& cfg);

}  // namespace leomem