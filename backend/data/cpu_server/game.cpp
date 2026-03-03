#include "impl.hpp"
#include <config.hpp>
#include <logger.hpp>
#include <game.hpp>
#include <packet.hpp>
#include <utils.hpp>
#include <network.hpp>
#include <cstddef>
#include <memory>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>

using namespace snakeio;

game::game() : memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
}

std::expected<id_t, game::add_session_error> game::add_session(
    id_t human_players, id_t ai_players, std::span<const key_t> keys) noexcept {
    using enum add_session_error;
    impl& impl_ = get_impl();
    auto session_id = sm_.allocate();
    if (!session_id) [[unlikely]] {
        return std::unexpected(no_memory);
    }
    impl::session& session = impl_.sessions[*session_id];
    session.players = human_players + ai_players;
    if (session.players > game_max_players) [[unlikely]] {
        return std::unexpected(too_many_players);
    }
    session.tick = 0;
    session.max_tick = game_max_tick;
    session.width = game_width_psqp * std::sqrt(session.players);
    session.height = game_height_psqp * std::sqrt(session.players);
    std::uniform_real_distribution<scalar_t> angle_dist(0, M_PI * 2);
    std::uniform_real_distribution<scalar_t> width_dist(0, session.width), height_dist(0, session.height);
    for (id_t i = 0; i < session.players; ++i) {
        // sync not needed here because session is still inactive
        impl::client& client = impl_.clients[*session_id][i];
        client.key = keys[i];
        client.last_packet.tick = -1;
        snake& s = session.snakes[i];
        s.basic = {
            .speed = snake_init_speed,
            .angle = angle_dist(rng_),
            .width = snake_init_width,
            .length = snake_init_length,
            .score = 0,
            .alive = true,
            .human = i < human_players
        };
        s.segments[0] = {width_dist(rng_), height_dist(rng_)};
        for (size_t seg = 1; seg < s.basic.length; ++seg) {
            s.segments[seg] = s.segments[seg-1] +
                vector2d{std::cos(s.basic.angle) * s.basic.width, std::sin(s.basic.angle) * s.basic.width};
        }
    }
    std::uniform_real_distribution<scalar_t> food_width_dist(food_min_width, food_max_width);
    for (size_t i = 0; i < game_init_food_pp * session.players; ++i) {
        session.food_set.insert({.pos = {width_dist(rng_), height_dist(rng_)}, .width = food_width_dist(rng_)});
    }
    sm_.activate(*session_id);
    return *session_id;
}

void game::impl::port(game& game, std::stop_token stop_token, int sock) noexcept {
    impl& impl_ = game.get_impl();
    std::byte buffer[in_packet_max_text_size + data_packet::header_size];
    data_packet packet(buffer);
    sockaddr_storage client_addr{};
    while (true) {
        if (stop_token.stop_requested()) [[unlikely]] {
            logger::info("Data port received stop request, exiting.");
            return;
        }
        socklen_t client_addr_len = sizeof(client_addr);
        const ssize_t recv_len = recvfrom(sock, packet.bytes().data(), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (recv_len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) [[unlikely]] {
                logger::warn("recvfrom failed on data port: {}.", std::strerror(errno));
            }
            continue;
        }
        if (!game.sm_[packet.session_id()]) [[unlikely]] {
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
        switch (packet.verify(client.key)) {
            using enum data_packet::verify_result;
            case ok: break;
            case too_short: {
                logger::debug("Received packet that is too short from {}.", client_addr);
                logger::print_packet(logger::debug, packet.bytes());
                continue;
            }
            case invalid_text_size: {
                logger::debug("Received packet with invalid text size from {}.", client_addr);
                logger::print_packet(logger::debug, packet.bytes());
                break;
            }
            case invalid_tag: {
                logger::debug("Received packet with invalid tag for session {} player {} from {}.",
                    packet.session_id(), packet.player_id(), client_addr);
                logger::print_packet(logger::debug, packet.bytes());
                break;
            }
            default: std::unreachable();
        }
        packet.decrypt(client.key);
        std::atomic_ref(client.last_packet).store({
            .addr = client_addr,
            .tick = std::atomic_ref(session.tick).load(std::memory_order::acquire),
            .snapshot_requested = static_cast<bool>(load_32(packet.text().subspan<0, 4>())),
            .angle = load_float32(packet.text().subspan<4, 4>())
        }, std::memory_order::release);
    }
}

void game::impl::erase_snake(game& game, const session& session,
    snake& snake, size_t& erase_count, out_delta& delta) noexcept {
    snake.basic.alive = false;
    for (const vector2d& seg : snake.segments_view()) {
        if (session.food_set.size() + delta.foods_added_size >= game_max_food) break;
        if (std::bernoulli_distribution(seg_to_food_prob)(game.rng_)) {
            delta.foods_added[delta.foods_added_size++] = {
                .pos = seg,
                .width = seg_to_food_width
            };
        }
    }
    std::ranges::fill(snake.segments_view(), decltype(session::snakes_set)::erase_key);
    erase_count += snake.basic.length;
}

static void store_snake_basic(std::span<std::byte, 24> out, const snake_basic& basic) noexcept {
    store_float32(out.subspan<0, 4>(), basic.speed);
    store_float32(out.subspan<4, 4>(), basic.angle);
    store_float32(out.subspan<8, 4>(), basic.width);
    store_32(out.subspan<12, 4>(), basic.length);
    store_32(out.subspan<16, 4>(), basic.score);
    out[20] = static_cast<std::byte>(basic.alive);
    out[21] = static_cast<std::byte>(basic.human);
    out[22] = {};
    out[23] = {};
}
static std::byte* store_snake(std::byte* out, const snake& snake) noexcept {
    store_snake_basic(std::span<std::byte, 24>(out, 24), snake.basic);
    out += 24;
    for (const vector2d& seg : snake.segments_view()) {
        store_float32(std::span<std::byte, 4>(out, 4), seg[0]);
        store_float32(std::span<std::byte, 4>(out + 4, 4), seg[1]);
        out += 8;
    }
    return out;
}
static void store_food(std::span<std::byte, 12> out, const food& food) noexcept {
    store_float32(out.subspan<0, 4>(), food.pos[0]);
    store_float32(out.subspan<4, 4>(), food.pos[1]);
    store_float32(out.subspan<8, 4>(), food.width);
}

snakeio::size_t game::impl::store_delta(std::byte* const out, const session& session, out_delta& delta) noexcept {
    std::byte* it = out;
    for (const snake& snake : session.snakes_view()) {
        store_snake_basic(std::span<std::byte, 24>(it, 24), snake.basic);
        it += 24;
    }
    store_32(std::span<std::byte, 4>(it, 4), delta.foods_added_size);
    it += 4;
    for (const food& food : delta.foods_added_view()) {
        store_float32(std::span<std::byte, 4>(it, 4), food.pos[0]);
        store_float32(std::span<std::byte, 4>(it + 4, 4), food.pos[1]);
        store_float32(std::span<std::byte, 4>(it + 8, 4), food.width);
        it += 12;
    }
    store_32(std::span<std::byte, 4>(it, 4), delta.foods_removed_size);
    it += 4;
    for (const vector2d& pos : delta.foods_removed_view()) {
        store_float32(std::span<std::byte, 4>(it, 4), pos[0]);
        store_float32(std::span<std::byte, 4>(it + 4, 4), pos[1]);
        it += 8;
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

snakeio::size_t game::impl::store_snapshot(std::byte* const out, tick_t tick, const session& session) noexcept {
    std::byte* it = out;
    store_float32(std::span<std::byte, 4>(it, 4), session.width);
    store_float32(std::span<std::byte, 4>(it + 4, 4), session.height);
    store_32(std::span<std::byte, 4>(it + 8, 4), tick);
    it += 12;
    for (const snake& snake : session.snakes_view()) {
        it = store_snake(it, snake);
    }
    store_32(std::span<std::byte, 4>(it, 4), session.food_set.size());
    for (const food& food : session.food_set) {
        store_food(std::span<std::byte, 12>(it, 12), food);
        it += 12;
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

snakeio::size_t game::impl::store_lobby_status(std::byte* out, tick_t tick,
    const std::array<in_packet, game_max_players>& in_packets, const session& session) noexcept {
    for (id_t i = 0; i < session.players; ++i) {
        out[i] = static_cast<std::byte>(in_packets[i].tick == tick);
    }
    const size_t size = align(session.players);
    std::ranges::fill(out + session.players, out + size, std::byte(0));
    return size;
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
            session& session = impl_.sessions[i];
            const tick_t tick = std::atomic_ref(session.tick).load(std::memory_order::acquire);
            std::array<in_packet, game_max_players> in_packets;
            for (id_t j = 0; j < session.players; ++j) {
                in_packets[j] = std::atomic_ref(impl_.clients[i][j].last_packet).load(std::memory_order::acquire);
            }
            std::byte delta_text[delta_packet_max_text_size];
            size_t delta_text_size;
            std::byte snapshot_text[snapshot_packet_max_text_size];
            size_t snapshot_text_size;
            if (tick == 0) [[unlikely]] {
                if (std::ranges::all_of(in_packets, [tick](const in_packet& packet) { return packet.tick == tick; })) {
                    snapshot_text_size = store_snapshot(snapshot_text, tick, session);
                } else {
                    std::byte lobby_status_text[lobby_status_max_text_size];
                    const size_t lobby_status_text_size =
                        store_lobby_status(lobby_status_text, tick, in_packets, session);
                    for (id_t j = 0; j < session.players; ++j) {
                        const in_packet& in_packet = in_packets[j];
                        if (in_packet.tick != tick) continue;
                        std::byte buffer[lobby_status_max_text_size + data_packet::header_size];
                        data_packet packet(buffer, lobby_status_text_size + data_packet::header_size);
                        packet.session_id(i);
                        packet.player_id(j);
                        packet.sender(data_packet::sender_t::server);
                        store_32(packet.nonce_part(), tick);
                        std::ranges::copy_n(lobby_status_text, lobby_status_text_size, packet.text().begin());
                        packet.encrypt(impl_.clients[i][j].key);
                        sendto(sock, packet.bytes().data(), packet.bytes().size(), 0,
                            reinterpret_cast<const sockaddr*>(&in_packet.addr), sizeof(in_packet.addr));
                    }
                    std::atomic_ref(session.tick).store(1, std::memory_order::release);
                    continue;
                }
            } else {
                out_delta delta;
                bool snapshot_requested = false;
                // Processes in_packets
                snapshot_requested = false;
                for (id_t j = 0; j < session.players; ++j) {
                    const in_packet& in_packet = in_packets[j];
                    if (in_packet.tick != tick) goto next_session;
                    snapshot_requested |= in_packet.snapshot_requested;
                    session.snakes[j].basic.angle = in_packet.angle;
                }
                // Move snakes
                for (snake& snake : session.snakes_view()) {
                    if (!snake.basic.alive) continue;
                    std::ranges::copy_backward(snake.segments_view(), snake.segments.begin() + snake.basic.length + 1);
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
                for (snake& snake : session.snakes_view()) {
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
                for (snake& snake : session.snakes_view()) {
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
                        .width = std::uniform_real_distribution<scalar_t>(food_min_width, food_max_width)(game.rng_)
                    };
                }
                session.food_set.insert(std::span(delta.foods_added.begin(), delta.foods_added_size));
                session.food_set.refresh();
                session.food_set.erase(delta.foods_removed_size);
                // Write to buffer
                delta_text_size = store_delta(delta_text, session, delta);
                if (snapshot_requested) {
                    snapshot_text_size = store_snapshot(snapshot_text, tick, session);
                }
                std::atomic_ref(session.tick).store(tick + 1, std::memory_order::release);
            }
            // Send packets
            for (id_t j = 0; j < session.players; ++j) {
                const in_packet& in_packet = in_packets[j];
                const bool snapshot_requested = in_packet.snapshot_requested;
                std::byte buffer[std::max(delta_packet_max_text_size, snapshot_packet_max_text_size) +
                    data_packet::header_size];
                data_packet packet(buffer, snapshot_requested ?
                     snapshot_text_size + data_packet::header_size :
                     delta_text_size + data_packet::header_size);
                packet.session_id(i);
                packet.player_id(j);
                packet.sender(data_packet::sender_t::server);
                store_32(packet.nonce_part(), tick);
                const std::byte* text = snapshot_requested ? snapshot_text : delta_text;
                std::ranges::copy_n(text, snapshot_requested ? snapshot_text_size : delta_text_size,
                    packet.text().begin());
                packet.encrypt(impl_.clients[i][j].key);
                sendto(sock, packet.bytes().data(), packet.bytes().size(), 0,
                    reinterpret_cast<const sockaddr*>(&in_packet.addr), sizeof(in_packet.addr));
            }
            // Increment tick
            if (tick >= session.max_tick) {
                game.sm_.deallocate(i);
            }
            next_session:
        }
    }
}

void game::bind(std::stop_token stop_token, int sock) noexcept {
    std::jthread port_thread([this, stop_token, sock]() {
        impl::port(*this, stop_token, sock);
    }),
    game_loop_thread([this, stop_token, sock]() {;
        impl::game_loop(*this, stop_token, sock);
    });
}
