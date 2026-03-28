#include <tests/spatial_set.hpp>
#include <cpu_server/spatial_set.hpp>
#include <ranges>

namespace snakeio::test::spatial_set {
    using spatial_set = cpu::spatial_set<cell_length, objs_size>;

    handle* init() {
        return reinterpret_cast<handle*>(new spatial_set);
    }
    void destroy(handle* set) {
        delete reinterpret_cast<spatial_set*>(set);
    }
    void insert(handle* set, vector2d pos) noexcept {
        auto& set_ = *reinterpret_cast<spatial_set*>(set);
        set_.insert(pos);
    }
    void refresh(handle* set) noexcept {
        auto& set_ = *reinterpret_cast<spatial_set*>(set);
        set_.refresh();
    }
    std::vector<vector2d> find(const handle* set, vector2d key, scalar_t radius) noexcept {
        auto& set_ = *reinterpret_cast<const spatial_set*>(set);
        return std::ranges::to<std::vector<vector2d>>(set_.find(key, radius));
    }
}