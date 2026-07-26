#pragma once

#include "core/types.h"

namespace core {

/// Report a failed assertion and abort the process. Never returns.
/// # Panics
/// Always. This is the tail of every failed assertion.
[[noreturn]] void assert_fail(const char* file, i32 line, const char* expr,
                              const char* msg);

}  // namespace core

/// Abort unless `cond` holds.
#define ASSERT(cond) \
    ((cond) ? (void)0 : ::core::assert_fail(__FILE__, __LINE__, #cond, nullptr))

/// Abort with a message unless `cond` holds.
#define ASSERT_MSG(cond, msg) \
    ((cond) ? (void)0 : ::core::assert_fail(__FILE__, __LINE__, #cond, (msg)))

/// Abort unconditionally with a message.
#define PANIC(msg) (::core::assert_fail(__FILE__, __LINE__, "PANIC", (msg)))

/// Mark code that must never run. Aborts if reached.
#define UNREACHABLE() \
    (::core::assert_fail(__FILE__, __LINE__, "unreachable", nullptr))
