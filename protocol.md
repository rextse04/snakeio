# General Requirements
- Sizes are in bytes, with each byte being 8 bits.
- All integers and floats are written in little endian.
- All floats are written in IEEE 754 format.
- All angles are in radians.
- Unless otherwise specified, every packet is packed,
i.e. no padding is added.
    - Most explicit padding in this document is for alignment.
- All packet formats in this document are expected to be sent over UDP.
- A "packet" (logical packet) may not correspond to a single network packet.
When the size of a "packet" is greater 1024 bytes,
it is split into multiple chunks (transport packets) each of size 1024 bytes before encryption,
except for the last chunk, which may be smaller.
- Maximum sizes for packet formats in this document are calculated based on the current implementation.
They are for reference only and may change in the future.
- The server may impose additional limits on packet fields
(e.g. maximum number of players, maximum world size, etc.)

# Server Architecture
The server consists of two planes:
- The **data plane** is responsible for calculating the game state and receiving/sending game packets,
- The **control plane** is responsible for everything else,
including matchmaking, leaderboards, etc.

The game opens three ports:

| Port  | Plane   | Protocol                 | Client             |
|-------|---------|--------------------------|--------------------|
| 50000 | Control | WebSocket over TLS (wss) | External           |
| 50001 | Control | UDP                      | Internal (::50002) |
| 50002 | Data    | UDP                      | Internal (::50001) |
| 50003 | Data    | UDP, encrypted           | External           |

This document mainly describes the protocol for communication to and from the data plane.
Communication between clients and 50000 is expected to be in JSON format.
This document does not cover the WebSocket protocol due to rapid changes in features and implementation.

# Outline of Game Flow
1. Clients connect to 50000.
2. Clients specify their username and send lobby requirements to 50000.
3. 50000 responds with success or failure. On success, it also sends the following:
   - Session ID
   - Player ID
   - Number of players and their usernames
   - Unique key for encryption
4. Clients send (true, NaN) to 50003 to join the lobby.
5. 50003 sends lobby status to clients at regular intervals.
Clients need to respond with (true, 0) until all players have joined.
6. After all players have joined, 50003 sends the initial game snapshot to all clients.
7. Clients and 50003 communicate using the specified packet format below.
8. When game terminates, 50003 sends the termination packet to all clients.

# 50002
This is the UDP port used by the local data plane (bound to the IPv6 loopback).
It accepts simple commands from the internal control client (50001)
to manage server lifecycle and create new game sessions.

## Client to Server (50001 -> 50002)
| Field | Type     | Size     | Description                                 |
|-------|----------|----------|---------------------------------------------|
| cmd   | unsigned | 1        | Command ID. 0 = kill, 1 = new session token |
| args  | -        | variable | See below.                                  |

### Kill (0)
No arguments. No response.

### New Session Token (1)
| Field         | Type     | Size               | Description                                  |
|---------------|----------|--------------------|----------------------------------------------|
| session_token | char[5]  | 5                  | ASCII token provided by the control client.  |
| human_players | unsigned | 1                  | Number of human players requested.           |
| ai_players    | unsigned | 1                  | Number of AI players requested.              |
| max_tick      | unsigned | 4                  | Maximum tick number before termination.      |
| keys          | byte[]   | 32 * human_players | Array of 32-byte keys for each human player. |
Always responds.
- Although it is recommended for the client to validate fields before sending,
it is the server's responsibility to validate all fields and respond with appropriate error code if any field is invalid.

## Server to Client (50002 -> 50001)
If the client sends a command that requires a response, the server responds with the following packet:

| Field | Type     | Size     | Description |
|-------|----------|----------|-------------|
| cmd   | unsigned | 1        | Command ID. |
| args  | -        | variable | See below.  |

### New Session Token (1)
| Field         | Type     | Size | Description                                                                      |
|---------------|----------|------|----------------------------------------------------------------------------------|
| session_token | char[5]  | 5    | Non-NUL-terminated ASCII token provided by the control client.                   |
| result        | unsigned | 1    | 0: ok, 1: no memory, 2: too many players, 3: max tick too big, 4: unknown error. |
| padding       | -        | 1    | Padding.                                                                         |
| session_id    | unsigned | 4    | Session ID assigned to session_token.                                            |
Size = 12

- The server simply echos the session_token back to the client.
The control client is responsible for defining the requirements for and sanitizing session_token given by the user.
- Undefined value for session_id if result is not 0.

# 50003
## Client to Server
| Field              | Type  | Size | Description                                                                   |
|--------------------|-------|------|-------------------------------------------------------------------------------|
| snapshot_requested | bool  | 1    | Whether a snapshot should be requested from the server.                       |
| boost              | bool  | 1    | Whether the player is boosting. Boosting increases speed but decreases score. |
| padding            | -     | 2    | Padding.                                                                      |
| angle              | float | 4    | Update to player's angle.                                                     |
Size = 8

- If angle is not finite, the current angle is kept.

## Server to Client
| Field         | Type         | Size     | Description                                            |
|---------------|--------------|----------|--------------------------------------------------------|
| type          | unsigned     | 4        | 0: delta, 1: snapshot, 2: lobby_status, 3: termination |
| type specific | -            | variable | See below.                                             |

### delta
| Field              | Type             | Size                   | Description               |
|--------------------|------------------|------------------------|---------------------------|
| snake_basics       | snake_basic[]    | 24 * players           | See below.                |
| foods_added_size   | unsigned         | 4                      | Number of foods added.    |
| foods_added        | food[]           | 12 * foods_added_size  | See below.                |
| foods_removed_size | unsigned         | 4                      | Number of foods removed.  |
| foods_removed      | (float, float)[] | 8 * foods_removed_size | Vectors of removed foods. |
Max size = 41352 (~41KB)

### snapshot
| Field      | Type     | Size               | Description                             |
|------------|----------|--------------------|-----------------------------------------|
| width      | float    | 4                  | World width.                            |
| height     | float    | 4                  | World height.                           |
| max_tick   | unsigned | 4                  | Maximum tick number before termination. |
| players    | unsigned | 4                  | Number of players.                      |
| snakes     | snake[]  | variable * players | See below.                              |
| foods_size | unsigned | 4                  | Number of foods.                        |
| foods      | food[]   | 12 * foods_size    | See below.                              |
Max size = 156048 (~154KB)
- players remains constant for the entire game.
- The client is expected to have learnt players from 50000.
That said, this field is included in the snapshot to allow construction of game state without context.

### lobby_status
| Field     | Type   | Size              | Description                       |
|-----------|--------|-------------------|-----------------------------------|
| connected | bool[] | 1 * human_players | Whether each player is connected. |
Max size = 16

### termination
| Field        | Type          | Size         | Description                             |
|--------------|---------------|--------------|-----------------------------------------|
| max_tick     | unsigned      | 4            | Maximum tick number before termination. |
| snake_basics | snake_basic[] | 24 * players | Final snake states.                     |
Max size = 384

### snake_basic
| Field       | Type     | Size | Description                                                    |
|-------------|----------|------|----------------------------------------------------------------|
| speed       | float    | 4    | Speed.                                                         |
| angle       | float    | 4    | Angle.                                                         |
| width       | float    | 4    | Radius of each segment.                                        |
| length      | unsigned | 4    | Number of segments.                                            |
| score       | unsigned | 4    | Score.                                                         |
| boost       | unsigned | 1    | Remaining ticks in boost mode.                                 |
| status      | unsigned | 1    | 0: alive, 1: killed by snake, 2: killed by wall                |
| status_data | unsigned | 1    | If killed by snake, the player ID of the killer. Otherwise, 0. |
| human       | bool     | 1    | Whether the snake is human.                                    |
Size = 24

### snake
| Field       | Type             | Size       | Description                  |
|-------------|------------------|------------|------------------------------|
| snake_basic | snake_basic      | 24         | See above.                   |
| segments    | (float, float)[] | 8 * length | Coordinates of each segment. |
Max size = 8224 (~8KB)

### food
| Field | Type           | Size | Description                                       |
|-------|----------------|------|---------------------------------------------------|
| pos   | (float, float) | 8    | Coordinates of food.                              |
| width | float          | 4    | Radius of food. Round down to obtain score boost. |
Size = 12

# Encryption and Decryption
All packets between client and server are encrypted using ChaCha20-poly1305
([RFC 8439](https://www.rfc-editor.org/rfc/rfc8439.html)).
## Format of Encrypted Transport Packets (Chunks)
| Field        | Type     | Size     | Description                                   |
|--------------|----------|----------|-----------------------------------------------|
| session_id   | unsigned | 4        | Session ID.                                   |
| player_id    | unsigned | 4        | Player ID.                                    |
| sender       | unsigned | 1        | Client: 0; Server: 1.                         |
| total_chunks | unsigned | 1        | Total number of chunks in the logical packet. |
| chunk_id     | unsigned | 1        | The number of chunks preceding this chunk.    |
| padding      | -        | 1        | Padding.                                      |
| nonce_part   | unsigned | 4        | Last 4 bytes of nonce.                        |
| ciphertext   | byte[]   | variable | Ciphertext.                                   |
| tag          | byte[16] | 16       | Tag.                                          |
- The nonce is formed by bytes 5-16 (inclusive, 1-based).
- When sender is 0 (the chunk is from client), nonce_part is implementation-defined.
Only uniqueness (w.r.t. session and player) of the nonce needs to be guaranteed,
so clients can use any method to generate nonce_part as long as it ensures uniqueness.
- When sender is 1 (the chunk is from server), nonce_part is the (authoritative) tick number.
  - As only one logical packet is sent per tick per player, this is sufficient to ensure uniqueness.
- The first 16 bytes form the additional authenticated data (AAD).
- Size of ciphertext must be a multiple of 16, with padding if necessary.
  - Server must check that the ciphertext size is divisible by 16 before decryption.
  - Note: This requirement is non-standard but is added for ease of implementation.
- Only the ciphertext is encrypted. AAD and tag are not encrypted.