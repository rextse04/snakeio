#include <crypt/core.hpp>
#include <cuda_runtime.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
    __global__ void quarter_round_kernel(
        std::uint_least32_t* a, std::uint_least32_t* b, std::uint_least32_t* c, std::uint_least32_t* d) {
        snakeio::crypt::quarter_round(*a, *b, *c, *d);
    }

    __global__ void chacha20_block_kernel(const std::byte* key_bytes, std::uint_least32_t counter,
        const std::byte* nonce_bytes, std::uint_least32_t* out_words) {
        snakeio::key_t key;
        std::array<std::byte, 12> nonce;
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = key_bytes[i];
        for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = nonce_bytes[i];
        const auto state = snakeio::crypt::chacha20_block(key, counter, std::span<const std::byte, 12>(nonce));
        for (std::size_t i = 0; i < state.size(); ++i) out_words[i] = state[i];
    }

    __global__ void chacha20_encrypt_kernel(const std::byte* key_bytes, std::uint_least32_t counter,
        const std::byte* nonce_bytes, std::byte* text, std::size_t text_size) {
        snakeio::key_t key;
        std::array<std::byte, 12> nonce;
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = key_bytes[i];
        for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = nonce_bytes[i];
        snakeio::crypt::chacha20_encrypt(key, counter, std::span<const std::byte, 12>(nonce), std::span(text, text_size));
    }

    __global__ void poly1305_mac_kernel(
        std::byte* out, const std::byte* text, std::size_t text_size, const std::byte* key_bytes) {
        snakeio::key_t key;
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = key_bytes[i];
        snakeio::crypt::poly1305_mac(std::span<std::byte, 16>(out, 16), std::span(text, text_size), key);
    }

    __global__ void poly1305_key_gen_kernel(std::byte* out, const std::byte* key_bytes, const std::byte* nonce_bytes) {
        snakeio::key_t key;
        std::array<std::byte, 12> nonce;
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = key_bytes[i];
        for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = nonce_bytes[i];
        const auto otk = snakeio::crypt::poly1305_key_gen(key, std::span<const std::byte, 12>(nonce));
        for (std::size_t i = 0; i < otk.size(); ++i) out[i] = otk[i];
    }

    template <typename T>
    T* managed_alloc(std::size_t count = 1) noexcept {
        T* ptr = nullptr;
        cudaMallocManaged(&ptr, count * sizeof(T));
        return ptr;
    }
}

namespace snakeio::test::crypt {
    void quarter_round(
        std::uint_least32_t& a, std::uint_least32_t& b, std::uint_least32_t& c, std::uint_least32_t& d) noexcept {
        auto* da = managed_alloc<std::uint_least32_t>();
        auto* db = managed_alloc<std::uint_least32_t>();
        auto* dc = managed_alloc<std::uint_least32_t>();
        auto* dd = managed_alloc<std::uint_least32_t>();
        *da = a; *db = b; *dc = c; *dd = d;
        quarter_round_kernel<<<1, 1>>>(da, db, dc, dd);
        cudaDeviceSynchronize();
        a = *da; b = *db; c = *dc; d = *dd;
        cudaFree(da); cudaFree(db); cudaFree(dc); cudaFree(dd);
    }

    std::array<std::uint_least32_t, 16> chacha20_block(
        const std::array<std::byte, 32>& key, std::uint_least32_t counter, std::span<const std::byte, 12> nonce) noexcept {
        auto* dkey = managed_alloc<std::byte>(key.size());
        auto* dnonce = managed_alloc<std::byte>(nonce.size());
        auto* dout = managed_alloc<std::uint_least32_t>(16);
        for (std::size_t i = 0; i < key.size(); ++i) dkey[i] = key[i];
        for (std::size_t i = 0; i < nonce.size(); ++i) dnonce[i] = nonce[i];
        chacha20_block_kernel<<<1, 1>>>(dkey, counter, dnonce, dout);
        cudaDeviceSynchronize();
        std::array<std::uint_least32_t, 16> out{};
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = dout[i];
        cudaFree(dkey); cudaFree(dnonce); cudaFree(dout);
        return out;
    }

    void chacha20_encrypt(const std::array<std::byte, 32>& key, std::uint_least32_t counter, std::span<const std::byte, 12> nonce,
        std::span<std::byte> text) noexcept {
        auto* dkey = managed_alloc<std::byte>(key.size());
        auto* dnonce = managed_alloc<std::byte>(nonce.size());
        auto* dtext = managed_alloc<std::byte>(text.size());
        for (std::size_t i = 0; i < key.size(); ++i) dkey[i] = key[i];
        for (std::size_t i = 0; i < nonce.size(); ++i) dnonce[i] = nonce[i];
        for (std::size_t i = 0; i < text.size(); ++i) dtext[i] = text[i];
        chacha20_encrypt_kernel<<<1, 1>>>(dkey, counter, dnonce, dtext, text.size());
        cudaDeviceSynchronize();
        for (std::size_t i = 0; i < text.size(); ++i) text[i] = dtext[i];
        cudaFree(dkey); cudaFree(dnonce); cudaFree(dtext);
    }

    void poly1305_mac(std::span<std::byte, 16> out, std::span<const std::byte> text, const std::array<std::byte, 32>& key) noexcept {
        auto* dkey = managed_alloc<std::byte>(key.size());
        auto* dtext = managed_alloc<std::byte>(text.size());
        auto* dout = managed_alloc<std::byte>(out.size());
        for (std::size_t i = 0; i < key.size(); ++i) dkey[i] = key[i];
        for (std::size_t i = 0; i < text.size(); ++i) dtext[i] = text[i];
        poly1305_mac_kernel<<<1, 1>>>(dout, dtext, text.size(), dkey);
        cudaDeviceSynchronize();
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = dout[i];
        cudaFree(dkey); cudaFree(dtext); cudaFree(dout);
    }

    std::array<std::byte, 32> poly1305_key_gen(
        const std::array<std::byte, 32>& key, std::span<const std::byte, 12> nonce) noexcept {
        auto* dkey = managed_alloc<std::byte>(key.size());
        auto* dnonce = managed_alloc<std::byte>(nonce.size());
        auto* dout = managed_alloc<std::byte>(key.size());
        for (std::size_t i = 0; i < key.size(); ++i) dkey[i] = key[i];
        for (std::size_t i = 0; i < nonce.size(); ++i) dnonce[i] = nonce[i];
        poly1305_key_gen_kernel<<<1, 1>>>(dout, dkey, dnonce);
        cudaDeviceSynchronize();
        std::array<std::byte, 32> out{};
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = dout[i];
        cudaFree(dkey); cudaFree(dnonce); cudaFree(dout);
        return out;
    }
}