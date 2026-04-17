#pragma once
#include <config.hpp>
#include <snake_status.hpp>
#include <vector.hpp>
#include <array>
#include <cstddef>

namespace snakeio::gpu {
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

    struct tick_report {
        bool active;
        bool ended;
        bool has_payload;
        unsigned send_count;
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
        std::byte* plain_delta;
        std::byte* plain_snapshot;
        std::byte* plain_lobby;
        std::byte* plain_termination;
        size_t* plain_delta_size;
        size_t* plain_snapshot_size;
        size_t* plain_lobby_size;
        size_t* plain_termination_size;
        tick_report* report;
        bool* ingress_ok;
        id_t* ingress_session_id;
        id_t* ingress_player_id;
        std::byte* ingress_packet;
        size_t* ingress_packet_size;
        size_t ingress_packet_capacity;
    };

    void init_device_state(device_state& state) noexcept;
    void destroy_device_state(device_state& state) noexcept;

    void add_session_gpu(device_state& state, id_t session_id,
        id_t human_players, id_t ai_players, tick_t max_tick,
        const std::byte* keys_bytes) noexcept;

    void ingest_packet_gpu(device_state& state, const std::byte* packet, size_t bytes_size) noexcept;

    void tick_session_gpu(device_state& state, id_t session_id) noexcept;
}
