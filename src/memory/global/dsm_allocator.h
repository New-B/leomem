#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "leomem/addr.h"
#include "leomem/config.h"
#include "leomem/status.h"

namespace leomem {

class DsmAllocator {
public:
    explicit DsmAllocator(const Config& cfg);

    Status Init();
    Status Shutdown();

    GlobalAddr Allocate(std::size_t size);
    Status Free(GlobalAddr addr);

    void* TranslateLocal(GlobalAddr addr, std::size_t size);

private:
    Config cfg_;
    std::vector<std::uint8_t> region_;
    std::size_t next_offset_ = 0;
    std::mutex mu_;
};

}  // namespace leomem