#pragma once
#include <config.hpp>
#include <cpp_utils/ranges.hpp>
#include <cpp_utils/type.hpp>
#include <array>
#include <span>
#include <algorithm>
#include <ranges>
#include <type_traits>
#include <functional>
#include <concepts>
#include <limits>

namespace snakeio::cpu {
    template <scalar_t CellLength, size_t ObjsSize, typename Node = vector2d, auto GetPos = std::identity{}>
    requires requires(Node node) {
        { GetPos(node) } -> std::convertible_to<vector2d>;
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
                    const size_type next_cell = cell_id(row_id(pos) + 1, column_begin_);
                    current_ = std::ranges::lower_bound(current_, base->nodes_.data() + base->size_, next_cell, {},
                        [](const value_type& node) { return cell_id(GetPos(node)); });
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
        std::array<value_type, ObjsSize> nodes_;

        constexpr auto span_(this auto&& self) noexcept {
            return std::span{self.nodes_.data(), self.size_};
        }
    public:
        constexpr spatial_set() noexcept = default;
        constexpr spatial_set(std::initializer_list<value_type> nodes) {
            insert(nodes);
            refresh();
        }
        template <utils::container_compatible_range<value_type> R>
        constexpr spatial_set(R&& nodes) {
            insert(std::forward<R>(nodes));
            refresh();
        }
        constexpr auto begin(this auto&& self) noexcept {
            return self.span_().begin();
        }
        constexpr auto end(this auto&& self) noexcept {
            return self.span_().end();
        }
        constexpr auto rbegin(this auto&& self) noexcept {
            return self.span_().rbegin();
        }
        constexpr auto rend(this auto&& self) noexcept {
            return self.span_().rend();
        }
        constexpr bool empty() const noexcept { return size() == 0; }
        constexpr size_type size() const noexcept { return size_; }
        static constexpr size_type max_size() noexcept { return std::numeric_limits<size_type>::max(); }
        constexpr void clear() noexcept { size_ = 0; }
        // Erases n elements at the back.
        // Common use case: set n deleted nodes to erase_key, refresh(), then erase(n).
        constexpr void erase(size_type n) noexcept { size_ -= n; }
        constexpr void insert(const value_type& node) noexcept {
            nodes_[size_++] = node;
        }
        template <utils::container_compatible_range<value_type> R>
        constexpr void insert(R&& nodes) noexcept {
            if constexpr (std::ranges::sized_range<R>) {
                size_ += std::ranges::size(nodes);
                std::ranges::copy(nodes, nodes_.begin() + size_);
            } else {
                for (const value_type& seg : nodes) insert(seg);
            }
        }
        constexpr void insert(std::initializer_list<value_type> nodes) noexcept {
            return insert<std::initializer_list<value_type>>(nodes);
        }
        template <typename... Args>
        requires (std::is_constructible_v<value_type, Args&&...>)
        constexpr void emplace(Args&&... args) noexcept {
            new(nodes_.data() + size_++) value_type(std::forward<Args>(args)...);
        }
        // invalidates all iterators
        constexpr void refresh() noexcept {
            std::ranges::sort(nodes_.begin(), nodes_.begin() + size_, {}, [](const value_type& node) {
                return cell_id(GetPos(node));
            });
        }
        // Erroneous output if any node is inserted or if GetPos(node) is changed for any existing node
        // after the last call to refresh.
        // UB if key is outside game window.
        template <typename Self>
        constexpr auto find(this Self&& self, const key_type& key, scalar_t radius = 0) noexcept {
            const size_type cell_radius =
                static_cast<size_type>(radius / cell_length) + (std::fmod(radius, cell_length) > 0);
            const size_type center_row = row_id(key);
            const size_type center_column = column_id(key);
            const size_type row_begin = std::max<size_type>(0, center_row - cell_radius);
            const size_type column_begin = std::max<size_type>(0, center_column - cell_radius);
            return std::ranges::subrange(basic_find_iterator(&self,
                row_begin, std::min<size_type>(rows, center_row + cell_radius + 1),
                column_begin, std::min<size_type>(columns, center_column + cell_radius + 1),
                self.nodes_.data() + cell_id(row_begin, column_begin)
            ), std::default_sentinel)
            | std::views::filter([key, radius](const value_type& t) {
                auto d = static_cast<vector2d>(GetPos(t)) - key;
                return d[0] * d[0] + d[1] * d[1] < radius * radius;
            });
        }
    };
};