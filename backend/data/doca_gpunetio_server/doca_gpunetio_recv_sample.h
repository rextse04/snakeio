#pragma once
#include <doca_dev.h>
#include <doca_error.h>

struct doca_gpu;
struct doca_dev;
struct rxq_queue;
struct doca_flow_port;

#ifdef __cplusplus
extern "C" {
#endif

extern struct doca_flow_port *g_doca_df_port;

doca_error_t open_doca_device_with_pci(const char *pci_addr,
    doca_error_t (*)(struct doca_devinfo *),
    struct doca_dev **retval);

doca_error_t snakeio_recv_init_flow(void);
doca_error_t snakeio_recv_start_port(struct doca_dev *dev);
doca_error_t snakeio_recv_create_rxq(struct rxq_queue *rxq, struct doca_gpu *gpu_dev, int cuda_id, struct doca_dev *ddev);
doca_error_t snakeio_recv_destroy_rxq(struct rxq_queue *rxq);
doca_error_t snakeio_recv_create_txq(struct rxq_queue *rxq, struct doca_gpu *gpu_dev, struct doca_dev *ddev);
doca_error_t snakeio_recv_destroy_txq(struct rxq_queue *rxq);
doca_error_t snakeio_recv_progress_txq(struct rxq_queue *rxq);

#ifdef __cplusplus
}
#endif
