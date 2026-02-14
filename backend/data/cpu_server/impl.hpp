#pragma once
#include <game.hpp>
#include "spatial_set.hpp"
#include <array>
#include <tuple>

namespace snakeio {
    struct game::impl {
        struct session : utils::implements<game_session_view_interface> {
            id_t players;
            scalar_t width, height;
            std::array<snake, game_max_players> snakes;
            cpu::spatial_set<snake_max_width, snake_max_length * game_max_players,
                std::tuple<snake*, vector2d*>, [](const auto& node) { return *std::get<1>(node); }> snakes_set;
            cpu::spatial_set<(snake_max_width + food_max_width) / 2, game_max_food,
                food, [](const food& node) { return node.pos; }> food_set;

            constexpr session() noexcept = default;
            constexpr session(const game_session& session) noexcept :
                players(session.players),
                width(session.width),
                height(session.height) {
                std::ranges::copy_n(session.snakes.begin(), session.players, snakes.begin());
                for (snake& snake : std::span(snakes.data(), players)) {
                    for (vector2d& seg : std::span(snake.segments.data(), snake.length)) {
                        snakes_set.emplace(&snake, &seg);
                    }
                }
                food_set.insert(std::span(session.foods.begin(), session.foods_size));
            }

            UTILS_DYN
            game_session_view_interface {
                .players = [](utils::const_obj_ptr self) {
                    return static_cast<const session*>(self)->players;
                },
                .width = [](utils::const_obj_ptr self) {
                    return static_cast<const session*>(self)->width;
                },
                .height = [](utils::const_obj_ptr self) {
                    return static_cast<const session*>(self)->height;
                },
                .snakes = [](utils::const_obj_ptr self) {
                    const auto& session_ = *static_cast<const session*>(self);
                    return std::span(session_.snakes.begin(), session_.players);
                },
                .foods = [](utils::const_obj_ptr self) {
                    const auto& food_set = static_cast<const session*>(self)->food_set;
                    return std::span(food_set.begin(), food_set.end());
                }
            }
            UTILS_DYN_END
        };

        std::array<std::array<key_t, game_max_players>, game_max_sessions> keys;
        std::array<session, game_max_sessions> sessions;
    };
}