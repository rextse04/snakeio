#include "doca_gpunetio_runtime.hpp"
#include "doca_gpunetio_rx_stage.cuh"
#include <gpu_server/game_kernels.cuh>
#include <logger.hpp>
#include <packet.hpp>
#include <cuda_runtime.h>
#include <doca_error.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>
#include <mutex>
#include <vector>

namespace {

const char* env_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && v[0]) ? v : fallback;
}

thread_local std::vector<snakeio::doca_gpunetio::ingress_packet> ingress_pkts_tl;
thread_local std::vector<std::byte> ingress_packet_blob_tl;
thread_local std::vector<snakeio::size_t> ingress_sizes_tl;
thread_local std::vector<unsigned char> ingress_ok_tl;
thread_local std::vector<snakeio::id_t> ingress_sid_tl;
thread_local std::vector<snakeio::id_t> ingress_pid_tl;

} // namespace

void snakeio::doca_gpunetio::runtime::try_init_doca(snakeio::gpu::device_state& gs) noexcept
{
    if (doca_ready_)
        return;
    if (std::getenv("SNAKEIO_DOCA_GPUNETIO") == nullptr) {
        logger::warn(
            "DOCA GPUNetIO: SNAKEIO_DOCA_GPUNETIO not set — GPUNetIO dataplane disabled; use kernel UDP "
            "(set SNAKEIO_DOCA_GPUNETIO=1 for DOCA RX when hardware is available).");
        return;
    }
    doca_ = std::make_unique<DocaGpuIngress>();
    const char* nic_env = std::getenv("SNAKEIO_NIC_PCIE");
    const char* nic = env_or("SNAKEIO_NIC_PCIE", "0000:bd:00.0");
    if (nic_env == nullptr || nic_env[0] == '\0') {
        logger::warn(
            "DOCA: SNAKEIO_NIC_PCIE not set — using default NIC PCIe address {} (set SNAKEIO_NIC_PCIE "
            "when your NIC BDF differs).",
            nic);
    }
    const char* gpu_env = std::getenv("SNAKEIO_GPU_PCIE");
    const char* gpu = env_or("SNAKEIO_GPU_PCIE", "0000:ab:00.0");
    if (gpu_env == nullptr || gpu_env[0] == '\0') {
        logger::warn(
            "DOCA: SNAKEIO_GPU_PCIE not set — using default GPU PCIe address {} (set SNAKEIO_GPU_PCIE "
            "when your GPU BDF differs).",
            gpu);
    }
    doca_error_t r = doca_->try_init(nic, gpu, gs.cuda_device_id, gs.packet_ring, gs.packet_ring_capacity);
    if (r == DOCA_SUCCESS) {
        doca_ready_ = true;
    } else {
        doca_.reset();
        doca_ready_ = false;
        logger::warn("DOCA GPUNetIO init failed ({}); falling back to kernel UDP socket.", doca_error_get_descr(r));
    }
}

std::pair<std::size_t, std::size_t> snakeio::doca_gpunetio::runtime::process_kernel_udp_ingress(
    snakeio::gpu::device_state& gs, int sock, std::stop_token stop_token) noexcept
{
    (void)stop_token;
    ingress_packet pkt{};
    socklen_t source_len = sizeof(pkt.source_addr);
    const ssize_t recv_len = recvfrom(sock,
        pkt.bytes.data(),
        pkt.bytes.size(),
        0,
        reinterpret_cast<sockaddr*>(&pkt.source_addr),
        &source_len);
    if (recv_len < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return {0, 0};
        logger::warn("DOCA kernel dataplane recvfrom failed: {}.", std::strerror(errno));
        return {0, 0};
    }
    if (recv_len == 0) [[unlikely]] {
        return {0, 0};
    }
    pkt.size = static_cast<std::size_t>(recv_len);
    gpu::ingest_packet_gpu(gs, pkt.bytes.data(), pkt.size);
    if (!gs.host_ingress->ok)
        return {1, 0};
    const id_t session_id = gs.host_ingress->session_id;
    const id_t player_id = gs.host_ingress->player_id;
    cudaMemcpyAsync(gs.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
        &pkt.source_addr,
        sizeof(sockaddr_storage),
        cudaMemcpyHostToDevice,
        reinterpret_cast<cudaStream_t>(gs.stream));
    cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(gs.stream));
    return {1, 1};
}

std::pair<std::size_t, std::size_t> snakeio::doca_gpunetio::runtime::process_doca_ingress(
    snakeio::gpu::device_state& gs, std::stop_token stop_token) noexcept
{
    if (!doca_active())
        return {0, 0};

    cudaStream_t stream = reinterpret_cast<cudaStream_t>(gs.stream);
    constexpr std::uint16_t k_af_inet = 2;
    constexpr std::uint16_t k_af_inet6 = 10;
    constexpr std::uint32_t k_min_game_payload = static_cast<std::uint32_t>(data_packet::header_size);
    constexpr std::uint32_t k_max_game_payload =
        static_cast<std::uint32_t>(in_packet_max_text_size + data_packet::header_size);
    constexpr std::uint32_t k_preferred_client_udp = k_max_game_payload;
    constexpr int k_max_batches_per_tick = 64;

    ingress_pkts_tl.clear();
    ingress_pkts_tl.reserve(1024);

    std::unique_lock<std::mutex> lk(doca_mtx_);

    auto fill_sockaddr = [&](const snakeio_doca_rx_stage_entry& st, sockaddr_storage& ss) noexcept {
        if (st.addr_family == k_af_inet6) {
            sockaddr_in6 sin6{};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = st.src_port_be;
            sin6.sin6_flowinfo = 0;
            std::memcpy(&sin6.sin6_addr.s6_addr, st.src_ipv6_be, sizeof(st.src_ipv6_be));
            std::memcpy(&ss, &sin6, sizeof(sin6));
        } else if (st.addr_family == k_af_inet) {
            sockaddr_in6 sin6{};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = st.src_port_be;
            sin6.sin6_flowinfo = 0;
            std::memset(sin6.sin6_addr.s6_addr, 0, 10);
            sin6.sin6_addr.s6_addr[10] = 0xff;
            sin6.sin6_addr.s6_addr[11] = 0xff;
            std::memcpy(&sin6.sin6_addr.s6_addr[12], &st.src_ipv4_be, sizeof(st.src_ipv4_be));
            std::memcpy(&ss, &sin6, sizeof(sin6));
        }
    };

    auto try_stage_frame = [&](std::uint32_t idx, bool wire48_round) noexcept {
        const auto& st = doca_->stage_host()[idx];
        if (!st.valid)
            return;
        if (st.payload_len < k_min_game_payload || st.payload_len > k_max_game_payload)
            return;
        if (wire48_round) {
            if (st.payload_len != k_preferred_client_udp)
                return;
        } else if (st.payload_len == k_preferred_client_udp)
            return;
        if (!(st.addr_family == k_af_inet || st.addr_family == k_af_inet6))
            return;

        ingress_packet pkt{};
        fill_sockaddr(st, pkt.source_addr);

        const auto plen = static_cast<std::size_t>(st.payload_len);
        if (plen > pkt.bytes.size()) [[unlikely]]
            return;
        if (cudaMemcpy(pkt.bytes.data(),
                reinterpret_cast<const void*>(st.payload_dev_va),
                plen,
                cudaMemcpyDeviceToHost)
            != cudaSuccess) {
            return;
        }
        pkt.size = plen;

        // Bound staged packets so one call cannot run unbounded host work.
        if (ingress_pkts_tl.size() < 1024)
            ingress_pkts_tl.push_back(std::move(pkt));
    };

    for (int batch = 0; batch < k_max_batches_per_tick; ++batch) {
        if (stop_token.stop_requested())
            break;
        doca_error_t dr = doca_->receive_tick(stream);
        if (dr != DOCA_SUCCESS) {
            logger::warn("DOCA receive_tick failed: {}.", doca_error_get_descr(dr));
            break;
        }
        dr = doca_->progress_txq();
        if (dr != DOCA_SUCCESS && dr != DOCA_ERROR_AGAIN) {
            logger::warn("DOCA progress_txq failed: {}.", doca_error_get_descr(dr));
        }
        const std::uint32_t n = doca_->last_count();
        if (n == 0)
            break;

        for (std::uint32_t i = 0; i < n; ++i)
            try_stage_frame(i, true);
        for (std::uint32_t i = 0; i < n; ++i)
            try_stage_frame(i, false);

        if (stop_token.stop_requested())
            break;
        if (n < SNAKEIO_DOCA_RX_BATCH_MAX)
            break;
    }
    const std::size_t rx_count = ingress_pkts_tl.size();
    if (rx_count == 0)
        return {0, 0};
    const snakeio::size_t packet_stride = gs.ingress_packet_capacity;
    ingress_packet_blob_tl.resize(packet_stride * rx_count);
    ingress_sizes_tl.resize(rx_count);
    for (std::size_t i = 0; i < rx_count; ++i) {
        const auto& p = ingress_pkts_tl[i];
        ingress_sizes_tl[i] = p.size;
        std::memcpy(ingress_packet_blob_tl.data() + i * packet_stride, p.bytes.data(), p.size);
    }
    ingress_ok_tl.assign(rx_count, 0);
    ingress_sid_tl.resize(rx_count);
    ingress_pid_tl.resize(rx_count);
    gpu::ingest_packets_gpu_batch(gs,
        ingress_packet_blob_tl.data(),
        ingress_sizes_tl.data(),
        packet_stride,
        rx_count,
        ingress_ok_tl.data(),
        ingress_sid_tl.data(),
        ingress_pid_tl.data());
    std::size_t accepted_now = 0;
    for (std::size_t i = 0; i < rx_count; ++i) {
        if (!ingress_ok_tl[i])
            continue;
        ++accepted_now;
        cudaMemcpyAsync(gs.client_addrs
                + gpu::client_index(ingress_sid_tl[i], ingress_pid_tl[i]) * sizeof(sockaddr_storage),
            &ingress_pkts_tl[i].source_addr,
            sizeof(sockaddr_storage),
            cudaMemcpyHostToDevice,
            stream);
    }
    cudaStreamSynchronize(stream);
    return {rx_count, accepted_now};
}

std::size_t snakeio::doca_gpunetio::runtime::emit_egress_batch(
    snakeio::gpu::device_state& gs, int sock) noexcept
{
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(gs.stream);
    unsigned send_count = 0;
    int send_sock = sock;

    {
        std::lock_guard<std::mutex> lk(doca_mtx_);

        cudaMemcpyAsync(&send_count, gs.send_descs_size, sizeof(send_count), cudaMemcpyDeviceToHost, stream);
        cudaStreamSynchronize(stream);
        if (send_count == 0) {
            return 0;
        }
        if (send_count > gs.send_descs_capacity) {
            logger::warn("DOCA runtime egress: clamping invalid send_desc count {} to capacity {}.",
                send_count, gs.send_descs_capacity);
            send_count = gs.send_descs_capacity;
        }

        if (doca_active()) {
            if (doca_->gpu_tx_ready()) {
                const doca_error_t tr = doca_->emit_gpu_tx(stream, gs);
                if (tr == DOCA_SUCCESS) {
                    return static_cast<snakeio::size_t>(send_count);
                }
                logger::warn("DOCA GPU egress failed ({}); falling back to sendto for this tick.",
                    doca_error_get_descr(tr));
            } else {
                static bool warned_gpu_tx = false;
                if (!warned_gpu_tx) {
                    warned_gpu_tx = true;
                    logger::warn(
                        "DOCA: GPU Eth TXQ not ready — using kernel sendto until GPU egress is initialized.");
                }
            }
        }

        cudaMemcpyAsync(gs.host_send_descs,
            gs.send_descs,
            sizeof(gpu::send_desc) * send_count,
            cudaMemcpyDeviceToHost,
            stream);
        cudaStreamSynchronize(stream);

        if (doca_active()) {
            const int k = doca_->kernel_egress_sock();
            if (k >= 0)
                send_sock = k;
        }
    }

    snakeio::size_t sent = 0;
    for (unsigned j = 0; j < send_count; ++j) {
        const gpu::send_desc& desc = gs.host_send_descs[j];
        if (desc.bytes_size == 0 || desc.bytes_size > out_packet_max_text_size) {
            continue;
        }
        if (desc.ring_offset + desc.bytes_size > gs.packet_ring_capacity) {
            continue;
        }
        sockaddr_storage addr{};
        {
            std::lock_guard<std::mutex> lk(doca_mtx_);
            cudaMemcpyAsync(gs.host_packet_copy,
                gs.packet_ring + desc.ring_offset,
                desc.bytes_size,
                cudaMemcpyDeviceToHost,
                stream);
            cudaMemcpyAsync(&addr,
                gs.client_addrs + gpu::client_index(desc.session_id, desc.player_id) * sizeof(sockaddr_storage),
                sizeof(sockaddr_storage),
                cudaMemcpyDeviceToHost,
                stream);
            cudaStreamSynchronize(stream);
        }
        sendto(send_sock,
            gs.host_packet_copy,
            desc.bytes_size,
            0,
            reinterpret_cast<const sockaddr*>(&addr),
            sizeof(sockaddr_storage));
        ++sent;
    }
    if (sent == 0 && send_count > 0) {
        logger::warn(
            "DOCA egress: {} device send_desc(s) but none sent (bytes_size/ring_offset vs out_packet_max_text_size "
            "{}, ring_capacity {}).",
            send_count,
            static_cast<unsigned>(out_packet_max_text_size),
            static_cast<unsigned>(gs.packet_ring_capacity));
    }
    return sent;
}
