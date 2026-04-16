#include "impl.cuh"
#include "cuda.cuh"
#include "game_kernels.cuh"
#include "tick_core.cuh"
#include <game.hpp>
#include <logger.hpp>
#include <packet.hpp>
#include <network.hpp>
#include <utils.hpp>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <ranges>
#include <span>

using namespace snakeio;
using namespace snakeio::gpu;

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
}

void game::add_session(id_t session_id,
    id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    impl& impl_ = get_impl();
    add_session_req req;
    req.human_players = human_players;
    req.ai_players = ai_players;
    req.max_tick = max_tick;
    std::ranges::copy(keys, req.keys);
    cudaMemcpyAsync(impl_.d_add_req + session_id, &req, sizeof(req),
        cudaMemcpyHostToDevice, impl_.cuda_streams[0]);
    cudaStreamSynchronize(impl_.cuda_streams[0]);
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

        data_packet packet(buffer, recv_len);
        if (recv_len <= data_packet::header_size) [[unlikely]] {
            logger::debug("Received packet that is too short from {}.", client_addr);
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        const id_t session_id = packet.session_id(), player_id = packet.player_id();
        if (session_id >= game_max_sessions || player_id >= game_max_players) [[unlikely]] {
            logger::debug("Received packet with invalid session ID ({}) or player ID ({} from {}.",
                session_id, player_id, client_addr);
            continue;
        }
        impl_.addrs[session_id][player_id] = client_addr;
    }
}

void game::tick(std::stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
    {
        std::scoped_lock inbox_lock(impl_.inbox_mutex);
        std::swap(impl_.inbox, impl_.inbox_back_);
    }
    cudaMemcpyAsync(impl_.device_inbox, impl_.inbox.data(), sizeof(impl::inbox_type),
        cudaMemcpyHostToDevice, impl_.cuda_streams[0]);

    tick_core(impl_, impl_.global_tick);
    cudaMemcpyAsync(&impl_.host_tick_result, impl_.device_tick_result, sizeof(session_tick_result),
        cudaMemcpyDeviceToHost, impl_.cuda_streams[0]);
    cudaStreamSynchronize(impl_.cuda_streams[0]);

    for (id_t session_id = 0; session_id < game_max_sessions; ++session_id) {
        if (!sm_[session_id]) continue;
        const auto meta = impl_.host_meta[session_id];
        if (!meta.active) continue;

        launch_tick(impl_, session_id);
        cudaMemcpyAsync(&impl_.host_tick_result, impl_.device_tick_result, sizeof(gpu::session_tick_result),
            cudaMemcpyDeviceToHost, impl_.cuda_streams[0]);
        cudaStreamSynchronize(impl_.cuda_streams[0]);

        if (!impl_.host_tick_result.active) continue;

        for (id_t player_id = 0; player_id < impl_.host_tick_result.human_players; ++player_id) {
            if (impl_.host_tick_result.send_lobby && !impl_.host_tick_result.joined[player_id]) {
                continue;
            }

            std::span<const std::byte> text;
            if (impl_.host_tick_result.send_lobby) {
                text = {impl_.host_tick_result.lobby_text, impl_.host_tick_result.lobby_size};
            } else if (impl_.host_tick_result.terminate) {
                text = {impl_.host_tick_result.termination_text, impl_.host_tick_result.termination_size};
            } else if (impl_.host_tick_result.snapshot_requested[player_id]) {
                text = {impl_.host_tick_result.snapshot_text, impl_.host_tick_result.snapshot_size};
            } else {
                text = {impl_.host_tick_result.delta_text, impl_.host_tick_result.delta_size};
            }

            const sockaddr_storage addr = impl_.inbox_back[global_id(session_id, player_id)].addr;
            std::byte buffer[data_packet::header_size + packet_chunk_size]{};
            data_packet packet(buffer);
            packet.session_id(session_id);
            packet.player_id(player_id);
            packet.sender(data_packet::sender_t::server);
            packet.chunk_id(0);
            store_32(packet.nonce_part(), impl_.host_tick_result.tick);

            const auto chunks = text | std::views::chunk(packet_chunk_size);
            packet.total_chunks(chunks.size());
            for (const auto& chunk : chunks) {
                data_packet chunk_packet(buffer, chunk.size() + data_packet::header_size);
                std::ranges::copy(chunk, chunk_packet.text().begin());
                chunk_packet.encrypt(impl_.d_keys[session_id][player_id]);
                sendto(sock, chunk_packet.bytes(), addr);
                packet.chunk_id(packet.chunk_id() + 1);
            }
        }

        const tick_t tick = std::atomic_ref(impl_.session_ticks[session_id]).load(std::memory_order::relaxed);
        if (impl_.host_tick_result.terminate) {
            impl_.host_meta[session_id].active = false;
            sm_.deallocate(session_id);
            continue;
        }
        if (!impl_.host_tick_result.send_lobby) {
            std::atomic_ref(impl_.session_ticks[session_id]).store(tick + 1, std::memory_order::relaxed);
        }
    }
}

