#pragma once
#include <config.hpp>
#include <cpp_utils/type.hpp>

namespace snakeio {
    using nonce_t = std::array<std::byte, 12>;
    using tag_t = std::array<std::byte, 16>;
    /*
     * We are using the ChaCha20-poly1305 AEAD cipher for each (data) packet.
     * Format of packet: <session ID: 4><player ID: 4><nonce: 12><ciphertext: N><tag: 16>
     * where N is the length of the plaintext data.
     * All integers are in small endian.
     * Size of plain text must be multiples of 64.
     */
    struct data_packet {
        id_t session_id, player_id;
        nonce_t nonce;
        tag_t tag;
        size_t size;
        mutable std::array<std::byte, in_packet_max_size> buffer;

        static constexpr size_t prefix_size = 4 + 4 + 12, suffix_size = 16;

        enum class load_result {
            ok,
            too_short,
            invalid_text_size
        };
        // Load fields from buffer. Checks size.
        [[nodiscard]] load_result load() noexcept;

        template <typename Self>
        constexpr utils::follow_t<Self, std::byte*> data(this Self&& self) noexcept {
            return self.buffer.data();
        }
        constexpr auto bytes(this auto&& self) noexcept {
            return std::span(self.buffer);
        }
        constexpr size_t text_size() const noexcept {
            return size - prefix_size - suffix_size;
        }
        constexpr auto text(this auto&& self) noexcept {
            return std::span(self.buffer.begin() + prefix_size, self.buffer.end() - suffix_size);
        }
        // Also copies fields to buffer and prepares the packet for sending. The text is encrypted in-place.
        void encrypt(const key_t& key) noexcept;
        [[nodiscard]] bool verify(const key_t& key) const noexcept;
        // Decrypts the text in-place and fill fields.
        // You must call verify() before calling this function, otherwise security is not guaranteed.
        void decrypt(const key_t& key) noexcept;
    };

}