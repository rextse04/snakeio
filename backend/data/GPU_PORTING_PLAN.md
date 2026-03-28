# CUDA GPU Acceleration Plan for snakeio Multiplayer Game Server

## Executive Summary

Port the CPU game server to GPU by identifying compute-heavy components (AI behavior, collision detection, spatial queries) that can be parallelized across 4096 sessions and their 16 players. Implement a hybrid architecture where network I/O and encryption remain on CPU, while game logic (physics, collisions, AI) runs on GPU. Use CUDA streams for concurrent session processing, unified memory for data transfer, and careful memory layout optimization to maximize parallelism within the 20ms tick budget.

---

## 1. Current Architecture Analysis

### Game Loop Structure
```
cpu_server/game.cpp - Main game loop processing:
├── tick 0: Lobby phase (wait for all players, snapshot)
├── tick 1+: Game phase
│   ├── Process human inputs from network packets
│   ├── Generate AI player commands (force calculations)
│   ├── Update snake physics (movement, angle, boost)
│   ├── Collision detection (walls, snakes, food)
│   ├── Serialization (delta/snapshot packets)
│   └── Send to clients
└── tick N: Termination (game end)
```

### Key Parameters (from config.hpp)
- **Max Sessions**: 4096 (game_max_sessions)
- **Max Players/Session**: 16 (game_max_players)
- **Max Snake Segments**: 1024 (snake_max_length)
- **Max Food/Session**: 2048 (game_max_food_pp * game_max_players)
- **Tick Rate**: 50 Hz (20ms per tick = game_tick_rate)
- **Max Ticks**: 9000 (300 sec / 20ms = game_max_tick)

### Parallelization Opportunity
```
4096 sessions × 16 players = 65,536 potential GPU threads/warp groups
Each player update includes:
  - AI behavior: 1000-5000 floating-point operations
  - Collision detection: O(segments × nearby_objects)
  - Physics: 10-50 operations
```

---

## 2. Compute Bottleneck Analysis

### High-Impact GPU Candidates (Prioritized)

| Component | CPU Cost | Parallelism | GPU Benefit |
|-----------|----------|------------|------------|
| **AI behavior** (force fields) | O(players × sight_range_objects^2) | Per-player | **⭐⭐⭐⭐⭐** |
| **Collision detection** | O(snakes × nearby_objects^2) | Per-segment | **⭐⭐⭐⭐⭐** |
| **Snake movement** | O(players × snake_length) | Per-player | **⭐⭐⭐** |
| **Food generation** | O(poisson_sample × spatial_insert) | Per-session | **⭐⭐** |
| **Serialization** | O(snakes + food) | Per-player (for send) | **⭐** |

### Low-Impact GPU Candidates (CPU-Bound)
- **Network I/O** (recvfrom/sendto): Limited by network latency, not CPU
- **Encryption** (ChaCha20): Fast on CPU, offloading overhead not worth unless latency critical
- **Session management** (allocation/deallocation): Rare, not worth GPU overhead

### Estimated Speedup Potential
- **Without GPU optimization**: ~10-20ms per tick at max sessions (margin acceptable)
- **With GPU (collision + physics + AI)**: ~2-5ms per tick (3-10x speedup possible)
- **Network I/O remains**: ~5-10ms per tick (bottleneck, not GPU-solvable)

---

## 3. GPU Memory Hierarchy & Data Layout Redesign

### Current CPU Data Layout Problem
```cpp
// CPU (tree-based, pointer-heavy, non-coalesced)
struct session {
    std::array<snake, 16> snakes;  // Each snake: 1024 * vector2d + metadata
    cpu::spatial_set snakes_set;   // Hierarchical grid with pointers
    cpu::spatial_set food_set;     // Hierarchical grid with pointers
};
```

**Issues for GPU**:
- Pointer-based spatial grid → non-coalesced memory access
- Per-snake separate segment arrays → bad memory locality
- Hierarchical structure → branch divergence

### Proposed GPU Data Layout

```cpp
// GPU (flat, SoA, coalesced-friendly)
struct gpu_session_batch {
    // Snake data (flat SoA layout)
    float* speed;              // [num_snakes]
    float* angle;              // [num_snakes]
    float* width;              // [num_snakes]
    float* frac_length;        // [num_snakes]
    uint32_t* score;           // [num_snakes]
    uint8_t* boost;            // [num_snakes]
    uint8_t* status;           // [num_snakes]
    
    // Snake segments (flat array, packed)
    float2* segments;          // [num_snakes * max_segments]
    uint16_t* segment_count;   // [num_snakes] - actual segment count per snake
    
    // Spatial grid (hash grid or grid cells)
    uint32_t* spatial_grid;    // [grid_cells] - start indices in spatial_objects
    uint16_t* grid_cell_counts;// [grid_cells] - object count per cell
    
    // Spatial object references (flattened snake/food references)
    uint32_t* spatial_objects; // [max_objects_per_session]
    
    // Food data (flat SoA)
    float2* food_pos;          // [num_food]
    float* food_width;         // [num_food]
    uint8_t* food_valid;       // [num_food] - for deletion
    
    // Session metadata
    float width, height;
    uint32_t num_snakes, num_food;
    uint32_t* player_inputs;   // [16] - input buffer handles
};

// Batch multiple sessions for efficiency
struct gpu_session_batch_collection {
    std::vector<gpu_session_batch> sessions;  // 4096 sessions
    // Allocate all on GPU device memory
    cudaDeviceMemory device_memory;
};
```

### Memory Allocation Strategy
```
Total VRAM required:
  4096 sessions × (
    16 snakes × 1024 segments × 8 bytes (float2) +  // 128 KB per session
    2048 food × 12 bytes +                           // 24 KB per session
    spatial_grid overhead ~50 KB +                   // Depends on grid resolution
    metadata ~2 KB
  ) ≈ 200-300 MB

→ Easily fits on modern GPUs (2-6 GB typical)
```

---

## 4. Spatial Index GPU Implementation

### Replace CPU `spatial_set` with Hash Grid

#### CPU Version Problem
```cpp
// Hierarchical, recursive queries, branch divergence
for (const auto [snake, seg] : session.snakes_set.find_possible(pos, range)) {
    // Find all snakes/segments within range
}
```

#### GPU Hash Grid Approach
```cpp
// 1. Build spatial index (pre-collision detection kernel)
__global__ void build_spatial_index(
    const float2* segments,           // All segment positions
    const uint16_t* segment_counts,   // Per-snake segment count
    uint32_t num_snakes,
    uint32_t* grid_cells,             // Output: cell → object mapping
    uint16_t* grid_counts
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= total_segments) return;
    
    // Determine which snake and segment this is
    int snake_id = binary_search(segment_counts, idx);
    int seg_local = idx - prefix_sum[snake_id];
    
    float2 pos = segments[idx];
    uint32_t cell = hash_grid_cell(pos);
    
    // Atomic add to grid cell
    uint16_t slot = atomicAdd(&grid_counts[cell], 1);
    grid_cells[cell * MAX_OBJS_PER_CELL + slot] = idx;
}

// 2. Query spatial index (during collision detection kernel)
__device__ void query_nearby_objects(
    float2 center, float range,
    const uint32_t* grid_cells,
    const uint16_t* grid_counts,
    // Process nearby objects in parallel
) {
    // Query hash grid cells overlapping with [center - range, center + range]
    // Load from grid_cells and process in parallel
}
```

---

## 5. Core CUDA Kernel Design

### Kernel 1: AI Behavior (Force Field Calculation)

```cpp
__global__ void kernel_ai_behavior(
    // Input: current snake states
    const float* angles,           // [num_snakes]
    const float2* snake_heads,     // [num_snakes] - head position
    const float* snake_speeds,     // [num_snakes]
    const float* snake_widths,     // [num_snakes]
    
    // Spatial data
    const uint32_t* grid_cells,
    const uint16_t* grid_counts,
    const float2* food_pos,
    const float* food_width,
    
    // Output: AI commands (angle, boost)
    float* out_angles,             // [num_ai_snakes]
    uint8_t* out_boosts            // [num_ai_snakes]
) {
    // Per-AI-player thread
    int player_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (player_id >= num_ai_players) return;
    
    float2 head = snake_heads[player_id];
    float sight_range = snake_speeds[player_id] * 32;
    
    // Forward bias
    float2 forward_force = {cosf(angles[player_id]), sinf(angles[player_id])};
    
    // Food attraction via spatial query
    float2 food_force = {0, 0};
    for (int cell_idx : query_nearby_cells(head, sight_range)) {
        for (int obj : grid_cells[cell_idx]) {
            float2 food_delta = food_pos[obj] - head;
            float dist_sq = dot(food_delta, food_delta);
            food_force += food_delta / (dist_sq + epsilon) * food_width[obj];
        }
    }
    
    // Collision repulsion (similar spatial query)
    float2 collision_force = {0, 0};
    // ... (similar to food attraction)
    
    // Wall repulsion
    float2 wall_force = {...};
    
    // Combine forces
    float2 total_force = forward_force + food_force + collision_force + wall_force;
    out_angles[player_id] = atan2f(total_force.y, total_force.x);
    out_boosts[player_id] = should_boost(total_force, collision_force, wall_force);
}
```

**Launch Configuration**:
```cpp
// One thread per AI player
// threads_per_block = 256-512 (balance occupancy vs. shared memory)
// grid_dim = (num_ai_players + threads_per_block - 1) / threads_per_block
```

### Kernel 2: Snake Movement & Physics

```cpp
__global__ void kernel_snake_movement(
    // Input: player inputs & current state
    const float* in_angles,        // [num_snakes] - desired angles
    const uint8_t* in_boosts,      // [num_snakes]
    float* angles,                 // [num_snakes] - current angles
    float* speeds,                 // [num_snakes]
    float* widths,                 // [num_snakes]
    float* frac_lengths,           // [num_snakes]
    uint8_t* boosts,               // [num_snakes]
    
    // Segment data (circular buffer)
    float2* segments,              // [num_snakes * max_segments]
    uint16_t* segment_counts,      // [num_snakes]
    
    // Constants
    float session_width, float session_height,
    
    // Output: updated state
    float2* out_segments,
    uint16_t* out_segment_counts
) {
    int player_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (player_id >= num_players) return;
    
    // Update angle with clamping
    float new_angle = angles[player_id] + 
        clamp(angle_diff(in_angles[player_id], angles[player_id]),
              -MAX_TURN_PER_TICK, MAX_TURN_PER_TICK);
    
    // Update boost
    uint8_t new_boost = max(0, boosts[player_id] - 1);
    if (in_boosts[player_id] && frac_lengths[player_id] > INIT_LENGTH) {
        new_boost += BOOST_TICKS;
    }
    
    // Sync dimensions based on snake length
    float progress = (frac_lengths[player_id] - INIT_LENGTH) / (MAX_LENGTH - INIT_LENGTH);
    float scaled = 1.0f - (1.0f - progress) * (1.0f - progress); // ease out quad
    float new_speed = INIT_SPEED + (MIN_SPEED - INIT_SPEED) * scaled;
    new_speed *= (1.0f + new_boost * (BOOST_FACTOR - 1.0f));
    float new_width = INIT_WIDTH + (MAX_WIDTH - INIT_WIDTH) * scaled;
    
    // Move snake: shift segments and update head
    uint16_t seg_count = segment_counts[player_id];
    float2 head = segments[player_id * MAX_SEGMENTS];
    float2 new_head = head + (float2){cosf(new_angle) * new_speed, sinf(new_angle) * new_speed};
    
    // Shift segments right
    for (int i = seg_count - 1; i > 0; --i) {
        segments[player_id * MAX_SEGMENTS + i] = segments[player_id * MAX_SEGMENTS + i-1];
    }
    segments[player_id * MAX_SEGMENTS] = new_head;
    
    // Update state
    angles[player_id] = new_angle;
    speeds[player_id] = new_speed;
    widths[player_id] = new_width;
    boosts[player_id] = new_boost;
}
```

### Kernel 3: Collision Detection

```cpp
__global__ void kernel_collision_detection(
    // Snake segments
    const float2* segments,
    const uint16_t* segment_counts,
    const float* snake_widths,
    uint8_t* snake_status,  // alive / killed_by_wall / killed_by_snake
    
    // Grid for spatial queries
    const uint32_t* grid_cells,
    const uint16_t* grid_counts,
    
    // Bounds
    float session_width, float session_height,
    
    // Output: snakes to erase, killed counts, etc.
    uint32_t* erased_snakes,  // [num_snakes] - death reason flags
    uint32_t* food_generated   // [num_snakes] - count of food from dead snakes
) {
    int player_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (player_id >= num_players) return;
    
    if (snake_status[player_id] != ALIVE) return;
    
    float2 head = segments[player_id * MAX_SEGMENTS];
    float width = snake_widths[player_id];
    
    // Check wall collision
    if (head.x < width || head.x > session_width - width ||
        head.y < width || head.y > session_height - width) {
        erased_snakes[player_id] = KILLED_BY_WALL;
        return;
    }
    
    // Check collision with other snakes via spatial query
    for (int cell_idx : query_nearby_cells(head, MAX_SNAKE_WIDTH * 2)) {
        for (int seg_idx : grid_cells[cell_idx]) {
            // Determine which snake owns seg_idx
            int other_player = binary_search_segment_owner(seg_idx);
            if (other_player == player_id) continue;
            
            float2 other_seg = load_segment(other_player, seg_idx);
            float dist_sq = distance_squared(head, other_seg);
            float min_dist = width + snake_widths[other_player];
            
            if (dist_sq < min_dist * min_dist) {
                erased_snakes[player_id] = KILLED_BY_SNAKE;
                // Also kill the other snake if hit head
                if (is_head_segment(other_player, seg_idx)) {
                    erased_snakes[other_player] = KILLED_BY_SNAKE;
                }
                return;
            }
        }
    }
}
```

### Kernel 4: Food Consumption & Generation

```cpp
__global__ void kernel_food_interaction(
    // Snake state
    const float2* snake_heads,
    const float* snake_widths,
    float* frac_lengths,
    uint32_t* scores,
    uint8_t* snake_alive,
    
    // Food state
    float2* food_pos,
    float* food_width,
    uint8_t* food_valid,
    
    // Grid
    const uint32_t* grid_cells,
    const uint16_t* grid_counts,
    
    // PRNG state (per-thread)
    uint32_t* prng_states,
    
    // Output
    uint32_t* food_to_generate,  // [num_sessions] - count
    float2* generated_food_pos,  // [max_new_food]
    float* generated_food_width,
    uint32_t* generated_food_count
) {
    int player_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (player_id >= num_players || !snake_alive[player_id]) return;
    
    float2 head = snake_heads[player_id];
    float width = snake_widths[player_id];
    
    // Query food in range and consume
    float2 new_length = frac_lengths[player_id];
    for (int food_idx : query_food_nearby(head, MAX_SNAKE_WIDTH + FOOD_MAX_WIDTH)) {
        if (!food_valid[food_idx]) continue;
        
        float2 food_delta = food_pos[food_idx] - head;
        float dist_sq = dot(food_delta, food_delta);
        float min_dist = width + food_width[food_idx];
        
        if (dist_sq < min_dist * min_dist) {
            scores[player_id] += (uint32_t)food_width[food_idx];
            new_length = min(MAX_LENGTH, 
                new_length + food_width[food_idx] * FOOD_WIDTH_TO_SEG);
            
            // Mark food as eaten
            atomicExch(&food_valid[food_idx], 0);
        }
    }
    
    frac_lengths[player_id] = new_length;
    
    // Food generation (Poisson distribution)
    // Typically done on CPU with one kernel call to atomically update count
}
```

---

## 6. Hybrid CPU-GPU Pipeline

### Execution Model
```
                       CPU Thread                          GPU
    ┌─────────────────────────────────┐
    │ T-1: Receive network packets    │
    │      Decrypt input packets      │
    │      Update client.last_packet  │
    └──────────────┬──────────────────┘
                   │
    ┌──────────────▼──────────────────────────────────────┐
    │ T-2: Copy input data to GPU (async)               │
    │      game_loop iteration for tick N               │
    └──────────────┬──────────────────────────────────────┘
                   │
                   │                ┌────────────────────┐
                   │                │ Build spatial idx  │
                   │                └────────┬───────────┘
                   │                         │
                   │                ┌────────▼───────────┐
                   │                │ AI behavior kernel │
                   │                │ Snake move kernel  │
                   │                │ Collision kernel   │
                   │                └────────┬───────────┘
                   │                         │
    ┌──────────────┼─────────────────────────┼──────────┐
    │ T-3: Serialize & send packets         │(blocking)│
    │      (may wait for GPU async D2H)     │          │
    └──────────────┴─────────────────────────┴──────────┘
                   │
                   ▼
            Next tick (T+1)
```

### Code Pseudocode
```cpp
void gpu_game_loop(game& game, cudaStream_t compute_stream, cudaStream_t transfer_stream) {
    std::byte delta_text[delta_packet_max_text_size];
    std::byte* gpu_delta_text = nullptr;
    cudaMalloc(&gpu_delta_text, delta_packet_max_text_size);
    
    while (!stop_token.stop_requested()) {
        // 1. Prepare input data on CPU side (from network packets)
        for (id_t i = 0; i < game_max_sessions; ++i) {
            if (!game.sm_[i]) continue;
            // Copy session state to GPU (async)
            cudaMemcpyAsync(gpu_session[i], cpu_session[i], sizeof(session), 
                           cudaMemcpyHostToDevice, transfer_stream);
        }
        
        // 2. Launch GPU kernels (on compute_stream)
        cudaStreamSynchronize(transfer_stream);  // Wait for data transfer
        
        // Build spatial indices
        kernel_build_spatial_index<<<...>>>(compute_stream);
        
        // AI behavior
        kernel_ai_behavior<<<...>>>(compute_stream);
        
        // Snake movement
        kernel_snake_movement<<<...>>>(compute_stream);
        
        // Collision detection
        kernel_collision_detection<<<...>>>(compute_stream);
        
        // Food interaction
        kernel_food_interaction<<<...>>>(compute_stream);
        
        // 3. Copy results back (async)
        cudaMemcpyAsync(cpu_session_results, gpu_session_results, ...,
                       cudaMemcpyDeviceToHost, transfer_stream);
        
        // 4. Serialize and send (while GPU computes next batch)
        // Can use cudaStreamSynchronize(transfer_stream) when needed
        
        // 5. Sleep until next tick
        next_tick += game_tick_rate;
        std::this_thread::sleep_until(next_tick);
    }
}
```

---

## 7. Incremental Implementation Phases

### Phase 1: GPU Collision Detection Only (Weeks 1-2)
**Goal**: Validate GPU kernel infrastructure and verify correctness.
- [x] Set up CUDA build system (CMakeLists.txt integration)
- [x] Implement `kernel_build_spatial_index`
- [x] Implement `kernel_collision_detection`
- [ ] Create unit tests comparing GPU vs. CPU collision results
- [ ] Benchmark: measure speedup on collision detection alone

**Deliverable**: `gpu_collision.cu` with unit tests passing.

### Phase 2: Snake Movement & Physics (Weeks 2-3)
**Goal**: Extend GPU pipeline to include physics updates.
- [ ] Implement `kernel_snake_movement`
- [ ] Implement segment shifting and angle updates on GPU
- [ ] Dimension sync (speed/width based on length)
- [ ] Integration tests: movement matches CPU version

**Deliverable**: `gpu_physics.cu` with integrated pipeline.

### Phase 3: AI Behavior (Weeks 3-4)
**Goal**: Add AI force field calculations to GPU.
- [ ] Implement `kernel_ai_behavior` with spatial queries
- [ ] Implement force field calculations (food attraction, collision repulsion, wall repulsion)
- [ ] Test AI behavior determinism (may require fixed seed control)

**Deliverable**: `gpu_ai.cu` with determinism validation.

### Phase 4: Food Generation & Consumption (Weeks 4-5)
**Goal**: Complete game loop on GPU.
- [ ] Implement `kernel_food_interaction` (consumption, scoring)
- [ ] Implement Poisson food generation (CPU-side, GPU-side, or hybrid)
- [ ] Handle food array compaction (mark-and-sweep on GPU)

**Deliverable**: `gpu_food.cu` with full game loop.

### Phase 5: Optimization & Profiling (Weeks 5-6)
**Goal**: Maximize GPU utilization and reduce latency.
- [ ] Profile kernel execution times
- [ ] Optimize thread block sizes, register pressure, shared memory usage
- [ ] Implement dynamic batch sizing based on active sessions
- [ ] Consider kernel fusion (e.g., merge movement + collision)

**Deliverable**: Performance report with speedup numbers.

### Phase 6: Advanced Optimization (Weeks 6-8)
**Goal**: Production-ready GPU server.
- [ ] Implement CUDA unified memory (if beneficial)
- [ ] Add error handling and fallback to CPU on GPU errors
- [ ] Implement session batching and dual-execution (GPU + CPU validation)
- [ ] Optimize for multi-GPU scenarios (if applicable)

**Deliverable**: Production-ready `gpu_server/` directory with full feature parity.

---

## 8. Data Structure Optimization: CPU `spatial_set` → GPU Hash Grid

### CPU `spatial_set` Problem
```cpp
// Recursive tree structure, pointer-heavy, branch divergence
template <scalar_t CellLength, size_t ObjsSize, typename Node, auto GetPos, auto SetPos>
class spatial_set {
    std::array<std::vector<value_type*>, cells> grid;  // Pointers → bad for GPU
    // Iterator API with ranges, complex state management
};
```

### GPU Hash Grid Solution
```cpp
// Flat array, atomic updates, coalesced memory
__host__ __device__ uint32_t spatial_hash(float2 pos, float grid_cell_size) {
    uint32_t x = (uint32_t)(pos.x / grid_cell_size);
    uint32_t y = (uint32_t)(pos.y / grid_cell_size);
    return (x * 73856093) ^ (y * 19349663);  // Hash function (Morton code variant)
}

__global__ void build_spatial_grid(
    const float2* positions,
    uint32_t count,
    uint32_t* grid_cells,           // [cell_id] → list start
    uint16_t* grid_counts,          // [cell_id] → count
    uint32_t* spatial_objects,      // [sorted indices]
    float grid_cell_size
) {
    // 1. Compute hash for each object (parallel)
    // 2. Sort by hash (CUB::DeviceRadixSort)
    // 3. Build grid with run-length encoding
}

__device__ inline void query_spatial_grid(
    float2 center, float range,
    const uint32_t* grid_cells,
    const uint16_t* grid_counts,
    const float2* positions,
    float grid_cell_size,
    // Process each nearby object
    auto callback
) {
    // Iterate through all grid cells overlapping [center-range, center+range]
    for (int cx = ...; cx < ...; ++cx) {
        for (int cy = ...; cy < ...; ++cy) {
            uint32_t cell = spatial_hash({cx * grid_cell_size, cy * grid_cell_size}, grid_cell_size);
            uint16_t count = grid_counts[cell];
            for (uint16_t i = 0; i < count; ++i) {
                uint32_t obj_idx = spatial_objects[grid_cells[cell] + i];
                callback(obj_idx, positions[obj_idx]);
            }
        }
    }
}
```

---

## 9. Synchronization & Per-Tick Barriers

### CPU-GPU Synchronization Points
```cpp
struct gpu_sync_points {
    cudaEvent_t spatial_index_done;        // After building spatial index
    cudaEvent_t physics_done;              // After movement/collision/AI
    cudaEvent_t serialization_ready;       // After final result copy to host
};

void gpu_game_loop(...) {
    for (tick_t tick = 0; tick < max_tick; ++tick) {
        // ===== Stage 1: Input preparation (CPU) =====
        prepare_player_inputs_cpu();  // From network packets
        
        // ===== Stage 2: GPU compute (overlapped with transfer) =====
        cudaMemcpyAsync(..., cudaMemcpyHostToDevice, ...);
        kernel_build_spatial_index<<<...>>>();
        cudaEventRecord(gpu_sync_points.spatial_index_done);
        
        // ===== Stage 3: Physics update (GPU) =====
        kernel_snake_movement<<<...>>>();
        kernel_collision_detection<<<...>>>();
        kernel_ai_behavior<<<...>>>();
        cudaEventRecord(gpu_sync_points.physics_done);
        
        // ===== Stage 4: Result retrieval (async copy back) =====
        cudaMemcpyAsync(..., cudaMemcpyDeviceToHost, ...);
        
        // ===== Stage 5: Serialization (CPU, overlapped) =====
        cudaEventSynchronize(gpu_sync_points.serialization_ready);
        serialize_and_send_packets();
        
        // ===== Stage 6: Timing =====
        next_tick += game_tick_rate;
        std::this_thread::sleep_until(next_tick);
    }
}
```

### Determinism & Reproducibility
```cpp
// Per-session deterministic PRNG seed
__global__ void init_prng_seeds(uint32_t* prng_states, uint32_t session_id, uint32_t tick) {
    int thread_id = blockIdx.x * blockDim.x + threadIdx.x;
    // Seed deterministically from session_id + tick
    prng_states[thread_id] = hash(session_id, tick, thread_id);
}

// Use deterministic float math (optional, may limit optimization)
// Alternatively: accept ±epsilon difference in physics results
```

---

## 10. Potential Bottlenecks & Solutions

### Bottleneck 1: Network I/O
**Problem**: With 4096 sessions × 16 players, serialization and sendto() calls dominate wall time (may be 5-10ms/tick).

**Solutions**:
- Keep GPU compute to <5ms/tick (3x speedup from baseline 15ms)
- Batch network sends (group multiple players' packets, send with single syscall)
- Use UDP GSO (Generic Segmentation Offload) if available on platform
- Consider in-kernel serialization on GPU to avoid D2H transfers (ambitious)

### Bottleneck 2: GPU Memory Transfer
**Problem**: Large D2H copies after each tick may stall compute pipeline.

**Solutions**:
- Use CUDA streams to pipeline overlapping transfers
- Implement ring buffer to avoid synchronization on every tick
- Use unified memory with `cudaMemAdvise(CUDA_MEM_ADVISE_SET_READ_MOSTLY)` for read-heavy data
- Selectively copy only changed sessions (delta updates)

### Bottleneck 3: Kernel Launch Overhead
**Problem**: Many small kernels (5-6 per tick) add launch overhead (~microseconds × 4096 sessions).

**Solutions**:
- Implement kernel fusion (combine multiple phases into one kernel with multiple phases)
- Use dynamic parallelism (GPU launching sub-kernels)
- Reduce number of kernel launches by grouping logic

### Bottleneck 4: Warp Divergence
**Problem**: AI behavior has many conditional branches (food vs. wall vs. collision), causing divergence.

**Solutions**:
- Use warp-level primitives (__ballot, __shfl) to reduce divergence
- Separate AI logic into branches by condition type (if profiling shows divergence cost)
- Consider data layout to minimize branch frequency

### Bottleneck 5: Spatial Grid Hash Collisions
**Problem**: Many objects hashing to same grid cell cause long linked lists, slowing queries.

**Solutions**:
- Use cuckoo hashing or secondary hash function
- Increase grid resolution (finer cells)
- Implement BVH (Bounding Volume Hierarchy) instead of simple hash grid
- Use CUB's spatial partitioning library

---

## 11. Determinism & Correctness Validation

### Offline Replay Mode
```cpp
// Enable with compile-time flag or runtime option
#define GPU_VALIDATION_MODE 1

void validate_gpu_results() {
    // 1. Run CPU simulation on saved input sequence
    cpu_session cpu_result = cpu_game_loop(inputs);
    
    // 2. Run GPU simulation on same inputs
    gpu_session gpu_result = gpu_game_loop(inputs);
    
    // 3. Compare with epsilon tolerance for floats
    for (size_t i = 0; i < num_snakes; ++i) {
        assert_near(cpu_result.snakes[i].angle, gpu_result.snakes[i].angle, 1e-5f);
        assert_near(cpu_result.snakes[i].score, gpu_result.snakes[i].score, 0);  // Exact
        // ... compare other fields
    }
}
```

### Unit Tests
- Collision detection: unit test grid collisions in isolation
- AI behavior: unit test force calculations
- Physics: unit test snake movement with known inputs
- Floating-point: document acceptable epsilon for FP differences

---

## 12. Testing & Validation Strategy

### 1. Unit Tests
```cpp
TEST(GpuCollision, WallCollisionDetection) {
    // Create 1 snake near wall
    // Copy to GPU
    // Launch collision kernel
    // Assert snake marked as killed_by_wall
}

TEST(GpuAI, FoodAttraction) {
    // Create AI snake with food nearby
    // Launch AI kernel
    // Assert force points toward food
}
```

### 2. Integration Tests
```cpp
TEST(GpuIntegration, FullTickSimulation) {
    // Create full session with players and food
    // Run GPU simulation for N ticks
    // Compare with CPU reference implementation
}
```

### 3. Stress Tests
```cpp
TEST(GpuStress, MaxSessions4096) {
    // Create 4096 sessions with 16 players each
    // Run for 100 ticks
    // Measure latency, memory usage, correctness
}

TEST(GpuStress, HighFoodDensity) {
    // Test with max food (2048/session)
    // Verify no buffer overflows or crashes
}
```

### 4. Correctness Validation (Offline)
```
Create deterministic replay logs:
  1. Record all player inputs for a full game (9000 ticks)
  2. Run CPU simulation → log all state changes
  3. Run GPU simulation → log all state changes
  4. Bitwise compare (or epsilon-compare for FP)
  5. If mismatch, save divergence point for debugging
```

---

## 13. Configuration & Build System

### CMakeLists.txt Integration

```cmake
# backend/data/CMakeLists.txt

option(SNAKEIO_GPU_ENABLED "Enable CUDA GPU acceleration" ON)

if (SNAKEIO_GPU_ENABLED)
    enable_language(CUDA)

    add_library(snakeio_gpu
            cpu_server/game.cpp
            gpu_server/game.cpp          # New GPU implementation
            gpu_server/kernels.cu       # Collision, movement, AI, etc.
            gpu_server/spatial_index.cu # GPU spatial grid
    )

    target_compile_options(snakeio_gpu PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-O3 -arch=sm_70>  # Adjust for target GPU
    )

    target_link_libraries(snakeio_gpu PUBLIC
            cudart
            cub  # NVIDIA CUB for primitives
    )

    # Validation mode for correctness testing
    target_compile_definitions(snakeio_gpu PRIVATE
            $<$<CONFIG:Debug>:GPU_VALIDATION_MODE=1>
    )
else ()
    add_library(snakeio_gpu cpu_server/game.cpp)
endif ()
```

---

## 14. Deployment & Rollout Strategy

### Phase 1: Development & Validation (Local)
- Build with `GPU_ENABLED=ON`
- Run offline correctness validation
- Benchmark on target GPU hardware

### Phase 2: Staged Rollout (Production)
```cpp
// Feature flag: route subset of sessions to GPU
bool use_gpu = (session_id % 10) < 3;  // 30% GPU, 70% CPU initially
if (use_gpu) {
    gpu_game_loop(...);
} else {
    cpu_game_loop(...);
}
```

### Phase 3: Monitor & Adjust
- Monitor latency, error rates, memory usage
- Gradually increase GPU traffic if stable
- Fallback to CPU on GPU errors

### Phase 4: Full Migration
- Route all new sessions to GPU
- Keep CPU as warm standby
- Decommission CPU path (optional) after 30+ days stable

---

## 15. Performance Targets

| Metric | CPU Baseline | GPU Target | Improvement |
|--------|-------------|-----------|------------|
| Per-tick latency (4096 sessions) | ~15-20ms | ~3-5ms | **3-6x** |
| Collision detection | O(n²) | O(n log n) via spatial grid | **2-10x** |
| AI behavior calculation | ~8ms | ~2ms (parallel FP) | **4x** |
| Snake movement | ~2ms | <1ms | **2x** |
| Memory usage | ~300-500 MB | ~200-300 MB (GPU) | **20% less** |

---

## Summary: Action Items

1. **Week 1**: Set up CUDA build system, implement spatial index (hash grid)
2. **Week 2**: Implement collision & physics kernels, validate correctness
3. **Week 3**: Add AI behavior, test determinism
4. **Week 4**: Add food generation, run stress tests
5. **Week 5**: Profile & optimize bottlenecks
6. **Week 6**: Implement fallback paths, error handling
7. **Week 7-8**: Staged rollout, monitoring, deployment

---

**Questions for Implementation**:
- What GPU target (V100, A100, RTX)?
- Do you need deterministic float math or accept ±epsilon?
- Can network I/O be batched/optimized separately?
- Will this run on multi-GPU cluster or single GPU?

