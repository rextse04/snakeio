#include "doca_gpunetio_server.hpp"
#include <gpu_server/game_kernels.cuh>
#include <game.hpp>
#include <config.hpp>
#include <network.hpp>
#include <packet.hpp>
#include <logger.hpp>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <new>

using namespace snakeio;

namespace {
    constexpr snakeio::size_t clients_size = game_max_sessions * game_max_players;

    void copy_env_string(const char* key, std::span<char> out) noexcept {
        if (out.empty()) return;
        out[0] = '\0';
        const char* value = std::getenv(key);
        if (value == nullptr || *value == '\0') return;
        std::snprintf(out.data(), out.size(), "%s", value);
    }

    doca_gpunetio_server::config selected_transport_config() noexcept {
        doca_gpunetio_server::config cfg{};
        cfg.backend = doca_gpunetio_server::backend_kind::doca_gpunetio;
        cfg.port = data_plane_ext_port;

        copy_env_string("SNAKEIO_DOCA_GPU_PCI", std::span(cfg.gpu_pci_addr));
        copy_env_string("SNAKEIO_DOCA_NIC_PCI", std::span(cfg.nic_pci_addr));
        return cfg;
    }
}

struct game::impl {
    gpu::device_state gpu_state{};
    doca_gpunetio_server::context transport{};
    std::mutex mutex;
};

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
    auto& impl_ = get_impl();
    gpu::init_device_state(impl_.gpu_state);
    gpu::init_client_addrs_gpu(impl_.gpu_state, sizeof(sockaddr_storage) * clients_size);
    doca_gpunetio_server::config transport_cfg = selected_transport_config();
    if (!doca_gpunetio_server::start(impl_.transport, transport_cfg, &impl_.gpu_state)) {
        logger::error("Failed to initialize DOCA GPUNetIO transport; aborting process.");
        std::exit(EXIT_FAILURE);
    }
}

game::~game() noexcept {
    impl& impl_ = get_impl();
    doca_gpunetio_server::stop(impl_.transport);
    gpu::destroy_client_addrs_gpu(impl_.gpu_state);
    gpu::destroy_device_state(impl_.gpu_state);
    impl_.~impl();
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
                std::memcpy(impl_.gpu_state.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
                    &client_addr, sizeof(sockaddr_storage));
            }
        }
    }
}

void game::tick(std::stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
    std::scoped_lock lock(impl_.mutex);
    gpu::tick_active_sessions_gpu(impl_.gpu_state);

    for (id_t i = 0; i < game_max_sessions; ++i) {
        if (!sm_[i]) continue;
        if (!impl_.gpu_state.sessions[i].active) {
            sm_.deallocate(i);
            logger::debug("Session {} ended", i);
        }
    }
}
