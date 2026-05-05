#pragma once
#include <cuda_runtime.h>
#include <doca_error.h>
#include <cstdint>

struct doca_gpu_eth_rxq;

/// RX staging: IPv6 (DOCA flow) primary; `addr_family` is `AF_INET` or `AF_INET6`.
struct snakeio_doca_rx_stage_entry {
    std::uint64_t payload_dev_va{};
    std::uint32_t payload_len{};
    std::uint16_t src_port_be{};
    std::uint16_t addr_family{}; ///< `AF_INET` (2) or `AF_INET6` (10)
    std::uint32_t src_ipv4_be{};
    std::uint32_t src_ipv6_be[4]{}; ///< four big-endian chunks, `in6_addr` wire order
    std::uint16_t valid{};
};

#ifndef SNAKEIO_DOCA_RX_BATCH_MAX
#define SNAKEIO_DOCA_RX_BATCH_MAX 256
#endif

doca_error_t snakeio_doca_rx_tick_launch(cudaStream_t stream,
    struct doca_gpu_eth_rxq* rxq_gpu,
    snakeio_doca_rx_stage_entry* stage_gpu,
    std::uint32_t* pkt_count_gpu);
