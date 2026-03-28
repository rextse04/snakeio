#pragma once
#include <game.hpp>
#include <config.hpp>
#include <network.hpp>
#include <cstddef>
#include <array>

namespace snakeio {
    namespace gpu {
        inline constexpr std::size_t cuda_streams_size = 2;
        inline constexpr std::size_t cuda_events_size = 1;
        inline constexpr std::size_t curand_states_size = game_max_sessions;

        struct session;
    }

    struct game::impl {
        void* cuda_streams;
        void* cuda_events;
        void* curand_states;
        std::array<std::array<key_t, game_max_players>, game_max_sessions> keys;
        std::array<std::array<sockaddr_storage, game_max_players>, game_max_sessions> addrs;
        std::array<std::array<std::byte, in_packet_max_text_size>, game_max_players * game_max_sessions> packet_buffer;
        std::size_t packet_buffer_size = 0;
        gpu::session* sessions;

        static void port(game& game, std::stop_token stop_token, int sock) noexcept;
    };
}