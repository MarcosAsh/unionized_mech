#include "core/arena.h"

#include "core/assert.h"

#include <cstdlib>

namespace core {

namespace {

constexpr u64 BASE_ALIGN = 64;

u64 align_up(u64 value, u64 align) {
    ASSERT((align & (align - 1)) == 0);  // power of two
    return (value + (align - 1)) & ~(align - 1);
}

}  // namespace

Arena Arena::with_capacity(u64 bytes) {
    const u64 rounded = align_up(bytes, BASE_ALIGN);
    void* mem = std::aligned_alloc(BASE_ALIGN, static_cast<size_t>(rounded));
    ASSERT_MSG(mem != nullptr, "arena backing allocation failed");

    Arena a;
    a.base_ = static_cast<u8*>(mem);
    a.cap_ = rounded;
    a.off_ = 0;
    return a;
}

Arena::~Arena() {
    if (base_ != nullptr) {
        std::free(base_);
    }
}

Arena::Arena(Arena&& other) noexcept
    : base_(other.base_), cap_(other.cap_), off_(other.off_) {
    other.base_ = nullptr;
    other.cap_ = 0;
    other.off_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        if (base_ != nullptr) {
            std::free(base_);
        }
        base_ = other.base_;
        cap_ = other.cap_;
        off_ = other.off_;
        other.base_ = nullptr;
        other.cap_ = 0;
        other.off_ = 0;
    }
    return *this;
}

void* Arena::alloc(u64 size, u64 align) {
    if (size == 0) {
        return nullptr;
    }
    ASSERT_MSG(align != 0 && align <= BASE_ALIGN, "alignment must be within 64 bytes");
    const u64 aligned = align_up(off_, align);
    ASSERT_MSG(aligned + size <= cap_, "arena out of memory");

    void* p = base_ + aligned;
    off_ = aligned + size;
    return p;
}

void Arena::reset() { off_ = 0; }

void Arena::rewind(u64 marker) {
    ASSERT(marker <= off_);
    off_ = marker;
}

}  // namespace core
