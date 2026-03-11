import math
import sys

KEY_SIZE = 32
NONCE_SIZE = 12

def rotl(a: int, b: int):
    """Rotate left: (a << b) | (a >> (32 - b))"""
    return ((a << b) & 0xFFFFFFFF) | (a >> (32 - b))

def quarter_round(a: int, b: int, c: int, d: int):
    a = (a + b) & 0xFFFFFFFF; d ^= a; d = rotl(d, 16)
    c = (c + d) & 0xFFFFFFFF; b ^= c; b = rotl(b, 12)
    a = (a + b) & 0xFFFFFFFF; d ^= a; d = rotl(d,  8)
    c = (c + d) & 0xFFFFFFFF; b ^= c; b = rotl(b,  7)
    return a, b, c, d

def chacha20_block(key: bytes, counter: int, nonce: bytes):
    # "expand 32-byte k"
    constants = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]
    key_ints = [int.from_bytes(key[i:i+4], "little") for i in range(0, KEY_SIZE, 4)]
    nonce_ints = [int.from_bytes(nonce[i:i+4], "little") for i in range(0, NONCE_SIZE, 4)]
    state = constants + key_ints + [counter] + nonce_ints
    # initial_state
    x = state[:]
    for _ in range(10):
        x[0], x[4], x[8],  x[12] = quarter_round(x[0], x[4], x[8],  x[12])
        x[1], x[5], x[9],  x[13] = quarter_round(x[1], x[5], x[9],  x[13])
        x[2], x[6], x[10], x[14] = quarter_round(x[2], x[6], x[10], x[14])
        x[3], x[7], x[11], x[15] = quarter_round(x[3], x[7], x[11], x[15])
        x[0], x[5], x[10], x[15] = quarter_round(x[0], x[5], x[10], x[15])
        x[1], x[6], x[11], x[12] = quarter_round(x[1], x[6], x[11], x[12])
        x[2], x[7], x[8],  x[13] = quarter_round(x[2], x[7], x[8],  x[13])
        x[3], x[4], x[9],  x[14] = quarter_round(x[3], x[4], x[9],  x[14])
    for i in range(16):
        x[i] = (x[i] + state[i]) & 0xFFFFFFFF
    keystream = bytearray(64)
    for i in range(16):
        keystream[i*4 : i*4+4] = x[i].to_bytes(4, "little")
    return bytes(keystream)

def chacha20_encrypt(key: bytes, nonce: bytes, plaintext: bytes, counter = 1) -> bytes:
    ciphertext = bytearray()
    block_count = 0
    for i in range(0, len(plaintext), 64):
        keystream = chacha20_block(key, counter + block_count, nonce)
        block = plaintext[i:i+64]
        ciphertext.extend(bytes([b ^ k for b, k in zip(block, keystream)]))
        block_count += 1
        debug and print(f"Block {block_count}: {keystream.hex()}")
    return bytes(ciphertext)

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

def poly1305_mac(key: bytes, nonce: None, msg: bytes) -> bytes:
    # Split key
    r = le_bytes_to_num(key[0:16])
    r = clamp_r(r)
    s = le_bytes_to_num(key[16:32])
    a = 0
    p = (1 << 130) - 5
    # Process 16-byte blocks
    block_count = math.ceil(len(msg) / 16)
    for i in range(block_count):
        debug and print("Block", i)
        block = msg[i*16:(i+1)*16]
        # Append 0x01 byte
        n = le_bytes_to_num(block + b"\x01")
        a = (a + n) % p
        debug and print_chunk(a)
        a = (r * a) % p
        debug and print_chunk(a)
    a += s
    print_chunk(a)
    return num_to_16_le_bytes(a)

if __name__ == "__main__":
    debug = False
    if len(sys.argv) == 1:
        debug = True
        key = bytes.fromhex(input("Key: "))
        if len(key) != KEY_SIZE:
            raise ValueError(f"Key must be {KEY_SIZE} bytes")
        nonce = bytes.fromhex(input("Nonce: "))
        if len(nonce) not in [0, NONCE_SIZE]:
            raise ValueError(f"Nonce must be {NONCE_SIZE} bytes")
        msg = bytes.fromhex(input("Message: "))
        if len(msg) % 16 != 0:
            raise ValueError("Message must be a multiple of 16 bytes")
        if len(nonce) != 0:
            ciphertext = chacha20_encrypt(key, nonce, msg)
            print("Ciphertext:", ciphertext.hex())
            print()
        tag = poly1305_mac(key, None, msg)
        print("Tag:", tag.hex())
    elif len(sys.argv) == 3:
        func = globals()[sys.argv[1]]
        msg_size = int(sys.argv[2])
        with open("crypt_data.bin", "rb") as f:
            with open("crypt_solution.bin", "wb") as g:
                while chunk := f.read(KEY_SIZE + NONCE_SIZE + msg_size):
                    out = func(chunk[:KEY_SIZE], chunk[KEY_SIZE:KEY_SIZE+NONCE_SIZE], chunk[KEY_SIZE+NONCE_SIZE:])
                    g.write(out)