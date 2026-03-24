#include <cpp_utils/tests/common.hpp>
#include <spatial_set.tpp>
#include <vector.hpp>
#include <array>
#include <random>
#include <algorithm>
#include <ranges>

using spatial_set = snakeio::spatial_set<snakeio::scalar_t(8), 128>;
namespace utf = boost::unit_test;

namespace snakeio {
    std::ostream& operator<<(std::ostream& os, const vector2d& v) {
        os << '(' << v[0] << ',' << v[1] << ')';
        return os;
    }
}

BOOST_AUTO_TEST_CASE(basic_test) {
    spatial_set set;
    set.insert({0, 0});
    set.insert({7, 15});
    set.insert({23, 15});
    set.insert({15, 31});
    set.insert({31, 0});
    set.insert({15, 20});
    set.insert({5, 10});
    set.insert({15, 25});
    set.insert({20, 10});
    set.insert({25, 5});
    set.refresh();
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({0, 0}, 10)), 1);
    {
        auto result = set.find({0, 0}, 15);
        const auto size = std::ranges::distance(result);
        BOOST_CHECK_EQUAL(size, 2);
        if (size == 2) {
            auto it = result.cbegin();
            BOOST_CHECK_EQUAL(*it, (snakeio::vector2d{0, 0}));
            ++it;
            BOOST_CHECK_EQUAL(*it, (snakeio::vector2d{5, 10}));
        }
    }
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({0, 0}, 20)), 3);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({0, 0}, 25)), 4);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({0, 0}, 30)), 8);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({0, 0}, 35)), 10);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({15, 15}, 5)), 0);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({15, 15}, 10)), 4);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({15, 15}, 15)), 7);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({15, 15}, 20)), 8);
    BOOST_CHECK_EQUAL(std::ranges::distance(set.find({15, 15}, 25)), 10);
}

struct random_tests_fixture {
    struct query {
        snakeio::vector2d center;
        snakeio::scalar_t radius;
    };
    static constexpr std::size_t queries_size = 1000;

    static inline bool ready;
    static inline spatial_set set;
    static inline std::array<snakeio::vector2d, spatial_set::max_size()> points;
    static inline std::array<query, queries_size> queries;
    static inline std::array<snakeio::size_t, queries_size> solutions;

    random_tests_fixture() {
        if (ready) return;
        std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<snakeio::scalar_t>
            x_dist(0, snakeio::game_max_width),
            y_dist(0, snakeio::game_max_height),
            r_dist(0, std::max(snakeio::game_max_width/2, snakeio::game_max_height/2));
        for (auto& p : points) {
            p = {x_dist(gen), y_dist(gen)};
            set.insert(p);
        }
        set.refresh();
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
        BOOST_CHECK_EQUAL(std::ranges::distance(set.find(query.center, query.radius)), solution);
    }
}
BOOST_AUTO_TEST_SUITE_END()