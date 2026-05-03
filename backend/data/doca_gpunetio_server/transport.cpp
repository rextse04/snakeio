#include "transport.hpp"
#include <doca_gpunetio.h>
#include <doca_dev.h>
#include <doca_ctx.h>
#include <doca_eth_rxq.h>
#include <doca_eth_txq.h>
#include <doca_pe.h>
#include <doca_mmap.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_eth_rxq_gpu_data_path.h>
#include <doca_eth_txq_gpu_data_path.h>
#include <doca_eth_txq_cpu_data_path.h>
#include <doca_error.h>
#include <logger.hpp>
#include <cstdlib>
#include <cstddef>
#include <algorithm>
#include <limits>
#include <memory>

namespace snakeio::doca_gpunetio {
    namespace {
        constexpr size_t clients_size = game_max_sessions * game_max_players;

        [[nodiscard]] constexpr size_t client_index(id_t session_id, id_t player_id) noexcept {
            return static_cast<size_t>(session_id) * game_max_players + player_id;
        }

        [[noreturn]] void fail_doca_startup(const char* what) {
            logger::error("Failed to initialize DOCA GPUNetIO transport: {}.", what);
            std::exit(EXIT_FAILURE);
        }

        [[nodiscard]] size_t read_env_size_t(
            const char* name,
            size_t default_value,
            size_t min_value,
            size_t max_value) noexcept {
            const char* const raw = std::getenv(name);
            if (raw == nullptr || raw[0] == '\0') {
                return default_value;
            }
            char* end = nullptr;
            const auto parsed = std::strtoull(raw, &end, 10);
            if (end == raw || *end != '\0') {
                logger::warn("Invalid {}='{}'; using default {}.", name, raw, default_value);
                return default_value;
            }
            constexpr auto size_t_max_u64 = static_cast<unsigned long long>(std::numeric_limits<size_t>::max());
            const auto bounded = std::min(parsed, size_t_max_u64);
            const size_t value = static_cast<size_t>(bounded);
            return std::clamp(value, min_value, max_value);
        }

        [[nodiscard]] const char* doca_err_name(doca_error_t err) noexcept {
            return doca_error_get_name(err);
        }

        [[nodiscard]] const char* doca_err_descr(doca_error_t err) noexcept {
            return doca_error_get_descr(err);
        }

        [[nodiscard]] doca_dev* open_device_by_pci_or_exit(const char* pci_addr) {
            doca_devinfo** dev_list = nullptr;
            uint32_t nb_devs = 0;
            const doca_error_t list_res = doca_devinfo_create_list(&dev_list, &nb_devs);
            if (list_res != DOCA_SUCCESS) {
                logger::error("doca_devinfo_create_list failed: {} ({})",
                    doca_err_name(list_res), doca_err_descr(list_res));
                std::exit(EXIT_FAILURE);
            }

            doca_dev* dev = nullptr;
            for (uint32_t i = 0; i < nb_devs && dev == nullptr; ++i) {
                uint8_t is_equal = 0;
                const doca_error_t cmp_res = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_equal);
                if (cmp_res != DOCA_SUCCESS) {
                    logger::warn("doca_devinfo_is_equal_pci_addr failed: {} ({})",
                        doca_err_name(cmp_res), doca_err_descr(cmp_res));
                    continue;
                }
                if (!is_equal) {
                    continue;
                }
                const doca_error_t open_res = doca_dev_open(dev_list[i], &dev);
                if (open_res != DOCA_SUCCESS) {
                    logger::error("doca_dev_open({}) failed: {} ({})",
                        pci_addr, doca_err_name(open_res), doca_err_descr(open_res));
                    std::exit(EXIT_FAILURE);
                }
            }

            (void) doca_devinfo_destroy_list(dev_list);
            if (dev == nullptr) {
                logger::error("No DOCA device found for PCI address {}.", pci_addr);
                std::exit(EXIT_FAILURE);
            }
            return dev;
        }

        void tx_task_complete_cb(struct doca_eth_txq_task_send* task_send,
            union doca_data,
            union doca_data) {
            doca_buf* pkt = nullptr;
            if (doca_eth_txq_task_send_get_pkt(task_send, &pkt) == DOCA_SUCCESS && pkt != nullptr) {
                uint16_t refcount = 0;
                (void) doca_buf_dec_refcount(pkt, &refcount);
            }
            doca_task_free(doca_eth_txq_task_send_as_doca_task(task_send));
        }
    }

    transport::transport() {
        peer_known_.fill(false);
        tx_stage_capacity_runtime_ = read_env_size_t(
            "SNAKEIO_DOCA_TX_STAGE_CAPACITY", tx_stage_capacity_, 1, tx_stage_capacity_max_);
        native_tx_mirror_enabled_ = native_tx_requested_;
        native_tx_mirror_strict_ = read_env_size_t("SNAKEIO_DOCA_TX_MIRROR_STRICT", 0, 0, 1) != 0;

        const char* const gpu_pci = std::getenv("SNAKEIO_DOCA_GPU_PCI");
        if (gpu_pci == nullptr || gpu_pci[0] == '\0') {
            fail_doca_startup("SNAKEIO_DOCA_GPU_PCI is required when DOCA transport is enabled");
        }
        const char* const nic_pci = std::getenv("SNAKEIO_DOCA_NIC_PCI");
        if (nic_pci == nullptr || nic_pci[0] == '\0') {
            fail_doca_startup("SNAKEIO_DOCA_NIC_PCI is required when DOCA transport is enabled");
        }

        const doca_error_t create_res = doca_gpu_create(gpu_pci, &gpu_dev_);
        if (create_res != DOCA_SUCCESS) {
            logger::error("doca_gpu_create({}) failed: {} ({})",
                gpu_pci, doca_err_name(create_res), doca_err_descr(create_res));
            std::exit(EXIT_FAILURE);
        }

        dev_ = open_device_by_pci_or_exit(nic_pci);
        const doca_error_t rxq_res = doca_eth_rxq_create(
            dev_, 64, in_packet_max_text_size + data_packet::header_size, &rxq_);
        if (rxq_res != DOCA_SUCCESS) {
            logger::error("doca_eth_rxq_create failed: {} ({})",
                doca_err_name(rxq_res), doca_err_descr(rxq_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t txq_res = doca_eth_txq_create(dev_, 64, &txq_);
        if (txq_res != DOCA_SUCCESS) {
            logger::error("doca_eth_txq_create failed: {} ({})",
                doca_err_name(txq_res), doca_err_descr(txq_res));
            std::exit(EXIT_FAILURE);
        }

        const doca_error_t rx_type_res = doca_eth_rxq_set_type(rxq_, DOCA_ETH_RXQ_TYPE_CYCLIC);
        if (rx_type_res != DOCA_SUCCESS) {
            logger::error("doca_eth_rxq_set_type failed: {} ({})",
                doca_err_name(rx_type_res), doca_err_descr(rx_type_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t tx_type_res = doca_eth_txq_set_type(txq_, DOCA_ETH_TXQ_TYPE_REGULAR);
        if (tx_type_res != DOCA_SUCCESS) {
            logger::error("doca_eth_txq_set_type failed: {} ({})",
                doca_err_name(tx_type_res), doca_err_descr(tx_type_res));
            std::exit(EXIT_FAILURE);
        }

        const doca_error_t rx_uar_res = doca_eth_rxq_gpu_set_uar_on_cpu(rxq_);
        if (rx_uar_res != DOCA_SUCCESS) {
            logger::error("doca_eth_rxq_gpu_set_uar_on_cpu failed: {} ({})",
                doca_err_name(rx_uar_res), doca_err_descr(rx_uar_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t tx_uar_res = doca_eth_txq_gpu_set_uar_on_cpu(txq_);
        if (tx_uar_res != DOCA_SUCCESS) {
            logger::error("doca_eth_txq_gpu_set_uar_on_cpu failed: {} ({})",
                doca_err_name(tx_uar_res), doca_err_descr(tx_uar_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t rx_mem_res = doca_eth_rxq_gpu_set_rq_mem_type(rxq_, DOCA_GPU_MEM_TYPE_GPU_CPU);
        if (rx_mem_res != DOCA_SUCCESS) {
            logger::error("doca_eth_rxq_gpu_set_rq_mem_type failed: {} ({})",
                doca_err_name(rx_mem_res), doca_err_descr(rx_mem_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t tx_mem_res = doca_eth_txq_gpu_set_sq_mem_type(txq_, DOCA_GPU_MEM_TYPE_GPU_CPU);
        if (tx_mem_res != DOCA_SUCCESS) {
            logger::error("doca_eth_txq_gpu_set_sq_mem_type failed: {} ({})",
                doca_err_name(tx_mem_res), doca_err_descr(tx_mem_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t tx_comp_res = doca_eth_txq_gpu_set_completion_on_gpu(txq_);
        if (tx_comp_res != DOCA_SUCCESS) {
            logger::error("doca_eth_txq_gpu_set_completion_on_gpu failed: {} ({})",
                doca_err_name(tx_comp_res), doca_err_descr(tx_comp_res));
            std::exit(EXIT_FAILURE);
        }

        const doca_error_t tx_task_conf_res =
            doca_eth_txq_task_send_set_conf(txq_, tx_task_complete_cb, tx_task_complete_cb, tx_stage_capacity_runtime_);
        if (tx_task_conf_res != DOCA_SUCCESS) {
            logger::error("doca_eth_txq_task_send_set_conf failed: {} ({})",
                doca_err_name(tx_task_conf_res), doca_err_descr(tx_task_conf_res));
            std::exit(EXIT_FAILURE);
        }

        rx_ctx_ = doca_eth_rxq_as_doca_ctx(rxq_);
        tx_ctx_ = doca_eth_txq_as_doca_ctx(txq_);
        if (rx_ctx_ == nullptr || tx_ctx_ == nullptr) {
            fail_doca_startup("Failed to convert ETH queues to DOCA contexts");
        }

        const doca_error_t pe_create_res = doca_pe_create(&pe_);
        if (pe_create_res != DOCA_SUCCESS) {
            logger::error("doca_pe_create failed: {} ({})",
                doca_err_name(pe_create_res), doca_err_descr(pe_create_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t pe_mode_res = doca_pe_set_event_mode(pe_, DOCA_PE_EVENT_MODE_PROGRESS_ALL);
        if (pe_mode_res != DOCA_SUCCESS) {
            logger::error("doca_pe_set_event_mode failed: {} ({})",
                doca_err_name(pe_mode_res), doca_err_descr(pe_mode_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t pe_rx_res = doca_pe_connect_ctx(pe_, rx_ctx_);
        if (pe_rx_res != DOCA_SUCCESS) {
            logger::error("doca_pe_connect_ctx(rx) failed: {} ({})",
                doca_err_name(pe_rx_res), doca_err_descr(pe_rx_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t pe_tx_res = doca_pe_connect_ctx(pe_, tx_ctx_);
        if (pe_tx_res != DOCA_SUCCESS) {
            logger::error("doca_pe_connect_ctx(tx) failed: {} ({})",
                doca_err_name(pe_tx_res), doca_err_descr(pe_tx_res));
            std::exit(EXIT_FAILURE);
        }

        const doca_error_t rx_dp_res = doca_ctx_set_datapath_on_gpu(rx_ctx_, gpu_dev_);
        if (rx_dp_res != DOCA_SUCCESS) {
            logger::error("doca_ctx_set_datapath_on_gpu(rx) failed: {} ({})",
                doca_err_name(rx_dp_res), doca_err_descr(rx_dp_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t tx_dp_res = doca_ctx_set_datapath_on_gpu(tx_ctx_, gpu_dev_);
        if (tx_dp_res != DOCA_SUCCESS) {
            logger::error("doca_ctx_set_datapath_on_gpu(tx) failed: {} ({})",
                doca_err_name(tx_dp_res), doca_err_descr(tx_dp_res));
            std::exit(EXIT_FAILURE);
        }

        const doca_error_t rx_start_res = doca_ctx_start(rx_ctx_);
        if (rx_start_res != DOCA_SUCCESS) {
            logger::error("doca_ctx_start(rx) failed: {} ({})",
                doca_err_name(rx_start_res), doca_err_descr(rx_start_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t tx_start_res = doca_ctx_start(tx_ctx_);
        if (tx_start_res != DOCA_SUCCESS) {
            logger::error("doca_ctx_start(tx) failed: {} ({})",
                doca_err_name(tx_start_res), doca_err_descr(tx_start_res));
            std::exit(EXIT_FAILURE);
        }

        scratch_bytes_ = std::max<size_t>(
            4096,
            tx_stage_capacity_runtime_ *
                (static_cast<size_t>(sizeof(shadow_tx_desc)) + staged_packet_capacity_));
        const doca_error_t alloc_res = doca_gpu_mem_alloc(
            gpu_dev_, scratch_bytes_, alignof(std::max_align_t), DOCA_GPU_MEM_TYPE_GPU_CPU, &scratch_gpu_ptr_, &scratch_cpu_ptr_);
        if (alloc_res != DOCA_SUCCESS) {
            logger::error("doca_gpu_mem_alloc failed: {} ({})",
                doca_err_name(alloc_res), doca_err_descr(alloc_res));
            std::exit(EXIT_FAILURE);
        }
        shadow_descs_cpu_ = static_cast<shadow_tx_desc*>(scratch_cpu_ptr_);
        scratch_desc_bytes_ = tx_stage_capacity_runtime_ * static_cast<size_t>(sizeof(shadow_tx_desc));
        scratch_payload_bytes_ = scratch_bytes_ - scratch_desc_bytes_;
        shadow_payloads_cpu_ = static_cast<std::byte*>(scratch_cpu_ptr_) + scratch_desc_bytes_;

        const doca_error_t mmap_create_res = doca_mmap_create(&tx_mmap_);
        if (mmap_create_res != DOCA_SUCCESS) {
            logger::error("doca_mmap_create failed: {} ({})",
                doca_err_name(mmap_create_res), doca_err_descr(mmap_create_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t mmap_dev_res = doca_mmap_add_dev(tx_mmap_, dev_);
        if (mmap_dev_res != DOCA_SUCCESS) {
            logger::error("doca_mmap_add_dev failed: {} ({})",
                doca_err_name(mmap_dev_res), doca_err_descr(mmap_dev_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t mmap_perm_res = doca_mmap_set_permissions(
            tx_mmap_, DOCA_ACCESS_FLAG_PCI_READ_WRITE | DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
        if (mmap_perm_res != DOCA_SUCCESS) {
            logger::error("doca_mmap_set_permissions failed: {} ({})",
                doca_err_name(mmap_perm_res), doca_err_descr(mmap_perm_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t mmap_range_res =
            doca_mmap_set_memrange(tx_mmap_, shadow_payloads_cpu_, scratch_payload_bytes_);
        if (mmap_range_res != DOCA_SUCCESS) {
            logger::error("doca_mmap_set_memrange failed: {} ({})",
                doca_err_name(mmap_range_res), doca_err_descr(mmap_range_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t mmap_start_res = doca_mmap_start(tx_mmap_);
        if (mmap_start_res != DOCA_SUCCESS) {
            logger::error("doca_mmap_start failed: {} ({})",
                doca_err_name(mmap_start_res), doca_err_descr(mmap_start_res));
            std::exit(EXIT_FAILURE);
        }

        const doca_error_t inv_create_res =
            doca_buf_inventory_create(tx_stage_capacity_runtime_ * 2, &tx_buf_inventory_);
        if (inv_create_res != DOCA_SUCCESS) {
            logger::error("doca_buf_inventory_create failed: {} ({})",
                doca_err_name(inv_create_res), doca_err_descr(inv_create_res));
            std::exit(EXIT_FAILURE);
        }
        const doca_error_t inv_start_res = doca_buf_inventory_start(tx_buf_inventory_);
        if (inv_start_res != DOCA_SUCCESS) {
            logger::error("doca_buf_inventory_start failed: {} ({})",
                doca_err_name(inv_start_res), doca_err_descr(inv_start_res));
            std::exit(EXIT_FAILURE);
        }

        if (shadow_descs_cpu_ == nullptr || shadow_payloads_cpu_ == nullptr) {
            native_tx_active_ = false;
        }
        if (!native_tx_active_ && native_tx_mirror_strict_) {
            fail_doca_startup("Native TX mirror buffers are unavailable");
        }
    }

    transport::~transport() noexcept {
        if (pe_ != nullptr) {
            while (doca_pe_progress(pe_) != 0) {
            }
        }
        if (rx_ctx_ != nullptr) {
            (void) doca_ctx_stop(rx_ctx_);
        }
        if (tx_ctx_ != nullptr) {
            (void) doca_ctx_stop(tx_ctx_);
        }
        if (rxq_ != nullptr) {
            (void) doca_eth_rxq_destroy(rxq_);
        }
        if (txq_ != nullptr) {
            (void) doca_eth_txq_destroy(txq_);
        }
        if (tx_buf_inventory_ != nullptr) {
            (void) doca_buf_inventory_stop(tx_buf_inventory_);
            (void) doca_buf_inventory_destroy(tx_buf_inventory_);
        }
        if (tx_mmap_ != nullptr) {
            (void) doca_mmap_stop(tx_mmap_);
            (void) doca_mmap_destroy(tx_mmap_);
        }
        if (dev_ != nullptr) {
            (void) doca_dev_close(dev_);
        }
        if (gpu_dev_ != nullptr && scratch_gpu_ptr_ != nullptr) {
            (void) doca_gpu_mem_free(gpu_dev_, scratch_gpu_ptr_);
        }
        if (gpu_dev_ != nullptr) {
            (void) doca_gpu_destroy(gpu_dev_);
        }
        if (pe_ != nullptr) {
            (void) doca_pe_destroy(pe_);
        }
        logger::debug("DOCA TX stats: staged={}, flushed={}, overflow_fallback={}, oversize_fallback={}, cache_hit={}, cache_miss={}, stage_cap={}",
            tx_stats_.staged,
            tx_stats_.flushed,
            tx_stats_.fallback_overflow,
            tx_stats_.fallback_oversize,
            tx_stats_.cache_hit,
            tx_stats_.cache_miss,
            tx_stage_capacity_runtime_);
    }

    void transport::progress() noexcept {
        if (pe_ != nullptr) {
            (void) doca_pe_progress(pe_);
        }
        const doca_error_t rx_res = doca_eth_rxq_gpu_cpu_proxy_progress(rxq_);
        if (rx_res != DOCA_SUCCESS) {
            fail_doca_startup("RX proxy progress failed (no POSIX fallback available)");
        }
        const doca_error_t tx_res = doca_eth_txq_gpu_cpu_proxy_progress(txq_);
        if (tx_res != DOCA_SUCCESS) {
            fail_doca_startup("TX proxy progress failed (no POSIX fallback available)");
        }
    }

    void transport::observe_ingress_peer(id_t session_id,
        id_t player_id,
        const sockaddr_storage& addr) noexcept {
        if (session_id >= game_max_sessions || player_id >= game_max_players) {
            return;
        }
        const size_t idx = client_index(session_id, player_id);
        peer_addrs_[idx] = addr;
        peer_known_[idx] = true;
    }

    [[nodiscard]] bool transport::try_send_cached_peer(int sock,
        std::span<std::byte> bytes,
        id_t session_id,
        id_t player_id) noexcept {
        if (session_id >= game_max_sessions || player_id >= game_max_players) {
            return false;
        }
        const size_t idx = client_index(session_id, player_id);
        if (!peer_known_[idx]) {
            ++tx_stats_.cache_miss;
            return false;
        }
        ++tx_stats_.cache_hit;
        return stage_tx_or_drop(sock, bytes, session_id, player_id, peer_addrs_[idx]);
    }

    bool transport::recv(std::stop_token,
        int,
        std::span<std::byte>,
        ingress_packet&) noexcept {
        fail_doca_startup("DOCA RX datapath is required; POSIX recv fallback has been removed");
    }

    void transport::send(int sock,
        std::span<std::byte> bytes,
        id_t session_id,
        id_t player_id,
        const sockaddr_storage& fallback_addr) noexcept {
        (void) stage_tx_or_drop(sock, bytes, session_id, player_id, fallback_addr);
    }

    void transport::flush_tx(int) noexcept {
        size_t shadow_desc_count = 0;
        while (staged_size_ != 0) {
            staged_tx_item& item = staged_tx_[staged_head_];
            shadow_tx_desc desc{};
            bool desc_ready = false;
            if (shadow_descs_cpu_ != nullptr && shadow_desc_count < tx_stage_capacity_runtime_) {
                desc = shadow_descs_cpu_[shadow_desc_count];
                desc.bytes_size = static_cast<std::uint_least32_t>(item.bytes_size);
                desc.session_id = item.session_id;
                desc.player_id = item.player_id;
                desc.ring_slot = static_cast<std::uint_least32_t>(staged_head_);
                desc.addr = item.addr;
                shadow_descs_cpu_[shadow_desc_count] = desc;
                ++shadow_desc_count;
                ++tx_stats_.shadow_desc_prepared;
                desc_ready = true;
            }
            bool sent_natively = false;
            if (desc_ready) {
                sent_natively = try_native_tx_submit(item, desc);
            }
            if (!sent_natively) {
                ++tx_stats_.dropped_submit_no_fallback;
            }
            ++tx_stats_.flushed;
            staged_head_ = (staged_head_ + 1) % tx_stage_capacity_runtime_;
            --staged_size_;
        }
    }

    [[nodiscard]] bool transport::try_native_tx_submit(
        const staged_tx_item& item,
        const shadow_tx_desc& desc) noexcept {
        if (!native_tx_active_) {
            return false;
        }
        ++tx_stats_.native_attempted;
        if (!native_tx_mirror_enabled_ || shadow_payloads_cpu_ == nullptr) {
            ++tx_stats_.native_fallback_disabled;
            return false;
        }
        if (item.bytes_size > staged_packet_capacity_) {
            ++tx_stats_.native_mirror_error;
            return false;
        }
        const size_t payload_off = static_cast<size_t>(desc.ring_slot) * staged_packet_capacity_;
        if (payload_off + item.bytes_size > scratch_payload_bytes_) {
            ++tx_stats_.native_mirror_error;
            return false;
        }
        std::copy(item.bytes.begin(), item.bytes.begin() + item.bytes_size, shadow_payloads_cpu_ + payload_off);
        shadow_descs_cpu_[desc.ring_slot].payload_offset = static_cast<std::uint_least32_t>(payload_off);

        doca_buf* pkt_buf = nullptr;
        const doca_error_t buf_res = doca_buf_inventory_buf_get_by_data(
            tx_buf_inventory_, tx_mmap_, shadow_payloads_cpu_ + payload_off, item.bytes_size, &pkt_buf);
        if (buf_res != DOCA_SUCCESS || pkt_buf == nullptr) {
            ++tx_stats_.native_fallback_error;
            return false;
        }

        doca_eth_txq_task_send* tx_task = nullptr;
        const doca_error_t task_res = doca_eth_txq_task_send_allocate_init(txq_, pkt_buf, {.ptr = nullptr}, &tx_task);
        if (task_res != DOCA_SUCCESS || tx_task == nullptr) {
            ++tx_stats_.native_fallback_error;
            uint16_t refcount = 0;
            (void) doca_buf_dec_refcount(pkt_buf, &refcount);
            return false;
        }

        const doca_error_t submit_res = doca_task_submit(doca_eth_txq_task_send_as_doca_task(tx_task));
        if (submit_res != DOCA_SUCCESS) {
            ++tx_stats_.native_fallback_error;
            doca_task_free(doca_eth_txq_task_send_as_doca_task(tx_task));
            uint16_t refcount = 0;
            (void) doca_buf_dec_refcount(pkt_buf, &refcount);
            return false;
        }
        ++tx_stats_.native_submitted;
        return true;
    }

    [[nodiscard]] bool transport::stage_tx_or_drop(int,
        std::span<std::byte> bytes,
        id_t session_id,
        id_t player_id,
        const sockaddr_storage& addr) noexcept {
        if (bytes.size() > staged_packet_capacity_) {
            ++tx_stats_.fallback_oversize;
            ++tx_stats_.dropped_oversize_no_fallback;
            return true;
        }
        if (staged_size_ >= tx_stage_capacity_runtime_) {
            ++tx_stats_.fallback_overflow;
            ++tx_stats_.dropped_overflow_no_fallback;
            return true;
        }

        const size_t write_idx = (staged_head_ + staged_size_) % tx_stage_capacity_runtime_;
        staged_tx_item& item = staged_tx_[write_idx];
        std::copy(bytes.begin(), bytes.end(), item.bytes.begin());
        item.bytes_size = static_cast<size_t>(bytes.size());
        item.session_id = session_id;
        item.player_id = player_id;
        item.addr = addr;
        ++staged_size_;
        ++tx_stats_.staged;
        return true;
    }
}

