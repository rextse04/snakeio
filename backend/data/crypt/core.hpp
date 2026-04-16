#pragma once
#include <config.hpp>
#include <cstdint>
#include <span>
#include <compatibility.hpp>

#ifdef SNAKEIO_CRYPT_DEVICE_ONLY
#define SNAKEIO_CRYPT_API __device__
#else
#define SNAKEIO_CRYPT_API __host__ __device__
#endif

namespace snakeio::crypt {
    SNAKEIO_CRYPT_API void quarter_round(
        std::uint_least32_t& a, std::uint_least32_t& b, std::uint_least32_t& c, std::uint_least32_t& d) noexcept;
    SNAKEIO_CRYPT_API std::array<std::uint_least32_t, 16> chacha20_block(
        const key_t& key, std::uint_least32_t counter, std::span<const std::byte, 12> nonce) noexcept;
    SNAKEIO_CRYPT_API void chacha20_encrypt(const key_t& key, std::uint_least32_t counter,
        std::span<const std::byte, 12> nonce, std::span<std::byte> text) noexcept;
    SNAKEIO_CRYPT_API void poly1305_mac(std::span<std::byte, 16> out,
        std::span<const std::byte> text, const key_t& key) noexcept;
    SNAKEIO_CRYPT_API key_t poly1305_key_gen(
        const key_t& key, std::span<const std::byte, 12> nonce) noexcept;
}

