#include "game.hpp"
#include "impl.hpp"
#include "session.cuh"
#include <random>
#include <cmath>
#include <algorithm>
#include <cuda_runtime.h>
#include <cuda/std/array>
#include <curand_kernel.h>

using namespace snakeio;
using namespace snakeio::gpu;

using curand_state = curandStatePhilox4_32_10;

namespace {
    // blocks: 1
    // threads: curand_states_size
    __global__ void init_curand(curand_state* states, std::random_device::result_type seed) {
        curand_init(seed, threadIdx.x, 0, states + threadIdx.x);
    }
}

void gpu::init(game::impl& impl) noexcept {
    impl.cuda_streams = new cudaStream_t[cuda_streams_size];
    for (std::size_t i = 0; i < cuda_streams_size; ++i) {
        cudaStreamCreate(static_cast<cudaStream_t*>(impl.cuda_streams) + i);
    }
    impl.cuda_events = new cudaEvent_t[cuda_events_size];
    for (std::size_t i = 0; i < cuda_events_size; ++i) {
        cudaEventCreate(static_cast<cudaEvent_t*>(impl.cuda_events) + i);
    }
    impl.curand_states = new curand_state[curand_states_size];
    init_curand<<<1, curand_states_size>>>(static_cast<curand_state*>(impl.curand_states), std::random_device()());
}

namespace {
    // blocks: 1
    // threads: players + 1
    __global__ void add_session_basic(curand_state* states, session* sessions, id_t session_id,
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
            curand_state* snake_state = states + idx;
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
        vector2d& pos = sets.segment_pos[idx];
        __sincosf(snakes.angle[blockIdx.x], &pos[1], &pos[0]);
        pos = pos * snakes.speed[blockIdx.x] * (threadIdx.x + 1) + sets.segment_pos[blockIdx.x];
    }
    // blocks: 1
    // threads: session.sets.food_size
    __global__ void add_session_food(curand_state* states, session* sessions, id_t session_id) noexcept {
        session& session = sessions[session_id];
        session.sets.food[threadIdx.x] = {
            curand_uniform(states) * session.width,
            curand_uniform(states) * session.height,
        };
    }
}
// Streams: 0, 1
// Events: 0
void gpu::add_session(game::impl& impl,
    id_t session_id, id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    const auto streams = static_cast<cudaStream_t*>(impl.cuda_streams) + 0;
    const auto events = static_cast<cudaEvent_t*>(impl.cuda_events) + 0;
    const auto states = static_cast<curand_state*>(impl.curand_states) + 0;

    const id_t players = human_players + ai_players;
    add_session_basic<<<1, players + 1, 0, streams[0]>>>(states, impl.sessions, session_id, human_players, ai_players, max_tick);
    cudaEventRecord(events[0], streams[0]);

    cudaStreamWaitEvent(streams[0], events[0], 0);
    add_session_snake<<<players, snake_init_length, 0, streams[0]>>>(impl.sessions, session_id);
    add_session_food<<<1, game_init_food_pp * players, 0, streams[1]>>>(states, impl.sessions, session_id);

    std::ranges::copy(keys, impl.keys[session_id].begin());
    cudaDeviceSynchronize();
}

