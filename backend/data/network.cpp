#include "network.hpp"
#include <logger.hpp>
#include <cstring>

using namespace snakeio;

udp_port::udp_port(std::string name, const sockaddr_in6& addr):
    name_(std::move(name)),
    sock_(socket(AF_INET6, SOCK_DGRAM, 0)) {
    if (sock_ < 0) [[unlikely]] {
        raise("Failed to create socket: {}.", std::strerror(errno));
    }
    constexpr int off = 0;
    if (setsockopt(sock_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0) [[unlikely]] {
        log(logger::warn, "Failed to clear IPV6_V6ONLY: {}.", std::strerror(errno));
    }
    if (bind(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) [[unlikely]] {
        raise("Failed to bind socket: {}.", std::strerror(errno));
    }
    constexpr timeval read_timeout{.tv_usec = std::chrono::microseconds(game_tick_rate).count()};
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));
    log(logger::info, "Listening on {}.", *reinterpret_cast<const sockaddr_storage*>(&addr));
}

udp_port::recv_result udp_port::recv(std::span<std::byte> buffer) const {
    recv_result out{};
    socklen_t client_addr_len = sizeof(out.client_addr);
    out.len = recvfrom(sock_, buffer.data(), buffer.size(), 0,
                       reinterpret_cast<sockaddr*>(&out.client_addr), &client_addr_len);
    if (out.len == -1 && errno != EAGAIN && errno != EWOULDBLOCK) [[unlikely]] {
        log(logger::warn, "recvfrom failed: {}.", std::strerror(errno));
    }
    return out;
}

void udp_port::send(const sockaddr_storage &addr, std::span<std::byte> buffer) const {
    const ssize_t res = sendto(sock_, buffer.data(), buffer.size(), 0,
                               reinterpret_cast<const sockaddr*>(&addr), sizeof(sockaddr_storage));
    if (res == -1) [[unlikely]] {
        log(logger::warn, "sendto failed: {}.", std::strerror(errno));
    }
}