#include "tx_gpu.cuh"
#include <gpu_server/game_kernels.cuh>

namespace {
    __global__ void k_tx_stage_stub(volatile bool* stop_flag, snakeio::gpu::device_state* device_state) {
        while (!*stop_flag) {
            // TODO: consume gpu_server send descriptors and submit GPUNetIO TX bursts.
        }
    }
}

bool snakeio::doca_gpunetio_server::start_tx_worker(transport_context& ctx) noexcept {
    if (!ctx.tx_ready) return false;
    if (ctx.backend == backend_kind::host_udp_shim) return true;
    if (ctx.doca_gpu_txq == nullptr) return true;

    cudaMalloc(&ctx.d_tx_stop, sizeof(bool));
    cudaMemset(ctx.d_tx_stop, 0, sizeof(bool));

    auto* d_state = static_cast<snakeio::gpu::device_state*>(ctx.device_state);
    k_tx_stage_stub<<<1, 1>>>(ctx.d_tx_stop, d_state);
    return true;
}

void snakeio::doca_gpunetio_server::stop_tx_worker(transport_context& ctx) noexcept {
    if (ctx.d_tx_stop != nullptr) {
        bool stop = true;
        cudaMemcpy(ctx.d_tx_stop, &stop, sizeof(bool), cudaMemcpyHostToDevice);
        cudaDeviceSynchronize();
        cudaFree(ctx.d_tx_stop);
        ctx.d_tx_stop = nullptr;
    }
}
