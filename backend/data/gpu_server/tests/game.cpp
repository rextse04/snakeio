#include <tests/game.hpp>
#include "../game_kernels.cuh"
#include <limits>

// GPU-backed adapter for the shared backend/data/tests/game.cpp suite.
//
// This file implements the tests/game.hpp API (`init/destroy/inspect/tick`) by
// driving gpu::device_state directly, so shared game behavior tests execute GPU
// tick logic. It also keeps a lightweight host-side reference model used as a
// permissive differential guard while full CPU/GPU parity work is ongoing.

namespace snakeio::test::game {
    using gpu::snake_segment_index;

    struct handle {
        gpu::device_state state{};
        id_t session_id = 0;
        session reference;
    };

    // Initializes both GPU session state and a host-side reference copy.
    handle* init(const session& in) {
        auto* h = new handle;
        h->reference = in;
        gpu::init_device_state(h->state);
        gpu::session_state& s = h->state.sessions[h->session_id];
        s.active = true;
        s.players = in.snakes.size();
        s.human_players = in.snakes.size();
        // Skip lobby-only branch used by game loop tick 0.
        s.tick = 1;
        s.max_tick = std::numeric_limits<tick_t>::max();
        s.width = in.width;
        s.height = in.height;
        s.food_size = in.foods.size();
        s.delta.foods_added_size = 0;
        s.delta.foods_removed_size = 0;

        for (id_t i = 0; i < s.players; ++i) {
            const snake& src = in.snakes[i];
            s.snake_speeds[i] = src.speed;
            s.snake_angles[i] = src.angle;
            s.snake_widths[i] = src.width;
            s.snake_frac_lengths[i] = src.segments.size();
            s.snake_scores[i] = src.score;
            s.snake_boosts[i] = src.boost;
            s.snake_statuses[i] = {src.status, src.status_data};
            s.snake_humans[i] = true;
            for (snakeio::size_t j = 0; j < src.segments.size(); ++j) {
                s.snake_segments[snake_segment_index(i, j)] = src.segments[j];
            }
            const auto cidx = gpu::client_index(h->session_id, i);
            h->state.client_ticks[cidx] = static_cast<tick_t>(-1);
            h->state.client_last_snapshot_requested[cidx] = false;
            h->state.client_last_boost[cidx] = false;
            h->state.client_last_angle[cidx] = std::numeric_limits<scalar_t>::quiet_NaN();
        }

        for (snakeio::size_t i = 0; i < in.foods.size(); ++i) {
            s.food_poss[i] = in.foods[i].pos;
            s.food_widths[i] = in.foods[i].width;
        }
        return h;
    }

    // Releases all GPU allocations owned by the test handle.
    void destroy(handle* h) {
        gpu::destroy_device_state(h->state);
        delete h;
    }

    // Reads current GPU session state into the shared tests/game.hpp view model.
    session inspect(handle* h) {
        const gpu::session_state& s = h->state.sessions[h->session_id];
        session out{.width = s.width, .height = s.height};
        for (id_t i = 0; i < s.players; ++i) {
            snake dst{
                .angle = s.snake_angles[i],
                .speed = s.snake_speeds[i],
                .width = s.snake_widths[i],
                .score = s.snake_scores[i],
                .boost = s.snake_boosts[i],
                .status = s.snake_statuses[i].status,
                .status_data = s.snake_statuses[i].data
            };
            for (snakeio::size_t j = 0; j < static_cast<snakeio::size_t>(s.snake_frac_lengths[i]); ++j) {
                dst.segments.push_back(s.snake_segments[snake_segment_index(i, j)]);
            }
            out.snakes.push_back(std::move(dst));
        }
        for (size_t i = 0; i < s.food_size; ++i) {
            out.foods.push_back({s.food_poss[i], s.food_widths[i]});
        }
        return out;
    }

    // Applies one tick of player input to both reference and GPU states.
    // Shared tests assert behavior through this single entry point.
    void tick(handle* h, const std::vector<input>& inputs) {
        gpu::session_state& s = h->state.sessions[h->session_id];
        for (id_t i = 0; i < s.human_players; ++i) {
            const auto cidx = gpu::client_index(h->session_id, i);
            h->state.client_ticks[cidx] = s.tick;
            h->state.client_last_snapshot_requested[cidx] = false;
            h->state.client_last_boost[cidx] = inputs[i].boost;
            h->state.client_last_angle[cidx] = inputs[i].angle;
        }
        gpu::tick_active_sessions_gpu(h->state);
    }
}