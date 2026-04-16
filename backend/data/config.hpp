#pragma once
#include <cstdint>
#include <limits>
#include <cmath>
#include <chrono>
#include <algorithm>

#ifdef __CUDACC__
#include <cuda/std/array>
#else
#include <array>
#endif
#include <compatibility.hpp>

namespace snakeio {
    using namespace std::chrono_literals;

    using size_t = unsigned;
    using id_t = std::uint_least32_t;
    using scalar_t = float;
    static_assert(std::numeric_limits<scalar_t>::has_quiet_NaN);
    using score_t = std::uint_least32_t;
    using key_t = std::array<std::byte, 32>;
    using tick_t = std::uint_least32_t;
    using boost_t = unsigned char;
    static_assert(std::numeric_limits<boost_t>::max() <= std::numeric_limits<tick_t>::max());

    constexpr id_t game_max_sessions = 1 << 12;
    constexpr id_t game_max_players_sqrt = 4, game_max_players = game_max_players_sqrt * game_max_players_sqrt;
    constexpr scalar_t game_width_psqp = 2048, game_height_psqp = 1024;
    constexpr scalar_t game_max_width = game_width_psqp * game_max_players_sqrt,
        game_max_height = game_height_psqp * game_max_players_sqrt;
    constexpr size_t game_init_food_pp = 32;
    constexpr size_t game_max_food_pp = 128, game_max_food = game_max_food_pp * game_max_players;
    constexpr auto game_tick_rate = 20ms;
    constexpr tick_t game_max_tick = 300s / game_tick_rate;
    constexpr scalar_t game_collision_eps = scalar_t(1)/1024;

    constexpr scalar_t snake_init_speed = 6, snake_init_width = 8;
    constexpr scalar_t snake_min_speed = snake_init_speed / 2, snake_max_width = snake_init_width * 6;
    constexpr size_t snake_init_length = 10, snake_max_length = 1024;
    constexpr scalar_t snake_max_turn_per_tick = M_PI / 32;
    constexpr boost_t snake_seg_to_boost_ticks = 2;
    constexpr scalar_t snake_boost_speed_factor = 2;

    constexpr scalar_t gen_food_min_width = 1, gen_food_max_width = 5;
    constexpr scalar_t seg_to_food_prob = 0.25;
    constexpr scalar_t seg_food_min_width = 6, seg_food_max_width = 10;
    constexpr scalar_t food_max_width = std::max(gen_food_max_width, seg_food_max_width);
    constexpr scalar_t food_width_to_seg = 0.5;
    constexpr float food_per_player_tick = 1./64; // expected value

    constexpr std::uint_least16_t control_plane_ext_port = 50000,
        control_plane_int_port = 50001,
        data_plane_int_port = 50002,
        data_plane_ext_port = 50003;
    constexpr size_t data_packet_align = 16;
    constexpr size_t align(size_t text_size) noexcept {
        return data_packet_align * (text_size / data_packet_align + (text_size % data_packet_align != 0));
    }
    constexpr size_t in_packet_max_text_size = align(8),
        delta_packet_max_text_size = align(24 * game_max_players + 4 + 12 * game_max_food + 4 + 8 * game_max_food),
        snapshot_packet_max_text_size = align(4 + 4 + 4 + game_max_players * (24 + 8 * snake_max_length) + 4 + 12 * game_max_food),
        lobby_status_max_text_size = align(game_max_players),
        termination_max_text_size = align(24 * game_max_players),
        out_packet_max_text_size = std::max({delta_packet_max_text_size, snapshot_packet_max_text_size, lobby_status_max_text_size, termination_max_text_size});
    constexpr size_t packet_chunk_size = 1024;
}