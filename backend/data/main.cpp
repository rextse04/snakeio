#include <game.hpp>
#include <logger.hpp>
#include <network.hpp>
#include <utils.hpp>
#include <benchmark.hpp>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <thread>
#include <stop_token>
#include <utility>
#include <span>
#include <chrono>
#include <new>

using namespace snakeio;

namespace snakeio {
     void control_port(game& game, std::stop_source stop_source) {
        constexpr std::size_t ctl_new_session_pkt = 1 + 5 + 1 + 1 + 4 + game_max_players * sizeof(snakeio::key_t);
        std::byte buffer[ctl_new_session_pkt]{};
        while (true) {
            const auto [client_addr, recv_len] = game.control_port_.recv(buffer);
            if (recv_len == -1) continue;
            if (recv_len == 0) {
                game.control_port_.log(logger::warn, "Received empty packet from {}.", client_addr);
                continue;
            }
            switch (static_cast<unsigned char>(buffer[0])) {
                case 0: { // kill
                    game.control_port_.log(logger::info, "Received kill command.");
                    (void) stop_source.request_stop();
                    return;
                }
                case 1: { // new session token
                    constexpr std::size_t header_size = 1 + 5 + 1 + 1 + 4;
                    if (recv_len < header_size) {
                        goto invalid_format;
                    }
                    const std::string_view token(reinterpret_cast<char*>(buffer + 1), 5);
                    const auto human_players = static_cast<unsigned char>(buffer[6]),
                        ai_players = static_cast<unsigned char>(buffer[7]);
                    const tick_t max_tick = load_32(std::span<const std::byte, 4>(buffer + 8, 4));
                    if (recv_len < header_size + human_players * sizeof(snakeio::key_t)) {
                        goto invalid_format;
                    }
                    // This is defined because
                    // 1. key_t is an implicit lifetime type
                    // 2. storage is given by an array of unsigned char
                    // 3. key_t is an array of std::byte, which is guaranteed to have an alignment of 1
                    const auto keys = std::launder(reinterpret_cast<const snakeio::key_t*>(buffer + header_size));
                    const auto id = game.add_session(human_players, ai_players, max_tick, std::span(keys, human_players));
                    if (id.has_value()) {
                        game.control_port_.log(logger::debug,
                            "Session token {} mapped to session ID {}.", token, id.value());
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
                            case max_tick_too_big: {
                                error = "max tick too big";
                                break;
                     		        }
                            case unknown_error: {
                                error = "unknown error";
                                break;
                            }
                            default: std::unreachable();
                        }
                        game.control_port_.log(logger::warn,
                            "Failed to create new session for token {}: {}.", token, error);
                        buffer[6] = static_cast<std::byte>(id.error());
                    }
                    game.control_port_.send(client_addr, std::span(buffer, header_size + 4));
                    break;
                }
                default: {
                    game.control_port_.log(logger::warn, "Received unknown command from {}", client_addr);
                    logger::print_packet(logger::debug, std::span(buffer, recv_len));
                    break;
                }
            }
            continue;
            invalid_format:
            game.control_port_.log(logger::warn, "Received invalid command format from {}", client_addr);
            logger::print_packet(logger::debug, std::span(buffer, recv_len));
        }
    }
}
namespace {
    void data_port(game& game, std::stop_token stop_token) {
        game.port(std::move(stop_token));
    }

    void game_loop(game& game, std::stop_token stop_token) noexcept {
        auto next_tick = game::clock::now();
        while (!stop_token.stop_requested()) {
            {
#ifdef SNAKEIO_BENCHMARK
                benchmarker bencher(game.tick_bench, game.session_manager().activated_slots());
#endif
                game.tick(stop_token);
            }
            next_tick += game_tick_rate;
            std::this_thread::sleep_until(next_tick);
        }
    }
}

int main() {
    game game;
    std::stop_source stop_source;
    std::jthread control_thread(control_port, std::ref(game), stop_source),
        data_thread(data_port, std::ref(game), stop_source.get_token()),
        game_thread(game_loop, std::ref(game), stop_source.get_token());
}