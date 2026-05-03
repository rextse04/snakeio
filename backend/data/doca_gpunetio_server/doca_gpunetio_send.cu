#include <doca_gpunetio_dev_eth_txq.cuh>

#include <cuda_runtime.h>

#include <cstdint>

namespace {

    __global__ void send_one_frame_kernel(struct doca_gpu_eth_txq* txq,
        uint8_t* pkt_buff_addr,
        const uint32_t pkt_buff_mkey,
        const uint32_t frame_len) {
        const uint16_t wqe_idx = static_cast<uint16_t>(threadIdx.x);
        enum doca_gpu_eth_send_flags flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NONE;
        const uint64_t addr = reinterpret_cast<uint64_t>(pkt_buff_addr);

        if (threadIdx.x == (blockDim.x - 1)) {
            flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;
        }

        struct doca_gpu_dev_eth_txq_wqe* wqe_ptr = doca_gpu_dev_eth_txq_get_wqe_ptr(txq, wqe_idx);
        doca_gpu_dev_eth_txq_wqe_prepare_send(txq, wqe_ptr, wqe_idx, addr, pkt_buff_mkey, frame_len, flags);
        __syncthreads();

        if (threadIdx.x == (blockDim.x - 1)) {
            doca_gpu_dev_eth_txq_submit(txq, wqe_idx + 1);
            const doca_error_t st = doca_gpu_dev_eth_txq_poll_completion_at<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_GPU,
                DOCA_GPUNETIO_ETH_SYNC_SCOPE_CTA>(txq, 0U, DOCA_GPUNETIO_ETH_WAIT_FLAG_B);
            (void)st;
        }
        __syncthreads();
    }

} // namespace

extern "C" cudaError_t snakeio_doca_gpunetio_tx_send_one_launch(cudaStream_t stream,
    struct doca_gpu_eth_txq* txq_gpu,
    uint8_t* pkt_gpu,
    uint32_t mkey_be,
    uint32_t frame_len) {
    send_one_frame_kernel<<<1, 1, 0, stream>>>(txq_gpu, pkt_gpu, mkey_be, frame_len);
    return cudaGetLastError();
}
