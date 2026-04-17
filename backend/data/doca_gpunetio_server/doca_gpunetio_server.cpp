#include "doca_gpunetio_server.hpp"
#include "doca_context.hpp"
#include "rx_gpu.cuh"
#include "tx_gpu.cuh"

namespace snakeio::doca_gpunetio_server {
    bool start(context& ctx, const config& cfg, void* device_state) noexcept {
        if (ctx.transport != nullptr) return true;
        ctx.transport = new transport_context{};
        ctx.transport->device_state = device_state;
        if (!init_transport(*ctx.transport, cfg)) {
            delete ctx.transport;
            ctx.transport = nullptr;
            return false;
        }

        if (!start_rx_worker(*ctx.transport) || !start_tx_worker(*ctx.transport)) {
            shutdown_transport(*ctx.transport);
            delete ctx.transport;
            ctx.transport = nullptr;
            return false;
        }

        ctx.transport->gpu_workers_running = true;
        ctx.initialized = true;
        return true;
    }

    void stop(context& ctx) noexcept {
        if (ctx.transport != nullptr) {
            stop_tx_worker(*ctx.transport);
            stop_rx_worker(*ctx.transport);
            shutdown_transport(*ctx.transport);
            delete ctx.transport;
            ctx.transport = nullptr;
        }
        ctx.initialized = false;
    }
}
