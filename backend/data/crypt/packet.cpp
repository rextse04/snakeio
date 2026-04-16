#include <crypt/core.hpp>
#include <config.hpp>
#include <packet.hpp>
#include <utils.hpp>
#include <cstddef>
#include <algorithm>
#include <compatibility.hpp>


__host__ __device__ void snakeio::data_packet::encrypt(const key_t& key) noexcept {
    using namespace snakeio::crypt;
    const auto otk = poly1305_key_gen(key, nonce());
    chacha20_encrypt(key, 1, nonce(), text());
    store_64(tag().subspan<0, 8>(), aad_size);
    store_64(tag().subspan<8, 8>(), text().size());
    poly1305_mac(tag(), *this, otk);
}

__host__ __device__ static bool safe_tag_equal(snakeio::const_volatile_tag_view a, snakeio::const_volatile_tag_view b) noexcept {
    volatile std::byte out{};
    for (std::size_t i = 0; i < snakeio::tag_view::extent; ++i) {
        out = out | (a[i] ^ b[i]);
    }
    return out == std::byte(0);
}

__host__ __device__ snakeio::data_packet::verify_result snakeio::data_packet::verify(const key_t& key) noexcept {
    using namespace snakeio::crypt;
    using enum verify_result;
    if (size() <= header_size) [[unlikely]] return too_short;
    if (size() % data_packet_align != 0) [[unlikely]] return invalid_size;
    const auto otk = poly1305_key_gen(key, nonce());
    tag_t tag, packet_tag;
    std::ranges::copy(this->tag(), packet_tag.begin());
    store_64(this->tag().subspan<0, 8>(), 16);
    store_64(this->tag().subspan<8, 8>(), text().size());
    poly1305_mac(tag, *this, otk);
    return safe_tag_equal(tag, packet_tag) ? ok : invalid_tag;
}

__host__ __device__ void snakeio::data_packet::decrypt(const key_t& key) noexcept {
    using namespace snakeio::crypt;
    chacha20_encrypt(key, 1, nonce(), text());
}

