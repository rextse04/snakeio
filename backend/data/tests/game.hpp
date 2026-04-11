#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <snake_status.hpp>
#include <vector>

namespace snakeio::test::game {
    struct handle;
    struct snake {
        scalar_t angle, speed, width;
        score_t score;
        boost_t boost;
        snake_status_t status;
        unsigned char status_data;
        std::vector<vector2d> segments;
    };
    struct food {
        vector2d pos;
        scalar_t width;
    };
    struct session {
        scalar_t width, height;
        std::vector<snake> snakes;
        std::vector<food> foods;
    };
    struct input {
        bool boost;
        scalar_t angle;
    };

    handle* init(const session& session);
    void destroy(handle* h);
    // It is guaranteed that the given session_id is valid.
    session inspect(handle* h);
    // Random food generation (not due to snake death) should be disabled.
    void tick(handle* h, const std::vector<input>& inputs);
}
