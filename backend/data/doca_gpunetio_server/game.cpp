#include "doca_gpunetio_runtime.hpp"
#include <game.hpp>
#include <network.hpp>
#include <logger.hpp>
#include <gpu_server/game_kernels.cuh>
#include <cuda_runtime.h>
#include <new>
#include <chrono>
#include <thread>
#include <atomic>
#include <optional>

using namespace snakeio;

namespace {
    constexpr snakeio::size_t clients_size = game_max_sessions * game_max_players;
}

struct game::impl {
    std::optional<udp_port> data_port;
    gpu::device_state gpu_state;
    doca_gpunetio::runtime runtime;
    std::atomic<std::size_t> ingress_total{};
    std::atomic<std::size_t> ingress_accepted_total{};
    std::size_t last_log_total{};
    std::size_t last_log_accepted{};
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
    if (!impl_.runtime.doca_active()) {
        static bool warned_kernel_data = false;
        if (!warned_kernel_data) {
            warned_kernel_data = true;
            logger::warn(
                "DOCA GPUNetIO inactive — binding kernel UDP data plane [::]:{} (ingress/egress use the "
                "kernel socket; configure DOCA NIC/GPU and SNAKEIO_DOCA_GPUNETIO for GPUNetIO RX).",
                data_plane_ext_port);
        }
        impl_.data_port.emplace("data", sockaddr_in6{
            .sin6_family = AF_INET6,
            .sin6_port = htons(data_plane_ext_port),
            .sin6_addr = in6addr_any,
        });
    }
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
    while (!stop_token.stop_requested()) {
        if (impl_.runtime.doca_active()) {
            const auto [rx_now, accepted_now] =
                impl_.runtime.process_doca_ingress(impl_.gpu_state, stop_token);
            impl_.ingress_total.fetch_add(rx_now, std::memory_order_relaxed);
            impl_.ingress_accepted_total.fetch_add(accepted_now, std::memory_order_relaxed);
            if (rx_now == 0 && !stop_token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            }
        } else {
            if (!impl_.data_port.has_value()) {
                logger::error("Kernel UDP ingress active but data port is not bound.");
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                continue;
            }
            const auto [rx_now, accepted_now] =
                impl_.runtime.process_kernel_udp_ingress(impl_.gpu_state, impl_.data_port->sock(), stop_token);
            impl_.ingress_total.fetch_add(rx_now, std::memory_order_relaxed);
            impl_.ingress_accepted_total.fetch_add(accepted_now, std::memory_order_relaxed);
        }
    }
}

void game::tick(std::stop_token) noexcept {
    impl& impl_ = get_impl();

    if (++impl_.log_tick_counter >= 50) { // ~1 second at 20ms tick
        const std::size_t total_now = impl_.ingress_total.load(std::memory_order_relaxed);
        const std::size_t accepted_now = impl_.ingress_accepted_total.load(std::memory_order_relaxed);
        logger::debug("ingress stats: total rx={}, accepted={} (last ~1s rx={}, accepted={}).",
            total_now,
            accepted_now,
            total_now - impl_.last_log_total,
            accepted_now - impl_.last_log_accepted);
        impl_.last_log_total = total_now;
        impl_.last_log_accepted = accepted_now;
        impl_.log_tick_counter = 0;
    }

    if (impl_.runtime.doca_active()) {
        gpu::tick_active_sessions_gpu(impl_.gpu_state, gpu::tick_host_finalize::sessions_only);
        const int data_sock = impl_.data_port.has_value() ? impl_.data_port->sock() : -1;
        // `sendto` uses the kernel egress socket inside `DocaGpuIngress` when GPUNetIO is active.
        impl_.runtime.emit_egress_batch(impl_.gpu_state, data_sock);
    } else {
        if (!impl_.data_port.has_value()) {
            logger::error("Kernel UDP egress active but data port is not bound.");
            return;
        }
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
            impl_.data_port->send(addr, bytes);
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
