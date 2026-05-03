#pragma once
#include <cstring>
#include <logger.hpp>
#include <span>
#include <cstring>
#include <format>

#if __has_include(<sys/socket.h>) && __has_include(<netinet/in.h>) && __has_include(<netdb.h>)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#elif defined(_WIN16) || defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
#else
    #error "Unsupported platform. Link with a library that provides POSIX sockets API and include the headers in this file."
#endif

template <>
struct std::formatter<sockaddr_storage, char> {
    template <typename ParseContext>
    constexpr ParseContext::iterator parse(ParseContext& ctx) {
        return ctx.begin();
    }
    template <typename FmtContext>
    constexpr FmtContext::iterator format(const sockaddr_storage& addr, FmtContext& ctx) const {
        char host[NI_MAXHOST], service[NI_MAXSERV];
        if (getnameinfo(reinterpret_cast<const sockaddr*>(&addr), sizeof(sockaddr_storage),
            host, sizeof(host),
            service, sizeof(service),
            NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            return std::format_to(ctx.out(), "[{}]:{}", host, service);
        } else {
            return std::format_to(ctx.out(), "<invalid address>");
        }
    }
};

namespace snakeio {
    [[nodiscard]] inline int open_port(std::string_view name, const sockaddr_in6& addr) {
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

    inline ssize_t sendto(int sock, std::span<std::byte> buffer, const sockaddr_storage& addr) {
        const ssize_t res = sendto(sock, buffer.data(), buffer.size(), 0,
            reinterpret_cast<const sockaddr*>(&addr), sizeof(sockaddr_storage));
        if (res == -1) [[unlikely]] {
            logger::warn("sendto failed: {}.", std::strerror(errno));
        }
        return res;
    }
}