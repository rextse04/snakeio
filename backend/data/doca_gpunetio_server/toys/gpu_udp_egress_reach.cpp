/**
 * DOCA GPUNetIO **GPU Ethernet egress** toy: sends one UDP datagram from the NIC PF toward a peer
 * (e.g. DPU data-plane IPv4), using the same `DocaGpuIngress::emit_gpu_tx` path as `doca_gpunetio_server`.
 *
 * Run on the GPU host as root. Requires GPU TX constants (same as server logs):
 *   SNAKEIO_DOCA_TX_SRC_MAC, SNAKEIO_DOCA_TX_DST_MAC, SNAKEIO_DOCA_TX_SRC_IPV4
 * Plus data-plane bind for `try_init` (same as RX toy):
 *   SNAKEIO_DOCA_PACKET_IO_DST or SNAKEIO_DATA_PLANE_BIND_IPV4
 * Destination for the UDP header / `sockaddr`:
 *   SNAKEIO_DOCA_EGRESS_DST — peer IPv4 (e.g. DPU 10.10.10.1)
 * Optional:
 *   SNAKEIO_DOCA_EGRESS_UDP_PORT — destination UDP port (default 50004)
 *   SNAKEIO_NIC_PCIE, SNAKEIO_GPU_PCIE
 *
 * If `SNAKEIO_DOCA_PACKET_IO_REMOTE` is set, pipes `doc_udp_egress_listen_remote.py` over SSH, waits for
 * READY, emits, then expects `OK` on the remote stdout (verifies the datagram reached the DPU).
 */
#include <cuda_runtime.h>
#include "doca_gpunetio_net.hpp"
#include <config.hpp>
#include <doca_error.h>
#include <gpu_server/game_kernels.cuh>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <unistd.h>

#include <exception>
#include <vector>

namespace {

const char* env_or(const char* key, const char* fallback)
{
    const char* v = std::getenv(key);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

bool parse_mac(const char* s, std::uint8_t out[6]) noexcept
{
    if (s == nullptr || s[0] == '\0')
        return false;
    const int n = std::sscanf(s,
        "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &out[0],
        &out[1],
        &out[2],
        &out[3],
        &out[4],
        &out[5]);
    return n == 6;
}

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

/// Must match `MAGIC` in `scripts/doc_udp_egress_listen_remote.py`.
constexpr std::string_view k_magic{"SNAKEIO_GPUNETIO_EGRESS_PROBE_01"};
constexpr std::size_t k_pad = 16;

bool drain_ssh_until(FILE* pipe,
    std::string& log,
    const char* needle,
    std::chrono::steady_clock::time_point deadline)
{
    const int fd = fileno(pipe);
    char chunk[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int pr = poll(&pfd, 1, 300);
        if (pr < 0 && errno != EINTR)
            return false;
        if (pr > 0 && (pfd.revents & POLLIN)) {
            const ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n == 0)
                return false;
            if (n > 0)
                log.append(chunk, static_cast<std::size_t>(n));
            if (log.find(needle) != std::string::npos)
                return true;
        }
    }
    return false;
}

} // namespace

#ifndef SNAKEIO_DOCA_EGRESS_LISTEN_SCRIPT
#define SNAKEIO_DOCA_EGRESS_LISTEN_SCRIPT ""
#endif

int main()
{
    if (geteuid() != 0) {
        std::fprintf(stderr, "doca_gpunetio_toy_gpu_udp_egress_reach: run as root (DOCA flow).\n");
        return 2;
    }
    if (std::getenv("SNAKEIO_DOCA_PACKET_IO_DST") == nullptr && std::getenv("SNAKEIO_DATA_PLANE_BIND_IPV4") == nullptr) {
        std::fprintf(stderr,
            "Set SNAKEIO_DOCA_PACKET_IO_DST (or SNAKEIO_DATA_PLANE_BIND_IPV4) for kernel bind during DocaGpuIngress::try_init.\n");
        return 2;
    }
    const char* dst_peer = std::getenv("SNAKEIO_DOCA_EGRESS_DST");
    if (dst_peer == nullptr || dst_peer[0] == '\0') {
        std::fprintf(stderr, "Set SNAKEIO_DOCA_EGRESS_DST to the peer IPv4 (e.g. DPU data-plane address).\n");
        return 2;
    }
    const char* src_mac_e = std::getenv("SNAKEIO_DOCA_TX_SRC_MAC");
    const char* dst_mac_e = std::getenv("SNAKEIO_DOCA_TX_DST_MAC");
    const char* src_ip4_e = std::getenv("SNAKEIO_DOCA_TX_SRC_IPV4");
    std::uint8_t smac[6]{};
    std::uint8_t dmac[6]{};
    if (!parse_mac(src_mac_e, smac) || !parse_mac(dst_mac_e, dmac) || src_ip4_e == nullptr || src_ip4_e[0] == '\0') {
        std::fprintf(stderr,
            "GPU egress requires SNAKEIO_DOCA_TX_SRC_MAC, SNAKEIO_DOCA_TX_DST_MAC, and SNAKEIO_DOCA_TX_SRC_IPV4 "
            "(see server log line about GPU egress).\n");
        return 2;
    }
    (void)smac;
    (void)dmac;
    in_addr src_v4{};
    in_addr dst_v4{};
    if (inet_pton(AF_INET, src_ip4_e, &src_v4) != 1 || inet_pton(AF_INET, dst_peer, &dst_v4) != 1) {
        std::fprintf(stderr, "Invalid SNAKEIO_DOCA_TX_SRC_IPV4 or SNAKEIO_DOCA_EGRESS_DST.\n");
        return 2;
    }

    const int udp_port = std::max(1, std::atoi(env_or("SNAKEIO_DOCA_EGRESS_UDP_PORT", "50004")));
    const char* nic = env_or("SNAKEIO_NIC_PCIE", "0000:bd:00.0");
    const char* gpu = env_or("SNAKEIO_GPU_PCIE", "0000:ab:00.0");

    snakeio::gpu::device_state gs{};
    try {
        snakeio::gpu::init_device_state(gs);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "init_device_state failed: %s\n", e.what());
        return 1;
    }
    const std::size_t clients_bytes =
        sizeof(sockaddr_storage) * static_cast<std::size_t>(snakeio::game_max_sessions)
        * static_cast<std::size_t>(snakeio::game_max_players);
    snakeio::gpu::init_client_addrs_gpu(gs, clients_bytes);

    cudaStream_t stream = reinterpret_cast<cudaStream_t>(gs.stream);

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<std::uint16_t>(udp_port));
    peer.sin_addr = dst_v4;
    sockaddr_storage peer_ss{};
    std::memcpy(&peer_ss, &peer, sizeof(peer));
    cudaMemcpyAsync(gs.client_addrs,
        &peer_ss,
        sizeof(peer_ss),
        cudaMemcpyHostToDevice,
        stream);

    std::vector<std::byte> payload(k_magic.size() + k_pad);
    std::memcpy(payload.data(), k_magic.data(), k_magic.size());
    std::memset(payload.data() + k_magic.size(), 0, k_pad);
    cudaMemcpyAsync(gs.packet_ring,
        payload.data(),
        payload.size(),
        cudaMemcpyHostToDevice,
        stream);

    snakeio::gpu::send_desc sd{};
    sd.session_id = 0;
    sd.player_id = 0;
    sd.ring_offset = 0;
    sd.bytes_size = payload.size();
    cudaMemcpyAsync(gs.send_descs, &sd, sizeof(sd), cudaMemcpyHostToDevice, stream);
    const unsigned one = 1u;
    cudaMemcpyAsync(gs.send_descs_size, &one, sizeof(one), cudaMemcpyHostToDevice, stream);
    cudaStreamSynchronize(stream);

    std::byte* packet_ring = reinterpret_cast<std::byte*>(gs.packet_ring);
    snakeio::doca_gpunetio::DocaGpuIngress doca;
    const doca_error_t ir =
        doca.try_init(nic, gpu, gs.cuda_device_id, packet_ring, gs.packet_ring_capacity);
    if (ir != DOCA_SUCCESS) {
        std::fprintf(stderr, "DocaGpuIngress::try_init failed: %s\n", doca_error_get_descr(ir));
        snakeio::gpu::destroy_client_addrs_gpu(gs);
        snakeio::gpu::destroy_device_state(gs);
        return 1;
    }
    if (!doca.gpu_tx_ready()) {
        std::fprintf(stderr,
            "GPU egress not enabled after try_init (check TX MAC/IP env and doca logs).\n");
        doca.shutdown();
        snakeio::gpu::destroy_client_addrs_gpu(gs);
        snakeio::gpu::destroy_device_state(gs);
        return 2;
    }

    FILE* ssh_pipe = nullptr;
    std::string remote_log;
    const char* remote = std::getenv("SNAKEIO_DOCA_PACKET_IO_REMOTE");
    const char* listen_script =
        (std::strlen(SNAKEIO_DOCA_EGRESS_LISTEN_SCRIPT) > 0) ? SNAKEIO_DOCA_EGRESS_LISTEN_SCRIPT
                                                             : std::getenv("SNAKEIO_DOCA_EGRESS_LISTEN_SCRIPT");
    const char* ssh_pass = std::getenv("SNAKEIO_DOCA_PACKET_IO_SSH_PASS");
    if (ssh_pass == nullptr || ssh_pass[0] == '\0')
        ssh_pass = std::getenv("SSHPASS");

    if (remote != nullptr && remote[0] != '\0' && listen_script != nullptr && listen_script[0] != '\0') {
        const bool use_sshpass = ssh_pass != nullptr && ssh_pass[0] != '\0';
        const std::string remote_py = std::string("python3 -u - ") + std::to_string(udp_port);
        std::string cmd;
        if (use_sshpass)
            cmd += "sshpass -p " + shell_single_quoted(ssh_pass) + " ";
        cmd += "ssh -o StrictHostKeyChecking=no " + std::string(remote) + " "
            + shell_single_quoted(remote_py.c_str()) + " < " + shell_single_quoted(listen_script) + " 2>&1";
        ssh_pipe = popen(cmd.c_str(), "r");
        if (ssh_pipe == nullptr) {
            std::fprintf(stderr, "popen(ssh listener) failed.\n");
            doca.shutdown();
            snakeio::gpu::destroy_client_addrs_gpu(gs);
            snakeio::gpu::destroy_device_state(gs);
            return 1;
        }
        const int sfd = fileno(ssh_pipe);
        if (sfd >= 0)
            fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL, 0) | O_NONBLOCK);
        const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        if (!drain_ssh_until(ssh_pipe, remote_log, "READY", ready_deadline)) {
            std::fprintf(stderr, "Timed out waiting for READY from remote listener. Log tail:\n%.*s\n",
                static_cast<int>(std::min(remote_log.size(), std::size_t{800})),
                remote_log.c_str());
            pclose(ssh_pipe);
            doca.shutdown();
            snakeio::gpu::destroy_client_addrs_gpu(gs);
            snakeio::gpu::destroy_device_state(gs);
            return 1;
        }
    }

    const doca_error_t er = doca.emit_gpu_tx(stream, gs);
    if (er != DOCA_SUCCESS) {
        std::fprintf(stderr, "emit_gpu_tx failed: %s\n", doca_error_get_descr(er));
        if (ssh_pipe)
            pclose(ssh_pipe);
        doca.shutdown();
        snakeio::gpu::destroy_client_addrs_gpu(gs);
        snakeio::gpu::destroy_device_state(gs);
        return 1;
    }

    int exit_code = 0;
    if (ssh_pipe != nullptr) {
        const auto ok_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        if (!drain_ssh_until(ssh_pipe, remote_log, "OK", ok_deadline)) {
            std::fprintf(stderr,
                "Remote did not report OK (egress may not have reached DPU). Log tail:\n%.*s\n",
                static_cast<int>(std::min(remote_log.size(), std::size_t{1200})),
                remote_log.c_str());
            exit_code = 1;
        } else {
            std::fprintf(stderr, "doca_gpunetio_toy_gpu_udp_egress_reach: remote confirmed OK.\n");
        }
        pclose(ssh_pipe);
    } else {
        std::fprintf(stderr,
            "doca_gpunetio_toy_gpu_udp_egress_reach: emit_gpu_tx succeeded (no SNAKEIO_DOCA_PACKET_IO_REMOTE / "
            "listen script — verify on DPU with tcpdump or nc).\n");
    }

    doca.shutdown();
    snakeio::gpu::destroy_client_addrs_gpu(gs);
    snakeio::gpu::destroy_device_state(gs);
    return exit_code;
}
