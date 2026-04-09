#include <tests/spatial_set.hpp>
#include <cpu_server/spatial_set.hpp>

namespace snakeio::test::spatial_set {
    using spatial_set = cpu::spatial_set<cell_length, objs_size>;

    handle* init() {
        return reinterpret_cast<handle*>(new spatial_set);
    }
    index_array* make_index_array() {
        return reinterpret_cast<index_array*>(new spatial_set::index_array_type);
    }
    void destroy(handle* set, index_array* index_array) {
        delete reinterpret_cast<spatial_set*>(set);
        delete reinterpret_cast<spatial_set::index_array_type*>(index_array);
    }
    void insert(handle* set, vector2d value) noexcept {
        auto& set_ = *reinterpret_cast<spatial_set*>(set);
        set_.insert(value);
    }
    void insert(handle* set, std::span<const vector2d> values) noexcept {
        auto& set_ = *reinterpret_cast<spatial_set*>(set);
        set_.insert(values);
    }
    std::vector<std::vector<vector2d>> find(const handle* set, const index_array* index_array,
        std::span<const query> queries) {
        std::vector<std::vector<vector2d>> out;
        out.reserve(queries.size());
        for (const auto& query : queries) {
            out.push_back(find(set, index_array, query));
        }
        return out;
    }
}