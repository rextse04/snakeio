#include "game_kernels.cuh"
#include "spatial_set.cuh"
#include <crypt/core.hpp>
#include <utils.hpp>
#include <curand_kernel.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <random>

namespace {
    constexpr unsigned kTickFlagAnyActive = 1u << 0;
    constexpr unsigned kTickFlagAnyGt0 = 1u << 1;
    constexpr unsigned kTickFlagAnyLobbyEmit = 1u << 2;
    constexpr unsigned kTickFlagAnyTick0SnapshotEmit = 1u << 3;
    constexpr unsigned kTickFlagAnyDeltaEmit = 1u << 4;
    constexpr unsigned kTickFlagAnyTermEmit = 1u << 5;

    struct snake_spatial_node {
        snakeio::vector2d pos;
        snakeio::id_t snake_id;
        __host__ __device__ snake_spatial_node& operator=(snakeio::vector2d key) noexcept {
            pos = key;
            return *this;
        }
        __host__ __device__ operator snakeio::vector2d() const noexcept {
            return pos;
        }
    };

    constexpr snakeio::size_t kClientsSize =
        static_cast<snakeio::size_t>(snakeio::game_max_sessions) * snakeio::game_max_players;
    constexpr snakeio::size_t kSnakeSpatialNodesPerSession =
        static_cast<snakeio::size_t>(snakeio::game_max_players) * snakeio::snake_max_length;
    constexpr snakeio::scalar_t kSnakeSpatialCellLength = snakeio::snake_max_width * 2;
    struct food_spatial_node {
        snakeio::vector2d pos;
        snakeio::size_t food_id;
        __host__ __device__ food_spatial_node& operator=(snakeio::vector2d key) noexcept {
            pos = key;
            return *this;
        }
        __host__ __device__ operator snakeio::vector2d() const noexcept {
            return pos;
        }
    };
    constexpr snakeio::size_t kFoodSpatialNodesPerSession = snakeio::game_max_food;
    constexpr snakeio::scalar_t kFoodSpatialCellLength = snakeio::snake_max_width + snakeio::food_max_width;
    constexpr unsigned kSendDescCapacity = 4096;
    constexpr snakeio::size_t kPacketRingCapacity = 32u * 1024u * 1024u;
    constexpr snakeio::size_t kPacketAadSize = 16;
    constexpr snakeio::size_t kPacketHeaderSize = 32;
    constexpr snakeio::size_t kIngressPacketCapacity = snakeio::in_packet_max_text_size + kPacketHeaderSize;
    constexpr snakeio::size_t kRngStatesSize =
        static_cast<snakeio::size_t>(snakeio::game_max_sessions) * snakeio::game_max_players;
    using snake_spatial_set = snakeio::gpu::spatial_set_batch<
        snakeio::game_max_width,
        snakeio::game_max_height,
        kSnakeSpatialCellLength,
        kSnakeSpatialNodesPerSession,
        snakeio::game_max_sessions,
        snake_spatial_node>;
    using food_spatial_set = snakeio::gpu::spatial_set_batch<
        snakeio::game_max_width,
        snakeio::game_max_height,
        kFoodSpatialCellLength,
        kFoodSpatialNodesPerSession,
        snakeio::game_max_sessions,
        food_spatial_node>;

    using snakeio::gpu::client_index;
    using snakeio::gpu::snake_segment_index;

    __host__ snake_spatial_set& snake_set(snakeio::gpu::device_state& s) noexcept {
        return *static_cast<snake_spatial_set*>(s.snake_spatial_set);
    }
    __host__ food_spatial_set& food_set(snakeio::gpu::device_state& s) noexcept {
        return *static_cast<food_spatial_set*>(s.food_spatial_set);
    }
    __host__ __device__ curandStatePhilox4_32_10_t* rng_states(snakeio::gpu::device_state& s) noexcept {
        return static_cast<curandStatePhilox4_32_10_t*>(s.rng_states);
    }

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

    __device__ snakeio::scalar_t rand01(
        std::uint_least32_t x,
        snakeio::gpu::device_state& st,
        snakeio::size_t rng_idx) noexcept {
        auto& state = rng_states(st)[(rng_idx + x) % st.rng_states_size];
        // Vectorized Philox draw; choose a lane to keep callsites unchanged.
        const float4 u4 = curand_uniform4(&state);
        const snakeio::scalar_t u = [&]() {
            switch (x & 3u) {
                case 0u: return static_cast<snakeio::scalar_t>(u4.x);
                case 1u: return static_cast<snakeio::scalar_t>(u4.y);
                case 2u: return static_cast<snakeio::scalar_t>(u4.z);
                default: return static_cast<snakeio::scalar_t>(u4.w);
            }
        }();
        return u;
    }
    __device__ snakeio::scalar_t rand_range(
        std::uint_least32_t x,
        snakeio::scalar_t lo,
        snakeio::scalar_t hi,
        snakeio::gpu::device_state& st,
        snakeio::size_t rng_idx) noexcept {
        return lo + (hi - lo) * rand01(x, st, rng_idx);
    }

    __global__ void k_init_rng_states(snakeio::gpu::device_state st) {
        const snakeio::size_t i = static_cast<snakeio::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
        if (i >= st.rng_states_size) return;
        curand_init(st.rng_seed, i, st.rng_offset, &rng_states(st)[i]);
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
        const snakeio::size_t rng_idx =
            client_index(sid, static_cast<snakeio::id_t>(threadIdx.x % snakeio::game_max_players));
        if (threadIdx.x < players) {
            const snakeio::id_t i = threadIdx.x;
            s.snake_speeds[i] = snakeio::snake_init_speed;
            s.snake_angles[i] = rand_range(0x101u + sid * 17u + i * 13u,
                -snakeio::scalar_t(M_PI), snakeio::scalar_t(M_PI), st, rng_idx);
            s.snake_widths[i] = snakeio::snake_init_width;
            s.snake_frac_lengths[i] = snakeio::snake_init_length;
            s.snake_scores[i] = 0;
            s.snake_boosts[i] = 0;
            s.snake_statuses[i] = {snakeio::snake_status_t::alive, 0};
            s.snake_humans[i] = i < human;
            snake_seg(s, i, 0) = {
                rand_range(0x201u + sid * 19u + i * 7u, 0.0f, s.width, st, rng_idx),
                rand_range(0x301u + sid * 23u + i * 11u, 0.0f, s.height, st, rng_idx)
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
                rand_range(0x401u + sid * 31u + i * 5u, 0.0f, s.width, st, rng_idx),
                rand_range(0x501u + sid * 37u + i * 3u, 0.0f, s.height, st, rng_idx)
            };
                s.food_widths[i] = rand_range(
                    0x601u + sid * 41u + i * 17u,
                    snakeio::gen_food_min_width, snakeio::gen_food_max_width, st, rng_idx);
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


    __global__ void k_prepare_inputs_all(snakeio::gpu::device_state st, const bool* active_mask) {
        const snakeio::id_t sid = blockIdx.y;
        const snakeio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
        if (!active_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active || i >= s.players) return;
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

    __global__ void k_build_active_gt0_masks(
        snakeio::gpu::device_state st,
        bool* active_mask,
        bool* gt0_mask,
        bool* lobby_emit_mask,
        bool* tick0_snapshot_emit_mask,
        bool* delta_emit_mask,
        bool* term_emit_mask,
        bool* tick_inc_mask,
        unsigned* flags) {
        const snakeio::id_t sid = blockIdx.x * blockDim.x + threadIdx.x;
        if (sid >= snakeio::game_max_sessions) return;
        const bool active = st.sessions[sid].active;
        active_mask[sid] = active;
        const bool gt0 = active && st.sessions[sid].tick != 0;
        gt0_mask[sid] = gt0;
        lobby_emit_mask[sid] = false;
        tick0_snapshot_emit_mask[sid] = false;
        delta_emit_mask[sid] = false;
        term_emit_mask[sid] = false;
        tick_inc_mask[sid] = false;
        if (active) atomicOr(flags, kTickFlagAnyActive);
        if (gt0) atomicOr(flags, kTickFlagAnyGt0);
    }

    __global__ void k_plan_emit_and_tick_masks(
        snakeio::gpu::device_state st,
        const bool* active_mask,
        bool* lobby_emit_mask,
        bool* tick0_snapshot_emit_mask,
        bool* delta_emit_mask,
        bool* term_emit_mask,
        bool* tick_inc_mask,
        unsigned* flags) {
        const snakeio::id_t sid = blockIdx.x * blockDim.x + threadIdx.x;
        if (sid >= snakeio::game_max_sessions || !active_mask[sid]) return;
        auto& ss = st.sessions[sid];

        if (ss.tick == 0) {
            bool all_ready = true;
            for (snakeio::id_t i = 0; i < ss.human_players; ++i) {
                all_ready = all_ready && (ss.in_packet_ticks[i] == 0);
            }
            if (!all_ready) {
                lobby_emit_mask[sid] = true;
                atomicOr(flags, kTickFlagAnyLobbyEmit);
                return;
            }
            for (snakeio::id_t i = 0; i < ss.human_players; ++i) {
                ss.in_packet_snapshot_requested[i] = true;
                ss.in_packet_boost[i] = false;
                ss.in_packet_angle[i] = NAN;
            }
            tick0_snapshot_emit_mask[sid] = true;
            tick_inc_mask[sid] = true;
            atomicOr(flags, kTickFlagAnyTick0SnapshotEmit);
            return;
        }

        delta_emit_mask[sid] = true;
        atomicOr(flags, kTickFlagAnyDeltaEmit);
        bool any_alive = false;
        for (snakeio::id_t i = 0; i < ss.players; ++i) any_alive = any_alive || snake_alive(ss, i);
        if (ss.tick + 1 > ss.max_tick || !any_alive) {
            term_emit_mask[sid] = true;
            atomicOr(flags, kTickFlagAnyTermEmit);
        } else {
            tick_inc_mask[sid] = true;
        }
    }

    __global__ void k_apply_tick_decisions(
        snakeio::gpu::device_state st,
        const bool* active_mask,
        const bool* term_emit_mask,
        const bool* tick_inc_mask) {
        const snakeio::id_t sid = blockIdx.x * blockDim.x + threadIdx.x;
        if (sid >= snakeio::game_max_sessions || !active_mask[sid]) return;
        if (term_emit_mask[sid]) {
            st.sessions[sid].active = false;
            return;
        }
        if (tick_inc_mask[sid]) st.sessions[sid].tick += 1;
    }

    __global__ void k_apply_inputs_all(snakeio::gpu::device_state st, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.y;
        const snakeio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
        if (!gt0_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active || i >= s.players) return;
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

    __global__ void k_move_all(snakeio::gpu::device_state st, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.y;
        const snakeio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
        if (!gt0_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active || i >= s.players || !snake_alive(s, i)) return;
        const snakeio::size_t len = snake_len(s, i);
        for (snakeio::size_t j = len - 1; j > 0; --j) snake_seg(s, i, j) = snake_seg(s, i, j - 1);
        snake_seg(s, i, 0) += {
            cosf(s.snake_angles[i]) * s.snake_speeds[i],
            sinf(s.snake_angles[i]) * s.snake_speeds[i]
        };
    }

    __global__ void k_reset_collision_state_all(snakeio::gpu::device_state st, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !gt0_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active) return;
        s.delta.foods_added_size = 0;
        s.delta.foods_removed_size = 0;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            s.kill_flags[i] = 0;
            s.kill_reasons[i] = {snakeio::snake_status_t::alive, 0};
        }
    }

    __global__ void k_begin_spatial_batch_all(snake_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x) return;
        const auto begin = snake_spatial_set::batch_offset(sid);
        set.end_offsets[sid] = gt0_mask[sid] ? begin : begin;
    }

    __global__ void k_fill_spatial_nodes_all(
        snakeio::gpu::device_state st, snake_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.y;
        if (!gt0_mask[sid]) return;
        const snakeio::size_t flat =
            static_cast<snakeio::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
        if (flat >= kSnakeSpatialNodesPerSession) return;
        auto& s = st.sessions[sid];
        if (!s.active) return;
        const snakeio::id_t pid = static_cast<snakeio::id_t>(flat / snakeio::snake_max_length);
        const snakeio::size_t seg = flat % snakeio::snake_max_length;
        if (pid >= s.players || !snake_alive(s, pid) || seg >= snake_len(s, pid)) return;
        const auto idx = static_cast<snakeio::size_t>(atomicAdd(set.end_offsets + sid, 1u));
        set.nodes[idx] = snake_spatial_node{snake_seg(s, pid, seg), pid};
    }

    __global__ void k_finalize_spatial_batch_all(snake_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !gt0_mask[sid]) return;
        const snakeio::size_t begin = snake_spatial_set::batch_offset(sid);
        const snakeio::size_t end = set.end_offsets[sid];
        if (end < begin + snake_spatial_set::max_nodes_size()) {
            snake_spatial_set::set_pos(set.nodes[end], snake_spatial_set::erase_key);
        }
    }

    __global__ void k_mark_wall_and_snake_collisions_all(
        snakeio::gpu::device_state st, snake_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.y;
        const snakeio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
        if (!gt0_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active || i >= s.players || !snake_alive(s, i)) return;

        const auto head = snake_seg(s, i, 0);
        const snakeio::scalar_t width_i = s.snake_widths[i];
        if (head[0] < width_i - snakeio::game_collision_eps
            || head[0] > s.width - width_i + snakeio::game_collision_eps
            || head[1] < width_i - snakeio::game_collision_eps
            || head[1] > s.height - width_i + snakeio::game_collision_eps) {
            s.kill_flags[i] = 1;
            s.kill_reasons[i] = {snakeio::snake_status_t::killed_by_wall, 0};
            return;
        }

        const snakeio::size_t begin = snake_spatial_set::batch_offset(sid);
        const snakeio::size_t end = set.end_offsets[sid];
        const snakeio::scalar_t radius = width_i + snakeio::snake_max_width;
        auto it = make_spatial_set_iterator<snake_spatial_set>(
            set.nodes + begin,
            set.indices + begin,
            set.indices + end,
            bounding_rect<snake_spatial_set>(head, radius));
        while (it != std::default_sentinel) {
            const snake_spatial_node& node = *(it++);
            if (node.snake_id == i || !snake_alive(s, node.snake_id)) continue;
            const snakeio::scalar_t req = width_i + s.snake_widths[node.snake_id] - snakeio::game_collision_eps;
            if ((node.pos - head).norm_sq() >= req * req) continue;
            s.kill_flags[i] = 1;
            s.kill_reasons[i] = {
                snakeio::snake_status_t::killed_by_snake,
                static_cast<unsigned char>(node.snake_id)
            };
            return;
        }
    }

    __global__ void k_apply_kills_and_spawn_food_all(snakeio::gpu::device_state st, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !gt0_mask[sid]) return;
        const snakeio::size_t rng_idx = client_index(sid, 0);
        auto& s = st.sessions[sid];
        if (!s.active) return;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            if (!s.kill_flags[i]) continue;
            s.snake_statuses[i] = s.kill_reasons[i];
            for (snakeio::size_t seg = 0; seg < snake_len(s, i); ++seg) {
                if (s.food_size + s.delta.foods_added_size >= snakeio::game_max_food) break;
                if (rand01(0x701u + sid * 43u + s.tick * 13u + i * 7u + seg, st, rng_idx) > snakeio::seg_to_food_prob) continue;
                const auto added_idx = s.delta.foods_added_size++;
                s.delta.foods_added_poss[added_idx] = snake_seg(s, i, seg);
                s.delta.foods_added_widths[added_idx] = rand_range(0x801u + sid * 11u + i * 19u + seg,
                    snakeio::seg_food_min_width, snakeio::seg_food_max_width, st, rng_idx);
            }
            s.snake_frac_lengths[i] = 0;
        }
    }

    __global__ void k_begin_food_spatial_batch_all(food_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x) return;
        const auto begin = food_spatial_set::batch_offset(sid);
        set.end_offsets[sid] = gt0_mask[sid] ? begin : begin;
    }

    __global__ void k_fill_food_spatial_nodes_all(
        snakeio::gpu::device_state st, food_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.y;
        if (!gt0_mask[sid]) return;
        const snakeio::size_t i =
            static_cast<snakeio::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
        auto& s = st.sessions[sid];
        if (!s.active || i >= s.food_size || i >= snakeio::game_max_food) return;
        const auto idx = static_cast<snakeio::size_t>(atomicAdd(set.end_offsets + sid, 1u));
        set.nodes[idx] = food_spatial_node{s.food_poss[i], i};
    }

    __global__ void k_finalize_food_spatial_batch_all(food_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !gt0_mask[sid]) return;
        const snakeio::size_t begin = food_spatial_set::batch_offset(sid);
        const snakeio::size_t end = set.end_offsets[sid];
        if (end < begin + food_spatial_set::max_nodes_size()) {
            food_spatial_set::set_pos(set.nodes[end], food_spatial_set::erase_key);
        }
    }

    __global__ void k_collide_food_and_compact_all(
        snakeio::gpu::device_state st, food_spatial_set set, const bool* gt0_mask) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !gt0_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active) return;
        for (snakeio::size_t i = 0; i < s.food_size; ++i) s.food_removed_flags[i] = 0;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            if (!snake_alive(s, i)) continue;
            snakeio::scalar_t new_len = s.snake_frac_lengths[i];
            const snakeio::scalar_t width_i = s.snake_widths[i];
            const auto head = snake_seg(s, i, 0);
            const snakeio::size_t begin = food_spatial_set::batch_offset(sid);
            const snakeio::size_t end = set.end_offsets[sid];
            const snakeio::scalar_t radius = width_i + snakeio::food_max_width;
            auto it = make_spatial_set_iterator<food_spatial_set>(
                set.nodes + begin,
                set.indices + begin,
                set.indices + end,
                bounding_rect<food_spatial_set>(head, radius));
            while (it != std::default_sentinel) {
                const food_spatial_node& node = *(it++);
                const snakeio::size_t j = node.food_id;
                if (s.food_removed_flags[j]) continue;
                const auto fpos = node.pos;
                const snakeio::scalar_t fwidth = s.food_widths[j];
                const snakeio::scalar_t req = width_i + fwidth - snakeio::game_collision_eps;
                if ((fpos - head).norm_sq() >= req * req) continue;
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


    __global__ void k_serialize_snapshot_all(
        snakeio::gpu::device_state st, const bool* active_mask,
        std::byte* out_all, snakeio::size_t* out_sizes) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !active_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active) return;
        std::byte* out = out_all + static_cast<snakeio::size_t>(sid) * snakeio::snapshot_packet_max_text_size;
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
        out_sizes[sid] = align16(it);
        for (snakeio::size_t i = it; i < out_sizes[sid]; ++i) out[i] = std::byte(0);
    }


    __global__ void k_serialize_delta_all(
        snakeio::gpu::device_state st, const bool* active_mask,
        std::byte* out_all, snakeio::size_t* out_sizes) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !active_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active || s.tick == 0) return;
        std::byte* out = out_all + static_cast<snakeio::size_t>(sid) * snakeio::delta_packet_max_text_size;
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
        out_sizes[sid] = align16(it);
        for (snakeio::size_t i = it; i < out_sizes[sid]; ++i) out[i] = std::byte(0);
    }

    __global__ void k_serialize_lobby_all(
        snakeio::gpu::device_state st, const bool* active_mask,
        std::byte* out_all, snakeio::size_t* out_sizes) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !active_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active || s.tick != 0) return;
        std::byte* out = out_all + static_cast<snakeio::size_t>(sid) * snakeio::lobby_status_max_text_size;
        snakeio::size_t it = 0;
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), 2); it += 4;
        for (snakeio::id_t i = 0; i < s.human_players; ++i) {
            out[it++] = static_cast<std::byte>(s.in_packet_ticks[i] == 0);
        }
        out_sizes[sid] = align16(it);
        for (snakeio::size_t i = it; i < out_sizes[sid]; ++i) out[i] = std::byte(0);
    }

    __global__ void k_serialize_termination_all(
        snakeio::gpu::device_state st, const bool* active_mask,
        std::byte* out_all, snakeio::size_t* out_sizes) {
        const snakeio::id_t sid = blockIdx.x;
        if (threadIdx.x || !active_mask[sid]) return;
        auto& s = st.sessions[sid];
        if (!s.active || s.tick == 0) return;
        std::byte* out = out_all + static_cast<snakeio::size_t>(sid) * snakeio::termination_max_text_size;
        snakeio::size_t it = 0;
        snakeio::store_32(std::span<std::byte, 4>(out + it, 4), 3);
        snakeio::store_32(std::span<std::byte, 4>(out + it + 4, 4), s.max_tick);
        it += 8;
        for (snakeio::id_t i = 0; i < s.players; ++i) {
            it += store_snake_basic(out + it, s, i);
        }
        out_sizes[sid] = align16(it);
        for (snakeio::size_t i = it; i < out_sizes[sid]; ++i) out[i] = std::byte(0);
    }


    __global__ void k_emit_masked(snakeio::gpu::device_state st, const bool* mask,
        const std::byte* payload_a_all, const snakeio::size_t* size_a_all, snakeio::size_t stride_a,
        const std::byte* payload_b_all, const snakeio::size_t* size_b_all, snakeio::size_t stride_b,
        bool per_player_snapshot, bool connected_only) {
        const snakeio::id_t sid = blockIdx.y;
        if (!mask[sid]) return;
        const snakeio::id_t pid = blockIdx.x * blockDim.x + threadIdx.x;
        auto& s = st.sessions[sid];
        if (!s.active || pid >= s.human_players) return;
        if (connected_only && s.in_packet_ticks[pid] != 0) return;

        const bool use_b = per_player_snapshot
            && s.in_packet_ticks[pid] == s.tick
            && s.in_packet_snapshot_requested[pid];
        const std::byte* payload = use_b
            ? (payload_b_all + static_cast<snakeio::size_t>(sid) * stride_b)
            : (payload_a_all + static_cast<snakeio::size_t>(sid) * stride_a);
        const snakeio::size_t payload_size = use_b ? size_b_all[sid] : size_a_all[sid];
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
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::uint_least64_t> dist;
    s.rng_seed = dist(gen);
    s.rng_offset = dist(gen);
    s.rng_states_size = kRngStatesSize;

    s.snake_spatial_set = new snake_spatial_set;
    s.food_spatial_set = new food_spatial_set;
    cudaMallocManaged(&s.plain_delta_all,
        delta_packet_max_text_size * static_cast<size_t>(game_max_sessions));
    cudaMallocManaged(&s.plain_delta_sizes_all, sizeof(size_t) * game_max_sessions);
    cudaMallocManaged(&s.plain_lobby_all,
        lobby_status_max_text_size * static_cast<size_t>(game_max_sessions));
    cudaMallocManaged(&s.plain_lobby_sizes_all, sizeof(size_t) * game_max_sessions);
    cudaMallocManaged(&s.plain_snapshot_all,
        snapshot_packet_max_text_size * static_cast<size_t>(game_max_sessions));
    cudaMallocManaged(&s.plain_snapshot_sizes_all, sizeof(size_t) * game_max_sessions);
    cudaMallocManaged(&s.plain_termination_all,
        termination_max_text_size * static_cast<size_t>(game_max_sessions));
    cudaMallocManaged(&s.plain_termination_sizes_all, sizeof(size_t) * game_max_sessions);
    cudaMallocManaged(&s.add_session_keys, sizeof(key_t) * game_max_players);
    cudaMallocManaged(&s.sessions, sizeof(session_state) * game_max_sessions);
    cudaMallocManaged(&s.clients, sizeof(client_state) * kClientsSize);
    cudaMallocManaged(&s.client_ticks, sizeof(tick_t) * kClientsSize);
    cudaMallocManaged(&s.client_last_snapshot_requested, sizeof(bool) * kClientsSize);
    cudaMallocManaged(&s.client_last_boost, sizeof(bool) * kClientsSize);
    cudaMallocManaged(&s.client_last_angle, sizeof(scalar_t) * kClientsSize);
    cudaMallocManaged(&s.packet_ring, kPacketRingCapacity);
    cudaMallocManaged(&s.packet_ring_head, sizeof(unsigned));
    cudaMallocManaged(&s.send_descs, sizeof(send_desc) * kSendDescCapacity);
    cudaMallocManaged(&s.send_descs_size, sizeof(unsigned));
    cudaMallocManaged(&s.ingress_ok, sizeof(bool));
    cudaMallocManaged(&s.ingress_session_id, sizeof(id_t));
    cudaMallocManaged(&s.ingress_player_id, sizeof(id_t));
    cudaMallocManaged(&s.ingress_packet, kIngressPacketCapacity);
    cudaMallocManaged(&s.ingress_packet_size, sizeof(size_t));
    cudaMallocManaged(&s.rng_states, sizeof(curandStatePhilox4_32_10_t) * s.rng_states_size);
    s.client_addrs = nullptr;
    cudaMallocManaged(&s.tick_masks, sizeof(bool) * game_max_sessions * 7u);
    cudaMallocManaged(&s.tick_flags, sizeof(unsigned));
    s.tick_active_mask = s.tick_masks + game_max_sessions * 0u;
    s.tick_gt0_mask = s.tick_masks + game_max_sessions * 1u;
    s.tick_lobby_emit_mask = s.tick_masks + game_max_sessions * 2u;
    s.tick_tick0_snapshot_emit_mask = s.tick_masks + game_max_sessions * 3u;
    s.tick_delta_emit_mask = s.tick_masks + game_max_sessions * 4u;
    s.tick_term_emit_mask = s.tick_masks + game_max_sessions * 5u;
    s.tick_inc_mask = s.tick_masks + game_max_sessions * 6u;
    s.packet_ring_capacity = kPacketRingCapacity;
    s.send_descs_capacity = kSendDescCapacity;
    s.ingress_packet_capacity = kIngressPacketCapacity;
    cudaMemset(s.sessions, 0, sizeof(session_state) * game_max_sessions);
    cudaMemset(s.clients, 0, sizeof(client_state) * kClientsSize);
    cudaMemset(s.client_ticks, 0, sizeof(tick_t) * kClientsSize);
    cudaMemset(s.client_last_snapshot_requested, 0, sizeof(bool) * kClientsSize);
    cudaMemset(s.client_last_boost, 0, sizeof(bool) * kClientsSize);
    cudaMemset(s.client_last_angle, 0, sizeof(scalar_t) * kClientsSize);
    cudaMemset(s.tick_masks, 0, sizeof(bool) * game_max_sessions * 7u);
    *s.tick_flags = 0;
    constexpr unsigned rng_threads = 256;
    const unsigned rng_blocks = static_cast<unsigned>((s.rng_states_size + rng_threads - 1) / rng_threads);
    k_init_rng_states<<<rng_blocks, rng_threads>>>(s);
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
    cudaFree(s.ingress_ok);
    cudaFree(s.ingress_session_id);
    cudaFree(s.ingress_player_id);
    cudaFree(s.ingress_packet);
    cudaFree(s.ingress_packet_size);
    cudaFree(s.rng_states);
    cudaFree(s.tick_masks);
    cudaFree(s.tick_flags);
    s.tick_masks = nullptr;
    s.tick_flags = nullptr;
    s.tick_active_mask = nullptr;
    s.tick_gt0_mask = nullptr;
    s.tick_lobby_emit_mask = nullptr;
    s.tick_tick0_snapshot_emit_mask = nullptr;
    s.tick_delta_emit_mask = nullptr;
    s.tick_term_emit_mask = nullptr;
    s.tick_inc_mask = nullptr;
    s.rng_states = nullptr;
    s.rng_states_size = 0;
    snake_set(s).destroy();
    delete static_cast<snake_spatial_set*>(s.snake_spatial_set);
    s.snake_spatial_set = nullptr;
    food_set(s).destroy();
    delete static_cast<food_spatial_set*>(s.food_spatial_set);
    s.food_spatial_set = nullptr;
    cudaFree(s.plain_delta_all);
    cudaFree(s.plain_delta_sizes_all);
    cudaFree(s.plain_lobby_all);
    cudaFree(s.plain_lobby_sizes_all);
    cudaFree(s.plain_snapshot_all);
    cudaFree(s.plain_snapshot_sizes_all);
    cudaFree(s.plain_termination_all);
    cudaFree(s.plain_termination_sizes_all);
    cudaFree(s.add_session_keys);
    s.plain_delta_all = nullptr;
    s.plain_delta_sizes_all = nullptr;
    s.plain_lobby_all = nullptr;
    s.plain_lobby_sizes_all = nullptr;
    s.plain_snapshot_all = nullptr;
    s.plain_snapshot_sizes_all = nullptr;
    s.plain_termination_all = nullptr;
    s.plain_termination_sizes_all = nullptr;
    s.add_session_keys = nullptr;
    s.rng_seed = 0;
    s.rng_offset = 0;
}

void snakeio::gpu::add_session_gpu(device_state& s, id_t sid,
    id_t human_players, id_t ai_players,
    tick_t max_tick, const std::byte* keys_bytes) noexcept {
    std::memcpy(s.add_session_keys, keys_bytes, sizeof(key_t) * human_players);
    const unsigned threads = static_cast<unsigned>(
        (human_players + ai_players) < 64 ? 64 : (human_players + ai_players));
    k_add_session<<<1, threads>>>(s, sid, human_players, ai_players, max_tick, s.add_session_keys);
    cudaDeviceSynchronize();
}

void snakeio::gpu::ingest_packet_gpu(device_state& s, const std::byte* packet, size_t bytes_size) noexcept {
    if (bytes_size > s.ingress_packet_capacity) {
        *s.ingress_ok = false;
        return;
    }
    std::memcpy(s.ingress_packet, packet, bytes_size);
    *s.ingress_packet_size = bytes_size;
    k_ingest<<<1, 1>>>(s);
    cudaDeviceSynchronize();
}

void snakeio::gpu::init_client_addrs_gpu(device_state& s, size_t bytes_size) noexcept {
    cudaMallocManaged(&s.client_addrs, bytes_size);
    cudaMemset(s.client_addrs, 0, bytes_size);
}

void snakeio::gpu::destroy_client_addrs_gpu(device_state& s) noexcept {
    cudaFree(s.client_addrs);
    s.client_addrs = nullptr;
}


void snakeio::gpu::tick_active_sessions_gpu(device_state& s) noexcept {
    *s.packet_ring_head = 0;
    *s.send_descs_size = 0;

    bool* d_active_mask = s.tick_active_mask;
    bool* d_gt0_mask = s.tick_gt0_mask;
    bool* d_lobby_emit_mask = s.tick_lobby_emit_mask;
    bool* d_tick0_snapshot_emit_mask = s.tick_tick0_snapshot_emit_mask;
    bool* d_delta_emit_mask = s.tick_delta_emit_mask;
    bool* d_term_emit_mask = s.tick_term_emit_mask;
    bool* d_tick_inc_mask = s.tick_inc_mask;

    constexpr unsigned sid_threads = 256;
    constexpr unsigned sid_blocks = (game_max_sessions + sid_threads - 1) / sid_threads;
    *s.tick_flags = 0;
    k_build_active_gt0_masks<<<sid_blocks, sid_threads>>>(
        s,
        d_active_mask,
        d_gt0_mask,
        d_lobby_emit_mask,
        d_tick0_snapshot_emit_mask,
        d_delta_emit_mask,
        d_term_emit_mask,
        d_tick_inc_mask,
        s.tick_flags);
    cudaDeviceSynchronize();
    const bool any_active = (*s.tick_flags & kTickFlagAnyActive) != 0;
    const bool any_gt0 = (*s.tick_flags & kTickFlagAnyGt0) != 0;
    if (!any_active) return;

    constexpr unsigned threads = 128;
    constexpr unsigned blocks = (game_max_players + threads - 1) / threads;
    k_prepare_inputs_all<<<dim3(blocks, game_max_sessions), threads>>>(s, d_active_mask);
    cudaDeviceSynchronize();
    k_serialize_lobby_all<<<game_max_sessions, 1>>>(
        s, d_active_mask, s.plain_lobby_all, s.plain_lobby_sizes_all);
    k_serialize_snapshot_all<<<game_max_sessions, 1>>>(
        s, d_active_mask, s.plain_snapshot_all, s.plain_snapshot_sizes_all);
    cudaDeviceSynchronize();

    if (any_gt0) {
        auto& snake_spatial = snake_set(s);
        auto& food_spatial = food_set(s);
        k_apply_inputs_all<<<dim3(blocks, game_max_sessions), threads>>>(s, d_gt0_mask);
        k_move_all<<<dim3(blocks, game_max_sessions), threads>>>(s, d_gt0_mask);
        k_reset_collision_state_all<<<game_max_sessions, 1>>>(s, d_gt0_mask);

        k_begin_spatial_batch_all<<<game_max_sessions, 1>>>(snake_spatial, d_gt0_mask);
        constexpr unsigned snake_node_threads = 256;
        constexpr unsigned snake_node_blocks =
            (kSnakeSpatialNodesPerSession + snake_node_threads - 1) / snake_node_threads;
        k_fill_spatial_nodes_all<<<dim3(snake_node_blocks, game_max_sessions), snake_node_threads>>>(
            s, snake_spatial, d_gt0_mask);
        k_finalize_spatial_batch_all<<<game_max_sessions, 1>>>(snake_spatial, d_gt0_mask);
        cudaDeviceSynchronize();
        snake_spatial.refresh();

        k_mark_wall_and_snake_collisions_all<<<dim3(blocks, game_max_sessions), threads>>>(
            s, snake_spatial, d_gt0_mask);
        k_apply_kills_and_spawn_food_all<<<game_max_sessions, 1>>>(s, d_gt0_mask);

        k_begin_food_spatial_batch_all<<<game_max_sessions, 1>>>(food_spatial, d_gt0_mask);
        constexpr unsigned food_node_threads = 256;
        constexpr unsigned food_node_blocks =
            (game_max_food + food_node_threads - 1) / food_node_threads;
        k_fill_food_spatial_nodes_all<<<dim3(food_node_blocks, game_max_sessions), food_node_threads>>>(
            s, food_spatial, d_gt0_mask);
        k_finalize_food_spatial_batch_all<<<game_max_sessions, 1>>>(food_spatial, d_gt0_mask);
        cudaDeviceSynchronize();
        food_spatial.refresh();

        k_collide_food_and_compact_all<<<game_max_sessions, 1>>>(s, food_spatial, d_gt0_mask);
        cudaDeviceSynchronize();
        k_serialize_delta_all<<<game_max_sessions, 1>>>(
            s, d_gt0_mask, s.plain_delta_all, s.plain_delta_sizes_all);
        k_serialize_termination_all<<<game_max_sessions, 1>>>(
            s, d_gt0_mask, s.plain_termination_all, s.plain_termination_sizes_all);
        cudaDeviceSynchronize();
    }

    *s.tick_flags = 0;
    k_plan_emit_and_tick_masks<<<sid_blocks, sid_threads>>>(
        s,
        d_active_mask,
        d_lobby_emit_mask,
        d_tick0_snapshot_emit_mask,
        d_delta_emit_mask,
        d_term_emit_mask,
        d_tick_inc_mask,
        s.tick_flags);
    cudaDeviceSynchronize();

    const bool any_lobby_emit = (*s.tick_flags & kTickFlagAnyLobbyEmit) != 0;
    const bool any_tick0_snapshot_emit = (*s.tick_flags & kTickFlagAnyTick0SnapshotEmit) != 0;
    const bool any_delta_emit = (*s.tick_flags & kTickFlagAnyDeltaEmit) != 0;
    const bool any_term_emit = (*s.tick_flags & kTickFlagAnyTermEmit) != 0;

    constexpr unsigned human_blocks = (game_max_players + threads - 1) / threads;
    if (any_lobby_emit) {
        k_emit_masked<<<dim3(human_blocks, game_max_sessions), threads>>>(
            s, d_lobby_emit_mask,
            s.plain_lobby_all, s.plain_lobby_sizes_all, lobby_status_max_text_size,
            s.plain_lobby_all, s.plain_lobby_sizes_all, lobby_status_max_text_size,
            false, true);
        cudaDeviceSynchronize();
    }
    if (any_tick0_snapshot_emit) {
        k_emit_masked<<<dim3(human_blocks, game_max_sessions), threads>>>(
            s, d_tick0_snapshot_emit_mask,
            s.plain_snapshot_all, s.plain_snapshot_sizes_all, snapshot_packet_max_text_size,
            s.plain_snapshot_all, s.plain_snapshot_sizes_all, snapshot_packet_max_text_size,
            false, false);
        cudaDeviceSynchronize();
    }
    if (any_delta_emit) {
        k_emit_masked<<<dim3(human_blocks, game_max_sessions), threads>>>(
            s, d_delta_emit_mask,
            s.plain_delta_all, s.plain_delta_sizes_all, delta_packet_max_text_size,
            s.plain_snapshot_all, s.plain_snapshot_sizes_all, snapshot_packet_max_text_size,
            true, false);
        cudaDeviceSynchronize();
    }
    if (any_term_emit) {
        k_emit_masked<<<dim3(human_blocks, game_max_sessions), threads>>>(
            s, d_term_emit_mask,
            s.plain_termination_all, s.plain_termination_sizes_all, termination_max_text_size,
            s.plain_termination_all, s.plain_termination_sizes_all, termination_max_text_size,
            false, false);
        cudaDeviceSynchronize();
    }

    k_apply_tick_decisions<<<sid_blocks, sid_threads>>>(
        s,
        d_active_mask,
        d_term_emit_mask,
        d_tick_inc_mask);
    cudaDeviceSynchronize();
}
