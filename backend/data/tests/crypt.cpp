#include <cpp_utils/tests/common.hpp>
#include "crypt.hpp"
#include <random>
#include <array>
#include <span>
#include <fstream>
#include <string_view>
#include <format>

using namespace snakeio::test::crypt;
namespace utf = boost::unit_test;

constexpr std::byte operator""_b(unsigned long long int n) noexcept {
    return std::byte(n);
}

// Mostly taken from RFC 8439
BOOST_AUTO_TEST_SUITE(test_vectors)
BOOST_AUTO_TEST_CASE(chacha_quarter_round_test) {
    std::uint_least32_t a = 0x11111111,
    b = 0x01020304,
    c = 0x9b8d6f43,
    d = 0x01234567;
    quarter_round(a, b, c, d);
    BOOST_CHECK_EQUAL(a, 0xea2a92f4);
    BOOST_CHECK_EQUAL(b, 0xcb1cf8ce);
    BOOST_CHECK_EQUAL(c, 0x4581472e);
    BOOST_CHECK_EQUAL(d, 0x5881c4bb);
}

BOOST_AUTO_TEST_CASE(chacha20_block_test) {
    snakeio::key_t key = {
        0x00_b, 0x01_b, 0x02_b, 0x03_b, 0x04_b, 0x05_b, 0x06_b, 0x07_b,
        0x08_b, 0x09_b, 0x0a_b, 0x0b_b, 0x0c_b, 0x0d_b, 0x0e_b, 0x0f_b,
        0x10_b, 0x11_b, 0x12_b, 0x13_b, 0x14_b, 0x15_b, 0x16_b, 0x17_b,
        0x18_b, 0x19_b, 0x1a_b, 0x1b_b, 0x1c_b, 0x1d_b, 0x1e_b, 0x1f_b
    };
    snakeio::nonce_t nonce = {
        0x00_b, 0x00_b, 0x00_b, 0x09_b, 0x00_b, 0x00_b, 0x00_b, 0x4a_b,
        0x00_b, 0x00_b, 0x00_b, 0x00_b
    };
    auto state = chacha20_block(key, 1, nonce);
    constexpr decltype(state) expected = {
        0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3,
        0xc7f4d1c7, 0x0368c033, 0x9aaa2204, 0x4e6cd4c3,
        0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
        0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(state.begin(), state.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(chacha20_test) {
    snakeio::key_t key = {
        0x00_b, 0x01_b, 0x02_b, 0x03_b, 0x04_b, 0x05_b, 0x06_b, 0x07_b,
        0x08_b, 0x09_b, 0x0a_b, 0x0b_b, 0x0c_b, 0x0d_b, 0x0e_b, 0x0f_b,
        0x10_b, 0x11_b, 0x12_b, 0x13_b, 0x14_b, 0x15_b, 0x16_b, 0x17_b,
        0x18_b, 0x19_b, 0x1a_b, 0x1b_b, 0x1c_b, 0x1d_b, 0x1e_b, 0x1f_b
    };
    snakeio::nonce_t nonce = {
        0x00_b, 0x00_b, 0x00_b, 0x00_b, 0x00_b, 0x00_b, 0x00_b, 0x4a_b,
        0x00_b, 0x00_b, 0x00_b, 0x00_b
    };
    unsigned char text[] = "Ladies and Gentlemen of the class of '99: "
        "If I could offer you only one tip for the future, sunscreen would be it.";
    chacha20_encrypt(key, 1, nonce, std::as_writable_bytes(std::span(text)));
    constexpr unsigned char expected[] = {
        0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81,
        0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2, 0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b,
        0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
        0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8,
        0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61, 0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e,
        0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
        0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42,
        0x87, 0x4d
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(text, text + sizeof(expected), expected, expected + sizeof(expected));
}

// modified to make text 32 bytes
BOOST_AUTO_TEST_CASE(poly1305_test) {
    snakeio::key_t key = {
        0x85_b, 0xd6_b, 0xbe_b, 0x78_b, 0x57_b, 0x55_b, 0x6d_b, 0x33_b,
        0x7f_b, 0x44_b, 0x52_b, 0xfe_b, 0x42_b, 0xd5_b, 0x06_b, 0xa8_b,
        0x01_b, 0x03_b, 0x80_b, 0x8a_b, 0xfb_b, 0x0d_b, 0xb2_b, 0xfd_b,
        0x4a_b, 0xbf_b, 0xf6_b, 0xaf_b, 0x41_b, 0x49_b, 0xf5_b, 0x1b_b
    };
    unsigned char text[] = "CryptographicForumResearch Group";
    snakeio::tag_t tag;
    poly1305_mac(tag, std::as_bytes(std::span(text)), key);
    const auto tag_begin = reinterpret_cast<const unsigned char*>(tag.data());
    constexpr unsigned char expected[] = {
        0xe8, 0xef, 0x04, 0xd4, 0xaf, 0x6a, 0x66, 0xbc,
        0x13, 0x73, 0x9b, 0x6e, 0x8a, 0x74, 0x4e, 0xe2
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(tag_begin, tag_begin + sizeof(expected), expected, expected + sizeof(expected));
}

BOOST_AUTO_TEST_CASE(poly1305_test2) {
    snakeio::key_t key = {
        0xb1_b, 0x03_b, 0x7c_b, 0x46_b, 0x45_b, 0x01_b, 0x5b_b, 0x44_b,
        0x0e_b, 0x7f_b, 0xb4_b, 0x98_b, 0x33_b, 0xec_b, 0x9a_b, 0x14_b,
        0xe7_b, 0xfe_b, 0x8f_b, 0xe3_b, 0xc2_b, 0x08_b, 0xdc_b, 0x58_b,
        0x1e_b, 0xc2_b, 0xac_b, 0x93_b, 0x7f_b, 0x8f_b, 0xb0_b, 0x85_b
    };
    std::array text = {
        0x81_b, 0x8d_b, 0xe0_b, 0x29_b, 0x9f_b, 0x23_b, 0x9a_b, 0x80_b,
        0xc0_b, 0x77_b, 0x75_b, 0x06_b, 0x74_b, 0x5f_b, 0xf0_b, 0x6d_b,
        0x3e_b, 0xa9_b, 0x65_b, 0xbe_b, 0x6a_b, 0x91_b, 0x4d_b, 0x20_b,
        0x85_b, 0xc9_b, 0xbb_b, 0xb3_b, 0x00_b, 0x6a_b, 0x1e_b, 0xb6_b
    };
    snakeio::tag_t tag;
    poly1305_mac(tag, text, key);
    const auto tag_begin = reinterpret_cast<const unsigned char*>(tag.data());
    constexpr unsigned char expected[] = {
        0xdf, 0x29, 0xa4, 0xfa, 0x6d, 0x19, 0xeb, 0x14,
        0xb1, 0xc3, 0x28, 0x51, 0x6a, 0x08, 0x99, 0x0a
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(tag_begin, tag_begin + sizeof(expected), expected, expected + sizeof(expected));
}

BOOST_AUTO_TEST_CASE(poly1305_key_gen_test) {
    snakeio::key_t key = {
        0x80_b, 0x81_b, 0x82_b, 0x83_b, 0x84_b, 0x85_b, 0x86_b, 0x87_b,
        0x88_b, 0x89_b, 0x8a_b, 0x8b_b, 0x8c_b, 0x8d_b, 0x8e_b, 0x8f_b,
        0x90_b, 0x91_b, 0x92_b, 0x93_b, 0x94_b, 0x95_b, 0x96_b, 0x97_b,
        0x98_b, 0x99_b, 0x9a_b, 0x9b_b, 0x9c_b, 0x9d_b, 0x9e_b, 0x9f_b
    };
    snakeio::nonce_t nonce = {
        0x00_b, 0x00_b, 0x00_b, 0x00_b, 0x00_b, 0x01_b, 0x02_b, 0x03_b,
        0x04_b, 0x05_b, 0x06_b, 0x07_b
    };
    snakeio::key_t otk = poly1305_key_gen(key, nonce);
    const auto otk_begin = reinterpret_cast<const unsigned char*>(otk.data());
    constexpr unsigned char expected[] = {
        0x8a, 0xd5, 0xa0, 0x8b, 0x90, 0x5f, 0x81, 0xcc,
        0x81, 0x50, 0x40, 0x27, 0x4a, 0xb2, 0x94, 0x71,
        0xa8, 0x33, 0xb6, 0x37, 0xe3, 0xfd, 0x0d, 0xa5,
        0x08, 0xdb, 0xb8, 0xe2, 0xfd, 0xd1, 0xa6, 0x46
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(otk_begin, otk_begin + sizeof(expected), expected, expected + sizeof(expected));
}
BOOST_AUTO_TEST_SUITE_END()

struct fuzzing_fixture {
    static constexpr snakeio::size_t msg_size = 16*7;
    static constexpr std::size_t data_size = 1000;

    struct data_item {
        std::array<std::byte, sizeof(snakeio::key_t)> key;
        std::array<std::byte, sizeof(snakeio::nonce_t)> nonce;
        std::array<std::byte, msg_size> text;
        constexpr data_item() noexcept {}
    };

    static inline std::independent_bits_engine<std::mt19937, 8, unsigned char> gen;
    static inline std::vector<data_item> data;

    static void gen_bytes(std::span<std::byte> out) noexcept {
        for (auto& b : out) {
            b = static_cast<std::byte>(gen());
        }
    }

    fuzzing_fixture() {
        gen = decltype(gen)(std::random_device{}());
        data.resize(data_size);
        for (data_item& item : data) {
            gen_bytes(item.key);
            gen_bytes(item.nonce);
            gen_bytes(item.text);
        }
        std::ofstream ofs("crypt_data.bin", std::ios::binary);
        ofs.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        const auto write_bytes = [&ofs](std::span<const std::byte> out) {
            ofs.write(reinterpret_cast<const char*>(out.data()), out.size());
        };
        for (const data_item& item : data) {
            write_bytes(item.key);
            write_bytes(item.nonce);
            write_bytes(item.text);
        }
    }
};
std::ostream& operator<<(std::ostream& os, const fuzzing_fixture::data_item& item) {
    const auto flags = os.flags();
    os << std::hex << std::setfill('0');
    os << "key: ";
    for (const auto& b : item.key) {
        os << std::setw(2) << static_cast<int>(b);
    }
    os << ", nonce: ";
    for (const auto& b : item.nonce) {
        os << std::setw(2) << static_cast<int>(b);
    }
    os << ", text: ";
    for (const auto& b : item.text) {
        os << std::setw(2) << static_cast<int>(b);
    }
    os.flags(flags);
    return os;
}
BOOST_FIXTURE_TEST_SUITE(fuzzing_tests, fuzzing_fixture)
constexpr std::string_view command = "python3 \"" SNAKEIO_TEST_CRYPT_PY "\" {} {}";
constexpr char const* solution_path = "crypt_solution.bin";

struct chacha20_encrypt_fixture {
    static inline std::ifstream ifs;
    chacha20_encrypt_fixture() {
        std::system(std::format(command, "chacha20_encrypt", fuzzing_fixture::msg_size).c_str());
        ifs.open(solution_path, std::ios::binary);
        ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    }
    static auto read() {
        std::array<char, fuzzing_fixture::msg_size> out;
        ifs.read(out.data(), out.size());
        return out;
    }
};
BOOST_FIXTURE_TEST_SUITE(chacha20_encrypt_tests, chacha20_encrypt_fixture)
BOOST_DATA_TEST_CASE(chacha20_encrypt_test, utf::data::xrange(fuzzing_fixture::data_size), idx) {
    const fuzzing_fixture::data_item& item = fuzzing_fixture::data[idx];
    BOOST_TEST_CONTEXT(item) {
        auto text = item.text;
        chacha20_encrypt(item.key, 1, item.nonce, text);
        const char* test_start = reinterpret_cast<const char*>(text.data());
        const auto expected = chacha20_encrypt_fixture::read();
        BOOST_CHECK_EQUAL_COLLECTIONS(test_start, test_start + expected.size(),
            expected.data(), expected.data() + expected.size());
    }
}
BOOST_AUTO_TEST_SUITE_END()

struct poly1305_tests_fixture {
    static inline std::ifstream ifs;
    poly1305_tests_fixture() {
        std::system(std::format(command, "poly1305_mac", fuzzing_fixture::msg_size).c_str());
        ifs.open(solution_path, std::ios::binary);
        ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    }
    static auto read() {
        std::array<char, sizeof(snakeio::tag_t)> out;
        ifs.read(out.data(), out.size());
        return out;
    }
};
BOOST_FIXTURE_TEST_SUITE(poly1305_tests, poly1305_tests_fixture)
BOOST_DATA_TEST_CASE(poly1305_test, utf::data::xrange(fuzzing_fixture::data_size), idx) {
    const fuzzing_fixture::data_item& item = fuzzing_fixture::data[idx];
    BOOST_TEST_CONTEXT(item) {
        snakeio::tag_t tag;
        poly1305_mac(tag, std::span(item.text), item.key);
        const char* tag_start = reinterpret_cast<const char*>(tag.data());
        auto expected = poly1305_tests_fixture::read();
        BOOST_CHECK_EQUAL_COLLECTIONS(tag_start, tag_start + expected.size(),
            expected.begin(), expected.begin() + expected.size());
    }
}
BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()