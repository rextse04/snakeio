#include <cpp_utils/tests/common.hpp>
#include "../game_kernels.cuh"
#include <tests/crypt.hpp>
#include <packet.hpp>
#include <utils.hpp>
#include <bit>
#include <array>
#include <set>
#include <limits>

// End-to-end packet IO tests for gpu_server.
//
// These tests verify real encrypted packet paths against gpu::device_state:
// - ingress: encrypted client packet -> ingest/decrypt/parse -> client last_packet state
// - egress: tick pipeline -> encrypted server packets -> verify/decrypt/assert packet type+headers

namespace {
namespace sio = snakeio;
namespace pcrypt = snakeio::test::crypt;

constexpr sio::id_t kSessionId = 7;
constexpr sio::id_t kPlayerId = 0;

sio::key_t test_key(std::byte seed = std::byte(1)) {
    sio::key_t k{};
    for (sio::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<std::byte>(static_cast<unsigned char>(seed) + i);
    }
    return k;
}

std::array<std::byte, sio::in_packet_max_text_size + sio::data_packet::header_size>
make_ingress_packet(const sio::key_t& key, sio::id_t session_id, sio::id_t player_id,
    bool snapshot_requested, bool boost, float angle, sio::tick_t nonce_part) {
    std::array<std::byte, sio::in_packet_max_text_size + sio::data_packet::header_size> raw{};
    sio::data_packet p(raw.data(), raw.size());
    p.session_id(session_id);
    p.player_id(player_id);
    p.sender(sio::data_packet::sender_t::client);
    p.total_chunks(1);
    p.chunk_id(0);
    sio::store_32(p.nonce_part(), nonce_part);

    auto text = p.text();
    std::fill(text.begin(), text.end(), std::byte(0));
    text[0] = static_cast<std::byte>(snapshot_requested);
    text[1] = static_cast<std::byte>(boost);
    sio::store_32(std::span<std::byte, 4>(text.data() + 4, 4), std::bit_cast<std::uint_least32_t>(angle));

    pcrypt::encrypt_packet(raw, key);
    return raw;
}

std::set<std::uint_least32_t> decrypt_types(sio::gpu::device_state& state, const sio::key_t& key, sio::tick_t expected_tick) {
    std::set<std::uint_least32_t> types;
    for (unsigned i = 0; i < state.report->send_count; ++i) {
        const auto& d = state.send_descs[i];
        BOOST_REQUIRE_EQUAL(d.session_id, kSessionId);
        BOOST_REQUIRE_EQUAL(d.player_id, kPlayerId);
        auto packet = std::span<std::byte>(state.packet_ring + d.ring_offset, d.bytes_size);
        BOOST_REQUIRE(pcrypt::verify_decrypt_packet(packet, key));
        sio::data_packet p(packet.data(), packet.size());
        BOOST_CHECK_EQUAL(p.session_id(), kSessionId);
        BOOST_CHECK_EQUAL(p.player_id(), kPlayerId);
        BOOST_CHECK(p.sender() == sio::data_packet::sender_t::server);
        BOOST_CHECK_GT(p.total_chunks(), 0);
        BOOST_CHECK_LT(p.chunk_id(), p.total_chunks());
        BOOST_CHECK_EQUAL(sio::load_32(p.nonce_part()), expected_tick);
        types.insert(sio::load_32(std::span<const std::byte, 4>(p.text().data(), 4)));
    }
    return types;
}

std::array<std::set<std::uint_least32_t>, 2>
decrypt_types_by_player_2(sio::gpu::device_state& state, const std::array<sio::key_t, 2>& keys, sio::tick_t expected_tick) {
    std::array<std::set<std::uint_least32_t>, 2> types{};
    for (unsigned i = 0; i < state.report->send_count; ++i) {
        const auto& d = state.send_descs[i];
        BOOST_REQUIRE_EQUAL(d.session_id, kSessionId);
        BOOST_REQUIRE_LT(d.player_id, 2u);
        auto packet = std::span<std::byte>(state.packet_ring + d.ring_offset, d.bytes_size);
        const auto& key = keys[d.player_id];
        BOOST_REQUIRE(pcrypt::verify_decrypt_packet(packet, key));
        sio::data_packet p(packet.data(), packet.size());
        BOOST_CHECK_EQUAL(p.session_id(), kSessionId);
        BOOST_CHECK_EQUAL(p.player_id(), d.player_id);
        BOOST_CHECK(p.sender() == sio::data_packet::sender_t::server);
        BOOST_CHECK_EQUAL(sio::load_32(p.nonce_part()), expected_tick);
        types[d.player_id].insert(sio::load_32(std::span<const std::byte, 4>(p.text().data(), 4)));
    }
    return types;
}
}

BOOST_AUTO_TEST_CASE(packet_ingress_e2e) {
    // Valid encrypted ingress packet should be accepted and parsed into client state.
    // A duplicate packet in the same tick should be ignored.
    sio::gpu::device_state state{};
    sio::gpu::init_device_state(state);

    const sio::key_t key = test_key();
    sio::gpu::add_session_gpu(state, kSessionId, 1, 0, 8, key.data());

    const auto packet = make_ingress_packet(key, kSessionId, kPlayerId, true, true, 0.75f, 123u);
    sio::gpu::ingest_packet_gpu(state, packet.data(), packet.size());

    BOOST_REQUIRE(*state.ingress_ok);
    BOOST_CHECK_EQUAL(*state.ingress_session_id, kSessionId);
    BOOST_CHECK_EQUAL(*state.ingress_player_id, kPlayerId);

    const auto& c = state.clients[sio::gpu::client_index(kSessionId, kPlayerId)];
    BOOST_CHECK(c.last_packet.snapshot_requested);
    BOOST_CHECK(c.last_packet.boost);
    BOOST_CHECK_CLOSE_FRACTION(c.last_packet.angle, 0.75f, 1e-5f);

    // Duplicate packet in same tick is ignored.
    sio::gpu::ingest_packet_gpu(state, packet.data(), packet.size());
    BOOST_CHECK(!*state.ingress_ok);

    sio::gpu::destroy_device_state(state);
}

BOOST_AUTO_TEST_CASE(packet_egress_e2e) {
    // Single-player egress should emit:
    // - tick 0: snapshot packet (type=1)
    // - next tick (max_tick reached): delta (type=0) and termination (type=3)
    sio::gpu::device_state state{};
    sio::gpu::init_device_state(state);

    const sio::key_t key = test_key();
    sio::gpu::add_session_gpu(state, kSessionId, 1, 0, 1, key.data());

    auto& s = state.sessions[kSessionId];
    auto& c = state.clients[sio::gpu::client_index(kSessionId, kPlayerId)];

    // Tick 0: mark player ready so snapshot is emitted.
    c.tick = 0;
    c.last_packet = {.snapshot_requested = false, .boost = false,
        .angle = std::numeric_limits<sio::scalar_t>::quiet_NaN()};
    sio::gpu::tick_session_gpu(state, kSessionId);

    BOOST_REQUIRE(state.report->has_payload);
    auto types0 = decrypt_types(state, key, 0);
    BOOST_CHECK(types0.contains(1));

    // Next tick: regular delta + termination (max_tick = 1).
    c.tick = s.tick;
    c.last_packet = {.snapshot_requested = false, .boost = false,
        .angle = std::numeric_limits<sio::scalar_t>::quiet_NaN()};
    sio::gpu::tick_session_gpu(state, kSessionId);

    BOOST_REQUIRE(state.report->has_payload);
    BOOST_REQUIRE(state.report->ended);
    auto types1 = decrypt_types(state, key, 1);
    BOOST_CHECK(types1.contains(0));
    BOOST_CHECK(types1.contains(3));

    sio::gpu::destroy_device_state(state);
}

BOOST_AUTO_TEST_CASE(packet_egress_multi_player_branch_e2e) {
    // Multi-player branch behavior:
    // - tick 0: both ready => both receive snapshot (type=1)
    // - tick 1: player0 receives delta (type=0), player1 requests snapshot (type=1)
    sio::gpu::device_state state{};
    sio::gpu::init_device_state(state);

    const std::array<sio::key_t, 2> keys = {test_key(std::byte(1)), test_key(std::byte(51))};
    sio::gpu::add_session_gpu(state, kSessionId, 2, 0, 3,
        reinterpret_cast<const std::byte*>(keys.data()));

    auto& s = state.sessions[kSessionId];
    auto& c0 = state.clients[sio::gpu::client_index(kSessionId, 0)];
    auto& c1 = state.clients[sio::gpu::client_index(kSessionId, 1)];

    // Tick 0: both ready => both receive snapshot.
    c0.tick = 0;
    c1.tick = 0;
    c0.last_packet = {.snapshot_requested = false, .boost = false,
        .angle = std::numeric_limits<sio::scalar_t>::quiet_NaN()};
    c1.last_packet = c0.last_packet;
    sio::gpu::tick_session_gpu(state, kSessionId);
    BOOST_REQUIRE(state.report->has_payload);
    auto types0 = decrypt_types_by_player_2(state, keys, 0);
    BOOST_CHECK(types0[0].contains(1));
    BOOST_CHECK(types0[1].contains(1));

    // Tick 1: player0 gets delta, player1 asks snapshot.
    c0.tick = s.tick;
    c1.tick = s.tick;
    c0.last_packet = {.snapshot_requested = false, .boost = false,
        .angle = std::numeric_limits<sio::scalar_t>::quiet_NaN()};
    c1.last_packet = {.snapshot_requested = true, .boost = false,
        .angle = std::numeric_limits<sio::scalar_t>::quiet_NaN()};
    sio::gpu::tick_session_gpu(state, kSessionId);
    BOOST_REQUIRE(state.report->has_payload);
    auto types1 = decrypt_types_by_player_2(state, keys, 1);
    BOOST_CHECK(types1[0].contains(0));
    BOOST_CHECK(!types1[0].contains(1));
    BOOST_CHECK(types1[1].contains(1));

    sio::gpu::destroy_device_state(state);
}
