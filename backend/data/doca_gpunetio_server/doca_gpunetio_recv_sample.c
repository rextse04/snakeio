#include <arpa/inet.h>
#include <netinet/in.h>
#include <doca_flow.h>
#include <doca_log.h>
DOCA_LOG_REGISTER(SNAKEIO_DOCA_RECV);
#include <doca_mmap.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_gpu_data_path.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_gpu_data_path.h>
#include <doca_dev.h>
#include <doca_gpunetio.h>
#include <cuda_runtime.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <doca_error.h>
#include "doca_gpunetio_rxq_queue.h"

typedef doca_error_t (*tasks_check)(struct doca_devinfo *);
typedef doca_error_t (*open_dev_cb)(struct doca_devinfo *devinfo, void *usr_ctx, struct doca_dev **dev);

#define FLOW_NB_COUNTERS 524228
#define MBUF_NUM 8192
#define MBUF_SIZE 2048
#define MAX_PCI_ADDRESS_LEN 32U
#define MAX_PKT_NUM 16384
#define MAX_PKT_SIZE 2048
#define SNAKEIO_ETH_TX_QUEUE_ID 0
#define ALIGN_SIZE(size, align) size = (((size) + ((align)-1)) / (align)) * (align)

struct doca_flow_port *g_doca_df_port;
static size_t get_host_page_size(void)
{
	long ret = sysconf(_SC_PAGESIZE);
	if (ret == -1)
		return 4096; // 4KB, default Linux page size
	return (size_t)ret;
}
doca_error_t open_doca_device_with_pci_and_callback(const char *pci_addr,
						    tasks_check func,
						    open_dev_cb open_dev_cb,
						    void *usr_ctx,
						    struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	uint8_t is_addr_equal = 0;
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_addr_equal);
		if (res == DOCA_SUCCESS && is_addr_equal) {
			/* If any special capabilities are needed */
			if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
				continue;

			/* if device can be opened */
			if (open_dev_cb != NULL) {
				res = open_dev_cb(dev_list[i], usr_ctx, retval);
				if (res == DOCA_SUCCESS) {
					doca_devinfo_destroy_list(dev_list);
					return res;
				}
			}
			res = doca_dev_open(dev_list[i], retval);
			if (res == DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				return res;
			}
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_pci(const char *pci_addr, tasks_check func, struct doca_dev **retval)
{
	return open_doca_device_with_pci_and_callback(pci_addr, func, NULL, NULL, retval);
}

static doca_error_t init_doca_flow(void)
{
	struct doca_flow_cfg *queue_flow_cfg;
	doca_error_t result;

	/* Initialize doca flow framework */
	result = doca_flow_cfg_create(&queue_flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_cfg_set_pipe_queues(queue_flow_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_queues: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}

	result = doca_flow_cfg_set_mode_args(queue_flow_cfg, "vnf");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg mode_args: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}

	result = doca_flow_cfg_set_nr_counters(queue_flow_cfg, FLOW_NB_COUNTERS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_counters: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}

	result = doca_flow_init(queue_flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca flow with: %s", doca_error_get_descr(result));
		doca_flow_cfg_destroy(queue_flow_cfg);
		return result;
	}
	doca_flow_cfg_destroy(queue_flow_cfg);

	return DOCA_SUCCESS;
}

/*
 * Start doca flow.
 *
 * @dev [in]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t start_doca_flow(struct doca_dev *dev)
{
	struct doca_flow_port_cfg *port_cfg;
	doca_error_t result;

	/* Start doca flow port */
	result = doca_flow_port_cfg_create(&port_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_port_id(port_cfg, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port ID: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	result = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg dev: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	result = doca_flow_port_start(port_cfg, &g_doca_df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca flow port with: %s", doca_error_get_descr(result));
		doca_flow_port_cfg_destroy(port_cfg);
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Flow UDP pipeline
 *
 * @rxq [in]: Receive queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_udp_pipe_l3(struct rxq_queue *rxq, bool ipv6, const char *pipe_name,
				       struct doca_flow_pipe **pipe_out)
{
	doca_error_t result;
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd miss_fwd = {0};
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	uint16_t rss_queues[1];
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};

	if (rxq == NULL || g_doca_df_port == NULL || pipe_out == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	match.parser_meta.outer_l3_type = ipv6 ? DOCA_FLOW_L3_META_IPV6 : DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

	doca_eth_rxq_apply_queue_id(rxq->eth_rxq_cpu, 0);
	rss_queues[0] = 0;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = (ipv6 ? DOCA_FLOW_RSS_IPV6 : DOCA_FLOW_RSS_IPV4) | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = 1;

	miss_fwd.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, g_doca_df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, pipe_out);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	/* Add HW offload */
	result = doca_flow_pipe_basic_add_entry(0,
						*pipe_out,
						&match,
						0,
						NULL,
						NULL,
						NULL,
						DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						NULL,
						&entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(g_doca_df_port, 0, 0, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RxQ pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create DOCA Flow root pipeline
 *
 * @rxq [in]: Receive queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_root_pipe(struct rxq_queue *rxq)
{
	doca_error_t result;
	struct doca_flow_monitor monitor = {
		.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
	};

	struct doca_flow_match udp6_match = {
		.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV6),
		.outer.l3_type = DOCA_FLOW_L3_TYPE_IP6,
		.outer.ip6.next_proto = IPPROTO_UDP,
	};
	struct doca_flow_match udp4_match = {
		.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4),
		.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4,
		.outer.ip4.next_proto = IPPROTO_UDP,
	};

	struct doca_flow_fwd udp6_fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
	};
	struct doca_flow_fwd udp4_fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
	};

	struct doca_flow_pipe_cfg *pipe_cfg;
	const char *pipe_name = "ROOT_PIPE";

	if (rxq == NULL || rxq->rxq_pipe == NULL || rxq->rxq_pipe_ip4 == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	udp6_fwd.next_pipe = rxq->rxq_pipe;
	udp4_fwd.next_pipe = rxq->rxq_pipe_ip4;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, g_doca_df_port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, pipe_name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg name: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg type: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_root: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, &rxq->root_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe creation failed with: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_control_add_entry(0,
						  rxq->root_pipe,
						  &udp6_match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  0,
						  &udp6_fwd,
						  NULL,
						  &rxq->root_udp_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe IPv6 UDP entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_control_add_entry(0,
						  rxq->root_pipe,
						  &udp4_match,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  NULL,
						  0,
						  &udp4_fwd,
						  NULL,
						  &rxq->root_udp4_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe IPv4 UDP entry creation failed with: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(g_doca_df_port, 0, 0, 0);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Root pipe entry process failed with: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created Pipe %s", pipe_name);

	return DOCA_SUCCESS;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Destroy DOCA Ethernet Tx queue for GPU
 *
 * @rxq [in]: DOCA Eth Rx queue handler
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_rxq(struct rxq_queue *rxq)
{
	doca_error_t result;

	if (rxq == NULL) {
		DOCA_LOG_ERR("Can't destroy UDP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Destroying Rxq");

	if (rxq->root_pipe != NULL) {
		doca_flow_pipe_destroy(rxq->root_pipe);
		rxq->root_pipe = NULL;
	}
	if (rxq->rxq_pipe != NULL) {
		doca_flow_pipe_destroy(rxq->rxq_pipe);
		rxq->rxq_pipe = NULL;
	}
	if (rxq->rxq_pipe_ip4 != NULL) {
		doca_flow_pipe_destroy(rxq->rxq_pipe_ip4);
		rxq->rxq_pipe_ip4 = NULL;
	}

	if (g_doca_df_port != NULL) {
		result = doca_flow_port_stop(g_doca_df_port);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->eth_rxq_ctx != NULL) {
		result = doca_ctx_stop(rxq->eth_rxq_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_stop: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->gpu_pkt_addr != NULL) {
		result = doca_gpu_mem_free(rxq->gpu_dev, rxq->gpu_pkt_addr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free gpu memory: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->eth_rxq_cpu != NULL) {
		result = doca_eth_rxq_destroy(rxq->eth_rxq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_rxq_destroy: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	if (rxq->pkt_buff_mmap != NULL) {
		result = doca_mmap_destroy(rxq->pkt_buff_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(result));
			return DOCA_ERROR_BAD_STATE;
		}
	}

	result = doca_dev_close(rxq->ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Eth dev: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (g_doca_df_port != NULL)
		doca_flow_destroy();

	return DOCA_SUCCESS;
}

/*
 * Create DOCA Ethernet Tx queue for GPU
 *
 * @rxq [in]: DOCA Eth Tx queue handler
 * @gpu_dev [in]: DOCA GPUNetIO device
 * @ddev [in]: DOCA device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_rxq(struct rxq_queue *rxq, struct doca_gpu *gpu_dev, int cuda_id, struct doca_dev *ddev)
{
	doca_error_t result;
	uint32_t cyclic_buffer_size = 0;
	struct cudaDeviceProp prop;

	if (rxq == NULL || gpu_dev == NULL || ddev == NULL) {
		DOCA_LOG_ERR("Can't create UDP queues, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rxq->gpu_dev = gpu_dev;
	rxq->ddev = ddev;
	rxq->port = g_doca_df_port;

	DOCA_LOG_INFO("Creating Sample Eth Rxq");

	result = doca_eth_rxq_create(rxq->ddev, MAX_PKT_NUM, MAX_PKT_SIZE, &(rxq->eth_rxq_cpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_create: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_eth_rxq_set_type(rxq->eth_rxq_cpu, DOCA_ETH_RXQ_TYPE_CYCLIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_set_type: %s", doca_error_get_descr(result));
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_CYCLIC,
						       0,
						       0,
						       MAX_PKT_SIZE,
						       MAX_PKT_NUM,
						       0,
						       0,
						       0,
						       &cyclic_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get eth_rxq cyclic buffer size: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_create(&rxq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_add_dev(rxq->pkt_buff_mmap, rxq->ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add dev to mmap: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	ALIGN_SIZE(cyclic_buffer_size, get_host_page_size());

	result = doca_gpu_mem_alloc(rxq->gpu_dev,
				    cyclic_buffer_size,
				    get_host_page_size(),
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)&rxq->gpu_pkt_addr,
				    NULL);
	if (result != DOCA_SUCCESS || rxq->gpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Failed to allocate gpu memory %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Map GPU memory buffer used to receive packets with DMABuf */
	result = doca_gpu_dmabuf_fd(rxq->gpu_dev, rxq->gpu_pkt_addr, cyclic_buffer_size, &(rxq->dmabuf_fd));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Mapping receive queue buffer (%p size %dB) with nvidia-peermem mode",
			      rxq->gpu_pkt_addr,
			      cyclic_buffer_size);

		/* If failed, use nvidia-peermem legacy method */
		result = doca_mmap_set_memrange(rxq->pkt_buff_mmap, rxq->gpu_pkt_addr, cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	} else {
		DOCA_LOG_INFO("Mapping receive queue buffer (%p size %dB dmabuf fd %d) with dmabuf mode",
			      rxq->gpu_pkt_addr,
			      cyclic_buffer_size,
			      rxq->dmabuf_fd);

		result = doca_mmap_set_dmabuf_memrange(rxq->pkt_buff_mmap,
						       rxq->dmabuf_fd,
						       rxq->gpu_pkt_addr,
						       0,
						       cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set dmabuf memrange for mmap %s", doca_error_get_descr(result));
			goto exit_error;
		}
	}

	result = doca_mmap_set_permissions(rxq->pkt_buff_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions for mmap %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_mmap_start(rxq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_rxq_set_pkt_buf(rxq->eth_rxq_cpu, rxq->pkt_buff_mmap, 0, cyclic_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set cyclic buffer  %s", doca_error_get_descr(result));
		goto exit_error;
	}

	cudaGetDeviceProperties(&prop, cuda_id);
	// If pre-Hopper GPU with __CUDA_ARCH__ < 900
	if (prop.major < 9) {
		result = doca_eth_rxq_gpu_enable_mcst_qp(rxq->eth_rxq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set GPU dump qp  %s", doca_error_get_descr(result));
			goto exit_error;
		}
	}

	rxq->eth_rxq_ctx = doca_eth_rxq_as_doca_ctx(rxq->eth_rxq_cpu);
	if (rxq->eth_rxq_ctx == NULL) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_as_doca_ctx: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_ctx_set_datapath_on_gpu(rxq->eth_rxq_ctx, rxq->gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_ctx_start(rxq->eth_rxq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	result = doca_eth_rxq_get_gpu_handle(rxq->eth_rxq_cpu, &(rxq->eth_rxq_gpu));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_get_gpu_handle: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Inner pipes: IPv6 and IPv4 UDP both RSS to this Rxq (queue 0). */
	result = create_udp_pipe_l3(rxq, true, "GPU_RXQ_UDP6_PIPE", &rxq->rxq_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_udp_pipe_l3 (IPv6) returned %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = create_udp_pipe_l3(rxq, false, "GPU_RXQ_UDP4_PIPE", &rxq->rxq_pipe_ip4);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_udp_pipe_l3 (IPv4) returned %s", doca_error_get_descr(result));
		goto exit_error;
	}

	/* Root control: IPv6 and IPv4 UDP matches forward to the corresponding inner pipe. */
	result = create_root_pipe(rxq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function create_root_pipe returned %s", doca_error_get_descr(result));
		goto exit_error;
	}

	return DOCA_SUCCESS;

exit_error:
	destroy_rxq(rxq);
	return DOCA_ERROR_BAD_STATE;
}

static doca_error_t create_txq(struct rxq_queue *rxq, struct doca_gpu *gpu_dev, struct doca_dev *ddev)
{
	doca_error_t result;

	if (rxq == NULL || gpu_dev == NULL || ddev == NULL) {
		DOCA_LOG_ERR("Can't create TXQ, invalid input");
		return DOCA_ERROR_INVALID_VALUE;
	}
	rxq->gpu_dev = gpu_dev;
	rxq->ddev = ddev;

	result = doca_eth_txq_create(ddev, MAX_PKT_NUM, &rxq->eth_txq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_create: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_eth_txq_set_type(rxq->eth_txq_cpu, DOCA_ETH_TXQ_TYPE_REGULAR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_set_type: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_eth_txq_set_l3_chksum_offload(rxq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_set_l3_chksum_offload: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_eth_txq_set_l4_chksum_offload(rxq->eth_txq_cpu, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_set_l4_chksum_offload: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_eth_txq_gpu_set_sq_mem_type(rxq->eth_txq_cpu, DOCA_GPU_MEM_TYPE_GPU);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_gpu_set_sq_mem_type: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_eth_txq_gpu_set_completion_on_gpu(rxq->eth_txq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_gpu_set_completion_on_gpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_eth_txq_gpu_set_uar_on_cpu(rxq->eth_txq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_gpu_set_uar_on_cpu: %s", doca_error_get_descr(result));
		goto exit_error;
	}

	rxq->eth_txq_ctx = doca_eth_txq_as_doca_ctx(rxq->eth_txq_cpu);
	if (rxq->eth_txq_ctx == NULL) {
		DOCA_LOG_ERR("Failed doca_eth_txq_as_doca_ctx");
		result = DOCA_ERROR_BAD_STATE;
		goto exit_error;
	}
	result = doca_ctx_set_datapath_on_gpu(rxq->eth_txq_ctx, gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu for TXQ: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_ctx_start(rxq->eth_txq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start for TXQ: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_eth_txq_apply_queue_id(rxq->eth_txq_cpu, SNAKEIO_ETH_TX_QUEUE_ID);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_apply_queue_id: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	result = doca_eth_txq_get_gpu_handle(rxq->eth_txq_cpu, &rxq->eth_txq_gpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_txq_get_gpu_handle: %s", doca_error_get_descr(result));
		goto exit_error;
	}
	return DOCA_SUCCESS;

exit_error:
	if (rxq->eth_txq_ctx != NULL)
		doca_ctx_stop(rxq->eth_txq_ctx);
	rxq->eth_txq_ctx = NULL;
	if (rxq->eth_txq_cpu != NULL)
		doca_eth_txq_destroy(rxq->eth_txq_cpu);
	rxq->eth_txq_cpu = NULL;
	rxq->eth_txq_gpu = NULL;
	return result;
}

static doca_error_t destroy_txq(struct rxq_queue *rxq)
{
	doca_error_t result;
	if (rxq == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (rxq->eth_txq_ctx != NULL) {
		result = doca_ctx_stop(rxq->eth_txq_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_ctx_stop for TXQ: %s", doca_error_get_descr(result));
			return result;
		}
	}
	rxq->eth_txq_ctx = NULL;
	if (rxq->eth_txq_cpu != NULL) {
		result = doca_eth_txq_destroy(rxq->eth_txq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed doca_eth_txq_destroy: %s", doca_error_get_descr(result));
			return result;
		}
	}
	rxq->eth_txq_cpu = NULL;
	rxq->eth_txq_gpu = NULL;
	return DOCA_SUCCESS;
}

doca_error_t snakeio_recv_init_flow(void)
{
	return init_doca_flow();
}

doca_error_t snakeio_recv_start_port(struct doca_dev *dev)
{
	return start_doca_flow(dev);
}

doca_error_t snakeio_recv_create_rxq(struct rxq_queue *rxq, struct doca_gpu *gpu_dev, int cuda_id, struct doca_dev *ddev)
{
	return create_rxq(rxq, gpu_dev, cuda_id, ddev);
}

doca_error_t snakeio_recv_destroy_rxq(struct rxq_queue *rxq)
{
	return destroy_rxq(rxq);
}

doca_error_t snakeio_recv_create_txq(struct rxq_queue *rxq, struct doca_gpu *gpu_dev, struct doca_dev *ddev)
{
	return create_txq(rxq, gpu_dev, ddev);
}

doca_error_t snakeio_recv_destroy_txq(struct rxq_queue *rxq)
{
	return destroy_txq(rxq);
}

doca_error_t snakeio_recv_progress_txq(struct rxq_queue *rxq)
{
	if (rxq == NULL || rxq->eth_txq_cpu == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	return doca_eth_txq_gpu_cpu_proxy_progress(rxq->eth_txq_cpu);
}
