#include "game.hpp"
#include "impl.hpp"
#include "session.cuh"
#include <>
#include <algorithm>
#include <cmath>
#include <cuda/std/array>
#include <curand_kernel.h>

using namespace snakeio;
using namespace snakeio::gpu;

namespace {
    __global__ void init_curand_(curand_state* state, unsigned int seed) {
        curand_init(seed, threadIdx.x, 0, state + threadIdx.x);
    }
}

void gpu::init_curand(curand_state* state, std::random_device::result_type seed) {
    init_curand_<<<1, curand_state_size>>>(state, seed);
}

namespace {
    // blocks: 1
    // threads: players + 1
    __global__ void add_session_basic(curand_state* state, session* sessions, id_t session_id,
        id_t human_players, id_t ai_players, tick_t max_tick) noexcept {
        session& session = sessions[session_id];
        const scalar_t width = game_width_psqp * sqrt(static_cast<scalar_t>(session.players));
        const scalar_t height = game_height_psqp * sqrt(static_cast<scalar_t>(session.players));
        if (threadIdx.x == 0) {
            session.players = human_players + ai_players;
            session.human_players = human_players;
            session.max_tick = max_tick;
            session.width = width;
            session.height = height;
            session.sets.segment_size = snake_init_length * session.players;
            session.sets.food_size = game_init_food_pp * session.players;
        } else {
            const auto idx = threadIdx.x - 1;
            auto& snakes = session.snakes;
            auto& sets = session.sets;
            curand_state* snake_state = state + idx;
            snakes.speed[idx] = snake_init_speed;
            snakes.angle[idx] = curand_uniform(snake_state) * 2*M_PI;
            snakes.frac_length[idx] = snake_init_length;
            snakes.score[idx] = 0;
            snakes.boost[idx] = 0;
            snakes.status[idx] = snake_status_t::alive;
            snakes.human[idx] = idx < human_players;
            snakes.length[idx] = snake_init_length;
            sets.segment_idx[idx] = {.player_id = idx, .segment_id = 0};
            sets.segment_pos[idx] = {
                curand_uniform(snake_state) * width,
                curand_uniform(snake_state) * height,
            };
        }
    }
    // blocks: players
    // threads: snake_init_length
    __global__ void add_session_snake(session* sessions, id_t session_id) noexcept {
        session& session = sessions[session_id];
        const auto& snakes = session.snakes;
        auto& sets = session.sets;
        const snakeio::size_t idx = blockIdx.x * blockDim.x + threadIdx.x + session.players;
        sets.segment_idx[idx] = {.player_id = blockIdx.x, .segment_id = threadIdx.x + 1};
        sets.segment_pos[idx] = sets.segment_pos[blockIdx.x] + vector2d{
            __cosf(snakes.angle[blockIdx.x]),
            __sinf(snakes.angle[blockIdx.x])
        } * snakes.speed[blockIdx.x] * (threadIdx.x + 1);
    }
    // blocks: 1
    // threads: session.sets.food_size
    __global__ void add_session_food(curand_state* state, session* sessions, id_t session_id) noexcept {
        sta
    }
}
void gpu::add_session(curand_state* state, session* sessions, id_t session_id,
    id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    std::array<key_t, game_max_players> keys_buf;
    const auto res = std::ranges::copy(keys, keys_buf.begin());
    std::ranges::fill(res.out, keys_buf.end(), key_t());
    add_session_basic<<<1, 1>>>(state, sessions, session_id, human_players, ai_players, max_tick);
}