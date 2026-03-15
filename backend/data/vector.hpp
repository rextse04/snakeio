#pragma once
#include "config.hpp"
#include <cstddef>
#include <array>

namespace snakeio {
    template <std::size_t Dim>
    struct vector : std::array<scalar_t, Dim> {
        constexpr bool operator==(const vector& other) const noexcept = default;
        constexpr auto operator<=>(const vector& other) const noexcept = delete;
        constexpr vector operator+(const vector& other) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] + other[i];
            }
            return result;
        }
        constexpr vector& operator+=(const vector& other) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] += other[i];
            }
            return *this;
        }
        constexpr vector operator-(const vector& other) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] - other[i];
            }
            return result;
        }
        constexpr vector& operator-=(const vector& other) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] -= other[i];
            }
            return *this;
        }
        constexpr vector operator*(scalar_t scalar) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] * scalar;
            }
            return result;
        }
        constexpr vector& operator*=(scalar_t scalar) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] *= scalar;
            }
            return *this;
        }
        constexpr vector operator/(scalar_t scalar) const noexcept {
            vector result{};
            for (std::size_t i = 0; i < Dim; ++i) {
                result[i] = (*this)[i] / scalar;
            }
            return result;
        }
        constexpr vector& operator/=(scalar_t scalar) noexcept {
            for (std::size_t i = 0; i < Dim; ++i) {
                (*this)[i] /= scalar;
            }
            return *this;
        }
        constexpr scalar_t norm_sq() const noexcept {
            scalar_t result = 0;
            for (std::size_t i = 0; i < Dim; ++i) {
                result += (*this)[i] * (*this)[i];
            }
            return result;
        }
    };

    using vector2d = vector<2>;
}
