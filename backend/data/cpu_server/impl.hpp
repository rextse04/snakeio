#pragma once
#include <game.hpp>
#include "spatial_set.hpp"
#include <cpp_utils/type.hpp>
#include <array>

namespace snakeio {
    struct game::impl {
        struct session {
            id_t players;
            scalar_t width, height;
            std::array<snake, game_max_players> snakes{};
            cpu::spatial_set set{snakes};
            size_t foods_size;
            std::array<food, game_max_food> foods{};
        };
        struct session_view : utils::implements<game_session_view_interface> {
            id_t id;
            scalar_t width, height;
            id_t players;
            const std::array<key_t, game_max_players>* keys;
            const cpu::spatial_set* set;

            UTILS_DYN
            game_session_view_interface {
                .id = [](utils::const_obj_ptr self) { return static_cast<const session_view*>(self)->id; },
                .players = [](utils::const_obj_ptr self) { return static_cast<const session_view*>(self)->players; },
                .width = [](utils::const_obj_ptr self) { return static_cast<const session_view*>(self)->width; },
                .height = [](utils::const_obj_ptr self) { return static_cast<const session_view*>(self)->height; },
                .keys = [](utils::const_obj_ptr self) {
                    const auto& view = *static_cast<const session_view*>(self);
                    return std::span(view.keys->data(), view.players);
                },
                .snakes = [](utils::const_obj_ptr self) {
                    const auto& view = *static_cast<const session_view*>(self);
                    return view.set->get_snakes_span(view.players);
                },
                .foods = [](utils::const_obj_ptr self) {
                    const auto& view = *static_cast<const session_view*>(self);
                    return view.set->get_foods_span();
                }
            }
            UTILS_DYN_END
        };

        std::array<std::array<key_t, game_max_players>, game_max_sessions> keys{};
        std::array<session, game_max_sessions> sessions{};

        template <typename Game>
        static constexpr auto&& get(Game&& game) noexcept {
            return *reinterpret_cast<utils::follow_t<Game, impl*>>(game.memory_.get());
        }
    };
}