#pragma once
#include <config.hpp>
#include <cstddef>

#ifdef __CUDACC__
#include <cuda/std/array>
#include <cuda/std/numeric>
#else
#include <array>
#include <numeric>
#endif
#include <compatibility.hpp>

namespace snakeio {
    template <std::size_t Dim>
    struct vector : stdc::array<scalar_t, Dim> {
        constexpr bool operator==(const vector& other) const noexcept = default;
        constexpr auto operator<=>(const vector& other) const noexcept = delete;
        __host__ __device__ constexpr vector operator+(const vector& other) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] + other[i];
            }
            return result;
        }
        __host__ __device__ constexpr vector& operator+=(const vector& other) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] += other[i];
            }
            return *this;
        }
        __host__ __device__ constexpr vector operator-(const vector& other) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] - other[i];
            }
            return result;
        }
        __host__ __device__ constexpr vector& operator-=(const vector& other) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] -= other[i];
            }
            return *this;
        }
        __host__ __device__ constexpr vector operator*(scalar_t scalar) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] * scalar;
            }
            return result;
        }
        __host__ __device__ constexpr vector& operator*=(scalar_t scalar) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] *= scalar;
            }
            return *this;
        }
        __host__ __device__ constexpr vector operator/(scalar_t scalar) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] / scalar;
            }
            return result;
        }
        __host__ __device__ constexpr vector& operator/=(scalar_t scalar) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] /= scalar;
            }
            return *this;
        }
        // inner product
        __host__ __device__ constexpr scalar_t operator*(const vector& other) const noexcept {
            return stdc::transform_reduce(this->begin(), this->end(), other.begin(), 0);
        }
        __host__ __device__ constexpr scalar_t norm_sq() const noexcept {
            return *this * *this;
        }
    };

    using vector2d = vector<2>;
}
