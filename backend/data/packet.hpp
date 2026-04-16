#pragma once
#include <config.hpp>
#include <utils.hpp>
#include <cstddef>
#include <span>

namespace snakeio {
    using nonce_t = std::array<std::byte, 12>;
    using nonce_view = std::span<std::byte, 12>;
    using const_nonce_view = std::span<const std::byte, 12>;
    using volatile_nonce_view = std::span<volatile std::byte, 12>;
    using const_volatile_nonce_view = std::span<const volatile std::byte, 12>;

    using tag_t = std::array<std::byte, 16>;
    using tag_view = std::span<std::byte, 16>;
    using const_tag_view = std::span<const std::byte, 16>;
    using volatile_tag_view = std::span<volatile std::byte, 16>;
    using const_volatile_tag_view = std::span<const volatile std::byte, 16>;

    // See protocol.md.
    class data_packet : std::span<std::byte> {
    public:
        static constexpr size_t aad_size = 16, header_size = aad_size + tag_view::extent;

        using std::span<std::byte>::span;
        data_packet() = delete;
        constexpr std::span<std::byte> bytes() noexcept {
            return *this;
        }
        constexpr std::span<const std::byte> bytes() const noexcept {
            return {data(), size()};
        }
        constexpr id_t session_id() const noexcept {
            return load_32(subspan<0, 4>());
        }
        constexpr void session_id(id_t id) noexcept {
            store_32(subspan<0, 4>(), id);
        }
        constexpr id_t player_id() const noexcept {
            return load_32(subspan<4, 4>());
        }
        constexpr void player_id(id_t id) noexcept {
            store_32(subspan<4, 4>(), id);
        }
        enum class sender_t : unsigned char {client, server};
        constexpr sender_t sender() const noexcept {
            return static_cast<sender_t>((*this)[8]);
        }
        constexpr void sender(sender_t sender) noexcept {
            (*this)[8] = static_cast<std::byte>(sender);
        }
        constexpr std::uint_least8_t total_chunks() const noexcept {
            return static_cast<std::uint_least8_t>((*this)[9]);
        }
        constexpr void total_chunks(std::uint_least8_t total_chunks) noexcept {
            (*this)[9] = static_cast<std::byte>(total_chunks);
        }
        constexpr std::uint_least8_t chunk_id() const noexcept {
            return static_cast<std::uint_least8_t>((*this)[10]);
        }
        constexpr void chunk_id(std::uint_least16_t chunk_id) noexcept {
            (*this)[10] = static_cast<std::byte>(chunk_id);
        }
        constexpr std::span<std::byte, 4> nonce_part() noexcept {
            return bytes().template subspan<12, 4>();
        }
        constexpr std::span<const std::byte, 4> nonce_part() const noexcept {
            return bytes().template subspan<12, 4>();
        }
        constexpr nonce_view nonce() noexcept {
            return bytes().template subspan<4, nonce_view::extent>();
        }
        constexpr const_nonce_view nonce() const noexcept {
            return bytes().template subspan<4, nonce_view::extent>();
        }
        constexpr std::span<std::byte> text() noexcept {
            auto b = bytes();
            return {b.data() + aad_size, b.size() - header_size};
        }
        constexpr std::span<const std::byte> text() const noexcept {
            auto b = bytes();
            return {b.data() + aad_size, b.size() - header_size};
        }
        constexpr tag_view tag() noexcept {
            return bytes().template last<tag_view::extent>();
        }
        constexpr const_tag_view tag() const noexcept {
            return bytes().template last<tag_view::extent>();
        }

        // Also copies fields to buffer and prepares the packet for sending. The text is encrypted in-place.
        __host__ __device__ void encrypt(const key_t& key) noexcept;
        enum class verify_result {
            ok,
            too_short,
            invalid_size,
            invalid_tag
        };
        // Verifies packet size and tag.
        // Implementation is allowed to replace tag() with appropriate values to satisfy the packet format in RFC 8439.
        [[nodiscard]] __host__ __device__ verify_result verify(const key_t& key) noexcept;
        // Decrypts the text in-place and fill fields.
        // You must call verify() before calling this function, otherwise authenticity is not guaranteed.
        __host__ __device__ void decrypt(const key_t& key) noexcept;
    };
}