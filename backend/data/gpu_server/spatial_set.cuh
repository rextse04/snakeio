#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <spatial_set_iterator.hpp>
#include <cstddef>
#include <type_traits>
#include <algorithm>
#include <cuda/std/array>
#include <cuda/iterator>
#include <cub/device/device_segmented_sort.cuh>

namespace snakeio::gpu {
    namespace detail {
        template <typename Node>
        __device__ void default_setter(Node& node, vector2d key) {
            node = key;
        }
        // Blocks: BatchSize
        // Threads: ObjsSize
        template <typename T>
        __global__ void init(T self) {
            const typename T::size_type batch_offset = T::batch_offset(blockIdx.x),
                i = batch_offset + threadIdx.x;
            if (threadIdx.x == 0) {
                self.begin_offsets[blockIdx.x] = self.end_offsets[blockIdx.x] = batch_offset;
            }
            T::set_pos(self.nodes[i], T::erase_key);
            T::set_pos(self.nodes_buffer_[i], T::erase_key);
        }
        // Blocks: BatchSize
        // Threads: ObjsSize
        template <typename T>
        __global__ void compute_indices(T self) {
            const typename T::size_type i = T::batch_offset(blockIdx.x) + threadIdx.x;
            self.indices[i] = self.cell_id(self.get_pos(self.nodes[i]));
            self.indices_buffer_[i] = T::erase_index;
        }
        // Blocks: BatchSize
        // Threads: keys per batch
        template <typename T>
        __global__ void find_possible(T self, const vector2d* keys, const scalar_t* radii,
            const typename T::value_type** out, std::size_t max_per_key) {
            using size_type = T::size_type;
            __shared__ cuda::std::array<size_t, T::max_objs_size()> indices;
            const size_type begin_offset = T::batch_offset(blockIdx.x);
            const size_type end_offset = self.end_offsets[blockIdx.x];
            const size_type size = end_offset - begin_offset;
            {
                const size_type workload = size / blockDim.x + 1;
                for (
                    size_type i = begin_offset + workload * threadIdx.x;
                    i < begin_offset + workload * (threadIdx.x + 1) || i < end_offset;
                    ++i) {
                    indices[i - begin_offset] = self.indices[i];
                }
            }
            __syncthreads();
            {
                const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
                const std::size_t out_offset = i * max_per_key;
                std::size_t j = out_offset;
                auto it = make_spatial_set_iterator<T>(self.nodes + begin_offset,
                    indices.begin(), indices.begin() + size,
                    bounding_rect<T>(keys[i], radii[i]));
                while (it != std::default_sentinel && j < out_offset + max_per_key) {
                    out[j++] = &*(it++);
                }
            }
        }
    }
    // GetPos must be device-capable.
    // SetPos(Node& node, vector2d key):
    // - It is guaranteed that setPos will not be used on active nodes.
    // - Semantic requirement: SetPos(node, key) => GetPos(node) == key.
    // - Must be device-capable.
    template <scalar_t WorldWidth, scalar_t WorldHeight, scalar_t CellLength, size_t ObjsSize, id_t BatchSize,
        typename Node = vector2d, auto GetPos = cuda::std::identity{}, auto SetPos = detail::default_setter<Node>>
    requires requires(Node& node, vector2d key) {
        requires std::is_trivial_v<Node>;
        requires position_getter_of<decltype(GetPos), Node>;
        { SetPos(node, key) };
    }
    class spatial_set_batch : public spatial_set_default_config<WorldWidth, WorldHeight, CellLength> {
        using config = spatial_set_default_config<WorldWidth, WorldHeight, CellLength>;
    public:
        using typename config::size_type;
        using value_type = Node;
        __device__ static constexpr vector2d get_pos(const Node& node) noexcept {
            return GetPos(node);
        }
        __device__ static constexpr void set_pos(Node& node, vector2d key) noexcept {
            SetPos(node, key);
        }

        size_type *begin_offsets, *end_offsets;
        size_type *indices, *indices_buffer_;
        Node *nodes, *nodes_buffer_;

        __host__ spatial_set_batch() {
            cudaMalloc(&begin_offsets, sizeof(size_type) * BatchSize);
            cudaMalloc(&end_offsets, sizeof(size_type) * BatchSize);
            cudaMalloc(&indices, sizeof(size_type) * BatchSize * ObjsSize);
            cudaMalloc(&indices_buffer_, sizeof(size_type) * BatchSize * ObjsSize);
            cudaMalloc(&nodes, sizeof(Node) * BatchSize * ObjsSize);
            cudaMalloc(&nodes_buffer_, sizeof(Node) * BatchSize * ObjsSize);
            detail::init<<<BatchSize, ObjsSize>>>(*this);
        }
        /*__host__ ~spatial_set_batch() {
            cudaFree(begin_offsets);
            cudaFree(end_offsets);
            cudaFree(indices);
            cudaFree(indices_buffer_);
            cudaFree(nodes);
            cudaFree(nodes_buffer_);
        }*/
        __host__ __device__ static constexpr size_type max_objs_size() noexcept { return ObjsSize; }
        __host__ __device__ static constexpr size_type batch_size() noexcept { return BatchSize; }
        __host__ __device__ static constexpr size_type batch_offset(size_type batch_idx) noexcept {
            return batch_idx * ObjsSize;
        }
        __host__ void refresh() noexcept {
            detail::compute_indices<<<BatchSize, ObjsSize>>>(*this);
            void* d_temp_storage = nullptr;
            std::size_t d_temp_storage_bytes = 0;
            cub::DeviceSegmentedSort::SortPairs(d_temp_storage, d_temp_storage_bytes,
                indices, indices_buffer_, nodes, nodes_buffer_,
                BatchSize * ObjsSize, BatchSize,
                begin_offsets, end_offsets);
            cudaMalloc(&d_temp_storage, d_temp_storage_bytes);
            cub::DeviceSegmentedSort::SortPairs(d_temp_storage, d_temp_storage_bytes,
                indices, indices_buffer_, nodes, nodes_buffer_,
                BatchSize * ObjsSize, BatchSize,
                begin_offsets, end_offsets);
            cudaDeviceSynchronize();
            cudaFree(d_temp_storage);
            size_type indices_[ObjsSize];
            std::fill(indices_, indices_ + ObjsSize, 999);
            cudaMemcpy(indices_, indices_buffer_, ObjsSize * sizeof(size_type), cudaMemcpyDeviceToHost);
            std::swap(indices, indices_buffer_);
            std::swap(nodes, nodes_buffer_);
        }
        __host__ void find_possible(const vector2d* keys, const scalar_t* radii, unsigned keys_per_batch,
            const value_type** out, std::size_t max_per_key) const noexcept {
            detail::find_possible<<<BatchSize, keys_per_batch>>>(*this, keys, radii, out, max_per_key);
        }
    };
}