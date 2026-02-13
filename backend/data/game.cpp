#include "game.hpp"
#include "config.hpp"
#include <cmath>
#include <algorithm>

void snakeio::game::generate_session(game_session& session,
    id_t human_players, id_t ai_players, std::span<const key_t> keys) noexcept {
    session.players = human_players + ai_players;
    session.width = game_width_psqp * std::sqrt(session.players);
    session.height = game_height_psqp * std::sqrt(session.players);
    std::ranges::copy(keys, session.keys.begin());
    std::uniform_real_distribution<float> angle_dist(0, M_PI * 2);
    std::uniform_real_distribution<float> width_dist(0, session.width), height_dist(0, session.height);
    for (id_t i = 0; i < session.players; ++i) {
        snake& s = session.snakes[i];
        s.speed = snake_init_speed;
        s.angle = angle_dist(rng_);
        s.width = snake_init_width;
        s.length = snake_init_length;
        s.segments[0] = {width_dist(rng_), height_dist(rng_)};
        for (score_t seg = 1; seg < s.length; ++seg) {
            s.segments[seg] = s.segments[seg-1] + vector2d{std::cos(s.angle) * s.width, std::sin(s.angle) * s.width};
        }
        s.alive = true;
        s.human = i < human_players;
    }
    session.foods_size = game_init_food_pp * session.players;
    for (size_t i = 0; i < session.foods_size; ++i) {
        session.foods[i] = {width_dist(rng_), height_dist(rng_)};
    }
}
