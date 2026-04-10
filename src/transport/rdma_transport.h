#pragma once

#include <memory>

#include "transport/transport.h"

namespace leomem {

std::unique_ptr<Transport> CreateRdmaTransport(const Config& cfg);

}  // namespace leomem
