#pragma once

#include <cstddef>
#include "leomem/addr.h"
#include "leomem/config.h"
#include "leomem/stats.h"
#include "leomem/status.h"

namespace leomem {

Status lm_init(const char* config_path = nullptr);
Status lm_shutdown();

GlobalAddr lm_malloc(std::size_t size);
Status lm_free(GlobalAddr addr);

Status lm_read(GlobalAddr addr, void* buf, std::size_t size);
Status lm_write(GlobalAddr addr, const void* buf, std::size_t size);

Status lm_fence();
Status lm_barrier();

StatsSnapshot lm_get_stats();

}  // namespace leomem