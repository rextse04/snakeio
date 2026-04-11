#pragma once
#include <tests/spatial_set.hpp>
#include <cpu_server/spatial_set.hpp>

namespace snakeio::test::spatial_set {
    using spatial_set = cpu::spatial_set<world_dim, cell_length, objs_size>;
}