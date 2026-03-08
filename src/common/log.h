#pragma once

#include <cstdio>

namespace leomem {

enum class LogLevel {
    kDebug = 0,
    kInfo,
    kWarn,
    kError,
};

void LogMessage(LogLevel level, const char* file, int line, const char* fmt, ...);

}  // namespace leomem

#define LM_LOG_DEBUG(fmt, ...) ::leomem::LogMessage(::leomem::LogLevel::kDebug, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LM_LOG_INFO(fmt, ...)  ::leomem::LogMessage(::leomem::LogLevel::kInfo,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LM_LOG_WARN(fmt, ...)  ::leomem::LogMessage(::leomem::LogLevel::kWarn,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LM_LOG_ERROR(fmt, ...) ::leomem::LogMessage(::leomem::LogLevel::kError, __FILE__, __LINE__, fmt, ##__VA_ARGS__)