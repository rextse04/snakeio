#include "game_kernels.cuh"
#include <game.hpp>
#include <network.hpp>
#include <packet.hpp>
#include <logger.hpp>
#include <cstring>
#include <new>
#include <cuda_runtime.h>

using namespace snakeio;

namespace {
    constexpr snakeio::size_t clients_size = game_max_sessions * game_max_players;
}


struct game::impl {
    udp_port data_port{"data", {
        .sin6_family = AF_INET6,
        .sin6_port = htons(data_plane_ext_port),
        .sin6_addr = in6addr_any
    }};
    gpu::device_state gpu_state;
};

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
    auto& impl_ = get_impl();
    gpu::init_device_state(impl_.gpu_state);
    gpu::init_client_addrs_gpu(impl_.gpu_state, sizeof(sockaddr_storage) * clients_size);
}

game::~game() noexcept {
    impl& impl_ = get_impl();
    gpu::destroy_client_addrs_gpu(impl_.gpu_state);
    gpu::destroy_device_state(impl_.gpu_state);
    impl_.~impl();
}

void game::add_session(id_t session_id,
    id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    impl& impl_ = get_impl();
    gpu::add_session_gpu(impl_.gpu_state, session_id, human_players, ai_players, max_tick,
        reinterpret_cast<const std::byte*>(keys.data()));
    sm_.activate(session_id);
}

void game::port(std::stop_token stop_token) noexcept {
    impl& impl_ = get_impl();
    std::byte buffer[in_packet_max_text_size + data_packet::header_size];
    while (true) {
        if (stop_token.stop_requested()) [[unlikely]] {
            impl_.data_port.log(logger::info, "Received stop request, exiting.");
            return;
        }
        const auto [client_addr, recv_len] = impl_.data_port.recv(buffer);
        if (recv_len == -1) continue;
        gpu::ingest_packet_gpu(impl_.gpu_state, buffer, static_cast<size_t>(recv_len));
        if (impl_.gpu_state.host_ingress->ok) {
            const id_t session_id = impl_.gpu_state.host_ingress->session_id;
            const id_t player_id = impl_.gpu_state.host_ingress->player_id;
            cudaMemcpyAsync(impl_.gpu_state.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
                &client_addr, sizeof(sockaddr_storage), cudaMemcpyHostToDevice,
                reinterpret_cast<cudaStream_t>(impl_.gpu_state.stream));
            cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(impl_.gpu_state.stream));
        }
    }
}

void game::tick(std::stop_token) noexcept {
    impl& impl_ = get_impl();
    gpu::tick_active_sessions_gpu(impl_.gpu_state);

    const unsigned send_count = *impl_.gpu_state.host_send_descs_size;
    for (unsigned j = 0; j < send_count; ++j) {
        const gpu::send_desc& desc = impl_.gpu_state.host_send_descs[j];
        cudaMemcpy(impl_.gpu_state.host_packet_copy,
            impl_.gpu_state.packet_ring + desc.ring_offset,
            desc.bytes_size,
            cudaMemcpyDeviceToHost);
        const std::span bytes(impl_.gpu_state.host_packet_copy, desc.bytes_size);
        sockaddr_storage addr{};
        cudaMemcpy(&addr,
            impl_.gpu_state.client_addrs + gpu::client_index(desc.session_id, desc.player_id) * sizeof(sockaddr_storage),
            sizeof(sockaddr_storage),
            cudaMemcpyDeviceToHost);
        impl_.data_port.send(addr, bytes);
    }

    for (id_t i = 0; i < game_max_sessions; ++i) {
        if (!sm_[i]) continue;
        if (!impl_.gpu_state.host_session_active[i]) {
            sm_.deallocate(i);
            logger::debug("Session {} ended", i);
        }
    }
}
