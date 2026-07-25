#pragma once

#include "core/assert.h"
#include "core/types.h"

namespace core {

/// A non-owning view over `count` contiguous `T`. Cheap value, freely copied.
template <class T>
class Span {
public:
    /// Empty span.
    Span() = default;
    /// View `count` elements starting at `data`.
    Span(T* data, u64 count) : data_(data), count_(count) {}

    /// Pointer to the first element.
    [[nodiscard]] T* data() const { return data_; }
    /// Number of elements in view.
    [[nodiscard]] u64 size() const { return count_; }
    /// True when the view has no elements.
    [[nodiscard]] bool empty() const { return count_ == 0; }

    /// Element access.
    /// # Panics
    /// If `i` is out of range.
    [[nodiscard]] T& operator[](u64 i) const {
        ASSERT(i < count_);
        return data_[i];
    }

    [[nodiscard]] T* begin() const { return data_; }
    [[nodiscard]] T* end() const { return data_ + count_; }

    /// A sub-view of `len` elements starting at `offset`.
    /// # Panics
    /// If the range runs past the end of the view.
    [[nodiscard]] Span<T> subspan(u64 offset, u64 len) const {
        ASSERT(offset + len <= count_);
        return Span<T>(data_ + offset, len);
    }

private:
    T* data_ = nullptr;
    u64 count_ = 0;
};

}  // namespace core
