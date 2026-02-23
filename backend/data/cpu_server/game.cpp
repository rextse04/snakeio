#include "impl.hpp"
#include <config.hpp>
#include <logger.hpp>
#include <game.hpp>
#include <packet.hpp>
#include <utils.hpp>
#include <network.hpp>
#include <memory>
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace snakeio;

game::game() : memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
}

std::expected<id_t, game::add_session_error> game::add_session(const session_snapshot& snapshot) noexcept {
    using enum add_session_error;
    impl& impl_ = get_impl();
    auto session_id = sm_.allocate();
    if (!session_id) [[unlikely]] {
        return std::unexpected(no_memory);
    }
    impl::session& session = impl_.sessions[*session_id];
    session.players = snapshot.players;
    session.tick = 0;
    session.width = snapshot.width;
    session.height = snapshot.height;
    std::ranges::copy_n(snapshot.snakes.begin(), snapshot.players, session.snakes.begin());
    session.snakes_set.clear();
    for (snake& snake : std::span(session.snakes.data(), session.players)) {
        for (vector2d& seg : std::span(snake.segments.data(), snake.basic.length)) {
            session.snakes_set.emplace(&snake, &seg);
        }
    }
    session.food_set.clear();
    session.food_set.insert(std::span(snapshot.foods.begin(), snapshot.foods_size));
    return *session_id;
}

bool game::remove_session(id_t session_id) noexcept {
    if (sm_[session_id]) {
        sm_.deallocate(session_id);
        return true;
    } else {
        return false;
    }
}

void game::impl::port(game& game, std::stop_token stop_token, int sock) noexcept {
    impl& impl_ = game.get_impl();
    data_packet packet;
    sockaddr_storage client_addr{};
    while (true) {
        if (stop_token.stop_requested()) [[unlikely]] {
            logger::info("Data port received stop request, exiting.");
            return;
        }
        socklen_t client_addr_len = sizeof(client_addr);
        const ssize_t recv_len = recvfrom(sock, packet.data(), in_packet_max_size, 0,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (recv_len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) [[unlikely]] {
                logger::warn("recvfrom failed: {}.", std::strerror(errno));
            }
            continue;
        }
        switch (packet.load()) {
            using enum data_packet::load_result;
            case ok: break;
            case too_short: {
                logger::debug("Received packet that is too short from {}.",
                    *reinterpret_cast<const sockaddr*>(&client_addr));
                logger::print_packet(logger::debug, packet.bytes());
                continue;
            }
            case invalid_text_size: {
                logger::debug("Received packet with invalid text size from {}.",
                    *reinterpret_cast<const sockaddr*>(&client_addr));
                logger::print_packet(logger::debug, packet.bytes());
                continue;
            }
        }
        if (!game.sm_[packet.session_id]) [[unlikely]] {
            logger::debug("Received packet for non-existent session {} from {}.",
                packet.session_id, *reinterpret_cast<const sockaddr*>(&client_addr));
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        impl::session& session = impl_.sessions[packet.session_id];
        if (packet.player_id >= session.players) [[unlikely]] {
            logger::debug("Received packet with invalid player ID {} for session {} from {}.",
                packet.player_id, packet.session_id, *reinterpret_cast<const sockaddr*>(&client_addr));
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        auto& client = impl_.clients[packet.session_id][packet.player_id];
        if (!packet.verify(client.key)) [[unlikely]] {
            logger::debug("Received packet with invalid tag for session {} player {} from {}.",
                packet.session_id, packet.player_id, *reinterpret_cast<const sockaddr*>(&client_addr));
            logger::print_packet(logger::debug, packet.bytes());
            continue;
        }
        packet.decrypt(client.key);
        client.addr = client_addr;
        client.last_packet = decode_packet(packet.text());
    }
}

void game::impl::erase_snake(game& game, const session& session,
    snake& snake, size_t& erase_count, session_delta& delta) noexcept {
    snake.basic.alive = false;
    std::ranges::fill(snake.segments.begin(), snake.segments.begin() + snake.basic.length,
        decltype(session::snakes_set)::erase_key);
    erase_count += snake.basic.length;
    for (const vector2d& seg : std::span(snake.segments.begin(), snake.basic.length)) {
        if (session.food_set.size() + delta.foods_added_size >= game_max_food) break;
        if (std::bernoulli_distribution(seg_to_food_prob)(game.rng_)) {
            delta.foods_added[delta.foods_added_size++] = {
                .pos = seg,
                .width = seg_to_food_width
            };
        }
    }
}

void game::impl::game_loop(game& game, std::stop_token stop_token, int sock) noexcept {
    clock::time_point last_tick{};
    while (!stop_token.stop_requested()) {
        const clock::time_point now = clock::now();
        if (now - last_tick < game_tick_rate) continue;
        last_tick = now;
        impl& impl_ = game.get_impl();
        for (id_t i = 0; i < game_max_sessions; ++i) {
            if (!game.sm_[i]) continue;
            bool snapshot_requested = false;
            session& session = impl_.sessions[i];
            session_delta delta;
            // Processes in_packets
            for (id_t j = 0; j < session.players; ++j) {
                client& client = impl_.clients[i][j];
                if (!client.last_packet.valid) continue;
                client.last_packet.valid = false;
                snapshot_requested |= client.last_packet.snapshot_requested;
                session.snakes[j].basic.angle = client.last_packet.angle;
            }
            // Move snakes
            for (snake& snake : std::span(session.snakes.data(), session.players)) {
                if (!snake.basic.alive) continue;
                std::ranges::copy_backward(snake.segments.begin(), snake.segments.begin() + snake.basic.length,
                    snake.segments.begin() + snake.basic.length + 1);
                snake.segments[0] += {
                    std::cos(snake.basic.angle) * snake.basic.speed,
                    std::sin(snake.basic.angle) * snake.basic.speed
                };
                if (snake.basic.length >= snake_max_length) {
                    session.snakes_set.emplace(&snake, &snake.segments[snake.basic.length]);
                    ++snake.basic.length;
                }
            }
            // Detect collision with wall or other snakes
            session.snakes_set.refresh();
            size_t erase_count = 0;
            for (snake& snake : std::span(session.snakes.data(), session.players)) {
                if (!snake.basic.alive) continue;
                vector2d head = snake.segments[0];
                if (head[0] < 0 || head[0] >= session.width || head[1] < 0 || head[1] >= session.height) {
                    erase_snake(game, session, snake, erase_count, delta);
                    continue;
                }
                for (const auto [other_snake, seg] : session.snakes_set.find(head, snake_max_width * 2)) {
                    erase_snake(game, session, snake, erase_count, delta);
                    if (seg == other_snake->segments.data()) {
                        erase_snake(game, session, *other_snake, erase_count, delta);
                    }
                }
            }
            session.snakes_set.refresh();
            session.snakes_set.erase(erase_count);
            // Detect collision with food
            for (snake& snake : std::span(session.snakes.data(), session.players)) {
                if (!snake.basic.alive) continue;
                for (food& food : session.food_set.find(snake.segments[0], snake_max_width)) {
                    const size_t growth = std::min<size_t>(food.width, snake_max_length - snake.basic.length);
                    for (size_t j = 0; j < growth; ++j) {
                        snake.segments[snake.basic.length + j] = snake.segments[snake.basic.length + j-1] + vector2d{
                            std::cos(snake.basic.angle) * snake.basic.speed,
                            std::sin(snake.basic.angle) * snake.basic.speed
                        };
                    }
                    snake.basic.length += growth;
                    delta.foods_removed[delta.foods_removed_size++] = food.pos;
                    food.pos = session.food_set.erase_key;
                }
            }
            // Add food
            const size_t food_added = std::min(game_max_food - session.food_set.size() - delta.foods_added_size,
                food_per_player_tick * session.players);
            for (size_t j = 0; j < food_added; ++j) {
                delta.foods_added[delta.foods_added_size++] = {
                    .pos = {
                        std::uniform_real_distribution<scalar_t>(0, session.width)(game.rng_),
                        std::uniform_real_distribution<scalar_t>(0, session.height)(game.rng_)
                    },
                    .width = std::uniform_real_distribution<scalar_t>(1, food_max_width)(game.rng_)
                };
            }
            session.food_set.insert(std::span(delta.foods_added.data(), delta.foods_added_size));
            session.food_set.refresh();
            session.food_set.erase(delta.foods_removed_size);
            // Complete delta
            for (id_t j = 0; j < session.players; ++j) {
                delta.snakes[j] = session.snakes[j].basic;
            }
            // Send snapshot if requested, otherwise send delta
            data_packet delta_packet;
            /* TODO: write delta */
            data_packet snapshot_packet;
            if (snapshot_requested) {
                /* TODO: write snapshot */
            }
            for (id_t j = 0; j < session.players; ++j) {
                const client& client = impl_.clients[i][j];
                data_packet& packet = snapshot_requested ? snapshot_packet : delta_packet;
                packet.session_id = i;
                packet.player_id = j;
                packet.nonce = {};
                packet.nonce[0] = std::byte(1); // from server
                store_32(std::span<std::byte, 4>(packet.nonce.begin() + 1, 4), session.tick);
                delta_packet.encrypt(client.key);
                sendto(sock, delta_packet.data(), delta_packet.size, 0,
                    reinterpret_cast<const sockaddr*>(&client.addr), sizeof(client.addr));
            }
            // Increment tick
            ++session.tick;
        }
    }
}
