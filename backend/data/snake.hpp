#pragma once
#include "config.hpp"
#include "vector.hpp"
#include <array>
#include <algorithm>

namespace snakeio {
    struct snake_basic {
        scalar_t speed, angle;
        scalar_t width;
        size_t length;
        score_t score;
        bool alive, human;
    };
    struct snake {
        snake_basic basic;
        std::array<vector2d, snake_max_length + 1> segments;
    };
}