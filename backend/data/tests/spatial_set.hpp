#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <span>
#include <vector>

namespace snakeio::test::spatial_set {
    struct handle;
    struct index_array;
    
    constexpr scalar_t cell_length = 8;
    constexpr size_t objs_size = 128;

    handle* init();
    index_array* make_index_array();
    void destroy(handle* set, index_array* index_array);
    void insert(handle* set, vector2d value) noexcept;
    void insert(handle* set, std::span<const vector2d> values) noexcept;
    void refresh(handle* set, index_array* index_array) noexcept;
    struct query {
        vector2d key;
        scalar_t radius;
    };
    std::vector<vector2d> find(const handle* set, const index_array* index_array, const query& query);
    std::vector<std::vector<vector2d>> find(const handle* set, const index_array* index_array,
        std::span<const query> queries);
}