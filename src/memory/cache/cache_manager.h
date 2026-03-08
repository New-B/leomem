#pragma once

#include "leomem/config.h"
#include "leomem/status.h"

namespace leomem {

class CacheManager {
public:
    explicit CacheManager(const Config& cfg) : cfg_(cfg) {}

    Status Init() { return Status::kOk; }
    Status Shutdown() { return Status::kOk; }

private:
    Config cfg_;
};

}  // namespace leomem