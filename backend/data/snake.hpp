#pragma once
#include "config.hpp"
#include "vector.hpp"
#include <array>
#include <span>

namespace snakeio {
    enum class snake_status_t : unsigned char {
        alive, killed_by_snake, killed_by_wall
    };
    struct snake_status {
        snake_status_t status;
        unsigned char data;
    };
    struct snake_basic {
        scalar_t speed, angle;
        scalar_t width;
        scalar_t frac_length;
        score_t score;
        boost_t boost;
        snake_status status;
        bool human;

        constexpr size_t length() const noexcept {
            return static_cast<size_t>(frac_length);
        }
        constexpr bool alive() const noexcept {
            return status.status == snake_status_t::alive;
        }
        constexpr void set_alive() noexcept {
            status.status = snake_status_t::alive;
        }
    };
    struct snake: snake_basic {
        std::array<vector2d, snake_max_length> segments;

        constexpr auto segments_view(this auto&& self) noexcept {
            return std::span(self.segments.begin(), self.length());
        }
    };
}