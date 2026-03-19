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
#include <ranges>

using namespace snakeio;

constexpr void game::impl::session::add_segment(snake& snake) noexcept {
    const vector2d seg_pos = snake.segments_view().back() +
        (snake.segments_view().back() - snake.segments_view()[snake.basic.length - 2]);
    add_segment(snake, seg_pos);
}

game::game() : memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
}

std::expected<id_t, game::add_session_error> game::add_session(
    id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
    using enum add_session_error;
    impl& impl_ = get_impl();
    auto session_id = sm_.allocate();
    if (!session_id) [[unlikely]] {
        return std::unexpected(no_memory);
    }
    impl::session& session = impl_.sessions[*session_id];
    session.players = human_players + ai_players;
    session.human_players = human_players;
    if (session.players > game_max_players) [[unlikely]] {
        return std::unexpected(too_many_players);
    }
    session.tick = 0;
    if (max_tick > game_max_tick) [[unlikely]] {
        return std::unexpected(max_tick_too_big);
    }
    session.max_tick = max_tick;
    session.width = game_width_psqp * std::sqrt(static_cast<scalar_t>(session.players));
    session.height = game_height_psqp * std::sqrt(static_cast<scalar_t>(session.players));
    std::uniform_real_distribution<scalar_t> angle_dist(-M_PI, M_PI);
    std::uniform_real_distribution<scalar_t> width_dist(0, session.width), height_dist(0, session.height);
    for (id_t i = 0; i < session.players; ++i) {
        // sync not needed here because session is still inactive
        if (i < human_players) {
            impl::client& client = impl_.clients[*session_id][i];
            client.key = keys[i];
            client.tick = -1;
        }
        snake& s = session.snakes[i];
        s.basic = {
            .speed = snake_init_speed,
            .angle = angle_dist(rng_),
            .width = snake_init_width,
            .length = 0,
            .score = 0,
            .alive = true,
            .human = i < human_players
        };
        session.add_segment(s, {width_dist(rng_), height_dist(rng_)});
        session.add_segment(s, s.segments_view()[0] - vector2d{
            std::cos(s.basic.angle) * s.basic.width * 2 * snake_displacement_factor,
            std::sin(s.basic.angle) * s.basic.width * 2 * snake_displacement_factor
        });
        while (s.basic.length < snake_init_length) {
             session.add_segment(s);
        }
    }
    std::uniform_real_distribution<scalar_t> food_width_dist(gen_food_min_width, gen_food_max_width);
    for (size_t i = 0; i < game_init_food_pp * session.players; ++i) {
        session.food_set.insert({
            vector2d{width_dist(rng_), height_dist(rng_)},
            food_width_dist(rng_)
        });
    }
    session.snakes_set.refresh();
    session.food_set.refresh();
    sm_.activate(*session_id);
    return *session_id;
}

void game::impl::port(game& game, std::stop_token stop_token, int sock) noexcept {
    impl& impl_ = game.get_impl();
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
        const scalar_t angle = load_float32(packet.text().subspan<4, 4>());
        client.last_packet = {
            .addr = client_addr,
            .snapshot_requested = static_cast<bool>(packet.text()[0]),
            .angle = std::isfinite(angle) ? angle : session.snakes[packet.player_id()].basic.angle,
        };
        std::atomic_ref(client.tick).store(tick, std::memory_order::release);
    }
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
    store_32(std::span<std::byte, 4>(it, 4), 0);
    it += 4;
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

snakeio::size_t game::impl::store_snapshot(std::byte* const out, const session& session) noexcept {
    std::byte* it = out;
    store_32(std::span<std::byte, 4>(it, 4), 1);
    store_float32(std::span<std::byte, 4>(it + 4, 4), session.width);
    store_float32(std::span<std::byte, 4>(it + 8, 4), session.height);
    store_32(std::span<std::byte, 4>(it + 12, 4), session.max_tick);
    store_32(std::span<std::byte, 4>(it + 16, 4), session.players);
    it += 20;
    for (const snake& snake : session.snakes_view()) {
        it = store_snake(it, snake);
    }
    store_32(std::span<std::byte, 4>(it, 4), session.food_set.size());
    it += 4;
    for (const food& food : session.food_set) {
        store_food(std::span<std::byte, 12>(it, 12), food);
        it += 12;
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

snakeio::size_t game::impl::store_lobby_status(std::byte* out, std::span<const in_packet_info> in_packets) noexcept {
    std::byte* it = out;
    store_32(std::span<std::byte, 4>(it, 4), 2);
    it += 4;
    for (const in_packet_info& in_packet : in_packets) {
        *(it++) = static_cast<std::byte>(in_packet.tick == 0);
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

snakeio::size_t game::impl::store_termination(std::byte* out, const session& session) noexcept {
    std::byte* it = out;
    store_32(std::span<std::byte, 4>(it, 4), 3);
    store_32(std::span<std::byte, 4>(it + 4, 4), session.max_tick);
    it += 8;
    for (const snake& snake : session.snakes_view()) {
        it = store_snake(it, snake);
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

void game::impl::game_loop(game& game, std::stop_token stop_token, int sock) noexcept {
    clock::time_point next_tick = clock::now();
    while (!stop_token.stop_requested()) {
        impl& impl_ = game.get_impl();
        for (id_t i = 0; i < game_max_sessions; ++i) {
            if (!game.sm_[i]) continue;
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
                bool snapshot_requested = false;
                // Processes in_packets
                snapshot_requested = false;
                for (id_t j = 0; j < session.human_players; ++j) {
                    const in_packet_info& in_packet = in_packets[j];
                    if (in_packet.tick != tick) continue;
                    snapshot_requested = snapshot_requested || in_packet.snapshot_requested;
                    session.snakes[j].basic.angle = in_packet.angle;
                }
                // AIs play
                for (snake& snake : session.snakes_view()) {
                    if (!snake.basic.alive || snake.basic.human) continue;
                    const vector2d head = snake.segments[0];
                    const scalar_t sightrange = snake.basic.speed * 32;
                    // Forward bias
                    vector2d force = vector2d{std::cos(snake.basic.angle), std::sin(snake.basic.angle)};
                    // Food attraction
                    constexpr scalar_t epsilon = 1./1024;
                    const auto get_field = [](vector2d d) noexcept {
                        return d / (d.norm_sq() + epsilon);
                    };
                    for (const food& food : session.food_set.find_possible(head, sightrange)) {
                        force += get_field(food.pos - head) * food.width;
                    }
                    // Collision repulsion
                    const scalar_t danger_mass = snake.basic.speed * 2;
                    for (const auto [other_snake, seg] : session.snakes_set.find_possible(head, sightrange)) {
                        force -= get_field(*seg - head) * danger_mass;
                    }
                    // Wall repulsion
                    const scalar_t wall_mass = snake.basic.speed * 10;
                    const auto get_wall_field = [&](scalar_t pos, scalar_t length) noexcept {
                        const scalar_t dist = length - pos;
                        return 1 / (pos - snake.basic.width + epsilon) - 1 / (dist - snake.basic.width + epsilon);
                    };
                    force[0] += wall_mass * get_wall_field(head[0], session.width);
                    force[1] += wall_mass * get_wall_field(head[1], session.height);
                    // Convert force to angle
                    snake.basic.angle = std::atan2(force[1], force[0]);
                }
                // Move snakes
                for (snake& snake : session.snakes_view()) {
                    if (!snake.basic.alive) continue;
                    std::shift_right(snake.segments_view().begin(), snake.segments_view().end(), 1);
                    snake.segments.front() += {
                        std::cos(snake.basic.angle) * snake.basic.speed,
                        std::sin(snake.basic.angle) * snake.basic.speed
                    };
                }
                session.snakes_set.refresh();
                // Detect collision with wall or other snakes
                size_t erase_count = 0;
                const auto erase_snake = [&](snake& snake) {
                    snake.basic.alive = false;
                    std::uniform_real_distribution<scalar_t> food_width_dist(seg_food_min_width, seg_food_max_width);
                    for (const vector2d& seg : snake.segments_view()) {
                        if (session.food_set.size() + delta.foods_added_size >= game_max_food) break;
                        if (std::bernoulli_distribution(seg_to_food_prob)(game.rng_)) {
                            delta.foods_added[delta.foods_added_size++] = {
                                .pos = seg,
                                .width = food_width_dist(game.rng_)
                            };
                        }
                    }
                    std::ranges::fill(snake.segments_view(), decltype(session::snakes_set)::erase_key);
                    erase_count += snake.basic.length;
                    snake.basic.length = 0;
                };
                for (snake& snake : session.snakes_view()) {
                    if (!snake.basic.alive) continue;
                    const vector2d head = snake.segments.front();
                    if (head[0] < snake.basic.width || head[0] > session.width - snake.basic.width ||
                        head[1] < snake.basic.width || head[1] > session.height - snake.basic.width) {
                        erase_snake(snake);
                        continue;
                    }
                    for (const auto [other_snake, seg] : session.snakes_set.find_possible(head, snake_max_width * 2)) {
                        const scalar_t req = snake.basic.width + other_snake->basic.width;
                        if ((*seg - head).norm_sq() > req * req)
                            continue;
                        if (other_snake == &snake) {
                            const size_t seg_idx = seg - snake.segments.data();
                            if (seg_idx < 1 / snake_displacement_factor) [[likely]] continue;
                            erase_snake(snake);
                        } else {
                            erase_snake(snake);
                            if (seg == other_snake->segments.data()) {
                                erase_snake(*other_snake);
                            }
                        }
                    }
                }
                session.snakes_set.refresh();
                session.snakes_set.erase(erase_count);
                // Detect collision with food
                for (snake& snake : session.snakes_view()) {
                    if (!snake.basic.alive) continue;
                    for (food& food : session.food_set.find_possible(snake.segments[0], snake_max_width + food_max_width)) {
                        const scalar_t req = snake.basic.width + food.width;
                        if ((food.pos - snake.segments[0]).norm_sq() > req * req)
                            continue;
                        snake.basic.score += static_cast<score_t>(food.width);
                        const size_t new_length = std::min<size_t>(snake.basic.length + food.width, snake_max_length);
                        while (snake.basic.length < new_length) {
                            session.add_segment(snake);
                        }
                        delta.foods_removed[delta.foods_removed_size++] = food.pos;
                        food.pos = session.food_set.erase_key;
                    }
                }
                session.snakes_set.refresh();
                // Add food
                const auto food_added = std::min(game_max_food - session.food_set.size() - delta.foods_added_size,
                    std::poisson_distribution<size_t>(food_per_player_tick * session.players)(game.rng_));
                std::uniform_real_distribution<scalar_t> food_width_dist(gen_food_min_width, gen_food_max_width);
                for (size_t j = 0; j < food_added; ++j) {
                    delta.foods_added[delta.foods_added_size++] = {
                        .pos = {
                            std::uniform_real_distribution<scalar_t>(0, session.width)(game.rng_),
                            std::uniform_real_distribution<scalar_t>(0, session.height)(game.rng_)
                        },
                        .width = food_width_dist(game.rng_)
                    };
                }
                session.food_set.insert(std::span(delta.foods_added.begin(), delta.foods_added_size));
                session.food_set.refresh();
                session.food_set.erase(delta.foods_removed_size);
                // Write to buffer
                delta_text_size = store_delta(delta_text, session, delta);
                if (snapshot_requested) {
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
            if (++tick > session.max_tick ||
                std::ranges::none_of(session.snakes_view(), [](const snake& s) { return s.basic.alive; })) {
                std::byte termination_text[termination_max_text_size];
                const size_t termination_text_size = store_termination(termination_text, session);
                for (id_t j = 0; j < session.human_players; ++j) {
                    send_packet(j, std::span(termination_text, termination_text_size));
                }
                game.sm_.deallocate(i);
                logger::debug("Session {} ended", i);
                continue;
            }
            // Increment tick
            (void) std::atomic_ref(session.tick).fetch_add(1, std::memory_order::relaxed);
        }
        next_tick += game_tick_rate;
        std::this_thread::sleep_until(next_tick);
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
