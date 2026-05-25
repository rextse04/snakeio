#pragma once
#include <config.hpp>
#include <network.hpp>
#include <packet.hpp>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

#include "doca_gpunetio_net.hpp"

namespace snakeio::gpu {
    struct device_state;
}

namespace snakeio::doca_gpunetio {

struct ingress_packet {
    std::array<std::byte, in_packet_max_text_size + data_packet::header_size> bytes{};
    size_t size{};
    sockaddr_storage source_addr{};
};

class runtime {
public:
    runtime() = default;

    void try_init_doca(snakeio::gpu::device_state& gs) noexcept;

    bool doca_active() const noexcept {
        return doca_ != nullptr && doca_ready_;
    }

    /// Port thread only: kernel UDP path — one blocking `recvfrom`, then direct GPU ingest.
    /// Returns `{rx packets consumed from socket, ingest accepted}` for this call.
    [[nodiscard]] std::pair<std::size_t, std::size_t> process_kernel_udp_ingress(
        snakeio::gpu::device_state& gs, int sock, std::stop_token stop_token) noexcept;

    /// Port thread only: sole GPUNetIO RX path — receives staged frames then ingests directly.
    /// Returns `{rx packets from GPUNetIO stage, ingest accepted}` for this call.
    [[nodiscard]] std::pair<std::size_t, std::size_t> process_doca_ingress(
        snakeio::gpu::device_state& gs, std::stop_token stop_token) noexcept;

    std::size_t emit_egress_batch(snakeio::gpu::device_state& gs, int sock) noexcept;

private:
    std::mutex doca_mtx_;

    std::unique_ptr<DocaGpuIngress> doca_;
    bool doca_ready_{false};
};

} // namespace snakeio::doca_gpunetio
