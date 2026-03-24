#pragma once
#include <spatial_set.hpp>
#include <thrust/sort.h>

namespace snakeio {
    template <scalar_t CellLength, size_t ObjsSize, typename Node, auto GetPos, auto SetPos>
    requires requires(const Node& node, Node& m_node, const vector2d& value) {
        requires std::default_initializable<Node>;
        { GetPos(node) } -> std::convertible_to<vector2d>;
        { SetPos(m_node, value) };
    }
    constexpr void spatial_set<CellLength, ObjsSize, Node, GetPos, SetPos>::refresh() noexcept {

        SetPos(nodes_[size_], erase_key);
        ready();
    }
}