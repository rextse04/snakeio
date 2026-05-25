#pragma once
#include <iostream>
#include <print>
#include <span>
#include <string>

namespace snakeio::logger {
    class logger {
    public:
#ifdef NDEBUG
        inline static int level = 1;
#else
        inline static int level = 0;
#endif
    private:
        std::ostream& os_;
        const char* prefix_;
        int level_;
    public:
        constexpr logger(std::ostream& os, const char* prefix, int level) noexcept :
            os_(os), prefix_(prefix), level_(level) {}
        void operator()(std::string_view msg) const {
            if (level_ < level) return;
            // must be one call because the logger is used by multiple threads
            std::println(os_, "{}{}", prefix_, msg);
        }
        template <typename... Args>
        void operator()(std::format_string<Args...> fmt, Args&&... args) const {
            operator()(std::format(fmt, std::forward<Args>(args)...));
        }
    };

    inline void print_packet(const logger& logger, std::span<const std::byte> packet) {
        auto str = std::format("{} bytes received:", packet.size());
        for (std::byte byte : packet) {
            str.append(std::format(" {}", static_cast<unsigned char>(byte)));
        }
        logger(str);
    }

    inline constexpr logger
        debug{std::cout, "[DEBUG] ", 0},
        info{std::cout, "\033[34m[INFO]\033[0m ", 1},
        warn{std::cout, "\033[33m[WARN]\033[0m ", 2},
        error{std::cout, "\033[31m[ERROR]\033[0m ", 3};
}
