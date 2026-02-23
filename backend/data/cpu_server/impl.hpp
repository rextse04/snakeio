#pragma once
#include <config.hpp>
#include <game.hpp>
#include <snake.hpp>
#include <vector.hpp>
#include <network.hpp>
#include "spatial_set.hpp"
#include <array>
#include <tuple>

namespace snakeio {
    struct game::impl {
        struct in_packet {
            bool valid = false;
            bool snapshot_requested;
            scalar_t angle;
        };

        struct client {
            key_t key;
            sockaddr_storage addr;
            in_packet last_packet;
        };

        struct session {
            id_t players;
            tick_t tick, max_tick;
            scalar_t width, height;
            std::array<snake, game_max_players> snakes;
            cpu::spatial_set<snake_max_width, snake_max_length * game_max_players,
                std::tuple<snake*, vector2d*>, [](const auto& node) { return *std::get<1>(node); }> snakes_set;
            cpu::spatial_set<(snake_max_width + food_max_width) / 2, game_max_food,
                food, [](const food& node) { return node.pos; }> food_set;
        };

        std::array<std::array<client, game_max_players>, game_max_sessions> clients;
        std::array<session, game_max_sessions> sessions;

        static void erase_snake(game& game, const session& session,
            snake &snake, size_t &erase_count, session_delta &delta) noexcept;
        void port(game& game, std::stop_token stop_token, int sock) noexcept;
        void game_loop(game& game, std::stop_token stop_token, int sock) noexcept;
    };
}
