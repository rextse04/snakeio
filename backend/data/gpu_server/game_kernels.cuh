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

    struct snake_status_info {
        snake_status_t status;
        unsigned char data;
    };

    struct snake_state {
        scalar_t speed;
        scalar_t angle;
        scalar_t width;
        scalar_t frac_length;
        score_t score;
        boost_t boost;
        snake_status_info status;
        bool human;
        std::array<vector2d, snake_max_length> segments;
    };

    struct food_state {
        vector2d pos;
        scalar_t width;
    };

    struct in_packet_state {
        bool snapshot_requested;
        bool boost;
        scalar_t angle;
    };

    struct in_packet_info_state : in_packet_state {
        tick_t tick;
    };

    struct client_state {
        key_t key;
        tick_t tick;
        in_packet_state last_packet;
    };

    struct out_delta_state {
        size_t foods_added_size;
        size_t foods_removed_size;
        std::array<food_state, game_max_food> foods_added;
        std::array<vector2d, game_max_food> foods_removed;
    };

    struct session_state {
        bool active;
        id_t players;
        id_t human_players;
        tick_t tick;
        tick_t max_tick;
        scalar_t width;
        scalar_t height;
        std::array<snake_state, game_max_players> snakes;
        std::array<food_state, game_max_food> foods;
        size_t food_size;
        out_delta_state delta;
        std::array<in_packet_info_state, game_max_players> in_packets;
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
