#include "game_kernels.cuh"
#include <crypt/core.hpp>
#include <utils.hpp>
#include <cuda_runtime.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace {
    constexpr snakeio::size_t kClientsSize =
        static_cast<snakeio::size_t>(snakeio::game_max_sessions) * snakeio::game_max_players;
    constexpr unsigned kSendDescCapacity = 4096;
    constexpr snakeio::size_t kPacketRingCapacity = 32u * 1024u * 1024u;
    constexpr snakeio::size_t kPacketAadSize = 16;
    constexpr snakeio::size_t kPacketHeaderSize = 32;
    constexpr snakeio::size_t kIngressPacketCapacity = snakeio::in_packet_max_text_size + kPacketHeaderSize;
    using snakeio::gpu::client_index;
    using snakeio::gpu::snake_segment_index;

    __host__ __device__ constexpr snakeio::size_t align16(snakeio::size_t n) noexcept {
        return snakeio::align(n);
    }

    __host__ __device__ bool snake_alive(const snakeio::gpu::session_state& s, snakeio::id_t pid) noexcept {
        return s.snake_statuses[pid].status == snakeio::snake_status_t::alive;
    }
    __device__ snakeio::size_t snake_len(const snakeio::gpu::session_state& s, snakeio::id_t pid) noexcept {
        return static_cast<snakeio::size_t>(s.snake_frac_lengths[pid]);
    }

    template <typename S>
    __host__ __device__ auto& snake_seg(S& s, snakeio::id_t pid, snakeio::size_t seg) noexcept {
        return s.snake_segments[snake_segment_index(pid, seg)];
    }

    __device__ std::uint_least32_t mix32(std::uint_least32_t x) noexcept {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }
    __device__ snakeio::scalar_t rand01(std::uint_least32_t x) noexcept {
        return static_cast<snakeio::scalar_t>(mix32(x)) / static_cast<snakeio::scalar_t>(0xffffffffU);
    }
    __device__ snakeio::scalar_t rand_range(std::uint_least32_t x, snakeio::scalar_t lo, snakeio::scalar_t hi) noexcept {
        return lo + (hi - lo) * rand01(x);
    }

    __device__ bool safe_tag_equal(const std::byte* a, const std::byte* b) noexcept {
        volatile std::byte out{};
        for (int i = 0; i < 16; ++i) {
            out = out | (a[i] ^ b[i]);
        }
        return out == std::byte(0);
    }

    __device__ bool verify_and_decrypt(const snakeio::key_t& key,
        std::byte* packet, snakeio::size_t bytes_size) noexcept {
        if (bytes_size <= kPacketHeaderSize) return false;
        if (bytes_size % snakeio::data_packet_align != 0) return false;
        const auto nonce = std::span<const std::byte, 12>{packet + 4, 12};
        const snakeio::key_t otk = snakeio::crypt::poly1305_key_gen(key, nonce);
        std::byte expected[16];
        for (int i = 0; i < 16; ++i) expected[i] = packet[bytes_size - 16 + i];
        snakeio::store_64(std::span<std::byte, 8>(packet + bytes_size - 16, 8), kPacketAadSize);
        snakeio::store_64(std::span<std::byte, 8>(packet + bytes_size - 8, 8), bytes_size - kPacketHeaderSize);
        std::byte computed[16];
        snakeio::crypt::poly1305_mac(
            std::span<std::byte, 16>{computed, 16},
            std::span<const std::byte>{packet, bytes_size}, otk);
        if (!safe_tag_equal(computed, expected)) return false;
        snakeio::crypt::chacha20_encrypt(key, 1, nonce,
            std::span<std::byte>{packet + kPacketAadSize, bytes_size - kPacketHeaderSize});
        return true;
    }

    __device__ void encrypt_packet(const snakeio::key_t& key, std::byte* packet, snakeio::size_t bytes_size) noexcept {
        const auto nonce = std::span<const std::byte, 12>{packet + 4, 12};
        const snakeio::key_t otk = snakeio::crypt::poly1305_key_gen(key, nonce);
        snakeio::crypt::chacha20_encrypt(key, 1, nonce,
            std::span<std::byte>{packet + kPacketAadSize, bytes_size - kPacketHeaderSize});
        snakeio::store_64(std::span<std::byte, 8>(packet + bytes_size - 16, 8), kPacketAadSize);
        snakeio::store_64(std::span<std::byte, 8>(packet + bytes_size - 8, 8), bytes_size - kPacketHeaderSize);
        snakeio::crypt::poly1305_mac(
            std::span<std::byte, 16>{packet + bytes_size - 16, 16},
            std::span<const std::byte>{packet, bytes_size}, otk);
    }

    __device__ void sync_dims(snakeio::gpu::session_state& s, snakeio::id_t pid) noexcept {
        const auto progress =
            (s.snake_frac_lengths[pid] - snakeio::snake_init_length)
            / (snakeio::snake_max_length - snakeio::snake_init_length);
        const snakeio::scalar_t scaled = 1 - (1 - progress) * (1 - progress);
        s.snake_speeds[pid] = snakeio::snake_init_speed
            + (snakeio::snake_min_speed - snakeio::snake_init_speed) * scaled;
        if (s.snake_boosts[pid]) s.snake_speeds[pid] *= snakeio::snake_boost_speed_factor;
        s.snake_widths[pid] = snakeio::snake_init_width
            + (snakeio::snake_max_width - snakeio::snake_init_width) * scaled;
    }

    __device__ void add_segments(snakeio::gpu::session_state& s,
        snakeio::id_t pid, snakeio::scalar_t new_length) noexcept {
        const snakeio::size_t current = snake_len(s, pid);
        const snakeio::size_t target = static_cast<snakeio::size_t>(new_length);
        if (current < 2 || target <= current) {
            s.snake_frac_lengths[pid] = new_length;
            return;
        }
        const snakeio::vector2d tail = snake_seg(s, pid, current - 1);
        const snakeio::vector2d dir = tail - snake_seg(s, pid, current - 2);
        for (snakeio::size_t i = current; i < target; ++i) {
            snake_seg(s, pid, i) = tail + dir * static_cast<snakeio::scalar_t>(i - current);
        }
        s.snake_frac_lengths[pid] = new_length;
    }

    __global__ void k_add_session(snakeio::gpu::device_state st, snakeio::id_t sid,
        snakeio::id_t human, snakeio::id_t ai, snakeio::tick_t max_tick, const snakeio::key_t* keys) {
        auto& s = st.sessions[sid];
        const snakeio::id_t players = human + ai;
        if (threadIdx.x == 0) {
            s.active = true;
            s.players = players;
            s.human_players = human;
            s.tick = 0;
            s.max_tick = max_tick;
            s.width = snakeio::game_width_psqp * sqrtf(static_cast<snakeio::scalar_t>(players));
            s.height = snakeio::game_height_psqp * sqrtf(static_cast<snakeio::scalar_t>(players));
            s.food_size = snakeio::game_init_food_pp * players;
            s.delta.foods_added_size = 0;
            s.delta.foods_removed_size = 0;
        }
        __syncthreads();
        if (threadIdx.x < players) {
            const snakeio::id_t i = threadIdx.x;
            s.snake_speeds[i] = snakeio::snake_init_speed;
            s.snake_angles[i] = rand_range(0x101u + sid * 17u + i * 13u,
                -snakeio::scalar_t(M_PI), snakeio::scalar_t(M_PI));
            s.snake_widths[i] = snakeio::snake_init_width;
            s.snake_frac_lengths[i] = snakeio::snake_init_length;
            s.snake_scores[i] = 0;
            s.snake_boosts[i] = 0;
            s.snake_statuses[i] = {snakeio::snake_status_t::alive, 0};
            s.snake_humans[i] = i < human;
            snake_seg(s, i, 0) = {
                rand_range(0x201u + sid * 19u + i * 7u, 0.0f, s.width),
                rand_range(0x301u + sid * 23u + i * 11u, 0.0f, s.height)
            };
            snake_seg(s, i, 1) = snake_seg(s, i, 0) -
                snakeio::vector2d{cosf(s.snake_angles[i]), sinf(s.snake_angles[i])} * snakeio::snake_init_speed;
            for (snakeio::size_t j = 2; j < snakeio::snake_init_length; ++j) {
                snake_seg(s, i, j) = snake_seg(s, i, 1)
                    + (snake_seg(s, i, 1)
                    - snake_seg(s, i, 0)) * static_cast<snakeio::scalar_t>(j - 1);
            }
            s.in_packet_ticks[i] = static_cast<snakeio::tick_t>(-1);
            s.in_packet_snapshot_requested[i] = false;
            s.in_packet_boost[i] = false;
            s.in_packet_angle[i] = NAN;
            const auto cidx = client_index(sid, i);
            st.client_ticks[cidx] = static_cast<snakeio::tick_t>(-1);
            st.client_last_snapshot_requested[cidx] = false;
            st.client_last_boost[cidx] = false;
            st.client_last_angle[cidx] = NAN;
            auto& c = st.clients[cidx];
            if (i < human) c.key = keys[i];
        }
        if (threadIdx.x < s.food_size && threadIdx.x < snakeio::game_max_food) {
            const snakeio::size_t i = threadIdx.x;
            s.food_poss[i] = {
                rand_range(0x401u + sid * 31u + i * 5u, 0.0f, s.width),
                rand_range(0x501u + sid * 37u + i * 3u, 0.0f, s.height)
            };
                s.food_widths[i] = rand_range(
                    0x601u + sid * 41u + i * 17u,
                    snakeio::gen_food_min_width, snakeio::gen_food_max_width);
        }
    }

    __global__ void k_ingest(snakeio::gpu::device_state st) {
        if (threadIdx.x || blockIdx.x) return;
        *st.ingress_ok = false;
        std::byte* p = st.ingress_packet;
        const snakeio::size_t n = *st.ingress_packet_size;
        if (n <= kPacketHeaderSize) return;
        const snakeio::id_t sid = snakeio::load_32(std::span<const std::byte, 4>(p + 0, 4));
        const snakeio::id_t pid = snakeio::load_32(std::span<const std::byte, 4>(p + 4, 4));
        if (sid >= snakeio::game_max_sessions) return;
        auto& s = st.sessions[sid];
        if (!s.active || pid >= s.players) return;
        const auto cidx = client_index(sid, pid);
        auto& c = st.clients[cidx];
        if (st.client_ticks[cidx] == s.tick) return;
        if (!verify_and_decrypt(c.key, p, n)) return;
        const std::byte* text = p + kPacketAadSize;
        st.client_last_snapshot_requested[cidx] = static_cast<bool>(text[0]);
        st.client_last_boost[cidx] = static_cast<bool>(text[1]);
        st.client_last_angle[cidx] = std::bit_cast<snakeio::scalar_t>(
            snakeio::load_32(std::span<const std::byte, 4>(text + 4, 4)));
        st.client_ticks[cidx] = s.tick;
        *st.ingress_ok = true;
        *st.ingress_session_id = sid;
        *st.ingress_player_id = pid;
    }

    __global__ void k_prepare_inputs(snakeio::gpu::device_state st, snakeio::id_t sid) {
        const snakeio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
        auto& s = st.sessions[sid];
        if (i >= s.players) return;
        if (i < s.human_players) {
            const auto cidx = client_index(sid, i);
            s.in_packet_ticks[i] = st.client_ticks[cidx];
            if (st.client_ticks[cidx] == s.tick) {
                s.in_packet_snapshot_requested[i] = st.client_last_snapshot_requested[cidx];
                s.in_packet_boost[i] = st.client_last_boost[cidx];
                s.in_packet_angle[i] = st.client_last_angle[cidx];
            }
            return;
        }
        if (!snake_alive(s, i)) return;
        s.in_packet_ticks[i] = s.tick;
        s.in_packet_snapshot_requested[i] = false;
        s.in_packet_boost[i] = false;
        s.in_packet_angle[i] = s.snake_angles[i];
    }

    __global__ void k_apply_inputs(snakeio::gpu::device_state st, snakeio::id_t sid) {
        const snakeio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
        auto& s = st.sessions[sid];
        if (i >= s.players) return;
        if (!snake_alive(s, i) || s.in_packet_ticks[i] != s.tick) return;
        if (isfinite(s.in_packet_angle[i])) {
            const snakeio::scalar_t diff = std::remainder(
                s.in_packet_angle[i] - s.snake_angles[i], static_cast<snakeio::scalar_t>(2 * M_PI));
            s.snake_angles[i] += fmaxf(
                -snakeio::snake_max_turn_per_tick,
                fminf(snakeio::snake_max_turn_per_tick, diff));
        }
        if (s.in_packet_boost[i] && s.snake_frac_lengths[i] > snakeio::snake_init_length) {
            const unsigned nb = static_cast<unsigned>(s.snake_boosts[i]) + snakeio::snake_seg_to_boost_ticks;
            s.snake_boosts[i] = static_cast<snakeio::boost_t>(nb > 255 ? 255 : nb);
            s.snake_frac_lengths[i] -= 1;
        }
        sync_dims(s, i);
        if (s.snake_boosts[i]) --s.snake_boosts[i];
    }

    __global__ void k_move(snakeio::gpu::device_state st, snakeio::id_t sid) {
        const snakeio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
        auto& s = st.sessions[sid];
        if (i >= s.players) return;
        if (!snake_alive(s, i)) return;
        const snakeio::size_t len = snake_len(s, i);
        for (snakeio::size_t j = len - 1; j > 0; --j) snake_seg(s, i, j) = snake_seg(s, i, j - 1);
        snake_seg(s, i, 0) += {
            cosf(s.snake_angles[i]) * s.snake_speeds[i],
            sinf(s.snake_angles[i]) * s.snake_speeds[i]
        };
    }

    __global__ void k_collide_food(snakeio::gpu::device_state st, snakeio::id_t sid) {
        if (threadIdx.x || blockIdx.x) return;
        auto& s = st.sessions[sid];
        s.delta.foods_added_size = 0;
        s.delta.foods_removed_size = 0;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            s.kill_flags[i] = 0;
            s.kill_reasons[i] = {snakeio::snake_status_t::alive, 0};
        }

        for (snakeio::id_t i = 0; i < s.players; ++i) {
            if (!snake_alive(s, i)) continue;
            const auto head = snake_seg(s, i, 0);
            const snakeio::scalar_t width_i = s.snake_widths[i];
            if (head[0] < width_i - snakeio::game_collision_eps
                || head[0] > s.width - width_i + snakeio::game_collision_eps
                || head[1] < width_i - snakeio::game_collision_eps
                || head[1] > s.height - width_i + snakeio::game_collision_eps) {
                s.kill_flags[i] = 1;
                s.kill_reasons[i] = {snakeio::snake_status_t::killed_by_wall, 0};
                continue;
                }
            for (snakeio::id_t j = 0; j < s.players; ++j) {
                if (i == j || !snake_alive(s, j)) continue;
                const snakeio::scalar_t req = width_i + s.snake_widths[j] - snakeio::game_collision_eps;
                const snakeio::scalar_t req_sq = req * req;
                for (snakeio::size_t seg = 0; seg < snake_len(s, j); ++seg) {
                    if ((snake_seg(s, j, seg) - head).norm_sq() >= req_sq) continue;
                    s.kill_flags[i] = 1;
                    s.kill_reasons[i] = {snakeio::snake_status_t::killed_by_snake, static_cast<unsigned char>(j)};
                    goto snake_done;
                }
            }
            snake_done:;
        }

        for (snakeio::id_t i = 0; i < s.players; ++i) {
            if (!s.kill_flags[i]) continue;
            s.snake_statuses[i] = s.kill_reasons[i];
            for (snakeio::size_t seg = 0; seg < snake_len(s, i); ++seg) {
                if (s.food_size + s.delta.foods_added_size >= snakeio::game_max_food) break;
                if (rand01(0x701u + sid * 43u + s.tick * 13u + i * 7u + seg) > snakeio::seg_to_food_prob) continue;
                const auto added_idx = s.delta.foods_added_size++;
                s.delta.foods_added_poss[added_idx] = snake_seg(s, i, seg);
                s.delta.foods_added_widths[added_idx] = rand_range(0x801u + sid * 11u + i * 19u + seg,
                    snakeio::seg_food_min_width, snakeio::seg_food_max_width);
            }
            s.snake_frac_lengths[i] = 0;
        }

        for (snakeio::size_t i = 0; i < s.food_size; ++i) s.food_removed_flags[i] = 0;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            if (!snake_alive(s, i)) continue;
            snakeio::scalar_t new_len = s.snake_frac_lengths[i];
            const snakeio::scalar_t width_i = s.snake_widths[i];
            for (snakeio::size_t j = 0; j < s.food_size; ++j) {
                if (s.food_removed_flags[j]) continue;
                const auto fpos = s.food_poss[j];
                const snakeio::scalar_t fwidth = s.food_widths[j];
                const snakeio::scalar_t req = width_i + fwidth - snakeio::game_collision_eps;
                if ((fpos - snake_seg(s, i, 0)).norm_sq() >= req * req) continue;
                s.snake_scores[i] += static_cast<snakeio::score_t>(fwidth);
                new_len = fminf(
                    static_cast<snakeio::scalar_t>(snakeio::snake_max_length),
                    s.snake_frac_lengths[i] + fwidth * snakeio::food_width_to_seg);
                s.food_removed_flags[j] = 1;
                const auto removed_idx = s.delta.foods_removed_size++;
                s.delta.foods_removed_xs[removed_idx] = fpos[0];
                s.delta.foods_removed_ys[removed_idx] = fpos[1];
            }
            add_segments(s, i, new_len);
        }

        snakeio::size_t write = 0;
        for (snakeio::size_t i = 0; i < s.food_size; ++i) {
            if (s.food_removed_flags[i]) continue;
            s.food_poss[write] = s.food_poss[i];
            s.food_widths[write] = s.food_widths[i];
            ++write;
        }
        s.food_size = write;
        for (snakeio::size_t i = 0; i < s.delta.foods_added_size; ++i) {
            if (s.food_size >= snakeio::game_max_food) break;
            s.food_poss[s.food_size] = s.delta.foods_added_poss[i];
            s.food_widths[s.food_size] = s.delta.foods_added_widths[i];
            ++s.food_size;
        }
    }

    __device__ snakeio::size_t store_snake_basic(std::byte* out,
        const snakeio::gpu::session_state& s, snakeio::id_t pid) {
        snakeio::store_32(
            std::span<std::byte, 4>(out + 0, 4),
            std::bit_cast<std::uint_least32_t>(s.snake_speeds[pid]));
        snakeio::store_32(
            std::span<std::byte, 4>(out + 4, 4),
            std::bit_cast<std::uint_least32_t>(s.snake_angles[pid]));
        snakeio::store_32(
            std::span<std::byte, 4>(out + 8, 4),
            std::bit_cast<std::uint_least32_t>(s.snake_widths[pid]));
        snakeio::store_32(
            std::span<std::byte, 4>(out + 12, 4),
            static_cast<std::uint_least32_t>(snake_len(s, pid)));
        snakeio::store_32(std::span<std::byte, 4>(out + 16, 4), s.snake_scores[pid]);
        out[20] = static_cast<std::byte>(s.snake_boosts[pid]);
        out[21] = static_cast<std::byte>(s.snake_statuses[pid].status);
        out[22] = static_cast<std::byte>(s.snake_statuses[pid].data);
        out[23] = static_cast<std::byte>(s.snake_humans[pid]);
        return 24;
    }

    __global__ void k_serialize_delta(snakeio::gpu::device_state st, snakeio::id_t sid) {
        if (threadIdx.x || blockIdx.x) return;
        auto& s = st.sessions[sid];
        std::byte* out = st.plain_delta;
        snakeio::size_t it = 0;
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), 0); it += 4;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            it += store_snake_basic(out + it, s, i);
        }
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), s.delta.foods_added_size); it += 4;
        for (snakeio::size_t i = 0; i < s.delta.foods_added_size; ++i) {
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 0, 4),
                std::bit_cast<std::uint_least32_t>(s.delta.foods_added_poss[i][0]));
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 4, 4),
                std::bit_cast<std::uint_least32_t>(s.delta.foods_added_poss[i][1]));
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 8, 4),
                std::bit_cast<std::uint_least32_t>(s.delta.foods_added_widths[i]));
            it += 12;
        }
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), s.delta.foods_removed_size); it += 4;
        for (snakeio::size_t i = 0; i < s.delta.foods_removed_size; ++i) {
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 0, 4),
                std::bit_cast<std::uint_least32_t>(s.delta.foods_removed_xs[i]));
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 4, 4),
                std::bit_cast<std::uint_least32_t>(s.delta.foods_removed_ys[i]));
            it += 8;
        }
        *st.plain_delta_size = align16(it);
        for (snakeio::size_t i = it; i < *st.plain_delta_size; ++i) out[i] = std::byte(0);
    }

    __global__ void k_serialize_snapshot(snakeio::gpu::device_state st, snakeio::id_t sid) {
        if (threadIdx.x || blockIdx.x) return;
        auto& s = st.sessions[sid];
        std::byte* out = st.plain_snapshot;
        snakeio::size_t it = 0;
        snakeio::store_32(std::span<std::byte, 4>(out + 0, 4), 1);
        snakeio::store_32(std::span<std::byte, 4>(out + 4, 4), std::bit_cast<std::uint_least32_t>(s.width));
        snakeio::store_32(
            std::span<std::byte, 4>(out + 8, 4),
            std::bit_cast<std::uint_least32_t>(s.height));
        snakeio::store_32(std::span<std::byte, 4>(out + 12, 4), s.max_tick);
        snakeio::store_32(std::span<std::byte, 4>(out + 16, 4), s.players);
        it = 20;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            it += store_snake_basic(out + it, s, i);
            for (snakeio::size_t seg = 0; seg < snake_len(s, i); ++seg) {
                snakeio::store_32(
                    std::span<std::byte, 4>(out + it + 0, 4),
                    std::bit_cast<std::uint_least32_t>(snake_seg(s, i, seg)[0]));
                snakeio::store_32(
                    std::span<std::byte, 4>(out + it + 4, 4),
                    std::bit_cast<std::uint_least32_t>(snake_seg(s, i, seg)[1]));
                it += 8;
            }
        }
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), s.food_size); it += 4;
        for (snakeio::size_t i = 0; i < s.food_size; ++i) {
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 0, 4),
                std::bit_cast<std::uint_least32_t>(s.food_poss[i][0]));
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 4, 4),
                std::bit_cast<std::uint_least32_t>(s.food_poss[i][1]));
            snakeio::store_32(
                std::span<std::byte, 4>(out + it + 8, 4),
                std::bit_cast<std::uint_least32_t>(s.food_widths[i]));
            it += 12;
        }
        *st.plain_snapshot_size = align16(it);
        for (snakeio::size_t i = it; i < *st.plain_snapshot_size; ++i) out[i] = std::byte(0);
    }

    __global__ void k_serialize_lobby(snakeio::gpu::device_state st, snakeio::id_t sid) {
        if (threadIdx.x || blockIdx.x) return;
        auto& s = st.sessions[sid];
        std::byte* out = st.plain_lobby;
        snakeio::size_t it = 0;
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), 2); it += 4;
        for (snakeio::id_t i = 0; i < s.human_players; ++i) {
            out[it++] = static_cast<std::byte>(s.in_packet_ticks[i] == 0);
        }
        *st.plain_lobby_size = align16(it);
        for (snakeio::size_t i = it; i < *st.plain_lobby_size; ++i) out[i] = std::byte(0);
    }

    __global__ void k_serialize_termination(snakeio::gpu::device_state st, snakeio::id_t sid) {
        if (threadIdx.x || blockIdx.x) return;
        auto& s = st.sessions[sid];
        std::byte* out = st.plain_termination;
        snakeio::size_t it = 0;
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), 3);
        snakeio::store_32(std::span<std::byte, 4>(out + it + 4, 4), s.max_tick);
        it += 8;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            it += store_snake_basic(out + it, s, i);
        }
        *st.plain_termination_size = align16(it);
        for (snakeio::size_t i = it; i < *st.plain_termination_size; ++i) out[i] = std::byte(0);
    }

    __global__ void k_emit(snakeio::gpu::device_state st, snakeio::id_t sid,
        const std::byte* payload_a, snakeio::size_t size_a,
        const std::byte* payload_b, snakeio::size_t size_b,
        bool per_player_snapshot, bool connected_only) {
        const snakeio::id_t pid = blockIdx.x * blockDim.x + threadIdx.x;
        auto& s = st.sessions[sid];
        if (pid >= s.human_players) return;
        if (connected_only && s.in_packet_ticks[pid] != 0) return;

        const bool use_b = per_player_snapshot
            && s.in_packet_ticks[pid] == s.tick
            && s.in_packet_snapshot_requested[pid];
        const std::byte* payload = use_b ? payload_b : payload_a;
        const snakeio::size_t payload_size = use_b ? size_b : size_a;
        const unsigned chunks = static_cast<unsigned>(
            (payload_size + snakeio::packet_chunk_size - 1) / snakeio::packet_chunk_size);

        for (unsigned c = 0; c < chunks; ++c) {
            const snakeio::size_t payload_off = static_cast<snakeio::size_t>(c) * snakeio::packet_chunk_size;
            const snakeio::size_t text_size = (payload_off + snakeio::packet_chunk_size < payload_size)
                ? snakeio::packet_chunk_size : (payload_size - payload_off);
            const snakeio::size_t packet_size = kPacketHeaderSize + text_size;
            const unsigned ring_off = atomicAdd(st.packet_ring_head, static_cast<unsigned>(packet_size));
            if (ring_off + packet_size > st.packet_ring_capacity) continue;

            std::byte* out = st.packet_ring + ring_off;
            snakeio::store_32(std::span<std::byte, 4>(out + 0, 4), sid);
            snakeio::store_32(std::span<std::byte, 4>(out + 4, 4), pid);
            out[8] = static_cast<std::byte>(1);
            out[9] = static_cast<std::byte>(chunks);
            out[10] = static_cast<std::byte>(c);
            out[11] = std::byte(0);
            snakeio::store_32(std::span<std::byte, 4>(out + 12, 4), s.tick);
            for (snakeio::size_t i = 0; i < text_size; ++i) out[kPacketAadSize + i] = payload[payload_off + i];
            encrypt_packet(st.clients[client_index(sid, pid)].key, out, packet_size);

            const unsigned desc = atomicAdd(st.send_descs_size, 1u);
            if (desc < st.send_descs_capacity) {
                st.send_descs[desc] = {
                    .session_id = sid,
                    .player_id = pid,
                    .ring_offset = ring_off,
                    .bytes_size = packet_size
                };
            }
        }
    }
}

void snakeio::gpu::init_device_state(device_state& s) noexcept {
    cudaMallocManaged(&s.sessions, sizeof(session_state) * snakeio::game_max_sessions);
    cudaMallocManaged(&s.clients, sizeof(client_state) * kClientsSize);
    cudaMallocManaged(&s.client_ticks, sizeof(snakeio::tick_t) * kClientsSize);
    cudaMallocManaged(&s.client_last_snapshot_requested, sizeof(bool) * kClientsSize);
    cudaMallocManaged(&s.client_last_boost, sizeof(bool) * kClientsSize);
    cudaMallocManaged(&s.client_last_angle, sizeof(snakeio::scalar_t) * kClientsSize);
    cudaMallocManaged(&s.packet_ring, kPacketRingCapacity);
    cudaMallocManaged(&s.packet_ring_head, sizeof(unsigned));
    cudaMallocManaged(&s.send_descs, sizeof(send_desc) * kSendDescCapacity);
    cudaMallocManaged(&s.send_descs_size, sizeof(unsigned));
    cudaMallocManaged(&s.plain_delta, snakeio::delta_packet_max_text_size);
    cudaMallocManaged(&s.plain_snapshot, snakeio::snapshot_packet_max_text_size);
    cudaMallocManaged(&s.plain_lobby, snakeio::lobby_status_max_text_size);
    cudaMallocManaged(&s.plain_termination, snakeio::termination_max_text_size);
    cudaMallocManaged(&s.plain_delta_size, sizeof(snakeio::size_t));
    cudaMallocManaged(&s.plain_snapshot_size, sizeof(snakeio::size_t));
    cudaMallocManaged(&s.plain_lobby_size, sizeof(snakeio::size_t));
    cudaMallocManaged(&s.plain_termination_size, sizeof(snakeio::size_t));
    cudaMallocManaged(&s.report, sizeof(tick_report));
    cudaMallocManaged(&s.ingress_ok, sizeof(bool));
    cudaMallocManaged(&s.ingress_session_id, sizeof(snakeio::id_t));
    cudaMallocManaged(&s.ingress_player_id, sizeof(snakeio::id_t));
    cudaMallocManaged(&s.ingress_packet, kIngressPacketCapacity);
    cudaMallocManaged(&s.ingress_packet_size, sizeof(snakeio::size_t));
    s.packet_ring_capacity = kPacketRingCapacity;
    s.send_descs_capacity = kSendDescCapacity;
    s.ingress_packet_capacity = kIngressPacketCapacity;
    cudaMemset(s.sessions, 0, sizeof(session_state) * snakeio::game_max_sessions);
    cudaMemset(s.clients, 0, sizeof(client_state) * kClientsSize);
    cudaMemset(s.client_ticks, 0, sizeof(snakeio::tick_t) * kClientsSize);
    cudaMemset(s.client_last_snapshot_requested, 0, sizeof(bool) * kClientsSize);
    cudaMemset(s.client_last_boost, 0, sizeof(bool) * kClientsSize);
    cudaMemset(s.client_last_angle, 0, sizeof(snakeio::scalar_t) * kClientsSize);
    cudaDeviceSynchronize();
}

void snakeio::gpu::destroy_device_state(device_state& s) noexcept {
    cudaFree(s.sessions);
    cudaFree(s.clients);
    cudaFree(s.client_ticks);
    cudaFree(s.client_last_snapshot_requested);
    cudaFree(s.client_last_boost);
    cudaFree(s.client_last_angle);
    cudaFree(s.packet_ring);
    cudaFree(s.packet_ring_head);
    cudaFree(s.send_descs);
    cudaFree(s.send_descs_size);
    cudaFree(s.plain_delta);
    cudaFree(s.plain_snapshot);
    cudaFree(s.plain_lobby);
    cudaFree(s.plain_termination);
    cudaFree(s.plain_delta_size);
    cudaFree(s.plain_snapshot_size);
    cudaFree(s.plain_lobby_size);
    cudaFree(s.plain_termination_size);
    cudaFree(s.report);
    cudaFree(s.ingress_ok);
    cudaFree(s.ingress_session_id);
    cudaFree(s.ingress_player_id);
    cudaFree(s.ingress_packet);
    cudaFree(s.ingress_packet_size);
}

void snakeio::gpu::add_session_gpu(device_state& s, snakeio::id_t sid,
    snakeio::id_t human_players, snakeio::id_t ai_players,
    snakeio::tick_t max_tick, const std::byte* keys_bytes) noexcept {
    snakeio::key_t* d_keys;
    cudaMallocManaged(&d_keys, sizeof(snakeio::key_t) * human_players);
    std::memcpy(d_keys, keys_bytes, sizeof(snakeio::key_t) * human_players);
    const unsigned threads = static_cast<unsigned>(
        (human_players + ai_players) < 64 ? 64 : (human_players + ai_players));
    k_add_session<<<1, threads>>>(s, sid, human_players, ai_players, max_tick, d_keys);
    cudaDeviceSynchronize();
    cudaFree(d_keys);
}

void snakeio::gpu::ingest_packet_gpu(device_state& s, const std::byte* packet, snakeio::size_t bytes_size) noexcept {
    if (bytes_size > s.ingress_packet_capacity) {
        *s.ingress_ok = false;
        return;
    }
    std::memcpy(s.ingress_packet, packet, bytes_size);
    *s.ingress_packet_size = bytes_size;
    k_ingest<<<1, 1>>>(s);
    cudaDeviceSynchronize();
}

void snakeio::gpu::tick_session_gpu(device_state& s, snakeio::id_t sid) noexcept {
    auto& ss = s.sessions[sid];
    if (!ss.active) {
        s.report->active = false;
        return;
    }
    *s.packet_ring_head = 0;
    *s.send_descs_size = 0;
    s.report->active = true;
    s.report->ended = false;
    s.report->has_payload = false;
    s.report->send_count = 0;

    const unsigned threads = 128;
    const unsigned blocks = (ss.players + threads - 1) / threads;
    const unsigned human_blocks = (ss.human_players + threads - 1) / threads;

    k_prepare_inputs<<<blocks, threads>>>(s, sid);
    cudaDeviceSynchronize();

    if (ss.tick == 0) {
        bool all_ready = true;
        for (snakeio::id_t i = 0; i < ss.human_players; ++i) {
            all_ready = all_ready && (ss.in_packet_ticks[i] == 0);
        }
        if (!all_ready) {
            k_serialize_lobby<<<1, 1>>>(s, sid);
            cudaDeviceSynchronize();
            k_emit<<<human_blocks, threads>>>(s, sid,
                s.plain_lobby, *s.plain_lobby_size,
                s.plain_lobby, *s.plain_lobby_size,
                false, true);
            cudaDeviceSynchronize();
            s.report->send_count = *s.send_descs_size;
            s.report->has_payload = s.report->send_count > 0;
            return;
        }
        k_serialize_snapshot<<<1, 1>>>(s, sid);
        cudaDeviceSynchronize();
        for (id_t i = 0; i < ss.human_players; ++i) {
            ss.in_packet_snapshot_requested[i] = true;
            ss.in_packet_boost[i] = false;
            ss.in_packet_angle[i] = NAN;
        }
        k_emit<<<human_blocks, threads>>>(s, sid,
            s.plain_snapshot, *s.plain_snapshot_size,
            s.plain_snapshot, *s.plain_snapshot_size,
            false, false);
        cudaDeviceSynchronize();
        ss.tick += 1;
        s.report->send_count = *s.send_descs_size;
        s.report->has_payload = s.report->send_count > 0;
        return;
    }

    k_apply_inputs<<<blocks, threads>>>(s, sid);
    k_move<<<blocks, threads>>>(s, sid);
    k_collide_food<<<1, 1>>>(s, sid);
    k_serialize_delta<<<1, 1>>>(s, sid);
    cudaDeviceSynchronize();

    bool any_snapshot = false;
    for (id_t i = 0; i < ss.human_players; ++i) {
        any_snapshot = any_snapshot || (ss.in_packet_ticks[i] == ss.tick && ss.in_packet_snapshot_requested[i]);
    }
    if (any_snapshot) {
        k_serialize_snapshot<<<1, 1>>>(s, sid);
        cudaDeviceSynchronize();
    }

    k_emit<<<human_blocks, threads>>>(s, sid,
        s.plain_delta, *s.plain_delta_size,
        s.plain_snapshot, *s.plain_snapshot_size,
        true, false);
    cudaDeviceSynchronize();

    bool any_alive = false;
    for (id_t i = 0; i < ss.players; ++i) any_alive = any_alive || snake_alive(ss, i);
    if (ss.tick + 1 > ss.max_tick || !any_alive) {
        k_serialize_termination<<<1, 1>>>(s, sid);
        cudaDeviceSynchronize();
        k_emit<<<human_blocks, threads>>>(s, sid,
            s.plain_termination, *s.plain_termination_size,
            s.plain_termination, *s.plain_termination_size,
            false, false);
        cudaDeviceSynchronize();
        ss.active = false;
        s.report->ended = true;
    } else {
        ss.tick += 1;
    }

    s.report->send_count = *s.send_descs_size;
    s.report->has_payload = s.report->send_count > 0;
}