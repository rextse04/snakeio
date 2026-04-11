#pragma once
#include <vector.hpp>
#include <ostream>

namespace snakeio {
    inline std::ostream& operator<<(std::ostream& os, const vector2d& v) {
        os << '(' << v[0] << ',' << v[1] << ')';
        return os;
    }
}