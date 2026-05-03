#include "transport_doca.hpp"
#include <game.hpp>
#include <logger.hpp>
#include <packet.hpp>
#include <array>
#include <cuda_runtime.h>
#include <cstring>
#include <new>
#include <span>

using namespace snakeio;

namespace {
    constexpr snakeio::size_t clients_size = game_max_sessions * game_max_players;
}

struct game::impl {
    gpu::device_state gpu_state;
    std::byte* client_eth_macs = nullptr;
};

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
    auto& impl_ = get_impl();
    gpu::init_device_state(impl_.gpu_state);
    gpu::init_client_addrs_gpu(impl_.gpu_state, sizeof(sockaddr_storage) * clients_size);
    void* eth_table = nullptr;
    if (cudaMalloc(&eth_table, 6UZ * clients_size) == cudaSuccess) {
        impl_.client_eth_macs = static_cast<std::byte*>(eth_table);
        cudaMemset(impl_.client_eth_macs, 0, 6UZ * clients_size);
    }
}

game::~game() noexcept {
    impl& impl_ = get_impl();
    if (impl_.client_eth_macs != nullptr) {
        cudaFree(impl_.client_eth_macs);
        impl_.client_eth_macs = nullptr;
    }
    doca_transport::shutdown_ingress_path();
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

void game::port(std::stop_token stop_token, int sock) noexcept {
    (void)sock;
    impl& impl_ = get_impl();
    std::array<std::byte, in_packet_max_text_size + data_packet::header_size> buffer{};
    std::array<std::byte, 6> client_src_eth{};
    sockaddr_storage client_addr{};
    snakeio::size_t payload_len = 0;
    if (!doca_transport::ensure_ingress_path_started()) [[unlikely]] {
        logger::error("Failed to start DOCA GPUNetIO ingress path; data port exiting.");
        std::exit(EXIT_FAILURE);
    }
    while (true) {
        const std::span<std::byte> buffer_view(buffer.data(), buffer.size());
        if (!doca_transport::recv_next_udp_payload(stop_token, buffer_view, payload_len, client_addr, client_src_eth)) [[unlikely]] {
            if (stop_token.stop_requested()) {
                logger::info("DOCA GPUNetIO data port received stop request, exiting.");
            }
            std::exit(EXIT_SUCCESS);
        }

        gpu::ingest_packet_gpu(impl_.gpu_state, buffer.data(), payload_len);
        if (impl_.gpu_state.host_ingress->ok) {
            const id_t session_id = impl_.gpu_state.host_ingress->session_id;
            const id_t player_id = impl_.gpu_state.host_ingress->player_id;
            doca_transport::store_client_addr(impl_.gpu_state, session_id, player_id, client_addr);
            if (impl_.client_eth_macs != nullptr) {
                doca_transport::store_client_eth(impl_.client_eth_macs, session_id, player_id, client_src_eth);
            }
        }
    }
}

void game::tick(std::stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
    gpu::tick_active_sessions_gpu(impl_.gpu_state);

    const unsigned send_count = *impl_.gpu_state.host_send_descs_size;
    for (unsigned j = 0; j < send_count; ++j) {
        const gpu::send_desc& desc = impl_.gpu_state.host_send_descs[j];
        const snakeio::size_t nbytes = static_cast<snakeio::size_t>(desc.bytes_size);
        const std::span<std::byte> bytes(impl_.gpu_state.packet_ring + desc.ring_offset, nbytes);
        const sockaddr_storage addr = doca_transport::load_client_addr(
            impl_.gpu_state, desc.session_id, desc.player_id);
        std::array<std::byte, 6> client_eth{};
        if (impl_.client_eth_macs != nullptr) {
            doca_transport::load_client_eth(impl_.client_eth_macs, desc.session_id, desc.player_id, client_eth);
        }
        doca_transport::send_egress_packet(sock, bytes, nbytes, addr, client_eth);
    }

    for (id_t i = 0; i < game_max_sessions; ++i) {
        if (!sm_[i]) continue;
        if (!impl_.gpu_state.host_session_active[i]) {
            sm_.deallocate(i);
            logger::debug("Session {} ended", i);
        }
    }
}
