#pragma once

#include <cstddef>
#include <cstdint>

#include <cuda_runtime.h>
#include <doca_error.h>

struct doca_gpu_eth_txq;

namespace snakeio::gpu {
    struct device_state;
}

namespace snakeio::doca_gpunetio {

/// L2/L3/L4 constants for UDP frames (client IP/port come from `client_addrs` per packet).
/// When both `have_src_v4` and `have_src_v6` are set, egress follows each peer's address family.
struct tx_frame_constants {
    std::uint8_t src_mac[6]{};
    std::uint8_t dst_mac[6]{};
    std::uint16_t src_udp_port_be{};
    std::uint8_t have_src_v4{};
    std::uint8_t have_src_v6{};
    std::uint32_t src_ipv4_be{}; ///< `in_addr` network byte order
    std::uint8_t src_ipv6[16]{}; ///< `in6_addr` wire bytes
};

doca_error_t tx_upload_constants(const tx_frame_constants& c) noexcept;

/// GPU TX: one thread serializes sends (two-segment WQE: 42-byte L2/L3/L4 hdr + payload in `packet_ring`).
doca_error_t tx_emit_serial(cudaStream_t stream,
    doca_gpu_eth_txq* txq_gpu,
    gpu::device_state& gs,
    std::byte* hdr_slab_gpu,
    std::uint32_t hdr_mkey_be,
    std::uint32_t ring_mkey_be,
    std::size_t ring_capacity,
    unsigned send_count) noexcept;

} // namespace snakeio::doca_gpunetio
