#include "game_kernels.cuh"
#include <crypt/core.hpp>
#include <cuda_runtime.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace sio = snakeio;

namespace {
constexpr sio::size_t kClientsSize = static_cast<sio::size_t>(sio::game_max_sessions) * sio::game_max_players;
constexpr unsigned kSendDescCapacity = 4096;
constexpr sio::size_t kPacketRingCapacity = 32u * 1024u * 1024u;
constexpr sio::size_t kPacketAadSize = 16;
constexpr sio::size_t kPacketHeaderSize = 32;
constexpr sio::size_t kIngressPacketCapacity = sio::in_packet_max_text_size + kPacketHeaderSize;
using sio::gpu::client_index;

__host__ __device__ constexpr std::uint_least32_t load32(const std::byte* p) noexcept {
    return (static_cast<std::uint_least32_t>(p[0]) << 0) |
           (static_cast<std::uint_least32_t>(p[1]) << 8) |
           (static_cast<std::uint_least32_t>(p[2]) << 16) |
           (static_cast<std::uint_least32_t>(p[3]) << 24);
}
__host__ __device__ constexpr void store32(std::byte* p, std::uint_least32_t v) noexcept {
    p[0] = static_cast<std::byte>(v >> 0);
    p[1] = static_cast<std::byte>(v >> 8);
    p[2] = static_cast<std::byte>(v >> 16);
    p[3] = static_cast<std::byte>(v >> 24);
}
__host__ __device__ constexpr std::uint_least64_t load64(const std::byte* p) noexcept {
    return (static_cast<std::uint_least64_t>(p[0]) << 0) |
           (static_cast<std::uint_least64_t>(p[1]) << 8) |
           (static_cast<std::uint_least64_t>(p[2]) << 16) |
           (static_cast<std::uint_least64_t>(p[3]) << 24) |
           (static_cast<std::uint_least64_t>(p[4]) << 32) |
           (static_cast<std::uint_least64_t>(p[5]) << 40) |
           (static_cast<std::uint_least64_t>(p[6]) << 48) |
           (static_cast<std::uint_least64_t>(p[7]) << 56);
}
__host__ __device__ constexpr void store64(std::byte* p, std::uint_least64_t v) noexcept {
    p[0] = static_cast<std::byte>(v >> 0);
    p[1] = static_cast<std::byte>(v >> 8);
    p[2] = static_cast<std::byte>(v >> 16);
    p[3] = static_cast<std::byte>(v >> 24);
    p[4] = static_cast<std::byte>(v >> 32);
    p[5] = static_cast<std::byte>(v >> 40);
    p[6] = static_cast<std::byte>(v >> 48);
    p[7] = static_cast<std::byte>(v >> 56);
}
__host__ __device__ inline float loadf32(const std::byte* p) noexcept {
    union { std::uint_least32_t u; float f; } u{};
    u.u = load32(p);
    return u.f;
}
__host__ __device__ inline void storef32(std::byte* p, float f) noexcept {
    union { std::uint_least32_t u; float f; } u{};
    u.f = f;
    store32(p, u.u);
}
__host__ __device__ constexpr sio::size_t align16(sio::size_t n) noexcept {
    return sio::align(n);
}

__host__ __device__ inline bool snake_alive(const sio::gpu::snake_state& s) noexcept {
    return s.status.status == sio::snake_status_t::alive;
}
__device__ inline sio::size_t snake_len(const sio::gpu::snake_state& s) noexcept {
    return static_cast<sio::size_t>(s.frac_length);
}

__device__ inline std::uint_least32_t mix32(std::uint_least32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
__device__ inline float rand01(std::uint_least32_t x) noexcept {
    return static_cast<float>(mix32(x)) / static_cast<float>(0xffffffffU);
}
__device__ inline float rand_range(std::uint_least32_t x, float lo, float hi) noexcept {
    return lo + (hi - lo) * rand01(x);
}

__device__ bool safe_tag_equal(const std::byte* a, const std::byte* b) noexcept {
    volatile std::byte out{};
    for (int i = 0; i < 16; ++i) {
        out = out | (a[i] ^ b[i]);
    }
    return out == std::byte(0);
}

__device__ bool verify_and_decrypt(const sio::key_t& key, std::byte* packet, sio::size_t bytes_size) noexcept {
    if (bytes_size <= kPacketHeaderSize) return false;
    if (bytes_size % sio::data_packet_align != 0) return false;
    const auto nonce = std::span<const std::byte, 12>{packet + 4, 12};
    const sio::key_t otk = sio::crypt::poly1305_key_gen(key, nonce);
    std::byte expected[16];
    for (int i = 0; i < 16; ++i) expected[i] = packet[bytes_size - 16 + i];
    store64(packet + bytes_size - 16, kPacketAadSize);
    store64(packet + bytes_size - 8, bytes_size - kPacketHeaderSize);
    std::byte computed[16];
    sio::crypt::poly1305_mac(std::span<std::byte, 16>{computed, 16}, std::span<const std::byte>{packet, bytes_size}, otk);
    if (!safe_tag_equal(computed, expected)) return false;
    sio::crypt::chacha20_encrypt(key, 1, nonce,
        std::span<std::byte>{packet + kPacketAadSize, bytes_size - kPacketHeaderSize});
    return true;
}

__device__ void encrypt_packet(const sio::key_t& key, std::byte* packet, sio::size_t bytes_size) noexcept {
    const auto nonce = std::span<const std::byte, 12>{packet + 4, 12};
    const sio::key_t otk = sio::crypt::poly1305_key_gen(key, nonce);
    sio::crypt::chacha20_encrypt(key, 1, nonce,
        std::span<std::byte>{packet + kPacketAadSize, bytes_size - kPacketHeaderSize});
    store64(packet + bytes_size - 16, kPacketAadSize);
    store64(packet + bytes_size - 8, bytes_size - kPacketHeaderSize);
    sio::crypt::poly1305_mac(
        std::span<std::byte, 16>{packet + bytes_size - 16, 16}, std::span<const std::byte>{packet, bytes_size}, otk);
}

__device__ void sync_dims(sio::gpu::snake_state& s) noexcept {
    const auto progress = (s.frac_length - sio::snake_init_length) / (sio::snake_max_length - sio::snake_init_length);
    const sio::scalar_t scaled = 1 - (1 - progress) * (1 - progress);
    s.speed = sio::snake_init_speed + (sio::snake_min_speed - sio::snake_init_speed) * scaled;
    if (s.boost) s.speed *= sio::snake_boost_speed_factor;
    s.width = sio::snake_init_width + (sio::snake_max_width - sio::snake_init_width) * scaled;
}

__device__ void add_segments(sio::gpu::snake_state& s, sio::scalar_t new_length) noexcept {
    const sio::size_t current = snake_len(s);
    const sio::size_t target = static_cast<sio::size_t>(new_length);
    if (current < 2 || target <= current) {
        s.frac_length = new_length;
        return;
    }
    const sio::vector2d tail = s.segments[current - 1];
    const sio::vector2d dir = tail - s.segments[current - 2];
    for (sio::size_t i = current; i < target; ++i) {
        s.segments[i] = tail + dir * static_cast<sio::scalar_t>(i - current);
    }
    s.frac_length = new_length;
}

__global__ void k_add_session(sio::gpu::device_state st, sio::id_t sid,
    sio::id_t human, sio::id_t ai, sio::tick_t max_tick, const sio::key_t* keys) {
    auto& s = st.sessions[sid];
    const sio::id_t players = human + ai;
    if (threadIdx.x == 0) {
        s.active = true;
        s.players = players;
        s.human_players = human;
        s.tick = 0;
        s.max_tick = max_tick;
        s.width = sio::game_width_psqp * sqrtf(static_cast<float>(players));
        s.height = sio::game_height_psqp * sqrtf(static_cast<float>(players));
        s.food_size = sio::game_init_food_pp * players;
        s.delta.foods_added_size = 0;
        s.delta.foods_removed_size = 0;
    }
    __syncthreads();
    if (threadIdx.x < players) {
        const sio::id_t i = threadIdx.x;
        auto& sn = s.snakes[i];
        sn.speed = sio::snake_init_speed;
        sn.angle = rand_range(0x101u + sid * 17u + i * 13u, -static_cast<float>(M_PI), static_cast<float>(M_PI));
        sn.width = sio::snake_init_width;
        sn.frac_length = sio::snake_init_length;
        sn.score = 0;
        sn.boost = 0;
        sn.status = {sio::snake_status_t::alive, 0};
        sn.human = i < human;
        sn.segments[0] = {
            rand_range(0x201u + sid * 19u + i * 7u, 0.0f, s.width),
            rand_range(0x301u + sid * 23u + i * 11u, 0.0f, s.height)
        };
        sn.segments[1] = sn.segments[0] - sio::vector2d{cosf(sn.angle), sinf(sn.angle)} * sio::snake_init_speed;
        for (sio::size_t j = 2; j < sio::snake_init_length; ++j) {
            sn.segments[j] = sn.segments[1] + (sn.segments[1] - sn.segments[0]) * static_cast<float>(j - 1);
        }
        s.in_packets[i].tick = static_cast<sio::tick_t>(-1);
        s.in_packets[i].snapshot_requested = false;
        s.in_packets[i].boost = false;
        s.in_packets[i].angle = NAN;
        auto& c = st.clients[client_index(sid, i)];
        c.tick = static_cast<sio::tick_t>(-1);
        c.last_packet = {.snapshot_requested = false, .boost = false, .angle = NAN};
        if (i < human) c.key = keys[i];
    }
    if (threadIdx.x < s.food_size && threadIdx.x < sio::game_max_food) {
        const sio::size_t i = threadIdx.x;
        s.foods[i] = {
            .pos = {
                rand_range(0x401u + sid * 31u + i * 5u, 0.0f, s.width),
                rand_range(0x501u + sid * 37u + i * 3u, 0.0f, s.height)
            },
            .width = rand_range(0x601u + sid * 41u + i * 17u, sio::gen_food_min_width, sio::gen_food_max_width)
        };
    }
}

__global__ void k_ingest(sio::gpu::device_state st) {
    if (threadIdx.x || blockIdx.x) return;
    *st.ingress_ok = false;
    std::byte* p = st.ingress_packet;
    const sio::size_t n = *st.ingress_packet_size;
    if (n <= kPacketHeaderSize) return;
    const sio::id_t sid = load32(p + 0);
    const sio::id_t pid = load32(p + 4);
    if (sid >= sio::game_max_sessions) return;
    auto& s = st.sessions[sid];
    if (!s.active || pid >= s.players) return;
    auto& c = st.clients[client_index(sid, pid)];
    if (c.tick == s.tick) return;
    if (!verify_and_decrypt(c.key, p, n)) return;
    const std::byte* text = p + kPacketAadSize;
    c.last_packet.snapshot_requested = static_cast<bool>(text[0]);
    c.last_packet.boost = static_cast<bool>(text[1]);
    c.last_packet.angle = loadf32(text + 4);
    c.tick = s.tick;
    *st.ingress_ok = true;
    *st.ingress_session_id = sid;
    *st.ingress_player_id = pid;
}

__global__ void k_prepare_inputs(sio::gpu::device_state st, sio::id_t sid) {
    const sio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
    auto& s = st.sessions[sid];
    if (i >= s.players) return;
    auto& in = s.in_packets[i];
    if (i < s.human_players) {
        const auto& c = st.clients[client_index(sid, i)];
        in.tick = c.tick;
        if (c.tick == s.tick) {
            in.snapshot_requested = c.last_packet.snapshot_requested;
            in.boost = c.last_packet.boost;
            in.angle = c.last_packet.angle;
        }
        return;
    }
    auto& sn = s.snakes[i];
    if (!snake_alive(sn)) return;
    in.tick = s.tick;
    in.snapshot_requested = false;
    in.boost = false;
    in.angle = sn.angle;
}

__global__ void k_apply_inputs(sio::gpu::device_state st, sio::id_t sid) {
    const sio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
    auto& s = st.sessions[sid];
    if (i >= s.players) return;
    auto& sn = s.snakes[i];
    auto& in = s.in_packets[i];
    if (!snake_alive(sn) || in.tick != s.tick) return;
    if (isfinite(in.angle)) {
        const float diff = remainderf(in.angle - sn.angle, static_cast<float>(2 * M_PI));
        sn.angle += fmaxf(-sio::snake_max_turn_per_tick, fminf(sio::snake_max_turn_per_tick, diff));
    }
    if (in.boost && sn.frac_length > sio::snake_init_length) {
        const unsigned nb = static_cast<unsigned>(sn.boost) + sio::snake_seg_to_boost_ticks;
        sn.boost = static_cast<sio::boost_t>(nb > 255 ? 255 : nb);
        sn.frac_length -= 1;
    }
    sync_dims(sn);
    if (sn.boost) --sn.boost;
}

__global__ void k_move(sio::gpu::device_state st, sio::id_t sid) {
    const sio::id_t i = blockIdx.x * blockDim.x + threadIdx.x;
    auto& s = st.sessions[sid];
    if (i >= s.players) return;
    auto& sn = s.snakes[i];
    if (!snake_alive(sn)) return;
    const sio::size_t len = snake_len(sn);
    for (sio::size_t j = len - 1; j > 0; --j) sn.segments[j] = sn.segments[j - 1];
    sn.segments[0] += {cosf(sn.angle) * sn.speed, sinf(sn.angle) * sn.speed};
}

__global__ void k_collide_food(sio::gpu::device_state st, sio::id_t sid) {
    if (threadIdx.x || blockIdx.x) return;
    auto& s = st.sessions[sid];
    s.delta.foods_added_size = 0;
    s.delta.foods_removed_size = 0;
    for (sio::id_t i = 0; i < s.players; ++i) {
        s.kill_flags[i] = 0;
        s.kill_reasons[i] = {sio::snake_status_t::alive, 0};
    }

    for (sio::id_t i = 0; i < s.players; ++i) {
        auto& sn = s.snakes[i];
        if (!snake_alive(sn)) continue;
        const auto head = sn.segments[0];
        if (head[0] < sn.width - sio::game_collision_eps || head[0] > s.width - sn.width + sio::game_collision_eps ||
            head[1] < sn.width - sio::game_collision_eps || head[1] > s.height - sn.width + sio::game_collision_eps) {
            s.kill_flags[i] = 1;
            s.kill_reasons[i] = {sio::snake_status_t::killed_by_wall, 0};
            continue;
        }
        for (sio::id_t j = 0; j < s.players; ++j) {
            if (i == j || !snake_alive(s.snakes[j])) continue;
            const float req = sn.width + s.snakes[j].width - sio::game_collision_eps;
            const float req_sq = req * req;
            for (sio::size_t seg = 0; seg < snake_len(s.snakes[j]); ++seg) {
                if ((s.snakes[j].segments[seg] - head).norm_sq() >= req_sq) continue;
                s.kill_flags[i] = 1;
                s.kill_reasons[i] = {sio::snake_status_t::killed_by_snake, static_cast<unsigned char>(j)};
                goto snake_done;
            }
        }
        snake_done:;
    }

    for (sio::id_t i = 0; i < s.players; ++i) {
        if (!s.kill_flags[i]) continue;
        auto& sn = s.snakes[i];
        sn.status = s.kill_reasons[i];
        for (sio::size_t seg = 0; seg < snake_len(sn); ++seg) {
            if (s.food_size + s.delta.foods_added_size >= sio::game_max_food) break;
            if (rand01(0x701u + sid * 43u + s.tick * 13u + i * 7u + seg) > sio::seg_to_food_prob) continue;
            s.delta.foods_added[s.delta.foods_added_size++] = {
                .pos = sn.segments[seg],
                .width = rand_range(0x801u + sid * 11u + i * 19u + seg,
                    sio::seg_food_min_width, sio::seg_food_max_width)
            };
        }
        sn.frac_length = 0;
    }

    for (sio::size_t i = 0; i < s.food_size; ++i) s.food_removed_flags[i] = 0;
    for (sio::id_t i = 0; i < s.players; ++i) {
        auto& sn = s.snakes[i];
        if (!snake_alive(sn)) continue;
        float new_len = sn.frac_length;
        for (sio::size_t j = 0; j < s.food_size; ++j) {
            if (s.food_removed_flags[j]) continue;
            const auto& f = s.foods[j];
            const float req = sn.width + f.width - sio::game_collision_eps;
            if ((f.pos - sn.segments[0]).norm_sq() >= req * req) continue;
            sn.score += static_cast<sio::score_t>(f.width);
            new_len = fminf(static_cast<float>(sio::snake_max_length), sn.frac_length + f.width * sio::food_width_to_seg);
            s.food_removed_flags[j] = 1;
            s.delta.foods_removed[s.delta.foods_removed_size++] = f.pos;
        }
        add_segments(sn, new_len);
    }

    sio::size_t write = 0;
    for (sio::size_t i = 0; i < s.food_size; ++i) {
        if (s.food_removed_flags[i]) continue;
        s.foods[write++] = s.foods[i];
    }
    s.food_size = write;
    for (sio::size_t i = 0; i < s.delta.foods_added_size; ++i) {
        if (s.food_size >= sio::game_max_food) break;
        s.foods[s.food_size++] = s.delta.foods_added[i];
    }
}

__device__ sio::size_t store_snake_basic(std::byte* out, const sio::gpu::snake_state& sn) {
    storef32(out + 0, sn.speed);
    storef32(out + 4, sn.angle);
    storef32(out + 8, sn.width);
    store32(out + 12, static_cast<std::uint_least32_t>(snake_len(sn)));
    store32(out + 16, sn.score);
    out[20] = static_cast<std::byte>(sn.boost);
    out[21] = static_cast<std::byte>(sn.status.status);
    out[22] = static_cast<std::byte>(sn.status.data);
    out[23] = static_cast<std::byte>(sn.human);
    return 24;
}

__global__ void k_serialize_delta(sio::gpu::device_state st, sio::id_t sid) {
    if (threadIdx.x || blockIdx.x) return;
    auto& s = st.sessions[sid];
    std::byte* out = st.plain_delta;
    sio::size_t it = 0;
    store32(out + it, 0); it += 4;
    for (sio::id_t i = 0; i < s.players; ++i) {
        it += store_snake_basic(out + it, s.snakes[i]);
    }
    store32(out + it, s.delta.foods_added_size); it += 4;
    for (sio::size_t i = 0; i < s.delta.foods_added_size; ++i) {
        storef32(out + it + 0, s.delta.foods_added[i].pos[0]);
        storef32(out + it + 4, s.delta.foods_added[i].pos[1]);
        storef32(out + it + 8, s.delta.foods_added[i].width);
        it += 12;
    }
    store32(out + it, s.delta.foods_removed_size); it += 4;
    for (sio::size_t i = 0; i < s.delta.foods_removed_size; ++i) {
        storef32(out + it + 0, s.delta.foods_removed[i][0]);
        storef32(out + it + 4, s.delta.foods_removed[i][1]);
        it += 8;
    }
    *st.plain_delta_size = align16(it);
    for (sio::size_t i = it; i < *st.plain_delta_size; ++i) out[i] = std::byte(0);
}

__global__ void k_serialize_snapshot(sio::gpu::device_state st, sio::id_t sid) {
    if (threadIdx.x || blockIdx.x) return;
    auto& s = st.sessions[sid];
    std::byte* out = st.plain_snapshot;
    sio::size_t it = 0;
    store32(out + 0, 1);
    storef32(out + 4, s.width);
    storef32(out + 8, s.height);
    store32(out + 12, s.max_tick);
    store32(out + 16, s.players);
    it = 20;
    for (sio::id_t i = 0; i < s.players; ++i) {
        it += store_snake_basic(out + it, s.snakes[i]);
        for (sio::size_t seg = 0; seg < snake_len(s.snakes[i]); ++seg) {
            storef32(out + it + 0, s.snakes[i].segments[seg][0]);
            storef32(out + it + 4, s.snakes[i].segments[seg][1]);
            it += 8;
        }
    }
    store32(out + it, s.food_size); it += 4;
    for (sio::size_t i = 0; i < s.food_size; ++i) {
        storef32(out + it + 0, s.foods[i].pos[0]);
        storef32(out + it + 4, s.foods[i].pos[1]);
        storef32(out + it + 8, s.foods[i].width);
        it += 12;
    }
    *st.plain_snapshot_size = align16(it);
    for (sio::size_t i = it; i < *st.plain_snapshot_size; ++i) out[i] = std::byte(0);
}

__global__ void k_serialize_lobby(sio::gpu::device_state st, sio::id_t sid) {
    if (threadIdx.x || blockIdx.x) return;
    auto& s = st.sessions[sid];
    std::byte* out = st.plain_lobby;
    sio::size_t it = 0;
    store32(out + it, 2); it += 4;
    for (sio::id_t i = 0; i < s.human_players; ++i) {
        out[it++] = static_cast<std::byte>(s.in_packets[i].tick == 0);
    }
    *st.plain_lobby_size = align16(it);
    for (sio::size_t i = it; i < *st.plain_lobby_size; ++i) out[i] = std::byte(0);
}

__global__ void k_serialize_termination(sio::gpu::device_state st, sio::id_t sid) {
    if (threadIdx.x || blockIdx.x) return;
    auto& s = st.sessions[sid];
    std::byte* out = st.plain_termination;
    sio::size_t it = 0;
    store32(out + it, 3);
    store32(out + it + 4, s.max_tick);
    it += 8;
    for (sio::id_t i = 0; i < s.players; ++i) {
        it += store_snake_basic(out + it, s.snakes[i]);
    }
    *st.plain_termination_size = align16(it);
    for (sio::size_t i = it; i < *st.plain_termination_size; ++i) out[i] = std::byte(0);
}

__global__ void k_emit(sio::gpu::device_state st, sio::id_t sid,
    const std::byte* payload_a, sio::size_t size_a,
    const std::byte* payload_b, sio::size_t size_b,
    bool per_player_snapshot, bool connected_only) {
    const sio::id_t pid = blockIdx.x * blockDim.x + threadIdx.x;
    auto& s = st.sessions[sid];
    if (pid >= s.human_players) return;
    if (connected_only && s.in_packets[pid].tick != 0) return;

    const bool use_b = per_player_snapshot && s.in_packets[pid].tick == s.tick && s.in_packets[pid].snapshot_requested;
    const std::byte* payload = use_b ? payload_b : payload_a;
    const sio::size_t payload_size = use_b ? size_b : size_a;
    const unsigned chunks = static_cast<unsigned>((payload_size + sio::packet_chunk_size - 1) / sio::packet_chunk_size);

    for (unsigned c = 0; c < chunks; ++c) {
        const sio::size_t payload_off = static_cast<sio::size_t>(c) * sio::packet_chunk_size;
        const sio::size_t text_size = (payload_off + sio::packet_chunk_size < payload_size)
            ? sio::packet_chunk_size : (payload_size - payload_off);
        const sio::size_t packet_size = kPacketHeaderSize + text_size;
        const unsigned ring_off = atomicAdd(st.packet_ring_head, static_cast<unsigned>(packet_size));
        if (ring_off + packet_size > st.packet_ring_capacity) continue;

        std::byte* out = st.packet_ring + ring_off;
        store32(out + 0, sid);
        store32(out + 4, pid);
        out[8] = static_cast<std::byte>(1);
        out[9] = static_cast<std::byte>(chunks);
        out[10] = static_cast<std::byte>(c);
        out[11] = std::byte(0);
        store32(out + 12, s.tick);
        for (sio::size_t i = 0; i < text_size; ++i) out[kPacketAadSize + i] = payload[payload_off + i];
        encrypt_packet(st.clients[client_index(sid, pid)].key, out, packet_size);

        const unsigned desc = atomicAdd(st.send_descs_size, 1u);
        if (desc < st.send_descs_capacity) {
            st.send_descs[desc] = {.session_id = sid, .player_id = pid, .ring_offset = ring_off, .bytes_size = packet_size};
        }
    }
}
} // namespace

void sio::gpu::init_device_state(device_state& s) noexcept {
    cudaMallocManaged(&s.sessions, sizeof(session_state) * sio::game_max_sessions);
    cudaMallocManaged(&s.clients, sizeof(client_state) * kClientsSize);
    cudaMallocManaged(&s.packet_ring, kPacketRingCapacity);
    cudaMallocManaged(&s.packet_ring_head, sizeof(unsigned));
    cudaMallocManaged(&s.send_descs, sizeof(send_desc) * kSendDescCapacity);
    cudaMallocManaged(&s.send_descs_size, sizeof(unsigned));
    cudaMallocManaged(&s.plain_delta, sio::delta_packet_max_text_size);
    cudaMallocManaged(&s.plain_snapshot, sio::snapshot_packet_max_text_size);
    cudaMallocManaged(&s.plain_lobby, sio::lobby_status_max_text_size);
    cudaMallocManaged(&s.plain_termination, sio::termination_max_text_size);
    cudaMallocManaged(&s.plain_delta_size, sizeof(sio::size_t));
    cudaMallocManaged(&s.plain_snapshot_size, sizeof(sio::size_t));
    cudaMallocManaged(&s.plain_lobby_size, sizeof(sio::size_t));
    cudaMallocManaged(&s.plain_termination_size, sizeof(sio::size_t));
    cudaMallocManaged(&s.report, sizeof(tick_report));
    cudaMallocManaged(&s.ingress_ok, sizeof(bool));
    cudaMallocManaged(&s.ingress_session_id, sizeof(sio::id_t));
    cudaMallocManaged(&s.ingress_player_id, sizeof(sio::id_t));
    cudaMallocManaged(&s.ingress_packet, kIngressPacketCapacity);
    cudaMallocManaged(&s.ingress_packet_size, sizeof(sio::size_t));
    s.packet_ring_capacity = kPacketRingCapacity;
    s.send_descs_capacity = kSendDescCapacity;
    s.ingress_packet_capacity = kIngressPacketCapacity;
    cudaMemset(s.sessions, 0, sizeof(session_state) * sio::game_max_sessions);
    cudaMemset(s.clients, 0, sizeof(client_state) * kClientsSize);
    cudaDeviceSynchronize();
}

void sio::gpu::destroy_device_state(device_state& s) noexcept {
    cudaFree(s.sessions);
    cudaFree(s.clients);
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

void sio::gpu::add_session_gpu(device_state& s, sio::id_t sid,
    sio::id_t human_players, sio::id_t ai_players, sio::tick_t max_tick, const std::byte* keys_bytes) noexcept {
    sio::key_t* d_keys;
    cudaMallocManaged(&d_keys, sizeof(sio::key_t) * human_players);
    std::memcpy(d_keys, keys_bytes, sizeof(sio::key_t) * human_players);
    const unsigned threads = static_cast<unsigned>((human_players + ai_players) < 64 ? 64 : (human_players + ai_players));
    k_add_session<<<1, threads>>>(s, sid, human_players, ai_players, max_tick, d_keys);
    cudaDeviceSynchronize();
    cudaFree(d_keys);
}

void sio::gpu::ingest_packet_gpu(device_state& s, const std::byte* packet, sio::size_t bytes_size) noexcept {
    if (bytes_size > s.ingress_packet_capacity) {
        *s.ingress_ok = false;
        return;
    }
    std::memcpy(s.ingress_packet, packet, bytes_size);
    *s.ingress_packet_size = bytes_size;
    k_ingest<<<1, 1>>>(s);
    cudaDeviceSynchronize();
}

void sio::gpu::tick_session_gpu(device_state& s, sio::id_t sid) noexcept {
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
        for (sio::id_t i = 0; i < ss.human_players; ++i) {
            all_ready = all_ready && (ss.in_packets[i].tick == 0);
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
        for (sio::id_t i = 0; i < ss.human_players; ++i) {
            ss.in_packets[i].snapshot_requested = true;
            ss.in_packets[i].boost = false;
            ss.in_packets[i].angle = NAN;
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
    for (sio::id_t i = 0; i < ss.human_players; ++i) {
        any_snapshot = any_snapshot || (ss.in_packets[i].tick == ss.tick && ss.in_packets[i].snapshot_requested);
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
    for (sio::id_t i = 0; i < ss.players; ++i) any_alive = any_alive || snake_alive(ss.snakes[i]);
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

