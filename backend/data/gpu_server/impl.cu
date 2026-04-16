#include "impl.cuh"

using namespace snakeio;

game::impl::impl() {
    cudaStreamCreate(&cuda_streams[0]);
    cudaStreamCreate(&cuda_streams[1]);
    cudaEventCreateWithFlags(&cuda_events[0], cudaEventDisableTiming);

    cudaMalloc(&global_tick, sizeof(std::uintmax_t));
    cudaMalloc(&d_keys, sizeof(key_t) * game_max_sessions * game_max_players);
    cudaMallocHost(&h_outbox, sizeof(gpu::outbox));
    cudaHostGetDevicePointer(&d_outbox, h_outbox, 0);
    new(h_outbox) gpu::outbox;
    cudaMalloc(&d_outbox_, sizeof(gpu::outbox));
    cudaMemcpyAsync(d_outbox_, h_outbox, sizeof(gpu::outbox), cudaMemcpyHostToDevice, cuda_streams[0]);
    cudaMalloc(&d_add_req, sizeof(gpu::add_session_req) * game_max_sessions * game_max_players);

    if (cudaPeekAtLastError() != cudaSuccess) {
        throw cudaPeekAtLastError();
    }
    cudaStreamSynchronize(cuda_streams[0]);
}

game::impl::~impl() {
    cudaFree(d_keys);
    h_outbox->destroy();
    cudaFreeHost(h_outbox);
    cudaFree(d_add_req);
    sessions.destroy();
    for (const auto& event : cuda_events) {
        cudaEventDestroy(event);
    }
    for (const auto& stream : cuda_streams) {
        cudaStreamDestroy(stream);
    }
}

