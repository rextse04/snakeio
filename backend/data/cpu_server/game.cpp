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
    impl& impl_ = impl::get(*this);
    auto session_id = sm_.allocate();
    if (!session_id) [[unlikely]] {
        return std::unexpected(no_memory);
    }
    std::ranges::copy_n(session.keys.begin(), session.players, impl_.keys[*session_id].begin());
    impl::session& impl_session = impl_.sessions[*session_id];
    impl_session.players = session.players;
    std::ranges::copy_n(session.snakes.begin(), session.players, impl_session.snakes.begin());
    impl_session.set.insert(std::ranges::subrange(impl_session.snakes, session.players));
    return *session_id;
}

std::vector<std::byte> game::get_session(id_t session_id) const noexcept {

}
