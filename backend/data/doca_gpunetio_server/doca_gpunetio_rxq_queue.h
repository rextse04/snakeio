#pragma once

struct doca_gpu;
struct doca_dev;
struct doca_ctx;
struct doca_eth_rxq;
struct doca_gpu_eth_rxq;
struct doca_eth_txq;
struct doca_gpu_eth_txq;
struct doca_mmap;
struct doca_flow_port;
struct doca_flow_pipe;
struct doca_flow_pipe_entry;

struct rxq_queue {
	struct doca_gpu *gpu_dev;
	struct doca_dev *ddev;
	struct doca_ctx *eth_rxq_ctx;
	struct doca_eth_rxq *eth_rxq_cpu;
	struct doca_gpu_eth_rxq *eth_rxq_gpu;
	struct doca_ctx *eth_txq_ctx;
	struct doca_eth_txq *eth_txq_cpu;
	struct doca_gpu_eth_txq *eth_txq_gpu;
	struct doca_mmap *pkt_buff_mmap;
	void *gpu_pkt_addr;
	int dmabuf_fd;
	struct doca_flow_port *port;
	///< Inner RSS pipes: IPv6 and IPv4 UDP both steer to queue 0 (same Eth Rxq).
	struct doca_flow_pipe *rxq_pipe;
	struct doca_flow_pipe *rxq_pipe_ip4;
	struct doca_flow_pipe *root_pipe;
	struct doca_flow_pipe_entry *root_udp_entry;
	struct doca_flow_pipe_entry *root_udp4_entry;
};
