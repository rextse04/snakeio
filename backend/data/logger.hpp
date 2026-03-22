#pragma once
#include <iostream>
#include <print>
#include <syncstream>

namespace snakeio::logger {
    class logger {
    private:
        std::ostream& os_;
        const char* prefix_;

        friend void print_packet(const logger&, std::span<const std::byte>);
    public:
        constexpr logger(std::ostream& os, const char* prefix) noexcept :
            os_(os), prefix_(prefix) {}
        template <typename... Args>
        void operator()(std::format_string<Args...> fmt, Args&&... args) const {
            std::osyncstream oss(os_);
            oss << prefix_;
            std::println(oss, fmt, std::forward<Args>(args)...);
        }
    };

    inline constexpr logger
        debug{std::cout, "[DEBUG] "},
        info{std::cout, "\033[34m[INFO]\033[0m "},
        warn{std::cout, "\033[33m[WARN]\033[0m "},
        error{std::cout, "\033[31m[ERROR]\033[0m "};

    inline void print_packet(const logger& logger, std::span<const std::byte> packet) {
        logger("{} bytes received:", packet.size());
        for (std::byte byte : packet) {
            std::print(logger.os_, " {}", static_cast<unsigned char>(byte));
        }
        std::println(logger.os_);
    }
}