#include <tests/spatial_set.hpp>
#include <cpu_server/spatial_set.hpp>
#include <ranges>

namespace snakeio::test::spatial_set {
    using spatial_set = cpu::spatial_set<cell_length, objs_size>;
    using index_array_type = spatial_set::index_array_type;

    void refresh(handle* set, index_array* index_array) noexcept {
        auto& set_ = *reinterpret_cast<spatial_set*>(set);
        auto& index_array_ = *reinterpret_cast<index_array_type*>(index_array);
        set_.refresh(index_array_);
    }
    std::vector<vector2d> find(const handle* set, const index_array* index_array, const query& query) {
        auto& set_ = *reinterpret_cast<const spatial_set*>(set);
        auto& index_array_ = *reinterpret_cast<const index_array_type*>(index_array);
        return std::ranges::to<std::vector<vector2d>>(set_.find(index_array_, query.key, query.radius));
    }
}