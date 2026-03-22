#include "parse.hpp"
#include <utils.hpp>
#include <span>

using namespace snakeio;
using namespace snakeio::cpu;

snakeio::size_t cpu::store_snake_basic(std::byte* out, const snake_basic& snake) noexcept {
    const std::span<std::byte, 24> bytes(out, 24);
    store_float32(bytes.subspan<0, 4>(), snake.speed);
    store_float32(bytes.subspan<4, 4>(), snake.angle);
    store_float32(bytes.subspan<8, 4>(), snake.width);
    store_32(bytes.subspan<12, 4>(), snake.length());
    store_32(bytes.subspan<16, 4>(), snake.score);
    bytes[20] = static_cast<std::byte>(snake.boost);
    bytes[21] = static_cast<std::byte>(snake.status.status);
    bytes[22] = static_cast<std::byte>(snake.status.data);
    bytes[23] = static_cast<std::byte>(snake.human);
    return bytes.size();
}
snakeio::size_t cpu::store_snake(std::byte* out, const snake& snake) noexcept {
    const std::byte* it = out;
    out += store_snake_basic(out, snake);
    for (const vector2d& seg : snake.segments_view()) {
        store_float32(std::span<std::byte, 4>(out, 4), seg[0]);
        store_float32(std::span<std::byte, 4>(out + 4, 4), seg[1]);
        out += 8;
    }
    return out - it;
}
snakeio::size_t cpu::store_food(std::byte* out, const food& food) noexcept {
    const std::span<std::byte, 12> bytes(out, 12);
    store_float32(bytes.subspan<0, 4>(), food.pos[0]);
    store_float32(bytes.subspan<4, 4>(), food.pos[1]);
    store_float32(bytes.subspan<8, 4>(), food.width);
    return bytes.size();
}

snakeio::size_t cpu::store_delta(std::byte* const out, const session& session, out_delta& delta) noexcept {
    std::byte* it = out;
    store_32(std::span<std::byte, 4>(it, 4), 0);
    it += 4;
    for (const snake& snake : session.snakes_view()) {
        it += store_snake_basic(it, snake);
    }
    store_32(std::span<std::byte, 4>(it, 4), delta.foods_added_size);
    it += 4;
    for (const food& food : delta.foods_added_view()) {
        store_float32(std::span<std::byte, 4>(it, 4), food.pos[0]);
        store_float32(std::span<std::byte, 4>(it + 4, 4), food.pos[1]);
        store_float32(std::span<std::byte, 4>(it + 8, 4), food.width);
        it += 12;
    }
    store_32(std::span<std::byte, 4>(it, 4), delta.foods_removed_size);
    it += 4;
    for (const vector2d& pos : delta.foods_removed_view()) {
        store_float32(std::span<std::byte, 4>(it, 4), pos[0]);
        store_float32(std::span<std::byte, 4>(it + 4, 4), pos[1]);
        it += 8;
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

snakeio::size_t cpu::store_snapshot(std::byte* const out, const session& session) noexcept {
    std::byte* it = out;
    store_32(std::span<std::byte, 4>(it, 4), 1);
    store_float32(std::span<std::byte, 4>(it + 4, 4), session.width);
    store_float32(std::span<std::byte, 4>(it + 8, 4), session.height);
    store_32(std::span<std::byte, 4>(it + 12, 4), session.max_tick);
    store_32(std::span<std::byte, 4>(it + 16, 4), session.players);
    it += 20;
    for (const snake& snake : session.snakes_view()) {
        it += store_snake(it, snake);
    }
    store_32(std::span<std::byte, 4>(it, 4), session.food_set.size());
    it += 4;
    for (const food& food : session.food_set) {
        it += store_food(it, food);
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

snakeio::size_t cpu::store_lobby_status(std::byte* out, std::span<const in_packet_info> in_packets) noexcept {
    std::byte* it = out;
    store_32(std::span<std::byte, 4>(it, 4), 2);
    it += 4;
    for (const in_packet_info& in_packet : in_packets) {
        *(it++) = static_cast<std::byte>(in_packet.tick == 0);
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}

snakeio::size_t cpu::store_termination(std::byte* out, const session& session) noexcept {
    std::byte* it = out;
    store_32(std::span<std::byte, 4>(it, 4), 3);
    store_32(std::span<std::byte, 4>(it + 4, 4), session.max_tick);
    it += 8;
    for (const snake_basic& snake : session.snakes_view()) {
        it += store_snake_basic(it, snake);
    }
    const size_t size = align(it - out);
    std::ranges::fill(it, out + size, std::byte(0));
    return size;
}