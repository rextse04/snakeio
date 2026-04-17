#include "doca_context.hpp"
#include <logger.hpp>
#include <packet.hpp>

extern "C" {
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_eth_rxq.h>
#include <doca_eth_txq.h>
#include <doca_gpunetio.h>
#include <doca_mmap.h>
#include <doca_flow.h>
#include <rte_byteorder.h>
}

#include <cstring>
#include <unistd.h>

namespace {
    using namespace snakeio::doca_gpunetio_server;

    constexpr std::size_t gpu_bus_id_size = 32;

    bool doca_ok(const doca_error_t rc, const char* step) noexcept {
        if (rc == DOCA_SUCCESS) return true;
        snakeio::logger::error("DOCA step '{}' failed: {} ({}).",
            step,
            doca_error_get_name(rc),
            doca_error_get_descr(rc));
        return false;
    }

    bool device_matches_filter(const doca_devinfo* info, const char* filter) noexcept {
        if (filter == nullptr || *filter == '\0') return true;
        uint8_t matches = 0;
        return doca_devinfo_is_equal_pci_addr(info, filter, &matches) == DOCA_SUCCESS && matches == 1;
    }

    bool device_supports_gpu_eth(const doca_devinfo* info) noexcept {
        return doca_eth_rxq_cap_is_type_supported(
                   info, DOCA_ETH_RXQ_TYPE_CYCLIC, DOCA_ETH_RXQ_DATA_PATH_TYPE_GPU)
                   == DOCA_SUCCESS
            && doca_eth_txq_cap_is_type_supported(
                   info, DOCA_ETH_TXQ_TYPE_REGULAR, DOCA_ETH_TXQ_DATA_PATH_TYPE_GPU)
                   == DOCA_SUCCESS;
    }

    bool open_compatible_nic(const char* filter,
        struct doca_dev** out_dev,
        char (&pci_addr)[DOCA_DEVINFO_PCI_ADDR_SIZE]) noexcept {
        *out_dev = nullptr;

        struct doca_devinfo** infos = nullptr;
        uint32_t count = 0;
        if (!doca_ok(doca_devinfo_create_list(&infos, &count), "doca_devinfo_create_list")) {
            return false;
        }

        bool opened = false;
        for (uint32_t i = 0; i < count; ++i) {
            auto* info = infos[i];
            if (!device_matches_filter(info, filter)) continue;
            if (!device_supports_gpu_eth(info)) continue;

            struct doca_dev* dev = nullptr;
            if (doca_dev_open(info, &dev) != DOCA_SUCCESS) continue;

            std::memset(pci_addr, 0, DOCA_DEVINFO_PCI_ADDR_SIZE);
            (void) doca_devinfo_get_pci_addr_str(info, pci_addr);
            *out_dev = dev;
            opened = true;
            break;
        }

        (void) doca_devinfo_destroy_list(infos);

        if (!opened) {
            snakeio::logger::error("No DOCA NIC compatible with GPU ETH RXQ/TXQ was found.");
        }
        return opened;
    }

    bool resolve_gpu_bus_id(const config& cfg, char (&gpu_bus_id)[gpu_bus_id_size]) noexcept {
        if (cfg.gpu_pci_addr[0] != '\0') {
            std::strncpy(gpu_bus_id, cfg.gpu_pci_addr.data(), gpu_bus_id_size - 1);
            gpu_bus_id[gpu_bus_id_size - 1] = '\0';

            // nvidia-smi can report an 8-digit PCI domain; DOCA expects 4-digit domain BDF.
            if (std::strlen(gpu_bus_id) >= 13 && gpu_bus_id[8] == ':') {
                std::memmove(gpu_bus_id, gpu_bus_id + 4, std::strlen(gpu_bus_id + 4) + 1);
            }
            return true;
        }

        snakeio::logger::error("DOCA GPUNetIO requires configured GPU PCI address (SNAKEIO_DOCA_GPU_PCI).");
        return false;
    }

    bool init_doca_flow(transport_context& ctx, const config& cfg) noexcept {
        doca_flow_cfg *flow_cfg;
        if (!doca_ok(doca_flow_cfg_create(&flow_cfg), "doca_flow_cfg_create")) return false;

        doca_flow_cfg_set_pipe_queues(flow_cfg, 1);
        doca_flow_cfg_set_mode_args(flow_cfg, "vnf,hws");

        bool ok = doca_ok(doca_flow_init(flow_cfg), "doca_flow_init");
        doca_flow_cfg_destroy(flow_cfg);
        if (!ok) return false;

        doca_flow_port_cfg *port_cfg;
        if (!doca_ok(doca_flow_port_cfg_create(&port_cfg), "doca_flow_port_cfg_create")) return false;
        doca_flow_port_cfg_set_dev(port_cfg, static_cast<doca_dev*>(ctx.doca_dev));

        doca_flow_port *port;
        if (!doca_ok(doca_flow_port_start(port_cfg, &port), "doca_flow_port_start")) {
            doca_flow_port_cfg_destroy(port_cfg);
            return false;
        }
        ctx.doca_flow_port = port;
        doca_flow_port_cfg_destroy(port_cfg);

        doca_flow_pipe_cfg *pipe_cfg;
        if (!doca_ok(doca_flow_pipe_cfg_create(&pipe_cfg, port), "doca_flow_pipe_cfg_create")) return false;
        doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
        doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);

        doca_flow_match match_mask = {};
        doca_flow_match match_value = {};
        match_mask.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
        match_mask.outer.udp.l4_port.dst_port = 0xFFFF;
        match_value.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
        match_value.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(cfg.port);
        doca_flow_pipe_cfg_set_match(pipe_cfg, &match_value, &match_mask);

        doca_flow_fwd fwd_cfg = {};
        fwd_cfg.type = DOCA_FLOW_FWD_PORT;
        fwd_cfg.port_id = 0;

        uint16_t rss_queues[] = {0};
        doca_flow_fwd fwd_rss_cfg = {};
        fwd_rss_cfg.type = DOCA_FLOW_FWD_RSS;
        fwd_rss_cfg.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
        fwd_rss_cfg.rss.queues_array = rss_queues;
        fwd_rss_cfg.rss.nr_queues = 1;

        doca_flow_pipe *rxq_pipe;
        ok = doca_ok(doca_flow_pipe_create(pipe_cfg, &fwd_rss_cfg, nullptr, &rxq_pipe), "doca_flow_pipe_create");
        doca_flow_pipe_cfg_destroy(pipe_cfg);

        if (ok) {
            ctx.doca_flow_pipe = rxq_pipe;

            doca_flow_pipe_entry *entry;
            ok = doca_ok(doca_flow_pipe_basic_add_entry(0, rxq_pipe, &match_value, 0, nullptr, nullptr, nullptr, 0, nullptr, &entry), "doca_flow_pipe_basic_add_entry");
        }

        return ok;
    }
}

namespace snakeio::doca_gpunetio_server {
    bool init_transport(transport_context& ctx, const config& cfg) noexcept {
        shutdown_transport(ctx);
        ctx.backend = cfg.backend;

        if (ctx.backend == backend_kind::host_udp_shim) {
            logger::error("Host UDP shim backend is disabled for this DOCA server build.");
            return false;
        }

        if (ctx.backend != backend_kind::doca_gpunetio) {
            logger::warn("Unknown DOCA backend kind {}, transport disabled.", static_cast<unsigned>(ctx.backend));
            return false;
        }

        char gpu_bus_id[gpu_bus_id_size]{};
        if (!resolve_gpu_bus_id(cfg, gpu_bus_id)) {
            return false;
        }

        char nic_pci[DOCA_DEVINFO_PCI_ADDR_SIZE]{};
        struct doca_dev* dev = nullptr;
        const char* nic_filter = cfg.nic_pci_addr[0] == '\0' ? nullptr : cfg.nic_pci_addr.data();
        if (!open_compatible_nic(nic_filter, &dev, nic_pci)) {
            return false;
        }
        ctx.doca_dev = dev;

        struct doca_gpu* gpu = nullptr;
        if (!doca_ok(doca_gpu_create(gpu_bus_id, &gpu), "doca_gpu_create")) {
            shutdown_transport(ctx);
            return false;
        }
        ctx.doca_gpu = gpu;

        if (!init_doca_flow(ctx, cfg)) {
            logger::error("Failed to intialize DOCA flow.");
            shutdown_transport(ctx);
            return false;
        }

        ctx.rx_ready = true;
        ctx.tx_ready = true;

        // Best-effort RX queue bring-up: ingress currently still runs through host sockets in game::port.
        {
            struct doca_eth_rxq* rxq = nullptr;
            struct doca_mmap* mmap = nullptr;
            int rx_dmabuf_fd = -1;
            const auto max_packet_size = static_cast<uint32_t>(in_packet_max_text_size + data_packet::header_size);
            bool rx_ok = true;

            rx_ok = doca_ok(doca_eth_rxq_create(dev,
                             static_cast<uint32_t>(cfg.rx_burst_size),
                             max_packet_size,
                             &rxq),
                "doca_eth_rxq_create");
            if (rx_ok) rx_ok = doca_ok(doca_eth_rxq_set_type(rxq, DOCA_ETH_RXQ_TYPE_CYCLIC), "doca_eth_rxq_set_type");

            uint32_t pkt_buf_size = 0;
            if (rx_ok
                && !doca_ok(doca_eth_rxq_estimate_packet_buf_size(
                                DOCA_ETH_RXQ_TYPE_CYCLIC,
                                1024,
                                500,
                                max_packet_size,
                                static_cast<uint32_t>(cfg.rx_burst_size),
                                0,
                                0,
                                0,
                                &pkt_buf_size),
                        "doca_eth_rxq_estimate_packet_buf_size")) {
                pkt_buf_size = max_packet_size * static_cast<uint32_t>(cfg.rx_ring_size);
            }

            void* rx_buf_gpu = nullptr;
            void* rx_buf_cpu = nullptr;
            if (rx_ok) {
                rx_ok = doca_ok(doca_gpu_mem_alloc(gpu,
                                 pkt_buf_size,
                                 64,
                                 DOCA_GPU_MEM_TYPE_GPU_CPU,
                                 &rx_buf_gpu,
                                 &rx_buf_cpu),
                    "doca_gpu_mem_alloc");
            }

            if (rx_ok) {
                ctx.rx_pkt_buf_gpu = rx_buf_gpu;
                ctx.rx_pkt_buf_cpu = rx_buf_cpu;
                ctx.rx_pkt_buf_size = pkt_buf_size;
                const doca_error_t dmabuf_rc = doca_gpu_dmabuf_fd(gpu, rx_buf_gpu, pkt_buf_size, &rx_dmabuf_fd);
                if (dmabuf_rc == DOCA_ERROR_NOT_SUPPORTED) {
                    logger::warn("DOCA DMABUF export not supported; falling back to CPU memrange RX mapping.");
                    rx_dmabuf_fd = -1;
                } else {
                    rx_ok = doca_ok(dmabuf_rc, "doca_gpu_dmabuf_fd");
                }
            }

            if (rx_ok) rx_ok = doca_ok(doca_mmap_create(&mmap), "doca_mmap_create");
            if (rx_ok) {
                if (rx_dmabuf_fd >= 0) {
                    rx_ok = doca_ok(doca_mmap_set_dmabuf_memrange(mmap, rx_dmabuf_fd, rx_buf_gpu, 0, pkt_buf_size),
                        "doca_mmap_set_dmabuf_memrange");
                } else {
                    rx_ok = doca_ok(doca_mmap_set_memrange(mmap, rx_buf_cpu, pkt_buf_size), "doca_mmap_set_memrange");
                }
            }
            if (rx_ok) {
                rx_ok = doca_ok(doca_mmap_set_permissions(mmap,
                            DOCA_ACCESS_FLAG_PCI_READ_WRITE | DOCA_ACCESS_FLAG_LOCAL_READ_WRITE),
                    "doca_mmap_set_permissions");
            }
            if (rx_ok) rx_ok = doca_ok(doca_mmap_add_dev(mmap, dev), "doca_mmap_add_dev");
            if (rx_ok) rx_ok = doca_ok(doca_mmap_start(mmap), "doca_mmap_start");
            if (rx_ok) rx_ok = doca_ok(doca_eth_rxq_set_pkt_buf(rxq, mmap, 0, pkt_buf_size), "doca_eth_rxq_set_pkt_buf");

            if (rx_ok) {
                auto* rx_ctx = doca_eth_rxq_as_doca_ctx(rxq);
                rx_ok = rx_ctx != nullptr
                    && doca_ok(doca_ctx_set_datapath_on_gpu(rx_ctx, gpu), "doca_ctx_set_datapath_on_gpu(rx)")
                    && doca_ok(doca_ctx_start(rx_ctx), "doca_ctx_start(rx)");
                ctx.rx_ctx_started = rx_ok;
            }

            if (rx_ok) {
                struct doca_gpu_eth_rxq* gpu_rxq = nullptr;
                rx_ok = doca_ok(doca_eth_rxq_get_gpu_handle(rxq, &gpu_rxq), "doca_eth_rxq_get_gpu_handle");
                ctx.doca_gpu_rxq = gpu_rxq;
            }

            if (rx_dmabuf_fd >= 0) {
                (void) close(rx_dmabuf_fd);
            }

            if (rx_ok) {
                ctx.doca_rxq = rxq;
                ctx.doca_mmap = mmap;
            } else {
                logger::warn("DOCA RXQ bring-up incomplete; using host-socket ingress path.");
                if (ctx.rx_pkt_buf_gpu != nullptr) {
                    (void) doca_gpu_mem_free(gpu, ctx.rx_pkt_buf_gpu);
                }
                ctx.rx_pkt_buf_gpu = nullptr;
                ctx.rx_pkt_buf_cpu = nullptr;
                ctx.rx_pkt_buf_size = 0;
                if (mmap != nullptr) {
                    (void) doca_mmap_stop(mmap);
                    (void) doca_mmap_destroy(mmap);
                }
                if (rxq != nullptr) {
                    (void) doca_eth_rxq_destroy(rxq);
                }
                ctx.doca_mmap = nullptr;
                ctx.doca_rxq = nullptr;
                ctx.doca_gpu_rxq = nullptr;
                ctx.rx_ctx_started = false;
            }
        }

        // Best-effort TX queue bring-up: host socket flush remains active until TX kernels are wired.
        {
            struct doca_eth_txq* txq = nullptr;
            bool tx_ok = doca_ok(doca_eth_txq_create(dev, static_cast<uint32_t>(cfg.tx_burst_size), &txq), "doca_eth_txq_create")
                && doca_ok(doca_eth_txq_set_type(txq, DOCA_ETH_TXQ_TYPE_REGULAR), "doca_eth_txq_set_type");

            if (tx_ok) {
                auto* tx_ctx = doca_eth_txq_as_doca_ctx(txq);
                tx_ok = tx_ctx != nullptr
                    && doca_ok(doca_ctx_set_datapath_on_gpu(tx_ctx, gpu), "doca_ctx_set_datapath_on_gpu(tx)")
                    && doca_ok(doca_ctx_start(tx_ctx), "doca_ctx_start(tx)");
                ctx.tx_ctx_started = tx_ok;
            }

            if (tx_ok) {
                struct doca_gpu_eth_txq* gpu_txq = nullptr;
                tx_ok = doca_ok(doca_eth_txq_get_gpu_handle(txq, &gpu_txq), "doca_eth_txq_get_gpu_handle");
                ctx.doca_gpu_txq = gpu_txq;
            }

            if (tx_ok) {
                ctx.doca_txq = txq;
            } else {
                logger::warn("DOCA TXQ bring-up incomplete; using host-socket egress path.");
                if (txq != nullptr) {
                    (void) doca_eth_txq_destroy(txq);
                }
                ctx.doca_txq = nullptr;
                ctx.doca_gpu_txq = nullptr;
                ctx.tx_ctx_started = false;
            }
        }

        logger::info("DOCA GPUNetIO transport initialized on NIC {} using GPU {}.", nic_pci, gpu_bus_id);

        return true;
    }

    void shutdown_transport(transport_context& ctx) noexcept {
        if (ctx.doca_flow_port) {
            doca_flow_port_stop(static_cast<doca_flow_port*>(ctx.doca_flow_port));
            doca_flow_destroy();
            ctx.doca_flow_pipe = nullptr;
            ctx.doca_flow_port = nullptr;
        }

        if (ctx.tx_ctx_started && ctx.doca_txq != nullptr) {
            auto* tx_ctx = doca_eth_txq_as_doca_ctx(static_cast<doca_eth_txq*>(ctx.doca_txq));
            if (tx_ctx != nullptr) {
                (void) doca_ctx_stop(tx_ctx);
            }
        }
        if (ctx.rx_ctx_started && ctx.doca_rxq != nullptr) {
            auto* rx_ctx = doca_eth_rxq_as_doca_ctx(static_cast<doca_eth_rxq*>(ctx.doca_rxq));
            if (rx_ctx != nullptr) {
                (void) doca_ctx_stop(rx_ctx);
            }
        }

        if (ctx.doca_txq != nullptr) {
            (void) doca_eth_txq_destroy(static_cast<doca_eth_txq*>(ctx.doca_txq));
        }
        if (ctx.doca_rxq != nullptr) {
            (void) doca_eth_rxq_destroy(static_cast<doca_eth_rxq*>(ctx.doca_rxq));
        }

        if (ctx.doca_mmap != nullptr) {
            (void) doca_mmap_stop(static_cast<doca_mmap*>(ctx.doca_mmap));
            (void) doca_mmap_destroy(static_cast<doca_mmap*>(ctx.doca_mmap));
        }

        if (ctx.rx_pkt_buf_gpu != nullptr && ctx.doca_gpu != nullptr) {
            (void) doca_gpu_mem_free(static_cast<doca_gpu*>(ctx.doca_gpu), ctx.rx_pkt_buf_gpu);
        }

        if (ctx.doca_gpu != nullptr) {
            (void) doca_gpu_destroy(static_cast<doca_gpu*>(ctx.doca_gpu));
        }
        if (ctx.doca_dev != nullptr) {
            (void) doca_dev_close(static_cast<doca_dev*>(ctx.doca_dev));
        }

        ctx.doca_gpu_txq = nullptr;
        ctx.doca_gpu_rxq = nullptr;
        ctx.doca_txq = nullptr;
        ctx.doca_rxq = nullptr;
        ctx.doca_mmap = nullptr;
        ctx.rx_pkt_buf_gpu = nullptr;
        ctx.rx_pkt_buf_cpu = nullptr;
        ctx.rx_pkt_buf_size = 0;
        ctx.doca_gpu = nullptr;
        ctx.doca_dev = nullptr;
        ctx.rx_ctx_started = false;
        ctx.tx_ctx_started = false;
        ctx.gpu_workers_running = false;
        ctx.rx_ready = false;
        ctx.tx_ready = false;
    }
}

