#include <doca_flow.h>
#include <doca_dev.h>
#include <arpa/inet.h>
int main() {
    struct doca_flow_cfg *flow_cfg;
    doca_flow_cfg_create(&flow_cfg);
    doca_flow_cfg_set_pipe_queues(flow_cfg, 1);
    doca_flow_cfg_set_mode_args(flow_cfg, "vnf,hws");
    doca_flow_init(flow_cfg);
    doca_flow_cfg_destroy(flow_cfg);

    struct doca_flow_port_cfg *port_cfg;
    doca_flow_port_cfg_create(&port_cfg);
    doca_flow_port_cfg_set_dev(port_cfg, NULL);
    struct doca_flow_port *port;
    doca_flow_port_start(port_cfg, &port);

    struct doca_flow_pipe_cfg *pipe_cfg;
    doca_flow_pipe_cfg_create(&pipe_cfg, port);
    doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
    doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);

    struct doca_flow_match match_mask = {};
    struct doca_flow_match match_value = {};
    match_mask.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
    match_mask.outer.udp.l4_port.dst_port_ext = 0xFFFF;
    match_value.outer.udp.l4_port.dst_port_ext = 1234;
    doca_flow_pipe_cfg_set_match(pipe_cfg, &match_value, &match_mask);

    struct doca_flow_fwd fwd_cfg = {};
    fwd_cfg.type = DOCA_FLOW_FWD_PORT;
    fwd_cfg.port_id = 0;

    struct doca_flow_pipe *rxq_pipe;
    doca_flow_pipe_create(pipe_cfg, &fwd_cfg, NULL, &rxq_pipe);
    doca_flow_pipe_cfg_destroy(pipe_cfg);

    return 0;
}
