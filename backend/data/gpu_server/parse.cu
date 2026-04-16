#include "parse.cuh"
#include <cuda/std/atomic>

namespace snakeio::gpu {
    outbox::outbox() {
        constexpr std::size_t buffer_size = out_packet_max_text_size * game_max_sessions * game_max_players;
        const auto [h_buffer, d_buffer] = cudaNetworkMalloc(buffer_size);
        this->h_buffer = static_cast<std::byte*>(h_buffer);
        this->d_buffer = static_cast<std::byte*>(d_buffer);
    }

    void outbox::destroy() {
        cudaNetworkFree(h_buffer);
    }

    __device__ id_t outbox::allocate() {
        return cuda::std::atomic_ref(size).fetch_add(1, cuda::std::memory_order_relaxed);
    }

    __device__ void outbox::clear() {
        cuda::std::atomic_ref(size).store(0, cuda::std::memory_order_relaxed);
    }
}
