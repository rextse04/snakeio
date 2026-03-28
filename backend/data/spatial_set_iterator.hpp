#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <cpp_utils/type.hpp>
#include <iterator>
#include <type_traits>
#include <concepts>

#ifdef __CUDACC__
#include <cuda/std/algorithm>
#include <cuda/std/cmath>
#include <cuda/std/functional>
#else
#include <algorithm>
#include <cmath>
#include <functional>
#endif
#include <compatibility.hpp>

namespace snakeio {
    template <typename F, typename Node>
    concept position_getter_of = requires (F f, const Node& node) {
        { f(node) } -> std::convertible_to<vector2d>;
    };
    template <typename T>
    concept spatial_set_relative_config = requires(const typename T::key_type key, typename T::size_type idx) {
        requires std::is_unsigned_v<typename T::size_type>;
        requires std::is_integral_v<typename T::difference_type>;
        { T::row_id(key) } -> std::same_as<typename T::size_type>;
        { T::column_id(key) } -> std::same_as<typename T::size_type>;
        { T::cell_id(idx, idx) } -> std::same_as<typename T::size_type>;
    };
    template <typename T>
    concept spatial_set_absolute_config = requires {
        requires spatial_set_relative_config<T>;
        { T::cell_length } -> utils::equiv_to<scalar_t>;
        { T::rows } -> utils::equiv_to<typename T::size_type>;
        { T::columns } -> utils::equiv_to<typename T::size_type>;
    };

    template <scalar_t CellLength>
    struct spatial_set_default_config {
        using size_type = size_t;
        using difference_type = std::make_signed_t<size_type>;
        using key_type = vector2d;

        static constexpr scalar_t
            cell_length = CellLength,
            cell_area = cell_length * cell_length;
        static constexpr size_type
            rows = game_max_height / cell_length + 1,
            columns = game_max_width / cell_length + 1,
            cells = rows * columns;
        static constexpr key_type erase_key = {columns * cell_length, rows * cell_length};

        __host__ __device__ static constexpr size_type row_id(const vector2d& key) noexcept {
            return key[1] / CellLength;
        }
        __host__ __device__ static constexpr size_type column_id(const vector2d& key) noexcept {
            return key[0] / CellLength;
        }
        __host__ __device__ static constexpr size_type cell_id(size_type row_id, size_type column_id) noexcept {
            return row_id * columns + column_id;
        }
        __host__ __device__ static constexpr size_type cell_id(const vector2d& key) noexcept {
            return cell_id(row_id(key), column_id(key));
        }
    };

    template <std::contiguous_iterator BaseIter, auto GetPos, spatial_set_relative_config Config>
    requires (position_getter_of<decltype(GetPos), typename std::iterator_traits<BaseIter>::reference>)
    class spatial_set_iterator {
    public:
        using size_type = Config::size_type;
        using difference_type = Config::difference_type;
        using value_type = std::iterator_traits<BaseIter>::value_type;
        using reference = std::iterator_traits<BaseIter>::reference;
    private:
        BaseIter current_, end_;
        size_type row_begin_, row_end_, column_begin_, column_end_;
    public:
        __host__ __device__ constexpr spatial_set_iterator(BaseIter current, BaseIter end,
            size_type row_begin, size_type row_end, size_type column_begin, size_type column_end) noexcept :
            current_(current), end_(end),
            row_begin_(row_begin), row_end_(row_end), column_begin_(column_begin), column_end_(column_end) {}
        __host__ __device__ constexpr auto&& operator*() const noexcept {
            return *current_;
        }
        __host__ __device__ constexpr auto* operator->() const noexcept {
            return &*current_;
        }
        __host__ __device__ constexpr void advance_to(size_type cell_id) noexcept {
            constexpr auto proj = [](reference node) {
                const vector2d& pos = GetPos(node);
                return Config::cell_id(Config::row_id(pos), Config::column_id(pos));
            };
#ifdef __CUDACC__
            constexpr auto comp = [](reference node, size_type cell_id) {
                return proj(node) < cell_id;
            };
            current_ = cuda::std::lower_bound(current_, end_, cell_id, comp);
#else
            current_ = std::ranges::lower_bound(current_, end_, cell_id, {}, proj);
#endif
        }
        // UB if [current_, end_) is not sorted wrt CellId
        __host__ __device__ constexpr spatial_set_iterator& operator++() noexcept {
            const vector2d& pos = GetPos(*++current_);
            if (Config::column_id(pos) >= column_end_) {
                advance_to(Config::cell_id(Config::row_id(pos) + 1, column_begin_));
            }
            return *this;
        }
        __host__ __device__ constexpr spatial_set_iterator operator++(int) noexcept {
            spatial_set_iterator temp = *this;
            ++*this;
            return temp;
        }
        __host__ __device__ constexpr bool operator==(std::default_sentinel_t) const noexcept {
            return Config::row_id(GetPos(*current_)) >= row_end_;
        }
    };

    template <
        spatial_set_relative_config Config, auto GetPos = stdc::identity{},
        std::contiguous_iterator BaseIter, typename SizeT = Config::size_type>
    requires (position_getter_of<decltype(GetPos), typename std::iterator_traits<BaseIter>::reference>)
    __host__ __device__ constexpr auto make_spatial_set_iterator(BaseIter begin, BaseIter end,
        SizeT row_begin, SizeT row_end, SizeT column_begin, SizeT column_end) noexcept {
        spatial_set_iterator<BaseIter, GetPos, Config> it(begin, end, row_begin, row_end, column_begin, column_end);
        it.advance_to(Config::cell_id(row_begin, column_begin));
        return it;
    }
    template <spatial_set_absolute_config Config, auto GetPos = stdc::identity{}, std::contiguous_iterator BaseIter>
    requires (position_getter_of<decltype(GetPos), typename std::iterator_traits<BaseIter>::reference>)
    __host__ __device__ constexpr auto make_spatial_set_iterator(BaseIter begin, BaseIter end,
        const vector2d& key, scalar_t radius) noexcept {
        using size_type = Config::size_type;
        const size_type cell_radius =
                static_cast<size_type>(radius / Config::cell_length) + (stdc::fmod(radius, Config::cell_length) > 0);
        const size_type center_row = Config::row_id(key);
        const size_type center_column = Config::column_id(key);
        const size_type row_begin = (center_row > cell_radius) ? (center_row - cell_radius) : 0;
        const size_type column_begin = (center_column > cell_radius) ? (center_column - cell_radius) : 0;
        // min takes const reference, but nvcc won't allow device code to reference constexpr field stored in host
        constexpr size_type rows = Config::rows;
        constexpr size_type columns = Config::columns;
        return make_spatial_set_iterator<Config, GetPos, BaseIter>(
            begin, end,
            row_begin, stdc::min<size_type>(rows, center_row + cell_radius + 1),
            column_begin, stdc::min<size_type>(columns, center_column + cell_radius + 1));
    }
}