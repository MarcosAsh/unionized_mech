#pragma once

#include "core/arena.h"
#include "core/assert.h"
#include "core/types.h"

#include <type_traits>

namespace core {

/// A generational handle to a pool slot: 32-bit index plus 32-bit generation.
/// The generation makes stale handles detectable instead of undefined
/// behaviour. Typed on `T` so a handle to one pool cannot address another.
template <class T>
struct Handle {
    u32 index = 0xFFFFFFFFu;
    u32 gen = 0;

    /// The null handle. Never resolves.
    [[nodiscard]] static constexpr Handle none() { return Handle{}; }
    /// True when this is not the null handle. Says nothing about staleness.
    [[nodiscard]] constexpr bool is_some() const { return index != 0xFFFFFFFFu; }

    [[nodiscard]] friend constexpr bool operator==(Handle a, Handle b) {
        return a.index == b.index && a.gen == b.gen;
    }
};

/// A fixed-capacity object pool over arena storage with a free list. Slots are
/// reused in LIFO order and each reuse bumps the slot's generation, so handles
/// to freed objects resolve to null rather than to the new occupant.
/// # Invariants
/// Capacity is fixed at init. Destructors never run, matching arena memory, so
/// `T` must be trivially destructible.
template <class T>
class Pool {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Pool does not run element destructors");

public:
    Pool() = default;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) noexcept = default;
    Pool& operator=(Pool&&) noexcept = default;

    /// Create a pool with room for `capacity` objects, backed by `arena`.
    [[nodiscard]] static Pool with_capacity(Arena& arena, u32 capacity) {
        Pool p;
        p.slots_ = arena.alloc_n<T>(capacity).data();
        p.gens_ = arena.alloc_n<u32>(capacity).data();
        p.next_free_ = arena.alloc_n<u32>(capacity).data();
        p.cap_ = capacity;
        for (u32 i = 0; i < capacity; ++i) {
            p.gens_[i] = 0;
            p.next_free_[i] = i + 1;  // slot i's next free is i+1, cap = end
        }
        p.free_head_ = 0;
        return p;
    }

    /// Take a slot and move `value` into it.
    /// # Panics
    /// If the pool is full.
    [[nodiscard]] Handle<T> insert(T value) {
        ASSERT_MSG(free_head_ < cap_, "pool is full");
        const u32 index = free_head_;
        free_head_ = next_free_[index];
        slots_[index] = static_cast<T&&>(value);
        ++len_;
        return Handle<T>{index, gens_[index]};
    }

    /// Release the slot behind `handle`. Later lookups with this handle
    /// resolve to null.
    /// # Panics
    /// If the handle is already stale or null.
    void remove(Handle<T> handle) {
        ASSERT_MSG(get(handle) != nullptr, "removing a stale or null handle");
        gens_[handle.index] += 1;  // invalidate every outstanding handle
        next_free_[handle.index] = free_head_;
        free_head_ = handle.index;
        --len_;
    }

    /// The object behind `handle`, or null when the handle is stale or null.
    [[nodiscard]] T* get(Handle<T> handle) {
        if (handle.index >= cap_ || gens_[handle.index] != handle.gen) {
            return nullptr;
        }
        return &slots_[handle.index];
    }
    [[nodiscard]] const T* get(Handle<T> handle) const {
        if (handle.index >= cap_ || gens_[handle.index] != handle.gen) {
            return nullptr;
        }
        return &slots_[handle.index];
    }

    /// Live object count.
    [[nodiscard]] u32 size() const { return len_; }
    /// Total slot count.
    [[nodiscard]] u32 capacity() const { return cap_; }

private:
    T* slots_ = nullptr;
    u32* gens_ = nullptr;
    u32* next_free_ = nullptr;
    u32 cap_ = 0;
    u32 len_ = 0;
    u32 free_head_ = 0;
};

}  // namespace core
