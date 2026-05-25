#pragma once
#include <config.hpp>
#include <logger.hpp>
#include <span>
#include <format>
#include <string>
#include <stdexcept>

#if __has_include(<sys/socket.h>) &&\
    __has_include(<netinet/in.h>) &&\
    __has_include(<arpa/inet.h>) &&\
    __has_include(<netdb.h>) &&\
    __has_include(<unistd.h>)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
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
    class network_error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class udp_port {
    private:
        std::string name_;
        int sock_;
    public:
        explicit constexpr udp_port(std::string name) : name_(std::move(name)), sock_(0) {}
        udp_port(std::string name, const sockaddr_in6& addr);
        // not moved to .cpp to allow using the class without linking against it
        ~udp_port() noexcept { close(sock_); }

        constexpr const std::string& name() const noexcept { return name_; }
        constexpr int sock() const noexcept { return sock_; }

        template <typename... Args>
        std::string log_str(std::format_string<Args...> fmt, Args&&... args) const {
            return std::format("({}) {}", name_, std::format(fmt, std::forward<Args>(args)...));
        }
        template <typename... Args>
        void log(const logger::logger& logger, std::format_string<Args...> fmt, Args&&... args) const {
            logger(log_str(fmt, std::forward<Args>(args)...));
        }
        template <typename... Args>
        void raise(std::format_string<Args...> fmt, Args&&... args) const {
            throw network_error(log_str(fmt, std::forward<Args>(args)...));
        }

        struct recv_result {
            sockaddr_storage client_addr;
            ssize_t len;
        };
        [[nodiscard]] recv_result recv(std::span<std::byte> buffer) const;
        void send(const sockaddr_storage& addr, std::span<std::byte> buffer) const;
    };
}