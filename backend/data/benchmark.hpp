#pragma once
#include <config.hpp>
#include <chrono>
#include <concepts>
#include <type_traits>
#include <memory>
#include <iostream>
#include <vector>
#include <string>
#include <print>

namespace snakeio {
    using benchmark_clock = std::chrono::high_resolution_clock;

    template <typename B>
    concept benchmark_item = requires(B b, const B cb, benchmark_clock::duration time,
        typename std::vector<B>::allocator_type alloc, std::ostream& os) {
        { b.time } -> std::convertible_to<benchmark_clock::duration&>;
        { std::allocator_traits<decltype(alloc)>::construct(alloc, &b, std::move(b)) }; // move insertable
        { B::print_header_to_csv(os) };
        { cb.print_to_csv(os) };
    };
    template <benchmark_item B>
    class benchmarker;
    template <benchmark_item B>
    class benchmark : std::vector<B> {
        friend benchmarker<B>;
    private:
        std::ostream& os_;
    public:
        explicit benchmark(std::ostream& os) noexcept: os_(os) {}
        ~benchmark() {
            B::print_header_to_csv(os_);
            for (const B& b : *this) b.print_to_csv(os_);
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
            base_.push_back(std::move(item));
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