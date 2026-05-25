#pragma once
#include "doca_gpunetio_rx_stage.cuh"
#include "doca_gpunetio_rxq_queue.h"
#include <cuda_runtime.h>
#include <doca_error.h>
#include <cstddef>
#include <cstdint>

struct doca_mmap;

namespace snakeio::gpu {
    struct device_state;
}

namespace snakeio::doca_gpunetio {

class DocaGpuIngress {
public:
    DocaGpuIngress() = default;
    ~DocaGpuIngress();
    DocaGpuIngress(const DocaGpuIngress&) = delete;
    DocaGpuIngress& operator=(const DocaGpuIngress&) = delete;

    doca_error_t try_init(const char* nic_pcie,
        const char* gpu_pcie,
        int cuda_device,
        std::byte* packet_ring,
        std::size_t packet_ring_capacity);
    void shutdown() noexcept;
    bool active() const noexcept { return ok_; }
    bool gpu_tx_ready() const noexcept { return gpu_tx_ready_; }

    doca_error_t receive_tick(cudaStream_t stream);
    doca_error_t progress_txq() noexcept;

    doca_error_t emit_gpu_tx(cudaStream_t stream, gpu::device_state& gs) noexcept;

    std::uint32_t last_count() const noexcept { return count_host_ != nullptr ? *count_host_ : 0u; }
    snakeio_doca_rx_stage_entry* stage_host() noexcept { return stage_host_; }

    struct doca_gpu_eth_txq* eth_txq_gpu() noexcept { return rxq_.eth_txq_gpu; }

    /// Kernel UDP socket bound to `data_plane_ext_port` for `sendto` when GPU Eth TX is off.
    /// Opened before DOCA RSS rules so replies use a real stack socket when GPUNetIO is active.
    int kernel_egress_sock() const noexcept { return kernel_egress_sock_; }

private:
    bool ok_{false};
    bool gpu_tx_ready_{false};
    rxq_queue rxq_{};
    struct doca_gpu* gpu_dev_{};
    struct doca_dev* nic_dev_{};
    snakeio_doca_rx_stage_entry* stage_gpu_{};
    snakeio_doca_rx_stage_entry* stage_host_{};
    std::uint32_t* count_gpu_{};
    std::uint32_t* count_host_{};

    struct doca_mmap* ring_mmap_{};
    int ring_dmabuf_fd_{-1}; ///< From `doca_gpu_dmabuf_fd` when used; otherwise -1.
    std::uint32_t ring_mkey_be_{};
    std::byte* packet_ring_base_{};
    std::size_t packet_ring_bytes_{};

    struct doca_mmap* tx_hdr_mmap_{};
    std::byte* tx_hdr_slab_gpu_{};
    std::uint32_t tx_hdr_mkey_be_{};
    int kernel_egress_sock_{-1};
};

} // namespace snakeio::doca_gpunetio
