#pragma once

#include <cstddef>
#include <limits>

namespace vnm::plot::detail {

inline bool checked_size_add(std::size_t lhs, std::size_t rhs, std::size_t& out)
{
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

inline bool checked_size_product(std::size_t lhs, std::size_t rhs, std::size_t& out)
{
    if (rhs != 0 && lhs > std::numeric_limits<std::size_t>::max() / rhs) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

} // namespace vnm::plot::detail
