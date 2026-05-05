#!/usr/bin/env bash
# Run the GPU UDP reachability toy on the GPUNetIO host (root). From the DPU, send probes, e.g.:
#   ./doca_udp_probe_send.sh 10.10.10.2 50003
set -euo pipefail
cd "$(dirname "$0")/../../cmake-build-release"
exec sudo -E env \
  SNAKEIO_DOCA_PACKET_IO_DST="${SNAKEIO_DOCA_PACKET_IO_DST:?set to this host data-plane IPv4}" \
  SNAKEIO_NIC_PCIE="${SNAKEIO_NIC_PCIE:-0000:bd:00.0}" \
  SNAKEIO_GPU_PCIE="${SNAKEIO_GPU_PCIE:-0000:ab:00.0}" \
  SNAKEIO_DOCA_TOY_TIMEOUT_SEC="${SNAKEIO_DOCA_TOY_TIMEOUT_SEC:-120}" \
  ./doca_gpunetio_server/doca_gpunetio_toy_gpu_udp_reach
