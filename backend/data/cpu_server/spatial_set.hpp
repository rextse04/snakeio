#pragma once
#include <config.hpp>
#include <vector.hpp>
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
#include <cmath>

namespace snakeio::cpu {
    // GetPos(const Node& node) -> vector2d.
    // SetPos(Node& node, const vector2d& value):
    // - It is guaranteed that value's lifetime spans at least that of node.
    // - It is guaranteed that setPos will not be used on valid nodes.
    // - Semantic requirement: SetPos(node, value) => GetPos(node) == value.
    template <scalar_t CellLength, size_t ObjsSize, typename Node = vector2d,
        auto GetPos = std::identity{}, auto SetPos = [](Node& node, const vector2d& value) { node = value; }>
    requires requires(const Node& cnode, Node& node, const vector2d& value) {
        requires std::default_initializable<Node>;
        { GetPos(cnode) } -> std::convertible_to<vector2d>;
        { SetPos(node, value) };
    }
    class spatial_set {
    public:
        using key_type = vector2d;
        using value_type = Node;
        using size_type = size_t;
        using difference_type = std::make_signed_t<size_type>;
        using iterator = std::span<value_type>::iterator;
        using const_iterator = std::span<const value_type>::iterator;
        using reverse_iterator = std::span<value_type>::reverse_iterator;
        using const_reverse_iterator = std::span<const value_type>::reverse_iterator;
        
        static constexpr scalar_t
            cell_length = CellLength,
            cell_area = cell_length * cell_length;
        static constexpr size_type
            rows = game_max_height / cell_length + 1,
            columns = game_max_width / cell_length + 1,
            cells = rows * columns;
        static constexpr key_type erase_key = {columns * cell_length, rows * cell_length};
        static constexpr size_type cell_id(size_type row_id, size_type column_id) noexcept {
            return row_id * columns + column_id;
        }
        static constexpr size_type row_id(const key_type& key) noexcept {
            return key[1] / cell_length;
        }
        static constexpr size_type column_id(const key_type& key) noexcept {
            return key[0] / cell_length;
        }
        static constexpr size_type cell_id(const key_type& key) noexcept {
            return cell_id(row_id(key), column_id(key));
        }

        template <typename BasePtr>
        class basic_find_iterator {
        private:
            BasePtr base;
            size_type row_begin_, row_end_, column_begin_, column_end_;
            utils::follow_t<BasePtr, value_type*> current_;
        public:
            using difference_type = difference_type;
            using value_type = value_type;

            constexpr basic_find_iterator(BasePtr base_ptr,
                size_type row_begin, size_type row_end, size_type column_begin, size_type column_end,
                utils::follow_t<BasePtr, value_type*> current) noexcept :
                base(base_ptr),
                row_begin_(row_begin), row_end_(row_end), column_begin_(column_begin), column_end_(column_end),
                current_(current) {}
            constexpr auto& operator*() const noexcept {
                return *current_;
            }
            constexpr auto* operator->() const noexcept {
                return current_;
            }
            constexpr basic_find_iterator& operator++() noexcept {
                const key_type& pos = GetPos(*++current_);
                if (column_id(pos) >= column_end_) {
                    current_ = base->find_begin(cell_id(row_id(pos) + 1, column_begin_));
                }
                return *this;
            }
            constexpr basic_find_iterator operator++(int) noexcept {
                basic_find_iterator temp = *this;
                ++*this;
                return temp;
            }
            constexpr bool operator==(std::default_sentinel_t) const noexcept {
                return row_id(GetPos(*current_)) >= row_end_;
            }
        };
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
        constexpr auto find_begin(this auto&& self, size_type cell_id) noexcept {
            return std::ranges::lower_bound(self.nodes_.data(), self.nodes_.data() + self.size_, cell_id, {},
                [](const value_type& node) { return spatial_set::cell_id(GetPos(node)); });
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
            assert(GetPos(nodes_[size_]) == erase_key);
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
                return cell_id(GetPos(node));
            });
            SetPos(nodes_[size_], erase_key);
            ready();
        }
        // UB if any node is inserted or if GetPos(node) is changed for any existing node
        // after the last call to refresh, unless GetPos(node) becomes erase_key for some node.
        // UB if key is outside game window.
        template <typename Self>
        constexpr auto find_possible(this Self&& self, const key_type& key, scalar_t radius = 0) noexcept {
            self.check_ready();
            const size_type cell_radius =
                static_cast<size_type>(radius / cell_length) + (std::fmod(radius, cell_length) > 0);
            const size_type center_row = row_id(key);
            const size_type center_column = column_id(key);
            const size_type row_begin = (center_row > cell_radius) ? (center_row - cell_radius) : 0;
            const size_type column_begin = (center_column > cell_radius) ? (center_column - cell_radius) : 0;
            return std::ranges::subrange(basic_find_iterator(&self,
                row_begin, std::min<size_type>(rows, center_row + cell_radius + 1),
                column_begin, std::min<size_type>(columns, center_column + cell_radius + 1),
                self.find_begin(cell_id(row_begin, column_begin))
            ), std::default_sentinel);
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