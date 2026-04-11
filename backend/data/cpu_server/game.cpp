#include "tests/game.hpp"
#include "impl.hpp"
#include "parse.hpp"
#include <config.hpp>
#include <logger.hpp>
#include <game.hpp>
#include <packet.hpp>
#include <network.hpp>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <random>

using namespace snakeio;
using namespace snakeio::cpu;

game::game() :
    memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
}

void game::add_session(id_t session_id,
    id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    using enum add_session_error;
    impl& impl_ = get_impl();
    auto& rng = impl_.add_session_rng_;
    session& session = impl_.sessions[session_id];
    session.players = human_players + ai_players;
    session.human_players = human_players;
    session.tick = 0;
    session.max_tick = max_tick;
    session.width = game_width_psqp * std::sqrt(static_cast<scalar_t>(session.players));
    session.height = game_height_psqp * std::sqrt(static_cast<scalar_t>(session.players));
    std::uniform_real_distribution<scalar_t> angle_dist(-M_PI, M_PI);
    std::uniform_real_distribution<scalar_t> width_dist(0, session.width), height_dist(0, session.height);
    for (id_t i = 0; i < session.players; ++i) {
        // sync not needed here because session is still inactive
        if (i < human_players) {
            client& client = impl_.clients[session_id][i];
            client.key = keys[i];
            client.tick = -1;
        }
        snake& s = session.snakes[i];
        s.speed = snake_init_speed;
        s.angle = angle_dist(rng);
        s.width = snake_init_width;
        s.frac_length = 2;
        s.score = 0;
        s.boost = 0;
        s.status = {snake_status_t::alive};
        s.human = i < human_players;
        s.segments[0] = {width_dist(rng), height_dist(rng)};
        session.snakes_set.emplace(&s, &s.segments[0]);
        s.segments[1] = s.segments[0] - vector2d{std::cos(s.angle), std::sin(s.angle)} * snake_init_speed;
        session.snakes_set.emplace(&s, &s.segments[1]);
        session.add_segments(s, snake_init_length);
    }
    std::uniform_real_distribution<scalar_t> food_width_dist(gen_food_min_width, gen_food_max_width);
    for (size_t i = 0; i < game_init_food_pp * session.players; ++i) {
        session.food_set.insert({
            vector2d{width_dist(rng), height_dist(rng)},
            food_width_dist(rng)
        });
    }
    session.snakes_set.refresh();
    session.food_set.refresh();
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
        data_packet packet(buffer, recv_len);
        if (recv_len <= data_packet::header_size) [[unlikely]] {
            logger::debug("Received packet that is too short from {}.", client_addr);
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        if (!sm_[packet.session_id()]) [[unlikely]] {
            logger::debug("Received packet for non-existent session {} from {}.", packet.session_id(), client_addr);
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        session& session = impl_.sessions[packet.session_id()];
        if (packet.player_id() >= session.players) [[unlikely]] {
            logger::debug("Received packet with invalid player ID {} for session {} from {}.",
                packet.player_id(), packet.session_id(), client_addr);
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        auto& client = impl_.clients[packet.session_id()][packet.player_id()];
        const tick_t tick = std::atomic_ref(session.tick).load(std::memory_order::relaxed);
        if (std::atomic_ref(client.tick).load(std::memory_order::relaxed) == tick)
            continue;
        switch (packet.verify(client.key)) {
            using enum data_packet::verify_result;
            case ok: break;
            case too_short: std::unreachable();
            case invalid_size: {
                logger::debug("Received packet with invalid size from {}.", client_addr);
                logger::print_packet(logger::debug, packet.bytes());
                continue;
            }
            case invalid_tag: {
                logger::debug("Received packet with invalid tag for session {} player {} from {}.",
                    packet.session_id(), packet.player_id(), client_addr);
                logger::print_packet(logger::debug, packet.bytes());
                continue;
            }
            default: std::unreachable();
        }
        packet.decrypt(client.key);
        client.last_packet = {
            .addr = client_addr,
            .snapshot_requested = static_cast<bool>(packet.text()[0]),
            .boost = static_cast<bool>(packet.text()[1]),
            .angle = load_float32(packet.text().subspan<4, 4>())
        };
        std::atomic_ref(client.tick).store(tick, std::memory_order::release);
    }
}

void game::tick(std::stop_token, int sock) noexcept {
    impl& impl_ = get_impl();
    for (id_t i = 0; i < game_max_sessions; ++i) {
        if (!sm_[i]) continue;
        session& session = impl_.sessions[i];
        tick_t tick = std::atomic_ref(session.tick).load(std::memory_order::relaxed);
        in_packet_info in_packets_buffer[game_max_players];
        for (id_t j = 0; j < session.human_players; ++j) {
            const client& client = impl_.clients[i][j];
            in_packets_buffer[j].tick = std::atomic_ref(client.tick).load(std::memory_order::acquire);
            if (in_packets_buffer[j].tick != tick) continue;
            static_cast<in_packet&>(in_packets_buffer[j]) = client.last_packet;
        }
        const std::span in_packets(in_packets_buffer, session.human_players);
        std::byte delta_text[delta_packet_max_text_size];
        size_t delta_text_size;
        std::byte snapshot_text[snapshot_packet_max_text_size];
        size_t snapshot_text_size;
        const auto send_packet = [&](id_t player_id, std::span<const std::byte> text) {
            const client& client = impl_.clients[i][player_id];
            static std::byte buffer[packet_chunk_size + data_packet::header_size]{};
            data_packet packet(buffer);
            const auto chunks = text | std::views::chunk(packet_chunk_size);
            packet.session_id(i);
            packet.player_id(player_id);
            packet.sender(data_packet::sender_t::server);
            packet.total_chunks(chunks.size());
            packet.chunk_id(0);
            store_32(packet.nonce_part(), tick);
            for (const auto& chunk : chunks) {
                data_packet chunk_packet(buffer, chunk.size() + data_packet::header_size);
                std::ranges::copy(chunk, chunk_packet.text().begin());
                chunk_packet.encrypt(client.key);
                sendto(sock, chunk_packet.bytes(), in_packets[player_id].addr);
                packet.chunk_id(packet.chunk_id() + 1);
            }
        };
        if (tick == 0) [[unlikely]] {
            if (std::ranges::all_of(in_packets, [](const in_packet_info& packet) { return packet.tick == 0; })) {
                snapshot_text_size = store_snapshot(snapshot_text, session);
                for (in_packet& in_packet : in_packets) {
                    in_packet.snapshot_requested = true;
                    in_packet.boost = false;
                    in_packet.angle = std::numeric_limits<scalar_t>::quiet_NaN();
                }
            } else {
                std::byte lobby_status_text[lobby_status_max_text_size];
                const size_t lobby_status_text_size = store_lobby_status(lobby_status_text, in_packets);
                for (id_t j = 0; j < session.human_players; ++j) {
                    if (in_packets[j].tick != 0) continue;
                    send_packet(j, std::span(lobby_status_text, lobby_status_text_size));
                }
                continue;
            }
        } else {
            out_delta delta;
            test::game::tick_core(tick, session, &impl_.game_loop_rng_, in_packets_buffer, delta);
            delta_text_size = store_delta(delta_text, session, delta);
            if (std::ranges::any_of(in_packets, &in_packet_info::snapshot_requested)) {
                snapshot_text_size = store_snapshot(snapshot_text, session);
            }
        }
        // Send packets
        for (id_t j = 0; j < session.human_players; ++j) {
            const in_packet_info& in_packet = in_packets[j];
            const bool snapshot_requested = in_packet.tick == tick && in_packet.snapshot_requested;
            const std::span<std::byte> text = snapshot_requested
                ? std::span(snapshot_text, snapshot_text_size)
                : std::span(delta_text, delta_text_size);
            send_packet(j, text);
        }
        // Check for game end
        if (++tick > session.max_tick || std::ranges::none_of(session.snakes_view(), &snake::alive)) {
            std::byte termination_text[termination_max_text_size];
            const size_t termination_text_size = store_termination(termination_text, session);
            for (id_t j = 0; j < session.human_players; ++j) {
                send_packet(j, std::span(termination_text, termination_text_size));
            }
            sm_.deallocate(i);
            logger::debug("Session {} ended", i);
            continue;
        }
        // Increment tick
        (void) std::atomic_ref(session.tick).fetch_add(1, std::memory_order::relaxed);
    }
}