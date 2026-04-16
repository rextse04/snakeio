#pragma once
#include "impl.cuh"

namespace snakeio::gpu {
    void tick_core(game::impl& impl, std::uintmax_t global_tick) noexcept;
}