#pragma once
#include <game.hpp>
#include <config.hpp>
#include <network.hpp>
#include "session.cuh"
#include <cstddef>
#include <array>

namespace snakeio {
    namespace gpu {
        inline constexpr std::size_t cuda_streams_size = 2;
        inline constexpr std::size_t cuda_events_size = 1;
        inline constexpr std::size_t curand_states_size = game_max_sessions;
    }

    struct game::impl {
        void* cuda_streams;
        void* cuda_events;
        void* curand_states;

        key_t keys[game_max_sessions][game_max_players];
        sockaddr_storage addrs[game_max_sessions][game_max_players];
        std::byte packet_buffer[game_max_sessions][game_max_players][in_packet_max_text_size];

        gpu::session_batch sessions;
    };
}