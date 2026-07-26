#pragma once

#include "core/arena.h"
#include "core/result.h"
#include "core/span.h"
#include "core/types.h"

namespace core {

/// Read a whole file into storage taken from `arena`.
/// # Errors
/// A static message when the file cannot be opened or read.
[[nodiscard]] Result<Span<u8>, const char*> read_entire_file(Arena& arena, const char* path);

/// Write `bytes` to `path`, replacing any existing file.
/// # Errors
/// A static message when the file cannot be opened or fully written.
[[nodiscard]] Result<Unit, const char*> write_entire_file(const char* path, Span<const u8> bytes);

}  // namespace core
