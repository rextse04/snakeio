#include <cpp_utils/tests/common.hpp>
#include "spatial_set.hpp"
#include <vector.hpp>
#include <array>
#include <random>
#include <algorithm>

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
    insert(set, {0, 0});
    insert(set, {7, 15});
    insert(set, {23, 15});
    insert(set, {15, 31});
    insert(set, {31, 0});
    insert(set, {15, 20});
    insert(set, {5, 10});
    insert(set, {15, 25});
    insert(set, {20, 10});
    insert(set, {25, 5});
    refresh(set);
    BOOST_CHECK_EQUAL(find(set, {0, 0}, 10).size(), 1);
    {
        const auto result = find(set, {0, 0}, 15);
        BOOST_CHECK_EQUAL(result.size(), 2);
        if (result.size() >= 2) {
            BOOST_CHECK_EQUAL(*result[0], (snakeio::vector2d{0, 0}));
            BOOST_CHECK_EQUAL(*result[1], (snakeio::vector2d{5, 10}));
        }
    }
    BOOST_CHECK_EQUAL(find(set, {0, 0}, 20).size(), 3);
    BOOST_CHECK_EQUAL(find(set, {0, 0}, 25).size(), 4);
    BOOST_CHECK_EQUAL(find(set, {0, 0}, 30).size(), 8);
    BOOST_CHECK_EQUAL(find(set, {0, 0}, 35).size(), 10);
    BOOST_CHECK_EQUAL(find(set, {15, 15}, 5).size(), 0);
    BOOST_CHECK_EQUAL(find(set, {15, 15}, 10).size(), 4);
    BOOST_CHECK_EQUAL(find(set, {15, 15}, 15).size(), 7);
    BOOST_CHECK_EQUAL(find(set, {15, 15}, 20).size(), 8);
    BOOST_CHECK_EQUAL(find(set, {15, 15}, 25).size(), 10);
    destroy(set);
}

struct random_tests_fixture {
    struct query {
        snakeio::vector2d center;
        snakeio::scalar_t radius;
    };
    static constexpr std::size_t queries_size = 1000;

    static inline handle* set;
    static inline std::array<snakeio::vector2d, objs_size> points;
    static inline std::array<query, queries_size> queries;
    static inline std::array<snakeio::size_t, queries_size> solutions;

    random_tests_fixture() {
        set = init();
        std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<snakeio::scalar_t>
            x_dist(0, snakeio::game_max_width),
            y_dist(0, snakeio::game_max_height),
            r_dist(0, std::max(snakeio::game_max_width/2, snakeio::game_max_height/2));
        for (auto& p : points) {
            p = {x_dist(gen), y_dist(gen)};
            insert(set, p);
        }
        refresh(set);
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
    }
    ~random_tests_fixture() { destroy(set); }
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
        const auto result = find(set, query.center, query.radius);
        BOOST_CHECK_EQUAL(result.size(), solution);
    }
}
BOOST_AUTO_TEST_SUITE_END()