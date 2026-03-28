#pragma once
#include <config.hpp>
#include <game.hpp>
#include "parse.hpp"
#include "session.hpp"
#include <array>

namespace snakeio {
    struct game::impl {
        std::mt19937 rng_{std::random_device()()};
        std::array<std::array<cpu::client, game_max_players>, game_max_sessions> clients;
        std::array<cpu::session, game_max_sessions> sessions;

        static void port(game& game, std::stop_token stop_token, int sock) noexcept;
        static void game_loop(game& game, std::stop_token stop_token, int sock) noexcept;
    };
}
