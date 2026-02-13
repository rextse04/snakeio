#pragma once
#include "config.hpp"
#include "vector.hpp"
#include <array>

namespace snakeio {
    struct snake {
        scalar_t speed, angle;
        scalar_t width;
        size_t length;
        std::array<vector2d, snake_max_length> segments;
        score_t score;
        bool alive, human;
    };

    struct snake_delta {
        scalar_t angle;
    };
}