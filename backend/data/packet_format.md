# General Requirements
- Sizes are in bytes, with each byte being 8 bits.
- All integers and floats are written in little endian.
- All floats are written in IEEE 754 format.
- All angles are in radians.
- Unless otherwise specified, every packet is packed,
i.e. no padding is added.
- All packet formats in this document are expected to be sent over UDP.

# Server Architecture
The server consists of two planes:
- The **data plane** is responsible for calculating the game state and receiving/sending game packets,
- The **control plane** is responsible for everything else,
including matchmaking, leaderboards, etc.

The game opens three ports:

| Port  | Plane   | Protocol                 | Client             |
|-------|---------|--------------------------|--------------------|
| 50000 | Control | WebSocket over TLS (wss) | External           |
| 50001 | Data    | UDP                      | Internal (::50000) |
| 50002 | Data    | UDP, encrypted           | External           |

# Outline of Game Flow
1. Clients connect to 50000.
2. Clients specify their username and send lobby requirements to 50000.
3. 50000 responds with success or failure. On success, it also sends the following:
   - Session ID
   - Player ID
   - Number of players and their usernames
   - Unique key for encryption
4. Clients send (true, 0) to 50002 to join the lobby.
5. 50002 sends lobby status to clients at regular intervals.
Clients need to respond with (true, 0) until all players have joined.
6. After all players have joined, 50002 sends the initial game snapshot to all clients.
7. Clients and 50002 communicate using the specified packet format below.
8. When tick reaches max_tick, i.e. when client receives a packet at tick = max_tick - 1,
game terminates. Clients disconnect.

# 50002
## Client to Server
| Field              | Type  | Size | Description                                             |
|--------------------|-------|------|---------------------------------------------------------|
| snapshot_requested | bool  | 4    | Whether a snapshot should be requested from the server. |
| angle              | float | 4    | Update to player's angle. |
Size = 8

## Server to Client
| Field         | Type         | Size     | Description                             |
|---------------|--------------|----------|-----------------------------------------|
| type          | unsigned     | 4        | 0: delta, 1: snapshot, 2: lobby_status  |
| type specific | -            | variable | See below.                              |

### delta
| Field | Type             | Size                   | Description               |
| ------|------------------|------------------------|---------------------------|
| snake_basics | snake_basic[]    | 24 * players           | See below.                |
| foods_added_size | unsigned         | 4                      | Number of foods added.    |
| foods_added | food[]           | 12 * foods_added_size  | See below.                |
| foods_removed_size | unsigned         | 4                      | Number of foods removed.  |
| foods_removed | (float, float)[] | 8 * foods_removed_size | Vectors of removed foods. |
Max size = 41352 (~41KB)

### snapshot
| Field      | Type      | Size            | Description              |
|------------|-----------|-----------------|--------------------------|
| width      | float     | 4               | World width.             |
| height     | float     | 4               | World height.            |
| max_tick   | unsigned  | 4               | Termination tick number. |
| snakes     | snake[]   | 8216 * players  | See below.               |
| foods_size | unsigned  | 4               | Number of foods.         |
| foods      | food[]    | 12 * foods_size | See below.               |
Max size = 156048 (~154KB)

### lobby_status
| Field     | Type     | Size        | Description |
|-----------|----------|-------------|-------------|
| connected | bool[]   | 1 * players | Whether each player is connected. |
Max size = 16

### snake_basic
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

### snake
| Field | Type    | Size | Description |
|-------|---------|------|-------------|
| snake_basic | snake_basic | 24 | See above. |
| segments | (float, float)[] | 8 * length | Coordinates of each segment. |
Max size = 8224 (~8KB)

### food
| Field | Type    | Size | Description |
|-------|---------|------|-------------|
| pos | (float, float) | 8 | Coordinates of food. |
| width | float | 4 | Radius of food. Round down to obtain score boost. |
Size = 12

# Encryption and Decryption
All packets between client and server are encrypted using ChaCha20-poly1305.
## Format of encrypted packets
| Field      | Type     | Size | Description            |
|------------|----------|------|------------------------|
| session_id | unsigned | 4 | Session ID.            |
| player_id  | unsigned | 4 | Player ID.             |
| sender     | unsigned | 4 | Client: 0; Server: 1.  |
| nonce_part | unsigned | 4 | Last 4 bytes of nonce. |
| ciphertext | byte[] | variable | Ciphertext.            |
| tag        | byte[16]  | 16 | Tag.                   |
- player_id, sender and nonce_part form the nonce.
- When sender is 1 (the packet is from server), nonce_part is the tick number.
  - As only one packet is sent per tick, this is sufficient to ensure uniqueness.
- Size of ciphertext must be a multiple of 64.
  - This is required for the decryption algorithm to work correctly.
  Server must check that the ciphertext size is divisible by 64 before decrypting.
  - This automatically guarantees the packet size is a multiple of 16.