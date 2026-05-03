#include "transport_doca.hpp"
#include "doca_gpunetio_runtime.hpp"

#include <doca_error.h>
#include <logger.hpp>

#include <cuda_runtime.h>
#include <vector>

namespace snakeio::doca_transport {
    bool ensure_ingress_path_started() noexcept {
        if (doca_gpunetio_runtime::started()) {
            return true;
        }
        if (doca_gpunetio_runtime::init() != DOCA_SUCCESS) {
            return false;
        }
        return doca_gpunetio_runtime::start_recv(nullptr) == DOCA_SUCCESS;
    }

    void shutdown_ingress_path() noexcept {
        doca_gpunetio_runtime::shutdown();
    }

    bool recv_next_udp_payload(std::stop_token stop_token,
        std::span<std::byte> buffer,
        snakeio::size_t& payload_len,
        sockaddr_storage& client_addr,
        std::array<std::byte, 6>& client_src_eth) noexcept {
        return doca_gpunetio_runtime::pop_udp_payload(
            std::move(stop_token), buffer, payload_len, client_addr, client_src_eth);
    }

    void store_client_addr(gpu::device_state& state, id_t session_id, id_t player_id, const sockaddr_storage& client_addr) noexcept {
        cudaMemcpyAsync(state.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
            &client_addr,
            sizeof(sockaddr_storage),
            cudaMemcpyHostToDevice,
            reinterpret_cast<cudaStream_t>(state.stream));
        cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(state.stream));
    }

    void store_client_eth(std::byte* device_eth_addrs, id_t session_id, id_t player_id, const std::array<std::byte, 6>& src_eth) noexcept {
        cudaMemcpy(device_eth_addrs + gpu::client_index(session_id, player_id) * 6U,
            src_eth.data(),
            6,
            cudaMemcpyHostToDevice);
    }

    sockaddr_storage load_client_addr(const gpu::device_state& state, id_t session_id, id_t player_id) noexcept {
        sockaddr_storage client_addr{};
        cudaMemcpy(&client_addr,
            state.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
            sizeof(sockaddr_storage),
            cudaMemcpyDeviceToHost);
        return client_addr;
    }

    void load_client_eth(const std::byte* device_eth_addrs, id_t session_id, id_t player_id, std::array<std::byte, 6>& out_eth) noexcept {
        cudaMemcpy(out_eth.data(),
            device_eth_addrs + gpu::client_index(session_id, player_id) * 6U,
            6,
            cudaMemcpyDeviceToHost);
    }

    ssize_t send_egress_packet(
        std::span<std::byte> bytes_gpu,
        snakeio::size_t nbytes,
        const sockaddr_storage& client_addr,
        const std::array<std::byte, 6>& client_eth) noexcept {
        return doca_gpunetio_runtime::send_udp_datagram_gpu(
            bytes_gpu.data(), nbytes, client_addr, client_eth.data());
    }

    void emit_tick_egress_batch(void* game_cuda_stream,
        gpu::device_state& state,
        const std::byte* client_eth_dev) noexcept {
        if (state.client_addrs == nullptr) {
            return;
        }
        if (doca_gpunetio_runtime::emit_tick_egress_on_stream(game_cuda_stream, state, client_eth_dev)) {
            return;
        }

        logger::warn("DOCA batched GPU egress failed; falling back to per-packet host sends.");
        if (state.cuda_device_id >= 0) {
            cudaSetDevice(state.cuda_device_id);
        }
        unsigned n = 0;
        if (cudaMemcpy(&n, state.send_descs_size, sizeof(unsigned), cudaMemcpyDeviceToHost) != cudaSuccess) {
            return;
        }
        if (n > state.send_descs_capacity) {
            return;
        }
        if (n > 0 &&
            cudaMemcpy(state.host_send_descs, state.send_descs, sizeof(gpu::send_desc) * n, cudaMemcpyDeviceToHost) !=
                cudaSuccess) {
            return;
        }
        for (unsigned j = 0; j < n; ++j) {
            const gpu::send_desc& desc = state.host_send_descs[j];
            const snakeio::size_t nbytes = static_cast<snakeio::size_t>(desc.bytes_size);
            const std::span<std::byte> bytes(state.packet_ring + desc.ring_offset, nbytes);
            const sockaddr_storage addr = load_client_addr(state, desc.session_id, desc.player_id);
            std::array<std::byte, 6> client_eth{};
            if (client_eth_dev != nullptr) {
                load_client_eth(client_eth_dev, desc.session_id, desc.player_id, client_eth);
            }
            send_egress_packet(bytes, nbytes, addr, client_eth);
        }
    }
}
