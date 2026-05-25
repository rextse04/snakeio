#pragma once
#include <config.hpp>
#include <game.hpp>
#include <network.hpp>
#include "parse.hpp"
#include "session.hpp"
#include <array>

namespace snakeio {
    struct game::impl {
        udp_port data_port{"data", {
            .sin6_family = AF_INET6,
            .sin6_port = htons(data_plane_ext_port),
            .sin6_addr = in6addr_any
        }};
        random_engine add_session_rng_{std::random_device()()}, game_loop_rng_{std::random_device()()};
        std::array<std::array<cpu::client, game_max_players>, game_max_sessions> clients;
        std::array<cpu::session, game_max_sessions> sessions;
    };
}
