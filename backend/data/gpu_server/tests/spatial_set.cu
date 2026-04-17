#include <tests/spatial_set.hpp>
#include "../spatial_set.cuh"
#include <vector.hpp>
#include <vector>
#include <cuda/std/algorithm>
#include <cmath>

// GPU adapter for the shared backend/data/tests/spatial_set*.cpp suites.
//
// This file implements tests/spatial_set.hpp by forwarding insert/refresh/find
// operations to gpu::spatial_set_batch and copying query/result buffers across
// host/device for assertions in shared tests.

namespace snakeio::test::spatial_set {
    using spatial_set = gpu::spatial_set_batch<world_width, world_height, cell_length, objs_size, 1>;

    handle* init() {
        return reinterpret_cast<handle*>(new spatial_set);
    }
    index_array* make_index_array() {
        return nullptr;
    }

    void destroy(handle* set, index_array*) {
        delete reinterpret_cast<spatial_set*>(set);
    }

    void insert(handle* set, vector2d value) noexcept {
        insert(set, std::span(&value, 1));
    }
    namespace {
        __global__ void insert(spatial_set set, const vector2d* values, size_t size) {
            cuda::std::copy_n(values, size, set.nodes + set.end_offsets[0]);
            set.end_offsets[0] += size;
        }
    }
    void insert(handle* set, std::span<const vector2d> values) noexcept {
        vector2d* buffer;
        cudaMalloc(&buffer, sizeof(vector2d) * values.size());
        cudaMemcpy(buffer, values.data(), sizeof(vector2d) * values.size(), cudaMemcpyHostToDevice);
        insert<<<1, 1>>>(*reinterpret_cast<spatial_set*>(set), buffer, values.size());
        cudaDeviceSynchronize();
        cudaFree(buffer);
    }

    void refresh(handle* set, index_array*) noexcept {
        reinterpret_cast<spatial_set*>(set)->refresh();
    }

    std::vector<vector2d> find(const handle* set, const index_array*, const query& query) {
        return find(set, nullptr, std::span(&query, 1))[0];
    }
    namespace {
        __global__ void fill_results(const vector2d** results) {
            const size_t i = blockIdx.x * blockDim.x + threadIdx.x;
            results[i] = nullptr;
        }
        __global__ void fetch_results(const vector2d** results, vector2d* out) {
            const size_t i = blockIdx.x * blockDim.x + threadIdx.x;
            out[i] = results[i] ? *results[i] : vector2d{-1, -1};
        }
    }
    std::vector<std::vector<vector2d>> find(const handle* set, const index_array*, std::span<const query> queries) {
        auto set_ = reinterpret_cast<const spatial_set*>(set);
        std::vector<vector2d> host_keys;
        host_keys.reserve(queries.size());
        for (const query& query : queries) {
            host_keys.push_back(query.key);
        }
        std::vector<scalar_t> host_radii;
        host_radii.reserve(queries.size());
        for (const query& query : queries) {
            host_radii.push_back(query.radius);
        }
        vector2d* keys;
        cudaMalloc(&keys, sizeof(vector2d) * queries.size());
        cudaMemcpy(keys, host_keys.data(), sizeof(vector2d) * queries.size(), cudaMemcpyHostToDevice);
        scalar_t* radii;
        cudaMalloc(&radii, sizeof(scalar_t) * queries.size());
        cudaMemcpy(radii, host_radii.data(), sizeof(scalar_t) * queries.size(), cudaMemcpyHostToDevice);
        const vector2d** results;
        cudaMalloc(&results, sizeof(const vector2d*) * queries.size() * objs_size);
        fill_results<<<queries.size(), objs_size>>>(results);
        set_->find_possible(keys, radii, queries.size(), results, objs_size);
        vector2d* fetched_results;
        cudaMalloc(&fetched_results, sizeof(vector2d) * queries.size() * objs_size);
        fetch_results<<<queries.size(), objs_size>>>(results, fetched_results);
        std::vector<vector2d> host_results(queries.size() * objs_size);
        cudaMemcpy(host_results.data(), fetched_results, sizeof(vector2d) * queries.size() * objs_size,
            cudaMemcpyDeviceToHost);
        cudaDeviceSynchronize();
        std::vector<std::vector<vector2d>> out(queries.size());
        for (std::size_t i = 0; i < queries.size(); ++i) {
            for (size_t j = 0; j < objs_size; ++j) {
                const vector2d found = host_results[i*objs_size + j];
                if (found == vector2d{-1, -1}) break;
                if ((found - host_keys[i]).norm_sq() >= host_radii[i] * host_radii[i]) continue;
                out[i].push_back(found);
            }
        }
        cudaFree(keys);
        cudaFree(radii);
        cudaFree(results);
        cudaFree(fetched_results);
        return out;
    }
}