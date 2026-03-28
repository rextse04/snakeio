#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <token_manager.hpp>
#include <cpp_utils/type.hpp>
#include <array>
#include <memory>
#include <random>
#include <expected>
#include <span>
#include <stop_token>
#include <chrono>

#ifdef SNAKEIO_BENCHMARK
#include <benchmark.hpp>
#include <fstream>
#endif

namespace snakeio {
    class game {
    private:
        struct impl;
        using clock = std::chrono::steady_clock;

        token_manager<game_max_sessions> sm_;
        std::unique_ptr<std::byte[]> memory_;
        // It is guaranteed that all parameters have valid values.
        void add_session(id_t session_id,
            id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept;
#ifdef SNAKEIO_BENCHMARK
        std::ofstream tick_bench_ofs_{"tick_bench_ofs.csv"};
#endif
    public:
#ifdef SNAKEIO_BENCHMARK
        benchmark<tick_benchmark_item> tick_bench{tick_bench_ofs_};
#endif
        // Allocate memory_ and initializes the impl in it.
        game();
        constexpr const auto& session_manager() const noexcept { return sm_; }
        enum class add_session_error : unsigned char {
            no_memory = 1,
            too_many_players,
            max_tick_too_big,
            unknown_error
        };
        std::expected<id_t, add_session_error> add_session(
            id_t human_players, id_t ai_players, tick_t max_tick, std::span<const key_t> keys) noexcept {
            using enum add_session_error;
            if (human_players + ai_players > game_max_players) [[unlikely]] {
                return std::unexpected(too_many_players);
            }
            if (max_tick > game_max_tick) [[unlikely]] {
                return std::unexpected(max_tick_too_big);
            }
            auto session_id = sm_.allocate();
            if (!session_id) [[unlikely]] {
                return std::unexpected(no_memory);
            }
            add_session(*session_id, human_players, ai_players, max_tick, keys);
            return *session_id;
        }
        void bind(std::stop_token stop_token, int sock) noexcept;
        template <typename Self>
        constexpr utils::follow_t<Self, impl&> get_impl(this Self&& self) noexcept {
            return *reinterpret_cast<impl*>(self.memory_.get());
        }
    };
}
