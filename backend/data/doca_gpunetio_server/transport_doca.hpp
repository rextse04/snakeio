#pragma once
#include <gpu_server/game_kernels.cuh>
#include <network.hpp>
#include <span>

namespace snakeio::doca_transport {
    ssize_t recv_ingress_packet(int sock, std::span<std::byte> buffer, sockaddr_storage& client_addr) noexcept;
    void store_client_addr(gpu::device_state& state, id_t session_id, id_t player_id, const sockaddr_storage& client_addr) noexcept;
    sockaddr_storage load_client_addr(const gpu::device_state& state, id_t session_id, id_t player_id) noexcept;
    ssize_t send_egress_packet(int sock, std::span<std::byte> bytes, const sockaddr_storage& client_addr) noexcept;
}
