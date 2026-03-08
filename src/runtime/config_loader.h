#pragma once

#include <string>
#include "leomem/config.h"
#include "leomem/status.h"

namespace leomem {

Status LoadConfigFile(const std::string& path, Config* cfg);

}  // namespace leomem