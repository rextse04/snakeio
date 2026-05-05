#pragma once
#include <iostream>
#include <print>
#include <span>
#include <syncstream>

namespace snakeio::logger {
    class logger {
    private:
        std::ostream& os_;
        const char* prefix_;

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

#ifndef NDEBUG
    class debug_logger {
    private:
        std::ostream& os_;
        const char* prefix_;
        friend void print_packet(const debug_logger&, std::span<const std::byte>);
    public:
        constexpr debug_logger(std::ostream& os, const char* prefix) noexcept :
            os_(os), prefix_(prefix) {}
        template <typename... Args>
        void operator()(std::format_string<Args...> fmt, Args&&... args) const {
            std::osyncstream oss(os_);
            oss << prefix_;
            std::println(oss, fmt, std::forward<Args>(args)...);
        }
    };

    inline void print_packet(const debug_logger& logger, std::span<const std::byte> packet) {
        std::osyncstream oss(logger.os_);
        std::print(oss, "{}{} bytes received:", logger.prefix_, packet.size());
        for (std::byte byte : packet) {
            std::print(oss, " {}", static_cast<unsigned char>(byte));
        }
        std::println(oss);
    }

    inline constexpr debug_logger debug{std::cout, "[DEBUG] "};
#else
    class debug_logger {
    public:
        template <typename... Args>
        constexpr void operator()(std::format_string<Args...>, Args&&...) const noexcept {}

    };

    inline void print_packet(const debug_logger&, std::span<const std::byte>) noexcept {}

    inline constexpr debug_logger debug{};
#endif

    inline constexpr logger
        info{std::cout, "\033[34m[INFO]\033[0m "},
        warn{std::cout, "\033[33m[WARN]\033[0m "},
        error{std::cout, "\033[31m[ERROR]\033[0m "};
}
