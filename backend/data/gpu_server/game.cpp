#include "impl.hpp"
#include <config.hpp>
#include <network.hpp>
#include <packet.hpp>
#include "game.hpp"
#include "impl.hpp"
#include <random>

using namespace snakeio;
using namespace snakeio::gpu;

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    curand_init(std::random_device()(), );
    new(memory_.get()) impl(curand_init());
}

void game::impl::port(game& game, std::stop_token stop_token, int sock) noexcept {
    impl& impl_ = game.get_impl();
    std::byte buffer[in_packet_max_text_size + data_packet::header_size];
    sockaddr_storage client_addr{};
    while (true) {
        if (stop_token.stop_requested()) [[unlikely]] {
            logger::info("Data port received stop request, exiting.");
            return;
        }
        socklen_t client_addr_len = sizeof(client_addr);
        const ssize_t recv_len = recvfrom(sock, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (recv_len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) [[unlikely]] {
                logger::warn("recvfrom failed on data port: {}.", std::strerror(errno));
            }
            continue;
        }
        data_packet packet(buffer, recv_len);
        if (recv_len <= data_packet::header_size) [[unlikely]] {
            logger::debug("Received packet that is too short from {}.", client_addr);
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        if (!game.sm_[packet.session_id()]) [[unlikely]] {
            logger::debug("Received packet for non-existent session {} from {}.", packet.session_id(), client_addr);
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        session& session = impl_.sessions[packet.session_id()];
        if (packet.player_id() >= session.players) [[unlikely]] {
            logger::debug("Received packet with invalid player ID {} for session {} from {}.",
                packet.player_id(), packet.session_id(), client_addr);
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        auto& client = impl_.clients[packet.session_id()][packet.player_id()];
        const tick_t tick = std::atomic_ref(session.tick).load(std::memory_order::relaxed);
        if (std::atomic_ref(client.tick).load(std::memory_order::relaxed) == tick)
            continue;
        switch (packet.verify(client.key)) {
            using enum data_packet::verify_result;
            case ok: break;
            case too_short: std::unreachable();
            case invalid_size: {
                logger::debug("Received packet with invalid size from {}.", client_addr);
                logger::print_packet(logger::debug, packet.bytes());
                continue;
            }
            case invalid_tag: {
                logger::debug("Received packet with invalid tag for session {} player {} from {}.",
                    packet.session_id(), packet.player_id(), client_addr);
                logger::print_packet(logger::debug, packet.bytes());
                continue;
            }
            default: std::unreachable();
        }
        packet.decrypt(client.key);
        client.last_packet = {
            .addr = client_addr,
            .snapshot_requested = static_cast<bool>(packet.text()[0]),
            .boost = static_cast<bool>(packet.text()[1]),
            .angle = load_float32(packet.text().subspan<4, 4>())
        };
        std::atomic_ref(client.tick).store(tick, std::memory_order::release);
    }
}

void game::add_session(id_t session_id,
    id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    using enum add_session_error;
    impl& impl_ = get_impl();
    gpu::add_session(impl_.sessions, session_id, human_players, ai_players, max_tick, keys);
}
