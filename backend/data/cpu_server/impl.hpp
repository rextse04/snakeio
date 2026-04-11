#pragma once
#include <config.hpp>
#include <game.hpp>
#include "parse.hpp"
#include "session.hpp"
#include <array>

namespace snakeio {
    struct game::impl {
        random_engine add_session_rng_{std::random_device()()}, game_loop_rng_{std::random_device()()};
        std::array<std::array<cpu::client, game_max_players>, game_max_sessions> clients;
        std::array<cpu::session, game_max_sessions> sessions;
    };
}
