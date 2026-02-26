#include <config.hpp>
#include <packet.hpp>
#include <utils.hpp>
#include <cstddef>

static void quarter_round(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d) noexcept {
    a += b; d ^= a; d = (d << 16) | (d >> 16);
    c += d; b ^= c; b = (b << 12) | (b >> 20);
    a += b; d ^= a; d = (d << 8) | (d >> 24);
    c += d; b ^= c; b = (b << 7) | (b >> 25);
}

static std::array<std::uint32_t, 16> chacha20_block(
    const snakeio::key_t& key, std::uint32_t counter, snakeio::const_nonce_view nonce) noexcept {
    // The state is initialized as follows:
    std::array<std::uint32_t, 16> out;
    out[0] = 0x61707865;
    out[1] = 0x3320646e;
    out[2] = 0x79622d32;
    out[3] = 0x6b206574;
    for (int i = 0; i < std::tuple_size_v<snakeio::key_t> / 4; ++i) {
        out[4 + i] = snakeio::load_32(std::span<const std::byte, 4>{key.begin() + i*4, 4});
    }
    out[12] = counter;
    for (int i = 0; i < snakeio::nonce_view::extent / 4; ++i) {
        out[13 + i] = snakeio::load_32(std::span<const std::byte, 4>{nonce.begin() + i*4, 4});
    }
    // Then we perform 20 rounds of the quarter round function.
    auto state = out;
    for (int i = 0; i < 10; ++i) {
        quarter_round(state[0], state[4], state[8], state[12]);
        quarter_round(state[1], state[5], state[9], state[13]);
        quarter_round(state[2], state[6], state[10], state[14]);
        quarter_round(state[3], state[7], state[11], state[15]);
        quarter_round(state[0], state[5], state[10], state[15]);
        quarter_round(state[1], state[6], state[11], state[12]);
        quarter_round(state[2], state[7], state[8], state[13]);
        quarter_round(state[3], state[4], state[9], state[14]);
    }
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] += state[i];
    }
    return out;
}

// text.size() must be a multiple of 64
static void chacha20_encrypt(const snakeio::key_t& key, std::uint32_t counter, snakeio::nonce_view nonce,
    std::span<std::byte> text) noexcept {
    [[assume(text.size() % 64 == 0)]];
    for (std::size_t i = 0; i < text.size() / 64; ++i) {
        const auto key_stream = chacha20_block(key, counter + i, nonce);
        const auto key_stream_bytes = reinterpret_cast<const std::byte*>(key_stream.data());
        for (std::size_t j = 0; j < 64; ++j) {
            text[i*64+j] ^= *(key_stream_bytes + j);
        }
    }
}

struct radix26 {
    using base_type = std::uint_least32_t;
    static constexpr int radix = 26;

    std::array<base_type, 5> limbs;

    static constexpr radix26 load(std::span<const std::byte, 16> bytes) noexcept {
        radix26 out;
        const std::uint_least64_t lo = snakeio::load_64(std::span<const std::byte, 8>(bytes.begin(), 8)),
            hi = snakeio::load_64(std::span<const std::byte, 8>(bytes.begin() + 8, 8));
        constexpr std::uint_least64_t mask = (base_type(1) << radix) - 1;
        out.limbs[0] = lo & mask;
        out.limbs[1] = (lo >> radix) & mask;
        constexpr int spill = radix - (64 - radix * 2);
        out.limbs[2] = (lo >> (2 * radix)) | ((hi & ((base_type(1) << spill) - 1)) << (radix - spill));
        out.limbs[3] = (hi >> spill) & mask;
        out.limbs[4] = hi >> (spill + radix);
        return out;
    }
    constexpr void store(snakeio::tag_view out) const noexcept {
        auto limb = limbs.begin();
        int pos = 0;
        for (std::byte& byte : out) {
            if (pos + 8 > radix) {
                const int spill = pos + 8 - radix;
                byte = static_cast<std::byte>((*limb >> pos) | (*(limb + 1) & ((base_type(1) << spill) - 1)));
                ++limb;
                pos = spill;
            } else {
                byte = static_cast<std::byte>(*limb >> pos);
                pos += 8;
            }
        }
    }

    constexpr void propagate_carry() noexcept {
        while (limbs[limbs.size() - 1] >> radix) {
            limbs[0] += (limbs[limbs.size() - 1] >> radix) * 5;
            limbs[limbs.size() - 1] &= (base_type(1) << radix) - 1;
            for (std::size_t i = 0; i < limbs.size() - 1; ++i) {
                limbs[i+1] += limbs[i] >> radix;
                limbs[i] &= (base_type(1) << radix) - 1;
            }
        }
    }
    constexpr radix26& operator+=(const radix26& other) noexcept {
        for (std::size_t i = 0; i < limbs.size(); ++i) {
            limbs[i] = limbs[i] + other.limbs[i];
        }
        return *this;
    }
    constexpr radix26& operator*=(const radix26& other) noexcept {
        std::array<std::uint_least64_t, 9> temp{};
        // schoolbook multiplication
        for (std::size_t i = 0; i < limbs.size(); ++i) {
            for (std::size_t j = 0; j < limbs.size(); ++j) {
                temp[i+j] += limbs[i] * other.limbs[j];
            }
        }
        // reduce temp modulo 2^130 - 5.
        for (std::size_t i = limbs.size(); i < temp.size(); ++i) {
            temp[i - limbs.size()] += 5 * temp[i];
        }
        do {
            temp[0] += (temp[limbs.size() - 1] >> radix) * 5;
            temp[limbs.size() - 1] &= (base_type(1) << radix) - 1;
            for (std::size_t i = 0; i < limbs.size() - 1; ++i) {
                temp[i+1] += temp[i] >> radix;
                temp[i] &= (base_type(1) << radix) - 1;
            }
        } while (temp[limbs.size() - 1] >> radix);
        for (std::size_t i = 0; i < limbs.size(); ++i) {
            limbs[i] = temp[i];
        }
        return *this;
    }
};

// text.size() must be a multiple of 16
static void poly1305_mac(snakeio::tag_view out, std::span<const std::byte> text, const snakeio::key_t& key) noexcept {
    auto r = radix26::load(std::span<const std::byte, 16>(key.begin(), 16)),
        s = radix26::load(std::span<const std::byte, 16>(key.begin() + 16, 16));
    // clamp
    r.limbs[0] &= 0x3ffffff;
    r.limbs[1] &= 0x3ffff03;
    r.limbs[2] &= 0x3ffc0ff;
    r.limbs[3] &= 0x3f03fff;
    r.limbs[4] &= 0x00fffff;
    radix26 a{};
    for (std::size_t i = 0; i < text.size() / 16; ++i) {
        auto block = radix26::load(std::span<const std::byte, 16>{text.begin() + i*16, 16});
        block.limbs[4] |= 1 << 24; // add the 1 bit
        a += block;
        a *= r;
    }
    a += s;
    a.propagate_carry();
    a.store(out);
}

static snakeio::key_t poly1305_key_gen(const snakeio::key_t& key, snakeio::const_nonce_view nonce) noexcept {
    snakeio::key_t out;
    const auto block = chacha20_block(key, 0, nonce);
    for (std::size_t i = 0; i < out.size() / 4; ++i) {
        snakeio::store_32(std::span<std::byte, 4>(out.begin() + i*4, out.begin() + (i+1)*4), block[i]);
    }
    return out;
}

void snakeio::data_packet::encrypt(const key_t& key) noexcept {
    const auto otk = poly1305_key_gen(key, nonce());
    chacha20_encrypt(key, 1, nonce(), text());
    poly1305_mac(tag(), std::span(begin(), end() - tag_view::extent), otk);
}

snakeio::data_packet::verify_result snakeio::data_packet::verify(const key_t& key) const noexcept {
    using enum verify_result;
    if (size() < header_size) [[unlikely]] return too_short;
    if (text().size() % 64 != 0) [[unlikely]] return invalid_text_size;
    const auto otk = poly1305_key_gen(key, nonce());
    tag_t tag;
    poly1305_mac(tag, std::span(begin(), end() - tag_view::extent), otk);
    return std::ranges::equal(tag, this->tag()) ? ok : invalid_tag;
}

void snakeio::data_packet::decrypt(const key_t& key) noexcept {
    chacha20_encrypt(key, 1, nonce(), text());
}