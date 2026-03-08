#pragma once

#include "leomem/status.h"

namespace leomem {

Status RuntimeInit(const char* config_path);
Status RuntimeShutdown();

}  // namespace leomem