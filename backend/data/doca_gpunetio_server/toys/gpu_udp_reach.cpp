/**
 * Minimal DOCA GPUNetIO RX probe: proves IPv4/UDP datagrams from the wire reach the GPU RX staging
 * (same init path as `DocaGpuIngress` / backend game).
 *
 * Run on the GPU host as root. From the DPU (or any sender on the data plane), send UDP to this
 * host's data-plane IPv4 and port 50003, e.g.:
 *   ./scripts/doca_udp_probe_send.sh 10.10.10.2 50003
 *
 * Env (same conventions as the server / remote IPv4 test):
 *   SNAKEIO_NIC_PCIE, SNAKEIO_GPU_PCIE — defaults 0000:bd:00.0 / 0000:ab:00.0
 *   SNAKEIO_DOCA_PACKET_IO_DST or SNAKEIO_DATA_PLANE_BIND_IPV4 — bind address for kernel sendto socket
 *   SNAKEIO_DOCA_TOY_TIMEOUT_SEC — wall-clock wait (default 120)
 */
#include "doca_gpunetio_net.hpp"
#include <doca_error.h>

#include <cuda_runtime.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

namespace {

const char* env_or(const char* key, const char* fallback)
{
    const char* v = std::getenv(key);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

} // namespace

int main()
{
    if (geteuid() != 0) {
        std::fprintf(stderr, "doca_gpunetio_toy_gpu_udp_reach: run as root (DOCA flow).\n");
        return 2;
    }
    if (std::getenv("SNAKEIO_DOCA_PACKET_IO_DST") == nullptr && std::getenv("SNAKEIO_DATA_PLANE_BIND_IPV4") == nullptr) {
        std::fprintf(stderr,
            "Set SNAKEIO_DOCA_PACKET_IO_DST (or SNAKEIO_DATA_PLANE_BIND_IPV4) to this host's data-plane IPv4 "
            "so the kernel egress socket can bind (same as remote E2E test).\n");
        return 2;
    }

    const char* nic = env_or("SNAKEIO_NIC_PCIE", "0000:bd:00.0");
    const char* gpu = env_or("SNAKEIO_GPU_PCIE", "0000:ab:00.0");
    const int timeout_sec = std::max(1, std::atoi(env_or("SNAKEIO_DOCA_TOY_TIMEOUT_SEC", "120")));

    std::byte* packet_ring = nullptr;
    constexpr std::size_t k_ring = 65536u;
    if (cudaMalloc(reinterpret_cast<void**>(&packet_ring), k_ring) != cudaSuccess) {
        std::fprintf(stderr, "cudaMalloc(packet_ring) failed.\n");
        return 1;
    }

    int exit_code = 1;
    {
        snakeio::doca_gpunetio::DocaGpuIngress doca;
        const doca_error_t ir = doca.try_init(nic, gpu, 0, packet_ring, k_ring);
        if (ir != DOCA_SUCCESS) {
            std::fprintf(stderr, "DocaGpuIngress::try_init failed: %s\n", doca_error_get_descr(ir));
            cudaFree(packet_ring);
            return 1;
        }

        cudaStream_t stream{};
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
            std::fprintf(stderr, "cudaStreamCreate failed.\n");
            cudaFree(packet_ring);
            return 1;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::uint64_t polls = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            ++polls;
            doca_error_t r = doca.receive_tick(stream);
            if (r != DOCA_SUCCESS) {
                std::fprintf(stderr, "receive_tick: %s\n", doca_error_get_descr(r));
                break;
            }
            (void)doca.progress_txq();
            const std::uint32_t n = doca.last_count();
            if (n == 0)
                continue;

            std::fprintf(stderr, "[toy] GPU RX batch: n=%u\n", static_cast<unsigned>(n));

            for (std::uint32_t i = 0; i < n; ++i) {
                const snakeio_doca_rx_stage_entry& e = doca.stage_host()[i];
                if (!e.valid || e.payload_len == 0)
                    continue;

                unsigned char buf[128]{};
                const std::size_t ncopy = std::min<std::size_t>(e.payload_len, sizeof(buf));
                if (cudaMemcpy(buf, reinterpret_cast<const void*>(e.payload_dev_va), ncopy, cudaMemcpyDeviceToHost)
                    != cudaSuccess) {
                    std::fprintf(stderr, "[toy] cudaMemcpy payload failed (idx %u).\n", static_cast<unsigned>(i));
                    continue;
                }

                char src[INET6_ADDRSTRLEN]{};
                if (e.addr_family == AF_INET) { // matches `doca_gpunetio_rx.cu` kAfInet
                    struct in_addr a {};
                    a.s_addr = e.src_ipv4_be;
                    if (inet_ntop(AF_INET, &a, src, sizeof(src)) == nullptr)
                        std::strncpy(src, "?", sizeof(src) - 1);
                } else {
                    struct in6_addr a6 {};
                    for (int w = 0; w < 4; ++w)
                        std::memcpy(a6.s6_addr + static_cast<std::size_t>(w) * 4u, &e.src_ipv6_be[w], 4u);
                    if (inet_ntop(AF_INET6, &a6, src, sizeof(src)) == nullptr)
                        std::strncpy(src, "?", sizeof(src) - 1);
                }
                const unsigned sport = ntohs(e.src_port_be);

                std::fprintf(stderr,
                    "[toy] Parsed UDP on GPU: src=%s sport=%u payload_len=%u first_hex=",
                    src,
                    sport,
                    static_cast<unsigned>(e.payload_len));
                for (std::size_t b = 0; b < std::min(ncopy, std::size_t{24}); ++b)
                    std::fprintf(stderr, "%02x", buf[b]);
                std::fprintf(stderr, "\n");

                exit_code = 0;
                break;
            }
            if (exit_code == 0)
                break;
        }

        if (exit_code != 0) {
            std::fprintf(stderr,
                "[toy] Timeout: no parsed UDP on GPU within %ds (receive_tick polls=%llu).\n"
                "  Send from remote: scripts/doca_udp_probe_send.sh <this-host-dp-ip> 50003\n"
                "  Check NIC PF, doca_flow RSS, and L2 path to SNAKEIO_NIC_PCIE=%s\n",
                timeout_sec,
                static_cast<unsigned long long>(polls),
                nic);
        }

        cudaStreamDestroy(stream);
    }

    cudaFree(packet_ring);
    return exit_code;
}
