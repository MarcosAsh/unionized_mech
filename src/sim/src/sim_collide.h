#pragma once

// Internal to the sim module. Moving a character hull through the static level:
// sliding along faces, stepping over kerbs, and vaulting ledges. Split from the
// controller so that file holds intent and this one holds geometry.

#include "core/types.h"
#include "sim/sim.h"

namespace sim {

// Steps. Anything this low is walked straight over, no vault and no hop, so
// kerbs and stair treads do not launch you.
constexpr f32 STEP_HEIGHT = 0.457f;  // tallest ledge taken without a vault

// Mantle. Moving into a ledge at chest height vaults it, keeping momentum.
constexpr f32 MANTLE_REACH = 1.3f;   // highest ledge that can be vaulted
constexpr f32 MANTLE_MARGIN = 0.5f;  // extra launch speed past the ledge lip

/// Advance `c` by its velocity against the static level, one axis at a time,
/// stopping at faces, stepping over kerbs, and vaulting ledges within reach.
/// Also records the landing impact for the camera dip. `grounded` is the state
/// coming in, which decides whether a low ledge is stepped or bumped into.
/// Returns true when the character ends the tick standing on something.
[[nodiscard]] bool move_and_slide(Character& c, const Character& prev_c, f32 hull_h, bool grounded);

}  // namespace sim
