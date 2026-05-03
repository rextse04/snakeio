#pragma once
#include <config.hpp>
#include <snake_status.hpp>
#include <vector.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace snakeio::gpu {
    // `full` copies send descriptors to host for CPU transports; `sessions_only` leaves them on device
    // for DOCA batched GPU egress (see doca_gpunetio_runtime::emit_tick_egress_on_stream).
    enum class tick_host_finalize : std::uint8_t {
        full = 0,
        sessions_only = 1,
    };

    __host__ __device__ constexpr size_t client_index(id_t session_id, id_t player_id) noexcept {
        return static_cast<size_t>(session_id) * game_max_players + player_id;
    }
    __host__ __device__ constexpr size_t snake_segment_index(id_t player_id, size_t seg_id) noexcept {
        return static_cast<size_t>(player_id) * snake_max_length + seg_id;
    }

    struct snake_status_info {
        snake_status_t status;
        unsigned char data;
    };


    struct client_state {
        key_t key;
    };

    struct out_delta_state {
        size_t foods_added_size;
        size_t foods_removed_size;
        std::array<vector2d, game_max_food> foods_added_poss;
        std::array<scalar_t, game_max_food> foods_added_widths;
        std::array<scalar_t, game_max_food> foods_removed_xs;
        std::array<scalar_t, game_max_food> foods_removed_ys;
    };

    struct session_state {
        bool active;
        id_t players;
        id_t human_players;
        tick_t tick;
        tick_t max_tick;
        scalar_t width;
        scalar_t height;
        std::array<scalar_t, game_max_players> snake_speeds;
        std::array<scalar_t, game_max_players> snake_angles;
        std::array<scalar_t, game_max_players> snake_widths;
        std::array<scalar_t, game_max_players> snake_frac_lengths;
        std::array<score_t, game_max_players> snake_scores;
        std::array<boost_t, game_max_players> snake_boosts;
        std::array<snake_status_info, game_max_players> snake_statuses;
        std::array<bool, game_max_players> snake_humans;
        std::array<vector2d, game_max_players * snake_max_length> snake_segments;
        std::array<vector2d, game_max_food> food_poss;
        std::array<scalar_t, game_max_food> food_widths;
        size_t food_size;
        out_delta_state delta;
        std::array<tick_t, game_max_players> in_packet_ticks;
        std::array<bool, game_max_players> in_packet_snapshot_requested;
        std::array<bool, game_max_players> in_packet_boost;
        std::array<scalar_t, game_max_players> in_packet_angle;
        // Scratch used across phase kernels.
        std::array<unsigned char, game_max_players> kill_flags;
        std::array<snake_status_info, game_max_players> kill_reasons;
        std::array<unsigned char, game_max_food> food_removed_flags;
    };

    struct send_desc {
        id_t session_id;
        id_t player_id;
        size_t ring_offset;
        size_t bytes_size;
    };

    struct device_state {
        session_state* sessions;
        client_state* clients;
        tick_t* client_ticks;
        bool* client_last_snapshot_requested;
        bool* client_last_boost;
        scalar_t* client_last_angle;
        std::byte* packet_ring;
        size_t packet_ring_capacity;
        unsigned* packet_ring_head;
        send_desc* send_descs;
        unsigned* send_descs_size;
        unsigned send_descs_capacity;
        bool* ingress_ok;
        id_t* ingress_session_id;
        id_t* ingress_player_id;
        std::byte* ingress_packet;
        size_t* ingress_packet_size;
        size_t ingress_packet_capacity;
        std::byte* client_addrs;
        std::uint_least64_t rng_seed;
        std::uint_least64_t rng_offset;
        void* rng_states;
        size_t rng_states_size;

        // Owned runtime resources previously kept as file-scope globals in game.cu.
        void* snake_spatial_set;
        void* food_spatial_set;
        std::byte* plain_delta_all;
        size_t* plain_delta_sizes_all;
        std::byte* plain_lobby_all;
        size_t* plain_lobby_sizes_all;
        std::byte* plain_snapshot_all;
        size_t* plain_snapshot_sizes_all;
        std::byte* plain_termination_all;
        size_t* plain_termination_sizes_all;
        key_t* add_session_keys;

        // Reused per-tick masks for batched multi-session orchestration.
        bool* tick_masks;
        bool* tick_active_mask;
        bool* tick_gt0_mask;
        bool* tick_lobby_emit_mask;
        bool* tick_tick0_snapshot_emit_mask;
        bool* tick_delta_emit_mask;
        bool* tick_term_emit_mask;
        bool* tick_inc_mask;
        unsigned* tick_flags;

        bool* session_active_flags;
        void* stream{};
        // Device index used for `cudaMalloc` / `cudaStreamCreate` (set in init_device_state).
        int cuda_device_id = -1;

        struct ingress_host_copy {
            bool ok;
            id_t session_id;
            id_t player_id;
        };
        ingress_host_copy* host_ingress{};
        unsigned* host_tick_flags{};
        bool* host_session_active{};
        unsigned* host_send_descs_size{};
        send_desc* host_send_descs{};
        std::byte* host_packet_copy{};
    };

    void init_device_state(device_state& state);
    void destroy_device_state(device_state& state) noexcept;

    void add_session_gpu(device_state& state, id_t session_id,
        id_t human_players, id_t ai_players, tick_t max_tick,
        const std::byte* keys_bytes) noexcept;

    void ingest_packet_gpu(device_state& state, const std::byte* packet, size_t bytes_size) noexcept;

    void init_client_addrs_gpu(device_state& state, size_t bytes_size) noexcept;
    void destroy_client_addrs_gpu(device_state& state) noexcept;

    void tick_active_sessions_gpu(device_state& state,
        tick_host_finalize host_finalize = tick_host_finalize::full) noexcept;
}
