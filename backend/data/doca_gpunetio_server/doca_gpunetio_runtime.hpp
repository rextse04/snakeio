#pragma once

#include <doca_error.h>
#include <config.hpp>
#include <array>
#include <span>
#include <stop_token>
#include <sys/types.h>

struct sockaddr_storage;

namespace snakeio::doca_gpunetio_runtime {
    doca_error_t init() noexcept;
    void shutdown() noexcept;

    bool started() noexcept;

    // Launches the GPUNetIO receive path (DOCA Flow + cyclic Eth RXQ + CUDA recv kernel).
    doca_error_t start_recv(void* cuda_stream) noexcept;
    void stop_recv(void* cuda_stream) noexcept;

    // Blocks until a UDP payload is available (UDP datagram bytes without L2/L3/L4 headers).
    // Returns false on stop/shutdown/empty wakeups.
    bool pop_udp_payload(std::stop_token stop_token,
        std::span<std::byte> out_payload,
        snakeio::size_t& out_payload_len,
        sockaddr_storage& out_src_addr,
        std::array<std::byte, 6>& out_src_eth) noexcept;

    // Sends one IPv4 or IPv6 UDP datagram over the GPUNetIO TX path (headers from host, UDP payload device-to-device
    // into the TX buffer). If the learned client Ethernet is all-zero, uses SNAKEIO_DOCA_GATEWAY_MAC when set.
    // Returns bytes sent on success, or -1 if the fast path is unavailable (caller should fall back to sendto).
    ssize_t send_udp_datagram_gpu(const std::byte* payload_dev,
        snakeio::size_t payload_len,
        const sockaddr_storage& dst,
        const std::byte dst_eth[6]) noexcept;
}
