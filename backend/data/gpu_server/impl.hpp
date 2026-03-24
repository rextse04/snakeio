#pragma once
#include <game.hpp>
#include "session.hpp"

namespace snakeio {
    struct game::impl {
        struct session {

        };

        static void port(game& game, std::stop_token stop_token, int sock) noexcept;
    };
}