#pragma once

#include <cstdint>

/// Unsigned 8-bit integer.
using u8 = std::uint8_t;
/// Unsigned 16-bit integer.
using u16 = std::uint16_t;
/// Unsigned 32-bit integer.
using u32 = std::uint32_t;
/// Unsigned 64-bit integer.
using u64 = std::uint64_t;
/// Signed 8-bit integer.
using i8 = std::int8_t;
/// Signed 16-bit integer.
using i16 = std::int16_t;
/// Signed 32-bit integer.
using i32 = std::int32_t;
/// Signed 64-bit integer.
using i64 = std::int64_t;
/// 32-bit IEEE-754 float.
using f32 = float;
/// 64-bit IEEE-754 float.
using f64 = double;

static_assert(sizeof(f32) == 4, "f32 must be 32 bits");
static_assert(sizeof(f64) == 8, "f64 must be 64 bits");
