#include "doca_gpunetio_runtime.hpp"
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

namespace {

const char* env_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && v[0]) ? v : fallback;
}

/// `st.addr_family` is `AF_INET` (2) or `AF_INET6` (10) per `doca_gpunetio_rx.cu`.
bool stage_src_ipv4_equals(const snakeio_doca_rx_stage_entry& st, std::uint32_t v4_be) noexcept
{
    constexpr std::uint16_t k_af_inet = 2;
    constexpr std::uint16_t k_af_inet6 = 10;
    if (st.addr_family == k_af_inet)
        return st.src_ipv4_be == v4_be;
    if (st.addr_family != k_af_inet6)
        return false;
    std::uint8_t b[16]{};
    for (int w = 0; w < 4; ++w)
        std::memcpy(b + static_cast<std::size_t>(w) * 4u, &st.src_ipv6_be[w], 4u);
    return b[10] == 0xff && b[11] == 0xff && std::memcmp(b + 12, &v4_be, 4) == 0;
}

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

snakeio::size_t snakeio::doca_gpunetio::runtime::poll_socket_batch(int sock, std::span<ingress_packet> out) noexcept
{
    snakeio::size_t produced = 0;
    while (produced < out.size()) {
        auto& pkt = out[produced];
        socklen_t source_len = sizeof(pkt.source_addr);
        const ssize_t recv_len = recvfrom(sock,
            pkt.bytes.data(),
            pkt.bytes.size(),
            MSG_DONTWAIT,
            reinterpret_cast<sockaddr*>(&pkt.source_addr),
            &source_len);
        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            logger::warn("DOCA runtime fallback recvfrom failed: {}.", std::strerror(errno));
            break;
        }
        if (recv_len == 0) [[unlikely]] {
            continue;
        }
        pkt.size = static_cast<size_t>(recv_len);
        ++produced;
    }
    return produced;
}

// If the game never sees ingress, verify the NIC→GPU path with `doca_gpunetio_toy_gpu_udp_reach` (same
// `DocaGpuIngress` / doca_flow setup, no CUDA game state).
snakeio::size_t snakeio::doca_gpunetio::runtime::poll_ingress_batch(
    snakeio::gpu::device_state& gs, int sock, std::span<ingress_packet> out) noexcept
{
    if (!doca_active()) {
        return poll_socket_batch(sock, out);
    }
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(gs.stream);
    snakeio::size_t produced = 0;
    const bool doca_debug = std::getenv("SNAKEIO_DOCA_DEBUG") != nullptr && std::getenv("SNAKEIO_DOCA_DEBUG")[0] != '\0';
    unsigned dbg_batches = 0;
    std::uint32_t dbg_max_n = 0;
    std::uint32_t dbg_sum_n = 0;
    std::uint32_t dbg_stage_valid = 0;
    std::uint32_t dbg_len_out_of_range = 0;
    std::uint32_t dbg_ingest_reject = 0;

    const char* trace_peer = std::getenv("SNAKEIO_DOCA_TRACE_PEER_IPV4");
    in_addr trace_peer_v4{};
    const bool tracing_peer =
        trace_peer != nullptr && trace_peer[0] != '\0' && inet_pton(AF_INET, trace_peer, &trace_peer_v4) == 1;
    const std::uint32_t trace_peer_be = tracing_peer ? trace_peer_v4.s_addr : 0u;
    // Default: only log peer frames whose UDP payload length matches the usual client wire size (reduces spam).
    const char* trace_all_len = std::getenv("SNAKEIO_DOCA_TRACE_PEER_ALL_LEN");
    const bool trace_peer_all_lengths =
        trace_all_len != nullptr && trace_all_len[0] != '\0' && trace_all_len[0] != '0';
    const char* trace_only_wire48 = std::getenv("SNAKEIO_DOCA_TRACE_PEER_ONLY_WIRE48");
    const bool trace_peer_only_wire48 =
        trace_only_wire48 != nullptr && trace_only_wire48[0] != '\0' && trace_only_wire48[0] != '0';
    unsigned trace_peer_hits = 0;
    unsigned trace_peer_hits_wire48 = 0;
    unsigned trace_peer_logged = 0;
    constexpr unsigned k_trace_peer_max_lines = 24u;

    constexpr std::uint32_t k_min_game_payload = static_cast<std::uint32_t>(data_packet::header_size);
    constexpr std::uint32_t k_max_game_payload =
        static_cast<std::uint32_t>(in_packet_max_text_size + data_packet::header_size);
    constexpr std::uint32_t k_preferred_client_udp = k_max_game_payload;

    // One `game::tick`: dequeue whatever is ready now—no wall-clock wait on empty RX, no kernel UDP
    // fallback. If a batch is full (`n == SNAKEIO_DOCA_RX_BATCH_MAX`), poll again immediately in case
    // more frames are already queued (still no blocking wait when `n == 0`).
    constexpr int k_max_batches_per_tick = 64;

    const auto ingest_frame = [&](std::uint32_t i) {
        if (produced >= out.size())
            return;
        const auto& st = doca_->stage_host()[i];
        if (!st.valid)
            return;
        if (st.payload_len < k_min_game_payload || st.payload_len > k_max_game_payload)
            return;
        sockaddr_storage ss{};
        if (st.addr_family == AF_INET6) {
            sockaddr_in6 sin6{};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = st.src_port_be;
            std::memcpy(&sin6.sin6_addr.s6_addr, st.src_ipv6_be, sizeof(st.src_ipv6_be));
            std::memcpy(&ss, &sin6, sizeof(sin6));
        } else {
            // IPv4-mapped IPv6 for `sendto` on the kernel egress socket (dual-stack bind).
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
        gpu::ingest_packet_gpu_from_device(gs,
            reinterpret_cast<const std::byte*>(st.payload_dev_va),
            st.payload_len);
        if (!gs.host_ingress->ok) {
            if (doca_debug)
                ++dbg_ingest_reject;
            return;
        }
        const id_t session_id = gs.host_ingress->session_id;
        const id_t player_id = gs.host_ingress->player_id;
        cudaMemcpyAsync(gs.client_addrs + gpu::client_index(session_id, player_id) * sizeof(sockaddr_storage),
            &ss,
            sizeof(sockaddr_storage),
            cudaMemcpyHostToDevice,
            stream);
        ++produced;
    };

    for (int batch = 0; batch < k_max_batches_per_tick && produced < out.size(); ++batch) {
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
        ++dbg_batches;
        dbg_sum_n += n;
        dbg_max_n = std::max(dbg_max_n, n);
        if (doca_debug) {
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto& st = doca_->stage_host()[i];
                if (!st.valid)
                    continue;
                ++dbg_stage_valid;
                if (st.payload_len < k_min_game_payload || st.payload_len > k_max_game_payload)
                    ++dbg_len_out_of_range;
            }
        }

        if (tracing_peer) {
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto& st = doca_->stage_host()[i];
                if (!st.valid || !stage_src_ipv4_equals(st, trace_peer_be))
                    continue;
                if (!trace_peer_all_lengths
                    && (st.payload_len < k_min_game_payload || st.payload_len > k_max_game_payload))
                    continue;
                ++trace_peer_hits;
                if (st.payload_len == k_preferred_client_udp)
                    ++trace_peer_hits_wire48;
                if (trace_peer_only_wire48 && st.payload_len != k_preferred_client_udp)
                    continue;
                if (trace_peer_logged >= k_trace_peer_max_lines)
                    continue;
                constexpr std::size_t k_prefix = 16u;
                unsigned char prefix[k_prefix]{};
                const std::size_t ncopy = std::min<std::size_t>(st.payload_len, k_prefix);
                if (ncopy > 0
                    && cudaMemcpy(prefix,
                            reinterpret_cast<const void*>(st.payload_dev_va),
                            ncopy,
                            cudaMemcpyDeviceToHost)
                        != cudaSuccess) {
                    logger::debug(
                        "DOCA trace_peer: batch={} idx={} plen={} sport={} (cudaMemcpy payload prefix failed).",
                        batch,
                        static_cast<unsigned>(i),
                        static_cast<unsigned>(st.payload_len),
                        static_cast<unsigned>(ntohs(st.src_port_be)));
                } else {
                    char hex[k_prefix * 2 + 1]{};
                    for (std::size_t b = 0; b < ncopy; ++b)
                        std::snprintf(hex + b * 2, 3, "%02x", static_cast<unsigned>(prefix[b]));
                    logger::debug(
                        "DOCA trace_peer: GPUNetIO RX staging hit peer={} batch={} idx={} af={} plen={} sport={} "
                        "payload_prefix_hex={}",
                        trace_peer,
                        batch,
                        static_cast<unsigned>(i),
                        static_cast<unsigned>(st.addr_family),
                        static_cast<unsigned>(st.payload_len),
                        static_cast<unsigned>(ntohs(st.src_port_be)),
                        hex);
                }
                ++trace_peer_logged;
            }
        }

        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& st = doca_->stage_host()[i];
            if (st.valid && st.payload_len == k_preferred_client_udp)
                ingest_frame(i);
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& st = doca_->stage_host()[i];
            if (st.valid && st.payload_len != k_preferred_client_udp)
                ingest_frame(i);
        }

        if (n < SNAKEIO_DOCA_RX_BATCH_MAX)
            break;
    }
    (void)sock; // GPUNetIO ingress is GPU RXQ only; `game::open_data_port` may not be a UDP socket.
    cudaStreamSynchronize(stream);
    if (tracing_peer) {
        if (trace_peer_hits == 0) {
            logger::debug(
                "DOCA trace_peer: no UDP from {} reached GPUNetIO RX staging this tick (or none in "
                "payload_len [{}..{}]; set SNAKEIO_DOCA_TRACE_PEER_ALL_LEN=1 to log all lengths).",
                trace_peer,
                static_cast<unsigned>(k_min_game_payload),
                static_cast<unsigned>(k_max_game_payload));
        } else {
            logger::debug(
                "DOCA trace_peer: {} UDP frame(s) from {} matched GPUNetIO staging this tick "
                "({} with udp_payload_len=={}, {} detail line(s) max{}).",
                trace_peer_hits,
                trace_peer,
                trace_peer_hits_wire48,
                static_cast<unsigned>(k_preferred_client_udp),
                static_cast<unsigned>(trace_peer_logged),
                trace_peer_only_wire48 ? "; ONLY_WIRE48 filter on" : "");
            if (trace_peer_hits != 0 && trace_peer_hits_wire48 == 0) {
                logger::debug(
                    "DOCA trace_peer: hint — no {}-byte UDP payload from {}; remote E2E uses that wire size. "
                    "Same IPv4 often carries other UDP (metrics/DNS-style); those fail gpu::k_ingest "
                    "(verify_and_decrypt / session). Set SNAKEIO_DOCA_TRACE_PEER_ONLY_WIRE48=1 to log only "
                    "that length.",
                    static_cast<unsigned>(k_preferred_client_udp),
                    trace_peer);
            }
        }
    }
    if (doca_debug) {
        logger::debug(
            "DOCA poll_ingress: receive_batches={} max_n={} wire_frames_total={} stage_valid_cells={} "
            "payload_len_outside_game_range={} ingest_verify_fail={} accepted_game_packets={}.",
            dbg_batches,
            static_cast<unsigned>(dbg_max_n),
            static_cast<unsigned>(dbg_sum_n),
            static_cast<unsigned>(dbg_stage_valid),
            static_cast<unsigned>(dbg_len_out_of_range),
            static_cast<unsigned>(dbg_ingest_reject),
            static_cast<unsigned>(produced));
        if (produced == 0 && dbg_sum_n > 0) {
            logger::debug(
                "DOCA poll_ingress: GPU RX saw {} UDP frame(s) this tick but accepted_game_packets=0 "
                "(check payload_len in [{}, {}] bytes, session/crypto ingest, or single-tick dequeue timing).",
                static_cast<unsigned>(dbg_sum_n),
                static_cast<unsigned>(k_min_game_payload),
                static_cast<unsigned>(k_max_game_payload));
        }
    }
    return produced;
}

snakeio::size_t snakeio::doca_gpunetio::runtime::emit_egress_batch(
    snakeio::gpu::device_state& gs, int sock) noexcept
{
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(gs.stream);
    unsigned send_count = 0;
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

    if (doca_active() && doca_->gpu_tx_ready()) {
        const doca_error_t tr = doca_->emit_gpu_tx(stream, gs);
        if (tr == DOCA_SUCCESS) {
            return static_cast<snakeio::size_t>(send_count);
        }
        logger::warn("DOCA GPU egress failed ({}); falling back to sendto for this tick.",
            doca_error_get_descr(tr));
    } else if (doca_active()) {
        static bool warned_gpu_tx = false;
        if (!warned_gpu_tx) {
            warned_gpu_tx = true;
            logger::warn(
                "DOCA: GPU Eth TXQ not ready — using kernel sendto until GPU egress is initialized.");
        }
    }

    cudaMemcpyAsync(gs.host_send_descs,
        gs.send_descs,
        sizeof(gpu::send_desc) * send_count,
        cudaMemcpyDeviceToHost,
        stream);
    cudaStreamSynchronize(stream);

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
        const int send_sock = (doca_->kernel_egress_sock() >= 0) ? doca_->kernel_egress_sock() : sock;
        sendto(send_sock, std::span(gs.host_packet_copy, desc.bytes_size), addr);
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
