#pragma once
#include <config.hpp>
#include <snake_status.hpp>
#include <vector.hpp>
#include "spatial_set.cuh"
#include <cuda_runtime.h>

namespace snakeio::gpu {
    struct session_batch {
        struct sessions_type {
            template <typename T>
            using per_session_t = T[game_max_sessions];
            template <typename T>
            using per_snake_t = T[game_max_sessions][game_max_players];

            per_session_t<bool> active;
            per_session_t<id_t> players, human_players;
            per_session_t<tick_t> tick, max_tick;
            per_session_t<scalar_t> game_width, game_height;

            per_snake_t<scalar_t> speed, angle, width, frac_length;
            per_snake_t<score_t> score;
            per_snake_t<boost_t> boost;
            per_snake_t<snake_status_t> status;
            per_snake_t<unsigned char> status_data;
            per_snake_t<bool> human;
            per_snake_t<size_t> length;
        } *sessions;
        struct snake_segment_index {
            vector2d pos;
            id_t player_id;
            size_t segment_id;
        };
    private:
        __device__ static vector2d segment_set_getter(const snake_segment_index& index) noexcept {
            return index.pos;
        }
        __device__ static void segment_set_setter(snake_segment_index& index, vector2d key) noexcept {
            index.pos = key;
        }
    public:
        spatial_set_batch<game_max_width, game_max_height, snake_max_width * 2,
            snake_max_length * game_max_players, game_max_sessions,
            snake_segment_index, segment_set_getter, segment_set_setter> segment_set;
        spatial_set_batch<game_max_width, game_max_height, snake_max_width + food_max_width,
            game_max_food, game_max_sessions> food_set;

        session_batch() {
            cudaMalloc(&sessions, sizeof(sessions_type));
        }
        void destroy() {
            cudaFree(sessions);
            segment_set.destroy();
            food_set.destroy();
        }
    };

    struct add_session_req {
        id_t human_players;
        id_t ai_players;
        tick_t max_tick;
        key_t keys[game_max_players];
    };
}
