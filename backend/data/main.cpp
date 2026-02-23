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

using namespace snakeio;

game game_;

[[nodiscard]] static int open_port(std::string_view name, const sockaddr_in6& addr) {
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        logger::error("Failed to create {} socket.", name);
        return sock;
    }
    constexpr int off = 0;
    if (setsockopt(sock, IPPROTO_IP, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
        logger::warn("Failed to clear IPV6_V6ONLY of {} port.", name);
    }
    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger::error("Failed to bind {} socket: {}.", name, std::strerror(errno));
        close(sock);
        return -1;
    }
    logger::info("{} port listening on {}.", name, *reinterpret_cast<const sockaddr*>(&addr));
    return sock;
}

static void control_port(std::stop_source stop_source, int sock) {
    std::byte buffer[128];
    sockaddr_storage client_addr{};
    while (true) {
        socklen_t client_addr_len = sizeof(client_addr);
        const ssize_t recv_len = recvfrom(sock, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (recv_len < 1) [[unlikely]] {
            if (recv_len == 0) [[unlikely]] {
                logger::warn("Received empty packet on control port from {}.",
                    *reinterpret_cast<sockaddr*>(&client_addr));
            } else if (errno != EINTR) {
                logger::warn("recvfrom failed on control port: {}.", std::strerror(errno));
            }
            continue;
        }
        switch (static_cast<unsigned char>(buffer[0])) {
            case 0: { // kill
                std::ignore = stop_source.request_stop();
                close(sock);
                return;
            }
            case 1: { // new session token
                // format: <1><human_players: 1><ai_players: 1><keys: human_players * 4>
                constexpr std::size_t header_size = 3;
                if (recv_len < header_size) {
                    goto invalid_format;
                }
                const auto human_players = static_cast<unsigned char>(buffer[1]),
                    ai_players = static_cast<unsigned char>(buffer[2]);
                if (recv_len < header_size + human_players * sizeof(snakeio::key_t)) {
                    goto invalid_format;
                }
                // This is defined because
                // 1. key_t is an implicit lifetime type
                // 2. storage is given by an array of unsigned char
                // 3. key_t is an array of std::byte, which is guaranteed to have an alignment of 1
                const auto keys = reinterpret_cast<const snakeio::key_t*>(buffer + header_size);
                session_snapshot snapshot;
                game_.generate_session(snapshot, human_players, ai_players, {keys, human_players});
                const auto result = game_.add_session(snapshot);
                if (result.has_value()) {
                    logger::debug("New session ID {}.", result.value());
                    snapshot.id = result.value();
                    // TODO: send session info back
                    sendto(sock, &result.value(), sizeof(id_t), 0,
                        reinterpret_cast<const sockaddr*>(&client_addr), client_addr_len);
                } else {
                    std::string_view error;
                    switch (result.error()) {
                        using enum game::add_session_error;
                        case no_memory:
                            error = "no memory";
                            break;
                        case too_many_players:
                            error = "too many players";
                            break;
                        default:
                            error = "unknown error";
                            break;
                    }
                    logger::warn("Failed to create new session: {}.", error);
                }
                break;
            }
            default: {
                logger::warn("Received unknown command on control port.");
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
        .sin6_port = htons(50001),
        .sin6_addr = in6addr_loopback
    });
    const int data_sock = open_port("data", {
        .sin6_family = AF_INET6,
        .sin6_port = htons(50002),
        .sin6_addr = in6addr_any
    });
    if (control_sock < 0 || data_sock < 0) {
        return EXIT_FAILURE;
    }
    std::stop_source stop_source;
    std::jthread control_thread(control_port, stop_source, control_sock),
        data_thread(data_port, stop_source.get_token(), data_sock);
    close(control_sock); close(data_sock);
}