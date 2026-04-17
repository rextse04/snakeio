#pragma once
#include <config.hpp>
#include <array>
#include <cstddef>

namespace snakeio::doca_gpunetio_server {
    struct transport_context;

    enum class backend_kind : unsigned char {
        host_udp_shim = 0,
        doca_gpunetio = 1
    };

    struct config {
        std::size_t rx_ring_size = 4096;
        std::size_t tx_ring_size = 4096;
        std::size_t rx_burst_size = 64;
        std::size_t tx_burst_size = 64;
        uint16_t port = 12051;
        backend_kind backend = backend_kind::doca_gpunetio;
        std::array<char, 32> gpu_pci_addr{};
        std::array<char, 16> nic_pci_addr{};
    };

    struct context {
        bool initialized = false;
        transport_context* transport = nullptr;
    };

    bool start(context& ctx, const config& cfg, void* device_state) noexcept;
    void stop(context& ctx) noexcept;
}
