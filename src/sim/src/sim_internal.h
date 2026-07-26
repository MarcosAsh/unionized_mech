#pragma once

// Internal to the sim module. The pieces simulate() composes each tick.

#include "sim/sim.h"

namespace sim {

/// Eye height above the feet, where rays start and bots look from.
constexpr f32 EYE_HEIGHT = 1.6f;

/// Advance one character by one tick of movement from `cmd`. Pure: reads
/// `prev_c`, writes `c` (already copied from prev).
void step_character(Character& c, const Character& prev_c, const InputCmd& cmd);

/// Generate the tick's input for bot `index` from the previous world state.
/// Deterministic: all randomness derives from the world seed and tick.
[[nodiscard]] InputCmd bot_think(const World& prev, u32 index);

/// Resolve this tick's shots, damage, deaths, and respawns. `fired[i]` is
/// whether character i pressed fire this tick.
void resolve_combat(World& next, const bool fired[MAX_PLAYERS]);

}  // namespace sim
