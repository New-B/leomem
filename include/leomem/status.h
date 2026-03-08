#pragma once

namespace leomem {

enum class Status {
    kOk = 0,
    kInvalidArg,
    kNotInitialized,
    kAlreadyInitialized,
    kNoMemory,
    kNotFound,
    kOutOfRange,
    kUnimplemented,
    kInternalError,
};

const char* StatusToString(Status s);

}  // namespace leomem