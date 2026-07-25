#pragma once

#include "core/assert.h"
#include "core/span.h"
#include "core/types.h"

#include <new>
#include <type_traits>

namespace core {

/// A fixed-capacity vector whose storage is borrowed from an arena.
/// # Invariants
/// Capacity is set once and never changes. Overflowing `push` is a bug, not a
/// recoverable error. Element destructors are never run, matching arena memory,
/// so `T` must be trivially destructible.
template <class T>
class Array {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Array does not run element destructors");

public:
    /// Empty, zero-capacity array.
    Array() = default;
    /// Wrap arena-provided `storage` with room for `capacity` elements.
    Array(T* storage, u64 capacity) : data_(storage), cap_(capacity) {}

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;
    Array(Array&&) noexcept = default;
    Array& operator=(Array&&) noexcept = default;

    /// Append `value`.
    /// # Panics
    /// If the array is full.
    void push(T value) {
        ASSERT(len_ < cap_);
        ::new (static_cast<void*>(&data_[len_])) T(static_cast<T&&>(value));
        ++len_;
    }

    /// Append `value` if there is room. Returns false when full.
    [[nodiscard]] bool try_push(T value) {
        if (len_ >= cap_) {
            return false;
        }
        ::new (static_cast<void*>(&data_[len_])) T(static_cast<T&&>(value));
        ++len_;
        return true;
    }

    /// Drop all elements. Capacity is unchanged.
    void clear() { len_ = 0; }

    [[nodiscard]] u64 size() const { return len_; }
    [[nodiscard]] u64 capacity() const { return cap_; }
    [[nodiscard]] bool empty() const { return len_ == 0; }
    [[nodiscard]] bool full() const { return len_ == cap_; }

    /// Element access.
    /// # Panics
    /// If `i` is out of range.
    [[nodiscard]] T& operator[](u64 i) {
        ASSERT(i < len_);
        return data_[i];
    }
    [[nodiscard]] const T& operator[](u64 i) const {
        ASSERT(i < len_);
        return data_[i];
    }

    /// A span over the live elements.
    [[nodiscard]] Span<T> as_span() const { return Span<T>(data_, len_); }

    [[nodiscard]] T* begin() { return data_; }
    [[nodiscard]] T* end() { return data_ + len_; }

private:
    T* data_ = nullptr;
    u64 cap_ = 0;
    u64 len_ = 0;
};

}  // namespace core
