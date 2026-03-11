#include <tests/spatial_set.hpp>
#include <cpu_server/spatial_set.hpp>

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
    std::vector<const vector2d*> find(const handle* set, vector2d key, scalar_t radius) noexcept {
        auto& set_ = *reinterpret_cast<const spatial_set*>(set);
        std::vector<const vector2d*> out;
        for (const auto& p : set_.find(key, radius)) {
            out.push_back(&p);
        }
        return out;
    }
}