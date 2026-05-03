#pragma once

#include <config.hpp>
#include <network.hpp>
#include <packet.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <stop_token>

struct doca_gpu;
struct doca_dev;
struct doca_eth_rxq;
struct doca_eth_txq;
struct doca_mmap;
struct doca_buf_inventory;
struct doca_ctx;
struct doca_pe;

namespace snakeio::doca_gpunetio {
    struct ingress_packet {
        size_t bytes_size = 0;
        sockaddr_storage client_addr{};
        id_t session_id = 0;
        id_t player_id = 0;
    };

    class transport final {
    public:
        static constexpr size_t clients_size_ = game_max_sessions * game_max_players;
        static constexpr size_t tx_stage_capacity_ = 512;
        static constexpr size_t tx_stage_capacity_max_ = 2048;
        static constexpr size_t staged_packet_capacity_ = packet_chunk_size + data_packet::header_size;

        struct shadow_tx_desc {
            std::uint_least32_t bytes_size = 0;
            std::uint_least32_t session_id = 0;
            std::uint_least32_t player_id = 0;
            std::uint_least32_t ring_slot = 0;
            std::uint_least32_t payload_offset = 0;
            sockaddr_storage addr{};
        };

        struct staged_tx_item {
            std::array<std::byte, staged_packet_capacity_> bytes{};
            size_t bytes_size = 0;
            id_t session_id = 0;
            id_t player_id = 0;
            sockaddr_storage addr{};
        };

        transport();
        ~transport() noexcept;
        transport(const transport&) = delete;
        transport& operator=(const transport&) = delete;
        transport(transport&&) noexcept = delete;
        transport& operator=(transport&&) noexcept = delete;

        void progress() noexcept;
        void flush_tx(int sock) noexcept;
        void observe_ingress_peer(id_t session_id,
            id_t player_id,
            const sockaddr_storage& addr) noexcept;
        [[nodiscard]] bool try_send_cached_peer(int sock,
            std::span<std::byte> bytes,
            id_t session_id,
            id_t player_id) noexcept;
        bool recv(std::stop_token stop_token,
            int sock,
            std::span<std::byte> buffer,
            ingress_packet& packet) noexcept;
        void send(int sock,
            std::span<std::byte> bytes,
            id_t session_id,
            id_t player_id,
            const sockaddr_storage& fallback_addr) noexcept;

    private:
        [[nodiscard]] bool try_native_tx_submit(const staged_tx_item& item,
            const shadow_tx_desc& desc) noexcept;
        [[nodiscard]] bool stage_tx_or_drop(int sock,
            std::span<std::byte> bytes,
            id_t session_id,
            id_t player_id,
            const sockaddr_storage& addr) noexcept;

        std::array<sockaddr_storage, clients_size_> peer_addrs_{};
        std::array<bool, clients_size_> peer_known_{};
        std::array<staged_tx_item, tx_stage_capacity_max_> staged_tx_{};
        size_t staged_head_ = 0;
        size_t staged_size_ = 0;
        size_t tx_stage_capacity_runtime_ = tx_stage_capacity_;
        bool tx_overflow_warned_ = false;
        bool tx_packet_size_warned_ = false;
        bool native_tx_requested_ = true;
        bool native_tx_active_ = true;
        bool native_tx_proxy_error_logged_ = false;
        bool native_tx_mirror_enabled_ = false;
        bool native_tx_mirror_strict_ = false;
        bool native_tx_mirror_error_logged_ = false;
        shadow_tx_desc* shadow_descs_cpu_ = nullptr;
        std::byte* shadow_payloads_cpu_ = nullptr;
        size_t scratch_bytes_ = 0;
        size_t scratch_desc_bytes_ = 0;
        size_t scratch_payload_bytes_ = 0;
        struct tx_stats {
            std::uint_least64_t staged = 0;
            std::uint_least64_t flushed = 0;
            std::uint_least64_t fallback_overflow = 0;
            std::uint_least64_t fallback_oversize = 0;
            std::uint_least64_t cache_hit = 0;
            std::uint_least64_t cache_miss = 0;
            std::uint_least64_t shadow_desc_prepared = 0;
            std::uint_least64_t native_attempted = 0;
            std::uint_least64_t native_submitted = 0;
            std::uint_least64_t native_fallback_disabled = 0;
            std::uint_least64_t native_fallback_error = 0;
            std::uint_least64_t dropped_oversize_no_fallback = 0;
            std::uint_least64_t dropped_overflow_no_fallback = 0;
            std::uint_least64_t dropped_submit_no_fallback = 0;
            std::uint_least64_t native_mirrored_packets = 0;
            std::uint_least64_t native_mirrored_bytes = 0;
            std::uint_least64_t native_mirror_error = 0;
            std::uint_least64_t proxy_rx_ok = 0;
            std::uint_least64_t proxy_tx_ok = 0;
            std::uint_least64_t proxy_rx_error = 0;
            std::uint_least64_t proxy_tx_error = 0;
        } tx_stats_{};
        doca_gpu* gpu_dev_ = nullptr;
        doca_dev* dev_ = nullptr;
        doca_eth_rxq* rxq_ = nullptr;
        doca_eth_txq* txq_ = nullptr;
        doca_mmap* tx_mmap_ = nullptr;
        doca_buf_inventory* tx_buf_inventory_ = nullptr;
        doca_ctx* rx_ctx_ = nullptr;
        doca_ctx* tx_ctx_ = nullptr;
        doca_pe* pe_ = nullptr;
        void* scratch_gpu_ptr_ = nullptr;
        void* scratch_cpu_ptr_ = nullptr;
    };
}

