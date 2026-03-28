#pragma once
#include <config.hpp>
#include <snake_status.hpp>
#include <vector.hpp>
#include <cuda/std/array>
#include <cuda/std/atomic>

namespace snakeio::gpu {
    struct session {
        id_t players, human_players;
        cuda::atomic<tick_t> tick = -1; // -1: inactive
        tick_t max_tick;
        scalar_t width, height;
        struct {
            cuda::std::array<scalar_t, game_max_players> speed, angle, width, frac_length;
            cuda::std::array<score_t, game_max_players> score;
            cuda::std::array<boost_t, game_max_players> boost;
            cuda::std::array<snake_status_t, game_max_players> status;
            cuda::std::array<unsigned char, game_max_players> status_data;
            cuda::std::array<bool, game_max_players> human;
            cuda::std::array<size_t, game_max_players> length;
        } snakes;
        struct {
            struct snake_segment_index {
                id_t player_id;
                size_t segment_id;
            };
            size_t segment_size, food_size;
            cuda::std::array<snake_segment_index, game_max_players * snake_max_length> segment_idx;
            cuda::std::array<vector2d, game_max_players * snake_max_length> segment_pos;
            cuda::std::array<vector2d, game_max_food> food;
        } sets;
    };
}
