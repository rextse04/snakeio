#pragma once

namespace snakeio {
    enum class snake_status_t : unsigned char {
        alive, killed_by_snake, killed_by_wall
    };
}