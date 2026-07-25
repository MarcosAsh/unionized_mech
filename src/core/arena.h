#pragma once

#include "core/array.h"
#include "core/span.h"
#include "core/types.h"

namespace core {

/// A bump allocator over a single fixed OS allocation.
/// # Invariants
/// Allocation only moves the offset forward. `reset` returns it to zero.
/// Individual allocations are never freed. Alignment up to 64 bytes is honoured.
class Arena {
public:
    /// Create an arena backed by a fresh `bytes`-byte OS allocation, rounded up
    /// to the 64-byte base alignment.
    /// # Panics
    /// If the OS allocation fails.
    [[nodiscard]] static Arena with_capacity(u64 bytes);

    ~Arena();
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    /// Allocate `size` bytes aligned to `align`, returning uninitialised storage.
    /// Returns null only when `size` is zero.
    /// # Panics
    /// If there is not enough room, or if `align` is not a power of two at most 64.
    [[nodiscard]] void* alloc(u64 size, u64 align);

    /// Allocate uninitialised storage for one `T`.
    template <class T>
    [[nodiscard]] T* alloc_one() {
        return static_cast<T*>(alloc(sizeof(T), alignof(T)));
    }

    /// Allocate uninitialised storage for `count` elements of `T`.
    template <class T>
    [[nodiscard]] Span<T> alloc_n(u64 count) {
        if (count == 0) {
            return Span<T>();
        }
        T* p = static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
        return Span<T>(p, count);
    }

    /// Make an empty `Array<T>` with room for `capacity` elements.
    template <class T>
    [[nodiscard]] Array<T> make_array(u64 capacity) {
        const Span<T> s = alloc_n<T>(capacity);
        return Array<T>(s.data(), capacity);
    }

    /// Reset the offset to zero. Memory is reused and destructors do not run.
    void reset();

    /// The current offset, usable as a rewind point for scratch work.
    [[nodiscard]] u64 marker() const { return off_; }

    /// Rewind the offset to a previous `marker`.
    /// # Panics
    /// If `marker` is past the current offset.
    void rewind(u64 marker);

    /// Bytes currently handed out.
    [[nodiscard]] u64 used() const { return off_; }
    /// Total capacity in bytes.
    [[nodiscard]] u64 capacity() const { return cap_; }

private:
    Arena() = default;

    u8* base_ = nullptr;
    u64 cap_ = 0;
    u64 off_ = 0;
};

}  // namespace core
