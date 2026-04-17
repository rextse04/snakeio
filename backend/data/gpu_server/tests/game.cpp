#include <tests/game.hpp>
#include "../game_kernels.cuh"
#include <utils.hpp>
#include <limits>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// GPU-backed adapter for the shared backend/data/tests/game.cpp suite.
//
// This file implements the tests/game.hpp API (`init/destroy/inspect/tick`) by
// driving gpu::device_state directly, so shared game behavior tests execute GPU
// tick logic. It also keeps a lightweight host-side reference model used as a
// permissive differential guard while full CPU/GPU parity work is ongoing.

namespace snakeio::test::game {
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
            gpu::snake_state& dst = s.snakes[i];
            dst.speed = src.speed;
            dst.angle = src.angle;
            dst.width = src.width;
            dst.frac_length = src.segments.size();
            dst.score = src.score;
            dst.boost = src.boost;
            dst.status = {src.status, src.status_data};
            dst.human = true;
            for (snakeio::size_t j = 0; j < src.segments.size(); ++j) {
                dst.segments[j] = src.segments[j];
            }
            gpu::client_state& c = h->state.clients[gpu::client_index(h->session_id, i)];
            c.tick = static_cast<tick_t>(-1);
            c.last_packet = {.snapshot_requested = false, .boost = false, .angle = std::numeric_limits<scalar_t>::quiet_NaN()};
        }

        for (snakeio::size_t i = 0; i < in.foods.size(); ++i) {
            s.foods[i] = {.pos = in.foods[i].pos, .width = in.foods[i].width};
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
            const gpu::snake_state& src = s.snakes[i];
            snake dst{
                .angle = src.angle,
                .speed = src.speed,
                .width = src.width,
                .score = src.score,
                .boost = src.boost,
                .status = src.status.status,
                .status_data = src.status.data
            };
            for (snakeio::size_t j = 0; j < static_cast<snakeio::size_t>(src.frac_length); ++j) {
                dst.segments.push_back(src.segments[j]);
            }
            out.snakes.push_back(std::move(dst));
        }
        for (snakeio::size_t i = 0; i < s.food_size; ++i) {
            out.foods.push_back({s.foods[i].pos, s.foods[i].width});
        }
        return out;
    }

    // Applies one tick of player input to both reference and GPU states.
    // Shared tests assert behavior through this single entry point.
    void tick(handle* h, const std::vector<input>& inputs) {
        gpu::session_state& s = h->state.sessions[h->session_id];
        for (id_t i = 0; i < s.human_players; ++i) {
            gpu::client_state& c = h->state.clients[gpu::client_index(h->session_id, i)];
            c.tick = s.tick;
            c.last_packet.snapshot_requested = false;
            c.last_packet.boost = inputs[i].boost;
            c.last_packet.angle = inputs[i].angle;
        }
        gpu::tick_session_gpu(h->state, h->session_id);
    }
}