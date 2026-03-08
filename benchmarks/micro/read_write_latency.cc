#include <chrono>
#include <cstdint>
#include <cstdio>

#include "leomem/leomem.h"

int main() {
    using namespace leomem;

    if (lm_init() != Status::kOk) {
        std::fprintf(stderr, "lm_init failed\n");
        return 1;
    }

    constexpr std::size_t kIters = 100000;
    GlobalAddr addr = lm_malloc(sizeof(std::uint64_t));
    if (!addr.IsValid()) return 1;

    std::uint64_t val = 1;
    auto t1 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIters; ++i) {
        lm_write(addr, &val, sizeof(val));
    }
    auto t2 = std::chrono::steady_clock::now();

    std::uint64_t out = 0;
    auto t3 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIters; ++i) {
        lm_read(addr, &out, sizeof(out));
    }
    auto t4 = std::chrono::steady_clock::now();

    auto write_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    auto read_ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(t4 - t3).count();

    std::printf("write avg: %.2f ns\n", static_cast<double>(write_ns) / kIters);
    std::printf("read  avg: %.2f ns\n", static_cast<double>(read_ns) / kIters);

    lm_shutdown();
    return 0;
}