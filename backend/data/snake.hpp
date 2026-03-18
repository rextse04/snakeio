#pragma once
#include "config.hpp"
#include "vector.hpp"
#include <array>
#include <span>

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
        std::array<vector2d, snake_max_length> segments;

        constexpr auto segments_view(this auto&& self) noexcept {
            return std::span(self.segments.begin(), self.basic.length);
        }
    };
}