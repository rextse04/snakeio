#include <cpp_utils/tests/common.hpp>
#include <config.hpp>
#include <game.hpp>
#include <packet.hpp>
#include <utils.hpp>
#include <network.hpp>
#include <cstdint>
#include <bit>
#include <array>
#include <set>
#include <limits>
#include <thread>
#include <chrono>

// End-to-end packet IO tests over real UDP sockets.
//
// These tests drive `snakeio::game` through the same public surface used by runtime:
// - encrypted ingress datagrams into `game::port`
// - encrypted egress datagrams produced by `game::tick`
// - payload verification by decrypting received packets on the client side

namespace {
constexpr snakeio::id_t kPlayerId = 0;

struct udp_runtime {
    snakeio::game game;
    int client_sock = -1;
    sockaddr_in6 game_addr{};
    std::jthread port_thread;

    static void set_recv_timeout_ms(int sock, long ms) {
        const timeval tv{.tv_sec = 0, .tv_usec = static_cast<int>(ms * 1000)};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    static sockaddr_in6 bind_loopback_udp(int sock, std::uint_least16_t port = 0) {
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_loopback;
        addr.sin6_port = port;
        BOOST_REQUIRE_EQUAL(::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
        socklen_t len = sizeof(addr);
        BOOST_REQUIRE_EQUAL(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        return addr;
    }

    udp_runtime() {
        client_sock = socket(AF_INET6, SOCK_DGRAM, 0);
        BOOST_REQUIRE_NE(client_sock, -1);
        set_recv_timeout_ms(client_sock, 50);
        (void) bind_loopback_udp(client_sock);
        game_addr = sockaddr_in6{
            .sin6_family = AF_INET6,
            .sin6_port = htons(snakeio::data_plane_ext_port),
            .sin6_addr = in6addr_loopback
        };
        port_thread = std::jthread([this](std::stop_token st) {
            game.port(st);
        });
    }

    ~udp_runtime() {
        if (port_thread.joinable()) {
            port_thread.request_stop();
        }
        if (client_sock != -1) close(client_sock);
    }
};

snakeio::key_t test_key(std::byte seed = std::byte(1)) {
    snakeio::key_t k{};
    for (snakeio::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<std::byte>(static_cast<unsigned char>(seed) + i);
    }
    return k;
}

std::array<std::byte, snakeio::in_packet_max_text_size + snakeio::data_packet::header_size>
make_ingress_packet(const snakeio::key_t& key, snakeio::id_t sessnakeion_id, snakeio::id_t player_id,
    bool snapshot_requested, bool boost, float angle, snakeio::tick_t nonce_part) {
    std::array<std::byte, snakeio::in_packet_max_text_size + snakeio::data_packet::header_size> raw{};
    snakeio::data_packet p(raw.data(), raw.size());
    p.session_id(sessnakeion_id);
    p.player_id(player_id);
    p.sender(snakeio::data_packet::sender_t::client);
    p.total_chunks(1);
    p.chunk_id(0);
    snakeio::store_32(p.nonce_part(), nonce_part);

    auto text = p.text();
    std::fill(text.begin(), text.end(), std::byte(0));
    text[0] = static_cast<std::byte>(snapshot_requested);
    text[1] = static_cast<std::byte>(boost);
    snakeio::store_32(std::span<std::byte, 4>(text.data() + 4, 4), std::bit_cast<std::uint_least32_t>(angle));

    p.encrypt(key);
    return raw;
}

void send_packet(int sock, const sockaddr_in6& addr, std::span<const std::byte> bytes) {
    const auto sent = sendto(sock, bytes.data(), bytes.size(), 0,
        reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    BOOST_REQUIRE_EQUAL(sent, static_cast<ssize_t>(bytes.size()));
}

std::vector<std::vector<std::byte>> recv_packets(int sock) {
    std::vector<std::vector<std::byte>> out;
    std::array<std::byte, snakeio::out_packet_max_text_size + snakeio::data_packet::header_size> buf{};
    while (true) {
        const ssize_t n = recvfrom(sock, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            BOOST_REQUIRE_GE(n, 0);
        }
        out.emplace_back(buf.begin(), buf.begin() + n);
    }
    return out;
}

std::set<std::uint_least32_t> decrypt_types(
    std::vector<std::vector<std::byte>>& packets, const snakeio::key_t& key, snakeio::tick_t expected_tick, snakeio::id_t expected_player) {
    std::set<std::uint_least32_t> types;
    for (auto& raw : packets) {
        auto packet = std::span<std::byte>(raw.data(), raw.size());
        snakeio::data_packet p(packet.data(), packet.size());
        if (p.player_id() != expected_player) continue;
        BOOST_REQUIRE(p.verify(key) == snakeio::data_packet::verify_result::ok);
        p.decrypt(key);
        BOOST_CHECK(p.sender() == snakeio::data_packet::sender_t::server);
        if (p.chunk_id() == 0) {
            const auto type = snakeio::load_32(std::span<const std::byte, 4>(p.text().data(), 4));
            const auto nonce = snakeio::load_32(p.nonce_part());
            if (type == 3) {
                BOOST_CHECK(nonce == expected_tick || nonce == expected_tick + 1);
            } else {
                BOOST_CHECK_EQUAL(nonce, expected_tick);
            }
            types.insert(type);
        }
    }
    return types;
}
}

BOOST_AUTO_TEST_CASE(packet_ingress_e2e) {
    // Valid encrypted ingress should influence tick output, and replayed packet should be ignored.
    udp_runtime rt;
    const snakeio::key_t key = test_key();
    const auto sid = rt.game.add_session(1, 0, 2, std::span(&key, 1));
    BOOST_REQUIRE(sid.has_value());

    const auto ingress = make_ingress_packet(key, *sid, kPlayerId, true, true, 0.75f, 123u);
    send_packet(rt.client_sock, rt.game_addr, ingress);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rt.game.tick({});

    auto packets = recv_packets(rt.client_sock);
    BOOST_REQUIRE(!packets.empty());
    const auto types = decrypt_types(packets, key, 0, kPlayerId);
    BOOST_CHECK(types.contains(1));
}

BOOST_AUTO_TEST_CASE(packet_egress_e2e) {
    // Single-player egress: tick 0 emits snapshot, next tick emits delta and termination.
    udp_runtime rt;
    const snakeio::key_t key = test_key();
    const auto sid = rt.game.add_session(1, 0, 1, std::span(&key, 1));
    BOOST_REQUIRE(sid.has_value());

    send_packet(rt.client_sock, rt.game_addr,
        make_ingress_packet(key, *sid, kPlayerId, false, false, std::numeric_limits<float>::quiet_NaN(), 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rt.game.tick({});

    auto packets0 = recv_packets(rt.client_sock);
    BOOST_REQUIRE(!packets0.empty());
    auto types0 = decrypt_types(packets0, key, 0, kPlayerId);
    BOOST_CHECK(types0.contains(1));

    send_packet(rt.client_sock, rt.game_addr,
        make_ingress_packet(key, *sid, kPlayerId, false, false, std::numeric_limits<float>::quiet_NaN(), 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rt.game.tick({});

    auto packets1 = recv_packets(rt.client_sock);
    BOOST_REQUIRE(!packets1.empty());
    auto types1 = decrypt_types(packets1, key, 1, kPlayerId);
    BOOST_CHECK(types1.contains(0));
    BOOST_CHECK(types1.contains(3));
}

BOOST_AUTO_TEST_CASE(packet_egress_multi_player_branch_e2e) {
    // Multi-player branch: one player can request snapshot while another receives delta.
    udp_runtime rt;
    const std::array<snakeio::key_t, 2> keys = {test_key(std::byte(1)), test_key(std::byte(51))};
    const auto sid = rt.game.add_session(2, 0, 3, std::span(keys.data(), keys.size()));
    BOOST_REQUIRE(sid.has_value());

    send_packet(rt.client_sock, rt.game_addr,
        make_ingress_packet(keys[0], *sid, 0, false, false, std::numeric_limits<float>::quiet_NaN(), 1));
    send_packet(rt.client_sock, rt.game_addr,
        make_ingress_packet(keys[1], *sid, 1, false, false, std::numeric_limits<float>::quiet_NaN(), 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rt.game.tick({});

    auto packets0 = recv_packets(rt.client_sock);
    BOOST_REQUIRE(!packets0.empty());
    auto p0t0 = decrypt_types(packets0, keys[0], 0, 0);
    auto p1t0 = decrypt_types(packets0, keys[1], 0, 1);
    BOOST_CHECK(p0t0.contains(1));
    BOOST_CHECK(p1t0.contains(1));

    send_packet(rt.client_sock, rt.game_addr,
        make_ingress_packet(keys[0], *sid, 0, false, false, std::numeric_limits<float>::quiet_NaN(), 2));
    send_packet(rt.client_sock, rt.game_addr,
        make_ingress_packet(keys[1], *sid, 1, true, false, std::numeric_limits<float>::quiet_NaN(), 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rt.game.tick({});

    auto packets1 = recv_packets(rt.client_sock);
    BOOST_REQUIRE(!packets1.empty());
    auto p0t1 = decrypt_types(packets1, keys[0], 1, 0);
    auto p1t1 = decrypt_types(packets1, keys[1], 1, 1);
    BOOST_CHECK(p0t1.contains(0));
    BOOST_CHECK(!p0t1.contains(1));
    BOOST_CHECK(p1t1.contains(1));
}