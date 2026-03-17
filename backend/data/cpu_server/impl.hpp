#pragma once
#include <config.hpp>
#include <game.hpp>
#include <snake.hpp>
#include <food.hpp>
#include <vector.hpp>
#include <network.hpp>
#include "spatial_set.hpp"
#include <array>
#include <tuple>
#include <span>
#include <atomic>

#define atomic_align(type) alignas(std::atomic_ref<type>::required_alignment) type

namespace snakeio {
    struct game::impl {
        struct in_packet {
            sockaddr_storage addr;
            bool snapshot_requested;
            scalar_t angle;
        };
        struct in_packet_info : in_packet {
            tick_t tick;
        };
        struct out_delta {
            size_t foods_added_size = 0, foods_removed_size = 0;
            std::array<food, game_max_food> foods_added;
            std::array<vector2d, game_max_food> foods_removed;

            constexpr auto foods_added_view(this auto&& self) noexcept {
                return std::span(self.foods_added.begin(), self.foods_added_size);
            }
            constexpr auto foods_removed_view(this auto&& self) noexcept {
                return std::span(self.foods_removed.begin(), self.foods_removed_size);
            }
        };

        struct client {
            key_t key;
            atomic_align(tick_t) tick;
            in_packet last_packet;
        };

        struct session {
            using snakes_set_type = cpu::spatial_set<snake_max_width * 2, snake_max_length * game_max_players,
                std::tuple<snake*, vector2d*>,
                [](const auto& node) { return *std::get<1>(node); },
                [](auto& node, const vector2d& pos) { std::get<1>(node) = const_cast<vector2d*>(&pos); }>;
            using food_set_type = cpu::spatial_set<snake_max_width + food_max_width, game_max_food,
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
            constexpr void add_segment(snake& snake, vector2d pos) noexcept {
                snake.segments[snake.basic.length] = pos;
                snakes_set.emplace(&snake, snake.segments.data() + snake.basic.length);
                ++snake.basic.length;
            }
            // UB if snake.basic.length < 2.
            constexpr void add_segment(snake& snake) noexcept;
        };

        std::array<std::array<client, game_max_players>, game_max_sessions> clients;
        std::array<session, game_max_sessions> sessions;

        static size_t store_delta(std::byte* out, const session& session, out_delta& delta) noexcept;
        static size_t store_snapshot(std::byte* out, const session& session) noexcept;
        static size_t store_lobby_status(std::byte* out, std::span<const in_packet_info> in_packets) noexcept;
        static size_t store_termination(std::byte* out, const session& session) noexcept;
        static void port(game& game, std::stop_token stop_token, int sock) noexcept;
        static void game_loop(game& game, std::stop_token stop_token, int sock) noexcept;
    };
}
