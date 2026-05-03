#include "transport_doca.hpp"
#include <cuda_runtime.h>

namespace snakeio::doca_transport {
    ssize_t recv_ingress_packet(int sock, std::span<std::byte> buffer, sockaddr_storage& client_addr) noexcept {
        socklen_t client_addr_len = sizeof(client_addr);
        return recvfrom(sock, buffer.data(), buffer.size(), 0,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
    }

    void store_client_addr(gpu::device_state& state, id_t session_id, id_t player_id, const sockaddr_storage& client_addr) noexcept {
        cudaMemcpyAsync(
            state.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
            &client_addr,
            sizeof(sockaddr_storage),
            cudaMemcpyHostToDevice);
    }

    sockaddr_storage load_client_addr(const gpu::device_state& state, id_t session_id, id_t player_id) noexcept {
        sockaddr_storage client_addr{};
        cudaMemcpyAsync(
            &client_addr,
            state.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
            sizeof(sockaddr_storage),
            cudaMemcpyDeviceToHost);
        return client_addr;
    }

    ssize_t send_egress_packet(int sock, std::span<std::byte> bytes, const sockaddr_storage& client_addr) noexcept {
        return sendto(sock, bytes, client_addr);
    }
}
