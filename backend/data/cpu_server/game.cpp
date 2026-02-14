#include "impl.hpp"
#include <game.hpp>
#include <memory>
#include <ranges>

using namespace snakeio;

game::game() : memory_(new(static_cast<std::align_val_t>(alignof(impl))) std::byte[sizeof(impl)]) {
    new(memory_.get()) impl;
}

std::expected<id_t, game::add_session_error> game::add_session(const game_session& session) noexcept {
    using enum add_session_error;
    impl& impl_ = get_impl();
    auto session_id = sm_.allocate();
    if (!session_id) [[unlikely]] {
        return std::unexpected(no_memory);
    }
    std::ranges::copy_n(session.keys.begin(), session.players, impl_.keys[*session_id].begin());
    impl::session& impl_session = impl_.sessions[*session_id];
    impl_session.players = session.players;
    impl_session.width = session.width;
    impl_session.height = session.height;
    std::ranges::copy_n(session.snakes.begin(), session.players, impl_session.snakes.begin());
    impl_session.snakes_set.clear();
    for (snake& snake : std::span(impl_session.snakes.data(), impl_session.players)) {
        for (vector2d& seg : std::span(snake.segments.data(), snake.length)) {
            impl_session.snakes_set.emplace(&snake, &seg);
        }
    }
    impl_session.food_set.clear();
    impl_session.food_set.insert(std::span(session.foods.begin(), session.foods_size));
    return *session_id;
}

utils::dptr<const game_session_view_interface> game::get_session(id_t session_id) const noexcept {
    if (!sm_[session_id]) return nullptr;
    return utils::dptr<const game_session_view_interface>(get_impl().sessions.data() + session_id);
}

bool game::remove_session(id_t session_id) noexcept {
    if (sm_[session_id]) {
        sm_.deallocate(session_id);
        return true;
    } else {
        return false;
    }
}

void game::bind(std::stop_token stop_token, int sock) noexcept {

}
