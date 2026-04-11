#pragma once

namespace snakeio {
    // snake_status_t should be accompanied by another one-byte data field.
    // See protocol.md for more information.
    enum class snake_status_t : unsigned char {
        alive, killed_by_snake, killed_by_wall
    };
}