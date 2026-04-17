#pragma once
#include "doca_gpunetio_server.hpp"

namespace snakeio::doca_gpunetio_server {
    // Transport bootstrap state kept separate from game logic reuse.
    struct transport_context {
        backend_kind backend = backend_kind::host_udp_shim;
        bool rx_ready = false;
        bool tx_ready = false;
        bool gpu_workers_running = false;
        bool rx_ctx_started = false;
        bool tx_ctx_started = false;

        void* device_state = nullptr;

        // DOCA core objects (kept as opaque pointers in the public header).
        void* doca_dev = nullptr;
        void* doca_gpu = nullptr;
        void* doca_mmap = nullptr;
        void* doca_rxq = nullptr;
        void* doca_txq = nullptr;
        void* doca_gpu_rxq = nullptr;
        void* doca_gpu_txq = nullptr;

        void* doca_flow_port = nullptr;
        void* doca_flow_pipe = nullptr;

        bool* d_rx_stop = nullptr;
        bool* d_tx_stop = nullptr;

        void* rx_pkt_buf_gpu = nullptr;
        void* rx_pkt_buf_cpu = nullptr;
        std::size_t rx_pkt_buf_size = 0;
    };

    bool init_transport(transport_context& ctx, const config& cfg) noexcept;
    void shutdown_transport(transport_context& ctx) noexcept;
}
