#include <cpp_utils/tests/common.hpp>
#include "spatial_set.hpp"
#include <vector.hpp>
#include <array>
#include <random>
#include <algorithm>
#include <ranges>

using namespace snakeio::test::spatial_set;
namespace utf = boost::unit_test;

namespace snakeio {
    std::ostream& operator<<(std::ostream& os, const vector2d& v) {
        os << '(' << v[0] << ',' << v[1] << ')';
        return os;
    }
}

BOOST_AUTO_TEST_CASE(basic_test) {
    handle* set = init();
    index_array* index_array = make_index_array();
    insert(set, {0, 0});
    const snakeio::vector2d values[] = {
        {7, 15},
        {23, 15},
        {15, 31},
        {31, 0},
        {15, 20},
        {5, 10},
        {15, 25},
        {20, 10},
        {25, 5}
    };
    insert(set, values);
    refresh(set, index_array);
    BOOST_CHECK_EQUAL(find(set, index_array, {{0, 0}, 10}).size(), 1);
    {
        const auto result = find(set, index_array, {{0, 0}, 15});
        BOOST_CHECK_EQUAL(result.size(), 2);
        if (result.size() >= 2) {
            BOOST_CHECK_EQUAL(result[0], (snakeio::vector2d{0, 0}));
            BOOST_CHECK_EQUAL(result[1], (snakeio::vector2d{5, 10}));
        }
    }
    const query queries[] = {
        {{0, 0}, 20},
        {{0, 0}, 25},
        {{0, 0}, 30},
        {{0, 0}, 35},
        {{15, 15}, 5},
        {{15, 15}, 10},
        {{15, 15}, 15},
        {{15, 15}, 20},
        {{15, 15}, 25}
    };
    const snakeio::size_t expected_sizes[] = {3, 4, 8, 10, 0, 4, 7, 8, 10};
    const auto ans = find(set, index_array, queries);
    const auto sizes = ans | std::views::transform([](const auto& a) { return a.size(); });
    BOOST_CHECK_EQUAL_COLLECTIONS(sizes.begin(), sizes.end(), std::begin(expected_sizes), std::end(expected_sizes));
    BOOST_CHECK_EQUAL(find(set, index_array, {{-1, -1}, 2}).size(), 1);
    BOOST_CHECK_EQUAL(find(set, index_array, {{1E6, 1E6}, 1}).size(), 0);
    destroy(set, index_array);
}

struct random_tests_fixture {
    struct query {
        snakeio::vector2d center;
        snakeio::scalar_t radius;
    };
    static constexpr std::size_t queries_size = 1000;

    static inline bool ready;
    static inline handle* set;
    static inline index_array* index_arr;
    static inline std::array<snakeio::vector2d, objs_size> points;
    static inline std::array<query, queries_size> queries;
    static inline std::array<snakeio::size_t, queries_size> solutions;

    random_tests_fixture() {
        if (ready) return;
        set = init();
        index_arr = make_index_array();
        std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<snakeio::scalar_t>
            x_dist(0, snakeio::game_max_width),
            y_dist(0, snakeio::game_max_height),
            r_dist(0, std::max(snakeio::game_max_width/2, snakeio::game_max_height/2));
        for (auto& p : points) {
            p = {x_dist(gen), y_dist(gen)};
        }
        for (auto& q : queries) {
            q.center = {x_dist(gen), y_dist(gen)};
            q.radius = r_dist(gen);
        }
        for (std::size_t i = 0; i < queries_size; ++i) {
            const auto& q = queries[i];
            solutions[i] = std::ranges::count_if(points, [&q](const snakeio::vector2d& p) {
                const auto d = p - q.center;
                return d[0] * d[0] + d[1] * d[1] < q.radius * q.radius;
            });
        }
        insert(set, points);
        refresh(set, index_arr);
        ready = true;
    }
};
std::ostream& operator<<(std::ostream& os, const random_tests_fixture::query& q) {
    os << '(' << q.center << ',' << q.radius << ')';
    return os;
}
BOOST_FIXTURE_TEST_SUITE(random_tests, random_tests_fixture)
BOOST_DATA_TEST_CASE(random_test, utf::data::xrange(random_tests_fixture::queries_size), idx) {
    const query& query = queries[idx];
    const size_t solution = solutions[idx];
    BOOST_TEST_CONTEXT(query) {
        BOOST_CHECK_EQUAL(find(set, index_arr, {query.center, query.radius}).size(), solution);
    }
}
BOOST_AUTO_TEST_SUITE_END()