#pragma once
#include "cpp_utils/integer.hpp"
#include <chrono>
#include <array>

namespace snakeio {
    using namespace std::chrono_literals;

    using size_t = unsigned;
    using id_t = std::uint_least32_t;
    using scalar_t = float; // 4 bytes
    using score_t = utils::integer<std::uint32_t, utils::integral_behavior::sat>;
    using key_t = std::array<std::byte, 32>;
    using tick_t = std::uint_least32_t;

    constexpr id_t game_max_sessions = 2 << 13;
    constexpr id_t game_max_players = 4*4;
    constexpr scalar_t game_width_psqp = 512, game_height_psqp = 256;
    constexpr scalar_t game_max_width = game_width_psqp * 4, game_max_height = game_height_psqp * 4;
    constexpr scalar_t game_max_area = game_width_psqp * game_height_psqp * game_max_players;
    constexpr size_t game_init_food_pp = 32;
    constexpr size_t game_max_food_pp = 128, game_max_food = game_max_food_pp * game_max_players;
    constexpr auto game_tick_rate = 20ms;
    constexpr tick_t game_max_tick = 300s / game_tick_rate;

    constexpr scalar_t snake_init_speed = 5, snake_init_width = 8;
    constexpr scalar_t snake_max_width = snake_init_width;
    constexpr size_t snake_init_length = 10;
    constexpr size_t snake_max_length = 1024;

    constexpr scalar_t food_max_width = 10;
    constexpr scalar_t seg_to_food_width = 8;
    static_assert(seg_to_food_width <= food_max_width);
    constexpr scalar_t seg_to_food_prob = 0.35;
    constexpr size_t food_per_player_tick = 1;

    constexpr size_t data_packet_align = 64;
    constexpr size_t in_packet_max_size = 128;
}