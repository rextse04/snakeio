#pragma once
#include "parse.hpp"
#include <game.hpp>
#include <span>

namespace snakeio::cpu {
    void tick_core(
        tick_t tick, session& session, game::random_engine* rng,
        std::span<in_packet_info> in_packets_buffer, out_delta& delta) noexcept;
}