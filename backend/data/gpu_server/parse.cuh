#pragma once
#include <config.hpp>
#include <utils.hpp>
#include "cuda.cuh"
#include <cstddef>

namespace snakeio::gpu {
    struct outbox {
        atomic_align(id_t) size;
        atomic_align(std::size_t) packet_sizes[game_max_sessions * game_max_players];
        std::byte *h_buffer, *d_buffer;

        outbox();
        void destroy();
        __device__ id_t allocate();
        __device__ void clear();
    };
}