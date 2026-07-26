#pragma once

#include "core/assert.h"
#include "core/types.h"

#include <new>

namespace core {

/// Success payload for operations that return no meaningful value.
struct Unit {};

/// Holds either a value of type `T` or an error of type `E`, never both.
/// # Invariants
/// Exactly one of the value or the error is alive at any time.
template <class T, class E>
class [[nodiscard]] Result {
public:
    /// Build a success result holding `value`.
    static Result ok(T value) {
        Result r;
        r.tag_ = Tag::Ok;
        ::new (static_cast<void*>(&r.storage_.ok)) T(static_cast<T&&>(value));
        return r;
    }

    /// Build an error result holding `error`.
    static Result err(E error) {
        Result r;
        r.tag_ = Tag::Err;
        ::new (static_cast<void*>(&r.storage_.err)) E(static_cast<E&&>(error));
        return r;
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    Result(Result&& other) noexcept { move_from(static_cast<Result&&>(other)); }

    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(static_cast<Result&&>(other));
        }
        return *this;
    }

    ~Result() { destroy(); }

    /// True when this holds a value.
    [[nodiscard]] bool is_ok() const { return tag_ == Tag::Ok; }
    /// True when this holds an error.
    [[nodiscard]] bool is_err() const { return tag_ == Tag::Err; }

    /// Borrow the value.
    /// # Panics
    /// If this holds an error.
    [[nodiscard]] T& value() & {
        ASSERT(is_ok());
        return storage_.ok;
    }
    [[nodiscard]] const T& value() const& {
        ASSERT(is_ok());
        return storage_.ok;
    }
    /// Move the value out.
    /// # Panics
    /// If this holds an error.
    [[nodiscard]] T&& value() && {
        ASSERT(is_ok());
        return static_cast<T&&>(storage_.ok);
    }

    /// Borrow the error.
    /// # Panics
    /// If this holds a value.
    [[nodiscard]] E& error() & {
        ASSERT(is_err());
        return storage_.err;
    }
    [[nodiscard]] const E& error() const& {
        ASSERT(is_err());
        return storage_.err;
    }

private:
    enum class Tag : u8 { Ok, Err };

    union Storage {
        Storage() {}
        ~Storage() {}
        T ok;
        E err;
    };

    Result() {}

    void destroy() {
        if (tag_ == Tag::Ok) {
            storage_.ok.~T();
        } else {
            storage_.err.~E();
        }
    }

    void move_from(Result&& other) {
        tag_ = other.tag_;
        if (tag_ == Tag::Ok) {
            ::new (static_cast<void*>(&storage_.ok)) T(static_cast<T&&>(other.storage_.ok));
        } else {
            ::new (static_cast<void*>(&storage_.err)) E(static_cast<E&&>(other.storage_.err));
        }
    }

    Storage storage_;
    Tag tag_ = Tag::Err;
};

}  // namespace core
