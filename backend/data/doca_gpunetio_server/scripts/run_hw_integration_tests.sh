#!/usr/bin/env bash
# Run doca_gpunetio integration tests on the non-skip path by exporting PCI BDFs.
# Optional: SNAKEIO_DOCA_FLOW_MODE (default vnf in code), SNAKEIO_DOCA_FLOW_PORT_DEVARGS, SNAKEIO_DOCA_FLOW_PORT_ID
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
server_dir=$(cd "${script_dir}/.." && pwd)
build_dir=${1:-"${server_dir}/../cmake-build-debug"}
bin_dir="${build_dir}/doca_gpunetio_server"

if [[ ! -x "${bin_dir}/doca_gpunetio_runtime_lifecycle_test" ]]; then
    echo "error: missing ${bin_dir}/doca_gpunetio_runtime_lifecycle_test (pass cmake build dir as \$1)" >&2
    exit 1
fi

if [[ -z "${SNAKEIO_DOCA_GPU_PCI:-}" ]]; then
    bus=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ' | tr '[:upper:]' '[:lower:]')
    # nvidia-smi: 00000000:ab:00.0 -> 0000:ab:00.0 (DOCA_DEVINFO_PCI_ADDR_SIZE format)
    if [[ "$bus" =~ ^00000000:([0-9a-f]{2}:[0-9a-f]{2}\.[0-7])$ ]]; then
        export SNAKEIO_DOCA_GPU_PCI="0000:${BASH_REMATCH[1]}"
    elif [[ "$bus" =~ ^0000: ]]; then
        export SNAKEIO_DOCA_GPU_PCI="$bus"
    else
        echo "error: could not parse GPU pci.bus_id from nvidia-smi: ${bus:-empty}" >&2
        exit 1
    fi
fi

if [[ -z "${SNAKEIO_DOCA_NIC_PCI:-}" ]]; then
    nic_pci=""
    for netdev in /sys/class/net/*; do
        [[ -e "$netdev/device" ]] || continue
        base=$(basename "$netdev")
        [[ "$base" == "lo" ]] && continue
        drv=$(readlink -f "$netdev/device/driver" 2>/dev/null || true)
        [[ "$drv" == *mlx5_core ]] || continue
        devpath=$(readlink -f "$netdev/device")
        nic_pci=$(basename "$devpath")
        break
    done
    if [[ -z "$nic_pci" ]]; then
        echo "error: no mlx5 class net device found; set SNAKEIO_DOCA_NIC_PCI manually" >&2
        exit 1
    fi
    export SNAKEIO_DOCA_NIC_PCI="$nic_pci"
fi

echo "SNAKEIO_DOCA_GPU_PCI=${SNAKEIO_DOCA_GPU_PCI}"
echo "SNAKEIO_DOCA_NIC_PCI=${SNAKEIO_DOCA_NIC_PCI}"
echo "SNAKEIO_DOCA_FLOW_MODE=${SNAKEIO_DOCA_FLOW_MODE:-<default vnf>}"
echo "SNAKEIO_DOCA_FLOW_PORT_ID=${SNAKEIO_DOCA_FLOW_PORT_ID:-0}"
echo "SNAKEIO_DOCA_FLOW_PORT_DEVARGS=${SNAKEIO_DOCA_FLOW_PORT_DEVARGS:-<unset>}"
cat <<EOF
If devlink returns "Operation not supported" (EOPNOTSUPP) on this PCI device:
  BlueField-3 "integrated" / bifurcated ports often do NOT allow changing eswitch mode from the x86 host.
  The e-switch is owned and configured on the DPU (ARM) side; representors live there, not on the host.
  Run DOCA Flow / GPUNetIO tests from the BlueField Linux (or use a standalone ConnectX on the host).

If devlink returns "Operation not permitted" (EPERM):
  Run the same command with privileges (root / CAP_NET_ADMIN). Only the PF that owns fw tracer may change mode.

If doca_flow_port_start still fails after eswitch is correct for your topology:
  Stop conflicting owners (OVS offload, other doca_flow users), clear stale flows, then retry.
EOF
echo ""

cd "$bin_dir"
./doca_gpunetio_runtime_lifecycle_test
./doca_gpunetio_transport_smoke_test
echo "hw integration tests finished OK"
