#pragma once
#include "config.hpp"
#include <bitset>
#include <memory>
#include <optional>

namespace snakeio {
    template <id_t IDBound> // IDBound = max ID + 1
    class token_manager {
    private:
        std::bitset<IDBound> active_;
        std::unique_ptr<id_t[]> free_list_;
        id_t free_list_begin_, free_list_end_;
        id_t free_list_size_;
    public:
        constexpr token_manager() :
            free_list_(std::make_unique_for_overwrite<id_t[]>(IDBound)),
            free_list_begin_(0), free_list_end_(IDBound), free_list_size_(IDBound) {
            for (id_t i = 0; i < IDBound; ++i) {
                free_list_[i] = i;
            }
        }
        constexpr id_t active_size() const noexcept {
            return IDBound - avail_size();
        }
        constexpr id_t avail_size() const noexcept {
            return free_list_size_;
        }
        constexpr std::optional<id_t> allocate() noexcept {
            if (free_list_begin_ == free_list_end_) {
                return std::nullopt;
            } else {
                const id_t id = free_list_[++free_list_begin_ %= IDBound];
                --free_list_size_;
                active_[id] = true;
                return id;
            }
        }
        // Note: This does NOT guard against double deallocation
        constexpr void deallocate(id_t id) noexcept {
            active_[id] = false;
            free_list_[++free_list_end_ %= IDBound] = id;
            ++free_list_size_;
        }
    };
}