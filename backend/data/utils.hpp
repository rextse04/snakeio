#pragma once
#include <cstdint>
#include <span>

namespace snakeio {
    inline std::uint_least32_t load_32(std::span<const std::byte, 4> bytes) noexcept {
        return (static_cast<std::uint_least32_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least32_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least32_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least32_t>(bytes[3]) << 24);
    }
    inline void store_32(std::span<std::byte, 4> bytes, std::uint_least32_t value) noexcept {
        bytes[0] = static_cast<std::byte>(value >> 0);
        bytes[1] = static_cast<std::byte>(value >> 8);
        bytes[2] = static_cast<std::byte>(value >> 16);
        bytes[3] = static_cast<std::byte>(value >> 24);
    }
    inline std::uint_least64_t load_64(std::span<const std::byte, 8> bytes) noexcept {
        return (static_cast<std::uint_least64_t>(bytes[0]) << 0) |
               (static_cast<std::uint_least64_t>(bytes[1]) << 8) |
               (static_cast<std::uint_least64_t>(bytes[2]) << 16) |
               (static_cast<std::uint_least64_t>(bytes[3]) << 24) |
               (static_cast<std::uint_least64_t>(bytes[4]) << 32) |
               (static_cast<std::uint_least64_t>(bytes[5]) << 40) |
               (static_cast<std::uint_least64_t>(bytes[6]) << 48) |
               (static_cast<std::uint_least64_t>(bytes[7]) << 56);
    }
    inline void store_64(std::span<std::byte, 8> bytes, std::uint_least64_t value) noexcept {
        bytes[0] = static_cast<std::byte>(value >> 0);
        bytes[1] = static_cast<std::byte>(value >> 8);
        bytes[2] = static_cast<std::byte>(value >> 16);
        bytes[3] = static_cast<std::byte>(value >> 24);
        bytes[4] = static_cast<std::byte>(value >> 32);
        bytes[5] = static_cast<std::byte>(value >> 40);
        bytes[6] = static_cast<std::byte>(value >> 48);
        bytes[7] = static_cast<std::byte>(value >> 56);
    }
}