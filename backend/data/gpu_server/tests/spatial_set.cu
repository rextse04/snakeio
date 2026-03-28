#include <tests/spatial_set.hpp>
#include <spatial_set_iterator.hpp>
#include <vector.hpp>
#include <cuda/std/array>
#include <thrust/sort.h>
#include <thrust/execution_policy.h>

namespace snakeio::test::spatial_set {
    using config = spatial_set_default_config<cell_length>;
    namespace {
        struct spatial_set {
            size_t size = 0;
            cuda::std::array<vector2d, objs_size> objs;
        };
    }

    namespace {
        __global__ void init_(spatial_set* out) {
            new(out) spatial_set;
        }
    }
    handle* init() {
        void* out;
        cudaMalloc(&out, sizeof(spatial_set));
        init_<<<1, 1>>>(static_cast<spatial_set*>(out));
        return static_cast<handle*>(out);
    }

    void destroy(handle* set) {
        cudaFree(set);
    }

    namespace {
        __global__ void insert(spatial_set* set, vector2d pos) {
            set->objs[set->size++] = pos;
        }
    }
    void insert(handle* set, vector2d pos) noexcept {
        insert<<<1, 1>>>(reinterpret_cast<spatial_set*>(set), pos);
    }

    namespace {
        __device__ bool comp(const vector2d& a, const vector2d& b) {
            return config::cell_id(a) < config::cell_id(b);
        }
        __global__ void refresh(spatial_set* set) {
            thrust::sort(thrust::device, set->objs.begin(), set->objs.begin() + set->size, comp);
            set->objs[set->size] = config::erase_key;
        }
    }
    void refresh(handle* set) noexcept {
        refresh<<<1, 1>>>(reinterpret_cast<spatial_set*>(set));
    }

    namespace {
        __global__ void find_(const spatial_set* set, vector2d key, scalar_t radius, spatial_set* out) {
            new(out) spatial_set;
            for (
                auto it = make_spatial_set_iterator<config>(
                    set->objs.begin(), set->objs.begin() + set->size, key, radius);
                it != std::default_sentinel; ++it) {
                if ((*it - key).norm_sq() >= radius * radius) continue;
                out->objs[out->size++] = *it;
            }
        }
    }
    std::vector<vector2d> find(const handle* set, vector2d key, scalar_t radius) noexcept {
        void* out;
        cudaMalloc(&out, sizeof(spatial_set));
        find_<<<1, 1>>>(reinterpret_cast<const spatial_set*>(set), key, radius, static_cast<spatial_set*>(out));
        spatial_set buffer;
        cudaMemcpy(&buffer, out, sizeof(spatial_set), cudaMemcpyDeviceToHost);
        cudaFree(out);
        return std::vector<vector2d>(buffer.objs.begin(), buffer.objs.begin() + buffer.size);
    }
}