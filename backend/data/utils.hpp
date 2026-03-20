#pragma once
#include <cstdint>
#include <span>
#include <bit>
#include <algorithm>
#include <array>
#include <concepts>
#include <cmath>

namespace snakeio {
    constexpr std::uint_least16_t load_16(std::span<const std::byte, 2> bytes) noexcept {
        return (static_cast<std::uint_least16_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least16_t>(bytes[1]) << 8);
    }
    constexpr void store_16(std::span<std::byte, 2> out, std::uint_least16_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
    }

    constexpr std::uint_least32_t load_32(std::span<const std::byte, 4> bytes) noexcept {
        return (static_cast<std::uint_least32_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least32_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least32_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least32_t>(bytes[3]) << 24);
    }
    constexpr void store_32(std::span<std::byte, 4> out, std::uint_least32_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
        out[2] = static_cast<std::byte>(value >> 16);
        out[3] = static_cast<std::byte>(value >> 24);
    }

    constexpr std::uint_least64_t load_64(std::span<const std::byte, 8> bytes) noexcept {
        return (static_cast<std::uint_least64_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least64_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least64_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least64_t>(bytes[3]) << 24) |
               (static_cast<std::uint_least64_t>(bytes[4]) << 32) |
               (static_cast<std::uint_least64_t>(bytes[5]) << 40) |
               (static_cast<std::uint_least64_t>(bytes[6]) << 48) |
               (static_cast<std::uint_least64_t>(bytes[7]) << 56);
    }
    constexpr void store_64(std::span<std::byte, 8> out, std::uint_least64_t value) noexcept {
        out[0] = static_cast<std::byte>(value >> 0);
        out[1] = static_cast<std::byte>(value >> 8);
        out[2] = static_cast<std::byte>(value >> 16);
        out[3] = static_cast<std::byte>(value >> 24);
        out[4] = static_cast<std::byte>(value >> 32);
        out[5] = static_cast<std::byte>(value >> 40);
        out[6] = static_cast<std::byte>(value >> 48);
        out[7] = static_cast<std::byte>(value >> 56);
    }

    constexpr float load_float32(std::span<const std::byte, 4> bytes) noexcept {
        using enum std::endian;
        if constexpr (sizeof(float) == 4) {
            if constexpr (native == little) {
                std::array<std::byte, 4> bytes_;
                std::ranges::copy(bytes, bytes_.begin());
                return std::bit_cast<float>(bytes_);
            } else if constexpr (native == big) {
                std::array<std::byte, 4> bytes_;
                std::ranges::reverse_copy(bytes, bytes_.begin());
                return std::bit_cast<float>(bytes_);
            }
        }
    }
    constexpr void store_float32(std::span<std::byte, 4> out, float value) noexcept {
        using enum std::endian;
        if constexpr (sizeof(float) == 4) {
            if constexpr (native == little) {
                std::ranges::copy(std::bit_cast<std::array<std::byte, 4>>(value), out.begin());
            } else if constexpr (native == big) {
                std::ranges::reverse_copy(std::bit_cast<std::array<std::byte, 4>>(value), out.begin());
            }
        }
    }

    template <std::floating_point T>
    constexpr T angle_diff(T a, T b) noexcept {
        return std::remainder(a-b, static_cast<T>(M_PI * 2));
    }
}