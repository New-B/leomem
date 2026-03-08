#include "common/utils.h"

namespace leomem {

std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    if (alignment == 0) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

}  // namespace leomem