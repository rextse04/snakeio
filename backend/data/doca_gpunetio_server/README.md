# DOCA GPUNetIO Server Module

This directory contains the DOCA-oriented server variant for the data plane.
It reuses the existing GPU game simulation path and introduces a transport
layer that can switch between:

- `doca_gpunetio`: real DOCA GPUNetIO initialization path (default)

The goal is to keep game logic stable while transport implementation evolves
from host UDP to GPUNetIO with minimal churn.

## What this module owns

- Lifecycle of the DOCA transport context (`start` / `stop`)
- Backend selection (`config::backend`)
- RX/TX worker lifecycle entry points for future persistent kernels
- Integration of transport with the GPU game implementation in `game.cpp`

## High-level architecture

1. `game` constructs GPU game state and starts the transport context.
2. `init_transport(...)` configures transport readiness based on backend.
3. RX/TX worker start hooks are invoked (`start_rx_worker`, `start_tx_worker`).
4. `game::port(...)` receives ingress packets and applies them to GPU state.
5. `game::tick(...)` advances simulation and flushes egress packets.
6. `stop(...)` tears down workers and transport resources.

## Current behavior

- Build is gated by `SNAKEIO_ENABLE_DOCA_GPUNETIO`.
- Default backend is `doca_gpunetio`.
- DOCA init creates GPU handle, discovers a GPU-capable NIC, creates RX/TX ETH
  queues, binds contexts to GPU datapath, and acquires GPU queue handles.
- Startup is strict: if DOCA SDK/runtime/device requirements are not met,
  initialization fails and process startup aborts (no automatic shim fallback).
- RX/TX GPU files still contain kernel stubs and worker lifecycle placeholders.
- The module links against existing `gpu_server_logic_lib` to reuse simulation.

## File map

- `doca_gpunetio_server.hpp`, `doca_gpunetio_server.cpp`
  - Public lifecycle API and top-level transport orchestration.
- `doca_context.hpp`, `doca_context.cpp`
  - Transport context structure and backend-dependent init/shutdown.
- `rx_gpu.cuh`, `rx_gpu.cu`
  - RX worker lifecycle interface and kernel stubs.
- `tx_gpu.cuh`, `tx_gpu.cu`
  - TX worker lifecycle interface and kernel stubs.
- `transport_shim.hpp`, `transport_shim.cpp`
  - Temporary host UDP shim functions for transport-style packet ingress/egress.
- `game.cpp`
  - DOCA server variant of the game integration (port + tick + lifecycle hookup).

## Build and run tests

```bash
cmake -S /home/lix1/Documents/snakeio/backend/data -B /home/lix1/Documents/snakeio/backend/data/cmake-build-debug -DSNAKEIO_ENABLE_DOCA_GPUNETIO=ON
cmake --build /home/lix1/Documents/snakeio/backend/data/cmake-build-debug --target doca_gpunetio_server_o
cmake --build /home/lix1/Documents/snakeio/backend/data/cmake-build-debug --target doca_gpunetio_server_packet_io_doca_test
/home/lix1/Documents/snakeio/backend/data/cmake-build-debug/doca_gpunetio_server/doca_gpunetio_server_packet_io_doca_test
```

### Required runtime environment for DOCA backend

- `SNAKEIO_DOCA_GPU_PCI` must be set (example: `0000:65:00.0`).
- Optional: `SNAKEIO_DOCA_NIC_PCI` to pin a specific NIC PCI BDF.

## Known limitations

- Persistent GPUNetIO RX/TX kernel datapaths are still stubs.
- Ingress path in `game::port` still uses host socket receive + GPU ingest.
- Egress currently uses host socket flush in `transport_shim.cpp` while GPUNetIO TX kernels are still stubs.

## Next milestones

1. Implement real DOCA device and GPUNetIO queue setup in `init_transport(...)`.
2. Replace RX/TX worker stubs with persistent kernels and stop signaling.
3. Bind GPUNetIO queues to GPU ingress/egress ring flow.
4. Add backend-specific tests for `doca_gpunetio` mode.

