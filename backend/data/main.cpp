#include "game.hpp"
#include "logger.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string_view>
#include <thread>
#include <stop_token>
#include <utility>
#include <span>
#include <cstring>

using namespace snakeio;

game game_;

[[nodiscard]] int open_port(std::string_view name, std::uint16_t port) {
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        logger::error("Failed to create control socket.");
        return sock;
    }
    sockaddr_in addr{
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger::error("Failed to bind {} socket.", name);
        return sock;
    }
    logger::info("{} port listening on 127.0.0.1:50001.", name);
    return sock;
}

void control_port(std::stop_source stop_source, int sock) {
    alignas(4) char buffer[packet_max_size];
    sockaddr_in client_addr;
    while (true) {
        socklen_t client_addr_len = sizeof(client_addr);
        const ssize_t recv_len = recvfrom(sock, buffer, sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (recv_len < 1) continue;
        switch (buffer[0]) {
            case 0: { // kill
                std::ignore = stop_source.request_stop();
                close(sock);
                return;
            }
            case 1: { // new session token
                // format: <1><human_players: 1><ai_players: 1><padding: 1><keys: human_players * 4>
                // padding for 4-byte alignment
                if (recv_len < 4) {
                    goto invalid_format;
                }
                const auto human_players = static_cast<std::uint8_t>(buffer[1]),
                    ai_players = static_cast<std::uint8_t>(buffer[2]);
                if (recv_len < 4 + human_players * sizeof(snakeio::key_t)) {
                    goto invalid_format;
                }
                // implicitly start lifetime of human_players count of key_t
                // replace with std::start_lifetime_as when implemented
                const auto keys = static_cast<snakeio::key_t*>(
                    std::memmove(buffer + 4, buffer + 4, sizeof(snakeio::key_t) * human_players));
                game_session session;
                game_.generate_session(session, human_players, ai_players, {keys, human_players});
                const auto result = game_.add_session(session);
                if (result.has_value()) {
                    logger::debug("New session ID {}.", result.value());
                    session.id = result.value();
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
                    logger::warn("Failed to create new session: {}", error);
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
        logger::debug("{} bytes received: {::0>8b}", recv_len, std::span(buffer, recv_len));
    }
}

void data_port(std::stop_token stop_token, int sock) {
    timeval read_timeout{.tv_sec = 0, .tv_usec = 10};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));
    game_.bind(std::move(stop_token), sock);
}

int main() {
    const int control_sock = open_port("control", 50001);
    const int data_sock = open_port("data", 50002);
    if (control_sock < 0 || data_sock < 0) {
        return EXIT_FAILURE;
    }
    std::stop_source stop_source;
    std::jthread control_thread(control_port, stop_source, control_sock),
        data_thread(data_port, stop_source.get_token(), data_sock);
}