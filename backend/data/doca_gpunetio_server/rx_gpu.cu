#include "rx_gpu.cuh"
#include <gpu_server/game_kernels.cuh>

namespace {
    __global__ void k_rx_stage_stub(volatile bool* stop_flag, snakeio::gpu::device_state* device_state) {
        while (!*stop_flag) {
            // TODO: poll GPUNetIO RX queue and stage validated packets into gpu_server ingress rings.
        }
    }
}

bool snakeio::doca_gpunetio_server::start_rx_worker(transport_context& ctx) noexcept {
    if (!ctx.rx_ready) return false;
    if (ctx.backend == backend_kind::host_udp_shim) return true;
    if (ctx.doca_gpu_rxq == nullptr) return true;

    cudaMalloc(&ctx.d_rx_stop, sizeof(bool));
    cudaMemset(ctx.d_rx_stop, 0, sizeof(bool));

    auto* d_state = static_cast<snakeio::gpu::device_state*>(ctx.device_state);
    k_rx_stage_stub<<<1, 1>>>(ctx.d_rx_stop, d_state);
    return true;
}

void snakeio::doca_gpunetio_server::stop_rx_worker(transport_context& ctx) noexcept {
    if (ctx.d_rx_stop != nullptr) {
        bool stop = true;
        cudaMemcpy(ctx.d_rx_stop, &stop, sizeof(bool), cudaMemcpyHostToDevice);
        cudaDeviceSynchronize();
        cudaFree(ctx.d_rx_stop);
        ctx.d_rx_stop = nullptr;
    }
}
