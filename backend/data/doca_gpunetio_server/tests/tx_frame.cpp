#define BOOST_TEST_MODULE doca_gpunetio_tx_frame
#include <boost/test/unit_test.hpp>

#include "doca_tx_frame.hpp"

#include <config.hpp>

#include <arpa/inet.h>
#include <array>
#include <cstring>

BOOST_AUTO_TEST_CASE(ipv4_udp_zero_payload_golden) {
    const uint8_t l2_dst[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const uint8_t nic_mac[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    uint8_t src_ip4[4]{};
    uint8_t dst_ip4[4]{};
    BOOST_REQUIRE_EQUAL(inet_pton(AF_INET, "192.168.1.1", src_ip4), 1);
    BOOST_REQUIRE_EQUAL(inet_pton(AF_INET, "192.168.1.2", dst_ip4), 1);

    const uint16_t src_port = htons(snakeio::data_plane_ext_port);
    const uint16_t dst_port = htons(12345);
    const uint8_t src_wire0 = static_cast<uint8_t>(snakeio::data_plane_ext_port >> 8);
    const uint8_t src_wire1 = static_cast<uint8_t>(snakeio::data_plane_ext_port & 0xff);

    std::array<uint8_t, 64> hdr{};
    unsigned hdr_len = 0;
    BOOST_REQUIRE(snakeio::doca_tx_frame::build_ipv4_udp(
        hdr, hdr_len, l2_dst, nic_mac, src_ip4, dst_ip4, src_port, dst_port, 0));
    BOOST_CHECK_EQUAL(hdr_len, snakeio::doca_tx_frame::ipv4_udp_headers_bytes);

    const std::array<uint8_t, 42> expected = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x08, 0x00,
        0x45, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x40, 0x11, 0x00, 0x00,
        0xc0, 0xa8, 0x01, 0x01, 0xc0, 0xa8, 0x01, 0x02,
        src_wire0,
        src_wire1,
        0x30,
        0x39,
        0x00, 0x08, 0x00, 0x00,
    };
    BOOST_REQUIRE_EQUAL(hdr_len, expected.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(hdr.begin(), hdr.begin() + static_cast<std::ptrdiff_t>(hdr_len), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(ipv4_udp_rejects_overflow) {
    const uint8_t l2[6]{};
    const uint8_t nic[6]{};
    const uint8_t s4[4]{};
    const uint8_t d4[4]{};
    std::array<uint8_t, 64> hdr{};
    unsigned hdr_len = 0;
    const snakeio::size_t too_big = snakeio::doca_tx_frame::max_frame_bytes - snakeio::doca_tx_frame::ipv4_udp_headers_bytes + 1;
    BOOST_CHECK(!snakeio::doca_tx_frame::build_ipv4_udp(hdr, hdr_len, l2, nic, s4, d4, htons(1), htons(2), too_big));
}

BOOST_AUTO_TEST_CASE(ipv6_udp_zero_payload_golden) {
    const uint8_t l2_dst[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const uint8_t nic_mac[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
    uint8_t src6[16]{};
    uint8_t dst6[16]{};
    BOOST_REQUIRE_EQUAL(inet_pton(AF_INET6, "fc00::1", src6), 1);
    BOOST_REQUIRE_EQUAL(inet_pton(AF_INET6, "fc00::2", dst6), 1);

    const uint16_t src_port = htons(40000);
    const uint16_t dst_port = htons(50001);

    std::array<uint8_t, 64> hdr{};
    unsigned hdr_len = 0;
    BOOST_REQUIRE(snakeio::doca_tx_frame::build_ipv6_udp(
        hdr, hdr_len, l2_dst, nic_mac, src6, dst6, src_port, dst_port, 0));
    BOOST_CHECK_EQUAL(hdr_len, snakeio::doca_tx_frame::ipv6_udp_headers_bytes);

    std::array<uint8_t, 62> expected{};
    unsigned o = 0;
    const auto push = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            expected[o++] = b;
        }
    };
    push({0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x86, 0xdd});
    push({0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x11, 0x40});
    std::memcpy(expected.data() + o, src6, 16);
    o += 16;
    std::memcpy(expected.data() + o, dst6, 16);
    o += 16;
    push({0x9c, 0x40, 0xc3, 0x51, 0x00, 0x08, 0x00, 0x00});
    BOOST_REQUIRE_EQUAL(o, expected.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(hdr.begin(), hdr.begin() + static_cast<std::ptrdiff_t>(hdr_len), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(ipv6_udp_rejects_overflow) {
    const uint8_t z6[6]{};
    const uint8_t s16[16]{};
    const uint8_t d16[16]{};
    std::array<uint8_t, 64> hdr{};
    unsigned hdr_len = 0;
    const snakeio::size_t too_big = snakeio::doca_tx_frame::max_frame_bytes - snakeio::doca_tx_frame::ipv6_udp_headers_bytes + 1;
    BOOST_CHECK(!snakeio::doca_tx_frame::build_ipv6_udp(hdr, hdr_len, z6, z6, s16, d16, htons(1), htons(2), too_big));
}
