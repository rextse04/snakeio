"""
Simple plaintext packet decoder for the snake-like game protocol (port 50003)
Only decodes UNENCRYPTED / plaintext packets in hex format.

Supports both directions:
- Client -> Server
- Server -> Client (delta, snapshot, lobby_status, termination)
"""

import struct
import sys
from typing import List, Tuple

def read_hex(prompt: str) -> bytes:
    while True:
        s = input(prompt).strip()
        if not s:
            continue
        try:
            if s.startswith("0x"):
                s = s[2:]
            hex_bytes = bytes.fromhex(s)
            return hex_bytes
        except ValueError:
            print("Invalid hex string. Use space-separated or continuous hex.")
            print("Example: 01000000 0000803f  or 010000000000803f")


def read_int(prompt: str, default=None) -> int:
    while True:
        s = input(prompt).strip()
        if not s and default is not None:
            return default
        try:
            return int(s)
        except ValueError:
            print("Please enter a valid integer.")


def read_float_list(data: bytes, count: int) -> List[Tuple[float, float]]:
    result = []
    pos = 0
    for _ in range(count):
        if pos + 8 > len(data):
            raise ValueError("Not enough data for vector list")
        x, y = struct.unpack_from("<ff", data, pos)
        result.append((x, y))
        pos += 8
    return result


def decode_snake_basic(data: bytes, offset: int = 0) -> dict:
    if offset + 24 > len(data):
        raise ValueError("Not enough data for snake_basic")
    s = struct.unpack_from("<ffffI I ? ? xx", data, offset)
    return {
        "speed": s[0],
        "angle": s[1],
        "width": s[2],          # radius of each segment
        "length": s[3],
        "score": s[4],
        "alive": bool(s[5]),
        "human": bool(s[6]),
    }


def decode_snake_full(data: bytes, offset: int, length: int) -> dict:
    basic = decode_snake_basic(data, offset)
    offset += 24
    segments = read_float_list(data[offset:], length)
    return {
        **basic,
        "segments": segments
    }


def decode_food(data: bytes, offset: int = 0) -> dict:
    if offset + 12 > len(data):
        raise ValueError("Not enough data for food")
    x, y, w = struct.unpack_from("<fff", data, offset)
    return {
        "pos": (x, y),
        "width": w
    }


def decode_client_to_server(packet: bytes):
    if len(packet) != 8:
        print(f"Warning: client→server expected 8 bytes, got {len(packet)}")

    snapshot_req, angle = struct.unpack("<I f", packet)
    print("Client → Server")
    print(f"  snapshot_requested : {bool(snapshot_req)}")
    print(f"  angle              : {angle:.6f} rad")


def decode_server_to_client(packet: bytes, num_players: int):
    if len(packet) < 4:
        print("Packet too short")
        return

    msg_type = struct.unpack_from("<I", packet)[0]
    payload = packet[4:]

    print(f"Server → Client  (type = {msg_type})")

    if msg_type == 0:  # delta
        print("  Type: DELTA")
        pos = 0

        # snake_basics
        snake_basics = []
        for i in range(num_players):
            if pos + 24 > len(payload):
                print(f"  Warning: truncated at snake_basic {i}")
                break
            sb = decode_snake_basic(payload, pos)
            snake_basics.append(sb)
            pos += 24
        print(f"  snake_basics ({len(snake_basics)}):")
        for i, sb in enumerate(snake_basics):
            print(f"    [{i}] {sb}")

        # foods added
        if pos + 4 > len(payload):
            return
        foods_added_count = struct.unpack_from("<I", payload, pos)[0]
        pos += 4
        foods_added = []
        for _ in range(foods_added_count):
            if pos + 12 > len(payload):
                print("  Warning: truncated in foods_added")
                break
            foods_added.append(decode_food(payload, pos))
            pos += 12
        print(f"  foods_added ({len(foods_added)}):")
        for f in foods_added:
            print(f"    {f}")

        # foods removed
        if pos + 4 > len(payload):
            return
        foods_removed_count = struct.unpack_from("<I", payload, pos)[0]
        pos += 4
        foods_removed = read_float_list(payload[pos:], foods_removed_count)
        print(f"  foods_removed ({len(foods_removed)}):")
        for v in foods_removed:
            print(f"    {v}")

    elif msg_type == 1:  # snapshot
        print("  Type: SNAPSHOT")
        pos = 0
        width, height, max_tick = struct.unpack_from("<ffI", payload, pos)
        pos += 12
        print(f"  world: {width:.1f} × {height:.1f}")
        print(f"  max_tick: {max_tick}")

        snakes = []
        for i in range(num_players):
            if pos + 24 > len(payload):
                print(f"  Warning: truncated at snake {i}")
                break
            length = struct.unpack_from("<I", payload, pos + 16)[0]  # length field
            snake = decode_snake_full(payload, pos, length)
            snakes.append(snake)
            pos += 24 + length * 8
        print(f"  snakes ({len(snakes)}):")
        for i, s in enumerate(snakes):
            print(f"    [{i}] length={s['length']}, score={s['score']}, alive={s['alive']}")

        foods_count = struct.unpack_from("<I", payload, pos)[0]
        pos += 4
        foods = []
        for _ in range(foods_count):
            if pos + 12 > len(payload):
                break
            foods.append(decode_food(payload, pos))
            pos += 12
        print(f"  foods ({len(foods)}):")
        for f in foods:
            print(f"    pos={f['pos']}, width={f['width']:.2f}")

    elif msg_type == 2:  # lobby_status
        print("  Type: LOBBY_STATUS")
        connected = list(payload[:num_players])
        print(f"  connected: {[bool(x) for x in connected]}")

    elif msg_type == 3:  # termination
        print("  Type: TERMINATION")
        finals = []
        pos = 0
        for i in range(num_players):
            if pos + 24 > len(payload):
                break
            sb = decode_snake_basic(payload, pos)
            finals.append(sb)
            pos += 24
        print(f"  final snake_basics ({len(finals)}):")
        for i, sb in enumerate(finals):
            print(f"    [{i}] {sb}")

    else:
        print(f"  Unknown message type: {msg_type}")


def main():
    print("=== Game Protocol 50003 Plaintext Packet Decoder ===\n")

    direction = input("Direction (c = client→server, s = server→client): ").strip().lower()
    if direction not in ('c', 's'):
        print("Please enter 'c' or 's'")
        return

    num_players = None
    if direction == 's':
        num_players = read_int("Number of players in the game? ", default=4)
        print()

    hex_data = read_hex("Paste hex packet (big-endian or little-endian byte order): ")

    try:
        if direction == 'c':
            decode_client_to_server(hex_data)
        else:
            decode_server_to_client(hex_data, num_players)
    except Exception as e:
        print(f"\nDecode error: {e}")
        print("Partial parse may have occurred above.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBye.")
    except Exception as e:
        print(f"Unexpected error: {e}", file=sys.stderr)