#include <game.hpp>
#include <logger.hpp>
#include <network.hpp>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <thread>
#include <stop_token>
#include <utility>
#include <span>
#include <chrono>

#include "utils.hpp"

using namespace snakeio;

game game_;

[[nodiscard]] static int open_port(std::string_view name, const sockaddr_in6& addr) {
    const int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        logger::error("Failed to create {} socket.", name);
        return sock;
    }
    constexpr int off = 0;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
        logger::warn("Failed to clear IPV6_V6ONLY of {} port.", name);
    }
    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger::error("Failed to bind {} socket: {}.", name, std::strerror(errno));
        return -1;
    }
    sockaddr_storage addr_storage{};
    std::memcpy(&addr_storage, &addr, sizeof(addr));
    logger::info("{} port listening on {}.", name, addr_storage);
    return sock;
}

static void control_port(std::stop_source stop_source, int sock) {
    std::byte buffer[1 + 5 + 1 + 1 + game_max_players * sizeof(snakeio::key_t)]{};
    sockaddr_storage client_addr{};
    while (true) {
        socklen_t client_addr_len = sizeof(client_addr);
        const ssize_t recv_len = recvfrom(sock, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (recv_len < 1) [[unlikely]] {
            if (recv_len == 0) [[unlikely]] {
                logger::warn("Received empty packet on control port from {}.", client_addr);
            } else if (errno != EINTR) {
                logger::warn("recvfrom failed on control port: {}.", std::strerror(errno));
            }
            continue;
        }
        switch (static_cast<unsigned char>(buffer[0])) {
            case 0: { // kill
                std::ignore = stop_source.request_stop();
                return;
            }
            case 1: { // new session token
                constexpr std::size_t header_size = 1 + 5 + 1 + 1;
                if (recv_len < header_size) {
                    goto invalid_format;
                }
                const std::string_view token(reinterpret_cast<char*>(buffer + 1), 5);
                const auto human_players = static_cast<unsigned char>(buffer[6]),
                    ai_players = static_cast<unsigned char>(buffer[7]);
                if (recv_len < header_size + human_players * sizeof(snakeio::key_t)) {
                    goto invalid_format;
                }
                // This is defined because
                // 1. key_t is an implicit lifetime type
                // 2. storage is given by an array of unsigned char
                // 3. key_t is an array of std::byte, which is guaranteed to have an alignment of 1
                const auto keys = reinterpret_cast<const snakeio::key_t*>(buffer + header_size);
                const auto id = game_.add_session(human_players, ai_players, std::span(keys, human_players));
                if (id.has_value()) {
                    logger::debug("Session token {} mapped to session ID {}.", token, id.value());
                    buffer[6] = std::byte(0);
                    store_32(std::span<std::byte, 4>(buffer + 8, 4), id.value());
                } else {
                    std::string_view error;
                    switch (id.error()) {
                        using enum game::add_session_error;
                        case no_memory: {
                            error = "no memory";
                            break;
                        }
                        case too_many_players: {
                            error = "too many players";
                            break;
                        }
                        case unknown_error: {
                            error = "unknown error";
                            break;
                        }
                        default: std::unreachable();
                    }
                    logger::warn("Failed to create new session: {}.", error);
                    buffer[6] = static_cast<std::byte>(id.error());
                }
                sendto(sock, buffer, header_size + 4, 0,
                    reinterpret_cast<const sockaddr*>(&client_addr), client_addr_len);
                break;
            }
            default: {
                logger::warn("Received unknown command on control port.");
                logger::print_packet(logger::debug, std::span(buffer, recv_len));
                break;
            }
        }
        continue;
        invalid_format:
        logger::warn("Received invalid command format on control port.");
        logger::print_packet(logger::debug, std::span(buffer, recv_len));
    }
}

static void data_port(std::stop_token stop_token, int sock) {
    constexpr timeval read_timeout{.tv_usec = std::chrono::microseconds(game_tick_rate).count()};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));
    game_.bind(std::move(stop_token), sock);
}

int main() {
    const int control_sock = open_port("control", {
        .sin6_family = AF_INET6,
        .sin6_port = htons(data_plane_int_port),
        .sin6_addr = in6addr_loopback
    });
    const int data_sock = open_port("data", {
        .sin6_family = AF_INET6,
        .sin6_port = htons(data_plane_ext_port),
        .sin6_addr = in6addr_any
    });
    if (control_sock < 0 || data_sock < 0) {
        return EXIT_FAILURE;
    }
    std::stop_source stop_source;
    std::jthread control_thread(control_port, stop_source, control_sock),
        data_thread(data_port, stop_source.get_token(), data_sock);
}