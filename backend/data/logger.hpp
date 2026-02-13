#pragma once
#include <iostream>
#include <print>
#include <syncstream>

namespace snakeio {
    struct logger {
        template <typename... Args>
        static void debug(std::format_string<Args...> fmt, Args&&... args) {
#ifndef NDEBUG
            std::osyncstream oss(std::cout);
            std::print(oss, "[DEBUG] ");
            std::println(oss, fmt, std::forward<Args>(args)...);
#endif
        }
        template <typename... Args>
        static void info(std::format_string<Args...> fmt, Args&&... args) {
            std::osyncstream oss(std::cout);
            std::print(oss, "\033[34m[INFO]\033[0m ");
            std::println(oss, fmt, std::forward<Args>(args)...);
        }
        template <typename... Args>
        static void warn(std::format_string<Args...> fmt, Args&&... args) {
            std::osyncstream oss(std::cout);
            std::print(oss, "\033[33m[WARN]\033[0m ");
            std::println(oss, fmt, std::forward<Args>(args)...);
        }
        template <typename... Args>
        static void error(std::format_string<Args...> fmt, Args&&... args) {
            std::osyncstream oss(std::cout);
            std::print(oss, "\033[31m[ERROR]\033[0m ");
            std::println(oss, fmt, std::forward<Args>(args)...);
        }
    };
}