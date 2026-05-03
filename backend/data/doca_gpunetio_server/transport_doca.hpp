#pragma once
#include <config.hpp>
#include <gpu_server/game_kernels.cuh>
#include <network.hpp>
#include <array>
#include <span>
#include <stop_token>

namespace snakeio::doca_transport {
    bool ensure_ingress_path_started() noexcept;
    void shutdown_ingress_path() noexcept;

    bool recv_next_udp_payload(std::stop_token stop_token,
        std::span<std::byte> buffer,
        snakeio::size_t& payload_len,
        sockaddr_storage& client_addr,
        std::array<std::byte, 6>& client_src_eth) noexcept;

    void store_client_addr(gpu::device_state& state, id_t session_id, id_t player_id, const sockaddr_storage& client_addr) noexcept;
    void store_client_eth(std::byte* device_eth_addrs, id_t session_id, id_t player_id, const std::array<std::byte, 6>& src_eth) noexcept;
    sockaddr_storage load_client_addr(const gpu::device_state& state, id_t session_id, id_t player_id) noexcept;
    void load_client_eth(const std::byte* device_eth_addrs, id_t session_id, id_t player_id, std::array<std::byte, 6>& out_eth) noexcept;
    ssize_t send_egress_packet(int sock,
        std::span<std::byte> bytes_gpu,
        snakeio::size_t nbytes,
        const sockaddr_storage& client_addr,
        const std::array<std::byte, 6>& client_eth) noexcept;
}
