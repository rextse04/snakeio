#include "tests/game.hpp"
#include <config.hpp>
#include <vector.hpp>
#include "session.hpp"
#include <cmath>
#include <algorithm>
#include <random>

using namespace snakeio;
using namespace snakeio::cpu;

namespace {
    void sync_snake_dims(snake_basic& snake) noexcept {
        const auto progress = (snake.frac_length - snake_init_length) / (snake_max_length - snake_init_length);
        const scalar_t scaled = 1 - (1 - progress) * (1 - progress); // ease out quadratic
        snake.speed = snake_init_speed + (snake_min_speed - snake_init_speed) * scaled;
        if (snake.boost) {
            snake.speed *= snake_boost_speed_factor;
        }
        snake.width = snake_init_width + (snake_max_width - snake_init_width) * scaled;
    }
    struct erased_snake {
        snake* target;
        snake_status reason;
    };
}

void test::game::tick_core(tick_t tick, session& session, snakeio::game::random_engine* rng,
    std::span<in_packet_info> in_packets_buffer, out_delta& delta) noexcept {
    using enum snake_status_t;
    using snakeio::size_t;
    session::snakes_set_type::index_array_type snakes_set_index_array;
    // AIs play
    for (size_t j = 0; j < session.players; ++j) {
        snake& snake = session.snakes[j];
        if (!snake.alive() || snake.human) continue;
        const vector2d head = snake.segments[0];
        const scalar_t sightrange = snake.speed * 32;
        // Forward bias
        vector2d forward_force = vector2d{std::cos(snake.angle), std::sin(snake.angle)};
        // Food attraction
        constexpr scalar_t eps = 1./1024;
        const auto get_field = [](vector2d d) noexcept {
            return d / (d.norm_sq() + eps);
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
            return 1 / (pos - snake.width + eps) - 1 / (dist - snake.width + eps);
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
    size_t erased_segs = 0;
    for (id_t j = 0; j < session.players; ++j) {
        in_packet_info& in_packet = in_packets_buffer[j];
        if (in_packet.tick != tick) continue;
        snake& snake = session.snakes[j];
        if (!snake.alive()) continue;
        if (std::isfinite(in_packet.angle)) {
            snake.angle += std::clamp(angle_diff(in_packet.angle, snake.angle),
                -snake_max_turn_per_tick, snake_max_turn_per_tick);
        }
        if (in_packet.boost &= snake.frac_length > snake_init_length) {
            snake.boost += snake_seg_to_boost_ticks;
            snake.segments[--snake.frac_length] = session.snakes_set.erase_key;
            ++erased_segs;
        }
        sync_snake_dims(snake);
        // saturation sub, no need to check if boost is 0 :)
        --snake.boost;
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
    session.snakes_set.refresh(snakes_set_index_array);
    session.snakes_set.erase(erased_segs);
    // Detect collision with wall or other snakes
    std::array<erased_snake, game_max_players> to_erase;
    id_t to_erase_size = 0;
    for (id_t j = 0; j < session.players; ++j) {
        snake& snake = session.snakes[j];
        if (!snake.alive()) continue;
        const vector2d head = snake.segments.front();
        if (head[0] < snake.width - game_collision_eps || head[0] > session.width - snake.width + game_collision_eps ||
            head[1] < snake.width - game_collision_eps || head[1] > session.height - snake.width + game_collision_eps) {
            to_erase[to_erase_size++] = {&snake, killed_by_wall};
            continue;
        }
        for (const auto [other_snake, seg] :
            session.snakes_set.find_possible(snakes_set_index_array, head, snake_max_width * 2)) {
            if (other_snake == &snake) continue;
            const scalar_t req = snake.width + other_snake->width - game_collision_eps;
            if ((*seg - head).norm_sq() >= req * req)
                continue;
            const unsigned char other_snake_id = other_snake - session.snakes.data();
            to_erase[to_erase_size++] = {&snake, killed_by_snake, other_snake_id};
            break;
        }
    }
    erased_segs = 0;
    for (id_t j = 0; j < to_erase_size; ++j) {
        const auto [target, reason] = to_erase[j];
        target->status = reason;
        std::uniform_real_distribution<scalar_t> food_width_dist(seg_food_min_width, seg_food_max_width);
        for (const vector2d& seg : target->segments_view()) {
            if (session.food_set.size() + delta.foods_added_size >= game_max_food) break;
            if (!rng || std::bernoulli_distribution(seg_to_food_prob)(*rng)) {
                delta.foods_added[delta.foods_added_size++] = {
                    .pos = seg,
                    .width = rng ? food_width_dist(*rng) : seg_food_min_width
                };
            }
        }
        std::ranges::fill(target->segments_view(), session.snakes_set.erase_key);
        erased_segs += target->length();
        target->frac_length = 0;
    }
    session.snakes_set.refresh(snakes_set_index_array);
    session.snakes_set.erase(erased_segs);
    // Detect collision with food and recalculate attributes
    std::array<food*, game_max_food> erased_foods;
    for (snake& snake : session.snakes_view()) {
        if (!snake.alive()) continue;
        scalar_t new_length = snake.frac_length;
        for (food& food : session.food_set.find_possible(snake.segments[0], snake_max_width + food_max_width)) {
            const scalar_t req = snake.width + food.width - game_collision_eps;
            if ((food.pos - snake.segments[0]).norm_sq() >= req * req)
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
    session.snakes_set.refresh(snakes_set_index_array);
    // Add and remove food
    const auto food_added = rng
        ? std::min(game_max_food - session.food_set.size() - delta.foods_added_size,
            std::poisson_distribution<size_t>(food_per_player_tick * session.players)(*rng))
        : 0;
    std::uniform_real_distribution<scalar_t> food_width_dist(gen_food_min_width, gen_food_max_width);
    for (size_t j = 0; j < food_added; ++j) {
        delta.foods_added[delta.foods_added_size++] = {
            .pos = {
                std::uniform_real_distribution<scalar_t>(0, session.width)(*rng),
                std::uniform_real_distribution<scalar_t>(0, session.height)(*rng)
            },
            .width = food_width_dist(*rng)
        };
    }
    for (size_t j = 0; j < delta.foods_removed_size; ++j) {
        erased_foods[j]->pos = session.food_set.erase_key;
    }
    session.food_set.insert(std::span(delta.foods_added.begin(), delta.foods_added_size));
    session.food_set.refresh();
    session.food_set.erase(delta.foods_removed_size);
}