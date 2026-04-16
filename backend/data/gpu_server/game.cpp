#include "game_kernels.cuh"
#include <game.hpp>
#include <network.hpp>
#include <packet.hpp>
#include <logger.hpp>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <new>

using namespace snakeio;

namespace {
    constexpr snakeio::size_t clients_size = game_max_sessions * game_max_players;
}

struct game::impl {
    gpu::device_state gpu_state{};
    std::array<std::array<sockaddr_storage, game_max_players>, game_max_sessions> addrs{};
    std::mutex mutex;
};

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
    gpu::init_device_state(get_impl().gpu_state);
}

void game::add_session(id_t session_id,
    id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    impl& impl_ = get_impl();
    std::scoped_lock lock(impl_.mutex);
    gpu::add_session_gpu(impl_.gpu_state, session_id, human_players, ai_players, max_tick,
        reinterpret_cast<const std::byte*>(keys.data()));
    sm_.activate(session_id);
}

void game::port(std::stop_token stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
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
        {
            std::scoped_lock lock(impl_.mutex);
            gpu::ingest_packet_gpu(impl_.gpu_state, buffer, static_cast<snakeio::size_t>(recv_len));
            if (*impl_.gpu_state.ingress_ok) {
                const id_t session_id = *impl_.gpu_state.ingress_session_id;
                const id_t player_id = *impl_.gpu_state.ingress_player_id;
                impl_.addrs[session_id][player_id] = client_addr;
            }
        }
    }
}

void game::tick(std::stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
    std::scoped_lock lock(impl_.mutex);
    for (id_t i = 0; i < game_max_sessions; ++i) {
        if (!sm_[i]) continue;
        gpu::tick_session_gpu(impl_.gpu_state, i);
        const gpu::tick_report& report = *impl_.gpu_state.report;
        if (!report.active || !report.has_payload) {
            if (report.ended) {
                sm_.deallocate(i);
                logger::debug("Session {} ended", i);
            }
            continue;
        }
        const unsigned send_count = report.send_count;
        for (unsigned j = 0; j < send_count; ++j) {
            const gpu::send_desc& desc = impl_.gpu_state.send_descs[j];
            std::span<std::byte> bytes(impl_.gpu_state.packet_ring + desc.ring_offset, desc.bytes_size);
            sendto(sock, bytes, impl_.addrs[desc.session_id][desc.player_id]);
        }
        if (report.ended) {
            sm_.deallocate(i);
            logger::debug("Session {} ended", i);
        }
    }
}
