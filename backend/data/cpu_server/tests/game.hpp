#pragma once
#include "../parse.hpp"
#include <game.hpp>
#include <span>

namespace snakeio::test::game {
    void tick_core(
        tick_t tick, cpu::session& session, snakeio::game::random_engine* rng,
        std::span<cpu::in_packet_info> in_packets_buffer, cpu::out_delta& delta) noexcept;
}