#pragma once

// Internal to the sim module. The player hull, the static level, and the
// collision queries against it.

#include "core/span.h"
#include "core/types.h"
#include "sim/sim.h"

namespace sim {

// Player collision hull, an axis-aligned box, feet at cam_y (Source lineage).
constexpr f32 HULL_HALF_WIDTH = 0.4f;
constexpr f32 HULL_HEIGHT = 1.8f;
constexpr f32 DUCK_HEIGHT = 1.0f;  // hull height while crouched

// Reach for grabbing a wall from a short gap, shared by wallrun detection.
constexpr f32 WALL_DETECT_DIST = 0.6f;

/// True when a player hull of `height` at (x, y, z) overlaps box `b`.
[[nodiscard]] bool hull_overlaps(f32 x, f32 y, f32 z, f32 height, const Aabb& b);

/// Look for a vertical box face within reach on a horizontal side of the hull
/// at (x, y, z). On success writes the outward wall normal and returns true.
[[nodiscard]] bool find_wall(f32 x, f32 y, f32 z, f32 height, core::Span<const Aabb> boxes,
                             f32* out_nx, f32* out_nz);

}  // namespace sim
