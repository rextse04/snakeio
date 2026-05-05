#!/usr/bin/env bash
# Send one UDP datagram (run on DPU). Usage: ./doca_udp_probe_send.sh 10.10.10.2 50003
set -euo pipefail
host="${1:?host ip}"
port="${2:?udp port}"
exec python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.sendto(b'probe', ('${host}', ${port})); print('sent', flush=True)"
