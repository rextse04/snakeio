#include <gpu_server/cuda.cuh>

cuda::std::tuple<void*, void*> cudaNetworkMalloc(size_t size) {
    void *host_ptr, *device_ptr;
    cudaHostAlloc(&host_ptr, size, cudaHostAllocMapped);
    cudaHostGetDevicePointer(&device_ptr, host_ptr, 0);
    return {host_ptr, device_ptr};
}

void cudaNetworkFree(void* host_ptr) {
    cudaFreeHost(host_ptr);
}