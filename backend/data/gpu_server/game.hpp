#pragma once
#include <config.hpp>
#include "impl.hpp"
#include <random>
#include <span>

namespace snakeio::gpu {
    struct session;
    void init_curand(curand_state* state, std::random_device::result_type seed);
    void add_session(curand_state* state, session* sessions, id_t session_id,
        id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept;
}