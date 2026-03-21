#pragma once
#include <config.hpp>
#include <vector.hpp>

namespace snakeio::cpu {
    struct food {
        vector2d pos;
        scalar_t width;
    };
}