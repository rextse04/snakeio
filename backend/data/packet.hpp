#pragma once
#include <config.hpp>
#include <utils.hpp>
#include <cpp_utils/type.hpp>
#include <cstddef>
#include <span>

namespace snakeio {
    using nonce_t = std::array<std::byte, 12>;
    using nonce_view = std::span<std::byte, 12>;
    using const_nonce_view = std::span<const std::byte, 12>;
    using tag_t = std::array<std::byte, 16>;
    using tag_view = std::span<std::byte, 16>;
    using const_tag_view = std::span<const std::byte, 16>;

    // See packet_format.md.
    class data_packet : std::span<std::byte> {
    public:
        static constexpr size_t header_size = 32;

        using std::span<std::byte>::span;
        data_packet() = delete;
        template <typename Self>
        constexpr auto bytes(this Self&& self) noexcept {
            return std::span<utils::follow_t<Self, std::byte>>(self);
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
        enum class sender_t : std::uint_least32_t {client, server};
        constexpr sender_t sender() const noexcept {
            return static_cast<sender_t>(load_32(subspan<8, 4>()));
        }
        constexpr void sender(sender_t sender) noexcept {
            store_32(subspan<8, 4>(), static_cast<std::uint_least32_t>(sender));
        }
        constexpr auto nonce_part(this auto&& self) noexcept {
            return self.bytes().template subspan<12, 4>();
        }
        constexpr auto nonce(this auto&& self) noexcept {
            return self.bytes().template subspan<8, nonce_view::extent>();
        }
        constexpr auto text(this auto&& self) noexcept {
            return std::span(self.bytes().begin() + 20, self.bytes().end() - tag_view::extent);
        }
        template <typename Self>
        constexpr auto tag(this Self&& self) noexcept {
            return std::span<utils::follow_t<Self, std::byte>, tag_view::extent>(
                self.end() - tag_view::extent, tag_view::extent);
        }

        // Also copies fields to buffer and prepares the packet for sending. The text is encrypted in-place.
        void encrypt(const key_t& key) noexcept;
        enum class verify_result {
            ok,
            too_short,
            invalid_text_size,
            invalid_tag
        };
        // Verifies packet size and tag.
        [[nodiscard]] verify_result verify(const key_t& key) const noexcept;
        // Decrypts the text in-place and fill fields.
        // You must call verify() before calling this function, otherwise authenticity is not guaranteed.
        void decrypt(const key_t& key) noexcept;
    };
}