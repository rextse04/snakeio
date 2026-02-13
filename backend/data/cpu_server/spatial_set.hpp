#pragma once
#include <config.hpp>
#include <cpp_utils/ranges.hpp>
#include <cpp_utils/type.hpp>
#include <array>
#include <tuple>
#include <span>
#include <algorithm>
#include <ranges>
#include <type_traits>

namespace snakeio::cpu {
    template <typename ObjData, scalar_t CellLength, size_t ObjsSize>
    class spatial_set {
    public:
        using key_type = vector2d;
        using value_type = std::conditional_t<std::is_same_v<ObjData, void>,
            std::tuple<vector2d*>, std::tuple<vector2d*, ObjData>>;
        using size_type = size_t;
        using difference_type = std::ptrdiff_t;
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
        static constexpr size_type cell_id(size_type row_id, size_type column_id) noexcept {
            return row_id * columns + column_id;
        }

        template <typename SpatialSet>
        class basic_find_iterator {
        private:
            SpatialSet& base;
            const size_type row_begin_, row_end_, column_begin_, column_end_;
            size_type row_current_, column_current_;
            utils::follow_t<SpatialSet, value_type*> current_;
        public:
            constexpr basic_find_iterator(SpatialSet& base,
                size_type row_begin, size_type row_end, size_type row_current,
                size_type column_begin, size_type column_end, size_type column_current,
                decltype(current_) current) noexcept :
                base(base),
                row_begin_(row_begin), row_end_(row_end), row_current_(row_current),
                column_begin_(column_begin), column_end_(column_end), column_current_(column_current),
                current_(current) {}
            constexpr auto& operator*() noexcept {
                return *current_;
            }
            constexpr basic_find_iterator& operator++() noexcept {
                const size_type seg_id = current_ - base.objs_.data(),
                    cell_id_ = cell_id(row_current_, column_current_);
                if (seg_id + 1 >= base.cell_begins_[cell_id_ + 1]) {
                    if (column_current_ + 1 >= column_end_) {
                        ++row_current_;
                        column_current_ = column_begin_;
                    } else {
                        ++column_current_;
                    }
                    current_ = base.objs_.data() + cell_id(row_current_, column_current_);
                } else {
                    ++current_;
                }
            }
            constexpr basic_find_iterator operator++(int) noexcept {
                basic_find_iterator temp = *this;
                ++*this;
                return temp;
            }
            constexpr bool operator==(std::default_sentinel_t) const noexcept {
                return row_current_ < row_end_ && column_current_ < column_end_;
            }
        };
    private:
        std::array<size_type, cells + 1> cell_begins_;
        std::array<value_type, ObjsSize> objs_;

        constexpr auto&& size_(this auto&& self) noexcept {
            return self.cell_begins_[cells];
        }
    public:
        constexpr spatial_set() noexcept : cell_begins_() {}
        template <typename R>
        requires (
            utils::container_compatible_range<R, value_type> ||
            utils::container_compatible_range<R, snake&>)
        constexpr spatial_set(R&& snakes) {
            size_() = 0;
            insert(std::forward<R>(snakes));
            refresh();
        }
        constexpr spatial_set(std::initializer_list<value_type> segments) {
            size_() = 0;
            insert(segments);
            refresh();
        }
        constexpr auto begin(this auto&& self) noexcept {
            return std::span{self.objs_, self.size_}.begin();
        }
        constexpr auto end(this auto&& self) noexcept {
            return std::span{self.objs_, self.size_}.end();
        }
        constexpr auto rbegin(this auto&& self) noexcept {
            return std::span{self.objs_, self.size_}.rbegin();
        }
        constexpr auto rend(this auto&& self) noexcept {
            return std::span{self.objs_, self.size_}.rend();
        }
        constexpr bool empty() const noexcept { return size() == 0; }
        constexpr size_type size() const noexcept { return size_(); }
        static constexpr size_type max_size() noexcept { return std::numeric_limits<size_type>::max(); }
        constexpr void clear() noexcept { size_() = 0; }
        constexpr void insert(const value_type& segment) noexcept {
            objs_[size_()++] = segment;
        }
        constexpr void insert(snake& s) noexcept {
            for (unsigned i = 0; i < s.length; ++i) {
                objs_[size_()++] = {&s, &s.segments[i]};
            }
        }
        constexpr void insert(utils::container_compatible_range<value_type> auto&& segments) noexcept {
            for (const value_type& seg : segments) insert(seg);
        }
        constexpr void insert(std::initializer_list<value_type> segments) noexcept {
            for (const value_type& seg : segments) insert(seg);
        }
        constexpr void insert(utils::container_compatible_range<snake&> auto&& snakes) noexcept {
            for (snake& s : snakes) insert(s);
        }
        template <typename T>
        requires (std::is_same_v<std::remove_cvref_t<T>, ObjData>)
        constexpr void emplace(vector2d* pos, T&& obj_data) noexcept {
            objs_[size_()++] = {pos, std::forward<T>(obj_data)};
        }
        // invalidates all iterators
        //
        constexpr void refresh() noexcept {
            std::array<size_type, std::tuple_size_v<decltype(objs_)>> cell_ids_;
            std::span cell_ids(cell_ids_.data(), size_());
            for (std::size_t i = 0; i < size_(); ++i) {
                const vector2d& pos = *std::get<0>(objs_[i]);
                cell_ids[i] = cell_id(pos[1] / cell_length, pos[0] / cell_length);
            }
            std::ranges::sort(objs_, {}, [&](value_type& t) { return cell_ids[&t - objs_.data()]; });
            for (size_type cell_id = 1; cell_id < cells ; ++cell_id) {
                cell_begins_[cell_id] =
                    std::ranges::upper_bound(cell_ids.begin() + cell_begins_[cell_id - 1], cell_ids.end(), cell_id) -
                    cell_ids.begin();
            }
        }
        // Erroneous output if any element is inserted or if the pointed-to segment is moved
        // after the last call to refresh().
        // UB if key is outside of game window.
        constexpr auto find(this auto&& self, const key_type& key, scalar_t radius = 0) noexcept {
            const size_type cell_radius =
                static_cast<size_type>(radius / cell_length) + (std::fmod(radius, cell_length) > 0);
            const size_type center_row = key[1] / cell_length;
            const size_type center_column = key[0] / cell_length;
            const size_type row_begin = std::max<size_type>(0, center_row - cell_radius);
            const size_type column_begin = std::max<size_type>(0, center_column - cell_radius);
            return std::ranges::subrange(basic_find_iterator(self,
                row_begin, std::min<size_type>(rows, center_row + cell_radius + 1), row_begin,
                column_begin, std::min<size_type>(columns, center_column + cell_radius + 1), column_begin,
                self.objs_.data() + cell_id(row_begin, column_begin)
            ), std::default_sentinel)
            | std::views::filter([key, radius](const value_type& t) {;
                const vector2d d = *std::get<0>(t) - key;
                return d[0] * d[0] + d[1] * d[1] < radius * radius;
            });
        }
    };
};