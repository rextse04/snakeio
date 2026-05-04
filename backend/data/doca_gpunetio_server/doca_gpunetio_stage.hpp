#pragma once

#include <config.hpp>
#include <packet.hpp>

#include <cstddef>
#include <cuda_runtime_api.h>

struct doca_gpu_eth_rxq;

// Shared between host runtime (CPU side of DOCA_GPU_MEM_TYPE_GPU_CPU) and the CUDA receive kernel.
struct snakeio_doca_stage_cpu {
    volatile int stop;
    volatile int cpu_sem;
    volatile int gpu_sem;
    volatile uint32_t payload_len;
    volatile uint16_t src_port_be;
    volatile uint8_t src_is_ipv6;
    volatile uint8_t src_eth[6];
    volatile uint32_t src_ip4_be;
    volatile uint32_t src_ip6_be[4];
    std::byte payload[snakeio::in_packet_max_text_size + snakeio::data_packet::header_size];
};

extern "C" cudaError_t snakeio_doca_gpunetio_recv_launch(
    cudaStream_t stream, struct doca_gpu_eth_rxq* rxq_gpu, struct snakeio_doca_stage_cpu* stage);
