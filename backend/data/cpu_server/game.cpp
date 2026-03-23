#include "impl.hpp"
#include "parse.hpp"
#include <config.hpp>
#include <logger.hpp>
#include <game.hpp>
#include <packet.hpp>
#include <network.hpp>
#include <cstddef>
#include <memory>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <ranges>

using namespace snakeio;
using namespace snakeio::cpu;

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
    session& session = impl_.sessions[*session_id];
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
            client& client = impl_.clients[*session_id][i];
            client.key = keys[i];
            client.tick = -1;
        }
        snake& s = session.snakes[i];
        s.speed = snake_init_speed;
        s.angle = angle_dist(rng_);
        s.width = snake_init_width;
        s.frac_length = 2;
        s.score = 0;
        s.boost = 0;
        s.status = {snake_status_t::alive};
        s.human = i < human_players;
        s.segments[0] = {width_dist(rng_), height_dist(rng_)};
        session.snakes_set.emplace(&s, &s.segments[0]);
        s.segments[1] = s.segments[0] - vector2d{
            std::cos(s.angle) * snake_init_speed,
            std::sin(s.angle) * snake_init_speed
        };
        session.snakes_set.emplace(&s, &s.segments[1]);
        session.add_segments(s, snake_init_length);
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
        client.last_packet = {
            .addr = client_addr,
            .snapshot_requested = static_cast<bool>(packet.text()[0]),
            .boost = static_cast<bool>(packet.text()[1]),
            .angle = load_float32(packet.text().subspan<4, 4>())
        };
        std::atomic_ref(client.tick).store(tick, std::memory_order::release);
    }
}

namespace {
    void sync_snake_dims(snake_basic& snake) noexcept {
        const auto progress = (snake.frac_length - snake_init_length) / (snake_max_length - snake_init_length);
        const scalar_t scaled = 1 - (1 - progress) * (1 - progress); // ease out quadratic
        snake.speed = (snake_init_speed + (snake_min_speed - snake_init_speed) * scaled)
            * (1 + static_cast<bool>(snake.boost) * (snake_boost_speed_factor - 1));
        snake.width = snake_init_width + (snake_max_width - snake_init_width) * scaled;
    }
    struct erased_snake {
        snake* target;
        snake_status reason;
    };
}
void game::impl::game_loop(game& game, std::stop_token stop_token, int sock) noexcept {
    using enum snake_status_t;
    clock::time_point next_tick = clock::now();
    while (!stop_token.stop_requested()) {
        {
#ifdef SNAKEIO_BENCHMARK
            benchmarker bencher(game.tick_bench);
#endif
            impl& impl_ = game.get_impl();
            for (id_t i = 0; i < game_max_sessions; ++i) {
                if (!game.sm_[i]) continue;
#ifdef SNAKEIO_BENCHMARK
                ++bencher.item.sessions;
#endif
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
                    // AIs play
                    for (size_t j = 0; j < session.players; ++j) {
                        snake& snake = session.snakes[j];
                        if (!snake.alive() || snake.human) continue;
                        const vector2d head = snake.segments[0];
                        const scalar_t sightrange = snake.speed * 32;
                        // Forward bias
                        vector2d forward_force = vector2d{std::cos(snake.angle), std::sin(snake.angle)};
                        // Food attraction
                        constexpr scalar_t epsilon = 1./1024;
                        const auto get_field = [](vector2d d) noexcept {
                            return d / (d.norm_sq() + epsilon);
                        };
                        vector2d food_force{};
                        for (const food& food : session.food_set.find_possible(head, sightrange)) {
                            food_force += get_field(food.pos - head) * food.width;
                        }
                        // Collision repulsion
                        vector2d collision_force{};
                        const scalar_t danger_mass = snake.speed * 2;
                        for (const auto [other_snake, seg] : session.snakes_set.find_possible(head, sightrange)) {
                            if (other_snake == &snake) continue;
                            collision_force -= get_field(*seg - head) * danger_mass;
                        }
                        // Wall repulsion
                        const scalar_t wall_mass = snake.speed * 10;
                        const auto get_wall_field = [&](scalar_t pos, scalar_t length) noexcept {
                            const scalar_t dist = length - pos;
                            return 1 / (pos - snake.width + epsilon) - 1 / (dist - snake.width + epsilon);
                        };
                        vector2d wall_force{
                            wall_mass * get_wall_field(head[0], session.width),
                            wall_mass * get_wall_field(head[1], session.height)
                        };
                        // Save AI command
                        // Does not directly write to snake to ensure users and AIs use the same interface.
                        const vector2d force = forward_force + food_force + collision_force + wall_force;
                        in_packet_info& in_packet = in_packets_buffer[j];
                        in_packet.tick = tick;
                        in_packet.snapshot_requested = false;
                        in_packet.angle = std::atan2(force[1], force[0]);
                        in_packet.boost = (
                            snake.frac_length > snake_init_length * 5 &&
                            force * collision_force > 0 &&
                            force * wall_force > 0);
                    }
                    // Processes in_packets
                    snapshot_requested = false;
                    for (id_t j = 0; j < session.players; ++j) {
                        in_packet_info& in_packet = in_packets_buffer[j];
                        if (in_packet.tick != tick) continue;
                        snapshot_requested = snapshot_requested || in_packet.snapshot_requested;
                        snake& snake = session.snakes[j];
                        if (!snake.alive()) continue;
                        if (std::isfinite(in_packet.angle)) {
                            snake.angle += std::clamp(angle_diff(in_packet.angle, snake.angle),
                                -snake_max_turn_per_tick, snake_max_turn_per_tick);
                        }
                        // saturation sub, no need to check if boost is 0 :)
                        --snake.boost;
                        if (in_packet.boost &= snake.frac_length > snake_init_length) {
                            snake.boost += snake_seg_to_boost_ticks;
                        }
                        sync_snake_dims(snake);
                    }
                    // Move snakes
                    for (snake& snake : session.snakes_view()) {
                        if (!snake.alive()) continue;
                        std::shift_right(snake.segments_view().begin(), snake.segments_view().end(), 1);
                        snake.segments.front() += {
                            std::cos(snake.angle) * snake.speed,
                            std::sin(snake.angle) * snake.speed
                        };
                    }
                    session.snakes_set.refresh();
                    // Detect collision with wall or other snakes
                    std::array<erased_snake, game_max_players> to_erase;
                    id_t to_erase_size = 0;
                    for (unsigned char j = 0; j < session.players; ++j) {
                        snake& snake = session.snakes[j];
                        if (!snake.alive()) continue;
                        const vector2d head = snake.segments.front();
                        if (head[0] < snake.width || head[0] > session.width - snake.width ||
                            head[1] < snake.width || head[1] > session.height - snake.width) {
                            to_erase[to_erase_size++] = {&snake, killed_by_wall};
                            continue;
                            }
                        for (const auto [other_snake, seg] : session.snakes_set.find_possible(head, snake_max_width * 2)) {
                            if (other_snake == &snake) continue;
                            const scalar_t req = snake.width + other_snake->width;
                            if ((*seg - head).norm_sq() > req * req)
                                continue;
                            const unsigned char other_snake_id = other_snake - session.snakes.data();
                            to_erase[to_erase_size++] = {&snake, killed_by_snake, other_snake_id};
                            break;
                        }
                    }
                    size_t erased_segs = 0;
                    for (id_t j = 0; j < to_erase_size; ++j) {
                        const auto [target, reason] = to_erase[j];
                        target->status = reason;
                        std::uniform_real_distribution<scalar_t> food_width_dist(seg_food_min_width, seg_food_max_width);
                        for (const vector2d& seg : target->segments_view()) {
                            if (session.food_set.size() + delta.foods_added_size >= game_max_food) break;
                            if (std::bernoulli_distribution(seg_to_food_prob)(game.rng_)) {
                                delta.foods_added[delta.foods_added_size++] = {
                                    .pos = seg,
                                    .width = food_width_dist(game.rng_)
                                };
                            }
                        }
                        std::ranges::fill(target->segments_view(), decltype(session::snakes_set)::erase_key);
                        erased_segs += target->length();
                        target->frac_length = 0;
                    }
                    session.snakes_set.refresh();
                    session.snakes_set.erase(erased_segs);
                    // Detect collision with food and recalculate attributes
                    std::array<food*, game_max_food> erased_foods;
                    for (snake& snake : session.snakes_view()) {
                        if (!snake.alive()) continue;
                        scalar_t new_length = snake.frac_length;
                        for (food& food : session.food_set.find_possible(snake.segments[0], snake_max_width)) {
                            const scalar_t req = snake.width;
                            if ((food.pos - snake.segments[0]).norm_sq() > req * req)
                                continue;
                            snake.score += static_cast<score_t>(food.width);
                            // There is no risk of double erasure because colliding snakes are already eliminated
                            new_length = std::min<scalar_t>(snake_max_length,
                                snake.frac_length + food.width * food_width_to_seg);
                            erased_foods[delta.foods_removed_size] = &food;
                            delta.foods_removed[delta.foods_removed_size++] = food.pos;
                        }
                        session.add_segments(snake, new_length);
                    }
                    erased_segs = 0;
                    for (size_t j = 0; j < session.players; ++j) {
                        snake& snake = session.snakes[j];
                        if (!snake.alive()) continue;
                        if (in_packets_buffer[j].boost) {
                            snake.segments[--snake.frac_length] = session.snakes_set.erase_key;
                            erased_segs += 1;
                        }
                    }
                    session.snakes_set.refresh();
                    session.snakes_set.erase(erased_segs);
                    // Add and remove food
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
                    for (size_t j = 0; j < delta.foods_removed_size; ++j) {
                        erased_foods[j]->pos = session.food_set.erase_key;
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
                    std::ranges::none_of(session.snakes_view(), [](const snake& s) { return s.alive(); })) {
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
