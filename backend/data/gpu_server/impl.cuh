#pragma once
#include <game.hpp>
#include <config.hpp>
#include <network.hpp>
#include "session.cuh"
#include "parse.cuh"
#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>

namespace snakeio {
    namespace gpu {
        inline constexpr std::size_t cuda_streams_size = 2;
        inline constexpr std::size_t cuda_events_size = 1;

        struct in_packet_info {
            std::atomic<std::uintmax_t> global_tick = 0;
            sockaddr_storage addr;
            bool snapshot_requested, boost;
            scalar_t angle;
        };
        __host__ __device__ constexpr std::size_t global_id(id_t session_id, id_t player_id) noexcept {
            return static_cast<std::size_t>(session_id) * game_max_players + player_id;
        }
    }

    struct game::impl {
        std::array<cudaStream_t, gpu::cuda_streams_size> cuda_streams;
        std::array<cudaEvent_t, gpu::cuda_events_size> cuda_events;

        std::uintmax_t* global_tick;
        std::array<std::array<sockaddr_storage, game_max_players>, game_max_sessions> addrs;
        key_t* d_keys;
        gpu::outbox *h_outbox, *d_outbox, *d_outbox_;
        gpu::add_session_req *d_add_req;

        gpu::session_batch sessions;

        impl();
        ~impl();
    };
}