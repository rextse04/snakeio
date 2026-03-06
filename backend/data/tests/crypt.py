import math

def clamp_r(r: int) -> int:
    return r & 0x0ffffffc0ffffffc0ffffffc0fffffff

def le_bytes_to_num(b: bytes) -> int:
    return int.from_bytes(b, byteorder="little")

def num_to_16_le_bytes(n: int) -> bytes:
    return (n & ((1 << 128) - 1)).to_bytes(16, byteorder="little")

def print_chunk(n: int):
    while n != 0:
        chunk = n & ((1 << 26) - 1)
        print(hex(chunk), end=" ")
        n >>= 26
    print()

def poly1305_mac(msg: bytes, key: bytes) -> bytes:
    # Split key
    r = le_bytes_to_num(key[0:16])
    r = clamp_r(r)
    s = le_bytes_to_num(key[16:32])
    a = 0
    p = (1 << 130) - 5
    # Process 16-byte blocks
    block_count = math.ceil(len(msg) / 16)
    for i in range(block_count):
        print("Block", i)
        block = msg[i*16:(i+1)*16]
        # Append 0x01 byte
        n = le_bytes_to_num(block + b"\x01")
        a = (a + n) % p
        print_chunk(a)
        a = (r * a) % p
        print_chunk(a)
        print()
    a += s
    return num_to_16_le_bytes(a)

if __name__ == "__main__":
    key_str = input("Key: ")
    key_str = key_str.replace(":", "")
    key = bytes.fromhex(key_str)
    if len(key) != 32:
        raise ValueError("Key must be 32 bytes")
    msg = input("Message: ").encode()
    tag = poly1305_mac(msg, key)
    print("Tag:", tag.hex())