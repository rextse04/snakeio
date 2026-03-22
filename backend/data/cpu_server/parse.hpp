#pragma once
#include <config.hpp>
#include <network.hpp>
#include <utils.hpp>
#include "food.hpp"
#include "session.hpp"
#include <atomic>
#include <span>

namespace snakeio::cpu {
    struct in_packet {
        sockaddr_storage addr;
        bool snapshot_requested, boost;
        scalar_t angle;
    };
    struct in_packet_info : in_packet {
        tick_t tick;
    };

    struct client {
        key_t key;
        atomic_align(tick_t) tick;
        in_packet last_packet;
    };

    struct out_delta {
        size_t foods_added_size = 0, foods_removed_size = 0;
        std::array<cpu::food, game_max_food> foods_added;
        std::array<vector2d, game_max_food> foods_removed;

        constexpr auto foods_added_view(this auto&& self) noexcept {
            return std::span(self.foods_added.begin(), self.foods_added_size);
        }
        constexpr auto foods_removed_view(this auto&& self) noexcept {
            return std::span(self.foods_removed.begin(), self.foods_removed_size);
        }
    };

    size_t store_snake_basic(std::byte* out, const snake_basic& snake) noexcept;
    size_t store_snake(std::byte* out, const snake& snake) noexcept;
    size_t store_food(std::byte* out, const food& food) noexcept;

    size_t store_delta(std::byte* out, const session& session, out_delta& delta) noexcept;
    size_t store_snapshot(std::byte* out, const session& session) noexcept;
    size_t store_lobby_status(std::byte* out, std::span<const in_packet_info> in_packets) noexcept;
    size_t store_termination(std::byte* out, const session& session) noexcept;
}