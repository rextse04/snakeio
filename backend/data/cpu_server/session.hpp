#pragma once
#include <vector.hpp>
#include <utils.hpp>
#include "spatial_set.hpp"
#include "snake.hpp"
#include "food.hpp"
#include <tuple>
#include <atomic>

namespace snakeio::cpu {
    struct session {
        static constexpr vector2d game_dim{game_max_width, game_max_height};
        using snakes_set_type = spatial_set<game_dim, snake_max_width * 2, snake_max_length * game_max_players,
            std::tuple<snake*, vector2d*>,
            [](const auto& node) { return *std::get<1>(node); },
            [](auto& node, const vector2d& pos) { std::get<1>(node) = const_cast<vector2d*>(&pos); }>;
        using food_set_type = spatial_set<game_dim, snake_max_width + food_max_width, game_max_food,
            food,
            [](const food& node) { return node.pos; },
            [](food& node, const vector2d& value) { node.pos = value; }>;
        id_t players, human_players;
        atomic_align(tick_t) tick;
        tick_t max_tick;
        scalar_t width, height;
        std::array<snake, game_max_players> snakes;
        snakes_set_type snakes_set;
        food_set_type food_set;

        constexpr auto snakes_view(this auto&& self) noexcept {
            return std::span(self.snakes.begin(), self.players);
        }
        constexpr auto human_snakes_view(this auto&& self) noexcept {
            return std::span(self.snakes.begin(), self.human_players);
        }
        constexpr auto ai_snakes_view(this auto&& self) noexcept {
            return std::span(self.snakes.begin() + self.human_players, self.snakes.begin() + self.players);
        }
        // UB if snake.basic.length() < 2.
        void add_segments(snake& snake, scalar_t new_length) noexcept;
    };
}