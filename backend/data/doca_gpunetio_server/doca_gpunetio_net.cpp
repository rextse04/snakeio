#include "doca_gpunetio_net.hpp"
#include "doca_gpunetio_recv_sample.h"
#include "doca_gpunetio_rx_stage.cuh"
#include "doca_gpunetio_tx.cuh"
#include <config.hpp>
#include <network.hpp>
#include <doca_flow.h>
#include <doca_gpunetio.h>
#include <doca_mmap.h>
#include <logger.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <gpu_server/game_kernels.cuh>
#include <cstring>
#include <cstdio>
#include <endian.h>
#include <unistd.h>

#include <cstdlib>

namespace {

size_t host_page_size() noexcept
{
    const long r = sysconf(_SC_PAGESIZE);
    return r > 0 ? static_cast<size_t>(r) : 4096;
}

bool parse_mac(const char* s, std::uint8_t out[6]) noexcept
{
    if (s == nullptr || s[0] == '\0')
        return false;
    const int n = std::sscanf(s,
        "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &out[0],
        &out[1],
        &out[2],
        &out[3],
        &out[4],
        &out[5]);
    return n == 6;
}

#define ALIGN_SIZE(size, align) size = (((size) + ((align)-1)) / (align)) * (align)

doca_error_t doca_mmap_gpu_buffer(struct doca_gpu* gpu,
    struct doca_dev* nic,
    void* gpu_va,
    size_t bytes,
    struct doca_mmap** mmap_out,
    int* dmabuf_fd_out,
    std::uint32_t* mkey_be_out)
{
    struct doca_mmap* mmap = nullptr;
    doca_error_t result = doca_mmap_create(&mmap);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_mmap_add_dev(mmap, nic);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }
    int dmabuf_fd = -1;
    result = doca_gpu_dmabuf_fd(gpu, gpu_va, bytes, &dmabuf_fd);
    if (result != DOCA_SUCCESS) {
        result = doca_mmap_set_memrange(mmap, gpu_va, bytes);
        if (result != DOCA_SUCCESS) {
            doca_mmap_destroy(mmap);
            return result;
        }
    } else {
        result = doca_mmap_set_dmabuf_memrange(mmap, dmabuf_fd, gpu_va, 0, bytes);
        if (result != DOCA_SUCCESS) {
            doca_mmap_destroy(mmap);
            return result;
        }
    }
    if (dmabuf_fd_out != nullptr)
        *dmabuf_fd_out = dmabuf_fd;
    result = doca_mmap_set_permissions(mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }
    result = doca_mmap_start(mmap);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }
    std::uint32_t mkey_host = 0;
    result = doca_mmap_get_mkey(mmap, nic, &mkey_host);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }
    *mkey_be_out = htobe32(mkey_host);
    *mmap_out = mmap;
    return DOCA_SUCCESS;
}

} // namespace

/// Binds before `snakeio_recv_create_rxq` installs RSS so `sendto` replies use the kernel stack.
static int open_kernel_udp_for_doca_sendto() noexcept
{
    const char* bind4 = std::getenv("SNAKEIO_DATA_PLANE_BIND_IPV4");
    if (bind4 == nullptr || bind4[0] == '\0')
        bind4 = std::getenv("SNAKEIO_DOCA_PACKET_IO_DST");
    if (bind4 != nullptr && bind4[0] != '\0') {
        in_addr v4{};
        if (inet_pton(AF_INET, bind4, &v4) != 1) {
            snakeio::logger::error("DOCA egress bind: invalid IPv4 in SNAKEIO_DATA_PLANE_BIND_IPV4 / SNAKEIO_DOCA_PACKET_IO_DST.");
            return -1;
        }
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(snakeio::data_plane_ext_port);
        sin6.sin6_flowinfo = 0;
        std::memset(sin6.sin6_addr.s6_addr, 0, 10);
        sin6.sin6_addr.s6_addr[10] = 0xff;
        sin6.sin6_addr.s6_addr[11] = 0xff;
        std::memcpy(&sin6.sin6_addr.s6_addr[12], &v4.s_addr, sizeof(v4.s_addr));
        return snakeio::open_port("doca-sendto", sin6);
    }
    return snakeio::open_port("doca-sendto", {
        .sin6_family = AF_INET6,
        .sin6_port = htons(snakeio::data_plane_ext_port),
        .sin6_addr = in6addr_any
    });
}

snakeio::doca_gpunetio::DocaGpuIngress::~DocaGpuIngress() {
    shutdown();
}

doca_error_t snakeio::doca_gpunetio::DocaGpuIngress::try_init(const char* nic_pcie,
    const char* gpu_pcie,
    int cuda_device,
    std::byte* packet_ring,
    std::size_t packet_ring_capacity)
{
    if (ok_)
        return DOCA_SUCCESS;
    cudaSetDevice(cuda_device);
    cudaFree(nullptr);

    packet_ring_base_ = packet_ring;
    packet_ring_bytes_ = packet_ring_capacity;
    gpu_tx_ready_ = false;

    doca_error_t r = open_doca_device_with_pci(nic_pcie, nullptr, &nic_dev_);
    if (r != DOCA_SUCCESS) {
        logger::warn("DOCA: open NIC {} failed: {}.", nic_pcie, doca_error_get_descr(r));
        return r;
    }
    r = snakeio_recv_init_flow();
    if (r != DOCA_SUCCESS) {
        logger::warn("DOCA: doca_flow init failed: {}.", doca_error_get_descr(r));
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return r;
    }
    r = snakeio_recv_start_port(nic_dev_);
    if (r != DOCA_SUCCESS) {
        logger::warn("DOCA: doca_flow port start failed: {}.", doca_error_get_descr(r));
        doca_flow_destroy();
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return r;
    }
    r = doca_gpu_create(gpu_pcie, &gpu_dev_);
    if (r != DOCA_SUCCESS) {
        logger::warn("DOCA: doca_gpu_create {} failed: {}.", gpu_pcie, doca_error_get_descr(r));
        doca_flow_port_stop(g_doca_df_port);
        doca_flow_destroy();
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return r;
    }

    kernel_egress_sock_ = open_kernel_udp_for_doca_sendto();
    if (kernel_egress_sock_ < 0) {
        doca_gpu_destroy(gpu_dev_);
        gpu_dev_ = nullptr;
        doca_flow_port_stop(g_doca_df_port);
        doca_flow_destroy();
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return DOCA_ERROR_BAD_STATE;
    }

    r = snakeio_recv_create_rxq(&rxq_, gpu_dev_, cuda_device, nic_dev_);
    if (r != DOCA_SUCCESS) {
        logger::warn("DOCA: create_rxq failed: {}.", doca_error_get_descr(r));
        doca_gpu_destroy(gpu_dev_);
        gpu_dev_ = nullptr;
        doca_flow_port_stop(g_doca_df_port);
        doca_flow_destroy();
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return r;
    }
    r = snakeio_recv_create_txq(&rxq_, gpu_dev_, nic_dev_);
    if (r != DOCA_SUCCESS) {
        logger::warn("DOCA: create_txq failed: {}.", doca_error_get_descr(r));
        snakeio_recv_destroy_rxq(&rxq_);
        doca_gpu_destroy(gpu_dev_);
        gpu_dev_ = nullptr;
        doca_flow_port_stop(g_doca_df_port);
        doca_flow_destroy();
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return r;
    }

    r = doca_gpu_mem_alloc(gpu_dev_,
        sizeof(snakeio_doca_rx_stage_entry) * SNAKEIO_DOCA_RX_BATCH_MAX,
        4096,
        DOCA_GPU_MEM_TYPE_CPU_GPU,
        reinterpret_cast<void**>(&stage_gpu_),
        reinterpret_cast<void**>(&stage_host_));
    if (r != DOCA_SUCCESS || stage_gpu_ == nullptr || stage_host_ == nullptr) {
        logger::warn("DOCA: stage buffer alloc failed: {}.", doca_error_get_descr(r));
        snakeio_recv_destroy_txq(&rxq_);
        snakeio_recv_destroy_rxq(&rxq_);
        doca_gpu_destroy(gpu_dev_);
        gpu_dev_ = nullptr;
        doca_flow_port_stop(g_doca_df_port);
        doca_flow_destroy();
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return r != DOCA_SUCCESS ? r : DOCA_ERROR_NO_MEMORY;
    }

    r = doca_gpu_mem_alloc(gpu_dev_,
        sizeof(std::uint32_t),
        4096,
        DOCA_GPU_MEM_TYPE_CPU_GPU,
        reinterpret_cast<void**>(&count_gpu_),
        reinterpret_cast<void**>(&count_host_));
    if (r != DOCA_SUCCESS || count_gpu_ == nullptr || count_host_ == nullptr) {
        logger::warn("DOCA: count buffer alloc failed: {}.", doca_error_get_descr(r));
        doca_gpu_mem_free(gpu_dev_, stage_gpu_);
        stage_gpu_ = nullptr;
        stage_host_ = nullptr;
        snakeio_recv_destroy_txq(&rxq_);
        snakeio_recv_destroy_rxq(&rxq_);
        doca_gpu_destroy(gpu_dev_);
        gpu_dev_ = nullptr;
        doca_flow_port_stop(g_doca_df_port);
        doca_flow_destroy();
        doca_dev_close(nic_dev_);
        nic_dev_ = nullptr;
        return r != DOCA_SUCCESS ? r : DOCA_ERROR_NO_MEMORY;
    }

    const char* src_mac_e = std::getenv("SNAKEIO_DOCA_TX_SRC_MAC");
    const char* dst_mac_e = std::getenv("SNAKEIO_DOCA_TX_DST_MAC");
    const char* src_ip4_e = std::getenv("SNAKEIO_DOCA_TX_SRC_IPV4");
    const char* src_ip6_e = std::getenv("SNAKEIO_DOCA_TX_SRC_IPV6");
    tx_frame_constants tfc{};
    const bool mac_ok = parse_mac(src_mac_e, tfc.src_mac) && parse_mac(dst_mac_e, tfc.dst_mac);
    if (mac_ok && src_ip6_e != nullptr && inet_pton(AF_INET6, src_ip6_e, tfc.src_ipv6) == 1)
        tfc.have_src_v6 = 1;
    if (mac_ok && src_ip4_e != nullptr && inet_pton(AF_INET, src_ip4_e, &tfc.src_ipv4_be) == 1)
        tfc.have_src_v4 = 1;
    if (tfc.have_src_v4 || tfc.have_src_v6) {
        tfc.src_udp_port_be = htons(snakeio::data_plane_ext_port);
        r = tx_upload_constants(tfc);
        if (r != DOCA_SUCCESS) {
            logger::warn("DOCA: tx_upload_constants failed: {}.", doca_error_get_descr(r));
        } else {
            const size_t hdr_bytes =
                static_cast<size_t>(snakeio::game_max_sessions) * snakeio::game_max_players * 64u;
            size_t hdr_alloc = hdr_bytes;
            ALIGN_SIZE(hdr_alloc, host_page_size());
            r = doca_gpu_mem_alloc(gpu_dev_,
                hdr_alloc,
                host_page_size(),
                DOCA_GPU_MEM_TYPE_GPU,
                reinterpret_cast<void**>(&tx_hdr_slab_gpu_),
                nullptr);
            if (r != DOCA_SUCCESS || tx_hdr_slab_gpu_ == nullptr) {
                logger::warn("DOCA: TX header slab alloc failed: {}.", doca_error_get_descr(r));
            } else {
                r = doca_mmap_gpu_buffer(gpu_dev_,
                    nic_dev_,
                    tx_hdr_slab_gpu_,
                    hdr_alloc,
                    &tx_hdr_mmap_,
                    nullptr,
                    &tx_hdr_mkey_be_);
                if (r != DOCA_SUCCESS) {
                    logger::warn("DOCA: TX header mmap failed: {}.", doca_error_get_descr(r));
                    doca_gpu_mem_free(gpu_dev_, tx_hdr_slab_gpu_);
                    tx_hdr_slab_gpu_ = nullptr;
                } else {
                    r = doca_mmap_gpu_buffer(gpu_dev_,
                        nic_dev_,
                        packet_ring,
                        packet_ring_capacity,
                        &ring_mmap_,
                        &ring_dmabuf_fd_,
                        &ring_mkey_be_);
                    if (r != DOCA_SUCCESS) {
                        logger::warn("DOCA: packet_ring mmap failed: {}.", doca_error_get_descr(r));
                        doca_mmap_destroy(tx_hdr_mmap_);
                        tx_hdr_mmap_ = nullptr;
                        doca_gpu_mem_free(gpu_dev_, tx_hdr_slab_gpu_);
                        tx_hdr_slab_gpu_ = nullptr;
                    } else {
                        gpu_tx_ready_ = true;
                        logger::debug(
                            "DOCA GPUNetIO GPU egress ready (UDP; IPv6 requires SNAKEIO_DOCA_TX_SRC_IPV6, "
                            "optional SNAKEIO_DOCA_TX_SRC_IPV4 for dual-stack peers).");
                    }
                }
            }
        }
    } else {
        logger::debug(
            "DOCA: GPU egress disabled; set SNAKEIO_DOCA_TX_SRC_MAC, SNAKEIO_DOCA_TX_DST_MAC, and "
            "SNAKEIO_DOCA_TX_SRC_IPV6 (preferred) and/or SNAKEIO_DOCA_TX_SRC_IPV4 for Eth TX (else sendto).");
    }

    ok_ = true;
    logger::debug("DOCA GPUNetIO ingress ready (NIC {}, GPU {}).", nic_pcie, gpu_pcie);
    return DOCA_SUCCESS;
}

void snakeio::doca_gpunetio::DocaGpuIngress::shutdown() noexcept
{
    if (!ok_) {
        if (kernel_egress_sock_ >= 0) {
            ::close(kernel_egress_sock_);
            kernel_egress_sock_ = -1;
        }
        return;
    }
    if (stage_gpu_ != nullptr && gpu_dev_ != nullptr) {
        doca_gpu_mem_free(gpu_dev_, stage_gpu_);
        stage_gpu_ = nullptr;
        stage_host_ = nullptr;
    }
    if (count_gpu_ != nullptr && gpu_dev_ != nullptr) {
        doca_gpu_mem_free(gpu_dev_, count_gpu_);
        count_gpu_ = nullptr;
        count_host_ = nullptr;
    }
    snakeio_recv_destroy_txq(&rxq_);
    if (ring_mmap_ != nullptr) {
        doca_mmap_destroy(ring_mmap_);
        ring_mmap_ = nullptr;
    }
    if (tx_hdr_mmap_ != nullptr) {
        doca_mmap_destroy(tx_hdr_mmap_);
        tx_hdr_mmap_ = nullptr;
    }
    if (tx_hdr_slab_gpu_ != nullptr && gpu_dev_ != nullptr) {
        doca_gpu_mem_free(gpu_dev_, tx_hdr_slab_gpu_);
        tx_hdr_slab_gpu_ = nullptr;
    }
    snakeio_recv_destroy_rxq(&rxq_);
    if (gpu_dev_ != nullptr) {
        doca_gpu_destroy(gpu_dev_);
        gpu_dev_ = nullptr;
    }
    nic_dev_ = nullptr;
    gpu_tx_ready_ = false;
    packet_ring_base_ = nullptr;
    packet_ring_bytes_ = 0;
    if (kernel_egress_sock_ >= 0) {
        ::close(kernel_egress_sock_);
        kernel_egress_sock_ = -1;
    }
    ok_ = false;
}

doca_error_t snakeio::doca_gpunetio::DocaGpuIngress::receive_tick(cudaStream_t stream)
{
    if (!ok_)
        return DOCA_ERROR_INVALID_VALUE;
    doca_error_t r = snakeio_doca_rx_tick_launch(stream, rxq_.eth_rxq_gpu, stage_gpu_, count_gpu_);
    if (r != DOCA_SUCCESS)
        return r;
    // `DOCA_GPU_MEM_TYPE_CPU_GPU` exposes distinct CPU vs GPU VAs; the RX kernel writes the GPU VA.
    // Pull staging to the CPU VA so `last_count()` / `stage_host()` match what the kernel produced.
    cudaError_t ce = cudaMemcpyAsync(stage_host_,
        stage_gpu_,
        sizeof(snakeio_doca_rx_stage_entry) * SNAKEIO_DOCA_RX_BATCH_MAX,
        cudaMemcpyDefault,
        stream);
    if (ce != cudaSuccess) {
        logger::warn("DOCA: rx stage sync memcpy failed: {}.", cudaGetErrorString(ce));
        return DOCA_ERROR_DRIVER;
    }
    ce = cudaMemcpyAsync(count_host_, count_gpu_, sizeof(std::uint32_t), cudaMemcpyDefault, stream);
    if (ce != cudaSuccess) {
        logger::warn("DOCA: rx count sync memcpy failed: {}.", cudaGetErrorString(ce));
        return DOCA_ERROR_DRIVER;
    }
    ce = cudaStreamSynchronize(stream);
    if (ce != cudaSuccess) {
        logger::warn("DOCA: rx stream sync failed: {}.", cudaGetErrorString(ce));
        return DOCA_ERROR_DRIVER;
    }
    return DOCA_SUCCESS;
}

doca_error_t snakeio::doca_gpunetio::DocaGpuIngress::progress_txq() noexcept
{
    if (!ok_) {
        return DOCA_ERROR_INVALID_VALUE;
    }
    return snakeio_recv_progress_txq(&rxq_);
}

doca_error_t snakeio::doca_gpunetio::DocaGpuIngress::emit_gpu_tx(cudaStream_t stream, gpu::device_state& gs) noexcept
{
    if (!ok_ || !gpu_tx_ready_)
        return DOCA_ERROR_INVALID_VALUE;
    if (reinterpret_cast<std::byte*>(gs.packet_ring) != packet_ring_base_
        || gs.packet_ring_capacity != packet_ring_bytes_) {
        logger::warn("DOCA emit_gpu_tx: packet_ring mismatch with DOCA mmap.");
        return DOCA_ERROR_INVALID_VALUE;
    }
    unsigned send_count = 0;
    cudaMemcpyAsync(&send_count, gs.send_descs_size, sizeof(send_count), cudaMemcpyDeviceToHost, stream);
    cudaStreamSynchronize(stream);
    if (send_count == 0)
        return DOCA_SUCCESS;
    if (send_count > gs.send_descs_capacity) {
        logger::warn("DOCA emit_gpu_tx: clamping send count {} to {}.", send_count, gs.send_descs_capacity);
        send_count = gs.send_descs_capacity;
    }
    doca_error_t r = tx_emit_serial(stream,
        rxq_.eth_txq_gpu,
        gs,
        tx_hdr_slab_gpu_,
        tx_hdr_mkey_be_,
        ring_mkey_be_,
        packet_ring_bytes_,
        send_count);
    if (r != DOCA_SUCCESS)
        return r;
    cudaStreamSynchronize(stream);
    for (int k = 0; k < 256; ++k) {
        doca_error_t pr = snakeio_recv_progress_txq(&rxq_);
        if (pr == DOCA_SUCCESS)
            break;
    }
    return DOCA_SUCCESS;
}
