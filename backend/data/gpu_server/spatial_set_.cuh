#pragma once
#include <config.hpp>
#include <vector.hpp>
#include <spatial_set_iterator.hpp>
#include <cstddef>
#include <type_traits>
#include <cuda/std/array>
#include <cuda/iterator>
#include <cub/device/device_segmented_radix_sort.cuh>

namespace snakeio::gpu {
    namespace detail {
        // Blocks: BatchSize
        // Threads: ObjsSize
        template <typename T>
        __global__ void compute_indices(T& self) {
            const typename T::size_type i = T::batch_offset(blockIdx.x) + threadIdx.x;
            self.indices()[i] = self.cell_id(self.get_pos(self.nodes()[i]));
        }
        // Blocks: 1
        // Threads: 1
        __global__ void after_sort(auto& self) {
            cuda::std::swap(self.indices, self.indices_buffer_);
            cuda::std::swap(self.nodes_current_, self.nodes_buffer_);
        }
        // Blocks: BatchSize
        // Threads: keys per block
        template <typename T>
        __global__ void find_possible(const T& self, const vector2d* keys, const scalar_t* radii,
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
                    indices[i - begin_offset] = self.indices()[i];
                }
            }
            __syncthreads();
            {
                const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
                const std::size_t out_offset = i * max_per_key;
                std::size_t j = out_offset;
                auto it = make_spatial_set_iterator<T>(self.nodes().begin() + begin_offset,
                    indices.begin() + begin_offset, indices.begin() + end_offset,
                    bounding_rect<T>(keys[i], radii[i]));
                while (it != std::default_sentinel && j < out_offset + max_per_key) {
                    out[j++] = &*(it++);
                }
            }
        }
    }
    // Instances should live in global memory on device.
    // GetPos must be device-capable.
    template <scalar_t WorldWidth, scalar_t WorldHeight, scalar_t CellLength, size_t ObjsSize, id_t BatchSize,
        typename Node = vector2d, auto GetPos = cuda::std::identity{}>
    requires (std::is_trivial_v<Node> && position_getter_of<decltype(GetPos), Node>)
    struct spatial_set_batch : spatial_set_default_config<WorldWidth, WorldHeight, CellLength> {
    private:
        using config = spatial_set_default_config<WorldWidth, WorldHeight, CellLength>;
    public:
        using typename config::size_type;
        using value_type = Node;
        __device__ static constexpr vector2d get_pos(const Node& node) noexcept {
            return GetPos(node);
        }

        cuda::std::array<size_type, BatchSize> end_offsets;
        cuda::std::array<size_type, BatchSize * ObjsSize> indices1_, indices2_;
        cuda::std::array<size_type, BatchSize * ObjsSize> *indices_current_, *indices_buffer_;
        cuda::std::array<Node, BatchSize * ObjsSize> nodes1_, nodes2_;
        cuda::std::array<Node, BatchSize * ObjsSize> *nodes_current_, *nodes_buffer_;

        // Blocks: BatchSize
        // Threads: ObjsSize
        __device__ constexpr void init() noexcept {
            const size_type i = batch_offset(blockIdx.x) + threadIdx.x;
            if (threadIdx.x == 0) {
                end_offsets[blockIdx.x] = batch_offset(blockIdx.x);
            }
            indices1_[i] = config::erase_id;
            if (blockIdx.x == 0 && threadIdx.x == 0) {
                indices_current_ = &indices1_;
                indices_buffer_ = &indices2_;
                nodes_current_ = &nodes1_;
                nodes_buffer_ = &nodes2_;
            }
        }
        __host__ __device__ static constexpr size_type max_objs_size() noexcept { return ObjsSize; }
        __host__ __device__ static constexpr size_type batch_size() noexcept { return BatchSize; }
        __host__ __device__ constexpr auto& indices() noexcept { return *indices_current_; }
        __host__ __device__ constexpr const auto& indices() const noexcept { return *indices_current_; }
        __host__ __device__ constexpr auto& nodes() noexcept{ return *nodes_current_; }
        __host__ __device__ constexpr const auto& nodes() const noexcept { return *nodes_current_; }
        __host__ __device__ static constexpr size_type batch_offset(size_type batch_idx) noexcept {
            return batch_idx * ObjsSize;
        }
    private:
        __host__ constexpr void sort_objs() noexcept {
            void* d_temp_storage = nullptr;
            std::size_t d_temp_storage_bytes;
            const cuda::transform_iterator d_begin_offsets(cuda::counting_iterator<size_type>(0), batch_offset);
            cub::DeviceSegmentedRadixSort::SortPairs(d_temp_storage, d_temp_storage_bytes,
                indices_current_->data(), indices_buffer_->data(),
                nodes_current_->data(), nodes_buffer_->data(),
                BatchSize * ObjsSize, BatchSize,
                d_begin_offsets, end_offsets.begin());
            cudaMalloc(&d_temp_storage, d_temp_storage_bytes);
            cub::DeviceSegmentedRadixSort::SortPairs(d_temp_storage, d_temp_storage_bytes,
                indices_current_->data(), indices_buffer_->data(),
                nodes_current_->data(), nodes_buffer_->data(),
                BatchSize * ObjsSize, BatchSize,
                d_begin_offsets, end_offsets.begin());
            cudaFree(d_temp_storage);
        }
    public:
        __host__ void refresh() noexcept {
            detail::compute_indices<<<BatchSize, ObjsSize>>>(*this);
            sort_objs();
            detail::after_sort<<<1, 1>>>(*this);
        }
        __host__ void find_possible(const vector2d* keys, const scalar_t* radii, unsigned keys_per_batch,
            const value_type** out, std::size_t max_per_key) const noexcept {
            detail::find_possible<<<BatchSize, keys_per_batch>>>(*this, keys, radii, out, max_per_key);
        }
    };

}