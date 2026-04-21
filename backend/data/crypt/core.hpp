#pragma once
#include <config.hpp>
#include <cstdint>
#include <span>
#include <compatibility.hpp>

namespace snakeio::crypt {
    __host__ __device__ void quarter_round(
        std::uint_least32_t& a, std::uint_least32_t& b, std::uint_least32_t& c, std::uint_least32_t& d) noexcept;
    __host__ __device__ std::array<std::uint_least32_t, 16> chacha20_block(
        const key_t& key, std::uint_least32_t counter, std::span<const std::byte, 12> nonce) noexcept;
    __host__ __device__ void chacha20_encrypt(const key_t& key, std::uint_least32_t counter,
        std::span<const std::byte, 12> nonce, std::span<std::byte> text) noexcept;
    __host__ __device__ void poly1305_mac(std::span<std::byte, 16> out,
        std::span<const std::byte> text, const key_t& key) noexcept;
    __host__ __device__ key_t poly1305_key_gen(
        const key_t& key, std::span<const std::byte, 12> nonce) noexcept;
}