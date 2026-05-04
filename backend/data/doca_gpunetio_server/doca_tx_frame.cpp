#include "doca_tx_frame.hpp"

#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace snakeio::doca_tx_frame {
    bool build_ipv4_udp(std::array<uint8_t, 64>& hdr,
        unsigned& out_hdr_len,
        const uint8_t l2_dst[6],
        const uint8_t nic_mac[6],
        const uint8_t src_ip4[4],
        const uint8_t dst_ip4[4],
        const uint16_t src_udp_port_be,
        const uint16_t dst_udp_port_be,
        const snakeio::size_t payload_len) noexcept {
        if (static_cast<unsigned>(payload_len) + ipv4_udp_headers_bytes > max_frame_bytes) {
            return false;
        }

        hdr.fill(0);
        std::memcpy(hdr.data(), l2_dst, 6);
        std::memcpy(hdr.data() + 6, nic_mac, 6);
        hdr[12] = 0x08;
        hdr[13] = 0x00;

        const uint16_t udp_len = static_cast<uint16_t>(8U + static_cast<unsigned>(payload_len));
        const uint16_t ip_len = static_cast<uint16_t>(20U + udp_len);

        unsigned o = 14;
        hdr[o++] = 0x45;
        hdr[o++] = 0;
        hdr[o++] = static_cast<uint8_t>(ip_len >> 8);
        hdr[o++] = static_cast<uint8_t>(ip_len & 0xff);
        hdr[o++] = 0;
        hdr[o++] = 0;
        hdr[o++] = 0;
        hdr[o++] = 0;
        hdr[o++] = 64;
        hdr[o++] = IPPROTO_UDP;
        hdr[o++] = 0;
        hdr[o++] = 0;
        std::memcpy(&hdr[o], src_ip4, 4);
        o += 4;
        std::memcpy(&hdr[o], dst_ip4, 4);
        o += 4;

        std::memcpy(&hdr[o], &src_udp_port_be, sizeof(src_udp_port_be));
        o += 2;
        std::memcpy(&hdr[o], &dst_udp_port_be, sizeof(dst_udp_port_be));
        o += 2;
        const uint16_t udp_len_be = htons(udp_len);
        std::memcpy(&hdr[o], &udp_len_be, sizeof(udp_len_be));
        o += 2;
        hdr[o++] = 0;
        hdr[o++] = 0;

        out_hdr_len = ipv4_udp_headers_bytes;
        return true;
    }

    bool build_ipv6_udp(std::array<uint8_t, 64>& hdr,
        unsigned& out_hdr_len,
        const uint8_t l2_dst[6],
        const uint8_t nic_mac[6],
        const uint8_t src_ip6[16],
        const uint8_t dst_ip6[16],
        const uint16_t src_udp_port_be,
        const uint16_t dst_udp_port_be,
        const snakeio::size_t payload_len) noexcept {
        if (static_cast<unsigned>(payload_len) + ipv6_udp_headers_bytes > max_frame_bytes) {
            return false;
        }

        hdr.fill(0);
        std::memcpy(hdr.data(), l2_dst, 6);
        std::memcpy(hdr.data() + 6, nic_mac, 6);
        hdr[12] = 0x86;
        hdr[13] = 0xdd;

        const uint16_t ipv6_payload_u16 = static_cast<uint16_t>(8U + static_cast<unsigned>(payload_len));
        unsigned o = 14;
        hdr[o++] = 0x60;
        hdr[o++] = 0;
        hdr[o++] = 0;
        hdr[o++] = 0;
        hdr[o++] = static_cast<uint8_t>(ipv6_payload_u16 >> 8);
        hdr[o++] = static_cast<uint8_t>(ipv6_payload_u16 & 0xff);
        hdr[o++] = IPPROTO_UDP;
        hdr[o++] = 64;
        std::memcpy(&hdr[o], src_ip6, 16);
        o += 16;
        std::memcpy(&hdr[o], dst_ip6, 16);
        o += 16;

        std::memcpy(&hdr[o], &src_udp_port_be, sizeof(src_udp_port_be));
        o += 2;
        std::memcpy(&hdr[o], &dst_udp_port_be, sizeof(dst_udp_port_be));
        o += 2;
        const uint16_t udp_len_be = htons(static_cast<uint16_t>(8U + static_cast<unsigned>(payload_len)));
        std::memcpy(&hdr[o], &udp_len_be, sizeof(udp_len_be));
        o += 2;
        hdr[o++] = 0;
        hdr[o++] = 0;

        out_hdr_len = ipv6_udp_headers_bytes;
        return true;
    }
}
