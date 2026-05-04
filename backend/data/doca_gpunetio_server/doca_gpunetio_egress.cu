#include <doca_gpunetio_dev_eth_txq.cuh>

#include <gpu_server/game_kernels.cuh>
#include <config.hpp>

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace {

constexpr unsigned kMaxFrame = 2048;
constexpr unsigned kIpv4UdpHdr = 14U + 20U + 8U;
constexpr unsigned kIpv6UdpHdr = 14U + 40U + 8U;
constexpr uint16_t kAfInet = 2;
constexpr uint16_t kAfInet6 = 10;
constexpr uint8_t kIpprotoUdp = 17;

__device__ void dev_write_u16_wire(uint8_t* dst, uint16_t host_packed_be_bits) noexcept {
    const auto* p = reinterpret_cast<const uint8_t*>(&host_packed_be_bits);
    dst[0] = p[0];
    dst[1] = p[1];
}

__device__ bool dev_eth_all_zero(const std::byte* e) noexcept {
    for (int i = 0; i < 6; ++i) {
        if (e[i] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

__device__ void dev_pick_l2_dst(uint8_t l2_dst[6],
    const std::byte* dst_eth,
    const uint8_t* gateway_mac,
    bool has_gateway_mac) noexcept {
    if (dst_eth == nullptr || dev_eth_all_zero(dst_eth)) {
        if (has_gateway_mac) {
            for (int i = 0; i < 6; ++i) {
                l2_dst[i] = gateway_mac[i];
            }
        } else {
            for (int i = 0; i < 6; ++i) {
                l2_dst[i] = 0;
            }
        }
    } else {
        for (int i = 0; i < 6; ++i) {
            l2_dst[i] = static_cast<uint8_t>(dst_eth[i]);
        }
    }
}

__device__ bool dev_v4mapped(const uint8_t* a) noexcept {
    for (int i = 0; i < 10; ++i) {
        if (a[i] != 0) {
            return false;
        }
    }
    return a[10] == 0xff && a[11] == 0xff;
}

__device__ bool dev_extract_ipv4_udp_dest(const void* ss,
    uint8_t ip4_out[4],
    uint16_t* udp_dst_port_be) noexcept {
    const auto* b = reinterpret_cast<const uint8_t*>(ss);
    const uint16_t fam = *reinterpret_cast<const uint16_t*>(ss);
    if (fam == kAfInet) {
        for (int i = 0; i < 4; ++i) {
            ip4_out[i] = b[4 + i];
        }
        *udp_dst_port_be = *reinterpret_cast<const uint16_t*>(b + 2);
        return true;
    }
    if (fam == kAfInet6) {
        const uint8_t* addr = b + 8;
        if (!dev_v4mapped(addr)) {
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            ip4_out[i] = addr[12 + i];
        }
        *udp_dst_port_be = *reinterpret_cast<const uint16_t*>(b + 2);
        return true;
    }
    return false;
}

__device__ bool dev_extract_ipv6_udp_dest(const void* ss,
    bool has_local_ip6,
    uint8_t ip6_out[16],
    uint16_t* udp_dst_port_be) noexcept {
    if (!has_local_ip6) {
        return false;
    }
    const auto* b = reinterpret_cast<const uint8_t*>(ss);
    const uint16_t fam = *reinterpret_cast<const uint16_t*>(ss);
    if (fam != kAfInet6) {
        return false;
    }
    const uint8_t* addr = b + 8;
    if (dev_v4mapped(addr)) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        ip6_out[i] = addr[i];
    }
    *udp_dst_port_be = *reinterpret_cast<const uint16_t*>(b + 2);
    return true;
}

__device__ bool dev_build_ipv4_udp(uint8_t* hdr,
    unsigned& out_hdr_len,
    const uint8_t* l2_dst,
    const uint8_t* nic_mac,
    const uint8_t* src_ip4,
    const uint8_t* dst_ip4,
    uint16_t src_udp_port_be,
    uint16_t dst_udp_port_be,
    snakeio::size_t payload_len) noexcept {
    if (static_cast<unsigned>(payload_len) + kIpv4UdpHdr > kMaxFrame) {
        return false;
    }
    for (unsigned i = 0; i < 64; ++i) {
        hdr[i] = 0;
    }
    for (int i = 0; i < 6; ++i) {
        hdr[i] = l2_dst[i];
    }
    for (int i = 0; i < 6; ++i) {
        hdr[6 + i] = nic_mac[i];
    }
    hdr[12] = 0x08;
    hdr[13] = 0x00;

    const uint16_t udp_len = static_cast<uint16_t>(8U + static_cast<unsigned>(payload_len));
    const uint16_t ip_len = static_cast<uint16_t>(20U + udp_len);

    unsigned o = 14;
    hdr[o++] = 0x45;
    hdr[o++] = 0;
    hdr[o++] = static_cast<uint8_t>(ip_len >> 8);
    hdr[o++] = static_cast<uint8_t>(ip_len & 0xff);
    hdr[o++] = 0;
    hdr[o++] = 0;
    hdr[o++] = 0;
    hdr[o++] = 0;
    hdr[o++] = 64;
    hdr[o++] = kIpprotoUdp;
    hdr[o++] = 0;
    hdr[o++] = 0;
    for (int i = 0; i < 4; ++i) {
        hdr[o++] = src_ip4[i];
    }
    for (int i = 0; i < 4; ++i) {
        hdr[o++] = dst_ip4[i];
    }

    dev_write_u16_wire(hdr + o, src_udp_port_be);
    o += 2;
    dev_write_u16_wire(hdr + o, dst_udp_port_be);
    o += 2;
    hdr[o++] = static_cast<uint8_t>((udp_len >> 8) & 0xff);
    hdr[o++] = static_cast<uint8_t>(udp_len & 0xff);
    hdr[o++] = 0;
    hdr[o++] = 0;

    out_hdr_len = kIpv4UdpHdr;
    return true;
}

__device__ bool dev_build_ipv6_udp(uint8_t* hdr,
    unsigned& out_hdr_len,
    const uint8_t* l2_dst,
    const uint8_t* nic_mac,
    const uint8_t* src_ip6,
    const uint8_t* dst_ip6,
    uint16_t src_udp_port_be,
    uint16_t dst_udp_port_be,
    snakeio::size_t payload_len) noexcept {
    if (static_cast<unsigned>(payload_len) + kIpv6UdpHdr > kMaxFrame) {
        return false;
    }
    for (unsigned i = 0; i < 64; ++i) {
        hdr[i] = 0;
    }
    for (int i = 0; i < 6; ++i) {
        hdr[i] = l2_dst[i];
    }
    for (int i = 0; i < 6; ++i) {
        hdr[6 + i] = nic_mac[i];
    }
    hdr[12] = 0x86;
    hdr[13] = 0xdd;

    const uint16_t ipv6_payload_u16 = static_cast<uint16_t>(8U + static_cast<unsigned>(payload_len));
    unsigned o = 14;
    hdr[o++] = 0x60;
    hdr[o++] = 0;
    hdr[o++] = 0;
    hdr[o++] = 0;
    hdr[o++] = static_cast<uint8_t>(ipv6_payload_u16 >> 8);
    hdr[o++] = static_cast<uint8_t>(ipv6_payload_u16 & 0xff);
    hdr[o++] = kIpprotoUdp;
    hdr[o++] = 64;
    for (int i = 0; i < 16; ++i) {
        hdr[o++] = src_ip6[i];
    }
    for (int i = 0; i < 16; ++i) {
        hdr[o++] = dst_ip6[i];
    }

    dev_write_u16_wire(hdr + o, src_udp_port_be);
    o += 2;
    dev_write_u16_wire(hdr + o, dst_udp_port_be);
    o += 2;
    const uint16_t ulen = static_cast<uint16_t>(8U + static_cast<unsigned>(payload_len));
    hdr[o++] = static_cast<uint8_t>((ulen >> 8) & 0xff);
    hdr[o++] = static_cast<uint8_t>(ulen & 0xff);
    hdr[o++] = 0;
    hdr[o++] = 0;

    out_hdr_len = kIpv6UdpHdr;
    return true;
}

__device__ void dev_memcpy_payload(uint8_t* dst, const std::byte* src, snakeio::size_t n) noexcept {
    for (snakeio::size_t i = 0; i < n; ++i) {
        dst[i] = static_cast<uint8_t>(src[i]);
    }
}

__device__ void dev_send_one_frame(struct doca_gpu_eth_txq* txq,
    uint8_t* pkt_gpu,
    uint32_t mkey_be,
    uint32_t frame_len) noexcept {
    const uint16_t wqe_idx = 0;
    enum doca_gpu_eth_send_flags flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NONE;
    const uint64_t addr = reinterpret_cast<uint64_t>(pkt_gpu);

    if (threadIdx.x == (blockDim.x - 1)) {
        flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;
    }

    struct doca_gpu_dev_eth_txq_wqe* wqe_ptr = doca_gpu_dev_eth_txq_get_wqe_ptr(txq, wqe_idx);
    doca_gpu_dev_eth_txq_wqe_prepare_send(txq, wqe_ptr, wqe_idx, addr, mkey_be, frame_len, flags);
    __syncthreads();

    if (threadIdx.x == (blockDim.x - 1)) {
        doca_gpu_dev_eth_txq_submit(txq, wqe_idx + 1);
        (void)doca_gpu_dev_eth_txq_poll_completion_at<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_GPU,
            DOCA_GPUNETIO_ETH_SYNC_SCOPE_CTA>(txq, 0U, DOCA_GPUNETIO_ETH_WAIT_FLAG_B);
    }
    __syncthreads();
}

__global__ void snakeio_doca_tick_egress_kernel(struct doca_gpu_eth_txq* txq,
    uint8_t* pkt_gpu,
    uint32_t mkey_be,
    const snakeio::gpu::send_desc* descs,
    unsigned n,
    const std::byte* packet_ring,
    std::size_t packet_ring_capacity,
    const std::byte* client_addrs,
    unsigned sockaddr_storage_bytes,
    const std::byte* client_eth_dev,
    const uint8_t* nic_mac,
    const uint8_t* local_ip4,
    const uint8_t* local_ip6,
    int has_local_ip6,
    const uint8_t* gateway_mac,
    int has_gateway_mac,
    uint16_t src_udp_port_be) {
    if (threadIdx.x != 0 || blockIdx.x != 0) {
        return;
    }

    for (unsigned i = 0; i < n; ++i) {
        const snakeio::gpu::send_desc d = descs[i];
        if (d.session_id >= snakeio::game_max_sessions || d.player_id >= snakeio::game_max_players) {
            continue;
        }
        const snakeio::size_t payload_len = d.bytes_size;
        if (payload_len > packet_ring_capacity || d.ring_offset > packet_ring_capacity - payload_len) {
            continue;
        }
        const std::byte* payload_src = packet_ring + d.ring_offset;

        const std::size_t cix = snakeio::gpu::client_index(d.session_id, d.player_id);
        const void* ss = client_addrs + cix * static_cast<std::size_t>(sockaddr_storage_bytes);
        const std::byte* dst_eth = (client_eth_dev != nullptr) ? (client_eth_dev + cix * 6U) : nullptr;

        if ((dst_eth == nullptr || dev_eth_all_zero(dst_eth)) && !has_gateway_mac) {
            continue;
        }

        uint8_t l2_dst[6]{};
        dev_pick_l2_dst(l2_dst, dst_eth, gateway_mac, has_gateway_mac != 0);

        uint8_t dst_ip4[4]{};
        uint8_t dst_ip6[16]{};
        uint16_t dst_udp_port_be = 0;
        const bool to_v4 = dev_extract_ipv4_udp_dest(ss, dst_ip4, &dst_udp_port_be);
        const bool to_v6 = !to_v4 && (has_local_ip6 != 0) &&
            dev_extract_ipv6_udp_dest(ss, true, dst_ip6, &dst_udp_port_be);

        if (!to_v4 && !to_v6) {
            continue;
        }

        uint8_t hdr[64]{};
        unsigned hdr_len = 0;
        bool built = false;
        uint32_t total = 0;
        if (to_v4) {
            built = dev_build_ipv4_udp(hdr,
                hdr_len,
                l2_dst,
                nic_mac,
                local_ip4,
                dst_ip4,
                src_udp_port_be,
                dst_udp_port_be,
                payload_len);
            total = static_cast<uint32_t>(hdr_len + payload_len);
        } else {
            built = dev_build_ipv6_udp(hdr,
                hdr_len,
                l2_dst,
                nic_mac,
                local_ip6,
                dst_ip6,
                src_udp_port_be,
                dst_udp_port_be,
                payload_len);
            total = static_cast<uint32_t>(hdr_len + payload_len);
        }
        if (!built || total > kMaxFrame) {
            continue;
        }

        for (unsigned h = 0; h < hdr_len; ++h) {
            pkt_gpu[h] = hdr[h];
        }
        dev_memcpy_payload(pkt_gpu + hdr_len, payload_src, payload_len);

        dev_send_one_frame(txq, pkt_gpu, mkey_be, total);
    }
}

} // namespace

extern "C" cudaError_t snakeio_doca_gpunetio_emit_tick_egress_launch(cudaStream_t stream,
    struct doca_gpu_eth_txq* txq_gpu,
    uint8_t* pkt_gpu,
    uint32_t mkey_be,
    const snakeio::gpu::send_desc* descs_dev,
    unsigned n,
    const std::byte* packet_ring,
    std::size_t packet_ring_capacity,
    const std::byte* client_addrs,
    unsigned sockaddr_storage_bytes,
    const std::byte* client_eth_dev,
    const uint8_t* nic_mac,
    const uint8_t* local_ip4,
    const uint8_t* local_ip6,
    int has_local_ip6,
    const uint8_t* gateway_mac,
    int has_gateway_mac,
    uint16_t src_udp_port_be) noexcept {
    if (n == 0) {
        return cudaSuccess;
    }
    snakeio_doca_tick_egress_kernel<<<1, 1, 0, stream>>>(txq_gpu,
        pkt_gpu,
        mkey_be,
        descs_dev,
        n,
        packet_ring,
        packet_ring_capacity,
        client_addrs,
        sockaddr_storage_bytes,
        client_eth_dev,
        nic_mac,
        local_ip4,
        local_ip6,
        has_local_ip6,
        gateway_mac,
        has_gateway_mac,
        src_udp_port_be);
    return cudaGetLastError();
}
