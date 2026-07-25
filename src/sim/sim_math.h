#pragma once

// Internal to the sim module. Deterministic scalar math for simulation code.
// No libm: sin and cos are fixed polynomials over add, subtract, multiply, and
// divide, so they are bit-identical on every target. See ARCHITECTURE.md.

#include "core/types.h"

namespace sim {

/// Wrap `x` (radians) into roughly [-PI, PI]. Assumes a bounded input.
[[nodiscard]] f32 wrap_angle(f32 x);

/// Deterministic sine of `x` radians.
[[nodiscard]] f32 sim_sin(f32 x);

/// Deterministic cosine of `x` radians.
[[nodiscard]] f32 sim_cos(f32 x);

}  // namespace sim
