#pragma once
#include "snake.hpp"
#include "food.hpp"
#include "token_manager.hpp"
#include <cpp_utils/dynamic.hpp>
#include <array>
#include <memory>
#include <random>
#include <expected>
#include <span>
#include <stop_token>
#include <vector>

namespace snakeio {
    struct game_session_view_interface {
        utils::dyn_method<id_t(utils::const_obj_ptr)> id, players;
        utils::dyn_method<scalar_t(utils::const_obj_ptr)> width, height;
        utils::dyn_method<std::span<const key_t>(utils::const_obj_ptr)> keys;
        utils::dyn_method<std::span<const snake>(utils::const_obj_ptr)> snakes;
        utils::dyn_method<std::span<const food>(utils::const_obj_ptr)> foods;
    };
    struct game_session : utils::implements<game_session_view_interface> {
        id_t id;
        scalar_t width, height;
        id_t players;
        std::array<key_t, game_max_players> keys;
        std::array<snake, game_max_players> snakes;
        size_t foods_size;
        std::array<food, game_max_food> foods;

        UTILS_DYN
        game_session_view_interface {
            .id = [](utils::const_obj_ptr self) { return static_cast<const game_session*>(self)->id; },
            .players = [](utils::const_obj_ptr self) { return static_cast<const game_session*>(self)->players; },
            .width = [](utils::const_obj_ptr self) { return static_cast<const game_session*>(self)->width; },
            .height = [](utils::const_obj_ptr self) { return static_cast<const game_session*>(self)->height; },
            .keys = [](utils::const_obj_ptr self) {
                const auto& session = *static_cast<const game_session*>(self);
                return std::span(session.keys.data(), session.players);
            },
            .snakes = [](utils::const_obj_ptr self) {
                const auto& session = *static_cast<const game_session*>(self);
                return std::span(session.snakes.data(), session.players);
            },
            .foods = [](utils::const_obj_ptr self) {
                const auto& session = *static_cast<const game_session*>(self);
                return std::span(session.foods.data(), session.foods_size);
            }
        }
        UTILS_DYN_END
    };

    class game {
    private:
        struct impl;
        std::mt19937 rng_{std::random_device()()};
        token_manager<game_max_sessions> sm_;
        std::unique_ptr<std::byte[]> memory_;
    public:
        game();
        void generate_session(game_session& session,
            id_t human_players, id_t ai_players, std::span<const key_t> keys) noexcept;
        enum class add_session_error {
            no_memory,
            too_many_players,
            unknown_error
        };
        const auto& session_manager() const noexcept { return sm_; }
        std::expected<id_t, add_session_error> add_session(const game_session& session) noexcept;
        // This returns the plaintext of the session data, which is then encrypted by the caller and sent to the clients.
        // The plaintext format is as follows:
        // <width: 4 bytes><height: 4 bytes><players: 4 bytes><snakes: variable><foods: variable>
        std::vector<std::byte> get_session(id_t session_id) const noexcept;
        bool remove_session(id_t session_id) noexcept;
        void bind(std::stop_token stop_token, int sock) noexcept;
    };
}
