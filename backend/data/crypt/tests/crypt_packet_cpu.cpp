#include <tests/crypt.hpp>

namespace snakeio::test::crypt {
    void encrypt_packet(std::span<std::byte> packet, const key_t& key) noexcept {
        data_packet(packet.data(), packet.size()).encrypt(key);
    }

    bool verify_decrypt_packet(std::span<std::byte> packet, const key_t& key) noexcept {
        auto p = data_packet(packet.data(), packet.size());
        if (p.verify(key) != data_packet::verify_result::ok) return false;
        p.decrypt(key);
        return true;
    }
}