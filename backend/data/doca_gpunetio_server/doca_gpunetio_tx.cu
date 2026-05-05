#include "doca_gpunetio_tx.cuh"

#include <cstring>

#include <gpu_server/game_kernels.cuh>
#include <doca_gpunetio_dev_eth_txq.cuh>

namespace snakeio::doca_gpunetio {
namespace {

__constant__ tx_frame_constants k_tx{};

/// Linux `sockaddr_storage` size (used for `client_addrs` stride).
constexpr std::size_t kSockaddrStorageBytes = 128;

/// Linux `sockaddr_*` `sa_family` values (peer address on RX path).
__device__ constexpr std::uint16_t kAfInet = 2;  // AF_INET
__device__ constexpr std::uint16_t kAfInet6 = 10; // AF_INET6

__device__ inline std::uint16_t bswap16(std::uint16_t v) noexcept
{
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
}

__device__ void build_hdr42(std::byte* hdr,
    std::uint32_t dst_ipv4_be,
    std::uint16_t dst_udp_port_be,
    std::uint32_t udp_payload_len) noexcept
{
    const std::uint16_t ip_total = static_cast<std::uint16_t>(20u + 8u + udp_payload_len);
    const std::uint16_t udp_total = static_cast<std::uint16_t>(8u + udp_payload_len);

    // Ethernet
    for (int i = 0; i < 6; ++i) hdr[i] = static_cast<std::byte>(k_tx.dst_mac[i]);
    for (int i = 0; i < 6; ++i) hdr[6 + i] = static_cast<std::byte>(k_tx.src_mac[i]);
    hdr[12] = std::byte{0x08};
    hdr[13] = std::byte{0x00};

    // IPv4
    hdr[14] = std::byte{0x45};
    hdr[15] = std::byte{0};
    const std::uint16_t ip_tot_be = bswap16(ip_total);
    std::memcpy(hdr + 16, &ip_tot_be, 2);
    hdr[18] = std::byte{0};
    hdr[19] = std::byte{0};
    hdr[20] = std::byte{0x40};
    hdr[21] = std::byte{0};
    hdr[22] = std::byte{64};
    hdr[23] = std::byte{17}; // UDP
    hdr[24] = std::byte{0};
    hdr[25] = std::byte{0};
    std::memcpy(hdr + 26, &k_tx.src_ipv4_be, 4);
    std::memcpy(hdr + 30, &dst_ipv4_be, 4);

    // UDP
    std::memcpy(hdr + 34, &k_tx.src_udp_port_be, 2);
    std::memcpy(hdr + 36, &dst_udp_port_be, 2);
    const std::uint16_t udp_len_be = bswap16(udp_total);
    std::memcpy(hdr + 38, &udp_len_be, 2);
    hdr[40] = std::byte{0};
    hdr[41] = std::byte{0};
}

__device__ void build_hdr62(std::byte* hdr,
    const std::uint8_t dst_ipv6[16],
    std::uint16_t dst_udp_port_be,
    std::uint32_t udp_payload_len) noexcept
{
    const std::uint16_t udp_total = static_cast<std::uint16_t>(8u + udp_payload_len);
    const std::uint16_t ip_payload = udp_total;

    for (int i = 0; i < 6; ++i) hdr[i] = static_cast<std::byte>(k_tx.dst_mac[i]);
    for (int i = 0; i < 6; ++i) hdr[6 + i] = static_cast<std::byte>(k_tx.src_mac[i]);
    hdr[12] = std::byte{0x86};
    hdr[13] = std::byte{0xDD};

    // IPv6: fixed 40-byte header, no extension headers.
    hdr[14] = std::byte{0x60};
    hdr[15] = std::byte{0};
    hdr[16] = std::byte{0};
    hdr[17] = std::byte{0};
    const std::uint16_t ip_pl_be = bswap16(ip_payload);
    std::memcpy(hdr + 18, &ip_pl_be, 2);
    hdr[20] = std::byte{17}; // UDP
    hdr[21] = std::byte{64};
    std::memcpy(hdr + 22, k_tx.src_ipv6, 16);
    std::memcpy(hdr + 38, dst_ipv6, 16);

    constexpr std::uint32_t udp_off = 54u;
    std::memcpy(hdr + udp_off, &k_tx.src_udp_port_be, 2);
    std::memcpy(hdr + udp_off + 2, &dst_udp_port_be, 2);
    const std::uint16_t udp_len_be = bswap16(udp_total);
    std::memcpy(hdr + udp_off + 4, &udp_len_be, 2);
    hdr[udp_off + 6] = std::byte{0};
    hdr[udp_off + 7] = std::byte{0};
}

/// One thread, serial sends; `cqe_slot` matches sequential CQ indices (see NVIDIA `gpunetio_simple_send_kernel`).
__global__ void k_tx_serial(struct doca_gpu_eth_txq* txq,
    const gpu::send_desc* send_descs,
    unsigned n,
    std::byte* packet_ring,
    std::byte* client_addrs,
    std::byte* hdr_slab,
    std::uint32_t hdr_mkey,
    std::uint32_t ring_mkey,
    std::size_t ring_capacity)
{
    if (threadIdx.x != 0 || blockIdx.x != 0)
        return;

    std::uint64_t cqe_slot = 0;
    for (unsigned i = 0; i < n; ++i) {
        const gpu::send_desc& d = send_descs[i];
        if (d.bytes_size == 0 || d.ring_offset + d.bytes_size > ring_capacity)
            continue;

        const std::size_t cidx =
            gpu::client_index(d.session_id, d.player_id) * kSockaddrStorageBytes;
        const std::byte* sa = client_addrs + cidx;
        std::uint16_t family{};
        std::memcpy(&family, sa, sizeof(family));
        std::uint32_t nbytes0 = 0;

        std::byte* hdr = hdr_slab + static_cast<std::size_t>(i) * 64u;
        if (family == kAfInet6) {
            std::uint16_t dst_port_be{};
            std::uint8_t dst_ip6[16]{};
            std::memcpy(&dst_port_be, sa + 2, sizeof(dst_port_be));
            std::memcpy(dst_ip6, sa + 8, sizeof(dst_ip6));
            // IPv4-mapped `::ffff:a.b.c.d` (see `poll_ingress_batch`); emit IPv4 Ethernet, not IPv6.
            const bool v4mapped = dst_ip6[10] == 0xff && dst_ip6[11] == 0xff;
            if (v4mapped && k_tx.have_src_v4 != 0u) {
                std::uint32_t dst_ip_be{};
                std::memcpy(&dst_ip_be, dst_ip6 + 12, sizeof(dst_ip_be));
                build_hdr42(hdr, dst_ip_be, dst_port_be, static_cast<std::uint32_t>(d.bytes_size));
                nbytes0 = 42u;
            } else if (!v4mapped && k_tx.have_src_v6 != 0u) {
                build_hdr62(hdr, dst_ip6, dst_port_be, static_cast<std::uint32_t>(d.bytes_size));
                nbytes0 = 62u;
            } else {
                continue;
            }
        } else if (family == kAfInet) {
            if (k_tx.have_src_v4 == 0u)
                continue;
            std::uint16_t dst_port_be{};
            std::uint32_t dst_ip_be{};
            std::memcpy(&dst_port_be, sa + 2, sizeof(dst_port_be));
            std::memcpy(&dst_ip_be, sa + 4, sizeof(dst_ip_be));
            build_hdr42(hdr, dst_ip_be, dst_port_be, static_cast<std::uint32_t>(d.bytes_size));
            nbytes0 = 42u;
        } else {
            continue;
        }

        const std::uint64_t payload_va =
            reinterpret_cast<std::uint64_t>(packet_ring + d.ring_offset);

        const std::uint64_t wqe_idx = doca_gpu_dev_eth_txq_reserve_wq_slots(txq, 1);
        struct doca_gpu_dev_eth_txq_wqe* wqe =
            doca_gpu_dev_eth_txq_get_wqe_ptr(txq, static_cast<std::uint16_t>(wqe_idx));

        const doca_error_t pr = doca_gpu_dev_eth_txq_wqe_prepare_send(txq,
            wqe,
            static_cast<std::uint16_t>(wqe_idx),
            reinterpret_cast<std::uint64_t>(hdr),
            hdr_mkey,
            nbytes0,
            payload_va,
            ring_mkey,
            static_cast<std::uint32_t>(d.bytes_size),
            DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY);
        if (pr != DOCA_SUCCESS)
            return;

        doca_gpu_dev_eth_txq_mark_wqes_ready(txq, wqe_idx, wqe_idx);
        doca_gpu_dev_eth_txq_submit(txq, wqe_idx + 1);

        doca_error_t pst = doca_gpu_dev_eth_txq_poll_completion_at(txq,
            cqe_slot,
            DOCA_GPUNETIO_ETH_WAIT_FLAG_B);
        if (pst != DOCA_SUCCESS)
            return;
        ++cqe_slot;
    }
}

} // namespace

doca_error_t tx_upload_constants(const tx_frame_constants& c) noexcept
{
    const cudaError_t e =
        cudaMemcpyToSymbol(k_tx, &c, sizeof(c), 0, cudaMemcpyHostToDevice);
    return e == cudaSuccess ? DOCA_SUCCESS : DOCA_ERROR_DRIVER;
}

doca_error_t tx_emit_serial(cudaStream_t stream,
    doca_gpu_eth_txq* txq_gpu,
    gpu::device_state& gs,
    std::byte* hdr_slab_gpu,
    std::uint32_t hdr_mkey_be,
    std::uint32_t ring_mkey_be,
    std::size_t ring_capacity,
    unsigned send_count) noexcept
{
    if (txq_gpu == nullptr || hdr_slab_gpu == nullptr || send_count == 0)
        return DOCA_ERROR_INVALID_VALUE;

    k_tx_serial<<<1, 1, 0, stream>>>(txq_gpu,
        gs.send_descs,
        send_count,
        gs.packet_ring,
        gs.client_addrs,
        hdr_slab_gpu,
        hdr_mkey_be,
        ring_mkey_be,
        ring_capacity);

    const cudaError_t ce = cudaGetLastError();
    return ce == cudaSuccess ? DOCA_SUCCESS : DOCA_ERROR_DRIVER;
}

} // namespace snakeio::doca_gpunetio
