#pragma once
#include "config.hpp"
#include <memory>
#include <optional>
#include <atomic>

namespace snakeio {
    // This token manager requires SPSC (single-producer, single-consumer) to be thread safe,
    // i.e. only one thread should call allocate() and only one thread should call deallocate(),
    // but they can be different threads.
    template <id_t IDBound> // IDBound = max ID + 1
    class token_manager {
    private:
        // C++ standard guarantees unsigned long long has at least 64 bits.
        std::array<std::atomic<unsigned long long>, IDBound / 64 + (IDBound % 64 != 0)> active_;
        std::unique_ptr<id_t[]> free_list_;
        std::atomic<id_t> free_list_begin_, free_list_end_;

        constexpr id_t increment(id_t idx) const noexcept {
            return (idx + 1) % (IDBound + 1);
        }
    public:
        constexpr token_manager() :
            active_(),
            free_list_(std::make_unique_for_overwrite<id_t[]>(IDBound + 1)),
            free_list_begin_(0), free_list_end_(IDBound) {
            for (id_t i = 0; i < IDBound; ++i) {
                free_list_[i] = i;
            }
        }
        // Approximation of session ids in use (not necessarily activated).
        constexpr id_t in_use_size() const noexcept {
            return IDBound - avail_size();
        }
        /// Slots with `activate()` set (bitmap). Safe concurrent read with `activate`/`deallocate` on other threads.
        constexpr id_t activated_slots() const noexcept {
            id_t n = 0;
            for (id_t id = 0; id < IDBound; ++id) {
                if (operator[](id))
                    ++n;
            }
            return n;
        }
        // Approximation of the available size.
        constexpr id_t avail_size() const noexcept {
            const id_t begin = free_list_begin_.load(std::memory_order::relaxed),
                end = free_list_end_.load(std::memory_order::relaxed);
            return (end + (IDBound + 1) - begin) % (IDBound + 1);
        }
        // Checks if the given ID is active. Performs bounds checking.
        constexpr bool operator[](id_t id) const noexcept {
            return (id < IDBound) && (active_[id / 64].load(std::memory_order::acquire) & (1ULL << (id % 64)));
        }
        // No bounds checking.
        constexpr void activate(id_t id) noexcept {
            active_[id / 64].fetch_or(1ULL << (id % 64), std::memory_order::acq_rel);
        }
        // No bounds checking.
        constexpr void deactivate(id_t id) noexcept {
            active_[id / 64].fetch_and(~(1ULL << (id % 64)), std::memory_order::acq_rel);
        }
        // Does not automatically activate returned id.
        constexpr std::optional<id_t> allocate() noexcept {
            const id_t begin = free_list_begin_.load(std::memory_order::relaxed);
            if (begin == free_list_end_.load(std::memory_order::acquire)) {
                return std::nullopt;
            } else {
                const id_t id = free_list_[begin];
                free_list_begin_.store(increment(begin), std::memory_order::release);
                return id;
            }
        }
        // This does NOT guard against double deallocation. No bounds checking.
        // Automatically deactivates id because deallocating without deactivating is always unsafe.
        constexpr void deallocate(id_t id) noexcept {
            deactivate(id);
            const id_t end = free_list_end_.load(std::memory_order::relaxed);
            free_list_[end] = id;
            free_list_end_.store(increment(end), std::memory_order::release);
        }
    };
}