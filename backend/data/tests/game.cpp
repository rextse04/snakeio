#include <cpp_utils/tests/common.hpp>
#include "game.hpp"
#include "print_vector.hpp"
#include <config.hpp>
#include <limits>
#include <cmath>
#include <algorithm>
#include <array>

using namespace snakeio;
using namespace snakeio::test::game;
namespace utf = boost::unit_test;
using enum snake_status_t;

namespace {
    void init_segments(snake& snake) {
        while (snake.segments.size() < snake_init_length) {
            snake.segments.push_back(snake.segments.back() - vector2d{
                std::cos(snake.angle) * snake.speed,
                std::sin(snake.angle) * snake.speed
            });
        }
    }
}

BOOST_AUTO_TEST_CASE(basic_test) {
    const vector2d snake1_head{8, game_max_height / 2},
    snake0_head = snake1_head + vector2d{
        snake_init_speed + snake_init_width * 2 - scalar_t(0.1),
        -snake_init_speed * std::floor(snake_init_width * 2 / snake_init_speed)
    };
    const food init_food = {
        .pos = snake0_head - vector2d{0, snake_init_speed + snake_init_width + 0.1},
        .width = std::ceil(1 / food_width_to_seg)
    };
    session init_session{
        .width = game_max_width,
        .height = game_max_height,
        .snakes = {
            snake{
                .angle = -M_PI/2,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {snake0_head}
            },
            snake{
                .angle = 0,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {snake1_head}
            },
            snake{
                .angle = M_PI,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {{0, 0}}
            }
        },
        .foods = {init_food}
    };
    std::ranges::for_each(init_session.snakes, init_segments);
    handle* session = init(init_session);

    const input input0{.boost = false, .angle = std::numeric_limits<scalar_t>::quiet_NaN()};
    tick(session, {input0, input0, input0});
    const ::session tick0 = inspect(session);
    BOOST_REQUIRE_EQUAL(tick0.snakes.size(), 3);
    BOOST_CHECK(tick0.snakes[0].status == alive);
    BOOST_CHECK_EQUAL(tick0.snakes[0].speed, snake_init_speed);
    BOOST_CHECK_EQUAL(tick0.snakes[0].width, snake_init_width);
    BOOST_CHECK_EQUAL(tick0.snakes[0].score, init_food.width);
    BOOST_CHECK_EQUAL(tick0.snakes[0].boost, 0);
    BOOST_CHECK_GT(tick0.snakes[0].segments.size(), snake_init_length);
    if (tick0.snakes[0].segments.size() >= 1) {
        BOOST_CHECK_EQUAL(tick0.snakes[0].segments[0], (snake0_head - vector2d{0, snake_init_speed}));
    }
    const auto check_dead_snakes = [](const ::session& tick) {
        BOOST_CHECK(tick.snakes[1].status == killed_by_snake);
        BOOST_CHECK_EQUAL(tick.snakes[1].status_data, 0);
        BOOST_CHECK_EQUAL(tick.snakes[1].score, 0);
        BOOST_CHECK_EQUAL(tick.snakes[1].boost, 0);
        BOOST_CHECK_EQUAL(tick.snakes[1].segments.size(), 0);
        BOOST_CHECK(tick.snakes[2].status == killed_by_wall);
        BOOST_CHECK_EQUAL(tick.snakes[2].score, 0);
        BOOST_CHECK_EQUAL(tick.snakes[2].boost, 0);
        BOOST_CHECK_EQUAL(tick.snakes[2].segments.size(), 0);
    };
    check_dead_snakes(tick0);
    BOOST_CHECK_GT(tick0.foods.size(), 1);

    const input input1{.boost = false, .angle = 0};
    tick(session, {input1, input1, input1});
    const ::session tick1 = inspect(session);
    BOOST_REQUIRE_EQUAL(tick1.snakes.size(), 3);
    BOOST_CHECK(tick1.snakes[0].status == alive);
    BOOST_CHECK_EQUAL(tick1.snakes[0].angle, tick0.snakes[0].angle + snake_max_turn_per_tick);
    BOOST_CHECK_LT(tick1.snakes[0].speed, snake_init_speed);
    BOOST_CHECK_GT(tick1.snakes[0].width, snake_init_width);
    BOOST_CHECK_EQUAL(tick1.snakes[0].score, tick0.snakes[0].score);
    BOOST_CHECK_EQUAL(tick1.snakes[0].boost, 0);
    BOOST_CHECK_EQUAL(tick1.snakes[0].segments.size(), tick0.snakes[0].segments.size());
    check_dead_snakes(tick1);

    const input input2{.boost = true, .angle = 0};
    tick(session, {input2, input2, input2});
    const ::session tick2 = inspect(session);
    BOOST_REQUIRE_EQUAL(tick2.snakes.size(), 3);
    BOOST_CHECK(tick2.snakes[0].status == alive);
    BOOST_CHECK_EQUAL(tick2.snakes[0].angle, tick1.snakes[0].angle + snake_max_turn_per_tick);
    BOOST_CHECK_EQUAL(tick2.snakes[0].speed, snake_init_speed * snake_boost_speed_factor);
    BOOST_CHECK_EQUAL(tick2.snakes[0].width, snake_init_width);
    BOOST_CHECK_EQUAL(tick2.snakes[0].score, tick1.snakes[0].score);
    BOOST_CHECK_EQUAL(tick2.snakes[0].boost, snake_seg_to_boost_ticks - 1);
    BOOST_CHECK_EQUAL(tick2.snakes[0].segments.size(), tick1.snakes[0].segments.size() - 1);
    check_dead_snakes(tick2);

    const input input3{.boost = true, .angle = 0};
    tick(session, {input3, input3, input3});
    const ::session tick3 = inspect(session);
    BOOST_REQUIRE_EQUAL(tick3.snakes.size(), 3);
    BOOST_CHECK(tick3.snakes[0].status == alive);
    BOOST_CHECK_EQUAL(tick3.snakes[0].angle, tick2.snakes[0].angle + snake_max_turn_per_tick);
    if constexpr (snake_seg_to_boost_ticks > 1) {
        BOOST_CHECK_EQUAL(tick3.snakes[0].speed, tick2.snakes[0].speed);
    } else {
        BOOST_CHECK_EQUAL(tick3.snakes[0].speed, snake_init_speed);
    }
    BOOST_CHECK_EQUAL(tick3.snakes[0].width, tick2.snakes[0].width);
    BOOST_CHECK_EQUAL(tick3.snakes[0].score, tick2.snakes[0].score);
    BOOST_CHECK_EQUAL(tick3.snakes[0].boost, tick2.snakes[0].boost - 1);
    BOOST_CHECK_EQUAL(tick3.snakes[0].segments.size(), tick2.snakes[0].segments.size());
    check_dead_snakes(tick3);

    destroy(session);
}

BOOST_AUTO_TEST_CASE(boundary_test) {
    session init_session{
        .width = game_max_width,
        .height = game_max_height,
        .snakes = {
            snake{
                .angle = -M_PI/2,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {{snake_init_width, snake_init_width + snake_init_speed}}
            },
            snake{
                .angle = M_PI,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {{snake_init_width + snake_init_speed, game_max_height - snake_init_width}}
            },
            snake{
                .angle = M_PI/2,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {{game_max_width - snake_init_width, game_max_height - snake_init_width - snake_init_speed}}
            },
            snake{
                .angle = 0,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {{game_max_width - snake_init_width - snake_init_speed, snake_init_width}}
            },
            snake{
                .angle = 0,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {{game_max_width/2 - snake_init_width - snake_init_speed, game_max_height/2}}
            },
            snake{
                .angle = M_PI,
                .speed = snake_init_speed,
                .width = snake_init_width,
                .segments = {{game_max_width/2 + snake_init_width + snake_init_speed, game_max_height/2}}
            }
        },
        .foods = {
            food{
                .pos = vector2d{game_max_height, game_max_width} / 2,
                .width = 0
            }
        }
    };
    std::ranges::for_each(init_session.snakes, init_segments);
    handle* session = init(init_session);

    const input input0{.boost = false, .angle = std::numeric_limits<scalar_t>::quiet_NaN()};
    tick(session, {input0, input0, input0, input0, input0, input0});
    const ::session tick0 = inspect(session);
    BOOST_REQUIRE_EQUAL(tick0.snakes.size(), 6);
    for (id_t i = 0; i < tick0.snakes.size(); ++i) {
        BOOST_TEST_CONTEXT(i) {
            BOOST_CHECK(tick0.snakes[i].status == alive);
        }
    }
    BOOST_CHECK_EQUAL(tick0.foods.size(), 1);
}