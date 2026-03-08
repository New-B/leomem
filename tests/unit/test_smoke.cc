#include <cassert>
#include <cstdint>
#include <cstdio>

#include "leomem/leomem.h"

int main() {
    using namespace leomem;

    Status s = lm_init();
    assert(s == Status::kOk);

    GlobalAddr addr = lm_malloc(sizeof(std::uint64_t));
    assert(addr.IsValid());

    std::uint64_t x = 42;
    s = lm_write(addr, &x, sizeof(x));
    assert(s == Status::kOk);

    std::uint64_t y = 0;
    s = lm_read(addr, &y, sizeof(y));
    assert(s == Status::kOk);
    assert(y == 42);

    StatsSnapshot st = lm_get_stats();
    assert(st.alloc_ops == 1);
    assert(st.write_ops == 1);
    assert(st.read_ops == 1);

    s = lm_shutdown();
    assert(s == Status::kOk);

    std::printf("smoke test passed\n");
    return 0;
}