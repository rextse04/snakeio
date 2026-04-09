#include <tests/spatial_set.hpp>
#include <cpu_server/spatial_set.hpp>
#include <ranges>

namespace snakeio::test::spatial_set {
    using spatial_set = cpu::spatial_set<cell_length, objs_size>;

    void refresh(handle* set, index_array*) noexcept {
        auto& set_ = *reinterpret_cast<spatial_set*>(set);
        set_.refresh();
    }
    std::vector<vector2d> find(const handle* set, const index_array*, const query& query) {
        auto& set_ = *reinterpret_cast<const spatial_set*>(set);
        return std::ranges::to<std::vector<vector2d>>(set_.find(query.key, query.radius));
    }
}