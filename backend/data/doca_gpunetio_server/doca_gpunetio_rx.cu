#include "doca_gpunetio_rx_stage.cuh"
#include <doca_gpunetio_dev_eth_rxq.cuh>
#include <cuda_runtime.h>
#include <cstring>

namespace {

__device__ constexpr std::uint16_t kAfInet = 2;
__device__ constexpr std::uint16_t kAfInet6 = 10;

__device__ bool parse_ipv6_udp(uint8_t* pkt, std::uint32_t frame_len,
    std::uint64_t* out_payload_va, std::uint32_t* out_payload_len,
    std::uint32_t* out_src_ip6_be4, std::uint16_t* out_src_port)
{
    // Eth + IPv6 (40) + UDP (8) minimum
    if (frame_len < 62)
        return false;
    if (pkt[12] != 0x86 || pkt[13] != 0xDD)
        return false;
    constexpr std::uint32_t ip6_off = 14;
    const std::uint8_t nh = pkt[ip6_off + 6];
    if (nh != 17u)
        return false;
    const std::uint16_t ip_payload_len =
        (static_cast<std::uint32_t>(pkt[ip6_off + 4]) << 8) | static_cast<std::uint32_t>(pkt[ip6_off + 5]);
    constexpr std::uint32_t udp_off = ip6_off + 40u;
    if (udp_off + 8u > frame_len || ip_payload_len < 8u)
        return false;
    const std::uint16_t udp_len =
        (static_cast<std::uint32_t>(pkt[udp_off + 4]) << 8) | static_cast<std::uint32_t>(pkt[udp_off + 5]);
    if (udp_len < 8u || static_cast<std::uint32_t>(udp_len) != static_cast<std::uint32_t>(ip_payload_len))
        return false;
    const std::uint32_t plen = static_cast<std::uint32_t>(udp_len) - 8u;
    const std::uint32_t p0 = udp_off + 8u;
    if (p0 + plen > frame_len)
        return false;
    for (int w = 0; w < 4; ++w)
        __builtin_memcpy(out_src_ip6_be4 + w, pkt + ip6_off + 8u + static_cast<std::uint32_t>(w) * 4u, 4u);
    std::uint16_t spo{};
    __builtin_memcpy(&spo, pkt + udp_off, sizeof(spo));
    *out_payload_va = reinterpret_cast<std::uint64_t>(pkt + p0);
    *out_payload_len = plen;
    *out_src_port = spo;
    return true;
}

__device__ bool parse_ipv4_udp(uint8_t* pkt, std::uint32_t frame_len,
    std::uint64_t* out_payload_va, std::uint32_t* out_payload_len,
    std::uint32_t* out_src_ip, std::uint16_t* out_src_port)
{
    if (frame_len < 42)
        return false;
    if (pkt[12] != 0x08 || pkt[13] != 0x00)
        return false;
    const std::uint32_t ip0 = 14u;
    const std::uint32_t ihl = (pkt[14] & 0x0fu) * 4u;
    if (pkt[ip0 + 9u] != 17u)
        return false;
    if (ip0 + ihl + 8u > frame_len)
        return false;
    const std::uint32_t udp0 = ip0 + ihl;
    const std::uint32_t udp_len =
        (static_cast<std::uint32_t>(pkt[udp0 + 4]) << 8) | static_cast<std::uint32_t>(pkt[udp0 + 5]);
    if (udp_len < 8u)
        return false;
    const std::uint32_t plen = udp_len - 8u;
    const std::uint32_t p0 = udp0 + 8u;
    if (p0 + plen > frame_len)
        return false;
    std::uint32_t sip{};
    std::uint16_t spo{};
    __builtin_memcpy(&sip, pkt + ip0 + 12u, sizeof(sip));
    __builtin_memcpy(&spo, pkt + udp0, sizeof(spo));
    *out_payload_va = reinterpret_cast<std::uint64_t>(pkt + p0);
    *out_payload_len = plen;
    *out_src_ip = sip;
    *out_src_port = spo;
    return true;
}

__global__ void snakeio_doca_rx_tick_kernel(struct doca_gpu_eth_rxq* rxq,
    snakeio_doca_rx_stage_entry* stage,
    std::uint32_t* pkt_count)
{
    __shared__ std::uint64_t first_idx;
    __shared__ std::uint32_t n_pkts;
    __shared__ doca_gpu_dev_eth_rxq_attr attrs[SNAKEIO_DOCA_RX_BATCH_MAX];

    doca_error_t ret = doca_gpu_dev_eth_rxq_recv<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK,
        DOCA_GPUNETIO_ETH_MCST_AUTO,
        DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
        DOCA_GPUNETIO_ETH_RX_ATTR_ALL>(
        rxq, SNAKEIO_DOCA_RX_BATCH_MAX, 5'000'000ull, &first_idx, &n_pkts, attrs);

    __syncthreads();

    if (threadIdx.x == 0)
        *pkt_count = (ret == DOCA_SUCCESS) ? n_pkts : 0;

    __syncthreads();

    if (ret != DOCA_SUCCESS)
        return;

    for (std::uint32_t i = threadIdx.x; i < n_pkts && i < SNAKEIO_DOCA_RX_BATCH_MAX; i += blockDim.x) {
        const std::uint64_t pkt_va = doca_gpu_dev_eth_rxq_get_pkt_addr(rxq, first_idx + i);
        auto* pkt = reinterpret_cast<std::uint8_t*>(pkt_va);
        const std::uint32_t flen = attrs[i].bytes;
        snakeio_doca_rx_stage_entry& e = stage[i];
        e.valid = 0;
        std::uint64_t pva{};
        std::uint32_t plen{};
        std::uint32_t sip4{};
        std::uint32_t sip6[4]{};
        std::uint16_t spo{};
        if (parse_ipv6_udp(pkt, flen, &pva, &plen, sip6, &spo)) {
            e.addr_family = kAfInet6;
            for (int w = 0; w < 4; ++w)
                e.src_ipv6_be[w] = sip6[w];
            e.src_ipv4_be = 0;
        } else if (parse_ipv4_udp(pkt, flen, &pva, &plen, &sip4, &spo)) {
            e.addr_family = kAfInet;
            e.src_ipv4_be = sip4;
            for (int w = 0; w < 4; ++w)
                e.src_ipv6_be[w] = 0;
        } else {
            continue;
        }
        e.payload_dev_va = pva;
        e.payload_len = plen;
        e.src_port_be = spo;
        e.valid = 1;
    }
}

} // namespace

doca_error_t snakeio_doca_rx_tick_launch(cudaStream_t stream,
    struct doca_gpu_eth_rxq* rxq_gpu,
    snakeio_doca_rx_stage_entry* stage_gpu,
    std::uint32_t* pkt_count_gpu)
{
    snakeio_doca_rx_tick_kernel<<<1, 32, 0, stream>>>(rxq_gpu, stage_gpu, pkt_count_gpu);
    if (cudaGetLastError() != cudaSuccess)
        return DOCA_ERROR_DRIVER;
    return DOCA_SUCCESS;
}
