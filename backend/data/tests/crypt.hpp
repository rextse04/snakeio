#pragma once
#include <packet.hpp>
#include <cstdint>

namespace snakeio::test::crypt {
    void quarter_round(
        std::uint_least32_t& a, std::uint_least32_t& b, std::uint_least32_t& c, std::uint_least32_t& d) noexcept;
    std::array<std::uint_least32_t, 16> chacha20_block(
        const key_t& key, std::uint_least32_t counter, const_nonce_view nonce) noexcept;
    void chacha20_encrypt(const key_t& key, std::uint_least32_t counter, const_nonce_view nonce,
        std::span<std::byte> text) noexcept;
    void poly1305_mac(tag_view out, std::span<const std::byte> text, const key_t& key) noexcept;
    key_t poly1305_key_gen(const key_t& key, const_nonce_view nonce) noexcept;
    void encrypt_packet(std::span<std::byte> packet, const key_t& key) noexcept;
    bool verify_decrypt_packet(std::span<std::byte> packet, const key_t& key) noexcept;
}