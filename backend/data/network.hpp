#pragma once
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
struct std::formatter<sockaddr, char> {
    template <typename ParseContext>
    constexpr ParseContext::iterator parse(ParseContext& ctx) {
        return ctx.begin();
    }
    template <typename FmtContext>
    constexpr FmtContext::iterator format(const sockaddr& addr, FmtContext& ctx) const {
        char host[NI_MAXHOST], service[NI_MAXSERV];
        if (getnameinfo(&addr, sizeof(addr),
            host, sizeof(host),
            service, sizeof(service),
            NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            return std::format_to(ctx.out(), "[{}]:{}", host, service);
        } else {
            return std::format_to(ctx.out(), "<invalid address>");
        }
    }
};
