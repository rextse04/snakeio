#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <spatial_set_iterator.hpp>
#include <cpp_utils/ranges.hpp>
#include <cpp_utils/type.hpp>
#include <array>
#include <span>
#include <algorithm>
#include <cassert>
#include <ranges>
#include <type_traits>
#include <functional>
#include <concepts>

namespace snakeio::cpu {
    // GetPos(const Node& node) -> vector2d.
    // SetPos(Node& node, const vector2d& value):
    // - It is guaranteed that value's lifetime spans at least that of node.
    // - It is guaranteed that setPos will not be used on valid nodes.
    // - Semantic requirement: SetPos(node, value) => GetPos(node) == value.
    template <scalar_t CellLength, size_t ObjsSize, typename Node = vector2d,
        auto GetPos = std::identity{}, auto SetPos = [](Node& node, const vector2d& value) { node = value; }>
    requires requires(Node& node, const vector2d& value) {
        requires std::default_initializable<Node>;
        requires position_getter_of<decltype(GetPos), Node>;
        { SetPos(node, value) };
    }
    class spatial_set : public spatial_set_default_config<CellLength> {
    private:
        using config = spatial_set_default_config<CellLength>;
    public:
        using typename config::size_type;
        using typename config::difference_type;
        using typename config::key_type;
        using value_type = Node;
        using iterator = std::span<value_type>::iterator;
        using const_iterator = std::span<const value_type>::iterator;
        using reverse_iterator = std::span<value_type>::reverse_iterator;
        using const_reverse_iterator = std::span<const value_type>::reverse_iterator;
    private:
        size_type size_ = 0;
        std::array<value_type, ObjsSize + 1> nodes_;
#ifndef NDEBUG
        bool ready_ = true;
#endif
        constexpr void ready() noexcept {
#ifndef NDEBUG
            ready_ = true;
#endif
        }
        constexpr void unready() noexcept {
#ifndef NDEBUG
            ready_ = false;
#endif
        }
        constexpr void check_ready() const noexcept {
#ifndef NDEBUG
            assert(ready_);
#endif
        }
        constexpr auto span(this auto&& self) noexcept {
            return std::span{self.nodes_.data(), self.size_};
        }
    public:
        constexpr spatial_set() noexcept(std::is_nothrow_constructible_v<value_type>) = default;
        constexpr spatial_set(std::initializer_list<value_type> nodes) noexcept {
            insert(nodes);
            refresh();
        }
        template <utils::container_compatible_range<value_type> R>
        constexpr spatial_set(R&& nodes) {
            insert(std::forward<R>(nodes));
            refresh();
        }
        constexpr auto&& operator[](this auto&& self, size_type idx) noexcept {
            return self.nodes_[idx];
        }
        constexpr auto begin(this auto&& self) noexcept {
            return self.span().begin();
        }
        constexpr auto end(this auto&& self) noexcept {
            return self.span().end();
        }
        constexpr auto rbegin(this auto&& self) noexcept {
            return self.span().rbegin();
        }
        constexpr auto rend(this auto&& self) noexcept {
            return self.span().rend();
        }
        constexpr bool empty() const noexcept { return size() == 0; }
        constexpr size_type size() const noexcept { return size_; }
        static constexpr size_type max_size() noexcept { return ObjsSize; }
        constexpr void clear() noexcept { size_ = 0; }
        // Erases n elements at the back.
        // UB if the elements are not erase_key.
        // Common use case: set n deleted nodes to erase_key, refresh(), then erase(n).
        constexpr void erase(size_type n) noexcept {
            size_ -= n;
            assert(GetPos(nodes_[size_]) == config::erase_key);
        }
        constexpr void insert(const value_type& node) noexcept {
            nodes_[size_++] = node;
            unready();
        }
        template <utils::container_compatible_range<value_type> R>
        constexpr void insert(R&& nodes) {
            if constexpr (std::ranges::sized_range<R>) {
                std::ranges::copy(nodes, nodes_.begin() + size_);
                size_ += std::ranges::size(nodes);
            } else {
                for (const value_type& seg : nodes) insert(seg);
            }
            unready();
        }
        constexpr void insert(std::initializer_list<value_type> nodes) noexcept {
            return insert<std::initializer_list<value_type>>(nodes);
        }
        template <typename... Args>
        requires (std::is_constructible_v<value_type, Args&&...>)
        constexpr void emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<value_type, Args&&...>) {
            new(nodes_.data() + size_++) value_type(std::forward<Args>(args)...);
        }
        // invalidates all iterators
        constexpr void refresh() noexcept {
            std::ranges::sort(nodes_.begin(), nodes_.begin() + size_, {}, [](const value_type& node) {
                return config::cell_id(GetPos(node));
            });
            SetPos(nodes_[size_], config::erase_key);
            ready();
        }
        // UB if any node is inserted or if GetPos(node) is changed for any existing node
        // after the last call to refresh, unless GetPos(node) becomes erase_key for some node.
        // UB if key is outside game window.
        template <typename Self>
        constexpr auto find_possible(this Self&& self, const key_type& key, scalar_t radius = 0) noexcept {
            self.check_ready();
            return std::ranges::subrange(
                make_spatial_set_iterator<config, GetPos>(self.begin(), self.end(), key, radius),
                std::default_sentinel);
        }
        // See documentation on find_possible.
        template <typename Self>
        constexpr auto find(this Self&& self, const key_type& key, scalar_t radius = 0) noexcept {
            return self.find_possible(key, radius) | std::views::filter([key, radius](const value_type& t) {
                return (static_cast<vector2d>(GetPos(t)) - key).norm_sq() < radius * radius;
            });
        }
    };
};