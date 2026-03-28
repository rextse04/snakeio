#pragma once
#include <config.hpp>
#include "impl.hpp"
#include <random>
#include <span>

namespace snakeio::gpu {
    struct session;
    void init(game::impl& impl) noexcept;
    void add_session(game::impl& impl,
        id_t session_id, id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept;
}