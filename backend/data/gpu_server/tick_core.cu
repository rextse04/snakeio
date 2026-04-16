#include "game_kernels.cuh"
#include <utils.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cuda/std/algorithm>
#include <cuda/std/cmath>
#include <cuda/std/limits>

namespace snakeio::gpu {
    namespace {
        __host__ __device__ constexpr std::size_t inbox_index(id_t session_id, id_t player_id) noexcept {
            return static_cast<std::size_t>(session_id) * game_max_players + player_id;
        }
        __device__ constexpr std::size_t align_text(std::size_t n) noexcept {
            return data_packet_align * (n / data_packet_align + (n % data_packet_align != 0));
        }
        __device__ constexpr void store32(std::byte* out, std::uint_least32_t value) noexcept {
            out[0] = static_cast<std::byte>(value >> 0);
            out[1] = static_cast<std::byte>(value >> 8);
            out[2] = static_cast<std::byte>(value >> 16);
            out[3] = static_cast<std::byte>(value >> 24);
        }
        __device__ constexpr void storef32(std::byte* out, float value) noexcept {
            union {
                float f;
                unsigned u;
            } bits{.f = value};
            out[0] = static_cast<std::byte>(bits.u >> 0);
            out[1] = static_cast<std::byte>(bits.u >> 8);
            out[2] = static_cast<std::byte>(bits.u >> 16);
            out[3] = static_cast<std::byte>(bits.u >> 24);
        }
        __device__ constexpr scalar_t clamp_turn(scalar_t to, scalar_t from) noexcept {
            scalar_t d = stdc::remainder(to - from, scalar_t(M_PI * 2));
            d = d < -snake_max_turn_per_tick ? -snake_max_turn_per_tick : d;
            d = d > snake_max_turn_per_tick ? snake_max_turn_per_tick : d;
            return d;
        }
        __device__ constexpr unsigned hash_u32(unsigned x) noexcept {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }
        __device__ constexpr scalar_t rand01(unsigned seed) noexcept {
            return static_cast<scalar_t>(hash_u32(seed) & 0x00ffffff) / static_cast<scalar_t>(0x01000000);
        }
        __device__ constexpr bool alive(const session_batch::sessions_type* s, id_t session_id, id_t player_id) noexcept {
            return s->status[session_id][player_id].status == snake_status_t::alive;
        }
        __device__ void sync_snake_dims(session_batch::sessions_type* s, id_t session_id, id_t player_id) noexcept {
            const scalar_t frac = s->frac_length[session_id][player_id];
            const auto progress = (frac - snake_init_length) / (snake_max_length - snake_init_length);
            const scalar_t scaled = 1 - (1 - progress) * (1 - progress);
            scalar_t speed = snake_init_speed + (snake_min_speed - snake_init_speed) * scaled;
            if (s->boost[session_id][player_id]) {
                speed *= snake_boost_speed_factor;
            }
            s->speed[session_id][player_id] = speed;
            s->width[session_id][player_id] = snake_init_width + (snake_max_width - snake_init_width) * scaled;
        }
        __device__ void add_segments(session_batch::sessions_type* s, id_t session_id, id_t player_id, scalar_t new_length) noexcept {
            const size_t current_len = s->length[session_id][player_id];
            const size_t new_len = static_cast<size_t>(new_length);
            if (current_len < 2 || new_len <= current_len) {
                s->frac_length[session_id][player_id] = new_length;
                return;
            }
            const vector2d tail = s->segments[session_id][player_id][current_len - 1];
            const vector2d dir = tail - s->segments[session_id][player_id][current_len - 2];
            for (size_t i = current_len; i < new_len; ++i) {
                s->segments[session_id][player_id][i] = tail + dir * static_cast<scalar_t>(i - current_len);
            }
            s->length[session_id][player_id] = new_len;
            s->frac_length[session_id][player_id] = new_length;
        }

        __device__ std::size_t store_snake_basic(std::byte* out, const session_batch::sessions_type* s,
            id_t session_id, id_t player_id) noexcept {
            storef32(out + 0, s->speed[session_id][player_id]);
            storef32(out + 4, s->angle[session_id][player_id]);
            storef32(out + 8, s->width[session_id][player_id]);
            store32(out + 12, s->length[session_id][player_id]);
            store32(out + 16, s->score[session_id][player_id]);
            out[20] = static_cast<std::byte>(s->boost[session_id][player_id]);
            out[21] = static_cast<std::byte>(s->status[session_id][player_id].status);
            out[22] = static_cast<std::byte>(s->status[session_id][player_id].data);
            out[23] = static_cast<std::byte>(s->human[session_id][player_id]);
            return 24;
        }

        __device__ std::size_t store_delta(std::byte* out, const session_batch::sessions_type* s,
            id_t session_id, const food* foods_added, std::size_t foods_added_size,
            const vector2d* foods_removed, std::size_t foods_removed_size) noexcept {
            std::byte* it = out;
            store32(it, 0);
            it += 4;
            for (id_t i = 0; i < s->players[session_id]; ++i) {
                it += store_snake_basic(it, s, session_id, i);
            }
            store32(it, foods_added_size);
            it += 4;
            for (std::size_t i = 0; i < foods_added_size; ++i) {
                storef32(it + 0, foods_added[i].pos[0]);
                storef32(it + 4, foods_added[i].pos[1]);
                storef32(it + 8, foods_added[i].width);
                it += 12;
            }
            store32(it, foods_removed_size);
            it += 4;
            for (std::size_t i = 0; i < foods_removed_size; ++i) {
                storef32(it + 0, foods_removed[i][0]);
                storef32(it + 4, foods_removed[i][1]);
                it += 8;
            }
            const std::size_t size = align_text(it - out);
            for (std::size_t i = it - out; i < size; ++i) {
                out[i] = std::byte(0);
            }
            return size;
        }

        __device__ std::size_t store_snapshot(std::byte* out, const session_batch::sessions_type* s, id_t session_id) noexcept {
            std::byte* it = out;
            store32(it + 0, 1);
            storef32(it + 4, s->game_width[session_id]);
            storef32(it + 8, s->game_height[session_id]);
            store32(it + 12, s->max_tick[session_id]);
            store32(it + 16, s->players[session_id]);
            it += 20;
            for (id_t i = 0; i < s->players[session_id]; ++i) {
                it += store_snake_basic(it, s, session_id, i);
                for (size_t j = 0; j < s->length[session_id][i]; ++j) {
                    storef32(it + 0, s->segments[session_id][i][j][0]);
                    storef32(it + 4, s->segments[session_id][i][j][1]);
                    it += 8;
                }
            }
            store32(it, s->food_size[session_id]);
            it += 4;
            for (size_t i = 0; i < s->food_size[session_id]; ++i) {
                storef32(it + 0, s->foods[session_id][i].pos[0]);
                storef32(it + 4, s->foods[session_id][i].pos[1]);
                storef32(it + 8, s->foods[session_id][i].width);
                it += 12;
            }
            const std::size_t size = align_text(it - out);
            for (std::size_t i = it - out; i < size; ++i) {
                out[i] = std::byte(0);
            }
            return size;
        }

        __device__ std::size_t store_lobby_status(std::byte* out, id_t human_players,
            const in_packet_info* inbox, id_t session_id) noexcept {
            std::byte* it = out;
            store32(it, 2);
            it += 4;
            for (id_t i = 0; i < human_players; ++i) {
                const auto& in = inbox[inbox_index(session_id, i)];
                *(it++) = static_cast<std::byte>(in.tick == 0);
            }
            const std::size_t size = align_text(it - out);
            for (std::size_t i = it - out; i < size; ++i) {
                out[i] = std::byte(0);
            }
            return size;
        }

        __device__ std::size_t store_termination(std::byte* out, const session_batch::sessions_type* s, id_t session_id) noexcept {
            std::byte* it = out;
            store32(it + 0, 3);
            store32(it + 4, s->max_tick[session_id]);
            it += 8;
            for (id_t i = 0; i < s->players[session_id]; ++i) {
                it += store_snake_basic(it, s, session_id, i);
            }
            const std::size_t size = align_text(it - out);
            for (std::size_t i = it - out; i < size; ++i) {
                out[i] = std::byte(0);
            }
            return size;
        }

        __global__ void add_session_kernel(session_batch::sessions_type* s, const add_session_req* req) {
            if (threadIdx.x != 0 || blockIdx.x != 0) return;
            const id_t sid = req->session_id;
            const id_t players = req->human_players + req->ai_players;
            s->active[sid] = true;
            s->players[sid] = players;
            s->human_players[sid] = req->human_players;
            s->tick[sid] = 0;
            s->max_tick[sid] = req->max_tick;
            s->game_width[sid] = game_width_psqp * stdc::sqrt(static_cast<scalar_t>(players));
            s->game_height[sid] = game_height_psqp * stdc::sqrt(static_cast<scalar_t>(players));
            for (id_t i = 0; i < players; ++i) {
                const scalar_t angle = rand01(sid * 1315423911u + i * 2654435761u) * scalar_t(M_PI * 2) - scalar_t(M_PI);
                s->speed[sid][i] = snake_init_speed;
                s->angle[sid][i] = angle;
                s->width[sid][i] = snake_init_width;
                s->frac_length[sid][i] = static_cast<scalar_t>(snake_init_length);
                s->score[sid][i] = 0;
                s->boost[sid][i] = 0;
                s->status[sid][i] = {snake_status_t::alive, 0};
                s->human[sid][i] = i < req->human_players;
                s->length[sid][i] = snake_init_length;

                const vector2d head{
                    rand01(hash_u32((sid + 1) * (i + 17))) * s->game_width[sid],
                    rand01(hash_u32((sid + 3) * (i + 29))) * s->game_height[sid]
                };
                s->segments[sid][i][0] = head;
                for (size_t j = 1; j < snake_init_length; ++j) {
                    s->segments[sid][i][j] = s->segments[sid][i][j - 1] - vector2d{
                        stdc::cos(angle) * snake_init_speed,
                        stdc::sin(angle) * snake_init_speed
                    };
                }
            }
            s->food_size[sid] = static_cast<size_t>(game_init_food_pp * players);
            for (size_t i = 0; i < s->food_size[sid]; ++i) {
                s->foods[sid][i] = {
                    .pos = {
                        rand01(hash_u32(static_cast<unsigned>(sid * 911 + i * 31 + 1))) * s->game_width[sid],
                        rand01(hash_u32(static_cast<unsigned>(sid * 911 + i * 31 + 2))) * s->game_height[sid]
                    },
                    .width = gen_food_min_width +
                        rand01(hash_u32(static_cast<unsigned>(sid * 911 + i * 31 + 3))) *
                        (gen_food_max_width - gen_food_min_width)
                };
            }
        }

        __global__ void tick_kernel(session_batch::sessions_type* s, const in_packet_info* inbox,
            session_tick_result* out, id_t session_id) {
            if (threadIdx.x != 0 || blockIdx.x != 0) return;

            out->active = s->active[session_id];
            out->session_id = session_id;
            if (!out->active) {
                return;
            }
            const tick_t tick = s->tick[session_id];
            out->tick = tick;
            out->human_players = s->human_players[session_id];
            out->terminate = false;
            out->send_lobby = false;
            for (id_t i = 0; i < game_max_players; ++i) {
                out->joined[i] = false;
                out->snapshot_requested[i] = false;
            }

            if (tick == 0) {
                bool all_joined = true;
                for (id_t i = 0; i < s->human_players[session_id]; ++i) {
                    const auto& in = inbox[inbox_index(session_id, i)];
                    out->joined[i] = in.tick == 0;
                    all_joined &= out->joined[i];
                }
                if (!all_joined) {
                    out->send_lobby = true;
                    out->lobby_size = store_lobby_status(out->lobby_text, s->human_players[session_id], inbox, session_id);
                    return;
                }
                for (id_t i = 0; i < s->human_players[session_id]; ++i) {
                    out->snapshot_requested[i] = true;
                }
                out->snapshot_size = store_snapshot(out->snapshot_text, s, session_id);
                s->tick[session_id] = tick + 1;
                return;
            }

            for (id_t i = 0; i < s->players[session_id]; ++i) {
                if (!alive(s, session_id, i)) continue;
                bool do_boost = false;
                scalar_t req_angle = s->angle[session_id][i];
                if (i < s->human_players[session_id]) {
                    const auto& in = inbox[inbox_index(session_id, i)];
                    if (in.tick == tick) {
                        out->snapshot_requested[i] = in.snapshot_requested;
                        do_boost = in.boost;
                        req_angle = in.angle;
                    }
                }
                if (stdc::isfinite(req_angle)) {
                    s->angle[session_id][i] += clamp_turn(req_angle, s->angle[session_id][i]);
                }
                if (do_boost && s->frac_length[session_id][i] > snake_init_length) {
                    s->boost[session_id][i] = static_cast<boost_t>(s->boost[session_id][i] + snake_seg_to_boost_ticks);
                    if (s->length[session_id][i] > 0) {
                        --s->length[session_id][i];
                    }
                    s->frac_length[session_id][i] = static_cast<scalar_t>(s->length[session_id][i]);
                }
                sync_snake_dims(s, session_id, i);
                if (s->boost[session_id][i] > 0) {
                    --s->boost[session_id][i];
                }
            }

            for (id_t i = 0; i < s->players[session_id]; ++i) {
                if (!alive(s, session_id, i)) continue;
                const size_t len = s->length[session_id][i];
                for (size_t j = len - 1; j > 0; --j) {
                    s->segments[session_id][i][j] = s->segments[session_id][i][j - 1];
                }
                s->segments[session_id][i][0] += vector2d{
                    stdc::cos(s->angle[session_id][i]) * s->speed[session_id][i],
                    stdc::sin(s->angle[session_id][i]) * s->speed[session_id][i]
                };
            }

            food foods_added[game_max_food];
            vector2d foods_removed[game_max_food];
            size_t foods_added_size = 0;
            size_t foods_removed_size = 0;

            for (id_t i = 0; i < s->players[session_id]; ++i) {
                if (!alive(s, session_id, i)) continue;
                const vector2d head = s->segments[session_id][i][0];
                const scalar_t width = s->width[session_id][i];
                if (head[0] < width - game_collision_eps || head[0] > s->game_width[session_id] - width + game_collision_eps ||
                    head[1] < width - game_collision_eps || head[1] > s->game_height[session_id] - width + game_collision_eps) {
                    s->status[session_id][i] = {snake_status_t::killed_by_wall, 0};
                }
            }

            for (id_t i = 0; i < s->players[session_id]; ++i) {
                if (!alive(s, session_id, i)) continue;
                const vector2d head = s->segments[session_id][i][0];
                for (id_t k = 0; k < s->players[session_id]; ++k) {
                    if (k == i || !alive(s, session_id, k)) continue;
                    for (size_t seg = 0; seg < s->length[session_id][k]; ++seg) {
                        const scalar_t req = s->width[session_id][i] + s->width[session_id][k] - game_collision_eps;
                        if ((s->segments[session_id][k][seg] - head).norm_sq() < req * req) {
                            s->status[session_id][i] = {snake_status_t::killed_by_snake, static_cast<unsigned char>(k)};
                            goto done_collision;
                        }
                    }
                }
                done_collision:;
            }

            for (id_t i = 0; i < s->players[session_id]; ++i) {
                if (alive(s, session_id, i)) continue;
                for (size_t seg = 0; seg < s->length[session_id][i]; ++seg) {
                    if (s->food_size[session_id] + foods_added_size >= game_max_food) break;
                    const vector2d pos = s->segments[session_id][i][seg];
                    if (rand01(hash_u32(static_cast<unsigned>(tick + seg + i * 17))) < seg_to_food_prob) {
                        foods_added[foods_added_size++] = {
                            .pos = pos,
                            .width = seg_food_min_width +
                                rand01(hash_u32(static_cast<unsigned>(tick + seg + i * 33))) *
                                (seg_food_max_width - seg_food_min_width)
                        };
                    }
                }
                s->length[session_id][i] = 0;
                s->frac_length[session_id][i] = 0;
            }

            bool food_removed[game_max_food]{};
            for (id_t i = 0; i < s->players[session_id]; ++i) {
                if (!alive(s, session_id, i)) continue;
                scalar_t new_length = s->frac_length[session_id][i];
                const vector2d head = s->segments[session_id][i][0];
                for (size_t f = 0; f < s->food_size[session_id]; ++f) {
                    if (food_removed[f]) continue;
                    const scalar_t req = s->width[session_id][i] + s->foods[session_id][f].width - game_collision_eps;
                    if ((s->foods[session_id][f].pos - head).norm_sq() >= req * req) continue;
                    s->score[session_id][i] += static_cast<score_t>(s->foods[session_id][f].width);
                    new_length = stdc::min<scalar_t>(snake_max_length,
                        s->frac_length[session_id][i] + s->foods[session_id][f].width * food_width_to_seg);
                    food_removed[f] = true;
                    foods_removed[foods_removed_size++] = s->foods[session_id][f].pos;
                }
                add_segments(s, session_id, i, new_length);
            }

            {
                size_t dst = 0;
                for (size_t i = 0; i < s->food_size[session_id]; ++i) {
                    if (!food_removed[i]) {
                        s->foods[session_id][dst++] = s->foods[session_id][i];
                    }
                }
                s->food_size[session_id] = dst;
            }
            for (size_t i = 0; i < foods_added_size && s->food_size[session_id] < game_max_food; ++i) {
                s->foods[session_id][s->food_size[session_id]++] = foods_added[i];
            }

            const size_t can_add = game_max_food - s->food_size[session_id];
            const size_t random_add = stdc::min<size_t>(can_add,
                static_cast<size_t>(food_per_player_tick * s->players[session_id] + rand01(hash_u32(tick + session_id * 97))));
            for (size_t i = 0; i < random_add; ++i) {
                const food f{
                    .pos = {
                        rand01(hash_u32(static_cast<unsigned>(tick * 131 + i * 7 + 1))) * s->game_width[session_id],
                        rand01(hash_u32(static_cast<unsigned>(tick * 131 + i * 7 + 2))) * s->game_height[session_id]
                    },
                    .width = gen_food_min_width + rand01(hash_u32(static_cast<unsigned>(tick * 131 + i * 7 + 3))) *
                        (gen_food_max_width - gen_food_min_width)
                };
                s->foods[session_id][s->food_size[session_id]++] = f;
                foods_added[foods_added_size++] = f;
            }

            out->delta_size = store_delta(out->delta_text, s, session_id, foods_added, foods_added_size, foods_removed, foods_removed_size);
            bool need_snapshot = false;
            for (id_t i = 0; i < s->human_players[session_id]; ++i) {
                need_snapshot |= out->snapshot_requested[i];
            }
            if (need_snapshot) {
                out->snapshot_size = store_snapshot(out->snapshot_text, s, session_id);
            }

            bool any_alive = false;
            for (id_t i = 0; i < s->players[session_id]; ++i) {
                any_alive |= alive(s, session_id, i);
            }
            if (tick + 1 > s->max_tick[session_id] || !any_alive) {
                out->terminate = true;
                out->termination_size = store_termination(out->termination_text, s, session_id);
                s->active[session_id] = false;
                return;
            }
            s->tick[session_id] = tick + 1;
        }
    }

    void launch_add_session(game::impl& impl_, const add_session_req& req) noexcept {
        cudaMemcpyAsync(impl_.device_add_req, &req, sizeof(req), cudaMemcpyHostToDevice, impl_.cuda_streams[1]);
        add_session_kernel<<<1, 1, 0, impl_.cuda_streams[1]>>>(impl_.sessions.sessions, impl_.device_add_req);
        cudaEventRecord(impl_.cuda_events[0], impl_.cuda_streams[1]);
    }

    void launch_tick(game::impl& impl_, id_t session_id) noexcept {
        cudaStreamWaitEvent(impl_.cuda_streams[0], impl_.cuda_events[0]);
        tick_kernel<<<1, 1, 0, impl_.cuda_streams[0]>>>(
            impl_.sessions.sessions,
            impl_.device_inbox,
            impl_.device_tick_result,
            session_id);
    }
}

