#pragma once
#include <cuda/std/tuple>

// The host pointer may only be read by POSIX networking functions.
// Must be freed with cudaNetworkFree.
// Returns: (host pointer, device pointer)
cuda::std::tuple<void*, void*> cudaNetworkMalloc(size_t size);

// host_ptr must be produced by cudaNetworkMalloc.
void cudaNetworkFree(void* host_ptr);