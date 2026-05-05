#pragma once
#include <config.hpp>
#include <network.hpp>
#include <packet.hpp>
#include <array>
#include <cstddef>
#include <memory>
#include <span>

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

    size_t poll_ingress_batch(snakeio::gpu::device_state& gs, int sock, std::span<ingress_packet> out) noexcept;
    size_t emit_egress_batch(snakeio::gpu::device_state& gs, int sock) noexcept;

private:
    size_t poll_socket_batch(int sock, std::span<ingress_packet> out) noexcept;

    std::unique_ptr<DocaGpuIngress> doca_;
    bool doca_ready_{false};
};

} // namespace snakeio::doca_gpunetio
