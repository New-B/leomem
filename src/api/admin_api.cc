#include "leomem/leomem.h"

#include "runtime/context.h"
#include "runtime/init.h"
#include "stats/counters.h"

namespace leomem {

Status lm_init(const char* config_path) {
    return RuntimeInit(config_path);
}

Status lm_shutdown() {
    return RuntimeShutdown();
}

StatsSnapshot lm_get_stats() {
    auto& ctx = RuntimeContext::Instance();
    if (!ctx.IsInitialized()) return {};
    return ctx.stats()->Snapshot();
}

const char* StatusToString(Status s) {
    switch (s) {
        case Status::kOk: return "OK";
        case Status::kInvalidArg: return "InvalidArg";
        case Status::kNotInitialized: return "NotInitialized";
        case Status::kAlreadyInitialized: return "AlreadyInitialized";
        case Status::kNoMemory: return "NoMemory";
        case Status::kNotFound: return "NotFound";
        case Status::kOutOfRange: return "OutOfRange";
        case Status::kUnimplemented: return "Unimplemented";
        case Status::kInternalError: return "InternalError";
        default: return "Unknown";
    }
}

}  // namespace leomem