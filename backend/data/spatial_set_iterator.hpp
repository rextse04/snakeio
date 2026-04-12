#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <cpp_utils/type.hpp>
#include <cpp_utils/ranges.hpp>
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
    template <typename I, typename Config>
    concept spatial_set_index_iterator =
        std::random_access_iterator<I> && utils::container_compatible_iterator<I, typename Config::size_type>;
    template <typename R, typename Config>
    concept spatial_set_index_range =
        std::ranges::random_access_range<R> && utils::container_compatible_range<R, typename Config::size_type>;

    template <scalar_t WorldWidth, scalar_t WorldHeight, scalar_t CellLength>
    struct spatial_set_default_config {
        using size_type = size_t;
        using difference_type = std::make_signed_t<size_type>;
        using key_type = vector2d;

        static constexpr scalar_t
            cell_length = CellLength,
            cell_area = cell_length * cell_length;
        static constexpr size_type
            rows = WorldWidth / cell_length + 1,
            columns = WorldHeight / cell_length + 1,
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
        static constexpr size_type erase_index = cell_id(erase_key);
    };

    namespace detail {
        template <typename Config, typename BaseIter>
        class spatial_set_iterator_base {
        public:
            using size_type = Config::size_type;
            using difference_type = Config::difference_type;
            using value_type = std::iterator_traits<BaseIter>::value_type;
            using reference = std::iterator_traits<BaseIter>::reference;
        protected:
            BaseIter current_;

            __host__ __device__ constexpr spatial_set_iterator_base(BaseIter current) noexcept : current_(current) {}
        public:
            __host__ __device__ constexpr auto&& operator*() const noexcept {
                return *current_;
            }
            __host__ __device__ constexpr auto* operator->() const noexcept {
                return std::addressof(*current_);
            }
        };
    }
    template <spatial_set_relative_config Config, std::random_access_iterator BaseIter, auto GetPos>
    requires (position_getter_of<decltype(GetPos), typename std::iterator_traits<BaseIter>::reference>)
    class spatial_set_independent_iterator : public detail::spatial_set_iterator_base<Config, BaseIter> {
        using parent = detail::spatial_set_iterator_base<Config, BaseIter>;
    public:
        using typename parent::size_type;
        using typename parent::reference;
    private:
        BaseIter end_;
        size_type row_end_, column_begin_, column_end_;
    public:
        // UB if [current, end) is not a valid range or if GetPos(*end) is not erase_key,
        // where erase_key is a key with cell_id larger than all valid values.
        __host__ __device__ constexpr spatial_set_independent_iterator(BaseIter current, BaseIter end,
            size_type row_end, size_type column_begin, size_type column_end) noexcept :
            parent(current), end_(end),
            row_end_(row_end), column_begin_(column_begin), column_end_(column_end) {}
        __host__ __device__ constexpr void advance_to(size_type cell_id) noexcept {
            constexpr auto proj = [](reference node) {
                const vector2d& pos = GetPos(node);
                return Config::cell_id(Config::row_id(pos), Config::column_id(pos));
            };
#ifdef __CUDACC__
            constexpr auto comp = [](reference node, size_type cell_id) {
                return proj(node) < cell_id;
            };
            parent::current_ = cuda::std::lower_bound(parent::current_, end_, cell_id, comp);
#else
            parent::current_ = std::ranges::lower_bound(parent::current_, end_, cell_id, {}, proj);
#endif
        }
        __host__ __device__ constexpr spatial_set_independent_iterator& operator++() noexcept {
            const vector2d& pos = GetPos(*++parent::current_);
            if (Config::column_id(pos) >= column_end_) {
                advance_to(Config::cell_id(Config::row_id(pos) + 1, column_begin_));
            }
            return *this;
        }
        __host__ __device__ constexpr spatial_set_independent_iterator operator++(int) noexcept {
            spatial_set_independent_iterator temp = *this;
            ++*this;
            return temp;
        }
        __host__ __device__ constexpr bool operator==(std::default_sentinel_t) const noexcept {
            return Config::row_id(GetPos(*parent::current_)) >= row_end_;
        }
    };
    template <spatial_set_absolute_config Config,
        std::random_access_iterator BaseIter, spatial_set_index_iterator<Config> IndexIter>
    class spatial_set_dependent_iterator : public detail::spatial_set_iterator_base<Config, BaseIter> {
        using parent = detail::spatial_set_iterator_base<Config, BaseIter>;
    public:
        using typename parent::size_type;
    private:
        IndexIter index_current_, index_end_;
        size_type row_end_, column_begin_, column_end_;
    public:
        // Let size = index_end - index_current.
        // UB if [current, current + size] is not a valid range or if it does not correspond with [index_current, index_end].
        // UB if *index_end is not a cell id larger than all valid values.
        __host__ __device__ constexpr spatial_set_dependent_iterator(BaseIter current,
            IndexIter index_current, IndexIter index_end,
            size_type row_end, size_type column_begin, size_type column_end) noexcept :
            parent(current), index_current_(index_current), index_end_(index_end),
            row_end_(row_end), column_begin_(column_begin), column_end_(column_end) {}
        __host__ __device__ constexpr void advance_to(size_type cell_id) noexcept {
            const auto adv = stdc::lower_bound(index_current_, index_end_, cell_id) - index_current_;
            parent::current_ += adv;
            index_current_ += adv;
        }
        __host__ __device__ constexpr spatial_set_dependent_iterator& operator++() noexcept {
            ++parent::current_;
            ++index_current_;
            if (*index_current_ % Config::columns >= column_end_) {
                const size_type row_id = *index_current_ / Config::columns;
                advance_to(Config::cell_id(row_id + 1, column_begin_));
            }
            return *this;
        }
        __host__ __device__ constexpr spatial_set_dependent_iterator operator++(int) noexcept {
            spatial_set_dependent_iterator temp = *this;
            ++*this;
            return temp;
        }
        __host__ __device__ constexpr bool operator==(std::default_sentinel_t) const noexcept {
            return *index_current_ / Config::columns >= row_end_;
        }
    };

    template <typename SizeT>
    struct spatial_set_rect {
        SizeT row_begin, row_end, column_begin, column_end;
    };
    template <spatial_set_absolute_config Config>
    __host__ __device__ auto bounding_rect(const vector2d& key, scalar_t radius) noexcept
    -> spatial_set_rect<typename Config::size_type> {
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
        return {
            row_begin, stdc::min<size_type>(rows, center_row + cell_radius + 1),
            column_begin, stdc::min<size_type>(columns, center_column + cell_radius + 1)
        };
    }

    // Refer to the documentation of spatial_set_independent_iterator for preconditions.
    template <spatial_set_relative_config Config, auto GetPos = stdc::identity{}, std::random_access_iterator BaseIter>
    requires (position_getter_of<decltype(GetPos), typename std::iterator_traits<BaseIter>::reference>)
    __host__ __device__ constexpr auto make_spatial_set_iterator(BaseIter begin, BaseIter end,
        const spatial_set_rect<typename Config::size_type>& rect) noexcept {
        spatial_set_independent_iterator<Config, BaseIter, GetPos> it(begin, end,
            rect.row_end, rect.column_begin, rect.column_end);
        it.advance_to(Config::cell_id(rect.row_begin, rect.column_begin));
        return it;
    }
    // Refer to the documentation of spatial_set_dependent_iterator for preconditions.
    template <spatial_set_absolute_config Config,
        std::random_access_iterator BaseIter, spatial_set_index_iterator<Config> IndexIter>
    __host__ __device__ constexpr auto make_spatial_set_iterator(BaseIter begin,
        IndexIter index_begin, IndexIter index_end,
        const spatial_set_rect<typename Config::size_type>& rect) noexcept {
        spatial_set_dependent_iterator<Config, BaseIter, IndexIter>
            it(begin, index_begin, index_end, rect.row_end, rect.column_begin, rect.column_end);
        it.advance_to(Config::cell_id(rect.row_begin, rect.column_begin));
        return it;
    }
}