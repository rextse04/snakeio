# General Requirements
- Sizes are in bytes, with each byte being 8 bits.
- All integers and floats are written in little endian.
- All floats are written in IEEE 754 format.
- All angles are in radians.
- Unless otherwise specified, every packet is packed,
i.e. no padding is added.
- All packet formats in this document are expected to be sent over UDP.

# Client to Server
| Field              | Type  | Size | Description                                             |
|--------------------|-------|------|---------------------------------------------------------|
| snapshot_requested | bool  | 4    | Whether a snapshot should be requested from the server. |
| angle              | float | 4    | Update to player's angle. |
Size = 8

# Server to Client
| Field | Type       | Size     | Description           |
|-------|------------|----------|-----------------------|
| type | unsigned   | 4        | 0: delta, 1: snapshot |
| type specific | - | variable | See below. |

## Delta
| Field | Type          | Size                   | Description               |
| ------|---------------|------------------------|---------------------------|
| snake_basics | snake_basic[] | 24 * players           | See below.                |
| foods_added_size | unsigned      | 4                      | Number of foods added.    |
| foods_added | food[]        | 12 * foods_added_size  | See below.                |
| foods_removed_size | unsigned      | 4                      | Number of foods removed.  |
| foods_removed | (float, float) | 8 * foods_removed_size | Vectors of removed foods. |
Max size = 41352 (~41KB)

## Snapshot
| Field | Type    | Size            | Description               |
| ------|---------|-----------------|---------------------------|
| width | float   | 4               | World width.       |
| height | float   | 4               | World height.      |
| players | unsigned | 4               | Number of players. |
| snakes | snake[] | 8216 * players  | See below.       |
| foods_size | unsigned | 4               | Number of foods. |
| foods | food[] | 12 * foods_size | See below.     |
Max size = 156048 (~154KB)

## snake_basic
| Field | Type    | Size | Description |
|-------|---------|------|-------------|
| speed | float   | 4    | Speed.                  |
| angle | float   | 4    | Angle.                  |
| width | float | 4    | Radius of each segment. |
| length | unsigned | 4    | Number of segments. |
| score | unsigned | 4    | Score. |
| alive | bool    | 1    | Whether the snake is alive. |
| human | bool    | 1    | Whether the snake is human. |
| padding | - | 2 | Padding. |
Size = 24

## snake
| Field | Type    | Size | Description |
|-------|---------|------|-------------|
| snake_basic | snake_basic | 24 | See above. |
| segments | (float, float)[] | 8 * length | Coordinates of each segment. |
Max size = 8224 (~8KB)

## food
| Field | Type    | Size | Description |
|-------|---------|------|-------------|
| pos | (float, float) | 8 | Coordinates of food. |
| width | float | 4 | Radius of food. Round down to obtain score boost. |
Size = 12

# Encryption and Decryption
All packets between client and server are encrypted using ChaCha20-poly1305.
## Format of encrypted packets
| Field | Type     | Size | Description |
|-------|----------|------|-------------|
| session_id | unsigned | 4 | Session ID. |
| player_id | unsigned | 4 | Player ID. |
| nonce | byte[12] | 12 | Nonce. |
| ciphertext | byte[] | variable | Ciphertext. |
| tag | byte[16]  | 16 | Tag. |
- If sent by client, the first byte of nonce is 0.
- If sent by server, the first byte of nonce is 1 and the last 4 bytes of nonce are the tick number.
This ensures uniqueness as the server only sends one packet per tick.