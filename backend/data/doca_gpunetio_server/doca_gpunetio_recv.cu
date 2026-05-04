#include <doca_gpunetio_dev_eth_rxq.cuh>

#include "doca_gpunetio_stage.hpp"

#include <cuda_runtime.h>

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <netinet/in.h>

namespace {
    constexpr unsigned cuda_block_threads = 32;
    constexpr uint32_t max_rx_num_pkts = 32;
    constexpr uint64_t max_rx_timeout_ns = 500'000; // 500us

    constexpr uint16_t ether_type_ipv4 = 0x0800;
    constexpr uint16_t ether_type_ipv6 = 0x86dd;

    template <enum doca_gpu_dev_eth_exec_scope exec_scope>
    __global__ void recv_udp_kernel(struct doca_gpu_eth_rxq* rxq, snakeio_doca_stage_cpu* stage) {
        doca_error_t ret;
        __shared__ uint64_t out_first_pkt_idx;
        __shared__ uint32_t out_pkt_num;
        __shared__ struct doca_gpu_dev_eth_rxq_attr out_attr[max_rx_num_pkts];

        while (DOCA_GPUNETIO_VOLATILE(stage->stop) == 0) {
            if (threadIdx.x == 0) {
                DOCA_GPUNETIO_VOLATILE(stage->gpu_sem) = DOCA_GPU_SEMAPHORE_STATUS_FREE;
            }
            __syncthreads();

            // Wait for CPU to mark READY (new output slot).
            while (DOCA_GPUNETIO_VOLATILE(stage->stop) == 0 &&
                DOCA_GPUNETIO_VOLATILE(stage->cpu_sem) != DOCA_GPU_SEMAPHORE_STATUS_READY) {
                // busy-wait
            }
            __syncthreads();
            if (DOCA_GPUNETIO_VOLATILE(stage->stop) != 0) {
                break;
            }

            if (threadIdx.x == 0) {
                ret = doca_gpu_dev_eth_rxq_recv<exec_scope,
                    DOCA_GPUNETIO_ETH_MCST_AUTO,
                    DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
                    DOCA_GPUNETIO_ETH_RX_ATTR_ALL>(
                    rxq,
                    max_rx_num_pkts,
                    max_rx_timeout_ns,
                    &out_first_pkt_idx,
                    &out_pkt_num,
                    out_attr);
                if (ret != DOCA_SUCCESS) {
                    DOCA_GPUNETIO_VOLATILE(stage->payload_len) = 0;
                    DOCA_GPUNETIO_VOLATILE(stage->gpu_sem) = DOCA_GPU_SEMAPHORE_STATUS_ERROR;
                    DOCA_GPUNETIO_VOLATILE(stage->cpu_sem) = DOCA_GPU_SEMAPHORE_STATUS_FREE;
                    DOCA_GPUNETIO_VOLATILE(stage->stop) = 1;
                }
            }
            __syncthreads();

            if (DOCA_GPUNETIO_VOLATILE(stage->stop) != 0) {
                break;
            }

            if (threadIdx.x == 0) {
                DOCA_GPUNETIO_VOLATILE(stage->payload_len) = 0;

                if (out_pkt_num != 0) {
                    const uint64_t pkt_addr = doca_gpu_dev_eth_rxq_get_pkt_addr(rxq, out_first_pkt_idx);
                    const uint32_t pkt_bytes = out_attr[0].bytes;

                    const std::byte* payload_ptr = nullptr;
                    uint32_t payload_len = 0;
                    bool is_ipv6 = false;
                    uint32_t src_ip4_be = 0;
                    uint32_t src_ip6_be[4] = {0, 0, 0, 0};
                    uint16_t src_port_be = 0;

                    if (pkt_addr != 0 && pkt_bytes >= 14 + 20 + 8) {
                        uint8_t hdr[256];
                        const uint32_t copy_len = pkt_bytes < sizeof(hdr) ? pkt_bytes : sizeof(hdr);
                        memcpy(hdr, reinterpret_cast<const void*>(pkt_addr), copy_len);

                        if (copy_len >= 14) {
                            for (int e = 0; e < 6; e++) {
                                DOCA_GPUNETIO_VOLATILE(stage->src_eth[e]) = hdr[6 + e];
                            }
                        }

                        const uint16_t eth_type =
                            static_cast<uint16_t>(hdr[12]) << 8 | static_cast<uint16_t>(hdr[13]);

                        if (eth_type == ether_type_ipv4) {
                            const unsigned ihl = (hdr[14] & 0x0Fu) * 4U;
                            if (14U + ihl + 8U <= copy_len && hdr[14 + 9] == IPPROTO_UDP) {
                                src_ip4_be = *reinterpret_cast<const uint32_t*>(&hdr[14 + 12]);
                                src_port_be = *reinterpret_cast<const uint16_t*>(&hdr[14 + ihl]);
                                payload_ptr = reinterpret_cast<const std::byte*>(&hdr[14 + ihl + 8]);
                                payload_len = pkt_bytes - (14U + ihl + 8U);
                                is_ipv6 = false;
                            }
                        } else if (eth_type == ether_type_ipv6) {
                            if (14U + 40U + 8U <= copy_len && hdr[14 + 6] == IPPROTO_UDP) {
                                memcpy(src_ip6_be, &hdr[14 + 8], sizeof(src_ip6_be));
                                src_port_be = *reinterpret_cast<const uint16_t*>(&hdr[14 + 40]);
                                payload_ptr = reinterpret_cast<const std::byte*>(&hdr[14 + 40 + 8]);
                                payload_len = pkt_bytes - (14U + 40U + 8U);
                                is_ipv6 = true;
                            }
                        }
                    }

                    if (payload_ptr != nullptr) {
                        const uint32_t max_payload = sizeof(stage->payload);
                        if (payload_len > max_payload) {
                            payload_len = max_payload;
                        }

                        memcpy(stage->payload, payload_ptr, payload_len);
                        DOCA_GPUNETIO_VOLATILE(stage->payload_len) = payload_len;
                        DOCA_GPUNETIO_VOLATILE(stage->src_port_be) = src_port_be;
                        DOCA_GPUNETIO_VOLATILE(stage->src_is_ipv6) = is_ipv6 ? 1U : 0U;
                        DOCA_GPUNETIO_VOLATILE(stage->src_ip4_be) = src_ip4_be;
                        DOCA_GPUNETIO_VOLATILE(stage->src_ip6_be[0]) = src_ip6_be[0];
                        DOCA_GPUNETIO_VOLATILE(stage->src_ip6_be[1]) = src_ip6_be[1];
                        DOCA_GPUNETIO_VOLATILE(stage->src_ip6_be[2]) = src_ip6_be[2];
                        DOCA_GPUNETIO_VOLATILE(stage->src_ip6_be[3]) = src_ip6_be[3];
                    }
                }

                __threadfence_system();
                DOCA_GPUNETIO_VOLATILE(stage->gpu_sem) = DOCA_GPU_SEMAPHORE_STATUS_READY;
            }

            __syncthreads();

            // Wait for CPU to consume.
            while (DOCA_GPUNETIO_VOLATILE(stage->stop) == 0 &&
                DOCA_GPUNETIO_VOLATILE(stage->cpu_sem) != DOCA_GPU_SEMAPHORE_STATUS_DONE) {
            }
            __syncthreads();

            if (threadIdx.x == 0) {
                DOCA_GPUNETIO_VOLATILE(stage->cpu_sem) = DOCA_GPU_SEMAPHORE_STATUS_FREE;
            }
            __syncthreads();
        }
    }
} // namespace

extern "C" cudaError_t snakeio_doca_gpunetio_recv_launch(
    cudaStream_t stream, struct doca_gpu_eth_rxq* rxq_gpu, struct snakeio_doca_stage_cpu* stage) {
    recv_udp_kernel<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK><<<1, cuda_block_threads, 0, stream>>>(rxq_gpu, stage);
    return cudaGetLastError();
}
