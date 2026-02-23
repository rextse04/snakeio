#include "game.hpp"
#include "config.hpp"
#include <cmath>
#include <algorithm>

void snakeio::game::generate_session(session_snapshot &snapshot,
    id_t human_players, id_t ai_players, std::span<const key_t> keys) noexcept {
    snapshot.players = human_players + ai_players;
    snapshot.width = game_width_psqp * std::sqrt(snapshot.players);
    snapshot.height = game_height_psqp * std::sqrt(snapshot.players);
    std::ranges::copy(keys, snapshot.keys.begin());
    std::uniform_real_distribution<float> angle_dist(0, M_PI * 2);
    std::uniform_real_distribution<float> width_dist(0, snapshot.width), height_dist(0, snapshot.height);
    for (id_t i = 0; i < snapshot.players; ++i) {
        snake& s = snapshot.snakes[i];
        s.basic = {
            .speed = snake_init_speed,
            .angle = angle_dist(rng_),
            .width = snake_init_width,
            .length = snake_init_length,
            .score = 0
        };
        s.segments[0] = {width_dist(rng_), height_dist(rng_)};
        for (size_t seg = 1; seg < s.basic.length; ++seg) {
            s.segments[seg] = s.segments[seg-1] +
                vector2d{std::cos(s.basic.angle) * s.basic.width, std::sin(s.basic.angle) * s.basic.width};
        }
        s.alive = true;
        s.human = i < human_players;
    }
    snapshot.foods_size = game_init_food_pp * snapshot.players;
    for (size_t i = 0; i < snapshot.foods_size; ++i) {
        snapshot.foods[i] = {width_dist(rng_), height_dist(rng_)};
    }
}
