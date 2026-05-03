#include "../gpu_server/game_kernels.cuh"
#include "transport.hpp"
#include <game.hpp>
#include <logger.hpp>
#include <packet.hpp>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

using namespace snakeio;

namespace {
    constexpr snakeio::size_t clients_size = game_max_sessions * game_max_players;

    class doca_runtime {
    public:
        doca_runtime() {
            // Hard-fail startup when DOCA runtime cannot be loaded.
            handle_ = dlopen("libdoca_gpunetio.so", RTLD_NOW | RTLD_LOCAL);
            if (!handle_) {
                logger::error("Failed to initialize DOCA GPUNetIO runtime: {}.", dlerror());
                std::exit(EXIT_FAILURE);
            }
        }

        ~doca_runtime() noexcept {
            if (handle_) {
                dlclose(handle_);
            }
        }

        doca_runtime(const doca_runtime&) = delete;
        doca_runtime& operator=(const doca_runtime&) = delete;
        doca_runtime(doca_runtime&&) = delete;
        doca_runtime& operator=(doca_runtime&&) = delete;

    private:
        void* handle_ = nullptr;
    };
}

struct game::impl {
    [[maybe_unused]] doca_runtime doca;
    gpu::device_state gpu_state{};
    doca_gpunetio::transport transport;
    std::mutex mutex;
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
    std::scoped_lock lock(impl_.mutex);
    gpu::add_session_gpu(impl_.gpu_state, session_id, human_players, ai_players, max_tick,
        reinterpret_cast<const std::byte*>(keys.data()));
    sm_.activate(session_id);
}

void game::port(std::stop_token stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
    std::byte buffer[in_packet_max_text_size + data_packet::header_size]{};
    doca_gpunetio::ingress_packet packet{};
    while (true) {
        if (stop_token.stop_requested()) [[unlikely]] {
            logger::info("Data port received stop request, exiting.");
            return;
        }
        impl_.transport.progress();
        if (!impl_.transport.recv(stop_token, sock, std::span(buffer), packet)) {
            continue;
        }
        {
            std::scoped_lock lock(impl_.mutex);
            gpu::ingest_packet_gpu(impl_.gpu_state, buffer, packet.bytes_size);
            // session_id/player_id are read from packet AAD by GPU ingress parsing.
            if (*impl_.gpu_state.ingress_ok) {
                const id_t gpu_session_id = *impl_.gpu_state.ingress_session_id;
                const id_t gpu_player_id = *impl_.gpu_state.ingress_player_id;
                if (gpu_session_id != packet.session_id || gpu_player_id != packet.player_id) {
                    logger::warn("Ingress AAD mismatch: cpu=({}, {}), gpu=({}, {}).",
                        packet.session_id, packet.player_id, gpu_session_id, gpu_player_id);
                    continue;
                }
                std::memcpy(impl_.gpu_state.client_addrs
                        + gpu::client_index(packet.session_id, packet.player_id) * sizeof(sockaddr_storage),
                    &packet.client_addr, sizeof(sockaddr_storage));
                impl_.transport.observe_ingress_peer(packet.session_id, packet.player_id, packet.client_addr);
            }
        }
    }
}

void game::tick(std::stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
    std::scoped_lock lock(impl_.mutex);
    impl_.transport.progress();
    gpu::tick_active_sessions_gpu(impl_.gpu_state);

    const unsigned send_count = *impl_.gpu_state.send_descs_size;
    for (unsigned j = 0; j < send_count; ++j) {
        const gpu::send_desc& desc = impl_.gpu_state.send_descs[j];
        std::span<std::byte> bytes(impl_.gpu_state.packet_ring + desc.ring_offset, desc.bytes_size);
        sockaddr_storage addr;
        std::memcpy(&addr,
            impl_.gpu_state.client_addrs + gpu::client_index(desc.session_id, desc.player_id) * sizeof(sockaddr_storage),
            sizeof(sockaddr_storage));
        if (impl_.transport.try_send_cached_peer(sock, bytes, desc.session_id, desc.player_id)) {
            continue;
        }
        impl_.transport.send(sock, bytes, desc.session_id, desc.player_id, addr);
    }
    impl_.transport.flush_tx(sock);

    for (id_t i = 0; i < game_max_sessions; ++i) {
        if (!sm_[i]) continue;
        if (!impl_.gpu_state.sessions[i].active) {
            sm_.deallocate(i);
            logger::debug("Session {} ended", i);
        }
    }
}

