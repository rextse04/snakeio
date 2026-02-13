#pragma once
#include <config.hpp>

namespace snakeio {
    /*
     * We are using the ChaCha20-poly1305 AEAD cipher for each (data) packet.
     * Format of packet: <session ID: 4><player ID: 4><tag: 16><ciphertext: N>
     * where N is the length of the plaintext data.
     * All integers are in network byte order (big-endian).
     */
    struct data_packet {
        id_t session_id;
        id_t player_id;
        std::array<std::byte, 16> tag;
        std::size_t ciphertext_size;
        static constexpr std::size_t header_size = sizeof(id_t) * 2 + sizeof(tag);
        std::array<std::byte, packet_max_size - header_size> ciphertext_buffer;

        constexpr std::span<const std::byte> ciphertext() const noexcept {
            return {ciphertext_buffer.begin(), ciphertext_size};
        }
    };
}