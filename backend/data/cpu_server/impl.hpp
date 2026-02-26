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
            tick_t tick;
            bool snapshot_requested;
            scalar_t angle;
        };
        struct out_delta {
            size_t foods_added_size, foods_removed_size;
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
            atomic_align(in_packet) last_packet;
        };

        struct session {
            id_t players;
            atomic_align(tick_t) tick;
            tick_t max_tick;
            scalar_t width, height;
            std::array<snake, game_max_players> snakes;
            cpu::spatial_set<snake_max_width, snake_max_length * game_max_players,
                std::tuple<snake*, vector2d*>, [](const auto& node) { return *std::get<1>(node); }> snakes_set;
            cpu::spatial_set<(snake_max_width + food_max_width) / 2, game_max_food,
                food, [](const food& node) { return node.pos; }> food_set;

            constexpr auto snakes_view(this auto&& self) noexcept {
                return std::span(self.snakes.begin(), self.players);
            }
        };

        std::array<std::array<client, game_max_players>, game_max_sessions> clients;
        std::array<session, game_max_sessions> sessions;

        static void erase_snake(game& game, const session& session,
            snake& snake, size_t& erase_count, out_delta& delta) noexcept;
        static size_t store_delta(std::byte* out, const session& session, out_delta& delta) noexcept;
        static size_t store_snapshot(std::byte* out, const session& session) noexcept;
        static size_t store_lobby_status(std::byte *out, tick_t tick,
            const std::array<in_packet, 16>& in_packets, const session& session) noexcept;
        static void port(game& game, std::stop_token stop_token, int sock) noexcept;
        static void game_loop(game& game, std::stop_token stop_token, int sock) noexcept;
    };
}
