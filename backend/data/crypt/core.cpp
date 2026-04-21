#include <crypt/core.hpp>
#include <utils.hpp>
#include <compatibility.hpp>
#include <cstddef>

__host__ __device__ void snakeio::crypt::quarter_round(
    std::uint_least32_t& a, std::uint_least32_t& b, std::uint_least32_t& c, std::uint_least32_t& d) noexcept {
    a += b; d ^= a; d = (d << 16) | (d >> 16);
    c += d; b ^= c; b = (b << 12) | (b >> 20);
    a += b; d ^= a; d = (d << 8) | (d >> 24);
    c += d; b ^= c; b = (b << 7) | (b >> 25);
}

__host__ __device__ std::array<std::uint_least32_t, 16> snakeio::crypt::chacha20_block(
    const key_t& key, std::uint_least32_t counter, std::span<const std::byte, 12> nonce) noexcept {
    std::array<std::uint_least32_t, 16> out;
    out[0] = 0x61707865;
    out[1] = 0x3320646e;
    out[2] = 0x79622d32;
    out[3] = 0x6b206574;
    for (std::size_t i = 0; i < std::tuple_size_v<key_t> / 4; ++i) {
        out[4 + i] = load_32(std::span<const std::byte, 4>(key.begin() + i*4, 4));
    }
    out[12] = counter;
    for (std::size_t i = 0; i < nonce.size() / 4; ++i) {
        out[13 + i] = load_32(std::span<const std::byte, 4>(nonce.begin() + i*4, 4));
    }
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
    for (std::size_t i = 0; i < 16; ++i) out[i] += state[i];
    return out;
}

__host__ __device__ void snakeio::crypt::chacha20_encrypt(const key_t& key, std::uint_least32_t counter,
    std::span<const std::byte, 12> nonce, std::span<std::byte> text) noexcept {
    std::size_t i = 0;
    for (std::size_t j = 0; j < text.size() / 64; ++j) {
        const auto key_stream = chacha20_block(key, counter++, nonce);
        for (std::size_t k = 0; k < 64; ++k) {
            text[i++] ^= static_cast<std::byte>(key_stream[k/4] >> ((k%4) * 8));
        }
    }
    if (text.size() % 64 != 0) {
        const auto key_stream = chacha20_block(key, counter, nonce);
        for (std::size_t k = 0; k < text.size() % 64; ++k) {
            text[i++] ^= static_cast<std::byte>(key_stream[k/4] >> ((k%4) * 8));
        }
    }
}

struct radix26 {
    using base_type = std::uint_least32_t;
    using ext_type = std::uint_least64_t;
    static constexpr int radix = 26, size = 5;

    std::array<base_type, size> limbs;

    __host__ __device__ static constexpr radix26 load(std::span<const std::byte, 16> bytes) noexcept {
        radix26 out;
        const std::uint_least64_t lo = snakeio::load_64(bytes.subspan<0, 8>()),
            hi = snakeio::load_64(bytes.subspan<8, 8>());
        constexpr std::uint_least64_t mask = (base_type(1) << radix) - 1;
        out.limbs[0] = lo & mask;
        out.limbs[1] = (lo >> radix) & mask;
        constexpr int spill = radix - (64 - radix * 2);
        out.limbs[2] = (lo >> (2 * radix)) | ((hi & ((base_type(1) << spill) - 1)) << (radix - spill));
        out.limbs[3] = (hi >> spill) & mask;
        out.limbs[4] = hi >> (spill + radix);
        return out;
    }
    __host__ __device__ constexpr void store(std::span<std::byte, 16> out) const noexcept {
        auto limb = limbs.begin();
        int pos = 0;
        for (std::byte& byte : out) {
            if (pos + 8 >= radix) {
                const int spill = pos + 8 - radix;
                const auto lo = static_cast<std::byte>(*limb >> pos),
                    hi = static_cast<std::byte>(*(limb + 1) & ((base_type(1) << spill) - 1));
                byte = lo | (hi << (8 - spill));
                ++limb;
                pos = spill;
            } else {
                byte = static_cast<std::byte>(*limb >> pos);
                pos += 8;
            }
        }
    }
private:
    __host__ __device__ static constexpr void propagate_once(auto& limbs, bool fold = true) noexcept {
        if (fold) {
            limbs[0] += (limbs[size - 1] >> radix) * 5;
            limbs[size - 1] &= (base_type(1) << radix) - 1;
        }
        for (std::size_t i = 0; i < size - 1; ++i) {
            limbs[i+1] += limbs[i] >> radix;
            limbs[i] &= (base_type(1) << radix) - 1;
        }
    }
    __host__ __device__ static constexpr void propagate(auto& limbs) noexcept {
        propagate_once(limbs);
        propagate_once(limbs);
    }
public:
    __host__ __device__ constexpr void propagate_once(bool fold = true) noexcept {
        propagate_once(limbs, fold);
    }
    __host__ __device__ constexpr radix26& operator+=(const radix26& other) noexcept {
        for (std::size_t i = 0; i < size; ++i) limbs[i] += other.limbs[i];
        return *this;
    }
    __host__ __device__ constexpr radix26& operator*=(const radix26& other) noexcept {
        std::array<ext_type, 9> temp{};
        for (std::size_t i = 0; i < size; ++i) {
            for (std::size_t j = 0; j < size; ++j) {
                temp[i+j] += static_cast<ext_type>(limbs[i]) * static_cast<ext_type>(other.limbs[j]);
            }
        }
        for (std::size_t i = size; i < temp.size(); ++i) temp[i - size] += 5 * temp[i];
        propagate(temp);
        for (std::size_t i = 0; i < size; ++i) limbs[i] = static_cast<base_type>(temp[i]);
        return *this;
    }
};

__host__ __device__ void snakeio::crypt::poly1305_mac(
    std::span<std::byte, 16> out, std::span<const std::byte> text, const key_t& key) noexcept {
    auto r = radix26::load(std::span<const std::byte, 16>(key.begin(), 16)),
        s = radix26::load(std::span<const std::byte, 16>(key.begin() + 16, 16));
    r.limbs[0] &= 0x3ffffff;
    r.limbs[1] &= 0x3ffff03;
    r.limbs[2] &= 0x3ffc0ff;
    r.limbs[3] &= 0x3f03fff;
    r.limbs[4] &= 0x00fffff;
    radix26 a{};
    for (std::size_t i = 0; i < text.size() / 16; ++i) {
        auto n = radix26::load(std::span<const std::byte, 16>{text.begin() + i*16, 16});
        n.limbs[128 / radix26::radix] |= radix26::base_type(1) << (128 % radix26::radix);
        a += n;
        a *= r;
    }
    a += s;
    a.propagate_once(false);
    a.store(out);
}

__host__ __device__ snakeio::key_t snakeio::crypt::poly1305_key_gen(
    const key_t& key, std::span<const std::byte, 12> nonce) noexcept {
    key_t out;
    const auto block = chacha20_block(key, 0, nonce);
    for (std::size_t i = 0; i < out.size() / 4; ++i) {
        store_32(std::span<std::byte, 4>(out.begin() + i*4, 4), block[i]);
    }
    return out;
}