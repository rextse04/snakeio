#pragma once
#include <spatial_set.hpp>

namespace snakeio {
    template <scalar_t CellLength, size_t ObjsSize, typename Node, auto GetPos, auto SetPos>
    requires requires(const Node& node, Node& m_node, const vector2d& value) {
        requires std::default_initializable<Node>;
        { GetPos(node) } -> std::convertible_to<vector2d>;
        { SetPos(m_node, value) };
    }
    constexpr void spatial_set<CellLength, ObjsSize, Node, GetPos, SetPos>::refresh() noexcept {

        std::ranges::sort(nodes_.begin(), nodes_.begin() + size_, {}, [](const value_type& node) {
            return cell_id(GetPos(node));
        });
        SetPos(nodes_[size_], erase_key);
        ready();
    }
}