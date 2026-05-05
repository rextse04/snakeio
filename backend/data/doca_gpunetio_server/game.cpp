#include "doca_gpunetio_runtime.hpp"
#include <game.hpp>
#include <network.hpp>
#include <logger.hpp>
#include <gpu_server/game_kernels.cuh>
#include <cuda_runtime.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace snakeio;

namespace {
    constexpr snakeio::size_t clients_size = game_max_sessions * game_max_players;
    constexpr snakeio::size_t ingress_batch_capacity = 2048;
}

struct game::impl {
    gpu::device_state gpu_state;
    doca_gpunetio::runtime runtime;
    std::vector<doca_gpunetio::ingress_packet> ingress_batch{ingress_batch_capacity};
    size_t ingress_total{};
    size_t ingress_accepted_total{};
    unsigned log_tick_counter{};
};

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
    auto& impl_ = get_impl();
    if (cudaSetDevice(0) != cudaSuccess) {
        logger::error("cudaSetDevice(0) failed.");
    }
    gpu::init_device_state(impl_.gpu_state);
    impl_.runtime.try_init_doca(impl_.gpu_state);
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

int game::open_data_port() noexcept {
    // GPUNetIO ingress uses the GPU RXQ; no Linux UDP bind is required. Return a stable handle
    // for APIs that expect a non-negative fd (ignored by `tick` when DOCA RX is active).
    static int placeholder = -1;
    if (placeholder < 0) {
        placeholder = ::open("/dev/null", O_RDWR);
        if (placeholder < 0) {
            logger::error("open_data_port: /dev/null: {}.", std::strerror(errno));
            return -1;
        }
    }
    return placeholder;
}

void game::port(std::stop_token stop_token, int) noexcept {
    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(2ms);
    }
}

void game::tick(std::stop_token, int sock) noexcept {
    impl& impl_ = get_impl();

    size_t accepted_now = 0;
    size_t last_tick_rx = 0;
    if (impl_.runtime.doca_active()) {
        // `sock` is the placeholder from `open_data_port`; ingress is GPU-only.
        accepted_now = impl_.runtime.poll_ingress_batch(impl_.gpu_state, sock, impl_.ingress_batch);
        last_tick_rx = accepted_now;
        impl_.ingress_total += accepted_now;
        impl_.ingress_accepted_total += accepted_now;
    } else {
        const size_t ingress_count = impl_.runtime.poll_ingress_batch(impl_.gpu_state, sock, impl_.ingress_batch);
        last_tick_rx = ingress_count;
        impl_.ingress_total += ingress_count;
        for (size_t i = 0; i < ingress_count; ++i) {
            const auto& pkt = impl_.ingress_batch[i];
            gpu::ingest_packet_gpu(impl_.gpu_state, pkt.bytes.data(), pkt.size);
            if (!impl_.gpu_state.host_ingress->ok) {
                continue;
            }
            ++accepted_now;
            const id_t session_id = impl_.gpu_state.host_ingress->session_id;
            const id_t player_id = impl_.gpu_state.host_ingress->player_id;
            cudaMemcpyAsync(impl_.gpu_state.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
                &pkt.source_addr,
                sizeof(sockaddr_storage),
                cudaMemcpyHostToDevice,
                reinterpret_cast<cudaStream_t>(impl_.gpu_state.stream));
        }
        impl_.ingress_accepted_total += accepted_now;
        cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(impl_.gpu_state.stream));
    }
    if (++impl_.log_tick_counter >= 50) { // ~1 second at 20ms tick
        logger::debug("ingress batch stats: total rx={}, accepted={} (last tick rx={}, accepted={}).",
            impl_.ingress_total, impl_.ingress_accepted_total, last_tick_rx, accepted_now);
        impl_.log_tick_counter = 0;
    }

    if (impl_.runtime.doca_active()) {
        gpu::tick_active_sessions_gpu(impl_.gpu_state, gpu::tick_host_finalize::sessions_only);
        // `sendto` uses the kernel egress socket inside `DocaGpuIngress`, not `sock`.
        impl_.runtime.emit_egress_batch(impl_.gpu_state, sock);
    } else {
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
            sendto(sock, bytes, addr);
        }
    }

    for (id_t i = 0; i < game_max_sessions; ++i) {
        if (!sm_[i]) continue;
        if (!impl_.gpu_state.host_session_active[i]) {
            sm_.deallocate(i);
            logger::debug("Session {} ended", i);
        }
    }
}
