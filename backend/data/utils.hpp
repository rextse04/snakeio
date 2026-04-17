#pragma once
#include <cstdint>
#include <concepts>
#include <span>
#include <bit>
#include <array>
#include <cmath>
#include <compatibility.hpp>

#define atomic_align(type) alignas(std::atomic_ref<type>::required_alignment) type

namespace snakeio {
    __host__ __device__ constexpr std::uint_least16_t load_16(std::span<const std::byte, 2> bytes) noexcept {
        return (static_cast<std::uint_least16_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least16_t>(bytes[1]) << 8);
    }
    __host__ __device__ constexpr void store_16(std::span<std::byte, 2> out, std::uint_least16_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
    }

    __host__ __device__ constexpr std::uint_least32_t load_32(std::span<const std::byte, 4> bytes) noexcept {
        return (static_cast<std::uint_least32_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least32_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least32_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least32_t>(bytes[3]) << 24);
    }
    __host__ __device__ constexpr void store_32(std::span<std::byte, 4> out, std::uint_least32_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
        out[2] = static_cast<std::byte>(value >> 16);
        out[3] = static_cast<std::byte>(value >> 24);
    }

    __host__ __device__ constexpr std::uint_least64_t load_64(std::span<const std::byte, 8> bytes) noexcept {
        return (static_cast<std::uint_least64_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least64_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least64_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least64_t>(bytes[3]) << 24) |
               (static_cast<std::uint_least64_t>(bytes[4]) << 32) |
               (static_cast<std::uint_least64_t>(bytes[5]) << 40) |
               (static_cast<std::uint_least64_t>(bytes[6]) << 48) |
               (static_cast<std::uint_least64_t>(bytes[7]) << 56);
    }
    __host__ __device__ constexpr void store_64(std::span<std::byte, 8> out, std::uint_least64_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
        out[2] = static_cast<std::byte>(value >> 16);
        out[3] = static_cast<std::byte>(value >> 24);
        out[4] = static_cast<std::byte>(value >> 32);
        out[5] = static_cast<std::byte>(value >> 40);
        out[6] = static_cast<std::byte>(value >> 48);
        out[7] = static_cast<std::byte>(value >> 56);
    }

    __host__ __device__ constexpr float load_float32(std::span<const std::byte, 4> bytes) noexcept {
        using enum std::endian;
        if constexpr (sizeof(float) == 4) {
            if constexpr (native == little) {
                std::array<std::byte, 4> bytes_ = {bytes[0], bytes[1], bytes[2], bytes[3]};
                return std::bit_cast<float>(bytes_);
            } else if constexpr (native == big) {
                std::array<std::byte, 4> bytes_ = {bytes[3], bytes[2], bytes[1], bytes[0]};
                return std::bit_cast<float>(bytes_);
            }
        }
    }
    __host__ __device__ constexpr void store_float32(std::span<std::byte, 4> out, float value) noexcept {
        using enum std::endian;
        static_assert(sizeof(float) == 4);
        static_assert(native == little || native == big);
        auto bytes = std::bit_cast<std::array<std::byte, 4>>(value);
        if constexpr (native == little) {
            out[0] = bytes[0];
            out[1] = bytes[1];
            out[2] = bytes[2];
            out[3] = bytes[3];
        } else if constexpr (native == big) {
            out[0] = bytes[3];
            out[1] = bytes[2];
            out[2] = bytes[1];
            out[3] = bytes[0];
        }
    }

    template <std::floating_point T>
    __host__ __device__ constexpr T angle_diff(T a, T b) noexcept {
        return std::remainder(a-b, static_cast<T>(M_PI * 2));
    }
}