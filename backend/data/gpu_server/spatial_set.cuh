#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <spatial_set_iterator.hpp>
#include <cstddef>
#include <type_traits>
#include <algorithm>
#include <cuda/iterator>
#include <cuda_runtime_api.h>
#include <cub/device/device_segmented_sort.cuh>

namespace snakeio::gpu {
    namespace detail {
        template <typename Node>
        __device__ void default_setter(Node& node, vector2d key) noexcept {
            node = key;
        }
        // Blocks: T::grid_dim, NodesSize / T::block_dim
        // Threads: T::block_dim
        template <typename T>
        __global__ void init(T self) {
            const typename T::size_type batch_offset = T::batch_offset(blockIdx.x),
                node_offset = blockIdx.y * blockDim.x + threadIdx.x;
            if (node_offset >= T::max_nodes_size()) return;
            const typename T::size_type i = batch_offset + node_offset;
            if (blockIdx.y == 0 && threadIdx.x == 0) {
                self.begin_offsets[blockIdx.x] = self.end_offsets[blockIdx.x] = batch_offset;
            }
            T::set_pos(self.nodes[i], T::erase_key);
            T::set_pos(self.nodes_buffer_[i], T::erase_key);
        }
        // Blocks: T::grid_dim, NodesSize / T::block_dim
        // Threads: T::block_dim
        template <typename T>
        __global__ void compute_indices(T self) {
            const typename T::size_type batch_offset = T::batch_offset(blockIdx.x),
                node_offset = blockIdx.y * blockDim.x + threadIdx.x;
            if (node_offset >= T::max_nodes_size()) return;
            const typename T::size_type i = batch_offset + node_offset;
            self.indices[i] = self.cell_id(self.get_pos(self.nodes[i]));
            self.indices_buffer_[i] = T::erase_index;
        }
        // Blocks: BatchSize
        // Threads: keys per batch (must be within 1024)
        template <typename T>
        __global__ void find_possible(T self, const vector2d* keys, const scalar_t* radii,
            const typename T::value_type** out, std::size_t max_per_key) {
            using size_type = T::size_type;
            __shared__ size_t indices[T::max_nodes_size() + 1];
            const size_type begin_offset = T::batch_offset(blockIdx.x);
            const size_type end_offset = self.end_offsets[blockIdx.x];
            const size_type size = end_offset - begin_offset;
            {
                const size_type workload = size / blockDim.x + 1;
                for (
                    size_type i = begin_offset + workload * threadIdx.x;
                    i < begin_offset + workload * (threadIdx.x + 1) && i < end_offset;
                    ++i) {
                    indices[i - begin_offset] = self.indices[i];
                }
            }
            if (threadIdx.x == 0) indices[size] = T::erase_index;
            __syncthreads();
            {
                const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
                const std::size_t out_offset = i * max_per_key;
                std::size_t j = out_offset;
                auto it = make_spatial_set_iterator<T>(self.nodes + begin_offset,
                    indices, indices + size, bounding_rect<T>(keys[i], radii[i]));
                while (it != std::default_sentinel && j < out_offset + max_per_key) {
                    out[j++] = &*(it++);
                }
            }
        }
    }
    template <scalar_t WorldWidth, scalar_t WorldHeight, scalar_t CellLength, size_t NodesSize, id_t BatchSize,
        typename Node = vector2d, auto GetPos = cuda::std::identity{}, auto SetPos = detail::default_setter<Node>>
    requires requires(Node& node, vector2d key) {
        requires std::is_trivial_v<Node>;
        requires position_getter_of<decltype(GetPos), Node>;
        { SetPos(node, key) };
    }
    class spatial_set_batch : public spatial_set_default_config<WorldWidth, WorldHeight, CellLength> {
        using config = spatial_set_default_config<WorldWidth, WorldHeight, CellLength>;
        static constexpr unsigned block_dim = 256;
        static constexpr dim3 grid_dim{BatchSize, (NodesSize - 1) / block_dim + 1};
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

        cudaStream_t stream_{};
        void* cub_temp_{nullptr};
        std::size_t cub_temp_bytes_{0};

        __host__ explicit spatial_set_batch(cudaStream_t stream = nullptr) : stream_(stream) {
            cudaMalloc(&begin_offsets, sizeof(size_type) * BatchSize);
            cudaMalloc(&end_offsets, sizeof(size_type) * BatchSize);
            cudaMalloc(&indices, sizeof(size_type) * BatchSize * NodesSize);
            cudaMalloc(&indices_buffer_, sizeof(size_type) * BatchSize * NodesSize);
            cudaMalloc(&nodes, sizeof(Node) * BatchSize * NodesSize);
            cudaMalloc(&nodes_buffer_, sizeof(Node) * BatchSize * NodesSize);
            detail::init<<<grid_dim, block_dim, 0, stream_>>>(*this);
        }
        __host__ void destroy() {
            cudaFree(cub_temp_);
            cub_temp_ = nullptr;
            cub_temp_bytes_ = 0;
            cudaFree(begin_offsets);
            cudaFree(end_offsets);
            cudaFree(indices);
            cudaFree(indices_buffer_);
            cudaFree(nodes);
            cudaFree(nodes_buffer_);
        }
        __host__ __device__ static constexpr size_type max_nodes_size() noexcept { return NodesSize; }
        __host__ __device__ static constexpr size_type batch_size() noexcept { return BatchSize; }
        __host__ __device__ static constexpr size_type batch_offset(size_type batch_idx) noexcept {
            return batch_idx * NodesSize;
        }
        __host__ void refresh() noexcept {
            detail::compute_indices<<<grid_dim, block_dim, 0, stream_>>>(*this);
            if (cub_temp_bytes_ == 0) {
                cub::DeviceSegmentedSort::SortPairs(
                    nullptr,
                    cub_temp_bytes_,
                    indices,
                    indices_buffer_,
                    nodes,
                    nodes_buffer_,
                    static_cast<::cuda::std::int64_t>(BatchSize * NodesSize),
                    static_cast<::cuda::std::int64_t>(BatchSize),
                    begin_offsets,
                    end_offsets,
                    stream_);
                cudaMalloc(&cub_temp_, cub_temp_bytes_);
            }
            cub::DeviceSegmentedSort::SortPairs(
                cub_temp_,
                cub_temp_bytes_,
                indices,
                indices_buffer_,
                nodes,
                nodes_buffer_,
                static_cast<::cuda::std::int64_t>(BatchSize * NodesSize),
                static_cast<::cuda::std::int64_t>(BatchSize),
                begin_offsets,
                end_offsets,
                stream_);
            std::swap(indices, indices_buffer_);
            std::swap(nodes, nodes_buffer_);
        }
        __host__ void find_possible(const vector2d* keys, const scalar_t* radii, unsigned keys_per_batch,
            const value_type** out, std::size_t max_per_key) const noexcept {
            detail::find_possible<<<BatchSize, keys_per_batch, 0, stream_>>>(*this, keys, radii, out, max_per_key);
        }
    };
}
