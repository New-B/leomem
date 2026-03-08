#include "runtime/init.h"

#include "runtime/config_loader.h"
#include "runtime/context.h"

namespace leomem {

Status RuntimeInit(const char* config_path) {
    Config cfg;
    if (config_path != nullptr) {
        Status s = LoadConfigFile(config_path, &cfg);
        if (s != Status::kOk) return s;
    }
    return RuntimeContext::Instance().Init(cfg);
}

Status RuntimeShutdown() {
    return RuntimeContext::Instance().Shutdown();
}

}  // namespace leomem