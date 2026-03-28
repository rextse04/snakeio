#pragma once
#include <game.hpp>
#include <config.hpp>
#include <cstddef>
#include <array>

struct curandStatePhilox4_32_10;

namespace snakeio {
    namespace gpu {
        using curand_state = curandStatePhilox4_32_10;
        inline constexpr std::size_t curand_state_size = game_max_sessions;

        struct session;
    }

    struct game::impl {
        gpu::curand_state* curand_state;
        std::array<std::array<key_t, game_max_players>, game_max_sessions> keys;
        gpu::session* sessions;

        static void port(game& game, std::stop_token stop_token, int sock) noexcept;
    };
}