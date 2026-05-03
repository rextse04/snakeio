#include "doca_gpunetio_runtime.hpp"
#include "doca_gpunetio_stage.hpp"
#include "doca_tx_frame.hpp"

#include <config.hpp>
#include <logger.hpp>
#include <packet.hpp>

#include <cstddef>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_gpu_data_path.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_gpu_data_path.h>
#include <doca_flow.h>
#include <doca_gpunetio.h>
#include <doca_mmap.h>

#include <cuda_runtime.h>

#include <arpa/inet.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

extern "C" cudaError_t snakeio_doca_gpunetio_tx_send_one_launch(cudaStream_t stream,
    struct doca_gpu_eth_txq* txq_gpu,
    uint8_t* pkt_gpu,
    uint32_t mkey_be,
    uint32_t frame_len);

namespace {
    constexpr uint32_t max_pkt_num = 8192;
    constexpr uint32_t max_pkt_size = 2048;

    struct flow_state {
        struct doca_flow_port* port = nullptr;
    };

    struct rxq_state {
        struct doca_gpu* gpu = nullptr;
        struct doca_dev* nic = nullptr;

        struct doca_ctx* eth_rxq_ctx = nullptr;
        struct doca_eth_rxq* eth_rxq = nullptr;
        struct doca_gpu_eth_rxq* eth_rxq_gpu = nullptr;

        struct doca_mmap* pkt_mmap = nullptr;
        void* gpu_pkt_addr = nullptr;
        int dmabuf_fd = -1;

        struct doca_flow_pipe* rxq_pipe = nullptr;
        struct doca_flow_pipe* rxq_pipe_ip6 = nullptr;
        struct doca_flow_pipe* root_pipe = nullptr;
        struct doca_flow_pipe_entry* root_udp_entry = nullptr;
        struct doca_flow_pipe_entry* root_udp_ip6_entry = nullptr;
    };

    struct txq_state {
        struct doca_ctx* eth_txq_ctx = nullptr;
        struct doca_eth_txq* eth_txq = nullptr;
        struct doca_gpu_eth_txq* eth_txq_gpu = nullptr;
        struct doca_mmap* pkt_mmap = nullptr;
        uint8_t* pkt_gpu = nullptr;
        uint32_t mkey_be32 = 0;
        int dmabuf_fd = -1;
        uint8_t nic_mac[6]{};
        uint8_t local_ip4[4]{};
        uint8_t local_ip6[16]{};
        bool has_local_ip6 = false;
        uint8_t gateway_mac[6]{};
        bool has_gateway_mac = false;
        bool ready = false;
    };

    constexpr uint32_t k_tx_max_sq_descr = 8192;
    constexpr uint32_t k_tx_pkt_buf_bytes = 2048;
    constexpr uint16_t k_tx_queue_id = 0;

    bool g_inited = false;
    bool g_started = false;

    cudaStream_t g_stream{};
    snakeio_doca_stage_cpu* g_stage_cpu = nullptr;
    void* g_stage_gpu = nullptr;

    flow_state g_flow{};
    rxq_state g_rx{};
    txq_state g_tx{};

    std::size_t doca_host_page_size() noexcept {
        const long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) {
            return 4096;
        }
        return static_cast<std::size_t>(ps);
    }

    void align_up(uint32_t& v, std::size_t align) noexcept {
        v = static_cast<uint32_t>((static_cast<std::size_t>(v) + align - 1) / align * align);
    }

    const char* env_cstr(const char* key, const char* defaultv) noexcept {
        const char* v = std::getenv(key);
        return v ? v : defaultv;
    }

    doca_error_t open_nic_by_pci(const char* pci, struct doca_dev** out) noexcept {
        struct doca_devinfo** list = nullptr;
        uint32_t nb = 0;
        doca_error_t res = doca_devinfo_create_list(&list, &nb);
        if (res != DOCA_SUCCESS) {
            return res;
        }

        res = DOCA_ERROR_NOT_FOUND;
        for (uint32_t i = 0; i < nb; i++) {
            uint8_t eq = 0;
            if (doca_devinfo_is_equal_pci_addr(list[i], pci, &eq) != DOCA_SUCCESS || !eq) {
                continue;
            }
            if (doca_dev_open(list[i], out) == DOCA_SUCCESS) {
                res = DOCA_SUCCESS;
                break;
            }
        }

        doca_devinfo_destroy_list(list);
        return res;
    }

    doca_error_t open_first_nic(struct doca_dev** out) noexcept {
        struct doca_devinfo** list = nullptr;
        uint32_t nb = 0;
        doca_error_t res = doca_devinfo_create_list(&list, &nb);
        if (res != DOCA_SUCCESS) {
            return res;
        }

        res = DOCA_ERROR_NOT_FOUND;
        for (uint32_t i = 0; i < nb; i++) {
            if (doca_dev_open(list[i], out) == DOCA_SUCCESS) {
                res = DOCA_SUCCESS;
                break;
            }
        }

        doca_devinfo_destroy_list(list);
        return res;
    }

    doca_error_t init_doca_flow() noexcept {
        struct doca_flow_cfg* cfg = nullptr;
        doca_error_t res = doca_flow_cfg_create(&cfg);
        if (res != DOCA_SUCCESS) {
            return res;
        }

        res = doca_flow_cfg_set_pipe_queues(cfg, 1);
        if (res != DOCA_SUCCESS) {
            doca_flow_cfg_destroy(cfg);
            return res;
        }

        // DOCA Flow: "vnf" matches GPUNetIO samples (BlueField-oriented); host PCIe often needs "switch".
        const char* flow_mode = env_cstr("SNAKEIO_DOCA_FLOW_MODE", "vnf");
        res = doca_flow_cfg_set_mode_args(cfg, flow_mode);
        if (res != DOCA_SUCCESS) {
            doca_flow_cfg_destroy(cfg);
            return res;
        }

        // Large enough for basic counters; mirrors NVIDIA GPUNetIO samples.
        res = doca_flow_cfg_set_nr_counters(cfg, 1024u * 512u);
        if (res != DOCA_SUCCESS) {
            doca_flow_cfg_destroy(cfg);
            return res;
        }

        res = doca_flow_init(cfg);
        doca_flow_cfg_destroy(cfg);
        return res;
    }

    doca_error_t start_doca_flow_port(struct doca_dev* nic_dev) noexcept {
        struct doca_flow_port_cfg* port_cfg = nullptr;
        doca_error_t res = doca_flow_port_cfg_create(&port_cfg);
        if (res != DOCA_SUCCESS) {
            return res;
        }

        uint16_t flow_port_id = 0;
        if (const char* ps = env_cstr("SNAKEIO_DOCA_FLOW_PORT_ID", nullptr)) {
            const int parsed = std::atoi(ps);
            if (parsed >= 0 && parsed <= 65535) {
                flow_port_id = static_cast<uint16_t>(parsed);
            }
        }
        res = doca_flow_port_cfg_set_port_id(port_cfg, flow_port_id);
        if (res != DOCA_SUCCESS) {
            doca_flow_port_cfg_destroy(port_cfg);
            return res;
        }

        // Default action memory is 0; DOCA Flow requires a power-of-two ≥ 64 B for reliable port_start (see flow_common.c).
        res = doca_flow_port_cfg_set_actions_mem_size(port_cfg, 64u * 1024u);
        if (res != DOCA_SUCCESS) {
            doca_flow_port_cfg_destroy(port_cfg);
            return res;
        }

        res = doca_flow_port_cfg_set_dev(port_cfg, nic_dev);
        if (res != DOCA_SUCCESS) {
            doca_flow_port_cfg_destroy(port_cfg);
            return res;
        }

        if (const char* devargs = env_cstr("SNAKEIO_DOCA_FLOW_PORT_DEVARGS", nullptr)) {
            res = doca_flow_port_cfg_set_devargs(port_cfg, devargs);
            if (res != DOCA_SUCCESS) {
                doca_flow_port_cfg_destroy(port_cfg);
                return res;
            }
        }

        res = doca_flow_port_start(port_cfg, &g_flow.port);
        doca_flow_port_cfg_destroy(port_cfg);
        return res;
    }

    doca_error_t create_udp_meta_rss_pipe(bool for_ipv6, const char* pipe_name, struct doca_flow_pipe** out_pipe) noexcept {
        doca_error_t result;
        struct doca_flow_match match{};
        struct doca_flow_fwd fwd{};
        struct doca_flow_fwd miss_fwd{};
        struct doca_flow_pipe_cfg* pipe_cfg = nullptr;
        uint16_t rss_queues[1];
        struct doca_flow_monitor monitor{};

        monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

        if (for_ipv6) {
            match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
            match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
        } else {
            match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
            match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
        }

        rss_queues[0] = 0;

        fwd.type = DOCA_FLOW_FWD_RSS;
        fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
        fwd.rss.queues_array = rss_queues;
        fwd.rss.outer_flags =
            for_ipv6 ? (DOCA_FLOW_RSS_IPV6 | DOCA_FLOW_RSS_UDP) : (DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP);
        fwd.rss.nr_queues = 1;

        miss_fwd.type = DOCA_FLOW_FWD_DROP;

        result = doca_flow_pipe_cfg_create(&pipe_cfg, g_flow.port);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }

        result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, out_pipe);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        doca_flow_pipe_cfg_destroy(pipe_cfg);

        result = doca_flow_pipe_basic_add_entry(0,
            *out_pipe,
            &match,
            0,
            nullptr,
            nullptr,
            nullptr,
            DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
            nullptr,
            nullptr);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_flow_entries_process(g_flow.port, 0, 0, 0);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        return DOCA_SUCCESS;

    destroy_pipe_cfg:
        doca_flow_pipe_cfg_destroy(pipe_cfg);
        return result;
    }

    doca_error_t create_root_udp_pipe() noexcept {
        doca_error_t result;
        struct doca_flow_monitor monitor{};
        struct doca_flow_match udp_match{};
        struct doca_flow_fwd udp_fwd{};
        struct doca_flow_match udp6_match{};
        struct doca_flow_fwd udp6_fwd{};

        struct doca_flow_pipe_cfg* pipe_cfg = nullptr;
        const char* pipe_name = "SNAKEIO_ROOT_PIPE";

        monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

        udp_match.outer.eth.type = htons(static_cast<uint16_t>(0x0800));
        udp_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
        udp_match.outer.ip4.next_proto = IPPROTO_UDP;

        udp_fwd.type = DOCA_FLOW_FWD_PIPE;
        udp_fwd.next_pipe = g_rx.rxq_pipe;

        result = doca_flow_pipe_cfg_create(&pipe_cfg, g_flow.port);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }

        result = doca_flow_pipe_create(pipe_cfg, nullptr, nullptr, &g_rx.root_pipe);
        if (result != DOCA_SUCCESS) {
            goto destroy_pipe_cfg;
        }
        doca_flow_pipe_cfg_destroy(pipe_cfg);

        result = doca_flow_pipe_control_add_entry(0,
            g_rx.root_pipe,
            &udp_match,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            0,
            &udp_fwd,
            nullptr,
            &g_rx.root_udp_entry);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        udp6_match.outer.eth.type = htons(static_cast<uint16_t>(0x86dd));
        udp6_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6;
        udp6_match.outer.ip6.next_proto = IPPROTO_UDP;

        udp6_fwd.type = DOCA_FLOW_FWD_PIPE;
        udp6_fwd.next_pipe = g_rx.rxq_pipe_ip6;

        result = doca_flow_pipe_control_add_entry(0,
            g_rx.root_pipe,
            &udp6_match,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            0,
            &udp6_fwd,
            nullptr,
            &g_rx.root_udp_ip6_entry);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_flow_entries_process(g_flow.port, 0, 0, 0);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        return DOCA_SUCCESS;

    destroy_pipe_cfg:
        doca_flow_pipe_cfg_destroy(pipe_cfg);
        return result;
    }

    bool parse_colon_mac(const char* s, uint8_t mac[6]) noexcept {
        if (s == nullptr) {
            return false;
        }
        const int n = std::sscanf(s,
            "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
            &mac[0],
            &mac[1],
            &mac[2],
            &mac[3],
            &mac[4],
            &mac[5]);
        return n == 6;
    }

    void destroy_txq() noexcept {
        g_tx.ready = false;
        g_tx.mkey_be32 = 0;
        g_tx.dmabuf_fd = -1;

        if (g_tx.eth_txq_ctx != nullptr) {
            doca_error_t r = doca_ctx_stop(g_tx.eth_txq_ctx);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_ctx_stop(eth_txq) failed: {}.", doca_error_get_name(r));
            }
            g_tx.eth_txq_ctx = nullptr;
        }

        if (g_tx.pkt_gpu != nullptr) {
            if (g_rx.gpu != nullptr) {
                doca_error_t r = doca_gpu_mem_free(g_rx.gpu, g_tx.pkt_gpu);
                if (r != DOCA_SUCCESS) {
                    snakeio::logger::warn("doca_gpu_mem_free(tx pkt) failed: {}.", doca_error_get_name(r));
                }
            }
            g_tx.pkt_gpu = nullptr;
        }

        if (g_tx.eth_txq != nullptr) {
            doca_error_t r = doca_eth_txq_destroy(g_tx.eth_txq);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_eth_txq_destroy failed: {}.", doca_error_get_name(r));
            }
            g_tx.eth_txq = nullptr;
            g_tx.eth_txq_gpu = nullptr;
        }

        if (g_tx.pkt_mmap != nullptr) {
            doca_error_t r = doca_mmap_destroy(g_tx.pkt_mmap);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_mmap_destroy(tx) failed: {}.", doca_error_get_name(r));
            }
            g_tx.pkt_mmap = nullptr;
        }

        std::memset(g_tx.nic_mac, 0, sizeof(g_tx.nic_mac));
        std::memset(g_tx.local_ip4, 0, sizeof(g_tx.local_ip4));
        std::memset(g_tx.local_ip6, 0, sizeof(g_tx.local_ip6));
        g_tx.has_local_ip6 = false;
        std::memset(g_tx.gateway_mac, 0, sizeof(g_tx.gateway_mac));
        g_tx.has_gateway_mac = false;
    }

    doca_error_t create_txq() noexcept {
        doca_error_t result;
        uint32_t buffer_size = k_tx_pkt_buf_bytes;
        uint32_t mkey_host = 0;

        struct doca_devinfo* inf = doca_dev_as_devinfo(g_rx.nic);
        if (inf == nullptr) {
            return DOCA_ERROR_BAD_STATE;
        }

        result = doca_devinfo_get_mac_addr(inf, g_tx.nic_mac, DOCA_DEVINFO_MAC_ADDR_SIZE);
        if (result != DOCA_SUCCESS) {
            const char* em = env_cstr("SNAKEIO_DOCA_LOCAL_MAC", nullptr);
            if (em == nullptr || !parse_colon_mac(em, g_tx.nic_mac)) {
                snakeio::logger::error("DOCA TX: MAC unavailable ({}); set SNAKEIO_DOCA_LOCAL_MAC=aa:bb:...",
                    doca_error_get_descr(result));
                return result;
            }
        }

        result = doca_devinfo_get_ipv4_addr(inf, g_tx.local_ip4, sizeof(g_tx.local_ip4));
        if (result != DOCA_SUCCESS) {
            const char* es = env_cstr("SNAKEIO_DOCA_LOCAL_IP4", nullptr);
            in_addr addr{};
            if (es == nullptr || inet_pton(AF_INET, es, &addr) != 1) {
                snakeio::logger::error("DOCA TX: IPv4 unavailable ({}); set SNAKEIO_DOCA_LOCAL_IP4.",
                    doca_error_get_descr(result));
                return DOCA_ERROR_NOT_FOUND;
            }
            std::memcpy(g_tx.local_ip4, &addr.s_addr, sizeof(g_tx.local_ip4));
        }

        {
            const char* gw = env_cstr("SNAKEIO_DOCA_GATEWAY_MAC", nullptr);
            if (gw != nullptr && parse_colon_mac(gw, g_tx.gateway_mac)) {
                g_tx.has_gateway_mac = true;
            }
        }

        {
            uint8_t ip6_tmp[DOCA_DEVINFO_IPV6_ADDR_SIZE]{};
            doca_error_t r6 = doca_devinfo_get_ipv6_addr(inf, ip6_tmp, sizeof(ip6_tmp));
            if (r6 == DOCA_SUCCESS) {
                std::memcpy(g_tx.local_ip6, ip6_tmp, sizeof(g_tx.local_ip6));
                g_tx.has_local_ip6 = true;
            } else {
                const char* e6 = env_cstr("SNAKEIO_DOCA_LOCAL_IP6", nullptr);
                if (e6 != nullptr && inet_pton(AF_INET6, e6, g_tx.local_ip6) == 1) {
                    g_tx.has_local_ip6 = true;
                }
            }
        }

        result = doca_eth_txq_create(g_rx.nic, k_tx_max_sq_descr, &g_tx.eth_txq);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_eth_txq_set_l3_chksum_offload(g_tx.eth_txq, 1);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_eth_txq_set_l4_chksum_offload(g_tx.eth_txq, 1);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_eth_txq_gpu_set_completion_on_gpu(g_tx.eth_txq);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        g_tx.eth_txq_ctx = doca_eth_txq_as_doca_ctx(g_tx.eth_txq);
        if (g_tx.eth_txq_ctx == nullptr) {
            result = DOCA_ERROR_BAD_STATE;
            goto tx_fail;
        }

        result = doca_ctx_set_datapath_on_gpu(g_tx.eth_txq_ctx, g_rx.gpu);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_ctx_start(g_tx.eth_txq_ctx);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_eth_txq_apply_queue_id(g_tx.eth_txq, k_tx_queue_id);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_eth_txq_get_gpu_handle(g_tx.eth_txq, &g_tx.eth_txq_gpu);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_mmap_create(&g_tx.pkt_mmap);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_mmap_add_dev(g_tx.pkt_mmap, g_rx.nic);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        align_up(buffer_size, doca_host_page_size());

        result = doca_gpu_mem_alloc(g_rx.gpu,
            buffer_size,
            doca_host_page_size(),
            DOCA_GPU_MEM_TYPE_GPU,
            reinterpret_cast<void**>(&g_tx.pkt_gpu),
            nullptr);
        if (result != DOCA_SUCCESS || g_tx.pkt_gpu == nullptr) {
            result = result == DOCA_SUCCESS ? DOCA_ERROR_NO_MEMORY : result;
            goto tx_fail;
        }

        result = doca_gpu_dmabuf_fd(g_rx.gpu, g_tx.pkt_gpu, buffer_size, &g_tx.dmabuf_fd);
        if (result != DOCA_SUCCESS) {
            result = doca_mmap_set_memrange(g_tx.pkt_mmap, g_tx.pkt_gpu, buffer_size);
            if (result != DOCA_SUCCESS) {
                goto tx_fail;
            }
        } else {
            result = doca_mmap_set_dmabuf_memrange(g_tx.pkt_mmap,
                g_tx.dmabuf_fd,
                g_tx.pkt_gpu,
                0,
                buffer_size);
            if (result != DOCA_SUCCESS) {
                goto tx_fail;
            }
        }

        result = doca_mmap_set_permissions(g_tx.pkt_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_mmap_start(g_tx.pkt_mmap);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }

        result = doca_mmap_get_mkey(g_tx.pkt_mmap, g_rx.nic, &mkey_host);
        if (result != DOCA_SUCCESS) {
            goto tx_fail;
        }
        g_tx.mkey_be32 = htobe32(mkey_host);

        g_tx.ready = true;
        return DOCA_SUCCESS;

    tx_fail:
        destroy_txq();
        return result;
    }

    doca_error_t create_rxq(struct doca_gpu* gpu, struct doca_dev* nic) noexcept {
        doca_error_t result;
        uint32_t cyclic_buffer_size = 0;

        g_rx.gpu = gpu;
        g_rx.nic = nic;

        result = doca_eth_rxq_create(g_rx.nic, max_pkt_num, max_pkt_size, &g_rx.eth_rxq);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_eth_rxq_set_type(g_rx.eth_rxq, DOCA_ETH_RXQ_TYPE_CYCLIC);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_CYCLIC,
            0,
            0,
            max_pkt_size,
            max_pkt_num,
            0,
            0,
            0,
            &cyclic_buffer_size);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_mmap_create(&g_rx.pkt_mmap);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_mmap_add_dev(g_rx.pkt_mmap, g_rx.nic);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        align_up(cyclic_buffer_size, doca_host_page_size());

        result = doca_gpu_mem_alloc(g_rx.gpu,
            cyclic_buffer_size,
            doca_host_page_size(),
            DOCA_GPU_MEM_TYPE_GPU,
            &g_rx.gpu_pkt_addr,
            nullptr);
        if (result != DOCA_SUCCESS || g_rx.gpu_pkt_addr == nullptr) {
            return result == DOCA_SUCCESS ? DOCA_ERROR_NO_MEMORY : result;
        }

        result = doca_gpu_dmabuf_fd(g_rx.gpu, g_rx.gpu_pkt_addr, cyclic_buffer_size, &g_rx.dmabuf_fd);
        if (result != DOCA_SUCCESS) {
            result = doca_mmap_set_memrange(g_rx.pkt_mmap, g_rx.gpu_pkt_addr, cyclic_buffer_size);
            if (result != DOCA_SUCCESS) {
                return result;
            }
        } else {
            result = doca_mmap_set_dmabuf_memrange(g_rx.pkt_mmap,
                g_rx.dmabuf_fd,
                g_rx.gpu_pkt_addr,
                0,
                cyclic_buffer_size);
            if (result != DOCA_SUCCESS) {
                return result;
            }
        }

        result = doca_mmap_set_permissions(g_rx.pkt_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_mmap_start(g_rx.pkt_mmap);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_eth_rxq_set_pkt_buf(g_rx.eth_rxq, g_rx.pkt_mmap, 0, cyclic_buffer_size);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        int cuda_device = 0;
        cudaGetDevice(&cuda_device);
        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, cuda_device);
        if (prop.major < 9) {
            result = doca_eth_rxq_gpu_enable_mcst_qp(g_rx.eth_rxq);
            if (result != DOCA_SUCCESS) {
                return result;
            }
        }

        g_rx.eth_rxq_ctx = doca_eth_rxq_as_doca_ctx(g_rx.eth_rxq);
        if (g_rx.eth_rxq_ctx == nullptr) {
            return DOCA_ERROR_BAD_STATE;
        }

        result = doca_ctx_set_datapath_on_gpu(g_rx.eth_rxq_ctx, g_rx.gpu);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_ctx_start(g_rx.eth_rxq_ctx);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_eth_rxq_get_gpu_handle(g_rx.eth_rxq, &g_rx.eth_rxq_gpu);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = doca_eth_rxq_apply_queue_id(g_rx.eth_rxq, 0);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = create_udp_meta_rss_pipe(false, "SNAKEIO_GPU_RXQ_UDP_PIPE", &g_rx.rxq_pipe);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = create_udp_meta_rss_pipe(true, "SNAKEIO_GPU_RXQ_UDP6_PIPE", &g_rx.rxq_pipe_ip6);
        if (result != DOCA_SUCCESS) {
            return result;
        }

        result = create_root_udp_pipe();
        if (result != DOCA_SUCCESS) {
            return result;
        }

        return DOCA_SUCCESS;
    }

    void destroy_rxq() noexcept {
        if (g_rx.root_pipe != nullptr) {
            doca_flow_pipe_destroy(g_rx.root_pipe);
            g_rx.root_pipe = nullptr;
        }
        if (g_rx.rxq_pipe_ip6 != nullptr) {
            doca_flow_pipe_destroy(g_rx.rxq_pipe_ip6);
            g_rx.rxq_pipe_ip6 = nullptr;
        }
        if (g_rx.rxq_pipe != nullptr) {
            doca_flow_pipe_destroy(g_rx.rxq_pipe);
            g_rx.rxq_pipe = nullptr;
        }

        if (g_flow.port != nullptr) {
            doca_error_t r = doca_flow_port_stop(g_flow.port);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_flow_port_stop failed: {}.", doca_error_get_name(r));
            }
            g_flow.port = nullptr;
        }

        if (g_rx.eth_rxq_ctx != nullptr) {
            doca_error_t r = doca_ctx_stop(g_rx.eth_rxq_ctx);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_ctx_stop(eth_rxq) failed: {}.", doca_error_get_name(r));
            }
            g_rx.eth_rxq_ctx = nullptr;
        }

        if (g_rx.gpu_pkt_addr != nullptr) {
            doca_error_t r = doca_gpu_mem_free(g_rx.gpu, g_rx.gpu_pkt_addr);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_gpu_mem_free failed: {}.", doca_error_get_name(r));
            }
            g_rx.gpu_pkt_addr = nullptr;
        }

        if (g_rx.eth_rxq != nullptr) {
            doca_error_t r = doca_eth_rxq_destroy(g_rx.eth_rxq);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_eth_rxq_destroy failed: {}.", doca_error_get_name(r));
            }
            g_rx.eth_rxq = nullptr;
        }

        if (g_rx.pkt_mmap != nullptr) {
            doca_error_t r = doca_mmap_destroy(g_rx.pkt_mmap);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_mmap_destroy failed: {}.", doca_error_get_name(r));
            }
            g_rx.pkt_mmap = nullptr;
        }

        if (g_rx.nic != nullptr) {
            doca_error_t r = doca_dev_close(g_rx.nic);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_dev_close(nic) failed: {}.", doca_error_get_name(r));
            }
            g_rx.nic = nullptr;
        }

        if (g_rx.gpu != nullptr) {
            doca_error_t r = doca_gpu_destroy(g_rx.gpu);
            if (r != DOCA_SUCCESS) {
                snakeio::logger::warn("doca_gpu_destroy failed: {}.", doca_error_get_name(r));
            }
            g_rx.gpu = nullptr;
        }

        g_rx.eth_rxq_gpu = nullptr;
        g_rx.dmabuf_fd = -1;
        g_rx.root_udp_entry = nullptr;
        g_rx.root_udp_ip6_entry = nullptr;
    }

    void destroy_flow_framework() noexcept {
        doca_flow_destroy();
    }
} // namespace

namespace snakeio::doca_gpunetio_runtime {
    doca_error_t init() noexcept {
        if (g_inited) {
            return DOCA_SUCCESS;
        }

        cudaError_t cr = cudaFree(0);
        if (cr != cudaSuccess) {
            snakeio::logger::warn("cudaFree(0) failed during DOCA init: {}.", cudaGetErrorString(cr));
        }

        doca_error_t res = init_doca_flow();
        if (res != DOCA_SUCCESS) {
            snakeio::logger::error("doca_flow_init failed: {}.", doca_error_get_descr(res));
            return res;
        }

        const char* nic_pci = env_cstr("SNAKEIO_DOCA_NIC_PCI", nullptr);
        if (nic_pci != nullptr) {
            res = open_nic_by_pci(nic_pci, &g_rx.nic);
        } else {
            res = open_first_nic(&g_rx.nic);
        }
        if (res != DOCA_SUCCESS) {
            snakeio::logger::error("Failed to open DOCA NIC device: {}.", doca_error_get_descr(res));
            destroy_flow_framework();
            return res;
        }

        res = start_doca_flow_port(g_rx.nic);
        if (res != DOCA_SUCCESS) {
            snakeio::logger::error("doca_flow_port_start failed: {} ({}).",
                doca_error_get_descr(res),
                doca_error_get_name(res));
            doca_dev_close(g_rx.nic);
            g_rx.nic = nullptr;
            destroy_flow_framework();
            return res;
        }

        const char* gpu_pci = env_cstr("SNAKEIO_DOCA_GPU_PCI", "0000:ab:00.0");
        res = doca_gpu_create(gpu_pci, &g_rx.gpu);
        if (res != DOCA_SUCCESS) {
            snakeio::logger::error("doca_gpu_create failed: {}.", doca_error_get_descr(res));
            doca_flow_port_stop(g_flow.port);
            g_flow.port = nullptr;
            doca_dev_close(g_rx.nic);
            g_rx.nic = nullptr;
            destroy_flow_framework();
            return res;
        }

        res = create_rxq(g_rx.gpu, g_rx.nic);
        if (res != DOCA_SUCCESS) {
            snakeio::logger::error("DOCA Eth RXQ setup failed: {}.", doca_error_get_descr(res));
            destroy_rxq();
            destroy_flow_framework();
            return res;
        }

        res = create_txq();
        if (res != DOCA_SUCCESS) {
            snakeio::logger::error("DOCA Eth TXQ setup failed: {}.", doca_error_get_descr(res));
            destroy_rxq();
            destroy_flow_framework();
            return res;
        }

        const cudaError_t cs = cudaStreamCreateWithFlags(&g_stream, cudaStreamNonBlocking);
        if (cs != cudaSuccess) {
            snakeio::logger::error("cudaStreamCreateWithFlags failed: {}.", cudaGetErrorString(cs));
            destroy_txq();
            destroy_rxq();
            destroy_flow_framework();
            return DOCA_ERROR_DRIVER;
        }

        res = doca_gpu_mem_alloc(g_rx.gpu,
            sizeof(snakeio_doca_stage_cpu),
            doca_host_page_size(),
            DOCA_GPU_MEM_TYPE_GPU_CPU,
            &g_stage_gpu,
            reinterpret_cast<void**>(&g_stage_cpu));
        if (res != DOCA_SUCCESS || g_stage_cpu == nullptr) {
            snakeio::logger::error("doca_gpu_mem_alloc(stage) failed: {}.", doca_error_get_descr(res));
            cudaStreamDestroy(g_stream);
            g_stream = {};
            destroy_txq();
            destroy_rxq();
            destroy_flow_framework();
            return res == DOCA_SUCCESS ? DOCA_ERROR_NO_MEMORY : res;
        }

        g_stage_cpu->stop = 0;
        g_stage_cpu->cpu_sem = DOCA_GPU_SEMAPHORE_STATUS_FREE;
        g_stage_cpu->gpu_sem = DOCA_GPU_SEMAPHORE_STATUS_FREE;
        g_stage_cpu->payload_len = 0;
        g_stage_cpu->src_port_be = 0;
        g_stage_cpu->src_is_ipv6 = 0;
        for (int i = 0; i < 6; i++) {
            g_stage_cpu->src_eth[i] = 0;
        }
        g_stage_cpu->src_ip4_be = 0;
        g_stage_cpu->src_ip6_be[0] = 0;
        g_stage_cpu->src_ip6_be[1] = 0;
        g_stage_cpu->src_ip6_be[2] = 0;
        g_stage_cpu->src_ip6_be[3] = 0;

        g_inited = true;
        return DOCA_SUCCESS;
    }

    void shutdown() noexcept {
        if (!g_inited) {
            return;
        }

        stop_recv(&g_stream);

        if (g_stage_gpu != nullptr) {
            doca_gpu_mem_free(g_rx.gpu, g_stage_gpu);
            g_stage_gpu = nullptr;
            g_stage_cpu = nullptr;
        }

        if (g_stream != nullptr) {
            cudaStreamDestroy(g_stream);
            g_stream = nullptr;
        }

        destroy_txq();
        destroy_rxq();
        destroy_flow_framework();

        g_inited = false;
        g_started = false;
    }

    bool started() noexcept {
        return g_started;
    }

    doca_error_t start_recv(void* cuda_stream) noexcept {
        if (!g_inited) {
            return DOCA_ERROR_BAD_STATE;
        }
        if (g_started) {
            return DOCA_SUCCESS;
        }

        cudaStream_t stream = cuda_stream == nullptr ? g_stream : static_cast<cudaStream_t>(cuda_stream);

        const cudaError_t lr = snakeio_doca_gpunetio_recv_launch(stream, g_rx.eth_rxq_gpu, g_stage_cpu);
        if (lr != cudaSuccess) {
            snakeio::logger::error("snakeio_doca_gpunetio_recv_launch failed: {}.", cudaGetErrorString(lr));
            return DOCA_ERROR_DRIVER;
        }

        g_started = true;
        return DOCA_SUCCESS;
    }

    void stop_recv(void* cuda_stream) noexcept {
        if (!g_inited || !g_started) {
            return;
        }

        cudaStream_t stream = cuda_stream == nullptr ? g_stream : static_cast<cudaStream_t>(cuda_stream);

        if (g_stage_cpu != nullptr) {
            g_stage_cpu->stop = 1;
        }

        cudaError_t sync = cudaStreamSynchronize(stream);
        if (sync != cudaSuccess) {
            snakeio::logger::warn("cudaStreamSynchronize(recv) failed: {}.", cudaGetErrorString(sync));
        }

        g_started = false;
    }

    static void fill_sockaddr_from_stage(const snakeio_doca_stage_cpu& st, sockaddr_storage& out) noexcept {
        std::memset(&out, 0, sizeof(out));
        auto& a = reinterpret_cast<sockaddr_in6&>(out);
        a.sin6_family = AF_INET6;

        if (st.src_is_ipv6) {
            for (int i = 0; i < 4; i++) {
                a.sin6_addr.s6_addr32[i] = st.src_ip6_be[i];
            }
        } else {
            a.sin6_addr.s6_addr32[2] = htonl(0x0000ffff);
            a.sin6_addr.s6_addr32[3] = st.src_ip4_be;
        }

        a.sin6_port = st.src_port_be;
    }

    bool pop_udp_payload(std::stop_token stop_token,
        std::span<std::byte> out_payload,
        snakeio::size_t& out_payload_len,
        sockaddr_storage& out_src_addr,
        std::array<std::byte, 6>& out_src_eth) noexcept {
        if (!g_inited || !g_started || g_stage_cpu == nullptr) {
            out_payload_len = 0;
            return false;
        }

        while (!stop_token.stop_requested()) {
            doca_eth_rxq_gpu_cpu_proxy_progress(g_rx.eth_rxq);

            if (g_stage_cpu->gpu_sem == DOCA_GPU_SEMAPHORE_STATUS_ERROR) {
                snakeio::logger::warn("GPUNetIO receive kernel reported an error; stopping data port.");
                out_payload_len = 0;
                return false;
            }

            if (g_stage_cpu->gpu_sem != DOCA_GPU_SEMAPHORE_STATUS_READY) {
                std::this_thread::yield();
                continue;
            }

            const uint32_t n = g_stage_cpu->payload_len;
            fill_sockaddr_from_stage(*g_stage_cpu, out_src_addr);

            if (n != 0) {
                for (int i = 0; i < 6; i++) {
                    out_src_eth[static_cast<std::size_t>(i)] = static_cast<std::byte>(g_stage_cpu->src_eth[i]);
                }
                if (n > out_payload.size()) {
                    snakeio::logger::warn("Dropping oversize UDP payload ({} bytes > buffer {}).", n, out_payload.size());
                    out_payload_len = 0;
                } else {
                    std::memcpy(out_payload.data(), const_cast<const std::byte*>(g_stage_cpu->payload), n);
                    out_payload_len = static_cast<snakeio::size_t>(n);
                }
            }

            g_stage_cpu->cpu_sem = DOCA_GPU_SEMAPHORE_STATUS_DONE;
            while (g_stage_cpu->gpu_sem == DOCA_GPU_SEMAPHORE_STATUS_READY && !stop_token.stop_requested()) {
                doca_eth_rxq_gpu_cpu_proxy_progress(g_rx.eth_rxq);
                std::this_thread::yield();
            }

            while (g_stage_cpu->cpu_sem != DOCA_GPU_SEMAPHORE_STATUS_FREE && !stop_token.stop_requested()) {
                doca_eth_rxq_gpu_cpu_proxy_progress(g_rx.eth_rxq);
                std::this_thread::yield();
            }

            // Only surface non-empty payloads to the game loop.
            if (n != 0) {
                return true;
            }
        }

        out_payload_len = 0;
        return false;
    }

    static bool extract_ipv4_udp_dest(const sockaddr_storage& dst,
        uint8_t ip4_out[4],
        uint16_t* udp_dst_port_be) noexcept {
        if (dst.ss_family == AF_INET) {
            const auto* s = reinterpret_cast<const sockaddr_in*>(&dst);
            std::memcpy(ip4_out, &s->sin_addr.s_addr, sizeof(s->sin_addr.s_addr));
            *udp_dst_port_be = s->sin_port;
            return true;
        }
        if (dst.ss_family == AF_INET6) {
            const auto* s6 = reinterpret_cast<const sockaddr_in6*>(&dst);
            if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
                std::memcpy(ip4_out, &s6->sin6_addr.s6_addr[12], 4);
                *udp_dst_port_be = s6->sin6_port;
                return true;
            }
        }
        return false;
    }

    static bool extract_ipv6_udp_dest(const sockaddr_storage& dst,
        uint8_t ip6_out[16],
        uint16_t* udp_dst_port_be) noexcept {
        if (dst.ss_family != AF_INET6) {
            return false;
        }
        const auto* s6 = reinterpret_cast<const sockaddr_in6*>(&dst);
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            return false;
        }
        std::memcpy(ip6_out, s6->sin6_addr.s6_addr, 16);
        *udp_dst_port_be = s6->sin6_port;
        return true;
    }

    static bool eth_all_zero(const std::byte e[6]) noexcept {
        for (int i = 0; i < 6; i++) {
            if (e[static_cast<std::size_t>(i)] != std::byte{0}) {
                return false;
            }
        }
        return true;
    }

    static ssize_t commit_tx_headers_and_payload(const uint8_t* hdr,
        unsigned hdr_len,
        const std::byte* payload_dev,
        snakeio::size_t payload_len,
        uint32_t total_frame_len,
        snakeio::size_t return_payload_len) noexcept {
        if (static_cast<std::size_t>(hdr_len) + static_cast<std::size_t>(payload_len) > k_tx_pkt_buf_bytes) {
            return -1;
        }

        const cudaError_t h2d =
            cudaMemcpyAsync(g_tx.pkt_gpu, hdr, hdr_len, cudaMemcpyHostToDevice, g_stream);
        if (h2d != cudaSuccess) {
            snakeio::logger::warn("send_udp_datagram_gpu: cudaMemcpyAsync HtoD (headers) failed: {}.",
                cudaGetErrorString(h2d));
            return -1;
        }

        if (payload_len != 0) {
            const cudaError_t d2d = cudaMemcpyAsync(g_tx.pkt_gpu + hdr_len,
                payload_dev,
                payload_len,
                cudaMemcpyDeviceToDevice,
                g_stream);
            if (d2d != cudaSuccess) {
                snakeio::logger::warn("send_udp_datagram_gpu: cudaMemcpyAsync DtoD (payload) failed: {}.",
                    cudaGetErrorString(d2d));
                return -1;
            }
        }

        const cudaError_t kl = snakeio_doca_gpunetio_tx_send_one_launch(
            g_stream, g_tx.eth_txq_gpu, g_tx.pkt_gpu, g_tx.mkey_be32, total_frame_len);
        if (kl != cudaSuccess) {
            snakeio::logger::warn("snakeio_doca_gpunetio_tx_send_one_launch failed: {}.", cudaGetErrorString(kl));
            return -1;
        }

        const cudaError_t sy = cudaStreamSynchronize(g_stream);
        if (sy != cudaSuccess) {
            snakeio::logger::warn("send_udp_datagram_gpu: cudaStreamSynchronize failed: {}.", cudaGetErrorString(sy));
            return -1;
        }

        doca_eth_txq_gpu_cpu_proxy_progress(g_tx.eth_txq);
        return static_cast<ssize_t>(return_payload_len);
    }

    ssize_t send_udp_datagram_gpu(const std::byte* payload_dev,
        snakeio::size_t payload_len,
        const sockaddr_storage& dst,
        const std::byte dst_eth[6]) noexcept {
        if (!g_inited || !g_tx.ready || g_stream == nullptr || g_tx.eth_txq_gpu == nullptr || g_tx.pkt_gpu == nullptr) {
            return -1;
        }

        uint8_t l2_dst[6]{};
        if (eth_all_zero(dst_eth)) {
            if (!g_tx.has_gateway_mac) {
                return -1;
            }
            std::memcpy(l2_dst, g_tx.gateway_mac, 6);
        } else {
            for (int i = 0; i < 6; i++) {
                l2_dst[static_cast<std::size_t>(i)] =
                    static_cast<uint8_t>(dst_eth[static_cast<std::size_t>(i)]);
            }
        }

        uint8_t dst_ip4[4]{};
        uint8_t dst_ip6[16]{};
        uint16_t dst_udp_port_be = 0;
        const bool to_v4 = extract_ipv4_udp_dest(dst, dst_ip4, &dst_udp_port_be);
        const bool to_v6 = !to_v4 && g_tx.has_local_ip6 && extract_ipv6_udp_dest(dst, dst_ip6, &dst_udp_port_be);

        if (!to_v4 && !to_v6) {
            return -1;
        }

        std::array<uint8_t, 64> hdr{};
        unsigned hdr_len = 0;
        const uint16_t src_udp_port_be = htons(snakeio::data_plane_ext_port);

        if (to_v4) {
            if (!doca_tx_frame::build_ipv4_udp(hdr,
                    hdr_len,
                    l2_dst,
                    g_tx.nic_mac,
                    g_tx.local_ip4,
                    dst_ip4,
                    src_udp_port_be,
                    dst_udp_port_be,
                    payload_len)) {
                return -1;
            }
            const uint32_t total = static_cast<uint32_t>(hdr_len + payload_len);
            return commit_tx_headers_and_payload(hdr.data(), hdr_len, payload_dev, payload_len, total, payload_len);
        }

        if (!doca_tx_frame::build_ipv6_udp(hdr,
                hdr_len,
                l2_dst,
                g_tx.nic_mac,
                g_tx.local_ip6,
                dst_ip6,
                src_udp_port_be,
                dst_udp_port_be,
                payload_len)) {
            return -1;
        }
        const uint32_t total6 = static_cast<uint32_t>(hdr_len + payload_len);
        return commit_tx_headers_and_payload(hdr.data(), hdr_len, payload_dev, payload_len, total6, payload_len);
    }
}
