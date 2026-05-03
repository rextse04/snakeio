#include <game.hpp>
#include <network.hpp>

int snakeio::game::open_data_port() noexcept {
    return open_port("data", {
        .sin6_family = AF_INET6,
        .sin6_port = htons(data_plane_ext_port),
        .sin6_addr = in6addr_any
    });
}