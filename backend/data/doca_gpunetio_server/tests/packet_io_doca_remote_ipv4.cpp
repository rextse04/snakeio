#include <cpp_utils/tests/common.hpp>
#include <config.hpp>
#include <game.hpp>
#include <packet.hpp>
#include <utils.hpp>
#include <array>
#include <bit>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <poll.h>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
#include <unistd.h>

#ifndef SNAKEIO_DOCA_REMOTE_AGENT
#define SNAKEIO_DOCA_REMOTE_AGENT ""
#endif

namespace {

/// Safe single-quoted shell word (for `sshpass -p ...`).
std::string shell_single_quoted(const char* s)
{
    std::string out(1, '\'');
    for (const char* p = s; p && *p; ++p) {
        if (*p == '\'')
            out += "'\\''";
        else
            out += *p;
    }
    out += '\'';
    return out;
}

std::string b64_encode(std::span<const std::byte> in)
{
    static constexpr char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(in[i]);
        const unsigned b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const unsigned c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0u;
        const unsigned triple = (a << 16) | (b << 8) | c;
        const int n = static_cast<int>(in.size() - i);
        out += tab[(triple >> 18) & 63];
        out += tab[(triple >> 12) & 63];
        out += n > 1 ? tab[(triple >> 6) & 63] : '=';
        out += n > 2 ? tab[triple & 63] : '=';
    }
    return out;
}

snakeio::key_t test_key(std::byte seed = std::byte(1))
{
    snakeio::key_t k{};
    for (snakeio::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<std::byte>(static_cast<unsigned char>(seed) + static_cast<unsigned char>(i));
    }
    return k;
}

std::array<std::byte, snakeio::in_packet_max_text_size + snakeio::data_packet::header_size> make_ingress_packet(
    const snakeio::key_t& key, snakeio::id_t session_id, snakeio::id_t player_id, bool snapshot_requested, bool boost,
    float angle, snakeio::tick_t nonce_part)
{
    std::array<std::byte, snakeio::in_packet_max_text_size + snakeio::data_packet::header_size> raw{};
    snakeio::data_packet p(raw.data(), raw.size());
    p.session_id(session_id);
    p.player_id(player_id);
    p.sender(snakeio::data_packet::sender_t::client);
    p.total_chunks(1);
    p.chunk_id(0);
    snakeio::store_32(p.nonce_part(), nonce_part);

    auto text = p.text();
    std::fill(text.begin(), text.end(), std::byte(0));
    text[0] = static_cast<std::byte>(snapshot_requested);
    text[1] = static_cast<std::byte>(boost);
    snakeio::store_32(std::span<std::byte, 4>(text.data() + 4, 4), std::bit_cast<std::uint_least32_t>(angle));

    p.encrypt(key);
    return raw;
}

std::set<std::uint_least32_t> decrypt_types(std::vector<std::vector<std::byte>>& packets, const snakeio::key_t& key,
    snakeio::tick_t expected_tick, snakeio::id_t expected_player)
{
    std::set<std::uint_least32_t> types;
    for (auto& raw : packets) {
        auto packet = std::span<std::byte>(raw.data(), raw.size());
        snakeio::data_packet p(packet.data(), packet.size());
        if (p.player_id() != expected_player)
            continue;
        BOOST_REQUIRE(p.verify(key) == snakeio::data_packet::verify_result::ok);
        p.decrypt(key);
        BOOST_CHECK(p.sender() == snakeio::data_packet::sender_t::server);
        if (p.chunk_id() == 0) {
            const auto type = snakeio::load_32(std::span<const std::byte, 4>(p.text().data(), 4));
            const auto nonce = snakeio::load_32(p.nonce_part());
            if (type == 3) {
                BOOST_CHECK(nonce == expected_tick || nonce == expected_tick + 1);
            } else {
                BOOST_CHECK_EQUAL(nonce, expected_tick);
            }
            types.insert(type);
        }
    }
    return types;
}

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<int>(c - 'A');
    if (c >= 'a' && c <= 'z')
        return 26 + static_cast<int>(c - 'a');
    if (c >= '0' && c <= '9')
        return 52 + static_cast<int>(c - '0');
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static std::vector<std::byte> b64_decode(std::string_view in)
{
    std::vector<std::byte> out;
    out.reserve(in.size() * 3 / 4);
    int buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c == '=')
            break;
        const int v = b64_value(c);
        if (v < 0)
            continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<std::vector<std::byte>> parse_pkts_from_remote_log(const std::string& log)
{
    std::vector<std::vector<std::byte>> out;
    std::size_t pos = 0;
    while (pos < log.size()) {
        const auto at = log.find("PKT ", pos);
        if (at == std::string::npos)
            break;
        const auto nl = log.find('\n', at);
        if (nl == std::string::npos)
            break;
        const std::string line = log.substr(at + 4, nl - (at + 4));
        pos = nl + 1;
        if (line.empty())
            continue;
        auto decoded = b64_decode(line);
        if (!decoded.empty())
            out.push_back(std::move(decoded));
    }
    return out;
}

/// Run `doc_packet_io_remote_agent.py` on `remote` via ssh; after remote prints `SENT`, call `on_sent` (game tick).
/// Collects full stdout (including `PKT ...` lines) into `remote_log`.
void run_remote_ipv4_agent(const char* remote, const char* ssh_pass, const char* agent_path, const char* dst_ipv4,
    int port, int recv_tail_ms, const std::vector<std::string>& b64_packets, const std::function<void()>& on_sent,
    std::string& remote_log)
{
    BOOST_REQUIRE_MESSAGE(agent_path != nullptr && agent_path[0] != '\0', "missing remote agent path macro.");
    const bool use_sshpass = ssh_pass != nullptr && ssh_pass[0] != '\0';

    std::string b64args;
    for (std::size_t i = 0; i < b64_packets.size(); ++i) {
        if (i)
            b64args += ' ';
        b64args += b64_packets[i];
    }

    // Redirect agent into ssh stdin (`< file`) so remote `python3 -` reads the script. Avoid `cat | sshpass | ssh`:
    // sshpass would consume the pipe meant for the ssh session.
    const std::string remote_py = std::string("python3 -u - ") + dst_ipv4 + " " + std::to_string(port) + " "
        + std::to_string(recv_tail_ms) + " " + b64args;
    std::string cmd;
    if (use_sshpass)
        cmd += "sshpass -p " + shell_single_quoted(ssh_pass) + " ";
    // Do not use BatchMode=yes: it disables password prompts and breaks `sshpass -p` authentication.
    cmd += "ssh -o StrictHostKeyChecking=no " + std::string(remote) + " "
        + shell_single_quoted(remote_py.c_str()) + " < " + shell_single_quoted(agent_path) + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    BOOST_REQUIRE_NE(pipe, nullptr);
    const int fd = fileno(pipe);
    BOOST_REQUIRE_GE(fd, 0);
    BOOST_REQUIRE_EQUAL(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK), 0);

    remote_log.clear();
    char chunk[2048];
    bool sent_seen = false;
    const auto wall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    for (;;) {
        if (std::chrono::steady_clock::now() > wall_deadline)
            break;
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int pr = poll(&pfd, 1, 300);
        if (pr < 0 && errno != EINTR)
            break;
        if (pfd.revents & (POLLERR | POLLHUP)) {
            if (pfd.revents & POLLIN) {
                const ssize_t n = read(fd, chunk, sizeof(chunk));
                if (n > 0)
                    remote_log.append(chunk, static_cast<std::size_t>(n));
            }
            break;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            const ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n == 0)
                break;
            if (n > 0)
                remote_log.append(chunk, static_cast<std::size_t>(n));
            if (!sent_seen && remote_log.find("SENT") != std::string::npos) {
                sent_seen = true;
                on_sent();
            }
        }
    }
    (void)pclose(pipe);
}

} // namespace

BOOST_AUTO_TEST_CASE(packet_io_e2e_gpunetio_remote_ipv4)
{
    if (geteuid() != 0) {
        BOOST_TEST_MESSAGE("skip: run as root for DOCA (e.g. sudo -E ...)");
        return;
    }
    if (std::getenv("SNAKEIO_DOCA_GPUNETIO") == nullptr) {
        BOOST_TEST_MESSAGE("skip: set SNAKEIO_DOCA_GPUNETIO=1 to exercise DOCA ingress.");
        return;
    }
    const char* remote = std::getenv("SNAKEIO_DOCA_PACKET_IO_REMOTE");
    if (remote == nullptr || remote[0] == '\0') {
        BOOST_TEST_MESSAGE("skip: set SNAKEIO_DOCA_PACKET_IO_REMOTE (e.g. ubuntu@10.10.10.1).");
        return;
    }
    const char* dst = std::getenv("SNAKEIO_DOCA_PACKET_IO_DST");
    if (dst == nullptr || dst[0] == '\0') {
        BOOST_TEST_MESSAGE("skip: set SNAKEIO_DOCA_PACKET_IO_DST to this host's IPv4 (data plane).");
        return;
    }
    const char* ssh_pass = std::getenv("SNAKEIO_DOCA_PACKET_IO_SSH_PASS");
    if (ssh_pass == nullptr || ssh_pass[0] == '\0')
        ssh_pass = std::getenv("SSHPASS");
    const char* agent = std::strlen(SNAKEIO_DOCA_REMOTE_AGENT) > 0 ? SNAKEIO_DOCA_REMOTE_AGENT : std::getenv("SNAKEIO_DOCA_PACKET_IO_AGENT");
    BOOST_REQUIRE_MESSAGE(agent != nullptr && agent[0] != '\0',
        "compile with SNAKEIO_DOCA_REMOTE_AGENT or set SNAKEIO_DOCA_PACKET_IO_AGENT to doc_packet_io_remote_agent.py");

    // Remote recv tail after SENT; keep generous for slow SSH / scheduling (GPU RX also retries in tick).
    constexpr int k_remote_recv_tail_ms = 8000;

    const int data_sock = snakeio::game::open_data_port();
    BOOST_REQUIRE_GE(data_sock, 0);

    snakeio::game game;
    const snakeio::key_t key = test_key();
    const auto sid = game.add_session(1, 0, 1, std::span(&key, 1));
    BOOST_REQUIRE(sid.has_value());

    constexpr snakeio::id_t kPlayerId = 0;
    std::string remote_log;

    /* Tick 0: same shape as tests/packet_io.cpp `packet_egress_e2e` first ingress. */
    {
        const auto raw = make_ingress_packet(key, *sid, kPlayerId, false, false,
            std::numeric_limits<float>::quiet_NaN(), 1);
        const std::string b64 = b64_encode(std::span<const std::byte>(raw.data(), raw.size()));
        run_remote_ipv4_agent(remote, ssh_pass, agent, dst, static_cast<int>(snakeio::data_plane_ext_port), k_remote_recv_tail_ms,
            {b64},
            [&] {
                std::stop_token st{};
                game.tick(st, data_sock);
            },
            remote_log);
        auto pkts = parse_pkts_from_remote_log(remote_log);
        if (pkts.empty()) {
            const std::string tail =
                remote_log.size() > 1200 ? remote_log.substr(remote_log.size() - 1200) : remote_log;
            BOOST_TEST_INFO("remote_log (tail if long): " << tail);
            BOOST_TEST_INFO(
                "If you see SENT but no PKT, isolate NIC→GPU RX with target doca_gpunetio_toy_gpu_udp_reach "
                "(see backend/data/instructions.md).");
        }
        BOOST_REQUIRE(!pkts.empty());
        auto types0 = decrypt_types(pkts, key, 0, kPlayerId);
        BOOST_CHECK(types0.contains(1));
    }

    {
        const auto raw = make_ingress_packet(key, *sid, kPlayerId, false, false,
            std::numeric_limits<float>::quiet_NaN(), 2);
        const std::string b64 = b64_encode(std::span<const std::byte>(raw.data(), raw.size()));
        remote_log.clear();
        run_remote_ipv4_agent(remote, ssh_pass, agent, dst, static_cast<int>(snakeio::data_plane_ext_port), k_remote_recv_tail_ms,
            {b64},
            [&] {
                std::stop_token st{};
                game.tick(st, data_sock);
            },
            remote_log);
        auto pkts = parse_pkts_from_remote_log(remote_log);
        if (pkts.empty()) {
            const std::string tail =
                remote_log.size() > 1200 ? remote_log.substr(remote_log.size() - 1200) : remote_log;
            BOOST_TEST_INFO("remote_log round2 (tail): " << tail);
            BOOST_TEST_INFO(
                "If you see SENT but no PKT, isolate NIC→GPU RX with target doca_gpunetio_toy_gpu_udp_reach "
                "(see backend/data/instructions.md).");
        }
        BOOST_REQUIRE(!pkts.empty());
        auto types1 = decrypt_types(pkts, key, 1, kPlayerId);
        BOOST_CHECK(types1.contains(0));
        BOOST_CHECK(types1.contains(3));
    }

    close(data_sock);
}
