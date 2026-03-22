#include "session.hpp"

using namespace snakeio;
using namespace snakeio::cpu;

void session::add_segments(snake& snake, scalar_t new_length) noexcept {
    const size_t current_len = snake.length(), new_len = new_length;
    const vector2d tail = snake.segments[current_len - 1];
    const vector2d dir = tail - snake.segments[current_len - 2];
    for (size_t i = current_len; i < new_len; ++i) {
        vector2d& seg = snake.segments[i];
        seg = tail + dir * static_cast<scalar_t>(i - current_len);
        snakes_set.emplace(&snake, &seg);
    }
    snake.frac_length = new_length;
}