#pragma once
#include <config.hpp>
#include <chrono>
#include <concepts>
#include <type_traits>
#include <iostream>
#include <string>
#include <print>

namespace snakeio {
    using benchmark_clock = std::chrono::high_resolution_clock;

    template <typename B>
    concept benchmark_item = requires(B b, const B cb, benchmark_clock::duration time, std::ostream& os) {
        { b.time } -> std::convertible_to<benchmark_clock::duration&>;
        { B::print_header_to_csv(os) };
        { cb.print_to_csv(os) };
    };

    template <benchmark_item B>
    class benchmarker;

    template <benchmark_item B>
    class benchmark {
        friend class benchmarker<B>;

    private:
        std::ostream& os_;

        void commit_row(B&& row) {
            row.print_to_csv(os_);
            os_.flush();
        }

    public:
        explicit benchmark(std::ostream& os) noexcept: os_(os) {
            B::print_header_to_csv(os_);
            os_.flush();
        }
    };

    template <benchmark_item B>
    class benchmarker {
    private:
        benchmark<B>& base_;
        benchmark_clock::time_point start_;

    public:
        B item;

        template <typename... Args>
        requires (std::is_constructible_v<B, Args&&...>)
        explicit benchmarker(benchmark<B>& base, Args&&... args)
            noexcept(std::is_nothrow_constructible_v<B, Args&&...>):
            base_(base),
            item(std::forward<Args>(args)...),
            start_(benchmark_clock::now()) {}

        ~benchmarker() {
            item.time = benchmark_clock::now() - start_;
            base_.commit_row(std::move(item));
        }
    };

    struct tick_benchmark_item {
        id_t sessions;
        benchmark_clock::duration time;

        static void print_header_to_csv(std::ostream& os) {
            os << "sessions,time (ns)\n";
        }
        void print_to_csv(std::ostream& os) const {
            const auto time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time).count();
            std::println(os, "{},{}", sessions, time_ns);
        }
    };
    static_assert(benchmark_item<tick_benchmark_item>);
}
