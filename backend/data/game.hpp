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

namespace snakeio {
    class game {
    private:
        struct impl;
        using clock = std::chrono::steady_clock;

        std::mt19937 rng_{std::random_device()()};
        token_manager<game_max_sessions> sm_;
        std::unique_ptr<std::byte[]> memory_;
    public:
        // Allocate memory_ and initializes the impl in it.
        game();
        enum class add_session_error {
            no_memory = 1,
            too_many_players,
            unknown_error
        };
        constexpr const auto& session_manager() const noexcept { return sm_; }
        std::expected<id_t, add_session_error> add_session(
            id_t human_players, id_t ai_players, std::span<const key_t> keys) noexcept;
        void bind(std::stop_token stop_token, int sock) noexcept;
        template <typename Self>
        constexpr utils::follow_t<Self, impl&> get_impl(this Self&& self) noexcept {
            return *reinterpret_cast<impl*>(self.memory_.get());
        }
    };
}
