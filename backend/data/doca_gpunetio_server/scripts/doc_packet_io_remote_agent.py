#!/usr/bin/env python3
"""Run on the *packet sender* host (CPU-only is fine — no GPU/CUDA/DOCA required).

One UDP socket: send datagram(s) to the GPUNetIO test host's data-plane IPv4:port, then recv() replies.

Invoked as: cat this_file | ssh REMOTE python3 -u - DST_IPV4 PORT RECV_TAIL_MS B64_SEND [B64_SEND ...]
  (argv[0] is '-' when reading script from stdin.)

Prints one line per received datagram:  PKT <base64>
"""
import base64
import socket
import sys
import time


def main() -> None:
    if len(sys.argv) < 5:
        print("usage: python3 -u - DST PORT RECV_TAIL_MS B64 [B64...]  (script on stdin)", file=sys.stderr)
        sys.exit(2)
    dst = sys.argv[1]
    port = int(sys.argv[2])
    recv_tail_ms = int(sys.argv[3])
    sends = sys.argv[4:]

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.settimeout(0.05)

    for i, b64 in enumerate(sends):
        raw = base64.b64decode(b64)
        s.sendto(raw, (dst, port))
        if i + 1 < len(sends):
            time.sleep(0.15)

    # Match tests/packet_io.cpp (~50ms) but allow extra scheduling slack so one host `tick`
    # runs after the datagram is visible on the DOCA RX path.
    time.sleep(0.12)

    print("SENT", flush=True)

    deadline = time.monotonic() + recv_tail_ms / 1000.0
    got: list[bytes] = []
    while time.monotonic() < deadline:
        try:
            data, _addr = s.recvfrom(65536)
            if data:
                got.append(data)
        except socket.timeout:
            pass

    for chunk in got:
        print("PKT " + base64.b64encode(chunk).decode("ascii"), flush=True)


main()
