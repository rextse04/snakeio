#pragma once
#include <cstdint>
#include <concepts>

#ifdef __CUDACC__
#include <cuda/std/span>
#include <cuda/std/bit>
#include <cuda/std/algorithm>
#include <cuda/std/array>
#include <cuda/std/cmath>
#else
#include <span>
#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
#endif
#include <compatibility.hpp>

#define atomic_align(type) alignas(std::atomic_ref<type>::required_alignment) type

namespace snakeio {
    __host__ __device__ constexpr std::uint_least16_t load_16(stdc::span<const std::byte, 2> bytes) noexcept {
        return (static_cast<std::uint_least16_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least16_t>(bytes[1]) << 8);
    }
    __host__ __device__ constexpr void store_16(stdc::span<std::byte, 2> out, std::uint_least16_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
    }

    __host__ __device__ constexpr std::uint_least32_t load_32(stdc::span<const std::byte, 4> bytes) noexcept {
        return (static_cast<std::uint_least32_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least32_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least32_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least32_t>(bytes[3]) << 24);
    }
    __host__ __device__ constexpr void store_32(stdc::span<std::byte, 4> out, std::uint_least32_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
        out[2] = static_cast<std::byte>(value >> 16);
        out[3] = static_cast<std::byte>(value >> 24);
    }

    __host__ __device__ constexpr std::uint_least64_t load_64(stdc::span<const std::byte, 8> bytes) noexcept {
        return (static_cast<std::uint_least64_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least64_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least64_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least64_t>(bytes[3]) << 24) |
               (static_cast<std::uint_least64_t>(bytes[4]) << 32) |
               (static_cast<std::uint_least64_t>(bytes[5]) << 40) |
               (static_cast<std::uint_least64_t>(bytes[6]) << 48) |
               (static_cast<std::uint_least64_t>(bytes[7]) << 56);
    }
    __host__ __device__ constexpr void store_64(stdc::span<std::byte, 8> out, std::uint_least64_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
        out[2] = static_cast<std::byte>(value >> 16);
        out[3] = static_cast<std::byte>(value >> 24);
        out[4] = static_cast<std::byte>(value >> 32);
        out[5] = static_cast<std::byte>(value >> 40);
        out[6] = static_cast<std::byte>(value >> 48);
        out[7] = static_cast<std::byte>(value >> 56);
    }

    __host__ __device__ constexpr float load_float32(stdc::span<const std::byte, 4> bytes) noexcept {
        using enum std::endian;
        if constexpr (sizeof(float) == 4) {
            if constexpr (native == little) {
                stdc::array<std::byte, 4> bytes_;
                stdc::copy(bytes.begin(), bytes.end(), bytes_.begin());
                return std::bit_cast<float>(bytes_);
            } else if constexpr (native == big) {
                stdc::array<std::byte, 4> bytes_;
                stdc::reverse_copy(bytes.begin(), bytes.end(), bytes_.begin());
                return stdc::bit_cast<float>(bytes_);
            }
        }
    }
    __host__ __device__ constexpr void store_float32(stdc::span<std::byte, 4> out, float value) noexcept {
        using enum std::endian;
        static_assert(sizeof(float) == 4);
        static_assert(native == little || native == big);
        auto bytes = stdc::bit_cast<stdc::array<std::byte, 4>>(value);
        if constexpr (native == little) {
            stdc::copy(bytes.begin(), bytes.end(), out.begin());
        } else if constexpr (native == big) {
            stdc::reverse_copy(bytes.begin(), bytes.end(), out.begin());
        }
    }

    template <std::floating_point T>
    __host__ __device__ constexpr T angle_diff(T a, T b) noexcept {
        return stdc::remainder(a-b, static_cast<T>(M_PI * 2));
    }
}