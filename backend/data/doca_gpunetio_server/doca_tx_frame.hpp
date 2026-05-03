#pragma once

#include <config.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace snakeio::doca_tx_frame {
    constexpr unsigned max_frame_bytes = 2048;
    constexpr unsigned ipv4_udp_headers_bytes = 14U + 20U + 8U;
    constexpr unsigned ipv6_udp_headers_bytes = 14U + 40U + 8U;

    // Writes Ethernet + IPv4 + UDP headers (no payload). hdr must hold at least ipv4_udp_headers_bytes.
    // src_udp_port_be / dst_udp_port_be are already network byte order.
    [[nodiscard]] bool build_ipv4_udp(std::array<uint8_t, 64>& hdr,
        unsigned& out_hdr_len,
        const uint8_t l2_dst[6],
        const uint8_t nic_mac[6],
        const uint8_t src_ip4[4],
        const uint8_t dst_ip4[4],
        uint16_t src_udp_port_be,
        uint16_t dst_udp_port_be,
        snakeio::size_t payload_len) noexcept;

    // Writes Ethernet + IPv6 + UDP headers (no payload).
    [[nodiscard]] bool build_ipv6_udp(std::array<uint8_t, 64>& hdr,
        unsigned& out_hdr_len,
        const uint8_t l2_dst[6],
        const uint8_t nic_mac[6],
        const uint8_t src_ip6[16],
        const uint8_t dst_ip6[16],
        uint16_t src_udp_port_be,
        uint16_t dst_udp_port_be,
        snakeio::size_t payload_len) noexcept;
}
