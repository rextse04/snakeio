#!/usr/bin/env python3
"""Run on the DPU (receiver). Binds UDP, prints READY, then waits for one datagram.

Must match payload prefix in `toys/gpu_udp_egress_reach.cpp`.

Invoked as: cat this_file | ssh REMOTE python3 -u - PORT
"""
import socket
import sys

MAGIC = b"SNAKEIO_GPUNETIO_EGRESS_PROBE_01"


def main() -> None:
    if len(sys.argv) < 2:
        print("usage: python3 -u - PORT  (script on stdin)", file=sys.stderr)
        sys.exit(2)
    port = int(sys.argv[1])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    print("READY", flush=True)
    data, addr = s.recvfrom(4096)
    if data.startswith(MAGIC):
        print("OK", flush=True)
    else:
        print("BAD", len(data), flush=True)


main()
