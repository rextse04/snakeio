#pragma once
#include <config.hpp>
#include <snake_status.hpp>
#include <vector.hpp>

namespace snakeio::gpu {
    struct session {
        scalar_t width, height;
        struct {
            scalar_t* speed;
            scalar_t* angle;
            scalar_t* width;
            scalar_t* frac_length;
            score_t* score;
            boost_t* boost;
            snake_status_t* status;
            unsigned char* status_data;
            size_t* length;
            vector2d* segments;
        } snakes;

    };
}
