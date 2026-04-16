#include <tests/crypt.hpp>
#include <crypt/core.hpp>

namespace snakeio::test::crypt {
    void quarter_round(
        std::uint_least32_t& a, std::uint_least32_t& b, std::uint_least32_t& c, std::uint_least32_t& d) noexcept {
        snakeio::crypt::quarter_round(a, b, c, d);
    }

    std::array<std::uint_least32_t, 16> chacha20_block(
        const key_t& key, std::uint_least32_t counter, const_nonce_view nonce) noexcept {
        return snakeio::crypt::chacha20_block(key, counter, nonce);
    }

    void chacha20_encrypt(const key_t& key, std::uint_least32_t counter, const_nonce_view nonce,
        std::span<std::byte> text) noexcept {
        snakeio::crypt::chacha20_encrypt(key, counter, nonce, text);
    }

    void poly1305_mac(tag_view out, std::span<const std::byte> text, const key_t& key) noexcept {
        snakeio::crypt::poly1305_mac(out, text, key);
    }

    key_t poly1305_key_gen(const key_t& key, const_nonce_view nonce) noexcept {
        return snakeio::crypt::poly1305_key_gen(key, nonce);
    }

}