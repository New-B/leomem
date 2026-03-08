#include "common/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace leomem {

static const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo:  return "INFO";
        case LogLevel::kWarn:  return "WARN";
        case LogLevel::kError: return "ERROR";
        default: return "UNKNOWN";
    }
}

void LogMessage(LogLevel level, const char* file, int line, const char* fmt, ...) {
    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif

    std::fprintf(stderr, "[%02d:%02d:%02d][%s][%s:%d] ",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 ToString(level), file, line);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    std::fprintf(stderr, "\n");
}

}  // namespace leomem