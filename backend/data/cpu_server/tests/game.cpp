#include <tests/game.hpp>
#include "../tick_core.hpp"
#include "../session.hpp"
#include "../parse.hpp"
#include <algorithm>

namespace snakeio::test::game {
    handle* init(const session& session) {
        const auto out = new cpu::session;
        out->players = session.snakes.size();
        out->human_players = session.snakes.size();
        out->tick = 0;
        out->max_tick = -1;
        out->width = session.width;
        out->height = session.height;
        for (id_t i = 0; i < session.snakes.size(); ++i) {
            const auto& in = session.snakes[i];
            static_cast<cpu::snake_basic&>(out->snakes[i]) = {
                .speed = in.speed,
                .angle = in.angle,
                .width = in.width,
                .frac_length = static_cast<scalar_t>(in.segments.size()),
                .score = in.score,
                .boost = in.boost,
                .status = {in.status},
                .human = true
            };
            auto& out_segments = out->snakes[i].segments;
            std::ranges::copy(in.segments, out_segments.begin());
            for (size_t j = 0; j < in.segments.size(); ++j) {
                out->snakes_set.emplace(&out->snakes[i], &out_segments[j]);
            }
        }
        for (const food& food : session.foods) {
            out->food_set.emplace(food.pos, food.width);
        }
        out->snakes_set.refresh();
        out->food_set.refresh();
        return reinterpret_cast<handle*>(out);
    }
    void destroy(handle* game) {
        delete reinterpret_cast<cpu::session*>(game);
    }
    session inspect(handle* h) {
        cpu::session& session = *reinterpret_cast<cpu::session*>(h);
        game::session out{
            .width = session.width,
            .height = session.height
        };
        for (cpu::snake& in_snake : session.snakes_view()) {
            snake out_snake{
                .angle = in_snake.angle,
                .speed = in_snake.speed,
                .width = in_snake.width,
                .score = in_snake.score,
                .boost = in_snake.boost,
                .status = in_snake.status.status,
                .status_data = in_snake.status.data
            };
            for (const vector2d& seg : in_snake.segments_view()) {
                out_snake.segments.push_back(seg);
            }
            out.snakes.push_back(std::move(out_snake));
        }
        for (cpu::food& food : session.food_set) {
            out.foods.emplace_back(food.pos, food.width);
        }
        return out;
    }
    void tick(handle* h, const std::vector<input>& inputs) {
        cpu::session& session = *reinterpret_cast<cpu::session*>(h);
        std::vector<cpu::in_packet_info> in_packets_buffer;
        for (const input& input : inputs) {
            in_packets_buffer.emplace_back(cpu::in_packet{
                .boost = input.boost,
                .angle = input.angle
            }, 0);
        }
        cpu::out_delta delta;
        cpu::tick_core(0, session, nullptr, in_packets_buffer, delta);
    }
}