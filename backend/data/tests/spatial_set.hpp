#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <vector>

namespace snakeio::test::spatial_set {
    struct handle;
    
    constexpr scalar_t cell_length = 8;
    constexpr size_t objs_size = 128;

    handle* init();
    void destroy(handle* set);
    void insert(handle* set, vector2d pos) noexcept;
    void refresh(handle* set) noexcept;
    std::vector<const vector2d*> find(const handle* set, vector2d key, scalar_t radius) noexcept;
}